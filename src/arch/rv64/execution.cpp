#include "ok/arch/execution_spaces.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/arch/clock.hpp"
#include "ok/smp/bringup.hpp"
#include "ok/smp/kernel_section.hpp"
#include "../riscv_common/clint.hpp"
#include "../riscv_common/plic.hpp"
#include "../tlb_tags.hpp"
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
    detail::TlbSpaceTag tlb{};
};
constinit SpaceTable<Space, fixed_space_slots> spaces;
// The slot storage is fixed, so entries and user copies validate their space
// lock-free inside their window (ok/smp/bringup.hpp); slot acquire, publish,
// retire and release, space ids and TLB generation bumps are serialized by
// spaces_lock.
constinit smp::RankedSpinLock<smp::LockRank::arch_spaces> spaces_lock{};
using SpacesGuard = smp::RankedSpinLock<smp::LockRank::arch_spaces>::Guard;
constinit detail::TlbSpaceIds space_ids{};
constexpr unsigned satp_asid_shift = 44;
bool initialized{false};

Space *lookup(const AddressSpace &space)
{
    return spaces.find(space);
}
// The leaf entry of a user address, read as the hardware walks it; 0 when
// unmapped (PROT_NONE has no entry). User accesses go through the tables, not
// the runtime's records, which it edits in place before a remap.
u64 leaf(const Space &space, uptr address)
{
    if (address < user_begin || address >= user_end)
        return 0;
    const auto middle = std::atomic_ref<u64>{space.middle->entries[(address >> 21) & 511u]}.load(std::memory_order_acquire);
    if ((middle & pte_valid) == 0)
        return 0;
    auto &entry = reinterpret_cast<Table *>((middle >> 10) << 12)->entries[(address >> 12) & 511u];
    return std::atomic_ref<u64>{entry}.load(std::memory_order_acquire);
}
[[nodiscard]] bool permits(u64 entry, u64 access)
{
    return (entry & (pte_valid | pte_user | access)) == (pte_valid | pte_user | access);
}
[[nodiscard]] uptr frame_of(u64 entry)
{
    return static_cast<uptr>((entry >> 10) << 12);
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
// `fence`: 0 keeps the TLB, ~0 flushes it whole on entry and exit (untagged),
// anything else is the ASID whose entries go before U-mode runs.
extern "C" void ok_rv64_enter_native(UserFrame *frame, uptr satp, Trap *trap, uptr stack_top, uptr fence);
// One hart's entry state, reached through tp: boot.S points hart 0 at the
// first block and a started hart gets its block from the start mailbox. The
// logical id comes first (ops.cpp reads it); the entry assembly reads the user
// mie and records the quantum's mtimecmp expiry at the next two offsets.
struct alignas(64) CpuBlock
{
    uptr logical{};
    uptr user_mie{};
    u64 user_deadline{};
    uptr hart{};
    std::atomic<bool> executing{};
    // The satp ASIDs this hart gives user spaces; untagged without ASIDLEN.
    detail::CpuTlbTags tlb_tags{};
};
static_assert(offsetof(CpuBlock, logical) == 0 && offsetof(CpuBlock, user_mie) == 8 &&
              offsetof(CpuBlock, user_deadline) == 16);
CpuBlock &this_cpu()
{
    CpuBlock *block;
    asm("mv %0, tp" : "=r"(block));
    return *block;
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
} // namespace ok::arch::execution

extern "C"
{
constinit std::array<ok::arch::execution::CpuBlock, ok::smp::max_cpus> ok_rv64_cpu_blocks{};
}

namespace ok::arch::execution
{
namespace
{
// PMP entry 0 as one NAPOT region over all memory, R/W/X: U-mode runs only
// under a PMP match, and the PTEs enforce its access. Each hart has its own.
void allow_user_memory()
{
    const uptr all_memory = ~uptr{0};
    const uptr permission = 0x1f;
    asm volatile("csrw pmpaddr0, %0\ncsrw pmpcfg0, %1" ::"r"(all_memory), "r"(permission) : "memory");
}
// ASIDLEN is WARL: write ones to the whole ASID field and count what sticks,
// lowest bits first. The M-mode kernel never translates, so a transient Sv39
// satp with no root is harmless.
void prepare_tlb_tags(CpuBlock &cpu)
{
    uptr saved, probed;
    const uptr probe = (uptr{8} << 60) | (uptr{0xffff} << satp_asid_shift);
    asm volatile("csrr %0, satp\ncsrw satp, %2\ncsrr %1, satp\ncsrw satp, %0\nsfence.vma zero, zero"
                 : "=&r"(saved), "=&r"(probed)
                 : "r"(probe)
                 : "memory");
    u32 bits = 0;
    while (bits < 16 && ((probed >> (satp_asid_shift + bits)) & 1) != 0)
        ++bits;
    cpu.tlb_tags.configure(bits);
}
} // namespace

bool supported() noexcept
{
    return true;
}
TlbTagReport tlb_tag_report(u32 online_mask) noexcept
{
    return summarize_tlb_tags(online_mask, smp::max_cpus, false,
                              [](u32 cpu) -> const auto & { return ok_rv64_cpu_blocks[cpu].tlb_tags; });
}

OwnershipReport ownership_report() noexcept
{
    OwnershipReport report{};
    for (const auto &block : ok_rv64_cpu_blocks)
        report.executing = report.executing || block.executing.load(std::memory_order_acquire);
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
    // Started harts set up their own part (ok_rv64_secondary_execution).
    if (riscv::Clint::hart_id() != 0)
        return Status::unsupported("native virt execution initializes on boot hart 0");
    // boot.S already did; probes that link this backend alone rely on it here.
    asm volatile("mv tp, %0" : : "r"(&ok_rv64_cpu_blocks[0]) : "memory");
    allow_user_memory();
    prepare_tlb_tags(ok_rv64_cpu_blocks[0]);
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
    auto entry = []() -> Result<decltype(spaces)::Entry *> {
        const SpacesGuard guard(spaces_lock);
        auto acquired = spaces.acquire();
        if (acquired) acquired.value()->space.tlb = space_ids.issue();
        return acquired;
    }();
    if (!entry)
        return entry.status();
    auto &space = entry.value()->space;
    auto fail = [&](Status status) {
        static_cast<void>(release_tables(space));
        const SpacesGuard guard(spaces_lock);
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
    {
        const SpacesGuard guard(spaces_lock);
        spaces.publish(*entry.value(), reinterpret_cast<uptr>(space.root));
    }
    return spaces.handle(*entry.value());
}
Status remap(const AddressSpace &space, const PageMapping &mapping)
{
    auto *stored = lookup(space);
    if (stored == nullptr)
        return Status::invalid_argument("stale native address space");
    if (!admissible(mapping) || mapping.flags == 0)
        return Status::invalid_argument("invalid Sv39 user remap");
    const auto middle = stored->middle->entries[(mapping.virtual_address >> 21) & 511u];
    if ((middle & pte_valid) == 0)
        return Status::not_found("native user page has no translation");
    auto &entry = reinterpret_cast<Table *>((middle >> 10) << 12)->entries[(mapping.virtual_address >> 12) & 511u];
    if ((entry & pte_valid) == 0)
        return Status::not_found("native user page has no translation");
    // Writer order (the owning process's mm held): publish, bump, quiesce;
    // the caller frees what the old entry reached only after this returns.
    // Sv39 allows an in-place rewrite. A hart that cached the old entry fences
    // it at its next entry: untagged harts fence every entry, and a tagged one
    // fences the ASID once the generation moved.
    std::atomic_ref<u64>{entry}.store(leaf_entry(mapping), std::memory_order_release);
    asm volatile("fence rw,rw\nfence.i" ::: "memory");
    {
        const SpacesGuard guard(spaces_lock);
        stored->tlb.changed();
    }
    static_cast<void>(smp::quiesce_root(space.root));
    return Status::success();
}
// remap() for a run of records, with one generation bump and one quiesce.
Status protect(const AddressSpace &space, MappingView mappings)
{
    auto *stored = lookup(space);
    if (stored == nullptr)
        return Status::invalid_argument("stale native address space");
    Status result{};
    usize changed = 0;
    for (const auto &mapping : mappings)
    {
        if (mapping.flags == 0)
            continue;
        if (!admissible(mapping))
        {
            result = Status::invalid_argument("invalid Sv39 user protection");
            break;
        }
        const auto middle = stored->middle->entries[(mapping.virtual_address >> 21) & 511u];
        auto *entry = (middle & pte_valid) == 0
                          ? nullptr
                          : &reinterpret_cast<Table *>((middle >> 10) << 12)->entries[(mapping.virtual_address >> 12) & 511u];
        if (entry == nullptr || (*entry & pte_valid) == 0)
        {
            result = Status::not_found("native user page has no translation");
            break;
        }
        std::atomic_ref<u64>{*entry}.store(leaf_entry(mapping), std::memory_order_release);
        ++changed;
    }
    if (changed == 0)
        return result;
    asm volatile("fence rw,rw\nfence.i" ::: "memory");
    {
        const SpacesGuard guard(spaces_lock);
        stored->tlb.changed();
    }
    static_cast<void>(smp::quiesce_root(space.root));
    return result;
}
Status destroy_space(AddressSpace &space)
{
    auto *stored = lookup(space);
    if (stored == nullptr)
        return Status::invalid_argument("stale native address space");
    // Retiring: no new window finds the space; quiesce_root waits for the
    // windows that found it before the tables go back.
    {
        const SpacesGuard guard(spaces_lock);
        static_cast<void>(spaces.retire(*stored));
        stored->tlb.changed();
    }
    static_cast<void>(smp::quiesce_root(space.root));
    const auto released = release_tables(*stored);
    const SpacesGuard guard(spaces_lock);
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
// An interrupt in user mode. The boot hart's timer is the system tick, a
// secondary's only its local tick. An IPI is acknowledged at the CLINT. A
// device line routed to this hart is claimed through its own PLIC context and
// its top half run once the interrupt service owns the PLIC, otherwise only
// completed.
void complete_irq(Trap &trap, const CpuBlock &cpu)
{
    if (trap.kind == TrapKind::timer && cpu.logical != 0)
        smp::local_tick();
    else if (trap.kind == TrapKind::timer)
    {
        const u64 now = riscv::Clint::time();
        interrupt::record_timer_latency(interrupt::TimerWake::user,
                                        now > cpu.user_deadline ? now - cpu.user_deadline : 0,
                                        arch::clock_frequency());
        ok_interrupt_tick();
    }
    if (trap.kind != TrapKind::interrupt)
        return;
    if ((trap.error_code & 0xff) == 3)
    {
        riscv::Clint::clear_software();
        static_cast<void>(smp::receive_ipi());
        return;
    }
    const u32 source = riscv::Plic::claim();
    trap.vector = source;
    if (source == 0)
        return;
    if (interrupt::platform_irqs_started())
        ok_interrupt_top_half(static_cast<interrupt::IrqLine>(source));
    else
        riscv::Plic::complete(source);
}
static Result<Trap> enter_impl(AddressSpace &space, UserFrame &frame, uptr stack_top)
{
    if (!initialized)
        return Status::not_initialized("native address space is unavailable");
    auto &cpu = this_cpu();
    if (cpu.executing.load(std::memory_order_relaxed))
        return Status::busy("nested native entry is unsupported");
    // The caller opened this hart's user window (begin_user), so the space
    // validated here cannot retire before the window closes.
    const auto *stored = lookup(space);
    if (stored == nullptr)
        return Status::not_initialized("native address space is unavailable");
    const auto tlb = stored->tlb.current();
    // Like Linux, the stack need not be mapped; only an access to it faults (a
    // detached musl thread unmaps its own stack before its exit syscall).
    if (!permits(leaf(*stored, frame.pc), pte_execute) || (frame.pc & 1) != 0 || frame.sp <= user_begin ||
        frame.sp > user_end)
        return Status::invalid_argument("invalid user entry frame");
    uptr saved_status;
    asm volatile("csrrc %0, mstatus, %1" : "=r"(saved_status) : "r"(uptr{8}) : "memory");
    cpu.executing.store(true, std::memory_order_release);
    // User mode takes this hart's timer and IPIs and, once the interrupt
    // service owns the PLIC, the device lines routed to this hart.
    cpu.user_mie = riscv::mie_mtie | riscv::mie_msie | (interrupt::platform_irqs_started() ? riscv::mie_meie : 0);
    Trap trap{};
    uptr satp = (uptr{8} << 60) | (space.root >> 12), fence = ~uptr{0};
    if (cpu.tlb_tags.tagged())
    {
        const auto selection = cpu.tlb_tags.select(tlb);
        satp |= uptr{selection.tag} << satp_asid_shift;
        fence = selection.invalidate ? selection.tag : 0;
    }
    ok_rv64_enter_native(&frame, satp, &trap, stack_top, fence);
    cpu.executing.store(false, std::memory_order_release);
    complete_irq(trap, cpu);
    // The assembly restored the masked status; re-enable only on the service stack.
    asm volatile("csrs mstatus, %0" : : "r"(saved_status & uptr{8}) : "memory");
    trap.translation_changed = stored->tlb.current().generation != tlb.generation;
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
// User accesses walk the tables inside a kernel window. A page whose write
// access is withheld (copy-on-write) fails a write; the runtime's uaccess
// layer resolves it outside the window and retries.
bool user_range(const AddressSpace &space, uptr address, usize size, bool write)
{
    const smp::KernelWindow window(space.root);
    const auto *stored = lookup(space);
    if (stored == nullptr)
        return false;
    if (size == 0)
        return true;
    if (address < user_begin || address >= user_end || size > user_end - address)
        return false;
    for (uptr cursor = address; cursor < address + size;)
    {
        if (!permits(leaf(*stored, cursor), write ? pte_write : pte_read))
            return false;
        const auto next = (cursor & ~(page_size - 1)) + page_size;
        cursor = next < address + size ? next : address + size;
    }
    return true;
}
Status copy_from_user(const AddressSpace &space, void *destination, uptr source, usize size)
{
    const smp::KernelWindow window(space.root);
    if ((destination == nullptr && size != 0) || !user_range(space, source, size))
        return Status::fault("invalid user read range");
    const auto *stored = lookup(space);
    auto *output = static_cast<std::byte *>(destination);
    for (usize i = 0; i < size;)
    {
        const auto frame = frame_of(leaf(*stored, source + i));
        const auto offset = (source + i) & (page_size - 1);
        const usize chunk = size - i < page_size - offset ? size - i : page_size - offset;
        const auto *input = reinterpret_cast<const std::byte *>(frame + offset);
        for (usize j = 0; j < chunk; ++j)
            output[i + j] = input[j];
        i += chunk;
    }
    return Status::success();
}
Status copy_to_user(const AddressSpace &space, uptr destination, const void *source, usize size)
{
    const smp::KernelWindow window(space.root);
    if ((source == nullptr && size != 0) || !user_range(space, destination, size, true))
        return Status::fault("invalid user write range");
    const auto *stored = lookup(space);
    const auto *input = static_cast<const std::byte *>(source);
    for (usize i = 0; i < size;)
    {
        const auto frame = frame_of(leaf(*stored, destination + i));
        const auto offset = (destination + i) & (page_size - 1);
        const usize chunk = size - i < page_size - offset ? size - i : page_size - offset;
        auto *output = reinterpret_cast<std::byte *>(frame + offset);
        for (usize j = 0; j < chunk; ++j)
            output[j] = input[i + j];
        i += chunk;
    }
    return Status::success();
}
// The pointer outlives this call's window: the caller holds the owning mm
// or its own window (with_user_word) while it uses the word.
Result<u32 *> user_word(const AddressSpace &space, uptr address, bool write)
{
    const smp::KernelWindow window(space.root);
    if (address % sizeof(u32))
        return Status::invalid_argument("user word is misaligned");
    if (!user_range(space, address, sizeof(u32), write))
        return Status::fault("user word is not mapped with the requested access");
    return reinterpret_cast<u32 *>(frame_of(leaf(*lookup(space), address)) + (address & (page_size - 1)));
}
} // namespace ok::arch::execution

// Secondary-hart start (smp.cpp): the block a started hart's tp points at,
// and that hart's own part of initialize(), which it runs first.
extern "C" ok::uptr ok_rv64_cpu_block(ok::u32 cpu, ok::uptr hart) noexcept
{
    auto &block = ok_rv64_cpu_blocks[cpu];
    block.logical = cpu;
    block.hart = hart;
    return reinterpret_cast<ok::uptr>(&block);
}

extern "C" void ok_rv64_secondary_execution() noexcept
{
    using namespace ok::arch;
    asm volatile("csrw mie, zero" : : : "memory");
    riscv::Clint::set_compare(~ok::u64{0});
    execution::allow_user_memory();
    execution::prepare_tlb_tags(execution::this_cpu());
}
