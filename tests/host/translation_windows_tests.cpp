// Model of the user-access window protocol (ok/smp/bringup.hpp) with poisoned
// frees: host threads stand in for CPUs, a model backend publishes leaf
// entries and slots exactly as the x86_64, aarch64 and rv64 backends do, and
// every table, frame or slot the writer frees is filled with poison first. A
// reader inside a window (kernel copy or user entry) must never read poison.
// Model coverage only (scope=model): no MMU, TLB, IPI or hardware ordering.

#include "ok/arch/execution_spaces.hpp"
#include "ok/smp/bringup.hpp"
#include "hosted_race.hpp"
#include "../../src/arch/tlb_tags.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace ok;
namespace hw = ok::arch::execution;

#define CHECK(condition)                                                                                   \
    do                                                                                                     \
    {                                                                                                      \
        if (!static_cast<bool>(condition))                                                                 \
        {                                                                                                  \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                            \
            std::abort();                                                                                  \
        }                                                                                                  \
    } while (false)

namespace
{
constexpr u64 poison = 0xdeadbeefdeadbeefull;
constexpr usize words = 64;
constexpr usize leaves = 8;
constexpr unsigned readers = 3;

// A frame of user data: every word holds its tag while it is live.
struct Frame
{
    std::array<std::atomic<u64>, words> word{};
};
// A leaf table: entries name frames.
struct Table
{
    std::array<std::atomic<Frame *>, leaves> entry{};
};

std::mutex table_mutex;
// Table storage for the slot table (execution_spaces.hpp asks the runtime).
std::vector<void *> table_runs;
} // namespace

ok::Result<ok::uptr> ok::arch::execution::allocate_table_pages(usize pages, usize alignment)
{
    const std::lock_guard guard(table_mutex);
    void *run = std::aligned_alloc(alignment < 4096 ? 4096 : alignment, pages * 4096);
    if (!run) return Status::no_memory();
    std::memset(run, 0, pages * 4096);
    table_runs.push_back(run);
    return reinterpret_cast<uptr>(run);
}
ok::Status ok::arch::execution::release_table_pages(uptr address, usize pages)
{
    const std::lock_guard guard(table_mutex);
    std::memset(reinterpret_cast<void *>(address), 0xa5, pages * 4096);
    return {};
}

namespace
{
// Trivially copyable like the backends' spaces; the table pointer is read
// and written as an atomic word.
struct ModelSpace
{
    Table *table{};
    arch::detail::TlbSpaceTag tlb{};
};
Table *table_of(const ModelSpace &space)
{
    return std::atomic_ref<Table *>{const_cast<Table *&>(space.table)}.load(std::memory_order_acquire);
}

hw::SpaceTable<ModelSpace, 8> spaces;
smp::RankedSpinLock<smp::LockRank::arch_spaces> spaces_lock;
arch::detail::TlbSpaceIds ids;

// The handle readers start from, published with a seqcount like
// ProcessResources::native_space.
std::atomic<u32> sequence{};
hw::AddressSpace published{};

void publish(const hw::AddressSpace &space)
{
    const auto start = sequence.load(std::memory_order_relaxed);
    sequence.store(start + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::atomic_ref<uptr>{published.root}.store(space.root, std::memory_order_relaxed);
    std::atomic_ref<u32>{published.slot}.store(space.slot, std::memory_order_relaxed);
    std::atomic_ref<u32>{published.generation}.store(space.generation, std::memory_order_relaxed);
    sequence.store(start + 2, std::memory_order_release);
}
hw::AddressSpace read_published()
{
    for (;;)
    {
        const auto before = sequence.load(std::memory_order_acquire);
        const hw::AddressSpace copy{std::atomic_ref<uptr>{published.root}.load(std::memory_order_relaxed),
                                    std::atomic_ref<u32>{published.slot}.load(std::memory_order_relaxed),
                                    std::atomic_ref<u32>{published.generation}.load(std::memory_order_relaxed)};
        std::atomic_thread_fence(std::memory_order_acquire);
        if ((before & 1) == 0 && sequence.load(std::memory_order_relaxed) == before)
            return copy;
    }
}

Frame *new_frame(u64 tag)
{
    auto *frame = new Frame;
    for (auto &word : frame->word)
        word.store(tag, std::memory_order_relaxed);
    return frame;
}
// A freed frame is poisoned and kept (never reused), so a late reader sees
// poison rather than another owner's data.
void poison_frame(Frame *frame)
{
    for (auto &word : frame->word)
        word.store(poison, std::memory_order_relaxed);
}
void poison_table(Table *table)
{
    for (auto &entry : table->entry)
        if (auto *frame = entry.load(std::memory_order_relaxed))
            poison_frame(frame);
}

hw::AddressSpace create(u64 tag)
{
    auto *table = new Table;
    for (usize i = 0; i < leaves; ++i)
        table->entry[i].store(new_frame(tag), std::memory_order_relaxed);
    decltype(spaces)::Entry *entry;
    {
        const decltype(spaces_lock)::Guard guard(spaces_lock);
        auto acquired = spaces.acquire();
        CHECK(acquired);
        entry = acquired.value();
        entry->space.tlb = ids.issue();
    }
    std::atomic_ref<Table *>{entry->space.table}.store(table, std::memory_order_release);
    {
        const decltype(spaces_lock)::Guard guard(spaces_lock);
        spaces.publish(*entry, reinterpret_cast<uptr>(table));
    }
    return spaces.handle(*entry);
}

// Writer order for one leaf: publish, bump, quiesce, then free (poison).
// `early_free` poisons before the quiesce (the order the protocol forbids);
// `published` runs between the publication and the quiesce.
template <typename Published>
void remap(const hw::AddressSpace &space, usize index, u64 tag, bool early_free, Published published)
{
    auto *value = spaces.find(space);
    CHECK(value);
    auto *table = table_of(*value);
    auto *old = table->entry[index].exchange(new_frame(tag), std::memory_order_release);
    if (early_free)
        poison_frame(old);
    {
        const decltype(spaces_lock)::Guard guard(spaces_lock);
        value->tlb.changed();
    }
    published();
    smp::quiesce_root(space.root);
    poison_frame(old);
}

// Writer order for a teardown: retire, bump, quiesce, then free.
void destroy(const hw::AddressSpace &space)
{
    auto *value = spaces.find(space);
    CHECK(value);
    {
        const decltype(spaces_lock)::Guard guard(spaces_lock);
        CHECK(spaces.retire(*value));
        value->tlb.changed();
    }
    smp::quiesce_root(space.root);
    poison_table(table_of(*value));
    const decltype(spaces_lock)::Guard guard(spaces_lock);
    CHECK(spaces.release(*value));
}

std::array<std::atomic<bool>, smp::max_cpus> kicked{};
std::atomic<u64> poisoned{}, reads{}, rejects{}, user_runs{}, kicks{};

// One walk of the published space inside a window; returns poison hits.
u64 walk(const hw::AddressSpace &space, ok::test::Sequence &random)
{
    auto *value = spaces.find(space);
    if (!value)
    {
        rejects.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    auto *table = table_of(*value);
    auto *frame = table->entry[random.next() % leaves].load(std::memory_order_acquire);
    u64 hits = 0;
    for (const auto &word : frame->word)
        hits += word.load(std::memory_order_relaxed) == poison;
    reads.fetch_add(1, std::memory_order_relaxed);
    return hits;
}

// A kernel copy: kernel window around the walk.
void kernel_reader(unsigned cpu, const std::atomic<bool> &stop)
{
    ok::test::Sequence random{cpu * 104729u};
    while (!stop.load(std::memory_order_acquire))
    {
        const auto space = read_published();
        const smp::KernelWindow window(space.root);
        poisoned.fetch_add(walk(space, random), std::memory_order_relaxed);
    }
}

// A user entry: the user window stays open, walking, until a kick (the
// model's IPI) sends the CPU out of user mode.
void user_reader(unsigned cpu, const std::atomic<bool> &stop)
{
    ok::test::Sequence random{cpu * 7907u};
    while (!stop.load(std::memory_order_acquire))
    {
        const auto space = read_published();
        smp::begin_user(space.root);
        for (unsigned step = 0; step < 64 && !kicked[cpu].load(std::memory_order_acquire); ++step)
            poisoned.fetch_add(walk(space, random), std::memory_order_relaxed);
        kicked[cpu].store(false, std::memory_order_relaxed);
        smp::end_user();
        user_runs.fetch_add(1, std::memory_order_relaxed);
    }
}

void kick(smp::CpuId cpu, u64) noexcept
{
    kicked[cpu].store(true, std::memory_order_release);
    kicks.fetch_add(1, std::memory_order_relaxed);
}

// One reader holds a kernel window across one remap of the frame it is
// reading. With the writer order the free waits for the window, so the
// reader sees only live bytes; freeing before quiesce_root (the control) is
// seen as poison. Returns the poisoned words the reader read.
u64 window_across_remap(bool early_free)
{
    const auto space = create(11);
    std::atomic<int> phase{0};
    u64 hits = 0;
    ok::test::race(2, [&](unsigned cpu) {
        if (cpu == 1)
        {
            const smp::KernelWindow window(space.root);
            auto *frame = table_of(*spaces.find(space))->entry[0].load(std::memory_order_acquire);
            phase.store(1);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            while (phase.load() != 2 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            CHECK(phase.load() == 2);
            for (const auto &word : frame->word)
                hits += word.load(std::memory_order_relaxed) == poison;
            return;
        }
        while (phase.load() == 0)
            std::this_thread::yield();
        remap(space, 0, 12, early_free, [&] { phase.store(2); });
    });
    destroy(space);
    return hits;
}

// Readers race a writer that remaps leaves and replaces whole spaces.
void race(unsigned rounds)
{
    poisoned = reads = rejects = user_runs = kicks = 0;
    std::atomic<bool> stop{false};
    auto space = create(1);
    publish(space);
    ok::test::race(readers + 1, [&](unsigned cpu) {
        if (cpu == readers + 1)
        {
            ok::test::Sequence random{99};
            for (unsigned round = 0; round < rounds; ++round)
            {
                if (round % 16 == 15)
                {
                    const auto replacement = create(round + 2);
                    const auto previous = space;
                    publish(replacement);
                    space = replacement;
                    destroy(previous);
                }
                else
                    remap(space, random.next() % leaves, round + 2, false, [] {});
            }
            stop.store(true, std::memory_order_release);
            return;
        }
        if (cpu == 1)
            user_reader(cpu, stop);
        else
            kernel_reader(cpu, stop);
    });
    destroy(space);
}

// quiesce_root must not return while another CPU's kernel window on the root
// is open, nor wait for one on another root.
void check_kernel_window_wait()
{
    const auto space = create(7);
    std::atomic<int> phase{0};
    std::atomic<bool> returned{false};
    ok::test::race(2, [&](unsigned cpu) {
        if (cpu == 1)
        {
            const smp::KernelWindow window(space.root);
            phase.store(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK(!returned.load());
            phase.store(2);
            return;
        }
        while (phase.load() == 0)
            std::this_thread::yield();
        CHECK(smp::quiesce_root(space.root + 4096) == 0);
        CHECK(smp::quiesce_root(space.root) == (u32{1} << 1));
        CHECK(phase.load() == 2);
        returned.store(true);
    });
    destroy(space);
}

} // namespace

int main()
{
    // CPUs 0 .. readers + 1 are online; kicks go to the model's flags.
    smp::detail::online.store((u32{1} << (readers + 2)) - 1);
    smp::detail::ipi_sender = kick;
    check_kernel_window_wait();
    CHECK(window_across_remap(false) == 0);
    // Negative control: freeing before quiesce_root is caught by the poison.
    const auto control = window_across_remap(true);
    CHECK(control == words);
    race(20000);
    CHECK(poisoned == 0);
    CHECK(reads > 10000 && kicks > 0 && user_runs > 0);
    std::printf("translation windows ordered: reads=%llu rejects=%llu user_runs=%llu kicks=%llu poisoned=0 "
                "control_poisoned=%llu\n",
                static_cast<unsigned long long>(reads.load()), static_cast<unsigned long long>(rejects.load()),
                static_cast<unsigned long long>(user_runs.load()), static_cast<unsigned long long>(kicks.load()),
                static_cast<unsigned long long>(control));
    CHECK(ok::test::lock_violations() == 0 && ok::test::lock_checks() == 0);
    std::puts("OK_TRANSLATION_WINDOWS_MODEL scope=model ordered=pass control=pass window_wait=pass");
    return 0;
}
