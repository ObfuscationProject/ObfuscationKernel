// Hosted byte/queue models, not native child exit or hardware execution.
#include "ok/user/native_signal_info.hpp"
#include "../../src/user/signals/process_pending.hpp"
#include "../../src/user/signals/wait_codec.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::user::native;
using Profile = abi::linux_user::Profile;
static unsigned checks;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "line=%d check=%s\n", __LINE__, #value); std::abort(); } } while (false)

template <Profile P, bool Wide, bool Mips, bool Big>
void profile()
{
    using Codec = SignalInfoCodec<P>;
    constexpr u32 maximum = Mips ? 128 : 64, child = Mips ? 18 : 17;
    constexpr usize payload = Wide ? 16 : 12, code_at = Mips ? 4 : 8;
    auto expected = [&](sched::PendingSignalInfo info) {
        std::array<std::byte, 128> value{};
        auto put = [&](usize at, u32 word) {
            for (usize byte = 0; byte != 4; ++byte)
                value[at + byte] = static_cast<std::byte>(word >> ((Big ? 3 - byte : byte) * 8));
        };
        put(0, info.signal); put(code_at, static_cast<u32>(info.code));
        put(payload, info.sender_pid); put(payload + 4, info.sender_uid);
        if (info.code == 1 || info.code == 2) put(payload + 8, static_cast<u32>(info.child_status));
        return value;
    };
    for (sched::PendingSignalInfo info : {sched::PendingSignalInfo{child, 1, 0x12345678, 0xfedcba98, 0},
        {child, 1, 0x12345678, 0xfedcba98, 165}, {child, 1, 0x12345678, 0xfedcba98, 255},
        {child, 2, 0x12345678, 0xfedcba98, 1}, {child, 2, 0x12345678, 0xfedcba98, static_cast<i32>(maximum)},
        {child, 0, 0x12345678, 0xfedcba98}, {8, -6, 0x12345678, 0xfedcba98}, {8, 128, 0, 0}, {maximum, 128, 0, 0}})
    {
        std::array<std::byte, 128> out{}, waited{}; out.fill(std::byte{0x5a});
        CHECK(Codec::encode(info, out)); CHECK(out == expected(info));
        CHECK(SignalWaitCodec<P>::encode_info(info, waited)); CHECK(waited == out);
        CHECK(Codec::template frame_valid<>(info) == (info.code != 128));
        CHECK(Codec::template frame_valid<true>(info) == (info.code != 128 || info.signal == 8));
        std::array<std::byte, 131> surrounded{}; surrounded.fill(std::byte{0xa5});
        CHECK(Codec::encode_into(info, surrounded, 1));
        CHECK(surrounded[0] == std::byte{0xa5} && surrounded[129] == std::byte{0xa5} && surrounded[130] == std::byte{0xa5});
        for (usize i = 0; i != 128; ++i) CHECK(surrounded[i + 1] == out[i]);
        const auto before = surrounded;
        CHECK(!Codec::encode_into(info, surrounded, 4)); CHECK(surrounded == before);
        CHECK(!Codec::encode_into(info, surrounded, ~usize{0})); CHECK(surrounded == before);
    }
    for (sched::PendingSignalInfo info : {sched::PendingSignalInfo{0, 1, 1, 0, 0},
        {maximum + 1, 1, 1, 0, 0}, {child + 1, 1, 1, 0, 0}, {child, 3, 1, 0, 0},
        {child, 1, 0, 0, 0}, {child, 1, 0x80000000, 0, 0}, {child, 1, 1, 0, -1},
        {child, 1, 1, 0, 256}, {child, 2, 1, 0, 0}, {child, 2, 1, 0, static_cast<i32>(maximum + 1)},
        {child, 0, 1, 0, 1}, {child, -6, 1, 0, 1}, {8, 128, 1, 0, 0}, {8, 128, 0, 1, 0},
        {8, 128, 0, 0, 1}, {child, -1, 1, 0, 0}})
    {
        std::array<std::byte, 128> out{}; out.fill(std::byte{0x5a}); const auto before = out;
        CHECK(!Codec::valid(info)); CHECK(!Codec::encode(info, out)); CHECK(out == before);
        CHECK(!SignalWaitCodec<P>::encode_info(info, out)); CHECK(out == before);
    }
    using Q = sched::PendingSignals<maximum, 1, 5>;
    Q queue; typename Q::Delivery ticket;
    CHECK(queue.reset({1, 1, 1}));
    CHECK(queue.enqueue({child, 1, 2, 0, 1}) == Q::Enqueue::invalid);
    CHECK(queue.enqueue({child, 0, 2, 0, 1}) == Q::Enqueue::invalid);
    CHECK(queue.enqueue_child_exit({}, 2, 0) == Q::Enqueue::invalid);
    CHECK(queue.enqueue_child_exit(sched::ChildExitStatus::normal(1), 0, 0) == Q::Enqueue::invalid);
    CHECK(queue.enqueue_child_exit(sched::ChildExitStatus::signaled(0), 2, 0) == Q::Enqueue::invalid);
    CHECK(queue.enqueue_child_exit(sched::ChildExitStatus::signaled(maximum + 1), 2, 0) == Q::Enqueue::invalid);
    CHECK(!queue.peek({}, ticket));
    CHECK(queue.enqueue({32, 0, 9, 0}) == Q::Enqueue::queued);
    CHECK(queue.enqueue_child_exit(sched::ChildExitStatus::normal(-1), 2, 0xffffffff) == Q::Enqueue::queued);
    CHECK(queue.enqueue_child_exit(sched::ChildExitStatus::normal(42), 3, 7) == Q::Enqueue::coalesced);
    CHECK(queue.peek({}, ticket)); CHECK((ticket.info == sched::PendingSignalInfo{child, 1, 2, 0xffffffff, 255}));
    const auto stale = ticket; CHECK(queue.consume(ticket));
    CHECK(queue.enqueue_child_exit(sched::ChildExitStatus::signaled(maximum), 3, 7) == Q::Enqueue::queued);
    CHECK(!queue.consume(stale)); CHECK(queue.peek({}, ticket));
    CHECK((ticket.info == sched::PendingSignalInfo{child, 2, 3, 7, static_cast<i32>(maximum)}));
    CHECK(queue.reset({1, 1, 2})); CHECK(!queue.consume(ticket));
    CHECK(queue.enqueue({child, 0, 7, 9}) == Q::Enqueue::queued);
    CHECK(queue.enqueue_child_exit(sched::ChildExitStatus::normal(2), 3, 7) == Q::Enqueue::coalesced);
    CHECK(queue.peek({}, ticket)); CHECK((ticket.info == sched::PendingSignalInfo{child, 0, 7, 9, 0}));
    queue.discard(child);
    CHECK(queue.enqueue_child_exit(sched::ChildExitStatus::normal(3), 3, 7) == Q::Enqueue::queued);
    CHECK(!queue.consume(ticket)); queue.discard(child);
    CHECK(queue.enqueue_child_exit(sched::ChildExitStatus::normal(3), 3, 7) == Q::Enqueue::full);

    using Shared = ProcessPendingSignals<maximum, 1, 2>;
    Shared shared; typename Shared::Delivery selected;
    CHECK(shared.enqueue_child_exit(sched::ChildExitStatus::normal(1), 2, 0) == Shared::Enqueue::invalid);
    CHECK(shared.reset({1, 1, 3}));
    CHECK(shared.enqueue({32, 0, 7, 9}) == Shared::Enqueue::queued);
    CHECK(shared.enqueue({child, 1, 2, 0, 1}) == Shared::Enqueue::invalid);
    CHECK(shared.enqueue({33, 0, 1, 0, 5}) == Shared::Enqueue::invalid);
    CHECK(shared.enqueue_child_exit(sched::ChildExitStatus::normal(1), 2, 0) == Shared::Enqueue::queued);
    CHECK(shared.enqueue_child_exit(sched::ChildExitStatus::normal(2), 3, 0) == Shared::Enqueue::coalesced);
    CHECK(shared.peek({}, selected)); CHECK(selected.info.child_status == 1); CHECK(!selected.fallback);
    const auto old = selected; CHECK(shared.consume(selected));
    CHECK(shared.enqueue_child_exit(sched::ChildExitStatus::normal(3), 4, 0) == Shared::Enqueue::exhausted);
    CHECK(!shared.pending_mask().contains(child)); shared.retire(); CHECK(!shared.consume(old));
    CHECK(!shared.pending_mask().words[0]);
}

int main()
{
    profile<Profile::i386, false, false, false>();
    profile<Profile::x86_64, true, false, false>();
    profile<Profile::arm32, false, false, false>();
    profile<Profile::aarch64, true, false, false>();
    profile<Profile::rv32, false, false, false>();
    profile<Profile::rv64, true, false, false>();
    profile<Profile::loongarch64, true, false, false>();
    profile<Profile::mips, false, true, true>();
    profile<Profile::mips64, true, true, true>();
    profile<Profile::ppc, false, false, true>();
    std::printf("CHILD_SIGINFO_PASS checks=%u profiles=10 hosted=1 native=0\n", checks);
}
