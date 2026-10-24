// Hosted transaction validation. Real stack switching is a separate guest gate.
#include "ok/user/native_signal_delivery_x86_64.hpp"
#include "ok/user/native_signal_delivery_aarch64.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
using namespace ok;
using namespace ok::user::native;
static usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::abort(); } } while (0)
constexpr u64 begin = 0x10000000, end = begin + 65536;
template <typename Delivery, bool Aarch64>
struct Fixture
{
    using Frame = typename Delivery::Frame;
    using Queue = sched::PendingSignals<64>;
    Queue pending;
    sched::SignalActionTable actions;
    sched::SignalMask mask;
    sched::SignalAltstack stack{begin + 8192, 32768, 0};
    arch::execution::UserFrame frame{};
    std::array<std::byte, 65536> memory{};
    usize writes{}, fail_at{};
    u32 signal{};
    Fixture()
    {
        CHECK(pending.reset({1, 2, 3}));
        frame.pc = begin + 128; frame.sp = end - 16;
        frame.flags = Aarch64 ? 0x20000000 : 0x202;
        if constexpr (!Aarch64) {
            const auto fp = Frame::reset_fp();
            for (usize i = 0; i < fp.size(); ++i) frame.fp_state[i] = static_cast<u8>(fp[i]);
        }
        for (u32 sig : {10U, 12U})
            *actions.find(sig) = {.handler = begin + 256, .flags = Delivery::restorer | Delivery::onstack | Delivery::siginfo,
                                 .restorer = begin + 512};
    }
    void queue(u32 sig = 10) { CHECK(pending.enqueue({sig, -6, 1, 2}) == Queue::Enqueue::queued); }
    auto deliver()
    {
        return Delivery::deliver(pending, actions, mask, frame, stack, begin, end, 0xffbf,
            [&](u64 at, std::span<const std::byte> data) {
                CHECK(at >= begin && at < end && data.size() <= end - at);
                ++writes;
                const bool fail = fail_at == writes;
                std::copy_n(data.begin(), fail ? 1 : data.size(), memory.begin() + at - begin);
                return !fail;
            }, signal);
    }
    bool restore(typename Frame::Restored &out)
    {
        auto read = [&](u64 at, std::span<std::byte> data) {
            if (at < begin || at >= end || data.size() > end - at) return false;
            std::copy_n(memory.begin() + at - begin, data.size(), data.begin()); return true;
        };
        auto raw = [](typename Frame::Stack value, typename Frame::Stack &out) noexcept { out = value; return true; };
        if constexpr (Aarch64) return Frame::decode(frame.sp, frame, begin, end, read, out, raw);
        else return Frame::decode(frame.sp + 8, frame, begin, end, 0xffbf, read, out, raw);
    }
};
template <typename Engine, bool Aarch64>
void exercise()
{
    using F = Fixture<Engine, Aarch64>;
    using R = typename Engine::Result;
    for (bool automatic : {false, true}) for (bool selected : {false, true}) {
        F f;
        if (automatic) f.stack.flags = sched::SignalAltstack::autodisarm;
        if (!selected) f.actions.find(10)->flags &= ~Engine::onstack;
        const auto original = f.frame;
        const auto old_stack = f.stack;
        f.queue(); CHECK(f.deliver() == R::handler);
        CHECK(old_stack.physically_contains(f.frame.sp) == selected);
        CHECK(f.stack == (automatic ? sched::SignalAltstack{} : old_stack));
        typename F::Frame::Restored saved;
        CHECK(f.restore(saved));
        CHECK(saved.frame.sp == original.sp && saved.frame.pc == original.pc);
        CHECK(saved.stack.pointer == old_stack.base && saved.stack.size == old_stack.size && saved.stack.flags == old_stack.flags);
        auto valid = [](u64) noexcept { return true; };
        constexpr auto profile = Aarch64 ? abi::linux_user::Profile::aarch64 : abi::linux_user::Profile::x86_64;
        apply_restored_altstack<profile>(f.stack, {saved.stack.pointer, saved.stack.size, saved.stack.flags}, saved.frame.sp, valid);
        CHECK(f.stack == old_stack);
    }
    {
        F f; f.queue(); CHECK(f.deliver() == R::handler);
        const auto outer = f.frame;
        f.queue(12); CHECK(f.deliver() == R::handler);
        CHECK(f.stack.physically_contains(f.frame.sp) && f.frame.sp < outer.sp);
        typename F::Frame::Restored saved;
        CHECK(f.restore(saved) && saved.frame.sp == outer.sp);
    }
    // A shallow alternate region must fail before touching user memory.
    for (u64 bad_sp : {u64{0}, u64{64}, begin - 16, end + 16, ~u64{0} - 15}) {
        F f; f.frame.sp = bad_sp; f.queue();
        CHECK(f.deliver() == R::handler && f.stack.physically_contains(f.frame.sp));
        typename F::Frame::Restored saved;
        CHECK(!f.restore(saved)); // A handler must repair SP before returning.
        constexpr usize sp_offset = Aarch64 ? 560 : 168;
        const auto at = static_cast<usize>(f.frame.sp - begin) + sp_offset;
        u64 captured{};
        for (usize i = 0; i < 8; ++i) captured |= static_cast<u64>(f.memory[at + i]) << (8 * i);
        CHECK(captured == bad_sp);
        for (usize i = 0; i < 8; ++i) f.memory[at + i] = std::byte((end - 16) >> (8 * i));
        CHECK(f.restore(saved) && saved.frame.sp == end - 16);
    }
    for (u64 size : {u64{1}, u64{128}, u64{512}}) {
        F f; f.stack.size = size; const auto old = f.frame; f.queue();
        CHECK(f.deliver() == R::terminate && f.signal == 11 && f.writes == 0);
        CHECK(f.frame.sp == old.sp && f.pending.pending_mask().contains(10));
    }
    for (u64 base : {end, ~u64{0} - 16}) {
        F f; f.stack.base = base; f.queue(); CHECK(f.deliver() == R::terminate && f.writes == 0);
    }
    // An interrupted handler near the bottom cannot spill onto ordinary pages.
    {
        F f; f.frame.sp = f.stack.base + 16; f.actions.find(10)->flags &= ~Engine::onstack;
        f.queue(); CHECK(f.deliver() == R::terminate && f.writes == 0);
    }
    // Partial bytes may remain, but pending/action/context/disarm commit must not.
    for (usize call = 1; call <= (Aarch64 ? 1U : 2U); ++call) {
        F f; f.fail_at = call; f.stack.flags = sched::SignalAltstack::autodisarm;
        f.actions.find(10)->flags |= Engine::resethand;
        const auto old = f.frame; const auto stack = f.stack; const auto action = *f.actions.find(10);
        f.queue(); CHECK(f.deliver() == R::terminate && f.signal == 11 && f.writes == call);
        CHECK(f.frame.sp == old.sp && f.frame.pc == old.pc && f.stack == stack && *f.actions.find(10) == action);
        CHECK(f.mask == sched::SignalMask{} && f.pending.pending_mask().contains(10));
    }
    // Semantic uc_stack errors are not malformed register/FP frames. They must
    // preserve the current stack after an otherwise successful staged decode.
    for (bool automatic : {false, true}) for (u32 flags : {0U, 3U, 0x40000000U}) {
        F f;
        if (automatic) f.stack.flags = sched::SignalAltstack::autodisarm;
        f.queue(); CHECK(f.deliver() == R::handler);
        const auto committed = f.stack;
        constexpr usize offset = Aarch64 ? 144 : 24;
        const auto at = static_cast<usize>(f.frame.sp - begin) + offset;
        auto put = [&](usize where, u64 value, usize bytes) {
            for (usize i = 0; i < bytes; ++i) f.memory[where + i] = std::byte(value >> (i * 8));
        };
        put(at, begin + 8192, 8); put(at + 8, flags, 4); put(at + 16, 1, 8);
        typename F::Frame::Restored saved;
        CHECK(f.restore(saved) && saved.stack.flags == flags && saved.stack.size == 1);
        constexpr auto profile = Aarch64 ? abi::linux_user::Profile::aarch64 : abi::linux_user::Profile::x86_64;
        apply_restored_altstack<profile>(f.stack, {saved.stack.pointer, saved.stack.size, saved.stack.flags},
                                       saved.frame.sp, [](u64) noexcept { return true; });
        CHECK(f.stack == committed);
    }
    // Entry admission fails before queue/mask/action/context publication even
    // if prepared user-frame bytes were already written successfully.
    for (bool admitted : {false, true}) {
        F f; f.stack.flags = sched::SignalAltstack::autodisarm;
        f.actions.find(10)->flags |= Engine::resethand;
        f.queue(); const auto stack=f.stack; const auto old=f.frame;
        const auto action=*f.actions.find(10); unsigned validations=0, writes=0;
        auto result=Engine::deliver(f.pending, f.actions, f.mask, f.frame, f.stack, begin, end, 0xffbf,
            [&](u64, auto) { ++writes; return true; }, f.signal, 0,
            [&](const arch::execution::UserFrame &staged) {
                ++validations; CHECK(writes > 0 && staged.pc == action.handler);
                CHECK(staged.sp != old.sp && stack.physically_contains(staged.sp));
                return admitted;
            });
        CHECK(validations == 1);
        if (admitted) {
            CHECK(result == R::handler && !f.pending.pending_mask().contains(10));
            CHECK(f.mask.contains(10) && f.stack == sched::SignalAltstack{} && f.actions.find(10)->handler == 0);
        } else {
            CHECK(result == R::terminate && f.signal == 11 && f.pending.pending_mask().contains(10));
            CHECK(f.mask == sched::SignalMask{} && f.stack == stack && *f.actions.find(10) == action);
            CHECK(f.frame.sp == old.sp && f.frame.pc == old.pc);
        }
    }
    // A failed queue commit after successful writes still cannot disarm state.
    {
        F f; f.stack.flags = sched::SignalAltstack::autodisarm;
        const auto stack = f.stack; const auto old = f.frame;
        struct RejectCommit {
            using Delivery = typename F::Queue::Delivery;
            typename F::Queue queue;
            bool peek(const sched::SignalMask &mask, Delivery &ticket) { return queue.peek(mask, ticket); }
            bool consume(const Delivery &) { return false; }
        } pending;
        CHECK(pending.queue.reset({1, 2, 3}));
        CHECK(pending.queue.enqueue({10, -6, 1, 2}) == F::Queue::Enqueue::queued);
        CHECK(Engine::deliver(pending, f.actions, f.mask, f.frame, f.stack, begin, end, 0xffbf,
                                [](u64, auto) { return true; }, f.signal) == R::corrupt);
        CHECK(f.stack == stack && f.frame.sp == old.sp && pending.queue.pending_mask().contains(10));
    }
}
int main()
{
    exercise<X86SignalDelivery, false>();
    exercise<Aarch64SignalDelivery, true>();
    std::printf("PASS alternate-stack delivery checks=%zu hosted=1 guest=0 smp=0\n", checks);
}
