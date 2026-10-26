#include "ok/arch/execution.hpp"

#include <cstddef>

namespace ok::arch::execution
{
namespace
{
constexpr u64 present = 1;
constexpr u64 writable = 2;
constexpr u64 user = 4;
constexpr u64 no_execute = u64{1} << 63;
constexpr u64 address_mask = 0x000ffffffffff000ULL;
constexpr usize table_count = 16;
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
struct SpaceStorage
{
    Table pml4{};
    Table pdpt{};
    Table directory{};
    Table tables[table_count]{};
    usize tables_used{0};
    u32 generation{0};
    bool allocated{false};
};
SpaceStorage spaces[max_address_spaces]{};
u64 kernel_pdpt[512]{};
bool initialized = false;
bool executing = false;
Trap last_trap{};

struct [[gnu::packed]] DescriptorPointer
{
    u16 limit;
    uptr base;
};
struct [[gnu::packed]] TaskState
{
    u32 reserved0{};
    u64 rsp[3]{};
    u64 reserved1{};
    u64 ist[7]{};
    u64 reserved2{};
    u16 reserved3{};
    u16 io_bitmap_offset{sizeof(TaskState)};
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
static_assert(sizeof(TaskState) == 104);
static_assert(sizeof(InterruptGate) == 16);
static_assert(offsetof(SavedTrap, cs) == 144);
static_assert(offsetof(UserFrame, pc) == 256);
static_assert(offsetof(UserFrame, sp) == 264);
static_assert(offsetof(UserFrame, flags) == 272);
static_assert(offsetof(UserFrame, fp_state) == 304);
static_assert(alignof(UserFrame) >= 16);

alignas(16) u64 gdt[7]{};
TaskState tss{};
alignas(16) InterruptGate idt[256]{};
alignas(16) u8 interrupt_stack[64 * 1024]{};
alignas(16) u8 emergency_stack[8 * 1024]{};

extern "C" void ok_x86_execution_enter(UserFrame *);
extern "C" void ok_x86_syscall_entry();
extern "C" void ok_x86_trap_255();
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
void install_gate(usize vector, uptr entry, u8 ist = 0)
{
    idt[vector] = {static_cast<u16>(entry),       0x08, ist, 0x8e, static_cast<u16>(entry >> 16),
                   static_cast<u32>(entry >> 32), 0};
}
SpaceStorage *storage(const AddressSpace &space)
{
    if (space.slot >= max_address_spaces)
        return nullptr;
    auto &value = spaces[space.slot];
    return value.allocated && value.generation == space.generation && reinterpret_cast<uptr>(&value.pml4) == space.root
               ? &value
               : nullptr;
}
u64 lookup(const AddressSpace &space, uptr address)
{
    if (address < user_begin || address >= user_end)
        return 0;
    const auto *value = storage(space);
    if (value == nullptr)
        return 0;
    const auto directory_entry = value->directory.entries[(address >> 21) & 511];
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

} // namespace

extern "C"
{
    uptr ok_x86_execution_resume_rsp = 0;
    uptr ok_x86_execution_user_rsp = 0;
    uptr ok_x86_execution_stack_top = 0;
    UserFrame *ok_x86_execution_active_frame = nullptr;
    alignas(16) u8 ok_x86_execution_kernel_fp[512]{};
    u64 ok_x86_execution_kernel_fs = 0;
    u64 ok_x86_execution_kernel_gs = 0;
}

bool supported() noexcept
{
    return true;
}

OwnershipReport ownership_report() noexcept
{
    OwnershipReport report{};
    report.executing = executing || ok_x86_execution_active_frame != nullptr || ok_x86_execution_resume_rsp != 0 || ok_x86_execution_user_rsp != 0;
    for (const auto &space : spaces)
    {
        if (!space.allocated) continue;
        ++report.address_spaces;
        if (!space.generation || space.tables_used > table_count)
            report.failure = Status::io_error("native backend has an inconsistent table owner");
    }
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

    gdt[0] = 0;
    gdt[1] = 0x00af9a000000ffffULL;
    gdt[2] = 0x00cf92000000ffffULL;
    gdt[3] = 0x00cff2000000ffffULL;
    gdt[4] = 0x00affa000000ffffULL;
    const auto tss_address = reinterpret_cast<uptr>(&tss);
    gdt[5] =
        (sizeof(tss) - 1) | ((tss_address & 0xffffff) << 16) | (u64{0x89} << 40) | (((tss_address >> 24) & 0xff) << 56);
    gdt[6] = tss_address >> 32;
    tss.rsp[0] = reinterpret_cast<uptr>(interrupt_stack + sizeof(interrupt_stack));
    tss.ist[0] = reinterpret_cast<uptr>(emergency_stack + sizeof(emergency_stack));
    ok_x86_execution_stack_top = tss.rsp[0];
    const DescriptorPointer gdtr{sizeof(gdt) - 1, reinterpret_cast<uptr>(gdt)};
    asm volatile("lgdt %0; mov $0x28, %%ax; ltr %%ax" : : "m"(gdtr) : "rax", "memory");

    for (usize i = 0; i < 256; ++i)
        install_gate(i, reinterpret_cast<uptr>(ok_x86_trap_255));
    for (usize i = 0; i < 48; ++i)
        install_gate(i, ok_x86_execution_vectors[i], i == 8 ? 1 : 0);
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
    constexpr u16 pit_divisor = 11932; // Approximately 100 actual IRQs/second.
    outb(0x43, 0x36);
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
    if (!mappings.valid() || mappings.empty() || mappings.size() > max_native_mappings)
        return Status::invalid_argument("invalid native mapping count");
    usize slot = 0;
    while (slot < max_address_spaces && spaces[slot].allocated)
        ++slot;
    if (slot == max_address_spaces)
        return Status::no_memory("native address-space slots exhausted");
    auto &value = spaces[slot];
    const auto generation = value.generation + 1;
    value = {};
    value.generation = generation == 0 ? 1 : generation;
    value.pml4.entries[0] = reinterpret_cast<uptr>(&value.pdpt) | present | writable | user;
    for (usize i = 0; i < 512; ++i)
        value.pdpt.entries[i] = kernel_pdpt[i] & ~user;
    value.pdpt.entries[1] = reinterpret_cast<uptr>(&value.directory) | present | writable | user;
    for (usize index = 0; index < mappings.size(); ++index)
    {
        const auto &mapping = mappings[index];
        if (!admissible(mapping))
            return Status::invalid_argument("invalid native user-page mapping or writable executable page");
        for (usize previous = 0; previous < index; ++previous)
            if (mappings[previous].virtual_address == mapping.virtual_address)
                return Status::already_exists("duplicate native user-page mapping");
        if (mapping.flags == 0) continue; // PROT_NONE owns backing without a PTE.
        auto &directory_entry = value.directory.entries[(mapping.virtual_address >> 21) & 511];
        if ((directory_entry & present) == 0)
        {
            if (value.tables_used == table_count)
                return Status::no_memory("native page-table pool exhausted");
            directory_entry = reinterpret_cast<uptr>(&value.tables[value.tables_used++]) | present | writable | user;
        }
        auto *table = reinterpret_cast<Table *>(directory_entry & address_mask);
        auto &entry = table->entries[(mapping.virtual_address >> 12) & 511];
        if ((entry & present) != 0)
            return Status::already_exists("duplicate native user-page mapping");
        entry = leaf_entry(mapping);
    }
    value.allocated = true;
    return AddressSpace{reinterpret_cast<uptr>(&value.pml4), static_cast<u32>(slot), value.generation};
}

Status remap(const AddressSpace &space, const PageMapping &mapping)
{
    auto *value = storage(space);
    if (value == nullptr)
        return Status::not_found("native address-space handle is stale");
    if (executing)
        return Status::busy("cannot remap an address space during native execution");
    if (!admissible(mapping) || mapping.flags == 0)
        return Status::invalid_argument("invalid native user-page remap");
    const auto directory_entry = value->directory.entries[(mapping.virtual_address >> 21) & 511];
    if ((directory_entry & present) == 0)
        return Status::not_found("native user page has no translation");
    auto &entry = reinterpret_cast<Table *>(directory_entry & address_mask)->entries[(mapping.virtual_address >> 12) & 511];
    if ((entry & present) == 0)
        return Status::not_found("native user page has no translation");
    // The next entry reloads CR3, which drops these non-global translations.
    entry = leaf_entry(mapping);
    return Status::success();
}

Status destroy_space(AddressSpace &space)
{
    auto *value = storage(space);
    if (value == nullptr)
        return Status::not_found("native address-space handle is stale");
    if (executing)
        return Status::busy("cannot destroy an address space during native execution");
    value->allocated = false;
    space = {};
    return Status::success();
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
    if (!initialized || storage(space) == nullptr)
        return Status::not_initialized("native address space is unavailable");
    if (executing)
        return Status::busy("native backend currently supports one executing CPU");
    const auto code = lookup(space, frame.pc);
    if (!permits(code, false) || (code & no_execute) != 0 || frame.sp <= user_begin || frame.sp > user_end ||
        !user_range(space, frame.sp - 1, 1, true))
        return Status::fault("native user entry or stack has invalid permissions");
    if ((frame.fs_base != 0 && (frame.fs_base < user_begin || frame.fs_base >= user_end)) ||
        (frame.gs_base != 0 && (frame.gs_base < user_begin || frame.gs_base >= user_end)))
        return Status::fault("user TLS base lies outside the native user window");
    frame.flags = (frame.flags & 0x250fd5u) | 0x202u;
    uptr saved_mask;
    asm volatile("pushfq; pop %0; cli" : "=r"(saved_mask) : : "memory");
    const auto old_stack_top = tss.rsp[0];
    if (stack_top) tss.rsp[0] = stack_top;
    ok_x86_execution_stack_top = tss.rsp[0];
    const auto old_root = read_cr3();
    const auto old_master = inb(0x21);
    const auto old_slave = inb(0xa1);
    ok_x86_execution_kernel_fs = read_msr(0xc0000100);
    ok_x86_execution_kernel_gs = read_msr(0xc0000101);
    executing = true;
    outb(0x21, 0xfe);
    outb(0xa1, 0xff);
    write_msr(0xc0000100, frame.fs_base);
    write_msr(0xc0000101, frame.gs_base);
    write_cr3(space.root);
    ok_x86_execution_enter(&frame);
    write_cr3(old_root);
    outb(0x21, old_master);
    outb(0xa1, old_slave);
    executing = false;
    ok_x86_execution_active_frame = nullptr;
    tss.rsp[0] = old_stack_top;
    ok_x86_execution_resume_rsp = 0;
    ok_x86_execution_user_rsp = 0;
    ok_x86_execution_stack_top = old_stack_top;
    asm volatile("push %0; popfq" : : "r"(saved_mask) : "memory", "cc");
    return last_trap;
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
    auto &frame = *ok_x86_execution_active_frame;
    for (usize i = 0; i < 15; ++i)
        frame.registers[i] = saved->registers[i];
    frame.pc = saved->pc;
    frame.sp = saved->sp;
    frame.flags = saved->flags;
    uptr fault_address = 0;
    if (saved->vector == 14)
        asm volatile("mov %%cr2, %0" : "=r"(fault_address));
    last_trap = {saved->vector == 256  ? TrapKind::syscall
                 : saved->vector == 32 ? TrapKind::timer
                 : saved->vector == 14 ? TrapKind::page_fault
                                       : TrapKind::exception,
                 static_cast<u32>(saved->vector), saved->error, fault_address, reinterpret_cast<uptr>(saved)};
    acknowledge_irq(saved->vector);
}

extern "C" void ok_x86_execution_kernel_trap(const SavedTrap *saved)
{
    if (saved->vector >= 32 && saved->vector < 48)
    {
        acknowledge_irq(saved->vector);
        return;
    }
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
