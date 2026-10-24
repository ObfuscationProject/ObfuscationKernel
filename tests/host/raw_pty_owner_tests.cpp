#include "ok/sched/raw_pty_transport.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <type_traits>

namespace
{
using namespace ok;
using namespace ok::sched::private_pty;
std::size_t checks{}, cases{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL line=%d expression=%s\n", __LINE__, #expression); std::abort(); } } while (false)

template <typename Table> PairHandles make(Table &table)
{
    const auto result = table.create(); CHECK(result); return result.value();
}
template <typename Table> void empty(const Table &table)
{
    const auto audit = table.audit();
    CHECK(audit.live_pairs == 0); CHECK(audit.live_endpoints == 0);
    CHECK(audit.buffered_bytes == 0); CHECK(audit.cleared_outside_live_bytes);
}
template <typename Table> void finish(Table &table, PairHandles pair)
{
    CHECK(table.retire(pair.master)); const auto last = table.retire(pair.slave);
    CHECK(last && last.value().pair_freed); empty(table);
}
template <typename Table, std::size_t N>
void put(Table &table, Handle handle, const std::array<std::byte, N> &bytes)
{
    const auto result = table.write(handle, bytes);
    CHECK(result && result.value().bytes == N && result.value().change.changed);
    CHECK(result.value().change.stamp.slot == handle.slot);
    CHECK(result.value().change.stamp.generation == handle.generation);
}
template <typename Table, std::size_t N>
void take(Table &table, Handle handle, const std::array<std::byte, N> &expected)
{
    std::array<std::byte, N + 2> output{}; output.fill(std::byte{0xa5});
    const auto result = table.read(handle, {output.data(), N});
    CHECK(result && result.value().bytes == N && result.value().change.changed);
    CHECK(std::equal(expected.begin(), expected.end(), output.begin()));
    CHECK(output[N] == std::byte{0xa5} && output[N + 1] == std::byte{0xa5});
    CHECK(table.audit().cleared_outside_live_bytes);
}

void dimensions_and_initial_state()
{
    static_assert(!std::is_copy_constructible_v<RawPtyTable<>>);
    static_assert(!std::is_move_constructible_v<RawPtyTable<>>);
    static_assert(!std::is_polymorphic_v<RawPtyTable<>>);
    RawPtyTable<> table; empty(table);
    const auto pair = make(table);
    CHECK(pair.master.slot == pair.slave.slot && pair.master.generation == 1);
    CHECK(pair.master.side == Side::master && pair.slave.side == Side::slave);
    for (auto handle : {pair.master, pair.slave})
    {
        const auto state = table.readiness(handle); CHECK(state);
        CHECK(!state.value().readable && state.value().writable && !state.value().hangup && !state.value().error);
        CHECK(state.value().buffered == 0 && state.value().outgoing_available == 4096);
        CHECK(state.value().stamp.generation == 1 && state.value().stamp.revision == 1);
        CHECK(!state.value().stamp.revision_saturated);
        std::array<std::byte, 1> output{};
        CHECK(table.read(handle, output).status().code() == StatusCode::would_block);
    }
    CHECK(table.audit().live_pairs == 1 && table.audit().live_endpoints == 2);
    finish(table, pair);
}

void independent_pairs_and_binary_directions()
{
    RawPtyTable<2, 5> table; const auto a = make(table), b = make(table);
    CHECK(a.master.slot != b.master.slot);
    const std::array first{std::byte{0}, std::byte{0xff}, std::byte{0x03}, std::byte{0x1a}, std::byte{0x0a}};
    const std::array second{std::byte{0x80}, std::byte{0}, std::byte{0x7f}};
    const std::array third{std::byte{0x42}, std::byte{0x21}};
    put(table, a.master, first); put(table, a.slave, second);
    put(table, b.slave, third); put(table, b.master, second);
    take(table, b.master, third); take(table, a.slave, first);
    take(table, b.slave, second); take(table, a.master, second);
    CHECK(table.audit().buffered_bytes == 0);
    CHECK(table.retire(a.master)); CHECK(table.retire(a.slave));
    CHECK(table.audit().live_pairs == 1 && table.audit().live_endpoints == 2);
    finish(table, b);
}

void wrap_short_transfers_and_backpressure()
{
    RawPtyTable<1, 4> table; const auto pair = make(table);
    const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}};
    auto result = table.write(pair.master, payload); CHECK(result && result.value().bytes == 4);
    const auto full = table.readiness(pair.master); CHECK(full && !full.value().writable && !full.value().outgoing_available);
    CHECK(table.write(pair.master, payload).status().code() == StatusCode::would_block);
    take(table, pair.slave, std::array{std::byte{1}, std::byte{2}});
    result = table.write(pair.master, {payload.data() + 4, 2}); CHECK(result && result.value().bytes == 2);
    take(table, pair.slave, std::array{std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}});
    CHECK(table.readiness(pair.master).value().writable);
    std::array<std::byte, 12> output{}; output.fill(std::byte{0xa5});
    put(table, pair.slave, std::array{std::byte{7}});
    result = table.read(pair.master, output); CHECK(result && result.value().bytes == 1);
    CHECK(output[0] == std::byte{7} && output[1] == std::byte{0xa5});
    CHECK(table.read(pair.master, output).status().code() == StatusCode::would_block);
    CHECK(table.audit().cleared_outside_live_bytes); finish(table, pair);
}

void close_drain_and_local_invalidation(bool close_master)
{
    RawPtyTable<1, 5> table; const auto pair = make(table);
    const auto local = close_master ? pair.master : pair.slave;
    const auto peer = close_master ? pair.slave : pair.master;
    const std::array accepted{std::byte{0}, std::byte{0xff}, std::byte{0x23}};
    const std::array discarded{std::byte{0x43}, std::byte{0x44}};
    put(table, local, accepted); put(table, peer, discarded);
    const auto closed = table.retire(local); CHECK(closed);
    CHECK(closed.value().changed && !closed.value().pair_freed);
    CHECK(closed.value().master_retired == close_master && closed.value().slave_retired != close_master);
    CHECK(closed.value().stamp.generation == local.generation);
    const auto audit = table.audit();
    CHECK(audit.live_pairs == 1 && audit.live_endpoints == 1 && audit.buffered_bytes == accepted.size());
    CHECK(audit.cleared_outside_live_bytes);
    std::array<std::byte, 6> output{};
    CHECK(table.read(local, output).status().code() == StatusCode::bad_descriptor);
    CHECK(table.write(local, accepted).status().code() == StatusCode::bad_descriptor);
    CHECK(table.read(local, {}).status().code() == StatusCode::bad_descriptor);
    CHECK(table.write(local, {}).status().code() == StatusCode::bad_descriptor);
    CHECK(table.readiness(local).status().code() == StatusCode::bad_descriptor);
    CHECK(table.retire(local).status().code() == StatusCode::bad_descriptor);
    auto ready = table.readiness(peer); CHECK(ready);
    CHECK(ready.value().readable && !ready.value().writable && ready.value().hangup);
    CHECK(ready.value().error == close_master); // Native convention, not Linux PTY poll.
    CHECK(table.write(peer, accepted).status().code() == StatusCode::io_error);
    const auto before_zero = ready.value().stamp;
    const auto read_zero = table.read(peer, {}), write_zero = table.write(peer, {});
    CHECK(read_zero && !read_zero.value().bytes && !read_zero.value().change.changed);
    CHECK(write_zero && !write_zero.value().bytes && !write_zero.value().change.changed);
    CHECK(read_zero.value().change.stamp == before_zero && write_zero.value().change.stamp == before_zero);
    take(table, peer, std::array{std::byte{0}, std::byte{0xff}});
    take(table, peer, std::array{std::byte{0x23}});
    const auto terminal = table.read(peer, output);
    if (close_master) CHECK(terminal && terminal.value().bytes == 0 && !terminal.value().change.changed);
    else CHECK(terminal.status().code() == StatusCode::io_error);
    ready = table.readiness(peer); CHECK(ready && ready.value().readable && ready.value().hangup);
    CHECK(ready.value().error == close_master && !ready.value().writable && ready.value().buffered == 0);
    CHECK(table.retire(peer).value().pair_freed); empty(table);
}

void invalid_handles_and_generation_reuse()
{
    RawPtyTable<1, 3> table; const auto first = make(table);
    const std::array payload{std::byte{0xa5}};
    std::array<std::byte, 1> output{};
    const std::array invalid{Handle{}, Handle{invalid_slot, first.master.generation, Side::master},
        Handle{first.master.slot, 0, Side::master},
        Handle{first.master.slot, first.master.generation + 1, Side::master},
        Handle{first.master.slot, first.master.generation, static_cast<Side>(2)}};
    for (auto handle : invalid)
    {
        CHECK(table.read(handle, output).status().code() == StatusCode::bad_descriptor);
        CHECK(table.write(handle, payload).status().code() == StatusCode::bad_descriptor);
        CHECK(table.readiness(handle).status().code() == StatusCode::bad_descriptor);
        CHECK(table.retire(handle).status().code() == StatusCode::bad_descriptor);
    }
    put(table, first.master, payload); finish(table, first);
    const auto next = make(table);
    CHECK(next.master.slot == first.master.slot && next.master.generation > first.master.generation);
    for (auto old : {first.master, first.slave})
    {
        CHECK(table.write(old, payload).status().code() == StatusCode::bad_descriptor);
        CHECK(table.read(old, output).status().code() == StatusCode::bad_descriptor);
        CHECK(table.retire(old).status().code() == StatusCode::bad_descriptor);
    }
    CHECK(table.read(next.slave, output).status().code() == StatusCode::would_block);
    CHECK(table.audit().buffered_bytes == 0); finish(table, next);
}

void capacity_and_generation_exhaustion()
{
    RawPtyTable<2, 1, 1> table;
    const auto a = make(table), b = make(table);
    CHECK(table.create().status().code() == StatusCode::system_file_limit);
    CHECK(table.retire(a.master)); CHECK(table.create().status().code() == StatusCode::system_file_limit);
    CHECK(table.retire(a.slave)); CHECK(table.create().status().code() == StatusCode::overflow);
    CHECK(table.retire(b.master)); CHECK(table.retire(b.slave)); empty(table);
    CHECK(table.create().status().code() == StatusCode::overflow); empty(table);
    RawPtyTable<2, 1, 1> skip;
    const auto once = make(skip); finish(skip, once);
    const auto other = make(skip); CHECK(other.master.slot != once.master.slot);
    finish(skip, other); CHECK(skip.create().status().code() == StatusCode::overflow);
}

void full_width_generation_does_not_wrap()
{
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    RawPtyTable<1, 1, maximum, maximum, maximum - 1> table;
    const auto pair = make(table);
    CHECK(pair.master.generation == maximum && pair.slave.generation == maximum);
    const auto ready = table.readiness(pair.master); CHECK(ready && ready.value().stamp.generation == maximum);
    const std::array payload{std::byte{1}}; put(table, pair.master, payload);
    const auto closed = table.retire(pair.master);
    CHECK(closed && closed.value().stamp.generation == maximum);
    take(table, pair.slave, payload); CHECK(table.retire(pair.slave));
    CHECK(table.create().status().code() == StatusCode::overflow); empty(table);
}

void revision_saturation_preserves_closure()
{
    RawPtyTable<1, 2, 3, 2> table; const auto pair = make(table);
    const std::array payload{std::byte{0xff}}; put(table, pair.master, payload);
    const auto saturated = table.readiness(pair.slave).value().stamp;
    CHECK(saturated.revision == 2 && saturated.revision_saturated);
    take(table, pair.slave, payload);
    CHECK(table.readiness(pair.slave).value().stamp == saturated);
    CHECK(table.retire(pair.master).value().stamp == saturated);
    const auto terminal = table.readiness(pair.slave);
    CHECK(terminal && terminal.value().error && terminal.value().hangup && terminal.value().readable);
    CHECK(table.retire(pair.slave).value().pair_freed); empty(table);
    const auto next = make(table);
    CHECK(next.master.generation != saturated.generation);
    CHECK(table.readiness(next.master).value().stamp.revision == 1); finish(table, next);
    RawPtyTable<1, 1, 1, 1> immediate;
    const auto at_limit = make(immediate); CHECK(immediate.readiness(at_limit.master).value().stamp.revision_saturated);
    finish(immediate, at_limit);
}

void zero_io_preserves_live_queue_state()
{
    RawPtyTable<1, 1> table; const auto pair = make(table);
    const std::array payload{std::byte{0x23}}; put(table, pair.master, payload);
    for (auto handle : {pair.master, pair.slave})
    {
        const auto before = table.readiness(handle).value();
        const auto read = table.read(handle, {}), write = table.write(handle, {});
        CHECK(read && write && !read.value().bytes && !write.value().bytes);
        CHECK(!read.value().change.changed && !write.value().change.changed);
        const auto after = table.readiness(handle).value();
        CHECK(before.stamp == after.stamp && before.buffered == after.buffered);
        CHECK(before.outgoing_available == after.outgoing_available && before.writable == after.writable);
    }
    take(table, pair.slave, payload); finish(table, pair);
}

void repeated_retirement_clears_storage()
{
    RawPtyTable<1, 7> table;
    Handle previous{};
    for (std::size_t round = 0; round < 1000; ++round)
    {
        const auto pair = make(table); CHECK(pair.master.generation == round + 1);
        CHECK(table.readiness(previous).status().code() == StatusCode::bad_descriptor);
        const std::array payload{std::byte{0xff}, std::byte{0}, std::byte{0x23}};
        put(table, pair.master, payload); put(table, pair.slave, payload);
        if (round % 2) { CHECK(table.retire(pair.master)); CHECK(table.retire(pair.slave)); }
        else { CHECK(table.retire(pair.slave)); CHECK(table.retire(pair.master)); }
        empty(table); previous = pair.master;
    }
}

// Independent deque oracle: no ring-buffer indices or payload storage are shared.
struct ModelPair
{
    bool used{}, master{}, slave{};
    std::uint64_t generation{}, revision{};
    PairHandles handles{};
    std::deque<std::byte> master_input{}, slave_input{};
};
std::uint64_t random_state = 0x619e727cc482308dULL;
std::uint64_t next_random()
{
    random_state ^= random_state << 13; random_state ^= random_state >> 7; random_state ^= random_state << 17;
    return random_state;
}

void deterministic_model_comparison()
{
    constexpr std::size_t pair_count = 3, capacity = 7;
    RawPtyTable<pair_count, capacity> table;
    std::array<ModelPair, pair_count> model{};
    std::array<std::byte, 13> input{}, output{};
    for (std::size_t step = 0; step < 100000; ++step)
    {
        const auto operation = next_random() % 6;
        if (operation == 0)
        {
            const auto free = std::count_if(model.begin(), model.end(), [](const auto &pair) { return !pair.used; });
            const auto created = table.create();
            if (!free) CHECK(created.status().code() == StatusCode::system_file_limit);
            else
            {
                CHECK(created); const auto pair = created.value();
                CHECK(pair.master.slot < model.size()); auto &state = model[pair.master.slot];
                CHECK(!state.used && pair.master.generation > state.generation);
                CHECK(pair.master.slot == pair.slave.slot && pair.master.generation == pair.slave.generation);
                state.used = state.master = state.slave = true;
                state.generation = pair.master.generation; state.revision = 1; state.handles = pair;
                CHECK(state.master_input.empty() && state.slave_input.empty());
            }
        }
        else
        {
            auto &state = model[next_random() % pair_count];
            const bool master = (next_random() & 1) != 0;
            const auto handle = master ? state.handles.master : state.handles.slave;
            const bool local_alive = state.used && (master ? state.master : state.slave);
            const bool peer_alive = state.used && (master ? state.slave : state.master);
            auto &incoming = master ? state.master_input : state.slave_input;
            auto &outgoing = master ? state.slave_input : state.master_input;
            const auto count = static_cast<std::size_t>(next_random() % input.size());
            if (operation == 1 || operation == 2)
            {
                for (auto &byte : input) byte = static_cast<std::byte>(next_random() & 0xff);
                const auto result = table.write(handle, {input.data(), count});
                const auto error = !local_alive ? StatusCode::bad_descriptor : !count ? StatusCode::ok :
                    !peer_alive ? StatusCode::io_error : outgoing.size() == capacity ? StatusCode::would_block : StatusCode::ok;
                CHECK(result.status().code() == error);
                if (result)
                {
                    const auto expected = std::min(count, capacity - outgoing.size());
                    CHECK(result.value().bytes == expected);
                    for (std::size_t index = 0; index < expected; ++index) outgoing.push_back(input[index]);
                    if (expected) ++state.revision;
                    CHECK(result.value().change.changed == (expected != 0));
                    CHECK(result.value().change.stamp.revision == state.revision);
                }
            }
            else if (operation == 3 || operation == 4)
            {
                output.fill(std::byte{0xa5});
                const auto result = table.read(handle, {output.data(), count});
                const auto error = !local_alive ? StatusCode::bad_descriptor : !count || !incoming.empty() ? StatusCode::ok :
                    peer_alive ? StatusCode::would_block : master ? StatusCode::io_error : StatusCode::ok;
                CHECK(result.status().code() == error);
                std::size_t expected = 0;
                if (result)
                {
                    expected = std::min(count, incoming.size()); CHECK(result.value().bytes == expected);
                    for (std::size_t index = 0; index < expected; ++index)
                    { CHECK(output[index] == incoming.front()); incoming.pop_front(); }
                    if (expected) ++state.revision;
                    CHECK(result.value().change.changed == (expected != 0));
                    CHECK(result.value().change.stamp.revision == state.revision);
                }
                for (std::size_t index = expected; index < output.size(); ++index) CHECK(output[index] == std::byte{0xa5});
            }
            else
            {
                const auto result = table.retire(handle);
                CHECK(result.status().code() == (local_alive ? StatusCode::ok : StatusCode::bad_descriptor));
                if (result)
                {
                    if (master) state.master = false; else state.slave = false;
                    incoming.clear(); ++state.revision;
                    state.used = state.master || state.slave;
                    CHECK(result.value().pair_freed == !state.used);
                    CHECK(result.value().master_retired == !state.master && result.value().slave_retired == !state.slave);
                    CHECK(result.value().stamp.generation == state.generation && result.value().stamp.revision == state.revision);
                }
            }
        }
        std::size_t pairs = 0, endpoints = 0, buffered = 0;
        for (const auto &state : model)
        {
            pairs += state.used ? 1 : 0; endpoints += (state.master ? 1 : 0) + (state.slave ? 1 : 0);
            buffered += state.master_input.size() + state.slave_input.size();
            for (const bool master : {false, true})
            {
                const auto handle = master ? state.handles.master : state.handles.slave;
                const bool local = state.used && (master ? state.master : state.slave);
                const bool peer = state.used && (master ? state.slave : state.master);
                const auto &incoming = master ? state.master_input : state.slave_input;
                const auto &outgoing = master ? state.slave_input : state.master_input;
                const auto actual = table.readiness(handle);
                CHECK(actual.status().code() == (local ? StatusCode::ok : StatusCode::bad_descriptor));
                if (!actual) continue;
                CHECK(actual.value().buffered == incoming.size() && actual.value().outgoing_available == capacity - outgoing.size());
                CHECK(actual.value().readable == (!incoming.empty() || !peer));
                CHECK(actual.value().writable == (peer && outgoing.size() != capacity));
                CHECK(actual.value().hangup == !peer && actual.value().error == (!master && !peer));
                CHECK(actual.value().stamp.slot == handle.slot && actual.value().stamp.generation == state.generation);
                CHECK(actual.value().stamp.revision == state.revision && !actual.value().stamp.revision_saturated);
            }
        }
        const auto audit = table.audit();
        CHECK(audit.live_pairs == pairs && audit.live_endpoints == endpoints && audit.buffered_bytes == buffered);
        CHECK(audit.cleared_outside_live_bytes);
    }
    for (auto &state : model)
    {
        if (state.master) CHECK(table.retire(state.handles.master));
        if (state.slave) CHECK(table.retire(state.handles.slave));
    }
    empty(table);
}

void checked_read_preview_identity_and_commit()
{
    using Table = RawPtyTable<1, 8>;
    static_assert(!std::is_copy_constructible_v<Table::PreparedRead>);
    static_assert(std::is_nothrow_move_constructible_v<Table::PreparedRead>);
    Table table; const auto pair = make(table);
    const std::array payload{std::byte{1}, std::byte{0}, std::byte{3}, std::byte{4}};
    CHECK(table.prepare_read(pair.slave, 4).status().code() == StatusCode::would_block);
    put(table, pair.master, payload);
    for (std::size_t prefix = 0; prefix <= payload.size(); ++prefix)
    {
        auto preview = table.prepare_read(pair.slave, 8); CHECK(preview);
        CHECK(preview.value().endpoint() == pair.slave && preview.value().bytes().size() == payload.size());
        std::array<std::byte, 4> partially_copied{};
        std::copy_n(preview.value().bytes().begin(), prefix, partially_copied.begin());
        preview.value().cancel();
        CHECK(preview.value().commit().status().code() == StatusCode::busy);
        CHECK(table.readiness(pair.slave).value().buffered == payload.size());
        auto next = table.prepare_read(pair.slave, 8); CHECK(next);
        CHECK(std::equal(payload.begin(), payload.end(), next.value().bytes().begin()));
    }
    auto first = table.prepare_read(pair.slave, 2); CHECK(first);
    Table::PreparedRead moved{std::move(first.value())};
    CHECK(first.value().commit().status().code() == StatusCode::busy);
    CHECK(moved.bytes().size() == 2 && moved.bytes()[0] == payload[0] && moved.bytes()[1] == payload[1]);
    const auto consumed = moved.commit(); CHECK(consumed && consumed.value().bytes == 2);
    CHECK(moved.commit().status().code() == StatusCode::busy);
    auto stale = table.prepare_read(pair.slave, 2); CHECK(stale);
    put(table, pair.slave, std::array{std::byte{9}}); // Either direction invalidates the exact revision.
    CHECK(stale.value().commit().status().code() == StatusCode::busy);
    take(table, pair.slave, std::array{std::byte{3}, std::byte{4}});
    auto zero = table.prepare_read(pair.slave, 0); CHECK(zero && zero.value().bytes().empty());
    CHECK(zero.value().commit().value().bytes == 0);
    put(table, pair.master, payload);
    auto retired = table.prepare_read(pair.slave, 4); CHECK(retired);
    CHECK(table.retire(pair.slave)); CHECK(table.retire(pair.master));
    const auto next = make(table); CHECK(next.slave.generation != pair.slave.generation);
    CHECK(retired.value().commit().status().code() == StatusCode::bad_descriptor);
    CHECK(table.retire(next.master));
    auto eof = table.prepare_read(next.slave, 4); CHECK(eof && eof.value().bytes().empty());
    CHECK(eof.value().commit().value().bytes == 0);
    CHECK(table.retire(next.slave)); empty(table);
}

void checked_read_preview_saturation_and_peer_retirement()
{
    RawPtyTable<1, 4, 2, 3> table; const auto pair = make(table);
    const std::array payload{std::byte{1}, std::byte{2}};
    put(table, pair.master, payload); // Revision two permits a preview.
    auto preview = table.prepare_read(pair.slave, 2); CHECK(preview);
    CHECK(table.retire(pair.master)); // Revision three saturates, preserving buffered input.
    CHECK(preview.value().commit().status().code() == StatusCode::busy);
    CHECK(table.prepare_read(pair.slave, 2).status().code() == StatusCode::overflow);
    take(table, pair.slave, payload); // Direct reads remain valid at diagnostic saturation.
    auto eof = table.prepare_read(pair.slave, 2); CHECK(eof && eof.value().bytes().empty());
    CHECK(eof.value().commit().value().bytes == 0);
    CHECK(table.retire(pair.slave)); empty(table);
}

template <typename Test> void run(const char *name, Test test)
{
    test(); ++cases; std::printf("PASS %s\n", name);
}
} // namespace

int main()
{
    run("default-dimensions-and-initial-state", dimensions_and_initial_state);
    run("independent-pairs-binary-directions", independent_pairs_and_binary_directions);
    run("wrap-short-transfers-backpressure", wrap_short_transfers_and_backpressure);
    run("master-close-slave-drain-eof", [] { close_drain_and_local_invalidation(true); });
    run("slave-close-master-drain-eio", [] { close_drain_and_local_invalidation(false); });
    run("invalid-handles-generation-reuse", invalid_handles_and_generation_reuse);
    run("capacity-and-generation-exhaustion", capacity_and_generation_exhaustion);
    run("full-width-generation-no-wrap", full_width_generation_does_not_wrap);
    run("revision-saturation-closure", revision_saturation_preserves_closure);
    run("zero-io-preserves-queues", zero_io_preserves_live_queue_state);
    run("repeated-retirement-storage-clear", repeated_retirement_clears_storage);
    run("checked-read-preview-identity-commit", checked_read_preview_identity_and_commit);
    run("checked-read-preview-saturation-closure", checked_read_preview_saturation_and_peer_retirement);
    run("independent-deque-model-100000-steps", deterministic_model_comparison);
    std::printf("PRIVATE_RAW_PTY_OWNER cases=%zu checks=%zu model_steps=100000 status=passed\n", cases, checks);
}
