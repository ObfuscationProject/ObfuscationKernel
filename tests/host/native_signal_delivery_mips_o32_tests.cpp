#include "ok/arch/mips/signal.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
using namespace ok;
namespace n = ok::user::native;
using D = n::MipsO32SignalDelivery;
using R = n::MipsO32SignalReturn;
using F = arch::execution::UserFrame;
using Q = sched::PendingSignals<128>;
using K = n::MipsO32SignalFormat;
using Context = n::MipsO32SignalContext;
constexpr u64 lo = 0x40000000, hi = 0x80000000, code = hi - 4096;
constexpr Context verified{
    Context::FpContract::eager_fr0_legacy_nan_no_extensions,
    Context::ResumeContract::normalized_resume_pc};
static unsigned checks;
#define CHECK(x)                                                               \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                     \
      std::abort();                                                            \
    }                                                                          \
  } while (false)
bool same(const F &a, const F &b) {
  return a.registers == b.registers && a.pc == b.pc && a.sp == b.sp &&
         a.flags == b.flags && a.fs_base == b.fs_base &&
         a.gs_base == b.gs_base && a.fp_state == b.fp_state;
}
template <K Kind> struct Fixture {
  using C = n::MipsO32SignalFrame<Kind>;
  static constexpr auto operation =
      C::realtime ? R::Operation::rt_sigreturn : R::Operation::sigreturn;
  Q queue;
  sched::SignalActionTable actions;
  sched::SignalMask mask;
  sched::SignalAltstack stack{0x60000000, 16384,
                              sched::SignalAltstack::autodisarm};
  F frame{};
  Context context = verified;
  typename C::Image image;
  u64 address{};
  unsigned writes{}, reads{}, admissions{};
  bool admit{true};
  usize read_cut{~usize{0}}, write_cut{~usize{0}};
  Fixture() {
    frame.pc = lo + 0x2000;
    frame.sp = 0x70004000;
    frame.flags = 0x20008013;
    frame.fs_base = lo + 0x10000;
    frame.gs_base = 0x1234;
    for (usize i = 1; i < 32; ++i) {
      frame.registers[i] = 0x10000 + i;
    }
    frame.registers[29] = frame.sp;
    for (usize i = 0; i < frame.fp_state.size(); ++i)
      frame.fp_state[i] = static_cast<u8>(i * 7);
    C::set_backend_word(frame, 256, 0);
    auto &a = *actions.find(16);
    a.handler = lo + 0x4000;
    a.flags = C::realtime ? D::siginfo : 0;
    a.mask.set(67);
    a.mask.set(9);
    a.mask.set(23);
    mask.set(95);
    actions.find(8)->handler = lo + 0x5000;
    actions.find(8)->flags = D::siginfo;
    CHECK(queue.reset({1, 2, 3}));
    CHECK(queue.enqueue({16, -6, 7, 8}) == Q::Enqueue::queued);
  }
  D::Result deliver(u32 &signal) {
    return D::deliver(
        queue, actions, mask, frame, stack, lo, hi, context,
        [&](u64 at, std::span<const std::byte> b) {
          ++writes;
          address = at;
          CHECK(b.size() == C::frame_bytes);
          std::copy_n(b.begin(), std::min(write_cut, b.size()),
                      image.bytes.begin());
          return write_cut >= b.size();
        },
        signal, code,
        [&](const F &) {
          ++admissions;
          return admit;
        });
  }
  void delivered() {
    actions.find(16)->flags |= D::onstack;
    u32 signal{};
    CHECK(deliver(signal) == D::Result::handler && signal == 16);
    writes = reads = admissions = 0;
  }
  bool restore() {
    const auto f = frame;
    const auto a = actions;
    const auto m = mask;
    const auto s = stack;
    const auto pending = queue.pending_mask();
    auto unchanged = [&] {
      CHECK(same(frame, f) && actions == a && mask == m && stack == s &&
            queue.pending_mask() == pending);
    };
    return R::restore(
        operation, queue, actions, mask, frame, stack, context, lo, hi,
        [&](u64 at, std::span<std::byte> b) {
          ++reads;
          unchanged();
          CHECK(at == address && b.size() == C::frame_bytes);
          std::copy_n(image.bytes.begin(), std::min(read_cut, b.size()),
                      b.begin());
          return read_cut >= b.size();
        },
        [&](u64 at, std::span<const std::byte> b) {
          ++writes;
          unchanged();
          CHECK(admissions == 1 && at == address + C::fcsr_offset &&
                b.size() == 4);
          std::copy_n(b.begin(), std::min(write_cut, b.size()),
                      image.bytes.begin() + C::fcsr_offset);
          return write_cut >= b.size();
        },
        [&](const F &) {
          ++admissions;
          unchanged();
          CHECK(writes == 0);
          return admit;
        },
        [](u64) noexcept { return true; });
  }
};
template <K Kind> void transactions() {
  using C = n::MipsO32SignalFrame<Kind>;
  for (unsigned flags = 0; flags < 16; ++flags) {
    Fixture<Kind> x;
    auto &a = *x.actions.find(16);
    a.flags |= (flags & 1 ? D::nodefer : 0) | (flags & 2 ? D::resethand : 0) |
               (flags & 4 ? D::onstack : 0);
    x.stack.flags = flags & 8 ? sched::SignalAltstack::autodisarm : 0;
    const auto f = x.frame;
    const auto s = x.stack;
    u32 signal{};
    CHECK(x.deliver(signal) == D::Result::handler && signal == 16);
    const u64 top = flags & 4 ? s.base + s.size : f.sp;
    CHECK(x.address == top - C::frame_bytes - (flags & 4 ? 0 : 32));
    CHECK(x.frame.pc == a.handler || flags & 2);
    CHECK(x.frame.registers[25] == lo + 0x4000 &&
          x.frame.registers[31] == code + (C::realtime ? 0 : 16));
    CHECK(x.frame.sp == x.address && x.frame.registers[29] == x.address &&
          x.frame.registers[4] == 16 &&
          x.frame.registers[5] == (C::realtime ? x.address + 24 : 0) &&
          x.frame.registers[6] == x.address + (C::realtime ? 152 : 24));
    CHECK(x.mask.contains(16) == !(flags & 1));
    CHECK(x.mask.contains(67) && x.mask.contains(95) && !x.mask.contains(9) &&
          !x.mask.contains(23));
    const auto after = x.stack;
    x.writes = x.admissions = 0;
    CHECK(x.restore());
    CHECK(same(x.frame, f) && x.stack == (C::realtime ? s : after) &&
          x.mask.contains(95) && !x.mask.contains(67) && !x.mask.contains(16) &&
          x.writes == 0);
  }
  for (usize cut = 0; cut < C::frame_bytes; ++cut) {
    Fixture<Kind> x;
    x.write_cut = cut;
    x.actions.find(16)->flags |= D::resethand | D::onstack;
    const auto f = x.frame;
    const auto a = x.actions;
    const auto m = x.mask;
    const auto s = x.stack;
    u32 signal{};
    CHECK(x.deliver(signal) == D::Result::terminate && signal == 11);
    CHECK(same(x.frame, f) && x.actions == a && x.mask == m && x.stack == s &&
          x.queue.pending_mask().contains(16) && !x.admissions);
  }
  for (usize cut = 0; cut < C::frame_bytes; ++cut) {
    Fixture<Kind> x;
    x.delivered();
    x.read_cut = cut;
    C::put(x.image.bytes, C::fcsr_offset, 0x21080, 4);
    const auto f = x.frame;
    const auto a = x.actions;
    const auto m = x.mask;
    const auto s = x.stack;
    CHECK(!x.restore() && !x.writes && !x.admissions);
    CHECK(same(x.frame, f) && x.actions == a && x.mask == m && x.stack == s &&
          !x.queue.pending_mask().contains(8));
  }
  for (unsigned invalid = 0; invalid < 5; ++invalid) {
    Fixture<Kind> x;
    if (invalid == 0)
      x.context = {};
    if (invalid == 1)
      x.context.fp = Context::FpContract::unverified;
    if (invalid == 2)
      x.context.resume = Context::ResumeContract::unverified;
    if (invalid == 3)
      x.frame.flags |= 0x04000000;
    if (invalid == 4)
      x.frame.flags &= ~0x20000000;
    u32 signal{};
    CHECK(x.deliver(signal) == D::Result::terminate && signal == 11 &&
          !x.writes && x.queue.pending_mask().contains(16));
  }
  for (u64 sp : {u64{0}, lo - 1, u64{0xffffffff}}) {
    Fixture<Kind> x;
    x.frame.sp = sp;
    x.delivered();
    CHECK(C::get(x.image.bytes, C::registers_offset + 29 * 8, 8) == sp);
  }
  // The pre-gap SP controls sigsp selection at the alternate-stack upper edge.
  for (unsigned over = 0; over <= 40; over += 8) {
    Fixture<Kind> x;
    x.stack.flags = 0;
    x.frame.sp = x.stack.base + x.stack.size + over;
    x.actions.find(16)->flags |= D::onstack;
    const u64 original = x.frame.sp;
    u32 signal{};
    CHECK(x.deliver(signal) == D::Result::handler);
    CHECK(x.address == (over <= 32
                            ? original - 32 - C::frame_bytes
                            : x.stack.base + x.stack.size - C::frame_bytes));
  }
  for (unsigned gap = 0; gap <= 40; gap += 8) {
    Fixture<Kind> x;
    x.stack.flags = 0;
    x.frame.sp = x.stack.base + C::frame_bytes + gap;
    u32 signal{};
    CHECK(x.deliver(signal) ==
          (gap <= 32 ? D::Result::terminate : D::Result::handler));
  }
}
template <K Kind> void returns() {
  using C = n::MipsO32SignalFrame<Kind>;
  for (u32 enable = 0; enable < 32; ++enable)
    for (u32 cause = 0; cause < 64; ++cause) {
      Fixture<Kind> x;
      x.delivered();
      const u32 csr = (enable << 7) | (cause << 12) | 0x80000043U;
      C::put(x.image.bytes, C::fcsr_offset, csr, 4);
      C::put(x.image.bytes, C::registers_offset + 2 * 8, 0xabcdef0098765432ULL,
             8);
      C::put(x.image.bytes, C::registers_offset + 7 * 8, 0x1122334400000001ULL,
             8);
      const bool pending = (cause & 32) || ((cause & enable) != 0);
      const u32 expected = pending ? csr & ~(0x20000U | (enable << 12)) : csr;
      CHECK(x.restore() && x.writes == unsigned(pending));
      CHECK(C::backend_word(x.frame, 256) == expected &&
            C::get(x.image.bytes, C::fcsr_offset, 4) == expected &&
            x.frame.registers[2] == 0x98765432U && x.frame.registers[7] == 1);
      CHECK(x.queue.pending_mask().contains(8) == pending);
      if (pending) {
        Q::Delivery ticket;
        CHECK(x.queue.peek(x.mask, ticket));
        CHECK((ticket.info == sched::PendingSignalInfo{8, 128, 0, 0}));
      }
    }
  for (usize cut = 0; cut < 4; ++cut) {
    Fixture<Kind> x;
    x.delivered();
    C::put(x.image.bytes, C::fcsr_offset, 0x31080, 4);
    x.write_cut = cut;
    const auto f = x.frame;
    const auto a = x.actions;
    const auto m = x.mask;
    const auto s = x.stack;
    CHECK(!x.restore() && x.writes == 1 && x.admissions == 1);
    CHECK(same(x.frame, f) && x.actions == a && x.mask == m && x.stack == s &&
          !x.queue.pending_mask().contains(8));
  }
  for (unsigned blocked = 0; blocked < 2; ++blocked)
    for (unsigned handler = 0; handler < 3; ++handler)
      for (unsigned coalesced = 0; coalesced < 2; ++coalesced) {
        Fixture<Kind> x;
        x.delivered();
        C::put(x.image.bytes, C::fcsr_offset, 0x20000, 4);
        auto &a = *x.actions.find(8);
        a.handler = handler == 2 ? lo + 0x5000 : handler;
        a.flags = D::nodefer;
        a.mask.set(77);
        const auto old = a;
        if (blocked) {
          sched::SignalMask m;
          m.set(8);
          m.set(95);
          auto bytes = C::MaskCodec::encode(m);
          std::copy(bytes.begin(), bytes.end(),
                    x.image.bytes.begin() + C::mask_offset);
        }
        Q::Delivery previous;
        if (coalesced) {
          CHECK(x.queue.enqueue({8, -6, 42, 43}) == Q::Enqueue::queued);
          CHECK(x.queue.peek({}, previous));
        }
        CHECK(x.restore() && !x.mask.contains(8) && x.mask.contains(95));
        auto expected = old;
        if (blocked || handler == 1)
          expected.handler = 0;
        CHECK(a == expected);
        Q::Delivery next;
        CHECK(x.queue.peek(x.mask, next));
        if (coalesced)
          CHECK(next == previous);
        else
          CHECK((next.info == sched::PendingSignalInfo{8, 128, 0, 0}));
      }
  for (unsigned invalid = 0; invalid < 5; ++invalid) {
    Fixture<Kind> x;
    x.delivered();
    C::put(x.image.bytes, C::fcsr_offset, 0x20000, 4);
    if (invalid == 0)
      x.context = {};
    if (invalid == 1)
      x.admit = false;
    if (invalid == 2)
      C::put(x.image.bytes, C::pc_offset, lo + 1, 8);
    if (invalid == 3)
      C::put(x.image.bytes, C::used_math_offset, 0, 4);
    if (invalid == 4)
      C::put(x.image.bytes, C::fcsr_offset, 0x7e0000, 4);
    const auto f = x.frame;
    const auto a = x.actions;
    const auto m = x.mask;
    const auto s = x.stack;
    CHECK(!x.restore() && !x.writes);
    CHECK(same(x.frame, f) && x.actions == a && x.mask == m && x.stack == s &&
          !x.queue.pending_mask().contains(8));
  }
  for (bool owned : {false, true}) {
    Fixture<Kind> x;
    x.delivered();
    C::put(x.image.bytes, C::fcsr_offset, 0x20000, 4);
    sched::PendingSignals<128, 1, 1> limited;
    if (owned) {
      CHECK(limited.reset({1, 2, 3}));
      CHECK(limited.enqueue({16, 0, 0, 0}) ==
            decltype(limited)::Enqueue::queued);
    }
    const auto f = x.frame;
    const auto a = x.actions;
    const auto m = x.mask;
    const auto s = x.stack;
    unsigned writes = 0;
    CHECK(!R::restore(
        x.operation, limited, x.actions, x.mask, x.frame, x.stack, verified, lo,
        hi,
        [&](u64, std::span<std::byte> b) {
          std::copy(x.image.bytes.begin(), x.image.bytes.end(), b.begin());
          return true;
        },
        [&](u64, std::span<const std::byte> b) {
          ++writes;
          CHECK(b.size() == 4 && b[0] == std::byte{0} && b[1] == std::byte{0} &&
                b[2] == std::byte{0} && b[3] == std::byte{0});
          return true;
        },
        [](const F &) { return true; }, [](u64) noexcept { return true; }));
    CHECK(writes == 1 && same(x.frame, f) && x.actions == a && x.mask == m &&
          x.stack == s && !limited.pending_mask().contains(8));
  }
  for (i32 sig = 1; sig <= 128; ++sig) {
    CHECK(C::valid_info({sig, 128, 0, 0}) == (sig == 8));
    CHECK(!C::valid_info({sig, 128, 1, 0}) && !C::valid_info({sig, 128, 0, 1}));
  }
}

template <K Kind> void policy_edges() {
  using C = n::MipsO32SignalFrame<Kind>;
  {
    Fixture<Kind> x;
    x.admit = false;
    x.actions.find(16)->flags |= D::resethand | D::onstack;
    const auto before = x.frame;
    const auto mask = x.mask;
    const auto stack = x.stack;
    const auto actions = x.actions;
    u32 signal{};
    CHECK(x.deliver(signal) == D::Result::terminate && signal == 11 &&
          x.writes == 1 && x.admissions == 1);
    CHECK(same(x.frame, before) && x.mask == mask && x.stack == stack &&
          x.actions == actions && x.queue.pending_mask().contains(16));
  }
  {
    Fixture<Kind> x;
    x.actions.find(16)->flags |= 1;
    u32 signal{};
    CHECK(x.deliver(signal) == D::Result::unsupported && !x.writes &&
          x.queue.pending_mask().contains(16));
  }
  {
    Fixture<Kind> x;
    x.delivered();
    unsigned calls{};
    CHECK(!R::restore(
        R::Operation::read, x.queue, x.actions, x.mask, x.frame, x.stack,
        verified, lo, hi,
        [&](u64, std::span<std::byte>) {
          ++calls;
          return true;
        },
        [&](u64, std::span<const std::byte>) {
          ++calls;
          return true;
        },
        [&](const F &) {
          ++calls;
          return true;
        },
        [](u64) noexcept { return true; }));
    CHECK(calls == 0);
  }
  // Raw altstack metadata is not dereferenced by sigreturn. Invalid semantic
  // requests leave existing state intact without rejecting restored registers.
  for (unsigned choice = 0; choice < 6; ++choice) {
    Fixture<Kind> x;
    x.delivered();
    const auto old = x.stack;
    if constexpr (C::realtime) {
      const u32 flags = choice == 0   ? 3
                        : choice == 1 ? 0
                        : choice == 2 ? 1
                        : choice == 3 ? 2
                        : choice == 4 ? 0x80000000U
                                      : 0;
      const u32 size = choice == 1 ? 1 : 4096;
      C::put(x.image.bytes, C::stack_offset, 0xfffff000, 4);
      C::put(x.image.bytes, C::stack_offset + 4, size, 4);
      C::put(x.image.bytes, C::stack_offset + 8, flags, 4);
      if (choice == 5)
        x.stack = {0x70000000, 0x8000, 0};
      const auto before = x.stack;
      CHECK(x.restore());
      if (choice <= 1 || choice == 5)
        CHECK(x.stack == before);
      else if (choice == 3)
        CHECK((x.stack == sched::SignalAltstack{0, 0, 2}));
      else
        CHECK((x.stack == sched::SignalAltstack{0xfffff000, 4096, flags}));
    } else {
      CHECK(x.restore() && x.stack == old);
    }
  }
  // A forced kernel-origin FPE must remain encodable by either delivery format.
  {
    Fixture<Kind> x;
    x.delivered();
    C::put(x.image.bytes, C::fcsr_offset, 0x20000, 4);
    CHECK(x.restore());
    x.actions.find(8)->flags = C::realtime ? D::siginfo : 0;
    x.writes = x.admissions = 0;
    u32 signal{};
    CHECK(x.deliver(signal) == D::Result::handler && signal == 8 &&
          x.writes == 1 && x.admissions == 1);
    CHECK(!x.queue.pending_mask().contains(8));
    if constexpr (C::realtime) {
      CHECK(C::get(x.image.bytes, C::info_offset, 4) == 8 &&
            C::get(x.image.bytes, C::info_offset + 4, 4) == 128);
      for (usize i = 8; i < 128; ++i)
        CHECK(x.image.bytes[C::info_offset + i] == std::byte{0});
    }
  }
}

int main() {
  policy_edges<K::legacy>();
  policy_edges<K::realtime>();
  transactions<K::legacy>();
  transactions<K::realtime>();
  returns<K::legacy>();
  returns<K::realtime>();
  std::printf("PASS MIPS o32 policy: %u checks\n", checks);
}
