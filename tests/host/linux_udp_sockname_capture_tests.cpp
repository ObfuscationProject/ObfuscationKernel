// Genuine production shared captures/OFD/UDP mechanics, synthetic hosted traps and user pages.
// Immediate name queries do not require restart instruction qualification.
#include "linux_udp_restart_capture_fixture.hpp"
#include <functional>
using namespace udp_restart_model;
namespace {
constexpr uptr name_at = hw::user_begin + 256, length_at = hw::user_begin + 384;
usize queries{}, indirect_queries{}, publications{}, denials{};
std::function<void()> callback_action;
usize callback_stage{}, callback_at{};
void after_copy() {
  if (++callback_stage == callback_at && callback_action) callback_action();
}
template <lu::Profile P> void length(u32 value) {
  // Raw bits, including negative signed capacities, in the target byte order.
  for (usize i=0;i<4;++i)
    mock::memory[length_at-hw::user_begin+i] = std::byte{
      static_cast<u8>(value >> (lu::Codec<P>::profile.big_endian ? (3-i)*8 : i*8))};
}
template <lu::Profile P> u32 length() {
  u32 value{};
  for (usize i=0;i<4;++i)
    value |= static_cast<u32>(mock::memory[length_at-hw::user_begin+i]) <<
      (lu::Codec<P>::profile.big_endian ? (3-i)*8 : i*8);
  return value;
}
template <lu::Profile P> i64 error(lu::UdpErrno e) {
  return -static_cast<i64>(*lu::UdpConstants<P>::errno_number(e));
}
template <lu::Profile P> hw::UserFrame query_frame(Fixture<P> &f, i32 fd,
                                                  uptr address, uptr len) {
  auto frame=f.frame();
  const std::array<u64,3> args{static_cast<u64>(static_cast<u32>(fd)),address,len};
  using C=lu::UdpConstants<P>;
  if (f.socketcall) {
    reg<P>(frame,lu::Codec<P>::profile.number_register,*C::socketcall_number());
    reg<P>(frame,lu::Codec<P>::profile.argument_registers[0],lu::udp_getsockname_subcall);
    reg<P>(frame,lu::Codec<P>::profile.argument_registers[1],words);
    for(usize i=0;i<3;++i) memory_word<P>(words+i*lu::Codec<P>::profile.word_bytes,args[i]);
    // This poison lies beyond the three query arguments and must not be read.
    memory_word<P>(words+3*lu::Codec<P>::profile.word_bytes,~u64{0});
  } else {
    reg<P>(frame,lu::Codec<P>::profile.number_register,*C::syscall_number(lu::UdpCall::getsockname));
    for(usize i=0;i<3;++i) reg<P>(frame,lu::Codec<P>::profile.argument_registers[i],args[i]);
  }
  return frame;
}
struct Outcome { Status status{}; i64 value{123456}; usize published{}; };
template <lu::Profile P> Outcome query(Fixture<P> &f, i32 fd,
                                      uptr address=name_at, uptr len=length_at) {
  f.observe(query_frame(f,fd,address,len));
  auto access=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(access);
  expect(access.value().operation()==(f.socketcall ? native::LinuxUdpImmediateOperation::socketcall :
                                                    native::LinuxUdpImmediateOperation::getsockname));
  auto decode=[&]() -> Result<lu::UdpRequest> {
    const auto copies=mock::copies;
    auto request=native::linux_udp::decode_captured<P>(f.thread().native_frame,access.value());
    CHECK(request);
    expect(request.value()==lu::UdpRequest{.call=lu::UdpCall::getsockname,.fd=fd,
                                          .address=address & lu::Codec<P>::word_mask,.source_length=len & lu::Codec<P>::word_mask});
    expect(mock::copies==copies+(f.socketcall?1:0));
    if(f.socketcall) {
      expect(mock::last_input==words && mock::last_input_size==3*lu::Codec<P>::profile.word_bytes);
      ++indirect_queries;
    }
    callback_stage=0;
    mock::after_usercopy=callback_at?after_copy:nullptr;
    return request;
  };
  Outcome out;
  auto publish=[&](i64 value) -> Status {
    expect(access.value().live()); ++out.published; ++publications; out.value=value; return {};
  };
  Status release_failure;
  native::linux_udp::OperationScratch scratch;
  out.status=scratch.run<P>(f.kernel,f.process(),access.value(),decode,publish,release_failure);
  mock::after_usercopy=nullptr; callback_action={}; callback_at=0;
  CHECK(release_failure);
  expect(!access.value().live() && !Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()));
  ++queries;
  if(!out.status) ++denials;
  return out;
}
template <lu::Profile P> void success(Fixture<P> &f,u32 capacity,
                                      net::NativeUdpBindingSnapshot wanted) {
  mock::memory.fill(std::byte{0xa7}); length<P>(capacity);
  const auto before=f.kernel.native_udp().readiness(f.endpoint); CHECK(before);
  const auto binding=f.kernel.native_udp().binding(f.endpoint); CHECK(binding);
  const auto sockets=f.kernel.native_udp().active_sockets(), dropped=f.kernel.native_udp().dropped_frames();
  const auto copies=mock::copies, outputs=mock::outputs;
  auto out=query(f,f.fd,capacity?name_at:~uptr{0}); CHECK(out.status);
  expect(out.published==1 && out.value==0 && length<P>()==16);
  expect(mock::copies==copies+1+(f.socketcall?1:0) && mock::outputs==outputs+(capacity?2:1));
  std::array<u8,16> bytes{}; CHECK(lu::UdpWire<P>::encode_address(bytes,{2,wanted.port,wanted.address}));
  for(usize i=0;i<32;++i)
    expect(mock::memory[name_at-hw::user_begin+i]==(i<std::min<usize>(capacity,16)?std::byte{bytes[i]}:std::byte{0xa7}));
  const auto after=f.kernel.native_udp().readiness(f.endpoint); CHECK(after);
  expect(after.value().sequence==before.value().sequence &&
         after.value().queued_datagrams==before.value().queued_datagrams &&
         after.value().terminal==before.value().terminal);
  expect(f.kernel.native_udp().binding(f.endpoint).value()==binding.value() &&
         f.kernel.native_udp().active_sockets()==sockets && f.kernel.native_udp().dropped_frames()==dropped);
  expect(f.kernel.scheduler().open_description(f.description)->references==1);
}
template <lu::Profile P> void fresh_endpoint(Fixture<P> &f) {
  CHECK(f.kernel.posix().activate(f.pid)); CHECK(f.kernel.posix().close(f.fd));
  expect(!f.kernel.native_udp().binding(f.endpoint));
  auto opened=f.kernel.native_udp().open(); CHECK(opened); f.endpoint=opened.value();
  auto installed=f.kernel.posix().install_native_udp(f.endpoint,false,false); CHECK(installed);
  f.fd=installed.value(); f.description=f.process().fd_table().entry(f.fd)->description;
}
template <lu::Profile P> void capacities_and_faults(Kernel &kernel,bool indirect) {
  Fixture<P> f(kernel,indirect); ++variants;
  for(u32 capacity:{0U,1U,2U,3U,7U,15U,16U,17U,0x7fffffffU})
    success(f,capacity,kernel.native_udp().binding(f.endpoint).value());
  for(u32 capacity:{0x80000000U,0xffffffffU}) {
    mock::memory.fill(std::byte{0xa7}); length<P>(capacity);
    const auto outputs=mock::outputs; auto out=query(f,f.fd); CHECK(out.status);
    expect(out.value==error<P>(lu::UdpErrno::e_inval) && out.published==1 && length<P>()==capacity && mock::outputs==outputs);
  }
  // Descriptor/type checks precede even an invalid user length pointer.
  for(i32 fd:{-1,31,0}) {
    const auto copies=mock::copies,outputs=mock::outputs;
    auto out=query(f,fd,0,~uptr{0}); CHECK(out.status);
    expect(out.value==error<P>(fd==0?lu::UdpErrno::e_notsock:lu::UdpErrno::e_badf));
    expect(mock::copies==copies+(indirect?1:0) && mock::outputs==outputs);
  }
  const auto faults=[&](uptr address,uptr len,uptr read_fault,uptr write_fault,bool name_written) {
    mock::memory.fill(std::byte{0xa7}); length<P>(16);
    mock::forbidden=read_fault; mock::write_forbidden=write_fault;
    auto out=query(f,f.fd,address,len); CHECK(out.status);
    expect(out.value==error<P>(lu::UdpErrno::e_fault) && out.published==1 && length<P>()==16);
    expect((mock::memory[name_at-hw::user_begin]!=std::byte{0xa7})==name_written);
    mock::forbidden=mock::write_forbidden=0;
  };
  faults(name_at,0,0,0,false); // Null length input.
  faults(name_at,length_at,length_at,0,false);
  faults(0,length_at,0,0,false);
  faults(name_at,length_at,0,name_at,false);
  faults(name_at,length_at,0,length_at,true); // Address prefix persists after length copyout fault.
  // Partial address copy also persists, but does not publish the actual length.
  mock::memory.fill(std::byte{0xa7}); length<P>(7); mock::partial_output=1;
  auto partial=query(f,f.fd); CHECK(partial.status); mock::partial_output=0;
  expect(partial.value==error<P>(lu::UdpErrno::e_fault) && length<P>()==7);
  expect(mock::memory[name_at-hw::user_begin]!=std::byte{0xa7} && mock::memory[name_at-hw::user_begin+1]==std::byte{0xa7});
  // Query neither consumes nor advances a queued datagram's readiness sequence.
  net::NativeUdpService peer; CHECK(peer.configure({network_link.peer_mac,network_link.local_mac,network_link.peer_ip,network_link.local_ip}));
  auto sender=peer.open(); CHECK(sender); CHECK(peer.bind(sender.value(),{},41000));
  std::array<std::byte,net::NativeUdpService::frame_capacity> packet{},reply{};
  const std::array<std::byte,1> byte{std::byte{'q'}};
  auto built=peer.build_datagram(sender.value(),network_link.local_ip,f.port,byte,packet); CHECK(built);
  auto ingested=kernel.native_udp().ingest({packet.data(),built.value()},reply); CHECK(ingested);
  expect(ingested.value().queued_handle==f.endpoint);
  success(f,16,kernel.native_udp().binding(f.endpoint).value());
  expect(kernel.native_udp().peek(f.endpoint).value().payload[0]==std::byte{'q'});
  CHECK(peer.close(sender.value()));
}
template <lu::Profile P> void bindings_and_lifetimes(Kernel &kernel) {
  Fixture<P> f(kernel); fresh_endpoint(f);
  success(f,16,{false,{},0});
  // Copy the name before a callback can bind this same retained endpoint.
  length<P>(16); callback_at=1; callback_action=[&] { CHECK(kernel.native_udp().bind_ephemeral(f.endpoint,network_link.local_ip)); };
  auto before_bind=query(f,f.fd); CHECK(before_bind.status); expect(before_bind.value==0);
  auto returned=lu::UdpWire<P>::decode_address({reinterpret_cast<u8 *>(mock::memory.data()+name_at-hw::user_begin),16}); CHECK(returned);
  expect(returned.value().port==0 && returned.value().address==net::wire::Ipv4{});
  success(f,16,kernel.native_udp().binding(f.endpoint).value());
  fresh_endpoint(f); CHECK(kernel.native_udp().bind_ephemeral(f.endpoint,{}));
  const auto ephemeral=kernel.native_udp().binding(f.endpoint).value(); expect(ephemeral.bound && ephemeral.port>=32768 && ephemeral.address==net::wire::Ipv4{});
  success(f,16,ephemeral);
  // Canonical alias is queried through its shared description.
  const auto duplicate=kernel.posix().duplicate(f.fd); CHECK(duplicate);
  length<P>(16); auto alias=query(f,duplicate.value()); CHECK(alias.status); expect(alias.value==0);
  expect(kernel.scheduler().open_description(f.description)->references==2);
  CHECK(kernel.posix().close(duplicate.value()));
  // A capacity-read callback reuses the fd. The original OFD remains held until cleanup.
  const auto old_endpoint=f.endpoint,old_description=f.description; u32 replacement{}; i32 replacement_fd{};
  length<P>(16); callback_at=1; callback_action=[&] {
    expect(kernel.scheduler().open_description(old_description)->references==2);
    CHECK(kernel.posix().close(f.fd)); expect(kernel.native_udp().binding(old_endpoint).ok());
    auto opened=kernel.native_udp().open(); CHECK(opened); replacement=opened.value();
    CHECK(kernel.native_udp().bind(replacement,network_link.local_ip,55001));
    auto installed=kernel.posix().install_native_udp(replacement,false,false); CHECK(installed); replacement_fd=installed.value();
    expect(replacement_fd==f.fd && kernel.scheduler().open_description(old_description)->references==1);
  };
  auto reused=query(f,f.fd); CHECK(reused.status); expect(reused.value==0);
  returned=lu::UdpWire<P>::decode_address({reinterpret_cast<u8 *>(mock::memory.data()+name_at-hw::user_begin),16}); CHECK(returned);
  expect(returned.value().port==ephemeral.port && returned.value().address==ephemeral.address);
  expect(!kernel.native_udp().binding(old_endpoint) && kernel.native_udp().binding(replacement).value().port==55001);
  f.endpoint=replacement; f.description=f.process().fd_table().entry(replacement_fd)->description;
  success(f,16,{true,network_link.local_ip,55001});
}
template <lu::Profile P> void revocation(Kernel &kernel) {
  for(usize stage:{1U,2U,3U}) {
    Fixture<P> f(kernel); mock::memory.fill(std::byte{0xa7}); length<P>(7);
    const auto outputs=mock::outputs; callback_at=stage;
    callback_action=[&] { ++f.thread().native_frame.fp_state[23]; };
    auto out=query(f,f.fd); expect(out.status.code()==StatusCode::denied && out.published==0);
    expect(mock::outputs==outputs+stage-1 && length<P>()==(stage==3?16U:7U));
    expect(kernel.scheduler().open_description(f.description)->references==1);
  }
  // Each immediate access is genuinely captured first, then exact authority is revoked.
  for(usize mode=0;mode<7;++mode) {
    Fixture<P> f(kernel); f.observe(query_frame(f,f.fd,name_at,length_at));
    auto held=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(held);
    expect(!Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()));
    typename Fixture<P>::Access access{std::move(held.value())};
    expect(!held.value().live() && access.live());
    std::array<std::byte,4> scratch{};
    expect(!held.value().read(length_at,scratch));
    const auto copies=mock::copies,outputs=mock::outputs;
    if(mode==0) ++f.thread().native_frame.registers[3];
    if(mode==1) ++f.process().resources().native_space.generation;
    if(mode==2) CHECK(f.origins.prepare(f.thread()));
    if(mode==3) CHECK(kernel.scheduler().activate_thread(f.pid,f.pid));
    if(mode==4) {
      auto view=native::entry_stacks.activate_entry(f.thread().entry_stack,{f.pid,f.pid},0); CHECK(view);
      CHECK(native::entry_stacks.deactivate_entry(f.thread().entry_stack,{f.pid,f.pid},0));
    }
    if(mode==5) f.thread().native_active=false;
    if(mode==6) f.origins.revoke(f.thread());
    expect(!access.live() && !access.read(length_at,scratch) && !access.write(length_at,scratch));
    expect(!native::linux_udp::decode_captured<P>(f.thread().native_frame,access));
    expect(mock::copies==copies && mock::outputs==outputs);
    f.thread().native_active=true;
  }
  Fixture<P> f(kernel); f.observe(query_frame(f,f.fd,name_at,length_at));
  auto held=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(held);
  auto changed=f.thread().native_frame;
  reg<P>(changed,lu::Codec<P>::profile.number_register,*lu::UdpConstants<P>::syscall_number(lu::UdpCall::socket));
  const auto copies=mock::copies;
  expect(!native::linux_udp::decode_captured<P>(changed,held.value()) && held.value().live() && mock::copies==copies);
  held.value().reset();
  // A number absent from the immediate classifier cannot acquire query authority.
  auto unknown=query_frame(f,f.fd,name_at,length_at); reg<P>(unknown,lu::Codec<P>::profile.number_register,0x7fffffffU);
  f.observe(unknown); expect(!Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()));
}
template <lu::Profile P> void decode_rejections(Kernel &kernel) {
  {
    Fixture<P> foreign(kernel),f(kernel);
    f.observe(query_frame(f,f.fd,name_at,length_at));
    expect(!Fixture<P>::Access::acquire(f.origins,foreign.process(),foreign.thread()));
    expect(!Fixture<P>::Access::acquire(f.origins,foreign.process(),f.thread()));
    auto exact=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread());CHECK(exact);exact.value().reset();
  }
  if(!lu::UdpConstants<P>::socketcall_number()) return;
  for(usize mode=0;mode<6;++mode) {
    Fixture<P> f(kernel,true);auto frame=query_frame(f,f.fd,name_at,length_at);
    if(mode==1) reg<P>(frame,lu::Codec<P>::profile.argument_registers[1],lu::Codec<P>::word_mask-1);
    if(mode==2) reg<P>(frame,lu::Codec<P>::profile.argument_registers[0],8);
    if(mode==3) reg<P>(frame,lu::Codec<P>::profile.argument_registers[0],0);
    f.observe(frame);auto exact=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread());CHECK(exact);
    if(mode==0) mock::forbidden=words+lu::Codec<P>::profile.word_bytes;
    callback_stage=0;callback_at=1;
    if(mode==4) callback_action=[&] { ++f.thread().native_frame.fp_state[23]; };
    if(mode==5) callback_action=[&] {
      for(usize i=0;i<3*lu::Codec<P>::profile.word_bytes;++i) mock::memory[words-hw::user_begin+i]=std::byte{};
    };
    mock::after_usercopy=mode>=4?after_copy:nullptr;
    const auto copies=mock::copies,outputs=mock::outputs;
    auto decoded=native::linux_udp::decode_captured<P>(f.thread().native_frame,exact.value());
    mock::after_usercopy=nullptr;callback_action={};callback_at=0;mock::forbidden=0;
    expect(mock::outputs==outputs && mock::copies==copies+((mode==0 || mode>=4)?1:0));
    if(mode==5) expect(decoded && decoded.value()==lu::UdpRequest{.call=lu::UdpCall::getsockname,
                                      .fd=f.fd,.address=name_at,.source_length=length_at});
    else {
      expect(!decoded);
      expect(decoded.status().code()==(mode<2?StatusCode::fault:mode==2?StatusCode::unsupported:
                                      mode==3?StatusCode::invalid_argument:StatusCode::denied));
    }
    expect(exact.value().live()==(mode!=4));exact.value().reset();
  }
}
template <lu::Profile P> void exercise(Kernel &kernel) {
  capacities_and_faults<P>(kernel,false);
  if(lu::UdpConstants<P>::socketcall_number()) capacities_and_faults<P>(kernel,true);
  bindings_and_lifetimes<P>(kernel); revocation<P>(kernel); decode_rejections<P>(kernel); ++profiles;
}
}
int main() {
  static Kernel kernel; CHECK(kernel.boot({}));
  CHECK(native::entry_stacks.activate(native::entry_stack_arena)); CHECK(kernel.native_udp().configure(network_link));
  exercise<lu::Profile::i386>(kernel); exercise<lu::Profile::x86_64>(kernel);
  exercise<lu::Profile::aarch64>(kernel); exercise<lu::Profile::arm32>(kernel);
  exercise<lu::Profile::rv64>(kernel); exercise<lu::Profile::rv32>(kernel);
  exercise<lu::Profile::loongarch64>(kernel); exercise<lu::Profile::mips>(kernel);
  exercise<lu::Profile::mips64>(kernel); exercise<lu::Profile::ppc>(kernel);
  expect(profiles==10 && variants==13 && denials==30 && queries==publications+denials);
  expect(kernel.native_udp().active_sockets()==0 && native::entry_stacks.accounting().leased_slots==0);
  std::printf("LINUX_UDP_SOCKNAME_CAPTURE_PASS checks=%zu profiles=%zu variants=%zu queries=%zu captures=%zu indirect_queries=%zu publications=%zu denials=%zu hosted=1 guest=0 nic=0\n",
              checks,profiles,variants,queries,captures,indirect_queries,publications,denials);
}
