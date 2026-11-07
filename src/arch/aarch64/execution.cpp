#include "ok/arch/execution_spaces.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/arch/clock.hpp"
#include "ok/smp/bringup.hpp"
#include "ok/smp/kernel_section.hpp"
#include "../arm_common/gicv2.hpp"
#include "../tlb_tags.hpp"
#include "secondary.hpp"

#include <cstddef>

namespace ok::arch::execution
{

// Everything one entry owns lives in its CPU's block, which TPIDR_EL1 names
// on every CPU; tables, vectors and the timer period are shared and fixed
// once initialize() ran. NativeInstructions<aarch64>::cpu_index() reads `cpu`.
struct alignas(64) CpuBlock
{
    u32 cpu{};
    uptr resume_sp{};
    // SP_EL1 while user code runs: the owned entry stack, or CPU 0's
    // interrupt stack for an unowned diagnostic entry.
    uptr stack_top{};
    UserFrame *active_frame{};
    bool executing{};
    Trap last_trap{};
    alignas(64) u8 kernel_fp[512]{};
    // The ASIDs this CPU gives user spaces.
    detail::CpuTlbTags tlb_tags{};
};
static_assert(offsetof(CpuBlock, cpu) == 0);
static_assert(offsetof(CpuBlock, resume_sp) == 8);
static_assert(offsetof(CpuBlock, stack_top) == 16);
static_assert(offsetof(CpuBlock, active_frame) == 24);
static_assert(offsetof(CpuBlock, kernel_fp) == 128);

extern "C"
{
    constinit CpuBlock ok_aarch64_cpu_blocks[ok::smp::max_cpus]{};
}

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
constexpr uptr ram_begin = 0x40000000;
constexpr uptr ram_end = 0x80000000;
constexpr u32 timer_irq = arm::GicV2::timer_ppi;
// 39-bit VA, 4 KiB granule, inner-shareable WBWA tables, 40-bit physical
// addresses, and TTBR1 disabled. QEMU virt RAM and MMIO stay identity mapped.
// TTBR0 holds the ASID (A1 clear). Each CPU sets TCR.AS (16-bit ASIDs) from
// its own ID_AA64MMFR0_EL1, so a CPU never sets a RES0 bit or reports a width
// it does not use.
constexpr u64 tcr = 25 | (u64{1} << 8) | (u64{1} << 10) | (u64{3} << 12) | (u64{1} << 23) | (u64{2} << 32);
constexpr u64 tcr_asid16 = u64{1} << 36;
constexpr unsigned ttbr_asid_shift = 48;
constexpr u64 mair = 0xff00;
constexpr u64 fp_control = u64{3} << 20;
// A secondary CPU's SCTLR_EL1 is UNKNOWN at reset, so it is written whole: the
// MMU and both caches, stack alignment checks, EL0 WFI/WFE untrapped as the
// boot CPU's reset value has them, and the ARMv8.0 RES1 bits.
constexpr u64 secondary_control = (u64{1} << 0) | (u64{1} << 2) | (u64{1} << 3) | (u64{1} << 4) |
                                  (u64{1} << 12) | (u64{1} << 16) | (u64{1} << 18) | (u64{1} << 11) |
                                  (u64{1} << 20) | (u64{1} << 22) | (u64{1} << 23) | (u64{1} << 28) |
                                  (u64{1} << 29);

struct alignas(page_size) Table
{
    u64 entries[512]{};
};
// Each space owns a root and a directory table and one leaf table per 2 MiB of
// user window it maps, all from table storage.
struct Space
{
    Table *root{};
    Table *directory{};
    usize tables{};
    detail::TlbSpaceTag tlb{};
};
constinit SpaceTable<Space, fixed_space_slots> spaces;
// The slot storage is fixed, so entries look a space up without the kernel
// lock; slot acquire and release, space ids and TLB generations are
// serialized by spaces_lock.
constinit smp::RankedSpinLock<smp::LockRank::arch_spaces> spaces_lock{};
using SpacesGuard = smp::RankedSpinLock<smp::LockRank::arch_spaces>::Guard;
constinit detail::TlbSpaceIds space_ids{};
// The kernel translation is ASID 0 and maps only global entries.
Table kernel_root{};
Table kernel_directory{};
bool initialized = false;
u64 timer_period = 0;
alignas(16) u8 interrupt_stack[64 * 1024]{};

CpuBlock &this_cpu()
{
    uptr block;
    asm("mrs %0, tpidr_el1" : "=r"(block));
    return *reinterpret_cast<CpuBlock *>(block);
}

extern "C" void ok_arm_execution_enter(UserFrame *);
extern "C" void ok_arm_execution_vectors();
extern "C" void ok_platform_console_write_char(char);

static_assert(offsetof(UserFrame, pc) == 256);
static_assert(offsetof(UserFrame, sp) == 264);
static_assert(offsetof(UserFrame, flags) == 272);
static_assert(offsetof(UserFrame, fs_base) == 280);
static_assert(offsetof(UserFrame, gs_base) == 288);
static_assert(offsetof(UserFrame, fp_state) == 304);

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
// This CPU's ID_AA64MMFR0_EL1.ASIDBits: 2 means 16-bit ASIDs, 0 the 8-bit
// minimum.
u32 asid_bits()
{
    u64 features;
    asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(features));
    return ((features >> 4) & 15) == 2 ? 16 : 8;
}
u64 translation_control(u32 bits)
{
    return tcr | (bits == 16 ? tcr_asid16 : 0);
}
// This CPU's user translation for one entry. The ASID's stale entries go
// first, while TTBR0 still selects the kernel's ASID 0, so no walk can refill
// them in between; the switch itself invalidates nothing.
void select_user_root(uptr root, detail::TlbSelection selection)
{
    const u64 asid = u64{selection.tag} << ttbr_asid_shift;
    if (selection.invalidate)
        asm volatile("tlbi aside1, %0; dsb nsh" : : "r"(asid) : "memory");
    asm volatile("dsb ishst; msr ttbr0_el1, %0; isb" : : "r"(root | asid) : "memory");
}
void select_kernel_root()
{
    asm volatile("msr ttbr0_el1, %0; isb" : : "r"(reinterpret_cast<uptr>(&kernel_root)) : "memory");
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
bool admissible(const PageMapping &mapping)
{
    return mapping.virtual_address >= user_begin && mapping.virtual_address < user_end &&
           (mapping.virtual_address & (page_size - 1)) == 0 && mapping.physical_address >= ram_begin &&
           mapping.physical_address < ram_end && (mapping.physical_address & (page_size - 1)) == 0 &&
           (mapping.flags == 0 || (mapping.flags & page_read) != 0) &&
           (mapping.flags & ~(page_read | page_write | page_execute)) == 0 &&
           (mapping.flags & (page_write | page_execute)) != (page_write | page_execute);
}
u64 leaf_entry(const PageMapping &mapping)
{
    u64 entry = mapping.physical_address | table_or_page | normal_memory | accessed | not_global | user_access |
                privileged_xn;
    if ((mapping.flags & page_write) == 0)
        entry |= read_only;
    if ((mapping.flags & page_execute) == 0)
        entry |= user_xn;
    return entry;
}
// The kernel runs with the MMU off, so table storage must be RAM the walker
// reaches at the same address.
Result<Table *> allocate_table()
{
    auto page = allocate_table_pages(1);
    if (!page) return page.status();
    if (page.value() < ram_begin || page.value() >= ram_end)
    {
        static_cast<void>(release_table_pages(page.value(), 1));
        return Status::unsupported("native table storage lies outside supervisor RAM");
    }
    return reinterpret_cast<Table *>(page.value());
}
void release_table(Table *&table, Status &result)
{
    if (!table) return;
    if (auto status = release_table_pages(reinterpret_cast<uptr>(table), 1); !status && result) result = status;
    table = nullptr;
}
// User leaves first (the low directory holds MMIO blocks), then the fixed
// tables; the first refused release is reported.
Status release_tables(Space &value)
{
    Status result{};
    if (value.directory)
        for (usize index = user_begin >> 21; index < 512; ++index)
            if (auto &entry = value.directory->entries[index]; (entry & table_or_page) == table_or_page)
            {
                auto *leaf = reinterpret_cast<Table *>(entry & address_mask);
                release_table(leaf, result);
                entry = 0;
                --value.tables;
            }
    release_table(value.directory, result);
    release_table(value.root, result);
    return result;
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

bool supported() noexcept
{
    return true;
}

TlbTagReport tlb_tag_report(u32 online_mask) noexcept
{
    return summarize_tlb_tags(online_mask, ok::smp::max_cpus, false,
                              [](u32 cpu) -> const auto & { return ok_aarch64_cpu_blocks[cpu].tlb_tags; });
}

OwnershipReport ownership_report() noexcept
{
    OwnershipReport report{};
    for (const auto &block : ok_aarch64_cpu_blocks)
        report.executing = report.executing || block.executing || block.active_frame != nullptr || block.resume_sp != 0;
    spaces.each([&](const Space &value, uptr root) {
        ++report.address_spaces;
        report.table_pages += 2 + value.tables;
        if (!value.root || !value.directory || root != reinterpret_cast<uptr>(value.root))
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
    u64 current_el, control;
    asm volatile("mrs %0, CurrentEL; mrs %1, sctlr_el1" : "=r"(current_el), "=r"(control));
    if (current_el != 4)
        return Status::unsupported("AArch64 native execution requires an EL1 kernel");
    if ((control & 1) != 0)
        return Status::unsupported("AArch64 native setup requires the boot identity environment");
    u64 saved_mask;
    asm volatile("mrs %0, daif; msr daifset, #0xf" : "=r"(saved_mask) : : "memory");
    // Runners without boot.S's kernel path still enter on CPU 0's block.
    auto &boot_cpu = ok_aarch64_cpu_blocks[0];
    asm volatile("msr tpidr_el1, %0" : : "r"(&boot_cpu) : "memory");
    populate_kernel(kernel_root, kernel_directory);
    boot_cpu.stack_top = reinterpret_cast<uptr>(interrupt_stack + sizeof(interrupt_stack));
    const u64 vectors = reinterpret_cast<uptr>(ok_arm_execution_vectors);
    const auto bits = asid_bits();
    asm volatile("msr vbar_el1, %0; msr mair_el1, %1; msr tcr_el1, %2; msr cpacr_el1, %3; isb"
                 :
                 : "r"(vectors), "r"(mair), "r"(translation_control(bits)), "r"(fp_control)
                 : "memory");
    // The one broadcast invalidation: from here on every CPU starts clean and
    // user entries invalidate only their own ASIDs.
    select_root(reinterpret_cast<uptr>(&kernel_root));
    boot_cpu.tlb_tags.configure(bits);
    control |= (u64{1} << 0) | (u64{1} << 2) | (u64{1} << 12); // MMU, data cache, instruction cache.
    asm volatile("ic iallu; dsb sy; isb; msr sctlr_el1, %0; isb" : : "r"(control) : "memory");

    // QEMU virt's GICv2 private physical timer interrupt and the IPI SGI. Other
    // device IRQs stay masked until a driver explicitly establishes its own
    // hardware IRQ path.
    arm::GicV2::reset_cpu_lines();
    arm::GicV2::enable_distributor();
    asm volatile("mrs %0, cntfrq_el0; msr cntp_ctl_el0, xzr; dsb sy" : "=r"(timer_period) : : "memory");
    timer_period /= 100;
    if (timer_period == 0)
        timer_period = 1;
    initialized = true;
    asm volatile("msr daif, %0" : : "r"(saved_mask) : "memory");
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
    auto entry = [&]() -> Result<decltype(spaces)::Entry *> {
        const SpacesGuard guard(spaces_lock);
        auto acquired = spaces.acquire();
        if (acquired) acquired.value()->space.tlb = space_ids.issue();
        return acquired;
    }();
    if (!entry)
        return entry.status();
    auto &value = entry.value()->space;
    auto fail = [&](Status status) {
        static_cast<void>(release_tables(value));
        const SpacesGuard guard(spaces_lock);
        static_cast<void>(spaces.release(value));
        return status;
    };
    for (Table **table : {&value.root, &value.directory})
    {
        auto allocated = allocate_table();
        if (!allocated)
            return fail(allocated.status());
        *table = allocated.value();
    }
    populate_kernel(*value.root, *value.directory);
    for (const auto &mapping : mappings)
    {
        if (mapping.flags == 0) continue; // PROT_NONE owns backing without a PTE.
        auto &directory_entry = value.directory->entries[(mapping.virtual_address >> 21) & 511];
        if ((directory_entry & valid) == 0)
        {
            auto table = allocate_table();
            if (!table)
                return fail(table.status());
            directory_entry = reinterpret_cast<uptr>(table.value()) | table_or_page;
            ++value.tables;
        }
        reinterpret_cast<Table *>(directory_entry & address_mask)->entries[(mapping.virtual_address >> 12) & 511] =
            leaf_entry(mapping);
        if ((mapping.flags & page_execute) != 0)
            clean_code(mapping.physical_address);
    }
    // Table stores must be visible to the walker before the root is selected,
    // and new code to every CPU's instruction fetch.
    asm volatile("dsb ish; ic ialluis; dsb ish; isb" : : : "memory");
    entry.value()->root = reinterpret_cast<uptr>(value.root);
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
    if ((directory_entry & table_or_page) != table_or_page)
        return Status::not_found("native user page has no translation");
    auto &entry = reinterpret_cast<Table *>(directory_entry & address_mask)->entries[(mapping.virtual_address >> 12) & 511];
    if ((entry & valid) == 0)
        return Status::not_found("native user page has no translation");
    if ((mapping.flags & page_execute) != 0)
        clean_code(mapping.physical_address);
    // Every other CPU leaves this root's user windows first. Break before
    // make; no CPU runs this space meanwhile, and one that cached the old
    // entry invalidates its ASID at its next entry, once the generation moved.
    static_cast<void>(smp::require_exclusive("address-space quiesce"));
    static_cast<void>(ok::smp::quiesce_root(space.root));
    entry = 0;
    asm volatile("dsb ishst" : : : "memory");
    entry = leaf_entry(mapping);
    asm volatile("dsb ish; ic ialluis; dsb ish; isb" : : : "memory");
    const SpacesGuard guard(spaces_lock);
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
    static_cast<void>(smp::require_exclusive("address-space quiesce"));
    static_cast<void>(ok::smp::quiesce_root(space.root));
    const auto released = release_tables(*value);
    Status retired;
    {
        const SpacesGuard guard(spaces_lock);
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
Result<u32 *> user_word(const AddressSpace &space, uptr address, bool write)
{
    if (address % sizeof(u32))
        return Status::invalid_argument("user word is misaligned");
    if (!user_range(space, address, sizeof(u32), write))
        return Status::fault("user word is not mapped with the requested access");
    return reinterpret_cast<u32 *>((lookup(space, address) & address_mask) + (address & (page_size - 1)));
}

static Result<Trap> enter_impl(AddressSpace &space, UserFrame &frame, uptr stack_top)
{
    if (!initialized)
        return Status::not_initialized("native address space is unavailable");
    auto &cpu = this_cpu();
    if (cpu.executing)
        return Status::busy("this CPU already executes native code");
    if (!stack_top && !cpu.stack_top)
        return Status::invalid_argument("a secondary CPU enters native code only on an owned kernel stack");
    u64 code;
    detail::TlbSpaceTag tlb;
    {
        const SpacesGuard guard(spaces_lock);
        const auto *value = storage(space);
        if (value == nullptr)
            return Status::not_initialized("native address space is unavailable");
        code = lookup(space, frame.pc);
        tlb = value->tlb;
    }
    // Like Linux, the stack need not be mapped; only an access to it faults (a
    // detached musl thread unmaps its own stack before its exit syscall).
    if (!permits(code, false) || (code & user_xn) != 0 || (frame.pc & 3) != 0 || frame.sp <= user_begin ||
        frame.sp > user_end || (frame.sp & 15) != 0)
        return Status::fault("native user entry or stack pointer is invalid");
    if ((frame.fs_base != 0 && (frame.fs_base < user_begin || frame.fs_base >= user_end)) ||
        (frame.gs_base != 0 && (frame.gs_base < user_begin || frame.gs_base >= user_end)))
        return Status::fault("user TLS base lies outside the native user window");
    frame.flags &= 0xf0000000; // Preserve NZCV only; always EL0t with IRQs enabled.
    u64 saved_mask;
    asm volatile("mrs %0, daif; msr daifset, #0xf" : "=r"(saved_mask) : : "memory");
    const auto old_stack_top = cpu.stack_top;
    if (stack_top) cpu.stack_top = stack_top;
    cpu.executing = true;
    select_user_root(space.root, cpu.tlb_tags.select(tlb));
    asm volatile("msr cntp_tval_el0, %0; msr cntp_ctl_el0, %1; isb" : : "r"(timer_period), "r"(u64{1}) : "memory");
    ok_arm_execution_enter(&frame);
    asm volatile("msr cntp_ctl_el0, xzr; isb" : : : "memory");
    select_kernel_root();
    cpu.executing = false;
    cpu.active_frame = nullptr;
    cpu.resume_sp = 0;
    cpu.stack_top = old_stack_top;
    asm volatile("msr daif, %0" : : "r"(saved_mask) : "memory");
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

extern "C" void ok_arm_execution_trap(u64 vector, uptr capture_sp)
{
    u64 syndrome, fault_address;
    asm volatile("mrs %0, esr_el1; mrs %1, far_el1" : "=r"(syndrome), "=r"(fault_address));
    auto &cpu = this_cpu();
    if (vector == 9)
    {
        using arm::GicV2;
        const auto acknowledge = GicV2::claim();
        const auto irq = acknowledge & 0x3ff;
        asm volatile("msr cntp_ctl_el0, xzr; isb" : : : "memory");
        // The timer and IPIs complete here; any other ID is a device line whose
        // top half masks and completes it once the interrupt service runs.
        // A device line reaches whichever CPU it is routed to; the global
        // tick belongs to CPU 0 and the others' timer is their local tick.
        if (irq < 16)
        {
            // The sender's reasons are read after the claim; the SGI's EOI
            // carries the source CPU bits of the full IAR value.
            asm volatile("dmb ish" : : : "memory");
            GicV2::complete(acknowledge);
            static_cast<void>(ok::smp::receive_ipi());
        }
        else if (irq == timer_irq && cpu.cpu != 0)
        {
            GicV2::complete(acknowledge);
            ok::smp::local_tick();
        }
        else if (irq == timer_irq)
        {
            // CNTP_CVAL keeps the user quantum's expiry.
            u64 compare, count;
            asm volatile("mrs %0, cntp_cval_el0; mrs %1, cntpct_el0" : "=r"(compare), "=r"(count));
            interrupt::record_timer_latency(interrupt::TimerWake::user, count > compare ? count - compare : 0,
                                            arch::clock_frequency());
            GicV2::complete(acknowledge);
            ok_interrupt_tick();
        }
        else if (irq >= 32 && irq < 1020 && interrupt::platform_irqs_started())
            ok_interrupt_top_half(static_cast<interrupt::IrqLine>(irq));
        else if (irq < 1020)
            GicV2::complete(acknowledge);
        cpu.last_trap = {irq == timer_irq ? TrapKind::timer : TrapKind::interrupt, irq, 0, 0, capture_sp};
        return;
    }
    const auto exception_class = (syndrome >> 26) & 0x3f;
    const bool page_fault = exception_class == 0x20 || exception_class == 0x24;
    cpu.last_trap = {vector == 8 && exception_class == 0x15 ? TrapKind::syscall
                 : page_fault                           ? TrapKind::page_fault
                                                        : TrapKind::exception,
                 static_cast<u32>(exception_class), syndrome, page_fault ? fault_address : 0, capture_sp};
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

namespace ok::arch::aarch64
{
// A secondary CPU shares the boot CPU's translation and vectors; its block
// has no unowned entry stack. It starts with 8-bit ASIDs (only the kernel's
// ASID 0 is used before its first user entry) and widens them itself.
Status prepare_secondary(ok::smp::CpuId cpu, SecondaryStart &start)
{
    using namespace execution;
    if (!initialized)
        return Status::not_initialized("native execution backend is not initialized");
    if (cpu == 0 || cpu >= ok::smp::max_cpus)
        return Status::invalid_argument("not a secondary CPU");
    auto &block = ok_aarch64_cpu_blocks[cpu];
    block = {};
    block.cpu = cpu;
    start.cpu_block = reinterpret_cast<uptr>(&block);
    start.mair = mair;
    start.tcr = tcr;
    start.vbar = reinterpret_cast<uptr>(ok_arm_execution_vectors);
    start.cpacr = fp_control;
    start.ttbr0 = reinterpret_cast<uptr>(&kernel_root);
    start.sctlr = secondary_control;
    return Status::success();
}

void prepare_local_tlb_tags()
{
    using namespace execution;
    const auto bits = asid_bits();
    // TLB entries may hold the ASID size they were cached under: drop this
    // CPU's (kernel ASID 0 only) after the change.
    if (bits == 16)
        asm volatile("msr tcr_el1, %0; isb; tlbi vmalle1; dsb nsh; isb"
                     :
                     : "r"(translation_control(bits))
                     : "memory");
    this_cpu().tlb_tags.configure(bits);
}
} // namespace ok::arch::aarch64
