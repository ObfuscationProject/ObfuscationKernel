#include "ok/arch/execution_spaces.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/arch/clock.hpp"
#include <cstddef>

namespace ok::arch::execution
{
namespace
{
constexpr u64 pte_valid = 1, pte_read = 2, pte_write = 4, pte_execute = 8, pte_user = 16, pte_accessed = 64,
              pte_dirty = 128;
struct alignas(page_size) Table
{
    std::array<u64, 512> entries{};
};
// Each space owns a root and a middle table and one leaf table per 2 MiB of
// user window it maps, all from table storage the M-mode kernel reaches
// untranslated.
struct Space
{
    Table *root{};
    Table *middle{};
    MappingView mappings{};
    usize leaves{};
};
constinit SpaceTable<Space> spaces;
bool initialized{false};
bool entering{false};

Space *lookup(const AddressSpace &space)
{
    return spaces.find(space);
}
const PageMapping *mapping(const Space &space, uptr address)
{
    const auto *page = space.mappings.find(address);
    return page && page->flags ? page : nullptr;
}
u64 table_entry(const Table &table)
{
    return (reinterpret_cast<uptr>(&table) >> 2) | pte_valid;
}
Result<Table *> allocate_table()
{
    auto page = allocate_table_pages(1);
    if (!page) return page.status();
    return reinterpret_cast<Table *>(page.value());
}
// Leaf tables first, then the fixed tables; the first refused release is
// reported.
Status release_tables(Space &space)
{
    Status result{};
    auto release = [&](Table *&table) {
        if (!table) return;
        if (auto status = release_table_pages(reinterpret_cast<uptr>(table), 1); !status && result) result = status;
        table = nullptr;
    };
    constexpr usize first = (user_begin >> 21) & 511u, last = ((user_end - 1) >> 21) & 511u;
    if (space.middle)
        for (usize index = first; index <= last; ++index)
            if (auto &entry = space.middle->entries[index]; entry & pte_valid)
            {
                auto *leaf = reinterpret_cast<Table *>((entry >> 10) << 12);
                release(leaf);
                entry = 0;
                --space.leaves;
            }
    release(space.middle);
    release(space.root);
    return result;
}
bool admissible(const PageMapping &page)
{
    return page.virtual_address >= user_begin && page.virtual_address < user_end &&
           (page.virtual_address & (page_size - 1)) == 0 && page.physical_address != 0 &&
           (page.physical_address & (page_size - 1)) == 0 && (page.physical_address >> 56) == 0 &&
           (page.flags == 0 || (page.flags & page_read) != 0) &&
           (page.flags & (page_write | page_execute)) != (page_write | page_execute) &&
           (page.flags & ~(page_read | page_write | page_execute)) == 0;
}
u64 leaf_entry(const PageMapping &page)
{
    u64 flags = pte_valid | pte_user | pte_accessed | pte_dirty;
    if (page.flags & page_read)
        flags |= pte_read;
    if (page.flags & page_write)
        flags |= pte_write;
    if (page.flags & page_execute)
        flags |= pte_execute;
    return static_cast<u64>(page.physical_address >> 2) | flags;
}
extern "C" void ok_rv64_enter_native(UserFrame *frame, uptr satp, Trap *trap, uptr stack_top);
// mie for user mode, read by the entry assembly, which records the user
// quantum's mtimecmp expiry.
extern "C"
{
constinit uptr ok_rv_user_mie = 0x80;
constinit u64 ok_rv_user_deadline = 0;
}
static_assert(offsetof(UserFrame, pc) == 256);
static_assert(offsetof(UserFrame, sp) == 264);
static_assert(offsetof(UserFrame, flags) == 272);
static_assert(offsetof(UserFrame, fp_state) == 304);
static_assert(offsetof(Trap, kernel_stack_pointer) == 24);
static_assert(offsetof(Trap, vector) == 4);
static_assert(offsetof(Trap, error_code) == 8);
static_assert(offsetof(Trap, fault_address) == 16);
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
        report.table_pages += 2 + space.leaves;
        if (!space.root || !space.middle || root != reinterpret_cast<uptr>(space.root) || space.mappings.empty() ||
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
    // The virt kernel remains in M-mode. U-mode uses Sv39 and returns directly
    // to the machine trap vector; no supervisor kernel mapping is exposed.
    uptr hart = 0;
    asm volatile("csrr %0, mhartid" : "=r"(hart));
    if (hart != 0)
        return Status::unsupported("native virt execution currently uses the boot hart");
    const uptr all_memory = ~uptr{0};
    const uptr permission = 0x1f; // PMP NAPOT region, R/W/X; PTEs enforce U access.
    asm volatile("csrw pmpaddr0, %0\ncsrw pmpcfg0, %1" ::"r"(all_memory), "r"(permission) : "memory");
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
            return Status::invalid_argument("invalid Sv39 user mapping");
    auto entry = spaces.acquire();
    if (!entry)
        return entry.status();
    auto &space = entry.value()->space;
    auto fail = [&](Status status) {
        static_cast<void>(release_tables(space));
        static_cast<void>(spaces.release(space));
        return status;
    };
    for (Table **table : {&space.root, &space.middle})
    {
        auto allocated = allocate_table();
        if (!allocated)
            return fail(allocated.status());
        *table = allocated.value();
    }
    space.root->entries[1] = table_entry(*space.middle);
    for (const auto &page : mappings)
    {
        if (!page.flags) continue; // PROT_NONE has no user translation.
        auto &directory_entry = space.middle->entries[(page.virtual_address >> 21) & 511u];
        if (directory_entry == 0)
        {
            auto table = allocate_table();
            if (!table)
                return fail(table.status());
            directory_entry = table_entry(*table.value());
            ++space.leaves;
        }
        auto *leaf = reinterpret_cast<Table *>((directory_entry >> 10) << 12);
        leaf->entries[(page.virtual_address >> 12) & 511u] = leaf_entry(page);
    }
    space.mappings = mappings;
    asm volatile("fence rw,rw\nfence.i" ::: "memory");
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
        return Status::invalid_argument("invalid Sv39 user remap");
    const auto middle = stored->middle->entries[(mapping.virtual_address >> 21) & 511u];
    if ((middle & pte_valid) == 0)
        return Status::not_found("native user page has no translation");
    auto &entry = reinterpret_cast<Table *>((middle >> 10) << 12)->entries[(mapping.virtual_address >> 12) & 511u];
    if ((entry & pte_valid) == 0)
        return Status::not_found("native user page has no translation");
    // Native entry executes sfence.vma before it switches to U-mode.
    entry = leaf_entry(mapping);
    asm volatile("fence rw,rw\nfence.i" ::: "memory");
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
    frame.pc = entry;
    frame.sp = stack;
    frame.registers[2] = stack;
    return frame;
}
// CLINT mtime, the counter mtimecmp compares against.
u64 machine_time()
{
    return *reinterpret_cast<volatile u64 *>(uptr{0x200bff8});
}
// A device interrupt in user mode: claim the PLIC source and run its top half
// once the interrupt service owns the PLIC, otherwise only complete it.
void complete_irq(Trap &trap)
{
    if (trap.kind == TrapKind::timer)
    {
        const u64 now = machine_time();
        interrupt::record_timer_latency(interrupt::TimerWake::user,
                                        now > ok_rv_user_deadline ? now - ok_rv_user_deadline : 0,
                                        arch::clock_frequency());
        ok_interrupt_tick();
    }
    if (trap.kind != TrapKind::interrupt)
        return;
    auto &claim = *reinterpret_cast<volatile u32 *>(uptr{0x0c200004});
    const u32 source = claim;
    trap.vector = source;
    if (source == 0)
        return;
    if (interrupt::platform_irqs_started())
        ok_interrupt_top_half(static_cast<interrupt::IrqLine>(source));
    else
        claim = source;
}
static Result<Trap> enter_impl(AddressSpace &space, UserFrame &frame, uptr stack_top)
{
    auto *stored = lookup(space);
    if (!initialized || stored == nullptr)
        return Status::not_initialized("native address space is unavailable");
    if (entering)
        return Status::busy("nested native entry is unsupported");
    const auto *entry = mapping(*stored, frame.pc);
    // Like Linux, the stack need not be mapped; only an access to it faults (a
    // detached musl thread unmaps its own stack before its exit syscall).
    if (entry == nullptr || !(entry->flags & page_execute) || (frame.pc & 1) != 0 || frame.sp <= user_begin ||
        frame.sp > user_end)
        return Status::invalid_argument("invalid user entry frame");
    uptr saved_status;
    asm volatile("csrrc %0, mstatus, %1" : "=r"(saved_status) : "r"(uptr{8}) : "memory");
    entering = true;
    // User mode takes the machine timer and, once the interrupt service owns the
    // PLIC, machine external interrupts.
    ok_rv_user_mie = interrupt::platform_irqs_started() ? 0x880 : 0x80;
    Trap trap{};
    const uptr satp = (uptr{8} << 60) | (space.root >> 12);
    ok_rv64_enter_native(&frame, satp, &trap, stack_top);
    entering = false;
    complete_irq(trap);
    // The assembly restored the masked status; re-enable only on the service stack.
    asm volatile("csrs mstatus, %0" : : "r"(saved_status & uptr{8}) : "memory");
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
    Syscall call{.number = frame.registers[17]};
    for (usize i = 0; i < call.arguments.size(); ++i)
        call.arguments[i] = frame.registers[10 + i];
    return call;
}
void set_return_value(UserFrame &frame, i64 value)
{
    frame.registers[10] = static_cast<uptr>(value);
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
