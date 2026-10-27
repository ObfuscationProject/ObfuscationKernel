#include "ok/arch/execution_spaces.hpp"

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
constexpr usize eax_slot = 7;
constexpr usize ebx_slot = 4;
constexpr usize ecx_slot = 6;
constexpr usize edx_slot = 5;
constexpr usize esi_slot = 1;
constexpr usize edi_slot = 0;
constexpr usize ebp_slot = 2;

struct alignas(page_size) Table
{
    u64 entries[512]{};
};
// Each space owns its fixed tables and one leaf table per 2 MiB of user
// window it maps, all from table storage.
struct Space
{
    Table *pdpt{};
    Table *directory{};
    usize tables{};
};
constinit SpaceTable<Space> spaces;
Table kernel_directory{};
alignas(32) u64 kernel_pdpt[4]{};
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
    u32 previous{};
    u32 esp0{};
    u32 ss0{0x10};
    u32 reserved[22]{};
    u16 trap{};
    u16 io_bitmap_offset{sizeof(TaskState)};
};
struct [[gnu::packed]] InterruptGate
{
    u16 offset_low;
    u16 selector;
    u8 reserved;
    u8 attributes;
    u16 offset_high;
};
struct SavedTrap
{
    uptr registers[12];
    uptr vector;
    uptr error;
    uptr pc;
    uptr cs;
    uptr flags;
    uptr sp;
    uptr ss;
};
static_assert(sizeof(TaskState) == 104);
static_assert(sizeof(InterruptGate) == 8);
static_assert(offsetof(SavedTrap, cs) == 60);
static_assert(offsetof(UserFrame, pc) == 128);
static_assert(offsetof(UserFrame, sp) == 132);
static_assert(offsetof(UserFrame, flags) == 136);
static_assert(offsetof(UserFrame, fp_state) == 160);
static_assert(alignof(UserFrame) >= 16);

alignas(16) u64 gdt[8]{};
TaskState tss{};
alignas(16) InterruptGate idt[256]{};
alignas(16) u8 interrupt_stack[64 * 1024]{};

extern "C" void ok_i386_execution_enter(UserFrame *);
extern "C" void ok_i386_syscall_entry();
extern "C" void ok_i386_trap_255();
extern "C" const uptr ok_i386_execution_vectors[48];
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
void install_gate(usize vector, uptr entry, u8 attributes = 0x8e)
{
    idt[vector] = {static_cast<u16>(entry), 0x08, 0, attributes, static_cast<u16>(entry >> 16)};
}
u64 data_descriptor(uptr base)
{
    return 0x00cff2000000ffffULL | (u64{base & 0xffffff} << 16) | (u64{base >> 24} << 56);
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
    return result;
}

} // namespace

extern "C"
{
    uptr ok_i386_execution_resume_esp = 0;
    UserFrame *ok_i386_execution_active_frame = nullptr;
    alignas(16) u8 ok_i386_execution_kernel_fp[512]{};
    u16 ok_i386_execution_kernel_fs = 0;
    u16 ok_i386_execution_kernel_gs = 0;
}

bool supported() noexcept
{
    return true;
}

OwnershipReport ownership_report() noexcept
{
    OwnershipReport report{};
    report.executing = executing || ok_i386_execution_active_frame != nullptr || ok_i386_execution_resume_esp != 0;
    spaces.each([&](const Space &value, uptr root) {
        ++report.address_spaces;
        report.table_pages += 2 + value.tables;
        if (!value.pdpt || !value.directory || root != reinterpret_cast<uptr>(value.pdpt))
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
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    constexpr u32 required = (1u << 6) | (1u << 24) | (1u << 25) | (1u << 26);
    if ((d & required) != required)
        return Status::unsupported("i386 native execution requires PAE, FXSR and SSE2");
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0u));
    if (a < 0x80000001u)
        return Status::unsupported("i386 native execution requires NX pages");
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0u));
    if ((d & (1u << 20)) == 0)
        return Status::unsupported("i386 native execution requires NX pages");

    uptr cr0, cr4;
    asm volatile("mov %%cr0, %0; mov %%cr4, %1" : "=r"(cr0), "=r"(cr4));
    if ((cr0 & (1u << 31)) != 0)
        return Status::unsupported("i386 native initialization expects the paging-disabled bootstrap");
    uptr saved_flags;
    asm volatile("pushfl; pop %0; cli" : "=r"(saved_flags) : : "memory");
    for (usize i = 0; i < 512; ++i)
        kernel_directory.entries[i] = (u64{i} << 21) | present | writable | 0x80;
    kernel_pdpt[0] = reinterpret_cast<uptr>(&kernel_directory) | present;
    cr4 |= (1u << 5) | (1u << 9) | (1u << 10);
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    write_msr(0xc0000080, read_msr(0xc0000080) | 0x800);
    write_cr3(reinterpret_cast<uptr>(kernel_pdpt));
    cr0 = (cr0 | (1u << 31) | (1u << 16) | (1u << 1)) & ~((1u << 2) | (1u << 3));
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    gdt[0] = 0;
    gdt[1] = 0x00cf9a000000ffffULL;
    gdt[2] = 0x00cf92000000ffffULL;
    gdt[3] = data_descriptor(0);
    gdt[4] = 0x00cffa000000ffffULL;
    const auto tss_address = reinterpret_cast<uptr>(&tss);
    gdt[5] =
        (sizeof(tss) - 1) | (u64{tss_address & 0xffffff} << 16) | (u64{0x89} << 40) | (u64{tss_address >> 24} << 56);
    gdt[6] = data_descriptor(0);
    gdt[7] = data_descriptor(0);
    tss.esp0 = reinterpret_cast<uptr>(interrupt_stack + sizeof(interrupt_stack));
    const DescriptorPointer gdtr{sizeof(gdt) - 1, reinterpret_cast<uptr>(gdt)};
    asm volatile("lgdt %0; mov $0x28, %%ax; ltr %%ax" : : "m"(gdtr) : "eax", "memory");

    for (usize i = 0; i < 256; ++i)
        install_gate(i, reinterpret_cast<uptr>(ok_i386_trap_255));
    for (usize i = 0; i < 48; ++i)
        install_gate(i, ok_i386_execution_vectors[i]);
    install_gate(128, reinterpret_cast<uptr>(ok_i386_syscall_entry), 0xee);
    const DescriptorPointer idtr{sizeof(idt) - 1, reinterpret_cast<uptr>(idt)};
    asm volatile("lidt %0" : : "m"(idtr) : "memory");

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
    asm volatile("push %0; popfl" : : "r"(saved_flags) : "memory", "cc");
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
    auto entry = spaces.acquire();
    if (!entry)
        return entry.status();
    auto &value = entry.value()->space;
    auto fail = [&](Status status) {
        static_cast<void>(release_tables(value));
        static_cast<void>(spaces.release(value));
        return status;
    };
    for (Table **table : {&value.pdpt, &value.directory})
    {
        auto allocated = allocate_table();
        if (!allocated)
            return fail(allocated.status());
        *table = allocated.value();
    }
    value.pdpt->entries[0] = kernel_pdpt[0];
    value.pdpt->entries[1] = reinterpret_cast<uptr>(value.directory) | present;
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
    entry.value()->root = reinterpret_cast<uptr>(value.pdpt);
    return spaces.handle(*entry.value());
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
    const auto directory_entry = value->directory->entries[(mapping.virtual_address >> 21) & 511];
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
    const auto released = release_tables(*value);
    const auto retired = spaces.release(*value);
    space = {};
    return !released ? released : retired;
}

UserFrame initial_frame(uptr entry, uptr stack)
{
    UserFrame frame{};
    frame.pc = entry;
    frame.sp = stack;
    frame.flags = 0x202;
    frame.registers[8] = 0x3b;
    frame.registers[9] = 0x33;
    frame.registers[10] = 0x1b;
    frame.registers[11] = 0x1b;
    frame.registers[12] = 0x1b; // SS selector; frame.sp stores the architectural ESP.
    frame.fp_state[0] = 0x7f;   // x87 reset control word 0x037f.
    frame.fp_state[1] = 0x03;
    frame.fp_state[24] = 0x80; // MXCSR reset state 0x1f80.
    frame.fp_state[25] = 0x1f;
    return frame;
}

Syscall syscall(const UserFrame &frame)
{
    return {frame.registers[eax_slot],
            {frame.registers[ebx_slot], frame.registers[ecx_slot], frame.registers[edx_slot], frame.registers[esi_slot],
             frame.registers[edi_slot], frame.registers[ebp_slot]}};
}
void set_return_value(UserFrame &frame, i64 value)
{
    frame.registers[eax_slot] = static_cast<uptr>(value);
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
    if (!permits(code, false) || (code & no_execute) != 0)
        return Status::fault("native user entry has invalid permissions");
    for (usize i = 8; i < 12; ++i)
    {
        const auto selector = frame.registers[i] & 0xffff;
        if (selector > 3 && selector != 0x1b && selector != 0x23 && selector != 0x33 && selector != 0x3b)
            return Status::fault("invalid native user segment selector");
    }
    const auto stack_selector = frame.registers[12] & 0xffff;
    if (stack_selector != 0x1b && stack_selector != 0x33 && stack_selector != 0x3b)
        return Status::fault("invalid native user stack selector");
    const auto stack_base = stack_selector == 0x33 ? frame.fs_base : stack_selector == 0x3b ? frame.gs_base : 0;
    const auto stack_address = frame.sp + stack_base;
    if (stack_address < frame.sp || stack_address <= user_begin || stack_address > user_end ||
        !user_range(space, stack_address - 1, 1, true))
        return Status::fault("native user stack has invalid permissions");
    if ((frame.fs_base != 0 && (frame.fs_base < user_begin || frame.fs_base >= user_end)) ||
        (frame.gs_base != 0 && (frame.gs_base < user_begin || frame.gs_base >= user_end)))
        return Status::fault("user TLS base lies outside the native user window");
    frame.flags = (frame.flags & 0x250fd5u) | 0x202u;
    uptr saved_mask;
    asm volatile("pushfl; pop %0; cli" : "=r"(saved_mask) : : "memory");
    const auto old_stack_top = tss.esp0;
    if (stack_top) tss.esp0 = stack_top;
    const auto old_root = read_cr3();
    const auto old_master = inb(0x21);
    const auto old_slave = inb(0xa1);
    asm volatile("mov %%fs, %0; mov %%gs, %1" : "=m"(ok_i386_execution_kernel_fs), "=m"(ok_i386_execution_kernel_gs));
    executing = true;
    outb(0x21, 0xfe);
    outb(0xa1, 0xff);
    gdt[6] = data_descriptor(frame.fs_base);
    gdt[7] = data_descriptor(frame.gs_base);
    write_cr3(space.root);
    ok_i386_execution_enter(&frame);
    write_cr3(old_root);
    outb(0x21, old_master);
    outb(0xa1, old_slave);
    executing = false;
    ok_i386_execution_active_frame = nullptr;
    tss.esp0 = old_stack_top;
    ok_i386_execution_resume_esp = 0;
    asm volatile("push %0; popfl" : : "r"(saved_mask) : "memory", "cc");
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

extern "C" void ok_i386_execution_trap(const SavedTrap *saved)
{
    auto &frame = *ok_i386_execution_active_frame;
    for (usize i = 0; i < 12; ++i)
        frame.registers[i] = saved->registers[i];
    frame.registers[12] = saved->ss;
    frame.pc = saved->pc;
    frame.sp = saved->sp;
    frame.flags = saved->flags;
    uptr fault_address = 0;
    if (saved->vector == 14)
        asm volatile("mov %%cr2, %0" : "=r"(fault_address));
    last_trap = {saved->vector == 128  ? TrapKind::syscall
                 : saved->vector == 32 ? TrapKind::timer
                 : saved->vector == 14 ? TrapKind::page_fault
                                       : TrapKind::exception,
                 static_cast<u32>(saved->vector), saved->error, fault_address, reinterpret_cast<uptr>(saved)};
    acknowledge_irq(saved->vector);
}

extern "C" void ok_i386_execution_kernel_trap(const SavedTrap *saved)
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
