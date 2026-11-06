#include "cpu_local.hpp"
#include "lapic.hpp"
#include "ok/arch/execution_spaces.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/smp/bringup.hpp"

#include <cstddef>

namespace ok::arch::execution
{
using x86::CpuLocal;
using x86::this_cpu;
namespace
{
namespace lapic = arch::x86::lapic;
constexpr u64 present = 1;
constexpr u64 writable = 2;
constexpr u64 user = 4;
constexpr u64 no_execute = u64{1} << 63;
constexpr u64 address_mask = 0x000ffffffffff000ULL;
constexpr usize rax_slot = 14;
constexpr usize rdi_slot = 9;
constexpr usize rsi_slot = 10;
constexpr usize rdx_slot = 11;
constexpr usize r10_slot = 5;
constexpr usize r8_slot = 7;
constexpr usize r9_slot = 6;

struct alignas(page_size) Table
{
    u64 entries[512]{};
};
// Each space owns its fixed tables and one leaf table per 2 MiB of user
// window it maps, all from table storage.
struct Space
{
    Table *pml4{};
    Table *pdpt{};
    Table *directory{};
    usize tables{};
    detail::TlbSpaceTag tlb{};
};
constinit SpaceTable<Space> spaces;
// Entries validate their space without the kernel lock while the CPU holding
// it may grow or shrink the slot table. It also serializes space ids and TLB
// generations.
constinit smp::SpinLock spaces_lock{};
constinit detail::TlbSpaceIds space_ids{};
// CR4.PCIDE: CR3[11:0] names the PCID, and bit 63 of a CR3 load keeps the
// loaded PCID's cached translations. The kernel's translation is PCID 0.
constexpr uptr cr4_pcide = uptr{1} << 17;
constexpr uptr cr3_keep = uptr{1} << 63;
u64 kernel_pdpt[512]{};
bool initialized = false;

struct [[gnu::packed]] DescriptorPointer
{
    u16 limit;
    uptr base;
};
struct [[gnu::packed]] InterruptGate
{
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 attributes;
    u16 offset_middle;
    u32 offset_high;
    u32 reserved;
};
struct SavedTrap
{
    uptr registers[15];
    uptr vector;
    uptr error;
    uptr pc;
    uptr cs;
    uptr flags;
    uptr sp;
    uptr ss;
};
static_assert(sizeof(InterruptGate) == 16);
static_assert(offsetof(SavedTrap, cs) == 144);
static_assert(offsetof(UserFrame, pc) == 256);
static_assert(offsetof(UserFrame, sp) == 264);
static_assert(offsetof(UserFrame, flags) == 272);
static_assert(offsetof(UserFrame, fs_base) == 280);
static_assert(offsetof(UserFrame, gs_base) == 288);
static_assert(offsetof(UserFrame, fp_state) == 304);
static_assert(alignof(UserFrame) >= 16);
static_assert(offsetof(CpuLocal, self) == 0);
static_assert(offsetof(CpuLocal, logical) == x86::cpu_logical_offset);
static_assert(offsetof(CpuLocal, resume_rsp) == 16);
static_assert(offsetof(CpuLocal, user_rsp) == 24);
static_assert(offsetof(CpuLocal, stack_top) == 32);
static_assert(offsetof(CpuLocal, active_frame) == 40);
static_assert(offsetof(CpuLocal, kernel_fs) == 48);
static_assert(offsetof(CpuLocal, kernel_fp) == 64);

// One IDT for every CPU; each CPU has its own GDT, TSS and stacks.
alignas(16) InterruptGate idt[256]{};
alignas(16) u8 interrupt_stack[64 * 1024]{};
alignas(16) u8 secondary_interrupt_stacks[smp::max_cpus - 1][16 * 1024]{};
alignas(16) u8 emergency_stacks[smp::max_cpus][8 * 1024]{};

extern "C" void ok_x86_execution_enter(UserFrame *);
extern "C" void ok_x86_syscall_entry();
extern "C" void ok_x86_trap_239();
extern "C" void ok_x86_trap_240();
extern "C" void ok_x86_trap_255();
extern "C" void ok_x86_trap_unexpected();
extern "C" const uptr ok_x86_execution_vectors[48];
extern "C" void ok_platform_console_write_char(char);

void outb(u16 port, u8 value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}
u8 inb(u16 port)
{
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}
void io_wait()
{
    outb(0x80, 0);
}
u64 read_msr(u32 index)
{
    u32 low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return (u64{high} << 32) | low;
}
void write_msr(u32 index, u64 value)
{
    asm volatile("wrmsr" : : "c"(index), "a"(static_cast<u32>(value)), "d"(static_cast<u32>(value >> 32)) : "memory");
}
uptr read_cr3()
{
    uptr value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}
void write_cr3(uptr value)
{
    asm volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}
bool pcid_supported()
{
    u32 a = 1, b, c, d;
    asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (c & (1u << 17)) != 0;
}
void console(std::string_view text)
{
    for (char value : text)
        ok_platform_console_write_char(value);
}
void console_hex(uptr value)
{
    constexpr char digits[] = "0123456789abcdef";
    for (usize i = 0; i < sizeof(value) * 2; ++i)
        ok_platform_console_write_char(digits[(value >> ((sizeof(value) * 2 - i - 1) * 4)) & 15]);
}
void acknowledge_irq(uptr vector)
{
    if (vector >= 40 && vector < 48)
        outb(0xa0, 0x20);
    if (vector >= 32 && vector < 48)
        outb(0x20, 0x20);
}
// PIT channel 0 runs in mode 2, the rate generator Linux programs: IRQ 0
// rises as the count reloads, so the count latched in the handler is how far
// past that edge the tick was taken (modulo one period).
constexpr u16 pit_divisor = 11932; // Approximately 100 actual IRQs/second.
constexpr u64 pit_frequency = 1193182;
u64 pit_tick_late()
{
    outb(0x43, 0x00); // Latch channel 0.
    const u16 low = inb(0x40);
    const u16 count = static_cast<u16>(low | (u16{inb(0x40)} << 8));
    return count != 0 && count <= pit_divisor ? pit_divisor - count : 0;
}
// Vector 32 is the PIT tick; 33..47 are platform lines whose top half the
// interrupt service runs once it has started. Before that, only acknowledge.
// The kernel takes interrupts only in the idle wait. The 8259 reaches the boot
// CPU alone, and everything here is boot-CPU state.
void complete_irq(uptr vector, interrupt::TimerWake wake)
{
    if (vector < 32 || vector >= 48)
        return;
    if (vector == 32)
    {
        interrupt::record_timer_latency(wake, pit_tick_late(), pit_frequency);
        ok_interrupt_tick();
    }
    if (vector == 32 || !interrupt::platform_irqs_started())
        acknowledge_irq(vector);
    else
        ok_interrupt_top_half(static_cast<interrupt::IrqLine>(vector - 32));
}
// The local APIC timer and IPIs, on whatever translation trapped. False for
// any other vector.
bool complete_local(uptr vector)
{
    if (vector == lapic::timer_vector)
    {
        lapic::end_of_interrupt();
        smp::local_tick();
    }
    else if (vector == lapic::ipi_vector)
    {
        lapic::end_of_interrupt();
        static_cast<void>(smp::receive_ipi());
    }
    else if (vector != lapic::spurious_vector)
        return false;
    return true;
}
void fill_descriptors(CpuLocal &cpu, uptr rsp0, uptr ist)
{
    cpu.gdt[0] = 0;
    cpu.gdt[1] = 0x00af9a000000ffffULL;
    cpu.gdt[2] = 0x00cf92000000ffffULL;
    cpu.gdt[3] = 0x00cff2000000ffffULL;
    cpu.gdt[4] = 0x00affa000000ffffULL;
    const auto tss_address = reinterpret_cast<uptr>(&cpu.tss);
    cpu.gdt[5] = (sizeof(cpu.tss) - 1) | ((tss_address & 0xffffff) << 16) | (u64{0x89} << 40) |
                 (((tss_address >> 24) & 0xff) << 56);
    cpu.gdt[6] = tss_address >> 32;
    cpu.tss.rsp[0] = rsp0;
    cpu.tss.ist[0] = ist;
    cpu.stack_top = rsp0;
}
void install_gate(usize vector, uptr entry, u8 ist = 0)
{
    idt[vector] = {static_cast<u16>(entry),       0x08, ist, 0x8e, static_cast<u16>(entry >> 16),
                   static_cast<u32>(entry >> 32), 0};
}
Space *storage(const AddressSpace &space)
{
    return spaces.find(space);
}
u64 lookup(const AddressSpace &space, uptr address)
{
    if (address < user_begin || address >= user_end)
        return 0;
    const auto *value = storage(space);
    if (value == nullptr)
        return 0;
    const auto directory_entry = value->directory->entries[(address >> 21) & 511];
    if ((directory_entry & (present | user)) != (present | user))
        return 0;
    const auto *table = reinterpret_cast<const Table *>(directory_entry & address_mask);
    return table->entries[(address >> 12) & 511];
}
bool permits(u64 entry, bool write)
{
    return (entry & (present | user)) == (present | user) && (!write || (entry & writable) != 0);
}
bool admissible(const PageMapping &mapping)
{
    return mapping.virtual_address >= user_begin && mapping.virtual_address < user_end &&
           (mapping.virtual_address & (page_size - 1)) == 0 && mapping.physical_address >= page_size &&
           mapping.physical_address < user_begin && (mapping.physical_address & (page_size - 1)) == 0 &&
           (mapping.flags == 0 || (mapping.flags & page_read) != 0) &&
           (mapping.flags & ~(page_read | page_write | page_execute)) == 0 &&
           (mapping.flags & (page_write | page_execute)) != (page_write | page_execute);
}
u64 leaf_entry(const PageMapping &mapping)
{
    u64 entry = mapping.physical_address | present | user;
    if ((mapping.flags & page_write) != 0)
        entry |= writable;
    if ((mapping.flags & page_execute) == 0)
        entry |= no_execute;
    return entry;
}
// Table storage is reached through the kernel's identity window, which ends
// where the user window begins.
Result<Table *> allocate_table()
{
    auto page = allocate_table_pages(1);
    if (!page) return page.status();
    if (page.value() >= user_begin)
    {
        static_cast<void>(release_table_pages(page.value(), 1));
        return Status::unsupported("native table storage lies outside the identity window");
    }
    return reinterpret_cast<Table *>(page.value());
}
void release_table(Table *&table, Status &result)
{
    if (!table) return;
    if (auto status = release_table_pages(reinterpret_cast<uptr>(table), 1); !status && result) result = status;
    table = nullptr;
}
// Leaves first, then the fixed tables; the first refused release is reported.
Status release_tables(Space &value)
{
    Status result{};
    if (value.directory)
        for (auto &entry : value.directory->entries)
            if (entry & present)
            {
                auto *leaf = reinterpret_cast<Table *>(entry & address_mask);
                release_table(leaf, result);
                entry = 0;
                --value.tables;
            }
    release_table(value.directory, result);
    release_table(value.pdpt, result);
    release_table(value.pml4, result);
    return result;
}

} // namespace

namespace x86
{
constinit CpuLocal ok_x86_cpu_locals[smp::max_cpus]{};

void prepare_cpu(smp::CpuId id, u32 apic_id)
{
    auto &cpu = ok_x86_cpu_locals[id];
    cpu = {};
    cpu.self = &cpu;
    cpu.logical = id;
    cpu.apic_id = apic_id;
    fill_descriptors(cpu, reinterpret_cast<uptr>(secondary_interrupt_stacks[id - 1] + sizeof(secondary_interrupt_stacks[0])),
                     reinterpret_cast<uptr>(emergency_stacks[id] + sizeof(emergency_stacks[0])));
}

void load_cpu(smp::CpuId id)
{
    auto &cpu = ok_x86_cpu_locals[id];
    write_msr(0xc0000101, reinterpret_cast<uptr>(&cpu)); // GS_BASE
    write_msr(0xc0000102, 0);                            // KERNEL_GS_BASE
    const DescriptorPointer gdtr{sizeof(cpu.gdt) - 1, reinterpret_cast<uptr>(cpu.gdt)};
    asm volatile("lgdt %0; mov $0x28, %%ax; ltr %%ax" : : "m"(gdtr) : "rax", "memory");
    const DescriptorPointer idtr{sizeof(idt) - 1, reinterpret_cast<uptr>(idt)};
    asm volatile("lidt %0" : : "m"(idtr) : "memory");
    write_msr(0xc0000080, read_msr(0xc0000080) | 0x801); // EFER.SCE and NXE.
    write_msr(0xc0000081, (u64{0x10} << 48) | (u64{0x08} << 32));
    write_msr(0xc0000082, reinterpret_cast<uptr>(ok_x86_syscall_entry));
    write_msr(0xc0000084, 0x44700); // IF, TF, DF, NT and AC clear on SYSCALL.
    uptr cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= uptr{1} << 16; // Supervisor writes also respect read-only PTEs.
    asm volatile("mov %0, %%cr0; clts" : : "r"(cr0) : "memory");
    // PCIDs where the CPU has them. Setting PCIDE needs long mode and CR3[11:0]
    // clear, which the boot PML4 load guarantees on every CPU.
    uptr cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    const bool pcid = pcid_supported();
    if (pcid && (cr4 & cr4_pcide) == 0)
        asm volatile("mov %0, %%cr4" : : "r"(cr4 | cr4_pcide) : "memory");
    cpu.tlb_tags.configure(pcid ? 12 : 0);
}
} // namespace x86

bool supported() noexcept
{
    return true;
}

TlbTagReport tlb_tag_report(u32 online_mask) noexcept
{
    return summarize_tlb_tags(online_mask, smp::max_cpus, true,
                              [](u32 cpu) -> const auto & { return x86::ok_x86_cpu_locals[cpu].tlb_tags; });
}

OwnershipReport ownership_report() noexcept
{
    OwnershipReport report{};
    for (const auto &cpu : x86::ok_x86_cpu_locals)
        report.executing = report.executing || cpu.executing || cpu.active_frame != nullptr || cpu.resume_rsp != 0 ||
                           cpu.user_rsp != 0;
    spaces.each([&](const Space &value, uptr root) {
        ++report.address_spaces;
        report.table_pages += 3 + value.tables;
        if (!value.pml4 || !value.pdpt || !value.directory || root != reinterpret_cast<uptr>(value.pml4))
            report.failure = Status::io_error("native backend has an inconsistent table owner");
    });
    report.table_pages += spaces.pages();
    if (!initialized && (report.address_spaces || report.executing))
        report.failure = Status::io_error("uninitialized native backend retains owners");
    return report;
}

Status initialize()
{
    if (initialized)
        return Status::success();
    u32 a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0u));
    if ((d & (1u << 20)) == 0)
        return Status::unsupported("x86 user execution requires NX pages");

    uptr saved_flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(saved_flags) : : "memory");
    const auto *boot_pml4 = reinterpret_cast<const Table *>(read_cr3() & address_mask);
    const auto *boot_pdpt = reinterpret_cast<const Table *>(boot_pml4->entries[0] & address_mask);
    for (usize i = 0; i < 512; ++i)
        kernel_pdpt[i] = boot_pdpt->entries[i];

    for (usize i = 0; i < 256; ++i)
        install_gate(i, reinterpret_cast<uptr>(ok_x86_trap_unexpected));
    for (usize i = 0; i < 48; ++i)
        install_gate(i, ok_x86_execution_vectors[i], i == 8 ? 1 : 0);
    install_gate(lapic::timer_vector, reinterpret_cast<uptr>(ok_x86_trap_239));
    install_gate(lapic::ipi_vector, reinterpret_cast<uptr>(ok_x86_trap_240));
    install_gate(lapic::spurious_vector, reinterpret_cast<uptr>(ok_x86_trap_255));

    auto &boot = x86::ok_x86_cpu_locals[0];
    boot.self = &boot;
    fill_descriptors(boot, reinterpret_cast<uptr>(interrupt_stack + sizeof(interrupt_stack)),
                     reinterpret_cast<uptr>(emergency_stacks[0] + sizeof(emergency_stacks[0])));
    x86::load_cpu(0);

    const auto master_mask = inb(0x21);
    const auto slave_mask = inb(0xa1);
    outb(0x20, 0x11);
    io_wait();
    outb(0xa0, 0x11);
    io_wait();
    outb(0x21, 0x20);
    io_wait();
    outb(0xa1, 0x28);
    io_wait();
    outb(0x21, 0x04);
    io_wait();
    outb(0xa1, 0x02);
    io_wait();
    outb(0x21, 0x01);
    io_wait();
    outb(0xa1, 0x01);
    io_wait();
    outb(0x21, master_mask);
    outb(0xa1, slave_mask);
    outb(0x43, 0x34); // Channel 0, low/high byte, mode 2, binary.
    outb(0x40, pit_divisor & 0xff);
    outb(0x40, pit_divisor >> 8);
    initialized = true;
    asm volatile("push %0; popfq" : : "r"(saved_flags) : "memory", "cc");
    return Status::success();
}

Result<AddressSpace> create_space(MappingView mappings)
{
    if (!initialized)
        return Status::not_initialized("native execution backend is not initialized");
    if (!mappings.valid() || mappings.empty())
        return Status::invalid_argument("invalid native mapping directory");
    for (const auto &mapping : mappings)
        if (!admissible(mapping))
            return Status::invalid_argument("invalid native user-page mapping or writable executable page");
    auto entry = [&]() -> Result<SpaceTable<Space>::Entry *> {
        smp::ScopedSpinLock guard(spaces_lock);
        auto acquired = spaces.acquire();
        if (acquired) acquired.value()->space.tlb = space_ids.issue();
        return acquired;
    }();
    if (!entry)
        return entry.status();
    auto &value = entry.value()->space;
    auto fail = [&](Status status) {
        static_cast<void>(release_tables(value));
        smp::ScopedSpinLock guard(spaces_lock);
        static_cast<void>(spaces.release(value));
        return status;
    };
    for (Table **table : {&value.pml4, &value.pdpt, &value.directory})
    {
        auto allocated = allocate_table();
        if (!allocated)
            return fail(allocated.status());
        *table = allocated.value();
    }
    value.pml4->entries[0] = reinterpret_cast<uptr>(value.pdpt) | present | writable | user;
    for (usize i = 0; i < 512; ++i)
        value.pdpt->entries[i] = kernel_pdpt[i] & ~user;
    value.pdpt->entries[1] = reinterpret_cast<uptr>(value.directory) | present | writable | user;
    for (const auto &mapping : mappings)
    {
        if (mapping.flags == 0) continue; // PROT_NONE owns backing without a PTE.
        auto &directory_entry = value.directory->entries[(mapping.virtual_address >> 21) & 511];
        if ((directory_entry & present) == 0)
        {
            auto table = allocate_table();
            if (!table)
                return fail(table.status());
            directory_entry = reinterpret_cast<uptr>(table.value()) | present | writable | user;
            ++value.tables;
        }
        reinterpret_cast<Table *>(directory_entry & address_mask)->entries[(mapping.virtual_address >> 12) & 511] =
            leaf_entry(mapping);
    }
    entry.value()->root = reinterpret_cast<uptr>(value.pml4);
    return spaces.handle(*entry.value());
}

Status remap(const AddressSpace &space, const PageMapping &mapping)
{
    auto *value = storage(space);
    if (value == nullptr)
        return Status::not_found("native address-space handle is stale");
    if (this_cpu().executing)
        return Status::busy("cannot remap an address space during native execution");
    if (!admissible(mapping) || mapping.flags == 0)
        return Status::invalid_argument("invalid native user-page remap");
    const auto directory_entry = value->directory->entries[(mapping.virtual_address >> 21) & 511];
    if ((directory_entry & present) == 0)
        return Status::not_found("native user page has no translation");
    auto &entry = reinterpret_cast<Table *>(directory_entry & address_mask)->entries[(mapping.virtual_address >> 12) & 511];
    if ((entry & present) == 0)
        return Status::not_found("native user page has no translation");
    // Every CPU running this space leaves it first. A CPU that cached the old
    // translation drops it at its next entry: an untagged CR3 load flushes
    // it, and a PCID whose generation moved is loaded without the keep bit.
    smp::quiesce_root(space.root);
    entry = leaf_entry(mapping);
    smp::ScopedSpinLock guard(spaces_lock);
    value->tlb.changed();
    return Status::success();
}

Status destroy_space(AddressSpace &space)
{
    auto *value = storage(space);
    if (value == nullptr)
        return Status::not_found("native address-space handle is stale");
    if (this_cpu().executing)
        return Status::busy("cannot destroy an address space during native execution");
    smp::quiesce_root(space.root);
    const auto released = release_tables(*value);
    Status retired;
    {
        smp::ScopedSpinLock guard(spaces_lock);
        retired = spaces.release(*value);
    }
    space = {};
    return !released ? released : retired;
}

UserFrame initial_frame(uptr entry, uptr stack)
{
    UserFrame frame{};
    frame.pc = entry;
    frame.sp = stack;
    frame.flags = 0x202;
    frame.fp_state[0] = 0x7f; // x87 reset control word 0x037f.
    frame.fp_state[1] = 0x03;
    frame.fp_state[24] = 0x80; // MXCSR reset state 0x1f80.
    frame.fp_state[25] = 0x1f;
    return frame;
}

Syscall syscall(const UserFrame &frame)
{
    return {frame.registers[rax_slot],
            {frame.registers[rdi_slot], frame.registers[rsi_slot], frame.registers[rdx_slot], frame.registers[r10_slot],
             frame.registers[r8_slot], frame.registers[r9_slot]}};
}
void set_return_value(UserFrame &frame, i64 value)
{
    frame.registers[rax_slot] = static_cast<uptr>(value);
}

bool user_range(const AddressSpace &space, uptr address, usize size, bool write)
{
    if (storage(space) == nullptr)
        return false;
    if (size == 0)
        return true;
    if (address < user_begin || address >= user_end || size > user_end - address)
        return false;
    const auto last = address + size - 1;
    for (auto page = address & ~(page_size - 1); page <= last; page += page_size)
        if (!permits(lookup(space, page), write) &&
            !(write && permits(lookup(space, page), false) && resolve_write(space, page) &&
              permits(lookup(space, page), true)))
            return false;
    return true;
}

Status copy_from_user(const AddressSpace &space, void *destination, uptr source, usize size)
{
    if ((size != 0 && destination == nullptr) || !user_range(space, source, size))
        return Status::fault("user source range is not mapped readable memory");
    auto *out = static_cast<u8 *>(destination);
    while (size != 0)
    {
        const auto offset = source & (page_size - 1);
        const auto length = size < page_size - offset ? size : page_size - offset;
        const auto *in = reinterpret_cast<const u8 *>((lookup(space, source) & address_mask) + offset);
        for (usize i = 0; i < length; ++i)
            out[i] = in[i];
        out += length;
        source += length;
        size -= length;
    }
    return Status::success();
}
Status copy_to_user(const AddressSpace &space, uptr destination, const void *source, usize size)
{
    if ((size != 0 && source == nullptr) || !user_range(space, destination, size, true))
        return Status::fault("user destination range is not mapped writable memory");
    const auto *in = static_cast<const u8 *>(source);
    while (size != 0)
    {
        const auto offset = destination & (page_size - 1);
        const auto length = size < page_size - offset ? size : page_size - offset;
        auto *out = reinterpret_cast<u8 *>((lookup(space, destination) & address_mask) + offset);
        for (usize i = 0; i < length; ++i)
            out[i] = in[i];
        in += length;
        destination += length;
        size -= length;
    }
    return Status::success();
}

static Result<Trap> enter_impl(AddressSpace &space, UserFrame &frame, uptr stack_top)
{
    if (!initialized)
        return Status::not_initialized("native address space is unavailable");
    auto &cpu = this_cpu();
    if (cpu.executing)
        return Status::busy("this CPU is already executing native code");
    u64 code;
    detail::TlbSpaceTag tlb;
    {
        smp::ScopedSpinLock guard(spaces_lock);
        const auto *value = storage(space);
        if (value == nullptr)
            return Status::not_initialized("native address space is unavailable");
        code = lookup(space, frame.pc);
        tlb = value->tlb;
    }
    // Like Linux, the stack need not be mapped; only an access to it faults (a
    // detached musl thread unmaps its own stack before its exit syscall).
    if (!permits(code, false) || (code & no_execute) != 0 || frame.sp <= user_begin || frame.sp > user_end)
        return Status::fault("native user entry or stack pointer is invalid");
    if ((frame.fs_base != 0 && (frame.fs_base < user_begin || frame.fs_base >= user_end)) ||
        (frame.gs_base != 0 && (frame.gs_base < user_begin || frame.gs_base >= user_end)))
        return Status::fault("user TLS base lies outside the native user window");
    frame.flags = (frame.flags & 0x250fd5u) | 0x202u;
    uptr saved_mask;
    asm volatile("pushfq; pop %0; cli" : "=r"(saved_mask) : : "memory");
    const auto old_stack_top = cpu.tss.rsp[0];
    if (stack_top) cpu.tss.rsp[0] = stack_top;
    cpu.stack_top = cpu.tss.rsp[0];
    const auto old_root = read_cr3();
    // The 8259 belongs to the boot CPU; secondaries take only local interrupts.
    const bool boot = cpu.logical == 0;
    const auto old_master = boot ? inb(0x21) : u8{};
    const auto old_slave = boot ? inb(0xa1) : u8{};
    cpu.kernel_fs = read_msr(0xc0000100);
    cpu.executing = true;
    const bool routed = !boot || interrupt::platform_irqs_started();
    if (!routed)
    {
        outb(0x21, 0xfe);
        outb(0xa1, 0xff);
    }
    write_msr(0xc0000100, frame.fs_base);
    write_msr(0xc0000102, frame.gs_base); // GS_BASE after the swapgs to user mode.
    // Untagged, each CR3 load flushes every non-global translation. With
    // PCIDs, the space's PCID keeps its entries unless this CPU must drop
    // them, and the kernel's PCID 0 keeps its own across the entry.
    const bool tagged = cpu.tlb_tags.tagged();
    if (tagged)
    {
        const auto selection = cpu.tlb_tags.select(tlb);
        write_cr3(space.root | selection.tag | (selection.invalidate ? 0 : cr3_keep));
    }
    else
        write_cr3(space.root);
    ok_x86_execution_enter(&frame);
    write_cr3(tagged ? old_root | cr3_keep : old_root);
    if (!routed)
    {
        outb(0x21, old_master);
        outb(0xa1, old_slave);
    }
    cpu.executing = false;
    cpu.active_frame = nullptr;
    cpu.tss.rsp[0] = old_stack_top;
    cpu.resume_rsp = 0;
    cpu.user_rsp = 0;
    cpu.stack_top = old_stack_top;
    asm volatile("push %0; popfq" : : "r"(saved_mask) : "memory", "cc");
    return cpu.last_trap;
}

Result<Trap> enter(AddressSpace &space, UserFrame &frame)
{
    return enter_impl(space, frame, 0);
}
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &stack)
{
    if (!stack.valid() || (stack.lower() & 15) || (stack.top() & 15) ||
        stack.top() <= stack.lower() || stack.top() - stack.lower() < 4096)
        return Status::invalid_argument("native entry requires a live owned kernel stack");
    auto result = enter_impl(space, frame, stack.top());
    if (result && result.value().kernel_stack_pointer &&
        (result.value().kernel_stack_pointer < stack.lower() || result.value().kernel_stack_pointer >= stack.top()))
        return Status::fault("privileged capture escaped the owned kernel stack");
    return result;
}

extern "C" void ok_x86_execution_trap(const SavedTrap *saved)
{
    auto &cpu = this_cpu();
    auto &frame = *cpu.active_frame;
    for (usize i = 0; i < 15; ++i)
        frame.registers[i] = saved->registers[i];
    frame.pc = saved->pc;
    frame.sp = saved->sp;
    frame.flags = saved->flags;
    uptr fault_address = 0;
    if (saved->vector == 14)
        asm volatile("mov %%cr2, %0" : "=r"(fault_address));
    const auto vector = saved->vector;
    cpu.last_trap = {vector == 256                                 ? TrapKind::syscall
                     : vector == 32 || vector == lapic::timer_vector ? TrapKind::timer
                     : vector == 14                                  ? TrapKind::page_fault
                     : (vector > 32 && vector < 48) || vector == lapic::ipi_vector ||
                             vector == lapic::spurious_vector
                         ? TrapKind::interrupt
                         : TrapKind::exception,
                     static_cast<u32>(vector), saved->error, fault_address, reinterpret_cast<uptr>(saved)};
    if (!complete_local(vector) && cpu.logical == 0)
        complete_irq(vector, interrupt::TimerWake::user);
}

extern "C" void ok_x86_execution_kernel_trap(const SavedTrap *saved)
{
    if (saved->vector >= 32 && saved->vector < 48)
    {
        if (this_cpu().logical == 0)
            complete_irq(saved->vector, interrupt::TimerWake::idle);
        return;
    }
    if (complete_local(saved->vector))
        return;
    console("OK_NATIVE_KERNEL_FAULT vector=");
    console_hex(saved->vector);
    console(" pc=");
    console_hex(saved->pc);
    console(" error=");
    console_hex(saved->error);
    console("\n");
    outb(0xf4, 0x11);
    for (;;)
        asm volatile("cli; hlt" : : : "memory");
}

} // namespace ok::arch::execution
