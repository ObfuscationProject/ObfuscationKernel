// Genuine ten-profile captured owner/restart models. No target execution/NIC.
#include "linux_udp_restart_capture_fixture.hpp"
namespace {
using namespace udp_restart_model;
net::NativeUdpService peer;
u32 sender{};
constexpr std::array<std::byte, 3> data{std::byte{'a'}, std::byte{'b'},
                                        std::byte{'c'}};
template <lu::Profile P> void packet(Fixture<P> &fixture) {
  std::array<std::byte, net::NativeUdpService::frame_capacity> frame{}, reply{};
  const auto built = peer.build_datagram(sender, network_link.local_ip,
                                         fixture.port, data, frame);
  CHECK(built);
  const auto ingested =
      fixture.kernel.native_udp().ingest({frame.data(), built.value()}, reply);
  CHECK(ingested);
  expect(ingested.value().queued_handle == fixture.endpoint);
}
template <lu::Profile P> struct BorrowCopy {
  using F = Fixture<P>;
  F &fixture;
  typename F::Origins::LinuxUdpReceiveBorrow borrow;
  bool live() const {
    return borrow.live() && fixture.store.member(fixture.object) &&
           fixture.owner().phase == native::LinuxUdpReceivePhase::published;
  }
  Status read(u64 address, std::span<std::byte> bytes) {
    if (!live())
      return Status::denied("model borrow expired");
    const auto space = borrow.address_space();
    if (!space)
      return space.status();
    const auto status =
        hw::copy_from_user(space.value(), bytes.data(), address, bytes.size());
    return live() ? status : Status::denied("model borrow expired after copy");
  }
  Status write(u64 address, std::span<const std::byte> bytes) {
    if (!live())
      return Status::denied("model borrow expired");
    const auto space = borrow.address_space();
    if (!space)
      return space.status();
    const auto status =
        hw::copy_to_user(space.value(), address, bytes.data(), bytes.size());
    return live() ? status : Status::denied("model borrow expired after copy");
  }
};
template <lu::Profile P>
void captured_restart(Kernel &kernel, bool socketcall = false,
                      bool thumb = false) {
  using F = Fixture<P>;
  using Origins = typename F::Origins;
  using Permit = typename Origins::LinuxUdpRestartPermit;
  F fixture{kernel, socketcall, thumb};
  fixture.observe(fixture.frame());
  fixture.arm();
  ++variants;
  auto &thread = fixture.thread();
  const auto original = thread.native_frame;
  const auto first = fixture.binding();
  const auto identity = fixture.identity();
  CHECK(identity);
  const auto qualified = fixture.origins.linux_udp_receive_restart_qualified(
      *fixture.owner().capture, original, first);
  CHECK(qualified);
  expect(qualified.value());
  auto permit =
      fixture.origins.prepare_restart(identity.value(), original, first);
  CHECK(permit);
  expect(!fixture.origins.identity(fixture.process(), thread).capture &&
         !fixture.origins.live(identity.value()));
  Origins foreign{native::entry_stacks};
  expect(!foreign.validate_restart(permit.value(), identity.value(), original,
                                   first));
  expect(!fixture.origins.validate_restart(Permit{}, identity.value(), original,
                                           first));
  for (unsigned field = 0; field < 7; ++field) {
    auto changed = first;
    if (field == 0)
      ++changed.object.generation;
    if (field == 1)
      ++changed.ticket.generation;
    if (field == 2)
      ++changed.request_sequence;
    if (field == 3)
      ++changed.description;
    if (field == 4)
      ++changed.endpoint;
    if (field == 5)
      ++changed.operation.count;
    if (field == 6)
      ++changed.operation.payload;
    expect(
        !fixture.origins.prepare_restart(identity.value(), original, changed));
    expect(!fixture.origins.validate_restart(permit.value(), identity.value(),
                                             original, changed));
  }
  if (socketcall) {
    memory_word<P>(words + lu::Codec<P>::profile.word_bytes, output + 32);
    memory_word<P>(words + 2 * lu::Codec<P>::profile.word_bytes, 2);
  } else if constexpr (P == lu::Profile::mips) {
    memory_word<P>(original.sp + 16, output + 64);
    memory_word<P>(original.sp + 20, hw::user_begin + 800);
    memory_word<P>(hw::user_begin + 800, 16);
  }
  // Published request words remain copied, while new execution decodes anew.
  expect(fixture.owner().operation == first.operation &&
         fixture.origins.validate_restart(permit.value(), identity.value(),
                                          original, first));
  packet(fixture);
  fixture.ready();
  CHECK(kernel.native_udp().consume(fixture.endpoint));
  {
    auto borrowed = fixture.origins.acquire_linux_udp_receive_access(
        *fixture.owner().capture, first);
    CHECK(borrowed);
    expect(!fixture.origins.prepare_restart(identity.value(), original, first));
    expect(!fixture.origins.consume_restart(permit.value(), identity.value(),
                                            original, first));
    BorrowCopy<P> access{fixture, std::move(borrowed.value())};
    native::linux_udp::ReceiveScratch scratch;
    const auto received =
        scratch.receive<P>(kernel.native_udp(), fixture.endpoint, access,
                           {.payload = first.operation.payload,
                            .count = first.operation.count,
                            .source = first.operation.address,
                            .source_length = first.operation.source_length,
                            .flags = first.operation.flags});
    expect(!received && received.status().code() == StatusCode::would_block);
  }
  fixture.rearm();
  const auto rearmed = fixture.binding();
  expect(!fixture.origins.validate_restart(permit.value(), identity.value(),
                                           original, first));
  expect(!fixture.origins.validate_restart(permit.value(), identity.value(),
                                           original, rearmed));
  permit = fixture.origins.prepare_restart(identity.value(), original, rearmed);
  CHECK(permit);
  fixture.code(false);
  expect(fixture.origins.validate_restart(permit.value(), identity.value(),
                                          original, rearmed));
  fixture.code(true);
  expect(fixture.origins.consume_restart(permit.value(), identity.value(),
                                         original, rearmed));
  expect(!fixture.origins.consume_restart(permit.value(), identity.value(),
                                          original, rearmed));
  expect(!fixture.origins.linux_udp_receive_identity(*fixture.owner().capture,
                                                     original, rearmed));
  thread.native_frame = permit.value().plan().frame;
  auto expected = original;
  expected.pc = site;
  if constexpr (P == lu::Profile::arm32)
    expected.registers[15] = site;
  if constexpr (P == lu::Profile::ppc)
    expected.fp_state[280] |= 0x10;
  expect(Origins::same_frame(thread.native_frame, expected));
  const auto old_object = fixture.object;
  fixture.retire();
  thread.syscall_pending = false;
  // Present the newly executed instruction through the sealed backend again.
  // This does not submit the old Plan or a copied request as capture authority.
  auto next = thread.native_frame;
  next.pc += fixture.instruction_bytes();
  if constexpr (P == lu::Profile::arm32)
    next.registers[15] = next.pc;
  if (!socketcall) {
    if constexpr (P != lu::Profile::mips)
      fixture.arguments(next, output + 32, 2);
  }
  fixture.observe(next);
  ++reentries;
  fixture.arm();
  const auto fresh = fixture.identity();
  CHECK(fresh);
  const auto second = fixture.binding();
  expect(fresh.value().capture > identity.value().capture &&
         fresh.value().epoch > identity.value().epoch &&
         fixture.object != old_object &&
         second.ticket.generation > rearmed.ticket.generation);
  expect(!fixture.origins.validate_restart(permit.value(), identity.value(),
                                           original, second));
  expect(!fixture.store.objects().resolve(old_object));
  if constexpr (P == lu::Profile::mips) {
    if (!socketcall)
      expect(second.operation.address == output + 64 &&
             second.operation.source_length == hw::user_begin + 800 &&
             second.operation.count == 8);
    else
      expect(second.operation.payload == output + 32 &&
             second.operation.count == 2);
  } else
    expect(second.operation.payload == output + 32 &&
           second.operation.count == 2);
  packet(fixture);
  fixture.ready();
  {
    auto borrowed = fixture.origins.acquire_linux_udp_receive_access(
        *fixture.owner().capture, second);
    CHECK(borrowed);
    BorrowCopy<P> access{fixture, std::move(borrowed.value())};
    native::linux_udp::ReceiveScratch scratch;
    const auto received =
        scratch.receive<P>(kernel.native_udp(), fixture.endpoint, access,
                           {.payload = second.operation.payload,
                            .count = second.operation.count,
                            .source = second.operation.address,
                            .source_length = second.operation.source_length,
                            .flags = second.operation.flags});
    CHECK(received);
    expect(received.value() ==
           (P == lu::Profile::mips && !socketcall ? 3U : 2U));
    expect(mock::memory[second.operation.payload - hw::user_begin] ==
           std::byte{'a'});
  }
  CHECK(fixture.origins.consume_linux_udp_receive(*fixture.owner().capture,
                                                  second));
  expect(!fixture.origins.consume_linux_udp_receive(*fixture.owner().capture,
                                                    second));
  fixture.retire();
  thread.syscall_pending = false;
}
template <lu::Profile P> void absent_qualification(Kernel &kernel) {
  Fixture<P> fixture{kernel};
  fixture.code(false);
  fixture.observe(fixture.frame());
  fixture.arm();
  const auto identity = fixture.identity();
  CHECK(identity);
  const auto binding = fixture.binding();
  const auto frame = fixture.thread().native_frame;
  const auto qualification =
      fixture.origins.linux_udp_receive_restart_qualified(
          *fixture.owner().capture, frame, binding);
  CHECK(qualification);
  expect(!qualification.value());
  expect(!fixture.origins.prepare_restart(identity.value(), frame, binding));
  CHECK(fixture.origins.consume_linux_udp_receive_interruption(
      *fixture.owner().capture, frame, binding));
  expect(!fixture.origins.consume_linux_udp_receive_interruption(
      *fixture.owner().capture, frame, binding));
  expect(native::SyscallOriginTable<P>::same_frame(
      frame, fixture.thread().native_frame));
}
template <lu::Profile P> void exercise(Kernel &kernel) {
  captured_restart<P>(kernel);
  absent_qualification<P>(kernel);
  if (lu::UdpConstants<P>::socketcall_number())
    captured_restart<P>(kernel, true);
  if constexpr (P == lu::Profile::arm32)
    captured_restart<P>(kernel, false, true);
  ++profiles;
}
} // namespace
int main() {
  static Kernel kernel;
  CHECK(kernel.boot({}));
  CHECK(native::entry_stacks.activate(native::entry_stack_arena));
  CHECK(kernel.native_udp().configure(network_link));
  CHECK(peer.configure({network_link.peer_mac, network_link.local_mac,
                        network_link.peer_ip, network_link.local_ip}));
  const auto opened = peer.open();
  CHECK(opened);
  sender = opened.value();
  CHECK(peer.bind(sender, {}, 41000));
  exercise<lu::Profile::i386>(kernel);
  exercise<lu::Profile::x86_64>(kernel);
  exercise<lu::Profile::aarch64>(kernel);
  exercise<lu::Profile::arm32>(kernel);
  exercise<lu::Profile::rv64>(kernel);
  exercise<lu::Profile::rv32>(kernel);
  exercise<lu::Profile::loongarch64>(kernel);
  exercise<lu::Profile::mips>(kernel);
  exercise<lu::Profile::mips64>(kernel);
  exercise<lu::Profile::ppc>(kernel);
  CHECK(peer.close(sender));
  expect(profiles == 10 && variants == 14 && reentries == 14);
  expect(kernel.native_udp().active_sockets() == 0 &&
         native::entry_stacks.accounting().leased_slots == 0);
  std::printf("LINUX_UDP_RESTART_CAPTURE_PASS checks=%zu profiles=%zu "
              "variants=%zu cases=%zu captures=%zu reentries=%zu hosted=1 "
              "guest=0 native_restart=0 nic=0\n",
              checks, profiles, variants, cases, captures, reentries);
}
