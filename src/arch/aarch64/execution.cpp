#include "ok/arch/execution.hpp"

#include <cstddef>

namespace ok::arch::execution
{
namespace
{
constexpr u64 valid = 1;
constexpr u64 table_or_page = 3;
constexpr u64 user_access = 1u << 6;
constexpr u64 read_only = 1u << 7;
constexpr u64 normal_memory = (1u << 2) | (3u << 8);
constexpr u64 accessed = 1u << 10;
constexpr u64 not_global = 1u << 11;
constexpr u64 privileged_xn = u64{1} << 53;
constexpr u64 user_xn = u64{1} << 54;
constexpr u64 address_mask = 0x0000fffffffff000ULL;
constexpr usize table_count = 16;
constexpr uptr ram_begin = 0x40000000;
constexpr uptr ram_end = 0x80000000;
constexpr uptr gic_distributor = 0x08000000;
constexpr uptr gic_cpu = 0x08010000;
constexpr u32 timer_irq = 30;

struct alignas(page_size) Table
{
    u64 entries[512]{};
};
struct SpaceStorage
{
    Table root{};
    Table directory{};
    Table tables[table_count]{};
    usize tables_used{0};
    u32 generation{0};
    bool allocated{false};
};
SpaceStorage spaces[max_address_spaces]{};
Table kernel_root{};
Table kernel_directory{};
bool initialized = false;
bool executing = false;
Trap last_trap{};
u64 timer_period = 0;
alignas(16) u8 interrupt_stack[64 * 1024]{};

extern "C" void ok_arm_execution_enter(UserFrame *);
extern "C" void ok_arm_execution_vectors();
extern "C" void ok_platform_console_write_char(char);

static_assert(offsetof(UserFrame, pc) == 256);
static_assert(offsetof(UserFrame, sp) == 264);
static_assert(offsetof(UserFrame, flags) == 272);
static_assert(offsetof(UserFrame, fs_base) == 280);
static_assert(offsetof(UserFrame, gs_base) == 288);
static_assert(offsetof(UserFrame, fp_state) == 304);

volatile u32 &mmio(uptr address)
{
    return *reinterpret_cast<volatile u32 *>(address);
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
void select_root(uptr root)
{
    asm volatile("dsb ishst; msr ttbr0_el1, %0; isb; tlbi vmalle1is; dsb ish; isb" : : "r"(root) : "memory");
}
void populate_kernel(Table &root, Table &directory)
{
    root.entries[0] = reinterpret_cast<uptr>(&directory) | table_or_page;
    root.entries[1] = ram_begin | valid | normal_memory | accessed | user_xn;
    // Only the low 256 MiB is platform MMIO. The remainder of this 1 GiB
    // directory is exclusively user space and starts completely unmapped.
    for (usize i = 0; i < user_begin / (2 * 1024 * 1024); ++i)
        directory.entries[i] = i * 2 * 1024 * 1024 | valid | accessed | privileged_xn | user_xn;
}
SpaceStorage *storage(const AddressSpace &space)
{
    if (space.slot >= max_address_spaces)
        return nullptr;
    auto &value = spaces[space.slot];
    return value.allocated && value.generation == space.generation && reinterpret_cast<uptr>(&value.root) == space.root
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
    if ((directory_entry & table_or_page) != table_or_page)
        return 0;
    const auto *table = reinterpret_cast<const Table *>(directory_entry & address_mask);
    return table->entries[(address >> 12) & 511];
}
bool permits(u64 entry, bool write)
{
    return (entry & (table_or_page | user_access)) == (table_or_page | user_access) &&
           (!write || (entry & read_only) == 0);
}
void clean_code(uptr address)
{
    u64 cache_type;
    asm volatile("mrs %0, ctr_el0" : "=r"(cache_type));
    const auto stride = usize{4} << ((cache_type >> 16) & 15);
    for (usize offset = 0; offset < page_size; offset += stride)
        asm volatile("dc cvau, %0" : : "r"(address + offset) : "memory");
}
} // namespace

extern "C"
{
    uptr ok_arm_execution_resume_sp = 0;
    uptr ok_arm_execution_stack_top = 0;
    UserFrame *ok_arm_execution_active_frame = nullptr;
    alignas(16) u8 ok_arm_execution_kernel_fp[512]{};
}

bool supported() noexcept
{
    return true;
}

Status initialize()
{
    if (initialized)
        return Status::success();
    u64 current_el, control;
    asm volatile("mrs %0, CurrentEL; mrs %1, sctlr_el1" : "=r"(current_el), "=r"(control));
    if (current_el != 4)
        return Status::unsupported("AArch64 native execution requires an EL1 kernel");
    if ((control & 1) != 0)
        return Status::unsupported("AArch64 native setup requires the boot identity environment");
    u64 saved_mask;
    asm volatile("mrs %0, daif; msr daifset, #0xf" : "=r"(saved_mask) : : "memory");
    populate_kernel(kernel_root, kernel_directory);
    ok_arm_execution_stack_top = reinterpret_cast<uptr>(interrupt_stack + sizeof(interrupt_stack));
    const u64 vectors = reinterpret_cast<uptr>(ok_arm_execution_vectors);
    // 39-bit VA, 4 KiB granule, inner-shareable WBWA tables, 40-bit physical
    // addresses, and TTBR1 disabled. QEMU virt RAM and MMIO stay identity mapped.
    const u64 tcr = 25 | (u64{1} << 8) | (u64{1} << 10) | (u64{3} << 12) | (u64{1} << 23) | (u64{2} << 32);
    const u64 mair = 0xff00;
    const u64 fp_control = u64{3} << 20;
    asm volatile("msr vbar_el1, %0; msr mair_el1, %1; msr tcr_el1, %2; msr cpacr_el1, %3; isb"
                 :
                 : "r"(vectors), "r"(mair), "r"(tcr), "r"(fp_control)
                 : "memory");
    select_root(reinterpret_cast<uptr>(&kernel_root));
    control |= (u64{1} << 0) | (u64{1} << 2) | (u64{1} << 12); // MMU, data cache, instruction cache.
    asm volatile("ic iallu; dsb sy; isb; msr sctlr_el1, %0; isb" : : "r"(control) : "memory");

    // QEMU virt's GICv2 private physical timer interrupt. Other device IRQs stay
    // masked until a driver explicitly establishes its own hardware IRQ path.
    mmio(gic_distributor + 0x180) = 0xffffffff;
    mmio(gic_distributor + 0x280) = 0xffffffff;
    *reinterpret_cast<volatile u8 *>(gic_distributor + 0x400 + timer_irq) = 0x80;
    mmio(gic_distributor + 0x100) = u32{1} << timer_irq;
    mmio(gic_distributor) = 1;
    mmio(gic_cpu + 4) = 0xff;
    mmio(gic_cpu + 8) = 0;
    mmio(gic_cpu) = 1;
    asm volatile("mrs %0, cntfrq_el0; msr cntp_ctl_el0, xzr; dsb sy" : "=r"(timer_period) : : "memory");
    timer_period /= 100;
    if (timer_period == 0)
        timer_period = 1;
    initialized = true;
    asm volatile("msr daif, %0" : : "r"(saved_mask) : "memory");
    return Status::success();
}

Result<AddressSpace> create_space(std::span<const PageMapping> mappings)
{
    if (!initialized)
        return Status::not_initialized("native execution backend is not initialized");
    usize slot = 0;
    while (slot < max_address_spaces && spaces[slot].allocated)
        ++slot;
    if (slot == max_address_spaces)
        return Status::no_memory("native address-space slots exhausted");
    auto &value = spaces[slot];
    const auto generation = value.generation + 1;
    value = {};
    value.generation = generation == 0 ? 1 : generation;
    populate_kernel(value.root, value.directory);
    for (const auto &mapping : mappings)
    {
        if (mapping.virtual_address < user_begin || mapping.virtual_address >= user_end ||
            (mapping.virtual_address & (page_size - 1)) != 0 || mapping.physical_address < ram_begin ||
            mapping.physical_address >= ram_end || (mapping.physical_address & (page_size - 1)) != 0 ||
            (mapping.flags & page_read) == 0 || (mapping.flags & ~(page_read | page_write | page_execute)) != 0 ||
            (mapping.flags & (page_write | page_execute)) == (page_write | page_execute))
            return Status::invalid_argument("invalid native user-page mapping or writable executable page");
        auto &directory_entry = value.directory.entries[(mapping.virtual_address >> 21) & 511];
        if ((directory_entry & valid) == 0)
        {
            if (value.tables_used == table_count)
                return Status::no_memory("native page-table pool exhausted");
            directory_entry = reinterpret_cast<uptr>(&value.tables[value.tables_used++]) | table_or_page;
        }
        auto *table = reinterpret_cast<Table *>(directory_entry & address_mask);
        auto &entry = table->entries[(mapping.virtual_address >> 12) & 511];
        if ((entry & valid) != 0)
            return Status::already_exists("duplicate native user-page mapping");
        entry = mapping.physical_address | table_or_page | normal_memory | accessed | not_global | user_access |
                privileged_xn;
        if ((mapping.flags & page_write) == 0)
            entry |= read_only;
        if ((mapping.flags & page_execute) == 0)
            entry |= user_xn;
        else
            clean_code(mapping.physical_address);
    }
    asm volatile("dsb ish; ic iallu; dsb ish; isb" : : : "memory");
    value.allocated = true;
    return AddressSpace{reinterpret_cast<uptr>(&value.root), static_cast<u32>(slot), value.generation};
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
    return frame;
}
Syscall syscall(const UserFrame &frame)
{
    return {frame.registers[8],
            {frame.registers[0], frame.registers[1], frame.registers[2], frame.registers[3], frame.registers[4],
             frame.registers[5]}};
}
void set_return_value(UserFrame &frame, i64 value)
{
    frame.registers[0] = static_cast<uptr>(value);
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
        if (!permits(lookup(space, page), write))
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

Result<Trap> enter(AddressSpace &space, UserFrame &frame)
{
    if (!initialized || storage(space) == nullptr)
        return Status::not_initialized("native address space is unavailable");
    if (executing)
        return Status::busy("native backend currently supports one executing CPU");
    const auto code = lookup(space, frame.pc);
    if (!permits(code, false) || (code & user_xn) != 0 || (frame.pc & 3) != 0 || frame.sp <= user_begin ||
        frame.sp > user_end || (frame.sp & 15) != 0 || !user_range(space, frame.sp - 1, 1, true))
        return Status::fault("native user entry or stack has invalid permissions");
    if ((frame.fs_base != 0 && (frame.fs_base < user_begin || frame.fs_base >= user_end)) ||
        (frame.gs_base != 0 && (frame.gs_base < user_begin || frame.gs_base >= user_end)))
        return Status::fault("user TLS base lies outside the native user window");
    frame.flags &= 0xf0000000; // Preserve NZCV only; always EL0t with IRQs enabled.
    u64 saved_mask;
    asm volatile("mrs %0, daif; msr daifset, #0xf" : "=r"(saved_mask) : : "memory");
    executing = true;
    select_root(space.root);
    asm volatile("msr cntp_tval_el0, %0; msr cntp_ctl_el0, %1; isb" : : "r"(timer_period), "r"(u64{1}) : "memory");
    ok_arm_execution_enter(&frame);
    asm volatile("msr cntp_ctl_el0, xzr; isb" : : : "memory");
    select_root(reinterpret_cast<uptr>(&kernel_root));
    executing = false;
    ok_arm_execution_active_frame = nullptr;
    asm volatile("msr daif, %0" : : "r"(saved_mask) : "memory");
    return last_trap;
}

extern "C" void ok_arm_execution_trap(u64 vector)
{
    u64 syndrome, fault_address;
    asm volatile("mrs %0, esr_el1; mrs %1, far_el1" : "=r"(syndrome), "=r"(fault_address));
    if (vector == 9)
    {
        const auto acknowledge = mmio(gic_cpu + 0xc);
        const auto irq = acknowledge & 0x3ff;
        asm volatile("msr cntp_ctl_el0, xzr; isb" : : : "memory");
        if (irq < 1020)
            mmio(gic_cpu + 0x10) = acknowledge;
        last_trap = {irq == timer_irq ? TrapKind::timer : TrapKind::exception, irq, 0, 0};
        return;
    }
    const auto exception_class = (syndrome >> 26) & 0x3f;
    const bool page_fault = exception_class == 0x20 || exception_class == 0x24;
    last_trap = {vector == 8 && exception_class == 0x15 ? TrapKind::syscall
                 : page_fault                           ? TrapKind::page_fault
                                                        : TrapKind::exception,
                 static_cast<u32>(exception_class), syndrome, page_fault ? fault_address : 0};
}

extern "C" [[noreturn]] void ok_arm_execution_kernel_fault(u64 vector)
{
    u64 syndrome, pc, address;
    asm volatile("mrs %0, esr_el1; mrs %1, elr_el1; mrs %2, far_el1" : "=r"(syndrome), "=r"(pc), "=r"(address));
    console("OK_NATIVE_KERNEL_FAULT vector=");
    console_hex(vector);
    console(" pc=");
    console_hex(pc);
    console(" syndrome=");
    console_hex(syndrome);
    console(" address=");
    console_hex(address);
    console("\n");
    for (;;)
        asm volatile("msr daifset, #0xf; wfe" : : : "memory");
}
} // namespace ok::arch::execution
