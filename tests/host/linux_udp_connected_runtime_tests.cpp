// Actual configured x86_64 runtime dispatch; synthetic traps/pages/transmitter.
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_udp_receive_model.hpp"
#include <source_location>
static_assert(lu::configured_native_profile()==lu::Profile::x86_64);
namespace {
using C=lu::UdpConstants<lu::Profile::x86_64>;using E=lu::UdpErrno;
constexpr uptr name_at=hw::user_begin+128,length_at=hw::user_begin+256,
               input_at=hw::user_begin+384,payload_at=hw::user_begin+640;
constexpr net::NativeUdpLink network_fixture{{2,1,2,3,4,5},{2,6,7,8,9,10},{192,0,2,2},{192,0,2,1}};
usize checks{},invocations{};
void expect(bool value,std::source_location at=std::source_location::current()) {
  ++checks;if(!value) { std::fprintf(stderr,"LINUX_UDP_CONNECTED_RUNTIME_FAIL line=%u checks=%zu\n",at.line(),checks);std::abort(); }
}
i64 error(E value) { return -static_cast<i64>(*C::errno_number(value)); }
class Link final:public KernelExtension {
public:
  usize queries{},submissions{};u16 last_port{},last_source{};usize last_payload{};bool fail_submission{};
  Result<net::NativeUdpLink> network_link(Kernel &) override { ++queries;return network_fixture; }
  Status network_try_transmit(Kernel &,std::span<const std::byte> bytes) override {
    ++submissions;auto ethernet=net::wire::ethernet(bytes);CHECK(ethernet);auto ip=net::wire::ipv4(ethernet.value().payload);CHECK(ip);
    const auto &packet=ip.value();CHECK(packet.payload.size()>=8 && net::wire::be16(packet.payload,4)==packet.payload.size());
    CHECK(net::wire::sum(packet.payload,net::wire::pseudo_sum(packet.source,packet.destination,17,static_cast<u16>(packet.payload.size())))==0xffffu);
    last_port=net::wire::be16(packet.payload,2);last_source=net::wire::be16(packet.payload,0);last_payload=packet.payload.size()-8;
    return fail_submission?Status::io_error("injected transmitter error"):Status{};
  }
} extension;
sched::ProcessId pid{};
i64 invoke(Kernel &kernel,lu::UdpCall call,std::array<uptr,6> args) {
  CHECK(kernel.scheduler().activate_thread(pid,pid));CHECK(kernel.posix().activate(pid));
  auto &caller=*kernel.scheduler().find_thread(pid,pid);
  hw::UserFrame frame{};frame.pc=hw::user_begin+512;frame.sp=hw::user_begin+4096;
  frame.registers[14]=*C::syscall_number(call);constexpr usize slots[]{9,10,11,5,7,6};
  for(usize i=0;i<6;++i) frame.registers[slots[i]]=args[i];
  caller.native_frame=frame;caller.syscall_pending=false;mock::entry_trap={hw::TrapKind::syscall,256};native::last_pid=native::last_tid=0;
  const auto entries=mock::entries;expect(native::step(kernel));++invocations;
  expect(mock::entries==entries+1 && !caller.syscall_pending && caller.native_active && native::runtime_status().ok());
  expect(!native::LinuxUdpImmediateAccess<lu::Profile::x86_64>::acquire(native::syscall_origins,*kernel.scheduler().find(pid),caller));
  return static_cast<i64>(caller.native_frame.registers[14]);
}
void input(u16 family,u16 port,net::wire::Ipv4 ip=network_fixture.peer_ip) {
  CHECK(lu::UdpWire<lu::Profile::x86_64>::encode_address({reinterpret_cast<u8 *>(mock::memory.data()+384),16},{family,port,ip}));
}
void length(u32 bits) { std::memcpy(mock::memory.data()+256,&bits,4); }
u32 length() { u32 bits{};std::memcpy(&bits,mock::memory.data()+256,4);return bits; }
u32 endpoint(Kernel &kernel,i32 fd) {
  const auto *slot=kernel.scheduler().find(pid)->fd_table().entry(fd);CHECK(slot);
  const auto *description=kernel.scheduler().open_description(slot->description);CHECK(description);return description->native_udp_handle;
}
}
int main() {
  static Kernel kernel;KernelConfig config{};config.extension=&extension;CHECK(kernel.boot(config));CHECK(native::entry_stacks.activate(native::entry_stack_arena));
  auto created=kernel.scheduler().create_user_process("udp-s6-runtime-host",kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096}));CHECK(created);pid=created.value();
  auto &process=*kernel.scheduler().find(pid);process.resources().personality=sched::UserPersonality::linux;process.resources().native_space={0x100000+pid*4096,static_cast<u32>(pid%16),1};
  auto &caller=*kernel.scheduler().find_thread(pid,pid);CHECK(native::prepare_native_thread(caller));caller.native_active=true;
  CHECK(kernel.scheduler().set_runnable(pid));CHECK(kernel.scheduler().activate_thread(pid,pid));CHECK(kernel.posix().activate(pid));native::runtime_kernel=&kernel;native::started=true;
  auto opened=invoke(kernel,lu::UdpCall::socket,{2,C::sock_dgram|C::sock_nonblock,17});expect(opened>=3);auto fd=static_cast<i32>(opened);auto socket=endpoint(kernel,fd);
  expect(invoke(kernel,lu::UdpCall::getpeername,{static_cast<uptr>(fd),0,0})==-static_cast<i64>(C::not_connected_errno_number()));
  input(2,41000);expect(invoke(kernel,lu::UdpCall::connect,{static_cast<uptr>(fd),input_at,2})==error(E::e_inval));
  const auto reserved=kernel.native_udp().binding(socket).value();expect(reserved.bound && reserved.address==net::wire::Ipv4{});
  expect(invoke(kernel,lu::UdpCall::connect,{static_cast<uptr>(fd),input_at,16})==0);
  const auto associated=kernel.native_udp().peer(socket).value();expect(associated.associated && associated.port==41000);
  const auto bound=kernel.native_udp().binding(socket).value();expect(bound.port==reserved.port && bound.address==network_fixture.local_ip);
  mock::memory[640]=std::byte{'a'};mock::memory[641]=std::byte{'b'};
  expect(invoke(kernel,lu::UdpCall::sendto,{static_cast<uptr>(fd),payload_at,2})==2);
  expect(extension.submissions==1 && extension.last_port==41000 && extension.last_source==bound.port && extension.last_payload==2);
  input(2,42000);expect(invoke(kernel,lu::UdpCall::sendto,{static_cast<uptr>(fd),payload_at,0,0,input_at,16})==0);
  expect(extension.submissions==2 && extension.last_port==42000 && extension.last_payload==0 && kernel.native_udp().peer(socket).value()==associated);
  length(7);expect(invoke(kernel,lu::UdpCall::getpeername,{static_cast<uptr>(fd),name_at,length_at})==0 && length()==16);
  expect(mock::memory[130]==std::byte{static_cast<u8>(41000>>8)} && mock::memory[131]==std::byte{static_cast<u8>(41000)});
  input(2,0);expect(invoke(kernel,lu::UdpCall::connect,{static_cast<uptr>(fd),input_at,16})==0);
  const auto zero=kernel.native_udp().peer(socket).value();expect(zero.associated && !zero.port);
  const auto copies=mock::copies,outputs=mock::outputs;
  expect(invoke(kernel,lu::UdpCall::getpeername,{static_cast<uptr>(fd),0,0})==-static_cast<i64>(C::not_connected_errno_number()));
  expect(mock::copies==copies && mock::outputs==outputs);
  mock::forbidden=payload_at;expect(invoke(kernel,lu::UdpCall::sendto,{static_cast<uptr>(fd),payload_at,2})==error(E::e_fault));mock::forbidden=0;
  const auto before_copy=mock::copies;
  expect(invoke(kernel,lu::UdpCall::sendto,{static_cast<uptr>(fd),payload_at,2})==2);
  expect(mock::copies==before_copy+1 && extension.submissions==3 && !extension.last_port && extension.last_source==bound.port && extension.last_payload==2);
  expect(kernel.native_udp().peer(socket).value()==zero && kernel.native_udp().binding(socket).value()==bound);
  expect(invoke(kernel,lu::UdpCall::sendto,{static_cast<uptr>(fd),payload_at,0})==0 && extension.submissions==4 && !extension.last_port && !extension.last_payload);
  extension.fail_submission=true;
  expect(invoke(kernel,lu::UdpCall::sendto,{static_cast<uptr>(fd),payload_at,2})==error(E::e_io) && extension.submissions==5);
  extension.fail_submission=false;
  expect(kernel.native_udp().peer(socket).value()==zero && kernel.native_udp().binding(socket).value()==bound);
  expect(invoke(kernel,lu::UdpCall::sendto,{static_cast<uptr>(fd),payload_at,2,~uptr{0}})==error(E::e_opnotsupp));
  expect(invoke(kernel,lu::UdpCall::sendto,{static_cast<uptr>(fd),payload_at,2,0,input_at,16})==error(E::e_inval));
  input(2,43000);expect(invoke(kernel,lu::UdpCall::sendto,{static_cast<uptr>(fd),payload_at,2,0,input_at,16})==2);
  expect(extension.submissions==6 && extension.last_port==43000 && kernel.native_udp().peer(socket).value()==zero);
  input(0,0);expect(invoke(kernel,lu::UdpCall::connect,{static_cast<uptr>(fd),input_at,2})==0);
  expect(!kernel.native_udp().binding(socket).value().bound && !kernel.native_udp().peer(socket).value().associated);
  expect(invoke(kernel,lu::UdpCall::sendto,{static_cast<uptr>(fd),payload_at,0})==error(E::e_destaddrreq));
  expect(kernel.native_udp().binding(socket).value().bound && kernel.native_udp().binding(socket).value().address==net::wire::Ipv4{});
  CHECK(kernel.posix().activate(pid));CHECK(kernel.posix().close(fd));expect(!kernel.native_udp().peer(socket));
  opened=invoke(kernel,lu::UdpCall::socket,{2,C::sock_dgram,17});expect(opened==fd);socket=endpoint(kernel,fd);
  input(2,41000);expect(invoke(kernel,lu::UdpCall::connect,{static_cast<uptr>(fd),input_at,16})==0); // Blocking connect remains synchronous.
  CHECK(kernel.posix().activate(pid));CHECK(kernel.posix().close(fd));native::release_process(kernel,pid);CHECK(kernel.scheduler().exit_process(pid,0));CHECK(kernel.scheduler().reap_orphans());
  expect(kernel.native_udp().active_sockets()==0 && native::entry_stacks.accounting().leased_slots==0 && native::runtime_status().ok() && extension.queries==1);
  std::printf("LINUX_UDP_CONNECTED_RUNTIME_PASS checks=%zu calls=%zu backend_entries=%zu tx=%zu hosted=1 guest=0 nic=0\n",checks,invocations,mock::entries,extension.submissions);
}
