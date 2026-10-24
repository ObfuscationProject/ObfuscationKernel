// Genuine shared captures and canonical OFDs; synthetic hosted traps/pages/link.
#include "linux_udp_restart_capture_fixture.hpp"
#include <functional>
using namespace udp_restart_model;
namespace {
constexpr uptr address_at=hw::user_begin+128,name_at=hw::user_begin+256,
               length_at=hw::user_begin+384,payload_at=hw::user_begin+640;
usize invocations{},indirect{},denials{},published{};
std::function<void()> callback;
std::function<void()> retired_access_probe;
usize callback_stage{},callback_at{};
u16 transmitted_port{},transmitted_source{};
usize transmissions{};
class Backend final {
public:
  explicit Backend(Kernel &kernel):kernel_{kernel},backend_{kernel} {}
  sched::SchedulerState &scheduler() { return backend_.scheduler(); }
  net::NativeUdpService &service() { return backend_.service(); }
  Status executor_status() { return backend_.executor_status(); }
  Result<net::NativeUdpLink> link() { return backend_.link(); }
  bool current(const sched::ProcessControlBlock &process) { return backend_.current(process); }
  Result<i32> install(sched::ProcessControlBlock &process,u32 endpoint,bool cloexec,bool nonblock) { return backend_.install(process,endpoint,cloexec,nonblock); }
  Status submit(std::span<const std::byte> frame) {
    auto ethernet=net::wire::ethernet(frame);CHECK(ethernet);auto ip=net::wire::ipv4(ethernet.value().payload);CHECK(ip);
    const auto &packet=ip.value();CHECK(packet.payload.size()>=8);
    CHECK(net::wire::be16(packet.payload,4)==packet.payload.size());
    CHECK(net::wire::sum(packet.payload,net::wire::pseudo_sum(packet.source,packet.destination,17,static_cast<u16>(packet.payload.size())))==0xffffu);
    transmitted_port=net::wire::be16(packet.payload,2);transmitted_source=net::wire::be16(packet.payload,0);++transmissions;
    expect(kernel_.native_udp().active_sockets()!=0);return {};
  }
private:
  Kernel &kernel_;native::linux_udp::KernelOperationBackend backend_;
};
void after_copy() { if(++callback_stage==callback_at && callback) callback(); }
template <lu::Profile P> i64 error(lu::UdpErrno e) { return -static_cast<i64>(*lu::UdpConstants<P>::errno_number(e)); }
template <lu::Profile P> void input(u16 family=2,u16 port=41000,net::wire::Ipv4 ip=network_link.peer_ip) {
  CHECK(lu::UdpWire<P>::encode_address({reinterpret_cast<u8 *>(mock::memory.data()+address_at-hw::user_begin),16},{family,port,ip}));
}
template <lu::Profile P> void length(u32 bits) {
  for(usize i=0;i<4;++i) mock::memory[length_at-hw::user_begin+i]=std::byte{static_cast<u8>(bits>>(lu::Codec<P>::profile.big_endian?(3-i)*8:i*8))};
}
template <lu::Profile P> u32 length() {
  u32 bits{};for(usize i=0;i<4;++i) bits|=static_cast<u32>(mock::memory[length_at-hw::user_begin+i])<<(lu::Codec<P>::profile.big_endian?(3-i)*8:i*8);return bits;
}
template <lu::Profile P> hw::UserFrame call_frame(Fixture<P> &f,lu::UdpCall call,std::array<u64,6> args) {
  using C=lu::UdpConstants<P>;
  auto frame=f.frame();
  const usize count=call==lu::UdpCall::sendto?6:3;
  if(f.socketcall) {
    const u32 selector=call==lu::UdpCall::connect?lu::udp_connect_subcall:
                       call==lu::UdpCall::getpeername?lu::udp_getpeername_subcall:
                       call==lu::UdpCall::getsockname?lu::udp_getsockname_subcall:11;
    reg<P>(frame,lu::Codec<P>::profile.number_register,*C::socketcall_number());
    reg<P>(frame,lu::Codec<P>::profile.argument_registers[0],selector);
    reg<P>(frame,lu::Codec<P>::profile.argument_registers[1],words);
    for(usize i=0;i<count;++i) memory_word<P>(words+i*lu::Codec<P>::profile.word_bytes,args[i]);
    memory_word<P>(words+count*lu::Codec<P>::profile.word_bytes,~u64{0});
  } else {
    reg<P>(frame,lu::Codec<P>::profile.number_register,*C::syscall_number(call));
    for(usize i=0;i<count;++i) {
      const auto selected=lu::Codec<P>::profile.argument_registers[i];
      if(selected>=0) reg<P>(frame,static_cast<usize>(selected),args[i]);
      else memory_word<P>(frame.sp+i*lu::Codec<P>::profile.word_bytes,args[i]);
    }
  }
  return frame;
}
struct Outcome { Status status{};i64 value{123456};usize publications{}; };
template <lu::Profile P> Outcome invoke(Fixture<P> &f,lu::UdpCall call,std::array<u64,6> args,bool release_fault=false) {
  f.observe(call_frame(f,call,args));
  auto access=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread());CHECK(access);
  if(!f.socketcall && (call==lu::UdpCall::connect || call==lu::UdpCall::getpeername))
    expect(access.value().operation()==(call==lu::UdpCall::connect?native::LinuxUdpImmediateOperation::connect:native::LinuxUdpImmediateOperation::getpeername));
  auto decode=[&]() -> Result<lu::UdpRequest> {
    const auto copies=mock::copies;
    auto request=native::linux_udp::decode_captured<P>(f.thread().native_frame,access.value());CHECK(request);
    expect(request.value().call==call && request.value().fd==static_cast<i32>(static_cast<u32>(args[0])));
    if(f.socketcall) {
      expect(mock::copies==copies+1 && mock::last_input==words && mock::last_input_size==(call==lu::UdpCall::sendto?6:3)*lu::Codec<P>::profile.word_bytes);++indirect;
    }
    callback_stage=0;mock::after_usercopy=callback_at?after_copy:nullptr;
    return request;
  };
  Outcome result;Status failure;
  retired_access_probe=[&] { expect(!access.value().live() && !Fixture<P>::Access::acquire(f.origins,f.process(),f.thread())); };
  auto publish=[&](i64 value)->Status { expect(access.value().live());result.value=value;++result.publications;++published;return {}; };
  native::linux_udp::OperationScratch scratch;
  Backend backend{f.kernel};
  result.status=scratch.run_backend<P>(backend,f.process(),access.value(),decode,publish,failure);
  mock::after_usercopy=nullptr;callback={};callback_at=0;retired_access_probe={};
  if(release_fault) expect(failure.code()==StatusCode::io_error && result.status.code()==failure.code());else CHECK(failure);
  expect(!access.value().live() && !Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()));
  ++invocations;if(!result.status && !result.publications) ++denials;return result;
}
template <lu::Profile P> i64 connect(Fixture<P> &f,i32 count=16,i32 fd=-99,uptr pointer=address_at) {
  auto result=invoke(f,lu::UdpCall::connect,{static_cast<u32>(fd==-99?f.fd:fd),pointer,static_cast<u32>(count)});
  CHECK(result.status);expect(result.publications==1);return result.value;
}
template <lu::Profile P> i64 peername(Fixture<P> &f,uptr name=name_at,uptr len=length_at,i32 fd=-99) {
  auto result=invoke(f,lu::UdpCall::getpeername,{static_cast<u32>(fd==-99?f.fd:fd),name,len});CHECK(result.status);return result.value;
}
template <lu::Profile P> void fresh(Fixture<P> &f) {
  CHECK(f.kernel.posix().activate(f.pid));CHECK(f.kernel.posix().close(f.fd));
  auto opened=f.kernel.native_udp().open();CHECK(opened);f.endpoint=opened.value();
  auto installed=f.kernel.posix().install_native_udp(f.endpoint,false,false);CHECK(installed);f.fd=installed.value();
  f.description=f.process().fd_table().entry(f.fd)->description;
}
template <lu::Profile P> void success_name(Fixture<P> &f,u32 capacity,u16 port=41000) {
  mock::memory.fill(std::byte{0xa7});length<P>(capacity);
  const auto ready=f.kernel.native_udp().readiness(f.endpoint).value();
  const auto binding=f.kernel.native_udp().binding(f.endpoint).value();
  expect(peername(f,capacity?name_at:~uptr{0})==0 && length<P>()==16);
  std::array<u8,16> encoded{};CHECK(lu::UdpWire<P>::encode_address(encoded,{2,port,network_link.peer_ip}));
  for(usize i=0;i<32;++i) expect(mock::memory[name_at-hw::user_begin+i]==(i<std::min<usize>(capacity,16)?std::byte{encoded[i]}:std::byte{0xa7}));
  expect(f.kernel.native_udp().binding(f.endpoint).value()==binding && f.kernel.native_udp().readiness(f.endpoint).value().sequence==ready.sequence);
}
template <lu::Profile P> void state_and_precedence(Kernel &kernel,bool socketcall) {
  Fixture<P> f(kernel,socketcall);++variants;fresh(f);
  const auto untouched=kernel.native_udp().binding(f.endpoint).value();
  const auto before=mock::copies;
  expect(peername(f,0,~uptr{0})==-static_cast<i64>(lu::UdpConstants<P>::not_connected_errno_number()));
  expect(mock::copies==before+(socketcall?1:0));
  input<P>();
  expect(connect(f,16,-1,0)==error<P>(lu::UdpErrno::e_badf));
  expect(connect(f,-1,0,0)==error<P>(lu::UdpErrno::e_inval));
  expect(connect(f,129,0,0)==error<P>(lu::UdpErrno::e_inval));
  expect(connect(f,16,0,0)==error<P>(lu::UdpErrno::e_fault));
  expect(connect(f,16,0)==error<P>(lu::UdpErrno::e_notsock));
  for(i32 count:{-1,129,0,1}) expect(connect(f,count)==error<P>(lu::UdpErrno::e_inval));
  mock::forbidden=address_at;expect(connect(f)==error<P>(lu::UdpErrno::e_fault));mock::forbidden=0;
  expect(kernel.native_udp().binding(f.endpoint).value()==untouched && !kernel.native_udp().peer(f.endpoint).value().associated);
  input<P>(0);expect(connect(f,2)==0);expect(kernel.native_udp().binding(f.endpoint).value()==untouched);
  input<P>();expect(connect(f,2)==error<P>(lu::UdpErrno::e_inval));
  const auto automatic=kernel.native_udp().binding(f.endpoint).value();expect(automatic.bound && automatic.port>=49152 && automatic.address==net::wire::Ipv4{});
  for(i32 count:{3,7,15}) expect(connect(f,count)==error<P>(lu::UdpErrno::e_inval));
  input<P>(10);expect(connect(f)==error<P>(lu::UdpErrno::e_afnosupport));
  input<P>(2,41000,network_link.local_ip);expect(connect(f)==error<P>(lu::UdpErrno::e_netunreach));
  expect(kernel.native_udp().binding(f.endpoint).value()==automatic);
  input<P>(2,0);expect(connect(f)==0);
  expect((kernel.native_udp().peer(f.endpoint).value()==net::NativeUdpPeerSnapshot{true,network_link.peer_ip,0}));
  const auto selected=kernel.native_udp().binding(f.endpoint).value();expect(selected.address==network_link.local_ip && selected.port==automatic.port);
  expect(peername(f,0,~uptr{0})==-static_cast<i64>(lu::UdpConstants<P>::not_connected_errno_number()));
  input<P>(2,41000);expect(connect(f)==0);
  const auto associated=kernel.native_udp().peer(f.endpoint).value();
  input<P>(2,41001,network_link.local_ip);expect(connect(f)==error<P>(lu::UdpErrno::e_netunreach));
  expect(kernel.native_udp().peer(f.endpoint).value()==associated && kernel.native_udp().binding(f.endpoint).value()==selected);
  for(u32 capacity:{0U,1U,2U,7U,15U,16U,17U,0x7fffffffU}) success_name(f,capacity);
  for(u32 capacity:{0x80000000U,0xffffffffU}) { length<P>(capacity);expect(peername(f)==error<P>(lu::UdpErrno::e_inval) && length<P>()==capacity); }
  length<P>(16);mock::forbidden=length_at;expect(peername(f)==error<P>(lu::UdpErrno::e_fault));mock::forbidden=0;
  length<P>(7);mock::write_forbidden=length_at;expect(peername(f)==error<P>(lu::UdpErrno::e_fault) && length<P>()==7);mock::write_forbidden=0;
  expect(mock::memory[name_at-hw::user_begin]!=std::byte{0xa7});
  length<P>(16);expect(peername(f,0)==error<P>(lu::UdpErrno::e_fault));
  length<P>(7);mock::partial_output=1;expect(peername(f)==error<P>(lu::UdpErrno::e_fault) && length<P>()==7);mock::partial_output=0;
  input<P>(0);expect(connect(f,2)==0);expect(kernel.native_udp().binding(f.endpoint).value()==untouched);
  // bind(address,0) retains the explicit address through disconnect and implicit send.
  CHECK(kernel.native_udp().bind_ephemeral(f.endpoint,network_link.local_ip));input<P>();expect(connect(f)==0);
  input<P>(0);expect(connect(f,2)==0);
  expect((kernel.native_udp().binding(f.endpoint).value()==net::NativeUdpBindingSnapshot{false,network_link.local_ip,0}));
  auto sent=invoke(f,lu::UdpCall::sendto,{static_cast<u32>(f.fd),payload_at,0,*lu::UdpConstants<P>::message(lu::UdpMessage::dontwait)});CHECK(sent.status);
  expect(sent.value==error<P>(lu::UdpErrno::e_destaddrreq) && kernel.native_udp().binding(f.endpoint).value().address==network_link.local_ip);
}
template <lu::Profile P> void copy_lifetimes(Kernel &kernel) {
  Fixture<P> f(kernel);input<P>();expect(connect(f)==0);
  // A copied name remains exact if the capacity callback reconnects the live endpoint.
  length<P>(16);callback_at=1;callback=[&] { CHECK(kernel.native_udp().connect(f.endpoint,network_link.peer_ip,42000)); };
  expect(peername(f)==0);
  auto decoded=lu::UdpWire<P>::decode_address({reinterpret_cast<u8 *>(mock::memory.data()+name_at-hw::user_begin),16});CHECK(decoded);expect(decoded.value().port==41000);
  success_name(f,16,42000);
  const auto duplicate=kernel.posix().duplicate(f.fd);CHECK(duplicate);
  length<P>(16);expect(peername(f,name_at,length_at,duplicate.value())==0);CHECK(kernel.posix().close(duplicate.value()));
  const auto child=kernel.scheduler().fork_process(f.pid);CHECK(child);
  auto *child_process=kernel.scheduler().find(child.value());CHECK(child_process);
  const auto *child_slot=child_process->fd_table().entry(f.fd);CHECK(child_slot);
  expect(child_slot->description==f.description && kernel.native_udp().peer(f.endpoint).value().port==42000);
  CHECK(kernel.scheduler().exit_process(child.value(),0));CHECK(kernel.scheduler().reap_process(child.value()));f.select();CHECK(kernel.posix().activate(f.pid));
  const auto queried=f.endpoint;u32 query_replacement{};u16 query_description{};
  length<P>(16);callback_at=1;callback=[&] {
    CHECK(kernel.posix().close(f.fd));expect(kernel.native_udp().peer(queried).ok());
    auto opened=kernel.native_udp().open();CHECK(opened);query_replacement=opened.value();
    auto installed=kernel.posix().install_native_udp(query_replacement,false,false);CHECK(installed);expect(installed.value()==f.fd);
    query_description=f.process().fd_table().entry(f.fd)->description;
  };
  expect(peername(f)==0 && !kernel.native_udp().peer(queried));
  decoded=lu::UdpWire<P>::decode_address({reinterpret_cast<u8 *>(mock::memory.data()+name_at-hw::user_begin),16});CHECK(decoded);expect(decoded.value().port==42000);
  f.endpoint=query_replacement;f.description=query_description;
  // The original OFD is held across close/reuse in the address input callback.
  const auto old=f.endpoint;u32 replacement{};u16 replacement_description{};
  input<P>();callback_at=1;callback=[&] {
    expect(kernel.scheduler().open_description(f.description)->references==2);
    CHECK(kernel.posix().close(f.fd));expect(kernel.native_udp().peer(old).ok());
    auto opened=kernel.native_udp().open();CHECK(opened);replacement=opened.value();
    auto installed=kernel.posix().install_native_udp(replacement,false,false);CHECK(installed);expect(installed.value()==f.fd);
    replacement_description=f.process().fd_table().entry(f.fd)->description;
  };
  expect(connect(f)==0 && !kernel.native_udp().peer(old));
  f.endpoint=replacement;f.description=replacement_description;
  expect(!kernel.native_udp().peer(replacement).value().associated && !kernel.native_udp().binding(replacement).value().bound);
  // Even a non-socket original remains retained until the copy and image lease retire.
  input<P>();const auto console=f.process().fd_table().entry(0)->description;
  const auto references=kernel.scheduler().open_description(console)->references;
  callback_at=1;callback=[&] { expect(kernel.scheduler().open_description(console)->references==references+1);CHECK(kernel.posix().close(0)); };
  expect(connect(f,16,0)==error<P>(lu::UdpErrno::e_notsock));
  const auto *remaining=kernel.scheduler().open_description(console);expect(!remaining || remaining->references==references-1);
}
template <lu::Profile P> void revoke(Kernel &kernel) {
  for(usize stage=0;stage<4;++stage) {
    Fixture<P> f(kernel);fresh(f);input<P>();
    if(stage) { expect(connect(f)==0);length<P>(7); }
    const auto before=kernel.native_udp().peer(f.endpoint).value();
    callback_at=stage?stage:1;callback=[&] { ++f.thread().native_frame.fp_state[23]; };
    auto result=invoke(f,stage?lu::UdpCall::getpeername:lu::UdpCall::connect,
                       stage?std::array<u64,6>{static_cast<u32>(f.fd),name_at,length_at}:std::array<u64,6>{static_cast<u32>(f.fd),address_at,16});
    expect(result.status.code()==StatusCode::denied && result.publications==0 && kernel.native_udp().peer(f.endpoint).value()==before);
    expect(kernel.scheduler().open_description(f.description)->references==1);
    if(!stage) expect(!kernel.native_udp().binding(f.endpoint).value().bound);
  }
}
struct CandidateNode final:fs::Node {
  usize retains{},releases{};bool fail{};std::function<void()> on_release;
  std::string_view name() const override { return "connected-candidate"; }
  fs::Metadata metadata() const override { return {}; }
  Result<fs::FileBuffer> read(usize,usize) const override { return Status::unsupported(); }
  Status write(usize,std::span<const std::byte>) override { return Status::unsupported(); }
  Status retain() override { ++retains;return {}; }
  Status release() override { ++releases;if(on_release) on_release();return fail?Status::io_error("hosted final candidate release failure"):Status{}; }
  fs::Node *lookup(std::string_view) override { return nullptr; }
  Status create(std::string_view,fs::NodeType) override { return Status::unsupported(); }
  Status chmod(u32) override { return Status::unsupported(); }
  Status chown(u32,u32) override { return Status::unsupported(); }
};
template <lu::Profile P> void node_candidate(Kernel &kernel) {
  for(bool fail:{false,true}) {
    Fixture<P> f(kernel);CandidateNode node;node.fail=fail;
    const auto candidate=kernel.scheduler().allocate_description({.readable=true,.node=&node});CHECK(candidate);
    constexpr i32 descriptor=10;CHECK(kernel.scheduler().install_descriptor(f.process().resources(),descriptor,candidate.value()));
    expect(node.retains==1 && node.releases==0);input<P>();
    u16 replacement{},reentrant{};
    callback_at=1;callback=[&] {
      expect(kernel.scheduler().open_description(candidate.value())->references==2 && node.retains==1);
      CHECK(kernel.scheduler().close_descriptor(f.process().resources(),descriptor));expect(node.releases==0);
      auto other=kernel.scheduler().allocate_description({.readable=true});CHECK(other);replacement=other.value();
      CHECK(kernel.scheduler().install_descriptor(f.process().resources(),descriptor,replacement));
    };
    node.on_release=[&] {
      CHECK(retired_access_probe);retired_access_probe();
      expect(!kernel.scheduler().open_description(candidate.value()));
      auto reused=kernel.scheduler().allocate_description({.readable=true});CHECK(reused);reentrant=reused.value();expect(reentrant==candidate.value());
      CHECK(kernel.scheduler().install_descriptor(f.process().resources(),descriptor+1,reentrant));
    };
    auto result=invoke(f,lu::UdpCall::connect,{descriptor,address_at,16},fail);
    expect(result.value==error<P>(lu::UdpErrno::e_notsock) && result.publications==1 && node.retains==1 && node.releases==1);
    expect(kernel.scheduler().open_description(reentrant)->references==1 && kernel.scheduler().open_description(replacement)->references==1);
    CHECK(kernel.scheduler().close_descriptor(f.process().resources(),descriptor));CHECK(kernel.scheduler().close_descriptor(f.process().resources(),descriptor+1));
  }
}
template <lu::Profile P> void default_send_snapshot(Kernel &kernel) {
  for(u16 selected_port:{u16{41000},u16{0}}) for(usize action=0;action<3;++action) {
    Fixture<P> f(kernel);input<P>(2,selected_port);expect(connect(f)==0);
    const auto old_endpoint=f.endpoint;const auto old_binding=kernel.native_udp().binding(old_endpoint).value();
    const auto tx=transmissions;u32 replacement{};u16 replacement_description{};
    callback_at=1;callback=[&] {
      expect(kernel.scheduler().open_description(f.description)->references==2);
      if(action==0) CHECK(kernel.native_udp().connect(old_endpoint,network_link.peer_ip,42000));
      if(action==1) CHECK(kernel.native_udp().disconnect(old_endpoint));
      if(action==2) {
        CHECK(kernel.posix().close(f.fd));expect(kernel.native_udp().peer(old_endpoint).ok());
        auto opened=kernel.native_udp().open();CHECK(opened);replacement=opened.value();
        auto installed=kernel.posix().install_native_udp(replacement,false,false);CHECK(installed);expect(installed.value()==f.fd);
        replacement_description=f.process().fd_table().entry(f.fd)->description;
      }
    };
    auto result=invoke(f,lu::UdpCall::sendto,{static_cast<u32>(f.fd),payload_at,1,*lu::UdpConstants<P>::message(lu::UdpMessage::dontwait)});CHECK(result.status);
    expect(result.value==1 && transmissions==tx+1 && transmitted_port==selected_port && transmitted_source==old_binding.port);
    if(action==0) expect(kernel.native_udp().peer(old_endpoint).value().port==42000);
    if(action==1) expect(!kernel.native_udp().peer(old_endpoint).value().associated);
    if(action==2) {
      expect(!kernel.native_udp().peer(old_endpoint) && !kernel.native_udp().peer(replacement).value().associated && !kernel.native_udp().binding(replacement).value().bound);
      f.endpoint=replacement;f.description=replacement_description;
    }
  }
}
template <lu::Profile P> void zero_default_send(Kernel &kernel,bool socketcall) {
  Fixture<P> f(kernel,socketcall);input<P>(2,0);expect(connect(f)==0);
  const auto binding=kernel.native_udp().binding(f.endpoint).value();
  const auto peer=kernel.native_udp().peer(f.endpoint).value();
  expect(peer.associated && !peer.port && peername(f,0,0)==-static_cast<i64>(lu::UdpConstants<P>::not_connected_errno_number()));
  for(usize count:{usize{0},usize{1},net::NativeUdpService::datagram_capacity}) {
    for(usize i=0;i<count;++i) { mock::memory[payload_at-hw::user_begin+i]=std::byte{0x5a}; }
    f.code(true);const auto before=transmissions;
    auto sent=invoke(f,lu::UdpCall::sendto,{static_cast<u32>(f.fd),payload_at,count,*lu::UdpConstants<P>::message(lu::UdpMessage::dontwait)});CHECK(sent.status);
    if(sent.value!=static_cast<i64>(count) || transmissions!=before+1 || transmitted_port || transmitted_source!=binding.port) std::fprintf(stderr,"ZERO_SEND_DIAGNOSTIC profile=%u indirect=%d count=%zu value=%lld tx=%zu before=%zu port=%u source=%u expected_source=%u\n",static_cast<unsigned>(P),socketcall,count,static_cast<long long>(sent.value),transmissions,before,transmitted_port,transmitted_source,binding.port);
    expect(sent.value==static_cast<i64>(count) && transmissions==before+1 && !transmitted_port && transmitted_source==binding.port);
    expect(kernel.native_udp().binding(f.endpoint).value()==binding && kernel.native_udp().peer(f.endpoint).value()==peer);
  }
  input<P>(2,0);const auto before=transmissions;
  auto explicit_zero=invoke(f,lu::UdpCall::sendto,{static_cast<u32>(f.fd),payload_at,1,*lu::UdpConstants<P>::message(lu::UdpMessage::dontwait),address_at,16});CHECK(explicit_zero.status);
  expect(explicit_zero.value==error<P>(lu::UdpErrno::e_inval) && transmissions==before);
  expect(kernel.native_udp().binding(f.endpoint).value()==binding && kernel.native_udp().peer(f.endpoint).value()==peer);
}
template <lu::Profile P> void blocked_owner(Kernel &kernel) {
  Fixture<P> f(kernel);f.observe(f.frame());f.arm();
  CHECK(kernel.scheduler().install_descriptor(f.process().resources(),f.fd+1,f.description));
  const auto ticket=f.owner().ticket;
  const auto ready=kernel.native_udp().readiness(f.endpoint).value();
  CHECK(kernel.native_udp().connect(f.endpoint,network_link.peer_ip,41000));
  CHECK(kernel.native_udp().disconnect(f.endpoint));
  CHECK(kernel.native_udp().connect(f.endpoint,network_link.peer_ip,42000));
  expect(f.thread().wait_registration.pending(ticket) && kernel.native_udp().readiness(f.endpoint).value().sequence==ready.sequence);
  std::array<std::byte,net::NativeUdpService::frame_capacity> packet{},reply{};
  auto built=net::wire::build_udp(std::span(packet).subspan(14),network_link.peer_ip,network_link.local_ip,41000,f.port,1,64,{});CHECK(built);
  for(usize i=0;i<6;++i) { packet[i]=std::byte{network_link.local_mac[i]};packet[6+i]=std::byte{network_link.peer_mac[i]}; }
  net::wire::be16(packet,12,0x0800);auto filtered=kernel.native_udp().ingest({packet.data(),14+built.value()},reply);CHECK(filtered);
  expect(filtered.value().kind==net::NativeUdpFrameKind::dropped && f.thread().wait_registration.pending(ticket) && kernel.native_udp().readiness(f.endpoint).value().sequence==ready.sequence);
  CHECK(kernel.scheduler().close_descriptor(f.process().resources(),f.fd+1));
  expect(kernel.scheduler().open_description(f.description)->references==2 && kernel.native_udp().peer(f.endpoint).value().port==42000);
  f.retire();expect(kernel.scheduler().open_description(f.description)->references==1);
}
template <lu::Profile P> void exercise(Kernel &kernel) {
  using C=lu::UdpConstants<P>;
  constexpr usize index=static_cast<usize>(P);
  constexpr u32 connect_numbers[]{362,42,203,283,203,203,203,4170,5041,328};
  constexpr u32 peer_numbers[]{368,52,205,287,205,205,205,4171,5051,332};
  expect(*C::syscall_number(lu::UdpCall::connect)==connect_numbers[index] && *C::syscall_number(lu::UdpCall::getpeername)==peer_numbers[index]);
  expect(C::not_connected_errno_number()==(P==lu::Profile::mips || P==lu::Profile::mips64?134U:107U));
  state_and_precedence<P>(kernel,false);
  zero_default_send<P>(kernel,false);
  if(C::socketcall_number()) { state_and_precedence<P>(kernel,true);zero_default_send<P>(kernel,true); }
  copy_lifetimes<P>(kernel);revoke<P>(kernel);node_candidate<P>(kernel);default_send_snapshot<P>(kernel);blocked_owner<P>(kernel);++profiles;
}
}
int main() {
  static_assert(static_cast<u8>(lu::UdpCall::getsockname)==4 && static_cast<u8>(lu::UdpCall::connect)==5 && static_cast<u8>(lu::UdpCall::getpeername)==6);
  static Kernel kernel;CHECK(kernel.boot({}));CHECK(native::entry_stacks.activate(native::entry_stack_arena));CHECK(kernel.native_udp().configure(network_link));
  exercise<lu::Profile::i386>(kernel);exercise<lu::Profile::x86_64>(kernel);exercise<lu::Profile::aarch64>(kernel);exercise<lu::Profile::arm32>(kernel);
  exercise<lu::Profile::rv64>(kernel);exercise<lu::Profile::rv32>(kernel);exercise<lu::Profile::loongarch64>(kernel);exercise<lu::Profile::mips>(kernel);
  exercise<lu::Profile::mips64>(kernel);exercise<lu::Profile::ppc>(kernel);
  expect(profiles==10 && variants==13 && denials==40 && invocations==published+denials);
  expect(kernel.native_udp().active_sockets()==0 && native::entry_stacks.accounting().leased_slots==0);
  std::printf("LINUX_UDP_CONNECTED_CAPTURE_PASS checks=%zu profiles=%zu variants=%zu calls=%zu indirect=%zu denials=%zu hosted=1 guest=0 nic=0\n",checks,profiles,variants,invocations,indirect,denials);
}
