// Model of the per-CPU user TLB tags (src/arch/tlb_tags.hpp): tag selection,
// reuse and rollover, and the generation protocol against a simulated tagged
// TLB. This checks the bookkeeping only; no hardware TLB is involved.
#include "../../src/arch/tlb_tags.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

using ok::u32;
using ok::u64;
using ok::usize;
using ok::arch::detail::TlbSelection;
using ok::arch::detail::TlbSpaceIds;
using ok::arch::detail::TlbSpaceTag;
using ok::arch::detail::TlbTagCache;

namespace
{

constexpr bool same(TlbSelection selection, u32 tag, bool invalidate)
{
    return selection.tag == tag && selection.invalidate == invalidate;
}

void check_configuration()
{
    TlbTagCache<4> cache;
    assert(!cache.tagged() && cache.bits() == 0);
    cache.configure(0);
    assert(!cache.tagged() && cache.limit() == 0);
    cache.configure(1); // Tag 0 is the kernel's: one user tag remains.
    assert(cache.tagged() && cache.limit() == 1 && cache.bits() == 1);
    cache.configure(2);
    assert(cache.limit() == 3);
    cache.configure(12);
    assert(cache.limit() == 4 && cache.bits() == 12);
    cache.configure(40); // Wider than any hardware field: clamped.
    assert(cache.bits() == 16 && cache.limit() == 4);
}

void check_selection()
{
    TlbSpaceIds ids;
    auto a = ids.issue(), b = ids.issue(), c = ids.issue(), d = ids.issue(), e = ids.issue();
    assert(a.id == 1 && b.id == 2 && e.id == 5 && a.generation == 0);

    TlbTagCache<4> cache;
    cache.configure(8);
    // A new space gets the next tag and invalidates it; a hit at the same
    // generation keeps what this CPU cached.
    assert(same(cache.select(a), 1, true));
    assert(same(cache.select(a), 1, false));
    // A translation change invalidates once.
    a.changed();
    assert(same(cache.select(a), 1, true));
    assert(same(cache.select(a), 1, false));
    assert(same(cache.select(b), 2, true));
    assert(same(cache.select(c), 3, true));
    assert(same(cache.select(d), 4, true));
    assert(same(cache.select(b), 2, false));
    assert(cache.rollovers() == 0);

    // Exhaustion: a rollover hands tags out again from the first, each one
    // invalidated as it is reassigned.
    assert(same(cache.select(e), 1, true));
    assert(cache.rollovers() == 1);
    // `a` lost tag 1 to `e`; it comes back under a new tag, invalidated.
    assert(same(cache.select(a), 2, true));
    assert(same(cache.select(e), 1, false));
    assert(same(cache.select(a), 2, false));
    // `b` and `c` held tags of the previous round; nothing of it is reused
    // without an invalidation.
    assert(same(cache.select(b), 3, true));
    assert(same(cache.select(c), 4, true));
    assert(same(cache.select(d), 1, true));
    assert(cache.rollovers() == 2);

    // One usable tag: every switch of space invalidates it.
    TlbTagCache<4> narrow;
    narrow.configure(1);
    assert(same(narrow.select(a), 1, true));
    assert(same(narrow.select(a), 1, false));
    assert(same(narrow.select(b), 1, true));
    assert(same(narrow.select(a), 1, true));
}

// The OK_SMP_TLB summary: the narrowest width among the named CPUs, PCIDs
// only when every one of them is tagged, rollovers and invalidations summed.
void check_report()
{
    std::array<TlbTagCache<2>, 4> caches;
    caches[0].configure(16);
    caches[1].configure(8);
    caches[2].configure(0);
    caches[3].configure(12);
    auto a = TlbSpaceIds{}.issue();
    auto b = a, c = a;
    b.id = 2;
    c.id = 3;
    for (const auto &space : {a, b, c})
        static_cast<void>(caches[0].select(space));
    auto cache_of = [&](u32 cpu) -> const TlbTagCache<2> & { return caches[cpu]; };
    using ok::arch::execution::summarize_tlb_tags;
    auto report = summarize_tlb_tags(0b0011, caches.size(), false, cache_of);
    // Three new spaces on two tags: each one invalidated the tag it took.
    assert(report.supported && report.asid_bits == 8 && !report.pcid && report.rollovers == 1 &&
           report.invalidations == 3);
    report = summarize_tlb_tags(0b0101, caches.size(), false, cache_of);
    assert(report.asid_bits == 0 && !report.pcid);
    report = summarize_tlb_tags(0b1000, caches.size(), true, cache_of);
    assert(report.asid_bits == 12 && report.pcid && report.rollovers == 0 && report.invalidations == 0);
    report = summarize_tlb_tags(0b1100, caches.size(), true, cache_of);
    assert(report.asid_bits == 0 && !report.pcid);
    // CPUs beyond the backend's count are never read.
    report = summarize_tlb_tags(0b10001, caches.size(), false, cache_of);
    assert(report.asid_bits == 16);
}

// A tagged TLB: entries survive switches of translation and are dropped only
// by an invalidation of their tag (or all of them, untagged). Every hit is
// compared with the page table it was filled from.
struct ModelCpu
{
    TlbTagCache<4> tags;
    std::map<std::pair<u32, u32>, u32> tlb;
    TlbSelection window{};
    bool open{};
};

struct ModelSpace
{
    TlbSpaceTag tag;
    std::vector<u32> pages;
    bool live{};
};

class Model
{
  public:
    Model(u32 cpus, u32 bits, u32 pages) : cpus_(cpus), pages_(pages)
    {
        for (auto &cpu : cpus_)
            cpu.tags.configure(bits);
    }

    usize create()
    {
        ModelSpace space{ids_.issue(), std::vector<u32>(pages_), true};
        for (auto &frame : space.pages)
            frame = next_frame_++;
        spaces_.push_back(space);
        return spaces_.size() - 1;
    }
    // Open a user window: select the tag and drop what must go.
    void open(usize cpu_index, usize space_index)
    {
        auto &cpu = cpus_[cpu_index];
        auto &space = spaces_[space_index];
        assert(space.live && !cpu.open);
        if (!cpu.tags.tagged())
        {
            cpu.tlb.clear();
            cpu.window = {0, true};
        }
        else
        {
            cpu.window = cpu.tags.select(space.tag);
            assert(cpu.window.tag >= 1 && cpu.window.tag <= cpu.tags.limit());
            if (cpu.window.invalidate)
                std::erase_if(cpu.tlb, [&](const auto &entry) { return entry.first.first == cpu.window.tag; });
        }
        cpu.open = true;
        current_[cpu_index] = space_index;
    }
    // User code touches a page inside the open window.
    void touch(usize cpu_index, u32 page)
    {
        auto &cpu = cpus_[cpu_index];
        assert(cpu.open);
        const auto &space = spaces_[current_[cpu_index]];
        const auto key = std::pair{cpu.window.tag, page};
        if (auto found = cpu.tlb.find(key); found != cpu.tlb.end())
        {
            ++hits_;
            if (found->second != space.pages[page])
                ++stale_;
        }
        else
            cpu.tlb.emplace(key, space.pages[page]);
    }
    void close(usize cpu_index)
    {
        cpus_[cpu_index].open = false;
    }
    // Rewrite one translation. Every window on the space is closed by now
    // (quiesce_root); the generation moved after the rewrite.
    void remap(usize space_index, u32 page, bool bump = true)
    {
        for (usize cpu = 0; cpu < cpus_.size(); ++cpu)
            assert(!cpus_[cpu].open || current_[cpu] != space_index);
        spaces_[space_index].pages[page] = next_frame_++;
        if (bump)
            spaces_[space_index].tag.changed();
    }
    // The writer order of the window protocol: publish the translation and
    // bump the generation while windows on the space may still be open; the
    // caller then closes them (quiesce_root) before the old frame is freed.
    void remap_published(usize space_index, u32 page)
    {
        spaces_[space_index].pages[page] = next_frame_++;
        spaces_[space_index].tag.changed();
    }
    // A frame change in two steps: the leaf is cleared (frame 0, the page
    // faults) and the generation moves; after quiesce_root the new frame is
    // installed and, with `bump`, the generation moves again.
    void clear(usize space_index, u32 page)
    {
        spaces_[space_index].pages[page] = 0;
        spaces_[space_index].tag.changed();
    }
    void install(usize space_index, u32 page, bool bump)
    {
        spaces_[space_index].pages[page] = next_frame_++;
        if (bump)
            spaces_[space_index].tag.changed();
    }
    // A touch between the publication and the end of quiesce_root: a hit on
    // the old frame is in flight (that frame is not freed yet), not stale.
    void touch_in_flight(usize cpu_index, u32 page)
    {
        const auto before = stale_;
        touch(cpu_index, page);
        in_flight_ += stale_ - before;
        stale_ = before;
    }
    // The race quiesce_root resolves: `cpu` opened its window on the space
    // before the remap started and runs until the remap's quiesce closes it.
    // With `early_bump` the generation moves before that window, which is
    // the order the protocol forbids.
    void remap_racing(usize cpu, usize space_index, u32 page, bool early_bump)
    {
        if (early_bump)
            spaces_[space_index].tag.changed();
        open(cpu, space_index);
        touch(cpu, page);
        close(cpu);
        spaces_[space_index].pages[page] = next_frame_++;
        if (!early_bump)
            spaces_[space_index].tag.changed();
    }
    void destroy(usize space_index)
    {
        for (usize cpu = 0; cpu < cpus_.size(); ++cpu)
            assert(!cpus_[cpu].open || current_[cpu] != space_index);
        // Frames go back and may be handed out again; leftover entries of
        // this space must never be hit afterwards.
        spaces_[space_index].live = false;
        for (auto &frame : spaces_[space_index].pages)
            frame = ~u32{0};
    }

    [[nodiscard]] u64 stale() const { return stale_; }
    [[nodiscard]] u64 in_flight() const { return in_flight_; }
    [[nodiscard]] u64 hits() const { return hits_; }
    [[nodiscard]] u64 rollovers() const
    {
        u64 total = 0;
        for (const auto &cpu : cpus_)
            total += cpu.tags.rollovers();
        return total;
    }
    [[nodiscard]] bool live(usize space) const { return spaces_[space].live; }
    [[nodiscard]] usize spaces() const { return spaces_.size(); }

  private:
    std::vector<ModelCpu> cpus_;
    std::map<usize, usize> current_;
    std::vector<ModelSpace> spaces_;
    TlbSpaceIds ids_;
    u32 pages_;
    u32 next_frame_{1};
    u64 stale_{}, hits_{}, in_flight_{};
};

void check_window_race()
{
    // Generation after the rewrite: the CPU that ran during the remap saw the
    // old generation and invalidates at its next entry.
    Model ordered(2, 8, 4);
    auto space = ordered.create();
    ordered.open(1, space);
    ordered.touch(1, 0);
    ordered.close(1);
    ordered.remap_racing(1, space, 0, false);
    ordered.open(1, space);
    ordered.touch(1, 0);
    ordered.close(1);
    assert(ordered.stale() == 0);

    // Generation before the rewrite: that CPU caches the old translation under
    // the new generation and later hits it. The model sees the error.
    Model early(2, 8, 4);
    space = early.create();
    early.remap_racing(1, space, 0, true);
    early.open(1, space);
    early.touch(1, 0);
    early.close(1);
    assert(early.stale() == 1);

    // No generation change at all: a CPU outside the window when the remap
    // ran keeps the old translation.
    Model missing(2, 8, 4);
    space = missing.create();
    missing.open(0, space);
    missing.touch(0, 2);
    missing.close(0);
    missing.remap(space, 2, false);
    missing.open(0, space);
    missing.touch(0, 2);
    missing.close(0);
    assert(missing.stale() == 1);
}

// The writer order (publish, bump, fence, quiesce_root, free): a CPU whose
// window was open across the publication may hit what it cached before, while
// the old frame is still allocated; once quiesce_root closed that window no
// entry on any CPU hits the old translation, tagged or not.
void check_publish_then_quiesce(u32 bits)
{
    Model model(3, bits, 4);
    const auto space = model.create();
    for (usize cpu = 0; cpu < 3; ++cpu)
    {
        model.open(cpu, space);
        model.touch(cpu, 1);
        model.close(cpu);
    }
    model.open(1, space);
    model.remap_published(space, 1);
    model.touch_in_flight(1, 1);
    model.close(1); // quiesce_root returns once this window closed
    for (usize cpu = 0; cpu < 3; ++cpu)
    {
        model.open(cpu, space);
        model.touch(cpu, 1);
        model.close(cpu);
    }
    assert(model.stale() == 0);
    assert(bits == 0 || model.in_flight() == 1);
}

// The writer order for a new frame (clear, bump, quiesce_root, install,
// bump): a CPU whose window was open across the clear may hit the old frame
// it cached (still allocated, and no new frame exists yet); a CPU that entered
// during the gap may cache the cleared entry (RISC-V may cache an invalid
// one). After the install every entry reaches the new frame, which needs the
// second bump; without it (`second_bump` false) the tagged CPU that cached
// the cleared entry keeps faulting on it.
u64 check_clear_then_install(u32 bits, bool second_bump)
{
    Model model(3, bits, 4);
    const auto space = model.create();
    for (usize cpu = 0; cpu < 3; ++cpu)
    {
        model.open(cpu, space);
        model.touch(cpu, 1);
        model.close(cpu);
    }
    model.open(1, space);
    model.clear(space, 1);
    model.touch_in_flight(1, 1);
    model.open(2, space);
    model.touch(2, 1);
    model.close(2);
    model.close(1); // quiesce_root returns once every window closed
    model.install(space, 1, second_bump);
    for (usize cpu = 0; cpu < 3; ++cpu)
    {
        model.open(cpu, space);
        model.touch(cpu, 1);
        model.close(cpu);
    }
    assert(bits == 0 || model.in_flight() == 1);
    return model.stale();
}

// Random entries, remaps, retirements and creations on several CPUs with few
// tags, so tags roll over and get reused constantly. No hit may ever return
// a translation that is no longer in the page table.
void check_random(u32 bits, u64 seed)
{
    constexpr u32 cpus = 4, pages = 6;
    Model model(cpus, bits, pages);
    std::vector<usize> live;
    for (int i = 0; i < 5; ++i)
        live.push_back(model.create());
    auto next = [&] {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(seed >> 33);
    };
    for (int step = 0; step < 50000; ++step)
    {
        const auto action = next() % 10;
        const auto space = live[next() % live.size()];
        if (action < 6)
        {
            const auto cpu = next() % cpus;
            model.open(cpu, space);
            for (u32 touches = next() % 4 + 1; touches != 0; --touches)
                model.touch(cpu, next() % pages);
            model.close(cpu);
        }
        else if (action < 8)
            model.remap(space, next() % pages);
        else
        {
            model.destroy(space);
            std::erase(live, space);
            live.push_back(model.create());
        }
    }
    assert(model.stale() == 0);
    assert(model.hits() > 10000);
    if (bits != 0)
        assert(model.rollovers() > 100);
    std::printf("tlb_tags random bits=%u spaces=%zu hits=%llu rollovers=%llu stale=0\n", bits, model.spaces(),
                static_cast<unsigned long long>(model.hits()), static_cast<unsigned long long>(model.rollovers()));
}

} // namespace

int main()
{
    check_configuration();
    check_selection();
    check_report();
    check_window_race();
    check_publish_then_quiesce(8);
    check_publish_then_quiesce(0);
    assert(check_clear_then_install(8, true) == 0);
    assert(check_clear_then_install(0, true) == 0);
    assert(check_clear_then_install(8, false) == 1);
    check_random(2, 1);  // three user tags per CPU
    check_random(1, 7);  // one user tag per CPU
    check_random(16, 3); // the cache's own four slots bound it
    check_random(0, 5);  // untagged: every entry flushes
    std::puts("OK_TLB_TAGS_MODEL scope=model selection=pass rollover=pass generation=pass race=pass publish_order=pass frame_change=pass report=pass");
    return 0;
}
