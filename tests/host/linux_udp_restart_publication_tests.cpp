// Actual UDP producer and typed n64/PPC transaction; hosted backend
// observations.
#define OK_MIPS64_N64_ADMISSION_MODEL 1
#define OK_PPC32_ADMISSION_MODEL 1
#include "linux_udp_restart_capture_fixture.hpp"
#include <fstream>
#include <iterator>
#include <vector>
namespace n64 = hw::mips64_n64_private;
namespace ppc = hw::ppc32_private;
// These existing hosted friends only own the CPU/backend issuers. Every
// observation is emitted inside enter_owned; neither friend can alter an
// origin, admission, opcode, owner store, readiness state or publication
// transaction.
namespace ok::arch::execution::mips64_n64_private::detail {
struct ModelAccess {
private:
  Tracker tracker;

public:
  void qualify() {
    CHECK(tracker.qualify({0x10000, 0x8000c482, 0xfea3519b, 0x80000000, 0x80, 0,
                           0, 0x24000080, 0x007f0000, 0, true, true, true,
                           false, false}));
  }
  auto capability() const { return tracker.capability(); }
  auto query(const UserFrame &frame) const { return tracker.query(frame); }
  void entered(AddressSpace space, UserFrame &frame,
               const KernelEntryStackView &view, Trap &trap) {
    trap.kernel_stack_pointer = view.top() - sizeof(UserFrame) - 16;
    tracker.invalidate();
    tracker.publish_owned(frame, space, view, trap);
  }
};
} // namespace ok::arch::execution::mips64_n64_private::detail
namespace ok::arch::execution::ppc32_private::detail {
struct ModelAccess {
private:
  Tracker tracker;

public:
  void qualify() {
    tracker.qualify_initialized({0x80210022, 0, 0, true});
    CHECK(tracker.capability().admitted());
  }
  auto capability() const { return tracker.capability(); }
  auto query(const UserFrame &frame) const { return tracker.query(frame); }
  void entered(AddressSpace space, UserFrame &frame,
               const KernelEntryStackView &view, Trap &trap) {
    trap.kernel_stack_pointer = view.top() - entry_extent;
    tracker.invalidate();
    tracker.publish_owned(frame, space, view, trap);
  }
};
} // namespace ok::arch::execution::ppc32_private::detail
namespace {
using namespace udp_restart_model;
n64::detail::ModelAccess n64_issuer;
ppc::detail::ModelAccess ppc_issuer;
lu::Profile selected_profile;
hw::AddressSpace selected_space{};
usize commits{}, cancels{}, refusals{}, typed_captures{};
std::vector<std::byte> linux_elf, sysv_elf;
void backend_entered(hw::AddressSpace &space, hw::UserFrame &frame,
                     const hw::KernelEntryStackView &view, hw::Trap &trap) {
  expect(native::BackendCapture::same_space(space, selected_space));
  if (selected_profile == lu::Profile::mips64)
    n64_issuer.entered(space, frame, view, trap);
  else {
    expect(selected_profile == lu::Profile::ppc);
    ppc_issuer.entered(space, frame, view, trap);
  }
}
template <lu::Profile P> class TypedFixture : public Fixture<P> {
  static_assert(P == lu::Profile::mips64 || P == lu::Profile::ppc);
  static constexpr bool is_n64 = P == lu::Profile::mips64;
  using F = Fixture<P>;

public:
  using Pub = native::SyscallRestartPublication<P>;
  using Binding = typename F::Binding;
  using Transaction = typename Pub::template BasicTransaction<Binding>;
  typename Pub::Readiness readiness;
  typename Pub::Identity id{};
  typename F::Origins::Identity captured_owner{};
  hw::UserFrame original{};
  explicit TypedFixture(Kernel &kernel, bool socketcall = false)
      : F(kernel, socketcall) {
    selected_profile = P;
    selected_space = this->process().resources().native_space;
    mock::backend_result_hook = backend_entered;
    const auto &t = this->thread();
    const auto lease = t.entry_stack;
    const auto top = native::entry_stacks.top(lease, {this->pid, this->pid});
    CHECK(top);
    if constexpr (is_n64) {
      id = {reinterpret_cast<uptr>(&native::entry_stacks),
            top.value() - memory::KernelEntryStackPool::usable_bytes,
            top.value(),
            lease.slot,
            this->pid,
            this->pid,
            lease.generation,
            101,
            selected_space};
      readiness.reset(id, cap());
    } else {
      const auto image = native::Ppc32ImageAdmission::inspect(
          cap(), linux_elf, linux_elf.size(),
          user::ppc32_elf_admission::Personality::linux_user,
          user::ppc32_elf_admission::RuntimeProfile::
              e500v2_scalar_soft_float);
      expect(image.valid(cap()));
      const auto rejected = native::Ppc32ImageAdmission::inspect(
          cap(), sysv_elf, sysv_elf.size(),
          user::ppc32_elf_admission::Personality::linux_user,
          user::ppc32_elf_admission::RuntimeProfile::
              e500v2_scalar_soft_float);
      expect(!rejected.valid(cap()));
      id = {selected_space, this->pid, this->pid, 101};
      CHECK(readiness.reset(
          id, image,
          native::Ppc32OwnedLease::inspect(native::entry_stacks, lease,
                                           {this->pid, this->pid}),
          cap()));
    }
  }
  auto cap() const {
    if constexpr (is_n64)
      return n64_issuer.capability();
    else
      return ppc_issuer.capability();
  }
  struct Observation {
    TypedFixture &fixture;
    void begin(const hw::KernelEntryStackView &view) {
      CHECK(fixture.readiness.begin_entry(fixture.id,
                                          fixture.thread().native_frame,
                                          selected_space, view, fixture.cap()));
    }
    void captured(const hw::KernelEntryStackView &view) {
      auto &f = fixture;
      auto &frame = f.thread().native_frame;
      if constexpr (is_n64)
        CHECK(f.readiness.capture(f.id, frame, selected_space, view,
                                  n64_issuer.query(frame), f.cap()));
      else
        CHECK(f.readiness.capture(f.id, frame, selected_space, view,
                                  ppc_issuer.query(frame), f.cap()));
      ++typed_captures;
    }
  };
  void capture(bool so = false) {
    this->retire();
    this->thread().syscall_pending = false;
    auto frame = this->frame();
    if constexpr (P == lu::Profile::ppc)
      frame.fp_state[280] = so ? 0x15 : 5;
    this->observe(frame, Observation{*this});
    this->arm();
    const auto identity = this->identity();
    CHECK(identity);
    captured_owner = identity.value();
    original = this->thread().native_frame;
    expect(id.image_generation != captured_owner.image);
    if constexpr (is_n64)
      expect(readiness.state() == Pub::Readiness::State::syscall_normalized);
    else
      expect(readiness.pending(id, this->thread().native_frame, cap()));
  }
  auto stage() {
    return Pub::stage(this->origins, captured_owner, this->binding(), readiness,
                      id, this->thread().native_frame, cap());
  }
  bool commit(Transaction &transaction) {
    return Pub::commit(transaction, captured_owner, this->binding(), id,
                       this->thread().native_frame, cap());
  }
  bool ready() {
    return readiness.ready(id, this->thread().native_frame, cap());
  }
  void normal_refused() {
    if constexpr (is_n64)
      expect(!readiness.publish_linux_completion(
          id, this->thread().native_frame, cap(), 0));
    else
      expect(!readiness.complete_syscall(id, this->thread().native_frame, cap(),
                                         {0, 0}));
  }
};
template <lu::Profile P>
void successful(Kernel &kernel, bool socketcall = false, bool so = false) {
  using F = TypedFixture<P>;
  using Pub = typename F::Pub;
  F fixture{kernel, socketcall};
  fixture.capture(so);
  const auto binding = fixture.binding();
  const auto before = fixture.original;
  auto staged = fixture.stage();
  CHECK(staged);
  auto replay = staged.value();
  expect(!fixture.ready());
  fixture.normal_refused();
  expect(Pub::Origin::same_frame(before, fixture.thread().native_frame));
  expect(fixture.commit(staged.value()));
  ++commits;
  expect(fixture.ready());
  fixture.normal_refused();
  auto expected = before;
  expected.pc = site;
  if constexpr (P == lu::Profile::ppc)
    expected.fp_state[280] |= 0x10;
  expect(Pub::Origin::same_frame(fixture.thread().native_frame, expected));
  expect(!fixture.origins.linux_udp_receive_identity(*fixture.owner().capture,
                                                     before, binding));
  auto &frame = fixture.thread().native_frame;
  const auto committed = frame;
  frame = before;
  expect(
      !fixture.origins.prepare_restart(fixture.captured_owner, frame, binding));
  frame = committed;
  expect(!fixture.commit(replay));
  Pub::cancel(replay);
  expect(fixture.ready());
  const auto context =
      fixture.readiness.context(fixture.id, frame, fixture.cap());
  if constexpr (P == lu::Profile::mips64) {
    expect(
        context.resume ==
        native::Mips64N64SignalFrame::ResumeContract::prepared_syscall_restart);
    expect(native::Mips64N64SignalFrame::capture(frame, context));
    expect(frame.registers[2] ==
               *lu::UdpConstants<P>::syscall_number(lu::UdpCall::recvfrom) &&
           frame.registers[7] == before.registers[7]);
  } else {
    expect(context.resume ==
           native::Ppc32SignalContext::Resume::prepared_syscall_restart);
    expect(context.metadata.result == 0 &&
           context.metadata.original_r3 == before.registers[3] &&
           context.metadata.trap == 0xc00);
    const auto legacy =
        context.template codec<native::Ppc32SignalFormat::legacy>();
    const auto realtime =
        context.template codec<native::Ppc32SignalFormat::realtime>();
    expect(native::Ppc32LegacySignalFrame::supported(frame, legacy.contract) &&
           native::Ppc32RtSignalFrame::supported(frame, realtime.contract));
  }
  // The old exact wait and retained OFD are retired before handler publication.
  fixture.retire();
  fixture.thread().syscall_pending = false;
  expect(!fixture.thread().wait_registration.active());
  auto handler = frame;
  handler.pc = site + 0x100;
  handler.sp -= 0x400;
  if constexpr (P == lu::Profile::ppc) {
    handler.registers[1] = handler.sp;
    CHECK(fixture.readiness.stage_handler(fixture.id, frame, handler,
                                          fixture.cap()));
    frame = handler;
  } else {
    handler.registers[29] = handler.sp;
    frame = handler;
  }
  CHECK(fixture.readiness.publish_handler(fixture.id, frame, fixture.cap()));
  expect(fixture.ready());
}
template <lu::Profile P> void refusal_cases(Kernel &kernel) {
  using F = TypedFixture<P>;
  using Pub = typename F::Pub;
  F fixture{kernel};
  for (unsigned field = 0; field < 11; ++field) {
    fixture.capture();
    auto staged = fixture.stage();
    CHECK(staged);
    auto &frame = fixture.thread().native_frame;
    auto owner = fixture.captured_owner;
    auto binding = fixture.binding();
    auto id = fixture.id;
    if (field == 0)
      ++owner.capture;
    if (field == 1)
      ++owner.epoch;
    if (field == 2)
      ++owner.lease.generation;
    if (field == 3)
      ++binding.object.generation;
    if (field == 4)
      ++binding.ticket.generation;
    if (field == 5)
      ++binding.operation.count;
    if (field == 6)
      ++id.image_generation;
    if (field == 7)
      frame.registers[3] ^= 1;
    if (field == 8)
      frame.fp_state[31] ^= 1;
    if (field == 9)
      fixture.origins.revoke(fixture.thread());
    if (field == 10) {
      const auto view = native::entry_stacks.activate_entry(
          fixture.thread().entry_stack, {fixture.pid, fixture.pid}, 0);
      CHECK(view);
      CHECK(native::entry_stacks.deactivate_entry(
          fixture.thread().entry_stack, {fixture.pid, fixture.pid}, 0));
    }
    const auto before = frame;
    expect(
        !Pub::commit(staged.value(), owner, binding, id, frame, fixture.cap()));
    ++refusals;
    expect(Pub::Origin::same_frame(before, frame) && !fixture.ready());
  }
  fixture.capture();
  auto cancelled = fixture.stage();
  CHECK(cancelled);
  auto stale = cancelled.value();
  Pub::cancel(cancelled.value());
  ++cancels;
  expect(!fixture.ready() && !fixture.commit(cancelled.value()));
  fixture.capture();
  auto fresh = fixture.stage();
  CHECK(fresh);
  Pub::cancel(stale);
  expect(fixture.commit(fresh.value()));
  ++commits;
  expect(fixture.ready());
  fixture.capture();
  auto rearmed = fixture.stage();
  CHECK(rearmed);
  static_cast<Fixture<P> &>(fixture).ready();
  fixture.rearm();
  expect(!fixture.commit(rearmed.value()));
  ++refusals;
  expect(!fixture.ready());
  fixture.capture();
  const auto binding = fixture.binding();
  static_cast<Fixture<P> &>(fixture).ready();
  {
    auto borrow = fixture.origins.acquire_linux_udp_receive_access(
        *fixture.owner().capture, binding);
    CHECK(borrow);
    expect(!fixture.stage());
    ++refusals;
  }
  fixture.capture();
  fixture.code(false); // Existing capture qualification is immutable.
  auto immutable = fixture.stage();
  CHECK(immutable);
  expect(fixture.commit(immutable.value()));
  ++commits;
  fixture.code(false);
  fixture.retire();
  fixture.thread().syscall_pending = false;
  fixture.observe(fixture.frame(), typename F::Observation{fixture});
  fixture.arm();
  const auto owner = fixture.identity();
  CHECK(owner);
  expect(!Pub::stage(fixture.origins, owner.value(), fixture.binding(),
                     fixture.readiness, fixture.id,
                     fixture.thread().native_frame, fixture.cap()));
  ++refusals;
}
std::vector<std::byte> read(const char *name) {
  std::ifstream stream(name, std::ios::binary);
  CHECK(stream.good());
  const std::vector<char> raw((std::istreambuf_iterator<char>(stream)), {});
  std::vector<std::byte> result(raw.size());
  std::memcpy(result.data(), raw.data(), raw.size());
  return result;
}
} // namespace
bool hw::ppc32_private::address_space_live(hw::AddressSpace space) noexcept {
  return native::BackendCapture::same_space(space, selected_space);
}
int main(int argc, char **argv) {
  expect(argc == 3);
  linux_elf = read(argv[1]);
  sysv_elf = read(argv[2]);
  expect(linux_elf.size() == sysv_elf.size() && linux_elf.size() > 52 &&
         linux_elf[7] == std::byte{3} && sysv_elf[7] == std::byte{0});
  for (usize index = 0; index < linux_elf.size(); ++index)
    if (index != 7)
      expect(linux_elf[index] == sysv_elf[index]);
  n64_issuer.qualify();
  ppc_issuer.qualify();
  static Kernel kernel;
  CHECK(kernel.boot({}));
  CHECK(native::entry_stacks.activate(native::entry_stack_arena));
  CHECK(kernel.native_udp().configure(network_link));
  successful<lu::Profile::mips64>(kernel);
  successful<lu::Profile::ppc>(kernel, false, false);
  successful<lu::Profile::ppc>(kernel, false, true);
  successful<lu::Profile::ppc>(kernel, true, false);
  refusal_cases<lu::Profile::mips64>(kernel);
  refusal_cases<lu::Profile::ppc>(kernel);
  expect(commits == 8 && cancels == 2 && refusals == 28 &&
         typed_captures == 38);
  expect(kernel.native_udp().active_sockets() == 0 &&
         native::entry_stacks.accounting().leased_slots == 0);
  std::printf("LINUX_UDP_RESTART_PUBLICATION_PASS checks=%zu profiles=2 "
              "commits=%zu cancels=%zu refusals=%zu captures=%zu hosted=1 "
              "guest=0 native_restart=0 nic=0 elf_metadata_only=1\n",
              checks, commits, cancels, refusals, typed_captures);
}
