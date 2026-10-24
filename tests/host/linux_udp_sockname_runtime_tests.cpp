// Actual configured x86_64 runtime entry, dispatch, captured access, OFD and publication.
// Backend trap, mappings and link service boundary are hosted models only.
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_udp_receive_model.hpp"
#include <source_location>
static_assert(lu::configured_native_profile()==lu::Profile::x86_64);
namespace {
using C=lu::UdpConstants<lu::Profile::x86_64>; using E=lu::UdpErrno;
constexpr uptr name_at=hw::user_begin+128,length_at=hw::user_begin+256,input_at=hw::user_begin+384;
constexpr net::NativeUdpLink network_fixture{{2,1,2,3,4,5},{2,6,7,8,9,10},{192,0,2,2},{192,0,2,1}};
usize checks{},invocations{};
void expect(bool value,std::source_location at=std::source_location::current()) {
  ++checks; if(!value) { std::fprintf(stderr,"LINUX_UDP_SOCKNAME_RUNTIME_FAIL line=%u checks=%zu\n",at.line(),checks); std::abort(); }
}
i64 error(E e) { return -static_cast<i64>(*C::errno_number(e)); }
class Link final:public KernelExtension {
public:
  usize queries{},submissions{};
  Result<net::NativeUdpLink> network_link(Kernel &) override { ++queries; return network_fixture; }
  Status network_try_transmit(Kernel &,std::span<const std::byte>) override { ++submissions; return {}; }
} extension;
sched::ProcessId pid{};
i64 invoke(Kernel &kernel,lu::UdpCall call,std::array<uptr,6> args) {
  CHECK(kernel.scheduler().activate_thread(pid,pid)); CHECK(kernel.posix().activate(pid));
  auto &caller=*kernel.scheduler().find_thread(pid,pid);
  hw::UserFrame frame{}; frame.pc=hw::user_begin+512; frame.sp=hw::user_begin+4096;
  frame.registers[14]=*C::syscall_number(call);
  constexpr usize slots[]{9,10,11,5,7,6};
  for(usize i=0;i<6;++i) frame.registers[slots[i]]=args[i];
  caller.native_frame=frame; caller.syscall_pending=false; mock::entry_trap={hw::TrapKind::syscall,256};
  native::last_pid=native::last_tid=0; const auto entries=mock::entries;
  expect(native::step(kernel)); ++invocations;
  expect(mock::entries==entries+1 && !caller.syscall_pending && caller.native_active && native::runtime_status().ok());
  auto retired=native::LinuxUdpImmediateAccess<lu::Profile::x86_64>::acquire(native::syscall_origins,*kernel.scheduler().find(pid),caller);
  expect(!retired);
  return static_cast<i64>(caller.native_frame.registers[14]);
}
void length(u32 value) { std::memcpy(mock::memory.data()+256,&value,4); }
u32 length() { u32 value{};std::memcpy(&value,mock::memory.data()+256,4);return value; }
void input(u16 port,net::wire::Ipv4 address={}) {
  CHECK(lu::UdpWire<lu::Profile::x86_64>::encode_address({reinterpret_cast<u8 *>(mock::memory.data()+384),16},{2,port,address}));
}
void name(Kernel &kernel,i32 fd,u32 endpoint,u32 capacity=16) {
  mock::memory.fill(std::byte{0xa7});length(capacity);
  const auto binding=kernel.native_udp().binding(endpoint);CHECK(binding);
  const auto readiness=kernel.native_udp().readiness(endpoint);CHECK(readiness);
  const auto queries=extension.queries,submissions=extension.submissions;
  expect(invoke(kernel,lu::UdpCall::getsockname,{static_cast<uptr>(fd),capacity?name_at:~uptr{0},length_at})==0);
  expect(length()==16 && extension.queries==queries && extension.submissions==submissions);
  expect(kernel.native_udp().binding(endpoint).value()==binding.value());
  const auto after=kernel.native_udp().readiness(endpoint);CHECK(after);
  expect(after.value().sequence==readiness.value().sequence && after.value().queued_datagrams==readiness.value().queued_datagrams);
  std::array<u8,16> bytes{};CHECK(lu::UdpWire<lu::Profile::x86_64>::encode_address(bytes,{2,binding.value().port,binding.value().address}));
  for(usize i=0;i<32;++i) expect(mock::memory[128+i]==(i<std::min<usize>(capacity,16)?std::byte{bytes[i]}:std::byte{0xa7}));
}
u32 endpoint(Kernel &kernel,i32 fd) {
  auto *slot=kernel.scheduler().find(pid)->fd_table().entry(fd);CHECK(slot);
  auto *description=kernel.scheduler().open_description(slot->description);CHECK(description);
  expect(description->references==1);return description->native_udp_handle;
}
}
int main() {
  static Kernel kernel;KernelConfig config{};config.extension=&extension;CHECK(kernel.boot(config));
  CHECK(native::entry_stacks.activate(native::entry_stack_arena));
  auto created=kernel.scheduler().create_user_process("linux-udp-sockname-runtime-host",kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096}));CHECK(created);pid=created.value();
  auto &process=*kernel.scheduler().find(pid);process.resources().personality=sched::UserPersonality::linux;
  process.resources().native_space={0x100000+pid*4096,static_cast<u32>(pid%16),1};
  auto &caller=*kernel.scheduler().find_thread(pid,pid);CHECK(native::prepare_native_thread(caller));caller.native_active=true;
  CHECK(kernel.scheduler().set_runnable(pid));CHECK(kernel.scheduler().activate_thread(pid,pid));CHECK(kernel.posix().activate(pid));
  native::runtime_kernel=&kernel;native::started=true;
  auto opened=invoke(kernel,lu::UdpCall::socket,{2,C::sock_dgram|C::sock_nonblock,17});expect(opened>=3);auto fd=static_cast<i32>(opened);
  auto socket=endpoint(kernel,fd);expect(!kernel.native_udp().binding(socket).value().bound);
  for(u32 capacity:{0U,1U,7U,16U,17U,0x7fffffffU}) name(kernel,fd,socket,capacity);
  expect(!kernel.native_udp().binding(socket).value().bound);
  input(0,network_fixture.local_ip);expect(invoke(kernel,lu::UdpCall::bind,{static_cast<uptr>(fd),input_at,16})==0);
  const auto bound=kernel.native_udp().binding(socket).value();expect(bound.bound && bound.port>=32768 && bound.address==network_fixture.local_ip);
  name(kernel,fd,socket);
  CHECK(kernel.posix().activate(pid));const auto duplicate=kernel.posix().duplicate(fd);CHECK(duplicate);
  name(kernel,duplicate.value(),socket);CHECK(kernel.posix().activate(pid));CHECK(kernel.posix().close(duplicate.value()));
  length(0xffffffffU);expect(invoke(kernel,lu::UdpCall::getsockname,{static_cast<uptr>(fd),name_at,length_at})==error(E::e_inval));expect(length()==0xffffffffU);
  const auto copies=mock::copies,outputs=mock::outputs;
  expect(invoke(kernel,lu::UdpCall::getsockname,{31,0,0})==error(E::e_badf));
  expect(invoke(kernel,lu::UdpCall::getsockname,{0,0,0})==error(E::e_notsock));
  expect(mock::copies==copies && mock::outputs==outputs);
  length(7);mock::write_forbidden=length_at;
  expect(invoke(kernel,lu::UdpCall::getsockname,{static_cast<uptr>(fd),name_at,length_at})==error(E::e_fault));
  expect(length()==7 && mock::memory[128]==std::byte{2});mock::write_forbidden=0;
  length(16);expect(invoke(kernel,lu::UdpCall::getsockname,{static_cast<uptr>(fd),0,length_at})==error(E::e_fault));
  expect(invoke(kernel,lu::UdpCall::getsockname,{static_cast<uptr>(fd),name_at,0})==error(E::e_fault));
  CHECK(kernel.posix().activate(pid));CHECK(kernel.posix().close(fd));expect(!kernel.native_udp().binding(socket));
  opened=invoke(kernel,lu::UdpCall::socket,{2,C::sock_dgram|C::sock_nonblock,17});expect(opened==fd);socket=endpoint(kernel,fd);
  // First-send error still autobinds. Query exposes that actual persistent port.
  expect(invoke(kernel,lu::UdpCall::sendto,{static_cast<uptr>(fd),name_at,0})==error(E::e_destaddrreq));
  const auto autobound=kernel.native_udp().binding(socket).value();expect(autobound.bound && autobound.port>=32768 && autobound.address==net::wire::Ipv4{});
  name(kernel,fd,socket);expect(extension.submissions==0 && extension.queries==1);
  CHECK(kernel.posix().activate(pid));CHECK(kernel.posix().close(fd));
  native::release_process(kernel,pid);CHECK(kernel.scheduler().exit_process(pid,0));CHECK(kernel.scheduler().reap_orphans());
  expect(native::runtime_status().ok() && kernel.native_udp().active_sockets()==0 && native::entry_stacks.accounting().leased_slots==0);
  std::printf("LINUX_UDP_SOCKNAME_RUNTIME_PASS checks=%zu invocations=%zu backend_entries=%zu hosted=1 profile=x86_64 guest=0 nic=0\n",checks,invocations,mock::entries);
}
