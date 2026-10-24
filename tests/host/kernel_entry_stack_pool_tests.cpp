#include "ok/memory/kernel_entry_stack_pool.hpp"

#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace ok::memory
{
struct KernelEntryStackPoolTestAccess
{
    static void last_generation(KernelEntryStackPool &pool) { pool.generation_ = ~u64{0} - 1; }
    static void last_epoch(KernelEntryStackPool &pool) { pool.epoch_ = ~u64{0} - 1; }
};
} // namespace ok::memory

namespace
{
using namespace ok;
using namespace ok::memory;
using Pool = KernelEntryStackPool;
using Owner = KernelEntryStackOwner;
using Handle = KernelEntryStackHandle;
using View = arch::execution::KernelEntryStackView;
static_assert(!std::is_copy_constructible_v<Pool> && !std::is_move_constructible_v<Pool>);
static_assert(std::is_trivially_copyable_v<Handle> && std::is_trivially_copyable_v<View>);
static_assert(Pool::capacity == 128 && Pool::usable_bytes == 65536 && Pool::slot_stride == 73728);
static_assert(Pool::arena_bytes == 9 * 1024 * 1024);
alignas(Pool::page_size) std::array<std::byte, Pool::arena_bytes> backing{};
alignas(Pool::page_size) std::array<std::byte, Pool::arena_bytes> other_backing{};
usize checks{};

void check(bool condition, const char *message)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL kernel-entry-stack-pool checks=%zu: %s\n", checks, message);
        std::exit(1);
    }
}
void status(Status result, StatusCode expected, const char *message)
{
    check(result.code() == expected, message);
}
void zero(std::span<const std::byte> bytes, const char *message)
{
    for (auto byte : bytes)
        if (byte != std::byte{})
        {
            check(false, message);
            return;
        }
    check(true, message);
}

void initialization_and_owner_checks()
{
    Pool pool, other;
    View empty;
    check(!empty.valid() && !empty.lower() && !empty.top() && !empty.owner_pid() && !empty.owner_tid(),
          "default view is inert");
    status(pool.reserve({1, 1}).status(), StatusCode::not_initialized, "uninitialized reserve denied");
    status(pool.activate({}), StatusCode::invalid_argument, "null arena rejected");
    status(pool.activate(std::span(backing).first(backing.size() - 1)), StatusCode::invalid_argument, "short arena rejected");
    status(pool.activate({backing.data() + 1, backing.size()}), StatusCode::invalid_argument, "misaligned arena rejected");
    const uptr wrapping = ~uptr{0} - Pool::page_size + 1;
    status(pool.activate({reinterpret_cast<std::byte *>(wrapping), Pool::arena_bytes}), StatusCode::invalid_argument,
           "wrapping arena rejected before touching it");
    check(pool.accounting().backing_bytes == 0, "failed initialization does not publish backing");
    check(pool.activate(backing).ok() && other.activate(other_backing).ok(), "two separately backed pools initialize");
    status(pool.activate(backing), StatusCode::already_exists, "reinitialization cannot revive stale views");
    status(pool.reserve({0, 1}).status(), StatusCode::invalid_argument, "PID zero rejected");
    status(pool.reserve({1, 0}).status(), StatusCode::invalid_argument, "TID zero rejected");
    auto first = pool.reserve({1, 2});
    check(first.ok(), "reserve first owner");
    const Handle handle = first.value();
    const auto top = pool.top(handle, {1, 2});
    check(top.ok() && top.value() == reinterpret_cast<uptr>(backing.data()) + Pool::canary_bytes + Pool::usable_bytes,
          "inactive top query uses canonical bounds");
    check(pool.validate_inactive(handle, {1, 2}).ok(), "healthy inactive preflight succeeds");
    status(pool.reserve({1, 2}).status(), StatusCode::already_exists, "duplicate thread lease rejected");
    status(other.release(handle, {1, 2}), StatusCode::invalid_argument, "foreign pool handle rejected");
    status(pool.release(handle, {2, 2}), StatusCode::denied, "wrong PID cannot release lease");
    status(pool.release(handle, {1, 3}), StatusCode::denied, "wrong TID cannot release lease");
    Handle bad = handle; bad.slot = Pool::capacity;
    status(pool.release(bad, {1, 2}), StatusCode::invalid_argument, "invalid slot rejected");
    bad = handle; ++bad.generation;
    status(pool.release(bad, {1, 2}), StatusCode::not_found, "forged generation rejected");
    status(pool.activate_entry(handle, {1, 2}, 1).status(), StatusCode::unsupported, "nonboot CPU rejected");
    status(pool.activate_entry(handle, {9, 2}, 0).status(), StatusCode::denied, "foreign activation denied");
    auto activated = pool.activate_entry(handle, {1, 2}, 0);
    check(activated.ok(), "activate owned lease");
    const View view = activated.value();
    check(view.valid() && view.owner_pid() == 1 && view.owner_tid() == 2, "view binds canonical owner");
    check(view.lower() % Pool::page_size == 0 && view.top() % Pool::page_size == 0 &&
          view.top() - view.lower() == Pool::usable_bytes, "view exposes full aligned 64 KiB stack");
    status(pool.validate_inactive(handle, {1, 2}), StatusCode::busy, "inactive preflight rejects active lease");
    status(pool.top(handle, {1, 2}).status(), StatusCode::busy, "inactive top query rejects active lease");
    check(view.contains(view.lower()) && view.contains(view.top() - 1) && !view.contains(view.top()) &&
          !view.contains(view.lower() - 1) && !view.contains(view.lower(), ~usize{0}), "capture range bounds checked");
    status(pool.release(handle, {1, 2}), StatusCode::busy, "active stack cannot be wiped");
    status(pool.activate_entry(handle, {1, 2}, 0).status(), StatusCode::busy, "nested activation denied");
    status(pool.deactivate_entry(handle, {1, 2}, 1), StatusCode::denied, "wrong CPU cannot deactivate");
    check(view.valid(), "failed foreign deactivation preserves live view");
    check(pool.deactivate_entry(handle, {1, 2}, 0).ok(), "deactivate legitimate owner");
    check(!view.valid() && !view.lower() && !view.top() && !view.contains(0), "deactivation invalidates copied view");
    status(pool.deactivate_entry(handle, {1, 2}, 0), StatusCode::not_found, "double deactivation rejected");
    auto next = pool.activate_entry(handle, {1, 2}, 0);
    check(next.ok() && next.value().valid() && !view.valid(), "same lease gets a fresh activation epoch");
    check(pool.deactivate_entry(handle, {1, 2}, 0).ok(), "deactivate second epoch");
    check(pool.release(handle, {1, 2}).ok(), "release inactive owned stack");
    status(pool.release(handle, {1, 2}), StatusCode::not_found, "double release rejected");
    auto reused = pool.reserve({3, 4});
    check(reused.ok() && reused.value().slot == handle.slot && reused.value().generation != handle.generation,
          "released slot reuse receives fresh generation");
    status(pool.activate_entry(handle, {1, 2}, 0).status(), StatusCode::not_found, "stale handle cannot activate reused slot");
    check(pool.release(reused.value(), {3, 4}).ok(), "release reused slot");
    zero(std::span(backing).first(Pool::slot_stride), "release wipes full slot including canaries");
    const auto accounting = pool.accounting();
    check(accounting.leased_slots == 0 && accounting.active_slots == 0 && accounting.quarantined_slots == 0 &&
          accounting.reservations == 2 && accounting.releases == 2 && accounting.activations == 2 &&
          accounting.peak_slots == 1 && accounting.backing_bytes == Pool::arena_bytes,
          "ownership and activation accounting remains exact");
}

void capacity_and_storage()
{
    Pool pool;
    check(pool.activate(backing).ok(), "capacity arena initialization");
    std::array<Handle, Pool::capacity> handles{};
    std::array<View, Pool::capacity> views{};
    for (usize i = 0; i < Pool::capacity; ++i)
    {
        const Owner owner{7, i + 1};
        auto handle = pool.reserve(owner); check(handle.ok(), "reserve all 128 leases");
        handles[i] = handle.value();
        auto view = pool.activate_entry(handles[i], owner, 0); check(view.ok(), "activate each reserved lease");
        views[i] = view.value();
        auto *bytes = reinterpret_cast<std::byte *>(view.value().lower());
        zero({bytes, Pool::usable_bytes}, "new usable stack bytes are zero");
        bytes[0] = std::byte{0x13}; bytes[Pool::usable_bytes - 1] = std::byte{0x37};
        if (i)
            status(pool.activate_entry(handles[0], {7, 1}, 0).status(), StatusCode::busy,
                   "only one active lease across owners");
        check(pool.deactivate_entry(handles[i], owner, 0).ok(), "complete each activation");
    }
    status(pool.reserve({9, 9}).status(), StatusCode::no_memory, "capacity exhaustion is explicit");
    check(pool.accounting().leased_slots == 128 && pool.accounting().leased_usable_bytes == 8 * 1024 * 1024,
          "canary backing charge distinct from usable stack bytes");
    for (usize i = 0; i < Pool::capacity; ++i)
    {
        check(!views[i].valid(), "saved activation stays inactive");
        auto bytes = std::span(backing).subspan(i * Pool::slot_stride + Pool::canary_bytes, Pool::usable_bytes);
        check(bytes.front() == std::byte{0x13} && bytes.back() == std::byte{0x37}, "disjoint leases preserve their bytes");
        check(pool.release(handles[i], {7, i + 1}).ok(), "release all leases");
    }
    zero(backing, "full retirement wipes arena");
    check(pool.accounting().releases == 128 && pool.accounting().peak_slots == 128, "capacity accounting exact");
}

void guard_failure(bool suffix, unsigned path)
{
    Pool pool;
    check(pool.activate(backing).ok(), "corruption arena initialization");
    auto lease = pool.reserve({20, 21}); auto healthy = pool.reserve({20, 22});
    check(lease.ok() && healthy.ok(), "two leases before corruption");
    View active;
    if (path == 1)
    {
        auto view = pool.activate_entry(lease.value(), {20, 21}, 0);
        check(view.ok(), "activate before corruption"); active = view.value();
    }
    const usize index = suffix ? Pool::canary_bytes + Pool::usable_bytes + 731 : 511;
    backing[index] ^= std::byte{1};
    if (path == 1)
        status(pool.deactivate_entry(lease.value(), {20, 21}, 0), StatusCode::fault, "deactivation detects canary corruption");
    else if (path == 2)
        status(pool.validate_inactive(lease.value(), {20, 21}), StatusCode::fault, "inactive preflight detects canary corruption");
    else if (path == 3)
        status(pool.activate_entry(lease.value(), {20, 21}, 0).status(), StatusCode::fault, "activation detects canary corruption");
    else
        status(pool.release(lease.value(), {20, 21}), StatusCode::fault, "release detects canary corruption");
    check(!active.valid() && pool.accounting().active_slots == 0 && pool.accounting().quarantined_slots == 1,
          "corruption revokes views but keeps quarantined backing owned");
    status(pool.reserve({30, 31}).status(), StatusCode::fault, "corrupted pool does not issue new leases");
    status(pool.activate_entry(healthy.value(), {20, 22}, 0).status(), StatusCode::fault, "corrupted pool cannot resume execution");
    status(pool.release(lease.value(), {20, 21}), StatusCode::fault, "quarantined stack cannot be freed");
    check(pool.release(healthy.value(), {20, 22}).ok(), "unrelated healthy lease can still retire");
    check(pool.accounting().leased_slots == 1 && pool.accounting().releases == 1, "quarantine accounting does not pretend to reclaim");
}

void counter_exhaustion()
{
    Pool pool;
    check(pool.activate(backing).ok(), "overflow arena initialization");
    KernelEntryStackPoolTestAccess::last_generation(pool);
    auto last = pool.reserve({90, 91});
    check(last.ok() && last.value().generation == ~u64{0}, "final lease generation is usable");
    status(pool.reserve({90, 92}).status(), StatusCode::overflow, "lease generation never wraps");
    KernelEntryStackPoolTestAccess::last_epoch(pool);
    auto view = pool.activate_entry(last.value(), {90, 91}, 0);
    check(view.ok(), "final activation epoch is usable");
    check(pool.deactivate_entry(last.value(), {90, 91}, 0).ok(), "final epoch can deactivate");
    status(pool.activate_entry(last.value(), {90, 91}, 0).status(), StatusCode::overflow, "activation epoch never wraps");
    check(!view.value().valid(), "exhausted activation cannot revive old view");
    check(pool.release(last.value(), {90, 91}).ok(), "counter exhaustion still permits retirement");
}
} // namespace

int main()
{
    initialization_and_owner_checks();
    capacity_and_storage();
    for (unsigned path = 0; path < 4; ++path)
    {
        guard_failure(false, path);
        guard_failure(true, path);
    }
    counter_exhaustion();
    std::printf("PASS kernel-entry-stack-pool checks=%zu scope=hosted-model guest=0 smp=0\n", checks);
}
