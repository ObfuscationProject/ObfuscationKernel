#include "ok/arch/execution_spaces.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/arch/arm32/fp_state.hpp"
#include <cstddef>

namespace ok::arch::execution
{
namespace
{
struct alignas(16384) RootTable
{
    std::array<u32, 4096> entries{};
};
struct alignas(1024) LeafTable
{
    std::array<u32, 256> entries{};
};
constexpr usize root_pages = sizeof(RootTable) / page_size;
// Table storage pages hold four coarse tables: one page serves an aligned
// 4 MiB group of first-level entries.
constexpr usize leaves_per_page = page_size / sizeof(LeafTable);
// The kernel runs with the MMU off; tables live in the RAM sections every
// first-level table maps.
constexpr uptr table_begin = uptr{1024} << 20, table_end = uptr{1280} << 20;
struct Space
{
    RootTable *root{};
    MappingView mappings{};
    usize leaf_pages{};
};
constinit SpaceTable<Space> spaces;
bool initialized{false}, entering{false};
volatile u32 &mmio(uptr address)
{
    return *reinterpret_cast<volatile u32 *>(address);
}
constexpr uptr distributor = 0x08000000, cpu_interface = 0x08010000;
struct TimerState
{
    u32 cpsr, control, compare_low, compare_high;
    u32 distributor_control, cpu_control, mask, priority, enabled;
};
TimerState arm_timer()
{
    TimerState old{};
    asm volatile("mrs %0, cpsr\ncpsid if" : "=r"(old.cpsr)::"memory");
    asm volatile("mrc p15, 0, %0, c14, c2, 1" : "=r"(old.control));
    asm volatile("mrrc p15, 2, %0, %1, c14" : "=r"(old.compare_low), "=r"(old.compare_high));
    old.distributor_control = mmio(distributor);
    old.cpu_control = mmio(cpu_interface);
    old.mask = mmio(cpu_interface + 4);
    old.priority = mmio(distributor + 0x41c);
    old.enabled = mmio(distributor + 0x100);
    mmio(distributor + 0x280) = 1u << 30;
    mmio(distributor + 0x41c) = (old.priority & 0xff00ffffu) | 0x00800000u;
    mmio(distributor + 0x100) = 1u << 30;
    mmio(distributor) = old.distributor_control | 1u;
    mmio(cpu_interface + 4) = 0xff;
    mmio(cpu_interface) = old.cpu_control | 1u;
    u32 frequency;
    asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(frequency));
    const u32 ticks = frequency / 100, enable = 1;
    asm volatile("mcr p15, 0, %0, c14, c2, 0\nmcr p15, 0, %1, c14, c2, 1\nisb" ::"r"(ticks), "r"(enable) : "memory");
    return old;
}
// A device line claimed in the user IRQ path: run its top half once the
// interrupt service owns the GIC, otherwise only complete it. 1023 is spurious.
void complete_irq(const Trap &trap)
{
    if (trap.kind == TrapKind::timer)
        ok_interrupt_tick();
    if (trap.kind != TrapKind::interrupt || trap.vector >= 1020)
        return;
    if (interrupt::platform_irqs_started())
        ok_interrupt_top_half(static_cast<interrupt::IrqLine>(trap.vector));
    else
        mmio(cpu_interface + 0x10) = trap.vector;
}
void restore_timer(const TimerState &old)
{
    const u32 disable = 0;
    asm volatile("mcr p15, 0, %0, c14, c2, 1" ::"r"(disable) : "memory");
    mmio(distributor + 0x180) = 1u << 30;
    mmio(distributor + 0x280) = 1u << 30;
    mmio(distributor + 0x41c) = old.priority;
    if (old.enabled & (1u << 30))
        mmio(distributor + 0x100) = 1u << 30;
    mmio(cpu_interface + 4) = old.mask;
    mmio(cpu_interface) = old.cpu_control;
    mmio(distributor) = old.distributor_control;
    asm volatile("mcrr p15, 2, %0, %1, c14\nmcr p15, 0, %2, c14, c2, 1\nisb" ::"r"(old.compare_low),
                 "r"(old.compare_high), "r"(old.control)
                 : "memory");
    asm volatile("msr cpsr_c, %0" ::"r"(old.cpsr) : "memory");
}
Space *lookup(const AddressSpace &space)
{
    return spaces.find(space);
}
const PageMapping *mapping(const Space &space, uptr address)
{
    const auto *page = space.mappings.find(address);
    return page && page->flags ? page : nullptr;
}
Result<uptr> allocate_table(usize pages, usize alignment)
{
    auto run = allocate_table_pages(pages, alignment);
    if (!run) return run.status();
    if (run.value() < table_begin || run.value() + pages * page_size > table_end)
    {
        static_cast<void>(release_table_pages(run.value(), pages));
        return Status::unsupported("native table storage lies outside the RAM sections");
    }
    return run.value();
}
// Leaf pages first, one per populated 4 MiB group, then the first-level table.
Status release_tables(Space &space)
{
    Status result{};
    auto release = [&](uptr address, usize pages) {
        if (auto status = release_table_pages(address, pages); !status && result) result = status;
    };
    if (space.root)
    {
        for (usize group = user_begin >> 20; group < user_end >> 20; group += leaves_per_page)
        {
            uptr page = 0;
            for (usize index = group; index < group + leaves_per_page; ++index)
                if ((space.root->entries[index] & 3u) == 1u)
                {
                    page = space.root->entries[index] & ~uptr{page_size - 1};
                    space.root->entries[index] = 0;
                }
            if (page)
            {
                release(page, 1);
                --space.leaf_pages;
            }
        }
        release(reinterpret_cast<uptr>(space.root), root_pages);
        space.root = nullptr;
    }
    return result;
}
bool admissible(const PageMapping &page)
{
    return page.virtual_address >= user_begin && page.virtual_address < user_end &&
           (page.virtual_address & (page_size - 1)) == 0 && page.physical_address != 0 &&
           (page.physical_address & (page_size - 1)) == 0 && (page.flags == 0 || (page.flags & page_read) != 0) &&
           (page.flags & (page_write | page_execute)) != (page_write | page_execute) &&
           (page.flags & ~(page_read | page_write | page_execute)) == 0;
}
u32 leaf_entry(const PageMapping &page)
{
    u32 flags = 0x72u; // User R/W, normal noncacheable small page.
    if (!(page.flags & page_write))
        flags |= 1u << 9;
    if (!(page.flags & page_execute))
        flags |= 1u;
    return static_cast<u32>(page.physical_address) | flags;
}
extern "C" void ok_arm32_enter_native(UserFrame *frame, uptr table, Trap *trap, uptr stack_top);
static_assert(offsetof(UserFrame, pc) == 128);
static_assert(offsetof(UserFrame, sp) == 132);
static_assert(offsetof(UserFrame, flags) == 136);
static_assert(offsetof(UserFrame, fp_state) == 160);
static_assert(offsetof(UserFrame, fp_state) + Arm32FpState::fpexc_offset == 420);
static_assert(offsetof(Trap, kernel_stack_pointer) == 16);
static_assert(offsetof(Trap, vector) == 4);
static_assert(offsetof(Trap, error_code) == 8);
static_assert(offsetof(Trap, fault_address) == 12);
} // namespace

bool supported() noexcept
{
    return true;
}
OwnershipReport ownership_report() noexcept
{
    OwnershipReport report{};
    report.executing = entering;
    spaces.each([&](const Space &space, uptr root) {
        ++report.address_spaces;
        report.table_pages += root_pages + space.leaf_pages;
        if (!space.root || root != reinterpret_cast<uptr>(space.root) || space.mappings.empty() ||
            !space.mappings.valid())
            report.failure = Status::io_error("native backend has an inconsistent mapping owner");
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
    uptr mode, sctlr, mpidr;
    asm volatile("mrs %0, cpsr\nmrc p15, 0, %1, c1, c0, 0\nmrc p15, 0, %2, c0, c0, 5"
                 : "=r"(mode), "=r"(sctlr), "=r"(mpidr));
    if ((mode & 31) != 0x13 || (mpidr & 0xff) != 0 || (sctlr & 5))
        return Status::unsupported("native virt ARM32 requires boot CPU in SVC with MMU/data cache disabled");
    initialized = true;
    return Status::success();
}
Result<AddressSpace> create_space(MappingView mappings)
{
    if (!initialized)
        return Status::not_initialized("native execution is not initialized");
    if (!mappings.valid() || mappings.empty())
        return Status::invalid_argument("invalid user mapping directory");
    for (const auto &page : mappings)
        if (!admissible(page))
            return Status::invalid_argument("invalid ARM user mapping");
    auto entry = spaces.acquire();
    if (!entry)
        return entry.status();
    auto &space = entry.value()->space;
    auto fail = [&](Status status) {
        static_cast<void>(release_tables(space));
        static_cast<void>(spaces.release(space));
        return status;
    };
    auto root = allocate_table(root_pages, sizeof(RootTable));
    if (!root)
        return fail(root.status());
    space.root = reinterpret_cast<RootTable *>(root.value());
    // Identity-map platform MMIO and kernel RAM as privileged-only sections.
    for (usize i = 0; i < 256; ++i)
        space.root->entries[i] = static_cast<u32>(i << 20) | 0x416u;
    for (usize i = 1024; i < 1280; ++i)
        space.root->entries[i] = static_cast<u32>(i << 20) | 0x1402u;
    for (const auto &page : mappings)
    {
        if (!page.flags) continue; // PROT_NONE has no user translation.
        const usize section = page.virtual_address >> 20;
        auto &root_entry = space.root->entries[section];
        if (root_entry == 0)
        {
            const usize group = section & ~(leaves_per_page - 1);
            uptr leaf_page = 0;
            for (usize index = group; index < group + leaves_per_page; ++index)
                if ((space.root->entries[index] & 3u) == 1u)
                    leaf_page = space.root->entries[index] & ~uptr{page_size - 1};
            if (!leaf_page)
            {
                auto allocated = allocate_table(1, page_size);
                if (!allocated)
                    return fail(allocated.status());
                leaf_page = allocated.value();
                ++space.leaf_pages;
            }
            root_entry = static_cast<u32>(leaf_page + (section - group) * sizeof(LeafTable)) | 1u;
        }
        if ((root_entry & 3u) != 1u)
            return fail(Status::invalid_argument("user mapping overlaps privileged platform section"));
        auto *leaf = reinterpret_cast<LeafTable *>(root_entry & ~uptr{1023});
        leaf->entries[(page.virtual_address >> 12) & 255u] = leaf_entry(page);
    }
    space.mappings = mappings;
    asm volatile("dsb sy\nisb" ::: "memory");
    entry.value()->root = reinterpret_cast<uptr>(space.root);
    return spaces.handle(*entry.value());
}
Status remap(const AddressSpace &space, const PageMapping &mapping)
{
    auto *stored = lookup(space);
    if (stored == nullptr)
        return Status::invalid_argument("stale native address space");
    if (entering)
        return Status::busy("cannot remap an executing address space");
    if (!admissible(mapping) || mapping.flags == 0)
        return Status::invalid_argument("invalid ARM user remap");
    const auto root_entry = stored->root->entries[mapping.virtual_address >> 20];
    if ((root_entry & 3u) != 1u)
        return Status::not_found("native user page has no translation");
    auto &entry = reinterpret_cast<LeafTable *>(root_entry & ~uptr{1023})->entries[(mapping.virtual_address >> 12) & 255u];
    if (entry == 0)
        return Status::not_found("native user page has no translation");
    // Native entry invalidates the whole TLB before it drops to user mode.
    entry = leaf_entry(mapping);
    asm volatile("dsb sy\nisb" ::: "memory");
    return Status::success();
}
Status destroy_space(AddressSpace &space)
{
    if (entering)
        return Status::busy("cannot destroy an executing address space");
    auto *stored = lookup(space);
    if (stored == nullptr)
        return Status::invalid_argument("stale native address space");
    const auto released = release_tables(*stored);
    const auto retired = spaces.release(*stored);
    space = {};
    return !released ? released : retired;
}
UserFrame initial_frame(uptr entry, uptr stack)
{
    UserFrame frame{};
    frame.pc = entry & ~uptr{1};
    frame.flags = (entry & 1) ? 0x30 : 0x10;
    frame.sp = stack;
    frame.registers[13] = stack;
    Arm32FpState::set_fpexc(frame, Arm32FpState::enabled);
    return frame;
}
static Result<Trap> enter_impl(AddressSpace &space, UserFrame &frame, uptr stack_top)
{
    auto *stored = lookup(space);
    if (!initialized || stored == nullptr)
        return Status::not_initialized("native address space is unavailable");
    if (entering)
        return Status::busy("nested native entry is unsupported");
    if (!Arm32FpState::admitted(frame))
        return Status::unsupported("ARM32 pending VFP exception state is not supported");
    const auto *entry = mapping(*stored, frame.pc);
    // Like Linux, the stack need not be mapped; only an access to it faults (a
    // detached musl thread unmaps its own stack before its exit syscall).
    if (entry == nullptr || !(entry->flags & page_execute) || (frame.pc & ((frame.flags & 0x20) ? 1u : 3u)) != 0 ||
        frame.sp <= user_begin || frame.sp > user_end)
        return Status::invalid_argument("invalid user entry frame");
    uptr saved_mask;
    asm volatile("mrs %0, cpsr; cpsid if" : "=r"(saved_mask) : : "memory");
    entering = true;
    Trap trap{};
    frame.registers[13] = frame.sp;
    const auto timer = arm_timer();
    ok_arm32_enter_native(&frame, space.root, &trap, stack_top);
    restore_timer(timer);
    complete_irq(trap);
    entering = false;
    asm volatile("msr cpsr_c, %0" : : "r"(saved_mask) : "memory", "cc");
    return trap;
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

Syscall syscall(const UserFrame &frame)
{
    Syscall call{.number = frame.registers[7]};
    for (usize i = 0; i < call.arguments.size(); ++i)
        call.arguments[i] = frame.registers[i];
    return call;
}
void set_return_value(UserFrame &frame, i64 value)
{
    frame.registers[0] = static_cast<uptr>(value);
}
bool user_range(const AddressSpace &space, uptr address, usize size, bool write)
{
    const auto *stored = lookup(space);
    if (stored == nullptr)
        return false;
    if (size == 0)
        return true;
    if (address < user_begin || address >= user_end || size > user_end - address)
        return false;
    for (uptr cursor = address; cursor < address + size;)
    {
        const auto *page = mapping(*stored, cursor);
        if (page == nullptr || (!(page->flags & (write ? page_write : page_read)) &&
                                !(write && resolve_write(space, cursor & ~(page_size - 1)) &&
                                  (page->flags & page_write))))
            return false;
        const auto next = (cursor & ~(page_size - 1)) + page_size;
        cursor = next < address + size ? next : address + size;
    }
    return true;
}
Status copy_from_user(const AddressSpace &space, void *destination, uptr source, usize size)
{
    if ((destination == nullptr && size != 0) || !user_range(space, source, size))
        return Status::fault("invalid user read range");
    const auto *stored = lookup(space);
    auto *output = static_cast<std::byte *>(destination);
    for (usize i = 0; i < size;)
    {
        const auto *page = mapping(*stored, source + i);
        const auto offset = (source + i) & (page_size - 1);
        const usize chunk = size - i < page_size - offset ? size - i : page_size - offset;
        const auto *input = reinterpret_cast<const std::byte *>(page->physical_address + offset);
        for (usize j = 0; j < chunk; ++j)
            output[i + j] = input[j];
        i += chunk;
    }
    return Status::success();
}
Status copy_to_user(const AddressSpace &space, uptr destination, const void *source, usize size)
{
    if ((source == nullptr && size != 0) || !user_range(space, destination, size, true))
        return Status::fault("invalid user write range");
    const auto *stored = lookup(space);
    const auto *input = static_cast<const std::byte *>(source);
    for (usize i = 0; i < size;)
    {
        const auto *page = mapping(*stored, destination + i);
        const auto offset = (destination + i) & (page_size - 1);
        const usize chunk = size - i < page_size - offset ? size - i : page_size - offset;
        auto *output = reinterpret_cast<std::byte *>(page->physical_address + offset);
        for (usize j = 0; j < chunk; ++j)
            output[j] = input[i + j];
        i += chunk;
    }
    return Status::success();
}
} // namespace ok::arch::execution
