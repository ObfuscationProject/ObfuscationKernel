#pragma once
// Production owner store and shared capture; hosted traps, memory and pages.
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_LINUX_UDP_RESTART_CAPTURE_MODEL 1
#include "../../src/user/native_syscall_restart_publication.hpp"
#include "native_udp_receive_model.hpp"
#include <memory>
#include <source_location>
namespace udp_restart_model {
inline usize checks{}, cases{}, captures{}, reentries{}, profiles{}, variants{};
inline constexpr uptr output = hw::user_begin + 128,
                      words = hw::user_begin + 512,
                      site = hw::user_begin + 1024;
inline constexpr net::NativeUdpLink network_link{
    {2, 1, 2, 3, 4, 5}, {2, 6, 7, 8, 9, 10}, {192, 0, 2, 2}, {192, 0, 2, 1}};
inline void expect(bool value,
                   std::source_location at = std::source_location::current()) {
  ++checks;
  if (!value) {
    std::fprintf(
        stderr, "LINUX_UDP_RESTART_CAPTURE_FAIL line=%u checks=%zu cases=%zu\n",
        at.line(), checks, cases);
    std::abort();
  }
}
struct Provider {
  static constexpr usize page_size = 4096, page_count = 16;
  struct Lease {
    std::span<std::byte> bytes{};
    const Provider *issuer{};
    usize index{};
    u64 generation{};
  };
  struct alignas(page_size) Backing {
    std::array<std::array<std::byte, page_size>, page_count> bytes{};
  };
  std::unique_ptr<Backing> backing{std::make_unique<Backing>()};
  std::array<u64, page_count> generations{};
  u64 sequence{};
  usize allocations{}, releases{};
  Result<Lease> allocate_kernel_page() {
    for (usize index = 0; index < page_count; ++index)
      if (!generations[index]) {
        const auto generation = ++sequence;
        generations[index] = generation;
        ++allocations;
        return Lease{
            {backing->bytes[index].data(), page_size}, this, index, generation};
      }
    return Status::no_memory("hosted receive store page limit");
  }
  Status release_kernel_page(Lease lease) {
    if (lease.issuer != this || lease.index >= page_count ||
        lease.bytes.size() != page_size ||
        lease.bytes.data() != backing->bytes[lease.index].data() ||
        !lease.generation || generations[lease.index] != lease.generation)
      return Status::denied("hosted receive store exact lease mismatch");
    generations[lease.index] = 0;
    ++releases;
    return {};
  }
};
static_assert(memory::ObjectPageProvider<Provider>);
template <lu::Profile P>
void reg(hw::UserFrame &frame, usize index, u64 value) {
  if constexpr (P == lu::Profile::i386) {
    constexpr usize map[]{7, 6, 5, 4, 0, 2, 1, 0};
    frame.registers[map[index]] = value;
  } else if constexpr (P == lu::Profile::x86_64) {
    constexpr usize map[]{14, 11, 12, 13, 10, 9, 8, 0, 7, 6, 5, 4, 3, 2, 1, 0};
    frame.registers[map[index]] = value;
  } else
    frame.registers[index] = value;
}
template <lu::Profile P> void memory_word(uptr address, u64 value) {
  constexpr auto word = lu::Codec<P>::profile.word_bytes;
  for (usize index = 0; index < word; ++index) {
    const auto shift =
        lu::Codec<P>::profile.big_endian ? (word - index - 1) * 8 : index * 8;
    mock::memory[address - hw::user_begin + index] =
        std::byte{static_cast<u8>(value >> shift)};
  }
}
struct NoObservation {
  void begin(const hw::KernelEntryStackView &) {}
  void captured(const hw::KernelEntryStackView &) {}
};
template <lu::Profile P> class Fixture {
public:
  using C = lu::UdpConstants<P>;
  using Origins = native::SyscallOriginTable<P>;
  using Access = native::LinuxUdpImmediateAccess<P>;
  using Store = native::LinuxUdpReceiveOwnerStore<P, Provider>;
  using Owner = native::LinuxUdpReceiveOwner<P>;
  using Binding = typename Origins::LinuxUdpReceiveBinding;
  static_assert(Store::pages <= Provider::page_count);
  Kernel &kernel;
  Origins origins{native::entry_stacks};
  native::VfsStatContinuation<P> vfs{native::entry_stacks};
  native::SharedNativeCapture<P> shared{native::entry_stacks};
  Provider provider;
  Store store{origins, provider};
  sched::ProcessId pid{};
  i32 fd{};
  u16 description{}, port{};
  u32 endpoint{};
  memory::ObjectHandle object{};
  bool socketcall{}, thumb{};
  explicit Fixture(Kernel &selected, bool indirect = false,
                   bool arm_thumb = false)
      : kernel(selected), socketcall(indirect), thumb(arm_thumb) {
    const auto made = kernel.scheduler().create_user_process(
        "linux-udp-captured-restart-host",
        kernel.arch().make_user_context(
            {hw::user_begin, hw::user_begin + 8192}));
    CHECK(made);
    pid = made.value();
    auto &p = process();
    auto &t = thread();
    p.resources().personality = sched::UserPersonality::linux;
    p.resources().native_space = {0x100000 + pid * 4096,
                                  static_cast<u32>(pid % 16), 1};
    CHECK(native::allocate(p.resources().native_pages, hw::user_begin,
                           hw::page_read | hw::page_execute));
    CHECK(native::prepare_native_thread(t));
    t.native_active = true;
    CHECK(origins.prepare(t));
    CHECK(vfs.prepare(t));
    CHECK(kernel.scheduler().set_runnable(pid));
    CHECK(kernel.scheduler().activate_thread(pid, pid));
    CHECK(kernel.posix().activate(pid));
    const auto opened = kernel.native_udp().open();
    CHECK(opened);
    endpoint = opened.value();
    port = static_cast<u16>(59000 + cases++);
    CHECK(kernel.native_udp().bind(endpoint, {}, port));
    const auto installed =
        kernel.posix().install_native_udp(endpoint, false, false);
    CHECK(installed);
    fd = installed.value();
    description = p.fd_table().entry(fd)->description;
    native::runtime_kernel = &kernel;
    native::started = true;
    CHECK(native::runtime_status());
    mock::memory.fill(std::byte{});
    code(true);
  }
  ~Fixture() {
    retire();
    origins.retire(thread());
    CHECK(vfs.cancel_process(kernel.scheduler(), pid));
    thread().syscall_pending = false;
    native::release_process(kernel, pid);
    CHECK(native::runtime_status());
    CHECK(kernel.scheduler().exit_process(pid, 0));
    CHECK(kernel.scheduler().reap_orphans());
    expect(provider.allocations == provider.releases &&
           store.objects().accounting().live_objects == 0);
    expect(!kernel.native_udp().binding(endpoint));
    mock::backend_result_hook = nullptr;
  }
  sched::ProcessControlBlock &process() {
    return *kernel.scheduler().find(pid);
  }
  sched::ThreadControlBlock &thread() {
    return *kernel.scheduler().find_thread(pid, pid);
  }
  Owner &owner() {
    const auto resolved = store.objects().resolve(object);
    CHECK(resolved);
    return *resolved.value();
  }
  Binding binding() { return *owner().binding; }
  void select() { CHECK(kernel.scheduler().activate_thread(pid, pid)); }
  usize instruction_bytes() const {
    return P == lu::Profile::i386 || P == lu::Profile::x86_64 || thumb ? 2 : 4;
  }
  void code(bool valid) {
    std::array<u8, 4> opcode{};
    if constexpr (P == lu::Profile::i386)
      opcode = {0xcd, 0x80, 0, 0};
    else if constexpr (P == lu::Profile::x86_64)
      opcode = {0x0f, 0x05, 0, 0};
    else if constexpr (P == lu::Profile::aarch64)
      opcode = {1, 0, 0, 0xd4};
    else if constexpr (P == lu::Profile::arm32)
      opcode = thumb ? std::array<u8, 4>{0, 0xdf, 0, 0}
                     : std::array<u8, 4>{0, 0, 0, 0xef};
    else if constexpr (P == lu::Profile::rv64 || P == lu::Profile::rv32)
      opcode = {0x73, 0, 0, 0};
    else if constexpr (P == lu::Profile::loongarch64)
      opcode = {0, 0, 0x2b, 0};
    else if constexpr (P == lu::Profile::mips || P == lu::Profile::mips64)
      opcode = {0, 0, 0, 12};
    else
      opcode = {0x44, 0, 0, 2};
    for (usize index = 0; index < instruction_bytes(); ++index)
      mock::memory[site - hw::user_begin + index] =
          std::byte{valid ? opcode[index] : u8{0}};
  }
  hw::Trap trap() const {
    using K = hw::TrapKind;
    if constexpr (P == lu::Profile::i386)
      return {K::syscall, 128};
    if constexpr (P == lu::Profile::x86_64)
      return {K::syscall, 256};
    if constexpr (P == lu::Profile::aarch64)
      return {K::syscall, 0x15, 0x54000000};
    if constexpr (P == lu::Profile::arm32)
      return {K::syscall, 2};
    if constexpr (P == lu::Profile::rv64 || P == lu::Profile::rv32)
      return {K::syscall, 8, 8};
    if constexpr (P == lu::Profile::loongarch64)
      return {K::syscall, 11};
    if constexpr (P == lu::Profile::mips || P == lu::Profile::mips64)
      return {K::syscall, 8, 32};
    return {K::syscall, 8};
  }
  void arguments(hw::UserFrame &frame, uptr payload = output, usize count = 8,
                 uptr source = 0, uptr source_length = 0) {
    const std::array<u64, 6> args{
        static_cast<u64>(fd), payload, count, 0, source, source_length};
    if (socketcall) {
      CHECK(C::socketcall_number());
      reg<P>(frame, lu::Codec<P>::profile.number_register,
             *C::socketcall_number());
      reg<P>(frame, lu::Codec<P>::profile.argument_registers[0], 12);
      reg<P>(frame, lu::Codec<P>::profile.argument_registers[1], words);
      for (usize index = 0; index < 6; ++index)
        memory_word<P>(words + index * lu::Codec<P>::profile.word_bytes,
                       args[index]);
    } else {
      reg<P>(frame, lu::Codec<P>::profile.number_register,
             *C::syscall_number(lu::UdpCall::recvfrom));
      for (usize index = 0; index < 6; ++index) {
        const auto selected = lu::Codec<P>::profile.argument_registers[index];
        if (selected >= 0)
          reg<P>(frame, static_cast<usize>(selected), args[index]);
        else
          memory_word<P>(frame.sp + index * lu::Codec<P>::profile.word_bytes,
                         args[index]);
      }
    }
  }
  hw::UserFrame frame() {
    hw::UserFrame result{};
    for (usize index = 0; index < result.registers.size(); ++index)
      result.registers[index] = 0x4200 + index * 29;
    for (usize index = 0; index < result.fp_state.size(); ++index)
      result.fp_state[index] = static_cast<u8>(index * 13 + 7);
    result.pc = site + instruction_bytes();
    result.sp = hw::user_begin + 4096;
    result.fs_base = hw::user_begin + 256;
    result.gs_base = hw::user_begin + 384;
    result.flags = P == lu::Profile::arm32 ? (thumb ? 0x30 : 0x10) : 0x202;
    if constexpr (P == lu::Profile::arm32)
      result.registers[15] = result.pc;
    if constexpr (P == lu::Profile::mips64) {
      result.flags = hw::mips64_n64_private::user_status;
      result.registers[29] = result.sp;
      for (usize index = 256; index < 260; ++index)
        result.fp_state[index] = 0;
    }
    if constexpr (P == lu::Profile::ppc) {
      result.flags = hw::ppc32_private::user_msr;
      result.registers[1] = result.sp;
      result.registers[2] = result.fs_base;
      result.registers[13] = result.gs_base;
      for (usize index = 264; index < 268; ++index)
        result.fp_state[index] = index == 267 ? 2 : 0;
      result.fp_state[280] = 5;
    }
    arguments(result);
    return result;
  }
  template <class Observation = NoObservation>
  void observe(hw::UserFrame presented, Observation observer = {}) {
    select();
    auto &t = thread();
    expect(!object.pool);
    t.native_frame = presented;
    t.syscall_pending = false;
    const auto active =
        native::entry_stacks.activate_entry(t.entry_stack, {pid, pid}, 0);
    CHECK(active);
    observer.begin(active.value());
    mock::entry_trap = trap();
    const auto entries = mock::entries;
    const auto captured = shared.enter(origins, vfs, process(), t,
                                       active.value(), kernel.scheduler());
    CHECK(captured);
    expect(mock::entries == entries + 1);
    observer.captured(active.value());
    CHECK(native::entry_stacks.deactivate_entry(t.entry_stack, {pid, pid}, 0));
    t.syscall_pending = true;
    ++captures;
  }
  void arm() {
    auto &p = process();
    auto &t = thread();
    auto access = Access::acquire(origins, p, t);
    CHECK(access);
    Status failure{};
    native::linux_udp::OriginalUdpDescription original{kernel.scheduler(),
                                                       failure};
    native::linux_udp::ImmediateLifetimeScope lifetime{access.value(),
                                                       original};
    native::linux_udp::OperationScratch scratch;
    auto prepared = scratch.prepare_blocking_receive<P>(
        kernel, p, t, access.value(), original);
    CHECK(prepared);
    CHECK(prepared.value().admission);
    const auto staged = store.stage(kernel, p, t, access.value(), original,
                                    *prepared.value().admission);
    CHECK(staged.ok());
    object = staged.object;
    expect(!access.value().live() && !original.held());
    CHECK(lifetime.finish());
    CHECK(failure);
    auto &wait = owner();
    expect(wait.phase == native::LinuxUdpReceivePhase::captured_staged &&
           wait.capture && wait.binding);
    const auto ready =
        kernel.native_udp_executor().readiness(kernel.native_udp(), endpoint);
    CHECK(ready);
    expect(ready.value().may_wait(ready.value().receive.sequence,
                                  ready.value().executor.readiness_sequence));
    const auto ticket = kernel.scheduler().register_current_wait(
        sched::ThreadWaitReason::udp_receive, endpoint);
    CHECK(ticket);
    wait.ticket = ticket.value();
    wait.request_sequence = wait.ticket.generation;
    const Binding exact{object,        wait.request_sequence,
                        wait.ticket,   wait.retained_description,
                        wait.endpoint, wait.operation};
    CHECK(origins.bind_linux_udp_receive(*wait.capture, t.native_frame, exact));
    wait.binding = exact;
    wait.phase = native::LinuxUdpReceivePhase::published;
    expect(kernel.scheduler().open_description(description)->references == 2 &&
           t.wait_registration.pending(wait.ticket));
  }
  auto identity() {
    return origins.linux_udp_receive_identity(*owner().capture,
                                              thread().native_frame, binding());
  }
  void ready() {
    CHECK(kernel.scheduler().complete_wait(owner().ticket,
                                           sched::WaitCompletion::ready));
    select();
  }
  void rearm() {
    auto &wait = owner();
    const auto before = binding();
    const auto ticket = kernel.scheduler().register_current_wait(
        sched::ThreadWaitReason::udp_receive, endpoint);
    CHECK(ticket);
    auto after = before;
    after.ticket = ticket.value();
    CHECK(origins.rearm_linux_udp_receive(*wait.capture, thread().native_frame,
                                          before, after));
    wait.ticket = after.ticket;
    wait.binding = after;
    expect(after.ticket.generation > before.ticket.generation &&
           after.request_sequence == before.request_sequence);
  }
  void retire() {
    if (!object.pool)
      return;
    auto &wait = owner();
    const auto ticket = wait.ticket;
    const auto selected = wait.retained_description;
    CHECK(origins.retire_linux_udp_receive(*wait.capture, *wait.binding));
    if (thread().wait_registration.active())
      CHECK(kernel.scheduler().complete_wait(ticket,
                                             sched::WaitCompletion::cancelled));
    *store.member(object) = {};
    CHECK(store.objects().destroy(object));
    CHECK(store.objects().trim_empty_pages());
    CHECK(kernel.scheduler().release_description(selected));
    object = {};
    expect(kernel.scheduler().open_description(description)->references == 1);
  }
};
} // namespace udp_restart_model
