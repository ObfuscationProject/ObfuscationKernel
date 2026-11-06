#include "ok/sched/raw_pty_transport.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <string>
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
    RawPtyTable<2, 64> table; const auto a = make(table), b = make(table);
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
    RawPtyTable<1, 64> table; const auto pair = make(table);
    std::array<std::byte, 66> payload{};
    for (std::size_t index = 0; index < payload.size(); ++index) payload[index] = static_cast<std::byte>(index + 1);
    auto result = table.write(pair.master, payload); CHECK(result && result.value().bytes == 64);
    const auto full = table.readiness(pair.master); CHECK(full && !full.value().writable && !full.value().outgoing_available);
    CHECK(table.write(pair.master, payload).status().code() == StatusCode::would_block);
    take(table, pair.slave, std::array{std::byte{1}, std::byte{2}});
    result = table.write(pair.master, {payload.data() + 64, 2}); CHECK(result && result.value().bytes == 2);
    std::array<std::byte, 64> rest{};
    std::copy(payload.begin() + 2, payload.end(), rest.begin());
    take(table, pair.slave, rest);
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
    RawPtyTable<1, 64> table; const auto pair = make(table);
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
    RawPtyTable<1, 64> table; const auto first = make(table);
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
    RawPtyTable<2, 64, 1> table;
    const auto a = make(table), b = make(table);
    CHECK(table.create().status().code() == StatusCode::system_file_limit);
    CHECK(table.retire(a.master)); CHECK(table.create().status().code() == StatusCode::system_file_limit);
    CHECK(table.retire(a.slave)); CHECK(table.create().status().code() == StatusCode::overflow);
    CHECK(table.retire(b.master)); CHECK(table.retire(b.slave)); empty(table);
    CHECK(table.create().status().code() == StatusCode::overflow); empty(table);
    RawPtyTable<2, 64, 1> skip;
    const auto once = make(skip); finish(skip, once);
    const auto other = make(skip); CHECK(other.master.slot != once.master.slot);
    finish(skip, other); CHECK(skip.create().status().code() == StatusCode::overflow);
}

void full_width_generation_does_not_wrap()
{
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    RawPtyTable<1, 64, maximum, maximum, maximum - 1> table;
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
    RawPtyTable<1, 64, 3, 2> table; const auto pair = make(table);
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
    RawPtyTable<1, 64, 1, 1> immediate;
    const auto at_limit = make(immediate); CHECK(immediate.readiness(at_limit.master).value().stamp.revision_saturated);
    finish(immediate, at_limit);
}

void zero_io_preserves_live_queue_state()
{
    RawPtyTable<1, 64> table; const auto pair = make(table);
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
    RawPtyTable<1, 64> table;
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
// Raw pairs run raw_pair_termios(); ptmx-style pairs are switched to the same
// raw termios so that N_TTY must equal the byte queue, while their open counts,
// dormant/open/closed states, locks and hangups follow the endpoint rules.
struct ModelPair
{
    bool used{}, multiplexed{}, seen{}, locked{};
    std::uint16_t master{}, slave{};
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
    constexpr std::size_t pair_count = 3, capacity = 64;
    using Table = RawPtyTable<pair_count, capacity>;
    Table table;
    std::array<ModelPair, pair_count> model{};
    std::array<std::byte, 40> input{}, output{};
    std::size_t multiplexed_created = 0, reopened = 0, hangups = 0;
    for (std::size_t step = 0; step < 100000; ++step)
    {
        const auto operation = next_random() % 10;
        if (operation == 0 || operation == 7)
        {
            const bool multiplexed = operation == 7;
            const auto free = std::count_if(model.begin(), model.end(), [](const auto &pair) { return !pair.used; });
            PairHandles pair{};
            if (multiplexed)
            {
                const auto created = table.create_multiplexed();
                if (!free) { CHECK(created.status().code() == StatusCode::system_file_limit); goto audit; }
                CHECK(created && created.value().side == Side::master);
                pair = {created.value(), {created.value().slot, created.value().generation, Side::slave}};
                auto *terminal = table.terminal(pair.master.slot, pair.master.generation);
                CHECK(terminal && terminal->termios == tty::linux_pty_termios() && !terminal->session);
                terminal->termios = tty::raw_pair_termios();
                ++multiplexed_created;
            }
            else
            {
                const auto created = table.create();
                if (!free) { CHECK(created.status().code() == StatusCode::system_file_limit); goto audit; }
                CHECK(created); pair = created.value();
                CHECK(table.terminal(pair.master.slot, pair.master.generation)->termios == tty::raw_pair_termios());
            }
            CHECK(pair.master.slot < model.size()); auto &state = model[pair.master.slot];
            CHECK(!state.used && pair.master.generation > state.generation);
            CHECK(pair.master.slot == pair.slave.slot && pair.master.generation == pair.slave.generation);
            state.used = true; state.multiplexed = multiplexed; state.seen = !multiplexed; state.locked = multiplexed;
            state.master = 1; state.slave = multiplexed ? 0 : 1;
            state.generation = pair.master.generation; state.revision = 1; state.handles = pair;
            CHECK(state.master_input.empty() && state.slave_input.empty());
        }
        else if (operation == 8)
        {
            // One more slave open (the /dev/pts/N path), with the admission order.
            auto &state = model[next_random() % pair_count];
            const auto status = table.admit_open(state.handles.slave);
            const auto expected = !state.handles.slave ? StatusCode::bad_descriptor :
                !state.used ? StatusCode::not_found : !state.master ? StatusCode::io_error :
                !state.multiplexed && !state.slave ? StatusCode::io_error : state.locked ? StatusCode::io_error : StatusCode::ok;
            CHECK(status.code() == expected);
            const auto change = table.attach(state.handles.slave);
            CHECK(change.changed == (expected == StatusCode::ok));
            if (expected == StatusCode::ok)
            {
                reopened += state.seen && !state.slave ? 1 : 0;
                ++state.slave; state.seen = true; ++state.revision;
                CHECK(change.stamp.revision == state.revision && !change.slave_retired && !change.master_retired);
            }
        }
        else if (operation == 9)
        {
            auto &state = model[next_random() % pair_count];
            const bool locked = (next_random() & 1) != 0;
            const auto status = table.set_locked(state.handles.master, locked);
            const auto index = table.index(state.handles.master);
            const auto expected = !state.used || !state.master ? StatusCode::bad_descriptor :
                !state.multiplexed ? StatusCode::not_terminal : StatusCode::ok;
            CHECK(status.code() == expected && index.status().code() == expected);
            CHECK(table.set_locked(state.handles.slave, locked).code() ==
                  (state.used && state.slave ? StatusCode::not_terminal : StatusCode::bad_descriptor));
            if (status) { state.locked = locked; CHECK(index.value() == state.handles.master.slot); }
        }
        else
        {
            auto &state = model[next_random() % pair_count];
            const bool master = (next_random() & 1) != 0;
            const auto handle = master ? state.handles.master : state.handles.slave;
            const bool local_alive = state.used && (master ? state.master : state.slave);
            // A raw master writes only to an open slave, a ptmx master always (the
            // slave keeps it for an open); a slave only while the master lives.
            const bool peer_open = state.used && (master ? state.multiplexed || state.slave != 0 : state.master != 0);
            const bool slave_closed = state.seen && !state.slave;
            auto &incoming = master ? state.master_input : state.slave_input;
            auto &outgoing = master ? state.slave_input : state.master_input;
            const auto count = static_cast<std::size_t>(next_random() % input.size());
            if (operation == 1 || operation == 2)
            {
                for (auto &byte : input) byte = static_cast<std::byte>(next_random() & 0xff);
                const auto result = table.write(handle, {input.data(), count});
                const auto error = !local_alive ? StatusCode::bad_descriptor : !count ? StatusCode::ok :
                    !peer_open ? StatusCode::io_error : outgoing.size() == capacity ? StatusCode::would_block : StatusCode::ok;
                CHECK(result.status().code() == error);
                if (result)
                {
                    const auto expected = std::min(count, capacity - outgoing.size());
                    CHECK(result.value().bytes == expected);
                    for (std::size_t index = 0; index < expected; ++index) outgoing.push_back(input[index]);
                    if (expected) ++state.revision;
                    CHECK(result.value().change.changed == (expected != 0));
                    CHECK(result.value().change.stamp.revision == state.revision);
                    if (expected) CHECK(result.value().change.wakes & (master ? wake_slave_read : wake_master_read));
                }
            }
            else if (operation == 3 || operation == 4)
            {
                output.fill(std::byte{0xa5});
                const auto result = table.read(handle, {output.data(), count});
                const auto error = !local_alive ? StatusCode::bad_descriptor : !count || !incoming.empty() ? StatusCode::ok :
                    master ? (slave_closed ? StatusCode::io_error : StatusCode::would_block) :
                    state.master ? StatusCode::would_block : StatusCode::ok;
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
                    if (expected) CHECK(result.value().change.wakes & (master ? wake_slave_write : wake_master_write));
                }
                for (std::size_t index = expected; index < output.size(); ++index) CHECK(output[index] == std::byte{0xa5});
            }
            else
            {
                const auto result = table.retire(handle);
                CHECK(result.status().code() == (local_alive ? StatusCode::ok : StatusCode::bad_descriptor));
                if (result)
                {
                    auto &opens = master ? state.master : state.slave;
                    if (!--opens)
                    {
                        if (master)
                        {
                            state.master_input.clear();
                            // The master's last open hangs a ptmx slave up: its input is discarded.
                            if (state.multiplexed) { state.slave_input.clear(); hangups += state.slave ? 1 : 0; }
                        }
                        else if (!state.multiplexed) state.slave_input.clear();
                    }
                    ++state.revision;
                    state.used = state.master || state.slave;
                    if (!state.used) { state.master_input.clear(); state.slave_input.clear(); state.locked = false; }
                    CHECK(result.value().pair_freed == !state.used);
                    CHECK(result.value().master_retired == !state.master);
                    CHECK(result.value().slave_retired == (state.seen && !state.slave));
                    CHECK(result.value().stamp.generation == state.generation && result.value().stamp.revision == state.revision);
                }
            }
        }
    audit:;
        std::size_t pairs = 0, endpoints = 0, buffered = 0, master_opens = 0, slave_opens = 0, dormant = 0, locked = 0;
        for (const auto &state : model)
        {
            pairs += state.used ? 1 : 0; endpoints += (state.master ? 1 : 0) + (state.slave ? 1 : 0);
            buffered += state.master_input.size() + state.slave_input.size();
            master_opens += state.master; slave_opens += state.slave;
            dormant += state.used && !state.seen ? 1 : 0; locked += state.used && state.locked ? 1 : 0;
            CHECK(table.open_count(state.handles.master) == (state.used ? state.master : 0));
            for (const bool master : {false, true})
            {
                const auto handle = master ? state.handles.master : state.handles.slave;
                const bool local = state.used && (master ? state.master : state.slave);
                const bool gone = !state.master, closed = state.seen && !state.slave;
                const auto &incoming = master ? state.master_input : state.slave_input;
                const auto &outgoing = master ? state.slave_input : state.master_input;
                const auto actual = table.readiness(handle);
                CHECK(actual.status().code() == (local ? StatusCode::ok : StatusCode::bad_descriptor));
                if (!actual) continue;
                CHECK(actual.value().buffered == incoming.size() && actual.value().outgoing_available == capacity - outgoing.size());
                if (master)
                {
                    CHECK(actual.value().readable == (!incoming.empty() || closed));
                    CHECK(actual.value().writable == ((state.multiplexed || state.slave) && outgoing.size() != capacity));
                    CHECK(actual.value().hangup == closed && !actual.value().error);
                    CHECK(actual.value().write_error == (!state.multiplexed && !state.slave));
                }
                else
                {
                    CHECK(actual.value().readable == (!incoming.empty() || gone));
                    CHECK(actual.value().writable == ((state.multiplexed && gone) || (!gone && outgoing.size() != capacity)));
                    CHECK(actual.value().hangup == gone && actual.value().error == gone && actual.value().write_error == gone);
                }
                CHECK(actual.value().stamp.slot == handle.slot && actual.value().stamp.generation == state.generation);
                CHECK(actual.value().stamp.revision == state.revision && !actual.value().stamp.revision_saturated);
            }
        }
        const auto audit = table.audit();
        CHECK(audit.live_pairs == pairs && audit.live_endpoints == endpoints && audit.buffered_bytes == buffered);
        CHECK(audit.master_opens == master_opens && audit.slave_opens == slave_opens);
        CHECK(audit.dormant_slaves == dormant && audit.locked_pairs == locked);
        CHECK(audit.cleared_outside_live_bytes);
    }
    CHECK(multiplexed_created > 100 && reopened > 10 && hangups > 10);
    for (auto &state : model)
    {
        while (state.used && state.master) { CHECK(table.retire(state.handles.master)); state.used = --state.master || state.slave; }
        while (state.slave) { CHECK(table.retire(state.handles.slave)); --state.slave; }
    }
    empty(table);
}

void checked_read_preview_identity_and_commit()
{
    using Table = RawPtyTable<1, 64>;
    static_assert(!std::is_copy_constructible_v<Table::PreparedRead>);
    static_assert(std::is_nothrow_move_constructible_v<Table::PreparedRead>);
    Table table; const auto pair = make(table);
    const std::array payload{std::byte{1}, std::byte{0}, std::byte{3}, std::byte{4}};
    CHECK(table.prepare_read(pair.slave, 4).status().code() == StatusCode::would_block);
    put(table, pair.master, payload);
    for (std::size_t prefix = 0; prefix <= payload.size(); ++prefix)
    {
        auto preview = table.prepare_read(pair.slave, 8); CHECK(preview);
        CHECK(preview.value().endpoint() == pair.slave && preview.value().size() == payload.size());
        std::array<std::byte, 4> partially_copied{};
        CHECK(preview.value().copy({partially_copied.data(), prefix}) == prefix);
        preview.value().cancel();
        CHECK(preview.value().commit().status().code() == StatusCode::busy);
        CHECK(table.readiness(pair.slave).value().buffered == payload.size());
        auto next = table.prepare_read(pair.slave, 8); CHECK(next);
        std::array<std::byte, 4> copied{}; CHECK(next.value().copy(copied) == 4);
        CHECK(std::equal(payload.begin(), payload.end(), copied.begin()));
        // Chunked previews start at an offset and stop at the plan.
        std::array<std::byte, 8> tail{}; CHECK(next.value().copy(tail, 3) == 1 && tail[0] == payload[3]);
    }
    auto first = table.prepare_read(pair.slave, 2); CHECK(first);
    Table::PreparedRead moved{std::move(first.value())};
    CHECK(first.value().commit().status().code() == StatusCode::busy);
    std::array<std::byte, 2> head{}; CHECK(moved.size() == 2 && moved.copy(head) == 2);
    CHECK(head[0] == payload[0] && head[1] == payload[1]);
    const auto consumed = moved.commit(); CHECK(consumed && consumed.value().bytes == 2);
    CHECK(moved.commit().status().code() == StatusCode::busy);
    auto stale = table.prepare_read(pair.slave, 2); CHECK(stale);
    put(table, pair.slave, std::array{std::byte{9}}); // Either direction invalidates the exact revision.
    CHECK(stale.value().commit().status().code() == StatusCode::busy);
    take(table, pair.slave, std::array{std::byte{3}, std::byte{4}});
    auto zero = table.prepare_read(pair.slave, 0); CHECK(zero && !zero.value().size());
    CHECK(zero.value().commit().value().bytes == 0);
    put(table, pair.master, payload);
    auto retired = table.prepare_read(pair.slave, 4); CHECK(retired);
    CHECK(table.retire(pair.slave)); CHECK(table.retire(pair.master));
    const auto next = make(table); CHECK(next.slave.generation != pair.slave.generation);
    CHECK(retired.value().commit().status().code() == StatusCode::bad_descriptor);
    CHECK(table.retire(next.master));
    auto eof = table.prepare_read(next.slave, 4); CHECK(eof && !eof.value().size());
    CHECK(eof.value().commit().value().bytes == 0);
    CHECK(table.retire(next.slave)); empty(table);
}

void checked_read_preview_saturation_and_peer_retirement()
{
    RawPtyTable<1, 64, 2, 3> table; const auto pair = make(table);
    const std::array payload{std::byte{1}, std::byte{2}};
    put(table, pair.master, payload); // Revision two permits a preview.
    auto preview = table.prepare_read(pair.slave, 2); CHECK(preview);
    CHECK(table.retire(pair.master)); // Revision three saturates, preserving buffered input.
    CHECK(preview.value().commit().status().code() == StatusCode::busy);
    CHECK(table.prepare_read(pair.slave, 2).status().code() == StatusCode::overflow);
    take(table, pair.slave, payload); // Direct reads remain valid at diagnostic saturation.
    auto eof = table.prepare_read(pair.slave, 2); CHECK(eof && !eof.value().size());
    CHECK(eof.value().commit().value().bytes == 0);
    CHECK(table.retire(pair.slave)); empty(table);
}

template <std::size_t N> std::span<const std::byte> text(const char (&value)[N])
{ return {reinterpret_cast<const std::byte *>(value), N - 1}; }
template <typename Table> std::string drain(Table &table, Handle handle)
{
    std::array<std::byte, 128> output{};
    const auto result = table.read(handle, output); CHECK(result);
    return {reinterpret_cast<const char *>(output.data()), result.value().bytes};
}

// Raw pairs: raw termios, both ends open once, no reopen after the slave closed;
// /dev/tty may still add slave opens while the slave is open.
void raw_termios_and_open_counts()
{
    RawPtyTable<2, 64> table; const auto pair = make(table);
    auto *terminal = table.terminal(pair.slave.slot, pair.slave.generation);
    CHECK(terminal && terminal->termios == tty::raw_pair_termios() && terminal->window == tty::WinSize{});
    CHECK(!table.terminal(pair.slave.slot, pair.slave.generation + 1));
    CHECK(table.index(pair.master).status().code() == StatusCode::not_terminal); // Not in /dev/pts.
    CHECK(table.set_locked(pair.master, true).code() == StatusCode::not_terminal);
    CHECK(!table.slave_handle(pair.slave.slot));
    CHECK(table.admit_open(pair.master).code() == StatusCode::invalid_argument);
    CHECK(table.admit_open(pair.slave)); CHECK(table.attach(pair.slave).changed);
    auto audit = table.audit();
    CHECK(audit.master_opens == 1 && audit.slave_opens == 2 && !audit.dormant_slaves && !audit.locked_pairs);
    CHECK(table.retire(pair.slave)); CHECK(table.readiness(pair.slave)); // One open left.
    put(table, pair.master, std::array{std::byte{'x'}});
    CHECK(table.retire(pair.slave)); CHECK(table.admit_open(pair.slave).code() == StatusCode::io_error);
    CHECK(!table.attach(pair.slave).changed && table.audit().slave_opens == 0);
    CHECK(table.read(pair.master, std::span<std::byte>{}).value().bytes == 0);
    CHECK(table.retire(pair.master).value().pair_freed); empty(table);
    CHECK(table.admit_open(pair.slave).code() == StatusCode::not_found);
    CHECK(table.admit_open(Handle{}).code() == StatusCode::bad_descriptor);
}

// Non-canonical slave reads below VMIN: an O_NONBLOCK read takes what is
// buffered (POSIX, n_tty_read), and after the master's last close a raw slave
// drains it before EOF. An unterminated canonical line is never returned.
void slave_vmin_nonblocking_and_drain(std::uint8_t vtime)
{
    RawPtyTable<2, 64> table; auto pair = make(table);
    auto *terminal = table.terminal(pair.slave.slot, pair.slave.generation);
    terminal->termios[tty::Cc::min] = 4; terminal->termios[tty::Cc::time] = vtime;
    std::array<std::byte, 8> output{};
    CHECK(table.read(pair.slave, output, true).status().code() == StatusCode::would_block); // Empty: EAGAIN.
    CHECK(table.write(pair.master, text("ab")).value().bytes == 2);
    CHECK(table.read(pair.slave, output).status().code() == StatusCode::would_block);
    CHECK(table.prepare_read(pair.slave, 8).status().code() == StatusCode::would_block);
    auto preview = table.prepare_read(pair.slave, 8, true); CHECK(preview && preview.value().size() == 2);
    preview.value().cancel();
    const auto taken = table.read(pair.slave, output, true);
    CHECK(taken && taken.value().bytes == 2 && output[1] == std::byte{'b'});
    CHECK(table.write(pair.master, text("cd")).value().bytes == 2);
    CHECK(table.retire(pair.master).value().master_retired);
    CHECK(table.readiness(pair.slave).value().readable && table.input_pending(pair.slave).value() == 2);
    CHECK(drain(table, pair.slave) == "cd" && drain(table, pair.slave).empty());
    CHECK(!table.readiness(pair.slave).value().buffered && table.audit().cleared_outside_live_bytes);
    CHECK(table.retire(pair.slave).value().pair_freed); empty(table);
    pair = make(table);
    terminal = table.terminal(pair.slave.slot, pair.slave.generation);
    terminal->termios.lflag |= tty::lflag::icanon;
    CHECK(table.write(pair.master, text("xy")).value().bytes == 2);
    CHECK(table.retire(pair.master).value().master_retired);
    CHECK(drain(table, pair.slave).empty() && terminal->ldisc.buffered() == 2);
    CHECK(table.retire(pair.slave).value().pair_freed); empty(table);
}

// ptmx-style pairs: dormant, locked, counted opens, N_TTY between the ends,
// slave reopen keeping input and session, and the master's hangup.
void multiplexed_terminal_semantics()
{
    RawPtyTable<2, 64> table;
    const auto created = table.create_multiplexed(); CHECK(created);
    const auto master = created.value();
    const auto slave = table.slave_handle(master.slot);
    CHECK(slave.side == Side::slave && slave.generation == master.generation);
    auto *terminal = table.terminal(slave.slot, slave.generation);
    CHECK(terminal && terminal->termios == tty::linux_pty_termios() && !terminal->hung_up);
    auto audit = table.audit();
    CHECK(audit.live_pairs == 1 && audit.master_opens == 1 && !audit.slave_opens && audit.dormant_slaves == 1);
    CHECK(audit.locked_pairs == 1 && audit.live_endpoints == 1);
    // Dormant: master reads block, no hangup; master writes go into the slave's
    // input for its first open (Linux counts the master's slave reference).
    std::array<std::byte, 4> scratch{};
    CHECK(table.read(master, scratch).status().code() == StatusCode::would_block);
    auto ready = table.readiness(master).value();
    CHECK(!ready.readable && ready.writable && !ready.hangup && !ready.error && !ready.write_error);
    CHECK(table.write(master, text("early\n")).value().bytes == 6 && drain(table, master) == "early\r\n");
    CHECK(table.readiness(slave).status().code() == StatusCode::bad_descriptor);
    CHECK(table.admit_open(slave).code() == StatusCode::io_error); // Locked.
    CHECK(table.index(master).value() == master.slot);
    CHECK(table.set_locked(master, false)); CHECK(table.admit_open(slave));
    CHECK(table.attach(slave).changed); CHECK(table.attach(slave).changed);
    CHECK(table.set_locked(slave, true).code() == StatusCode::not_terminal);
    CHECK(table.index(slave).status().code() == StatusCode::not_terminal);
    audit = table.audit(); CHECK(audit.slave_opens == 2 && !audit.dormant_slaves && !audit.locked_pairs);
    CHECK(drain(table, slave) == "early\n");
    // Canonical input with echo through OPOST into the master queue.
    auto written = table.write(master, text("hex\x7flo\r"));
    CHECK(written && written.value().bytes == 7);
    CHECK(written.value().change.wakes & wake_slave_read && written.value().change.wakes & wake_master_read);
    CHECK(drain(table, master) == "hex\b \blo\r\n");
    CHECK(drain(table, slave) == "helo\n");
    CHECK(table.write(master, text("ab")));
    CHECK(table.read(slave, scratch).status().code() == StatusCode::would_block); // No line yet.
    CHECK(table.input_pending(slave).value() == 0 && table.input_pending(master).value() == 2);
    CHECK(drain(table, master) == "ab");
    CHECK(table.write(master, text("\x04")));
    CHECK(drain(table, slave) == "ab");
    CHECK(table.read(slave, scratch).status().code() == StatusCode::would_block);
    // Slave output through OPOST (ONLCR).
    CHECK(table.write(slave, text("a\nb")).value().bytes == 3 && drain(table, master) == "a\r\nb");
    // ISIG: ^C to the foreground group, flushing input and the slave's unread output.
    terminal->session = 7; terminal->foreground = 9;
    CHECK(table.write(slave, text("out")) && table.write(master, text("in")));
    written = table.write(master, text("\x03"));
    CHECK(written && written.value().change.wakes & wake_slave_write);
    CHECK(terminal->outbox_count == 1 && terminal->outbox[0] == (sched::TerminalOutboxEntry{9, tty::TerminalSignal::interrupt, true}));
    CHECK(drain(table, master) == "^C" && !terminal->ldisc.buffered());
    // Repeated signal characters share one outbox entry: a write of more than
    // the outbox capacity is taken whole, with or without a foreground group.
    while (terminal->outbox_count) static_cast<void>(terminal->take_outbox());
    written = table.write(master, text("\x03\x03\x03\x03\x03\x03\x03\x03\x03\x03\x03\x1c"));
    CHECK(written && written.value().bytes == 12 && terminal->outbox_count == 2);
    CHECK(terminal->outbox[1] == (sched::TerminalOutboxEntry{9, tty::TerminalSignal::quit, true}));
    while (terminal->outbox_count) static_cast<void>(terminal->take_outbox());
    terminal->foreground = 0;
    CHECK(table.write(master, text("\x03\x03\x03\x03\x03\x03\x03\x03\x03")).value().bytes == 9 && !terminal->outbox_count);
    terminal->foreground = 9;
    static_cast<void>(drain(table, master));
    // A full outbox stops master input at the signal character (backpressure).
    for (sched::ProcessId target = 100; terminal->outbox_count < sched::terminal_outbox_capacity; ++target)
        CHECK(terminal->owe(target, tty::TerminalSignal::winch, false));
    CHECK(table.write(master, text("\x03")).status().code() == StatusCode::would_block);
    CHECK(table.write(master, text("k\x03")).value().bytes == 1);
    while (terminal->outbox_count) static_cast<void>(terminal->take_outbox());
    CHECK(table.write(master, text("\x15")).value().bytes == 1); // VKILL clears the partial line.
    static_cast<void>(drain(table, master));
    // IXON: ^S stops slave output, ^Q starts it again.
    CHECK(table.write(master, text("\x13")));
    CHECK(table.write(slave, text("z")).status().code() == StatusCode::would_block);
    CHECK(!table.readiness(slave).value().writable);
    written = table.write(master, text("\x11")); CHECK(written && written.value().change.wakes & wake_slave_write);
    CHECK(table.write(slave, text("z")).value().bytes == 1 && drain(table, master) == "z");
    // Echo that does not fit waits in the ldisc until the master reads.
    std::array<std::byte, 64> fill{}; fill.fill(std::byte{'f'});
    CHECK(table.write(slave, fill).value().bytes == 64);
    CHECK(table.write(master, text("qr")) && terminal->ldisc.echo_pending() == 2);
    CHECK(table.audit().cleared_outside_live_bytes);
    CHECK(table.write(slave, text("y")).status().code() == StatusCode::would_block);
    std::array<std::byte, 10> some{};
    const auto read = table.read(master, some); CHECK(read && read.value().bytes == 10);
    CHECK(read.value().change.wakes & wake_master_read && !terminal->ldisc.echo_pending());
    CHECK(table.flush(slave, false, true).value().changed && !table.input_pending(master).value());
    CHECK(table.flush(master, false, true).value().changed && !terminal->ldisc.buffered());
    // Last slave close: master drains then EIO with HUP; input and session stay (R18).
    CHECK(table.write(master, text("kept\n")) && table.write(slave, text("s")));
    static_cast<void>(drain(table, master)); CHECK(table.write(slave, text("t")));
    CHECK(table.retire(slave)); CHECK(table.retire(slave).value().slave_retired);
    ready = table.readiness(master).value();
    CHECK(ready.readable && ready.hangup && ready.writable && !ready.write_error);
    CHECK(drain(table, master) == "t" && table.read(master, scratch).status().code() == StatusCode::io_error);
    // The master still writes into the closed slave's input; its echo drains, then EIO again.
    CHECK(table.write(master, text("x\n")).value().bytes == 2 && drain(table, master) == "x\r\n");
    CHECK(table.read(master, scratch).status().code() == StatusCode::io_error);
    CHECK(terminal->ldisc.buffered() == 7 && terminal->session == 7 && table.audit().cleared_outside_live_bytes);
    // Relocking refuses new opens only; unlocked, the slave reopens with its input.
    CHECK(table.set_locked(master, true)); CHECK(table.admit_open(slave).code() == StatusCode::io_error);
    CHECK(table.set_locked(master, false)); CHECK(table.attach(slave).changed);
    CHECK(!table.readiness(master).value().hangup && drain(table, slave) == "kept\n" && drain(table, slave) == "x\n");
    // The master's last close hangs the slave up: input discarded, reads 0,
    // writes EIO, poll IN|OUT|ERR|HUP, no new opens.
    CHECK(table.write(master, text("lost\n")) && table.write(master, text("partial")));
    const auto hung = table.retire(master); CHECK(hung && hung.value().master_retired && !hung.value().pair_freed);
    CHECK(terminal->hung_up && !terminal->ldisc.buffered() && !terminal->ldisc.echo_pending());
    CHECK(table.read(slave, scratch).value().bytes == 0);
    CHECK(table.write(slave, text("w")).status().code() == StatusCode::io_error);
    ready = table.readiness(slave).value();
    CHECK(ready.readable && ready.writable && ready.hangup && ready.error && ready.write_error);
    CHECK(table.admit_open(slave).code() == StatusCode::io_error);
    CHECK(table.audit().cleared_outside_live_bytes);
    CHECK(table.retire(slave).value().pair_freed); empty(table);
    CHECK(!table.terminal(slave.slot, slave.generation));
    // A master closed while its slave is dormant frees the slot at once.
    const auto dormant = table.create_multiplexed(); CHECK(dormant);
    CHECK(table.retire(dormant.value()).value().pair_freed); empty(table);
    // The open count is bounded by the u16 counter.
    const auto counted = table.create_multiplexed(); CHECK(counted && table.set_locked(counted.value(), false));
    const auto counted_slave = table.slave_handle(counted.value().slot);
    for (std::size_t open = 0; open < max_endpoint_opens; ++open) CHECK(table.attach(counted_slave).changed);
    CHECK(table.admit_open(counted_slave).code() == StatusCode::overflow);
    CHECK(table.open_count(counted_slave) == max_endpoint_opens);
    for (std::size_t open = 0; open < max_endpoint_opens; ++open) CHECK(table.retire(counted_slave));
    CHECK(table.retire(counted.value()).value().pair_freed); empty(table);
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
    run("raw-termios-and-open-counts", raw_termios_and_open_counts);
    run("multiplexed-terminal-semantics", multiplexed_terminal_semantics);
    run("slave-vmin-nonblocking-and-drain", [] { slave_vmin_nonblocking_and_drain(0); });
    run("slave-vtime-nonblocking-and-drain", [] { slave_vmin_nonblocking_and_drain(5); });
    run("independent-deque-model-100000-steps", deterministic_model_comparison);
    std::printf("PRIVATE_RAW_PTY_OWNER cases=%zu checks=%zu model_steps=100000 open_counts=1 raw_termios=1 "
                "status=passed\n", cases, checks);
}
