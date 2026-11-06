// Hosted real scheduler/service models with synthetic image and device access.
#include "../../src/user/linux_udp_operation_body.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>
using namespace ok;
namespace lu = abi::linux_user;
namespace udp = user::native::linux_udp;
static usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "UDP_BODY_FAIL line=%d checks=%zu: %s\n", __LINE__, checks, #x); std::abort(); } } while (false)
static_assert(!std::is_copy_constructible_v<udp::OperationScratch> && !std::is_move_constructible_v<udp::OperationScratch>);
namespace ok::arch::execution { Status destroy_space(AddressSpace &space) { space = {}; return {}; } }
constexpr net::NativeUdpLink udp_link{{2,1,2,3,4,5},{2,6,7,8,9,10},{192,0,2,2},{192,0,2,1}};
constexpr u64 payload_at=64, address_at=1800, length_at=1840;

struct Memory
{
    struct Event { bool write{}; u64 address{}; usize size{}; };
    std::array<std::byte,4096> bytes{};
    std::array<Event,12> events{};
    usize calls{}, ranges{}, fail{}, revoke{}, resets{};
    bool active{true};
    void *context{};
    void (*hook)(Memory &){};
    void (*reset_hook)(Memory &){};
    bool live() const { return active; }
    void reset() { ++resets; active=false; if (reset_hook) reset_hook(*this); }
    Status range(uptr pointer,usize count,bool)
    {
        ++ranges;
        return pointer > 0xffffffffU || count > 0xffffffffU-pointer
            ? Status::fault("synthetic range wrap") : Status{};
    }
    Status copy(bool write,u64 pointer,std::span<std::byte> input,std::span<const std::byte> output)
    {
        const auto size=write?output.size():input.size();
        CHECK(calls<events.size());events[calls++]={write,pointer,size};
        if(hook)hook(*this);
        if(!active || (size && (!pointer || pointer>bytes.size() || size>bytes.size()-pointer)))
            return Status::fault("synthetic mapped copy fault");
        const auto copied=calls==fail?size/2:size;
        for(usize at=0;at<copied;++at)
            if(write)bytes[pointer+at]=output[at];else input[at]=bytes[pointer+at];
        if(calls==revoke)active=false;
        return calls==fail?Status::fault("synthetic partial copy"):Status{};
    }
    Status read(u64 pointer,std::span<std::byte> bytes) { return copy(false,pointer,bytes,{}); }
    Status write(u64 pointer,std::span<const std::byte> bytes) { return copy(true,pointer,{},bytes); }
    template<lu::Profile P> void address(u16 family,u16 port,net::wire::Ipv4 ip,usize length=16)
    {
        CHECK(lu::UdpWire<P>::encode_address({reinterpret_cast<u8 *>(bytes.data()+address_at),16},{family,port,ip}));
        for(usize at=16;at<length;++at)bytes[address_at+at]=std::byte{0x9d};
    }
    template<lu::Profile P> void address6(u16 family,u16 port,const net::wire::Ipv6 &ip,usize length=28)
    {
        CHECK(lu::Ipv6SocketAddressCodec<P>::encode({reinterpret_cast<u8 *>(bytes.data()+address_at),28},
            {family,port,0x01020304u,ip,7}));
        for(usize at=28;at<length;++at)bytes[address_at+at]=std::byte{0x9d};
    }
    template<lu::Profile P> void length(u32 size)
    {
        CHECK(lu::UdpWire<P>::encode_socklen({reinterpret_cast<u8 *>(bytes.data()+length_at),4},size));
    }
};

struct Fixture
{
    std::unique_ptr<sched::Scheduler> owner{std::make_unique<sched::Scheduler>()};
    net::EthernetInterface nic{};
    net::NativeUdpService network,peer;
    udp::OperationScratch scratch;
    sched::ProcessId pid{};
    Status installation{},submission{},executor{},release_failure{},link_status{};
    usize installs{},submits{},published{},decoded{},links{};
    void *link_context{};
    void (*link_hook)(Fixture &){};
    void (*submit_hook)(Fixture &){};
    i64 value{};
    bool allow_current{true},close_on_publish{};
    u32 created_endpoint{},sender{};
    u16 held_description{sched::invalid_description};
    std::array<std::byte,net::NativeUdpService::frame_capacity> transmitted{},incoming{},reply{};
    usize transmitted_size{};
    // An attached interface admits IPv6 sockets and settles its link-local address.
    explicit Fixture(bool configured=true,bool interface=false)
    {
        auto result=owner->create_user_process("udp-body-host-model",arch::configured_arch_operations().make_user_context(
            {arch::execution::user_begin,arch::execution::user_begin+4096}));CHECK(result);pid=result.value();
        if(interface)CHECK(network.attach_interface(nic));
        if(configured)CHECK(network.configure(udp_link));
        if(interface){CHECK(nic.tick(0)&&nic.tick(1000));while(nic.control())nic.pop_control();}
        CHECK(peer.configure({udp_link.peer_mac,udp_link.local_mac,udp_link.peer_ip,udp_link.local_ip}));
        CHECK(owner->set_native_udp_service(network));auto opened=peer.open();CHECK(opened);sender=opened.value();
        CHECK(peer.bind(sender,{},41000));
    }
    ~Fixture() { CHECK(owner->drain_resources(process().resources())); }
    sched::ProcessControlBlock &process() { return *owner->find(pid); }
    sched::SchedulerState &scheduler() { return *owner; }
    net::NativeUdpService &service() { return network; }
    Status executor_status() { return executor; }
    Result<net::NativeUdpLink> link() { ++links;if(link_hook)link_hook(*this);return link_status?Result<net::NativeUdpLink>{udp_link}:Result<net::NativeUdpLink>{link_status}; }
    bool current(sched::ProcessControlBlock &process) { return allow_current && &process==owner->find(pid); }
    Result<i32> install(sched::ProcessControlBlock &process,u32 endpoint,bool cloexec,bool nonblock)
    {
        ++installs;created_endpoint=endpoint;
        if(!installation)return installation;
        i32 next=3;while(process.fd_table().entry(next)&&process.fd_table().entry(next)->used)++next;
        if(!process.fd_table().entry(next))return Status::process_file_limit();
        auto allocated=owner->allocate_description({.readable=true,.writable=true,.flags=posix::o_RDWR|(nonblock?posix::o_NONBLOCK:0),
                                                    .native_udp_handle=endpoint});if(!allocated)return allocated.status();
        CHECK(owner->install_descriptor(process.resources(),next,allocated.value(),cloexec));return next;
    }
    Status submit(std::span<const std::byte> frame)
    {
        if(submit_hook)submit_hook(*this);
        ++submits;transmitted_size=frame.size();CHECK(frame.size()<=transmitted.size());
        for(usize at=0;at<frame.size();++at)transmitted[at]=frame[at];
        return submission;
    }
    template<lu::Profile P> Status run(Memory &memory,lu::UdpRequest request,Status decode_status={})
    {
        auto decode=[&]() -> Result<lu::UdpRequest> { ++decoded;return decode_status?Result<lu::UdpRequest>{request}:Result<lu::UdpRequest>{decode_status}; };
        auto publish=[&](i64 result) -> Status {
            ++published;CHECK(memory.live());CHECK(memory.resets==0);
            if(held_description!=sched::invalid_description)CHECK(owner->open_description(held_description));
            value=result;
            if(close_on_publish)CHECK(owner->close_descriptor(process().resources(),3));
            return {};
        };
        return scratch.run_backend<P>(*this,process(),memory,decode,publish,release_failure);
    }
    template<lu::Profile P> i32 socket(bool cloexec=false,i32 domain=2)
    {
        Memory memory;CHECK(run<P>(memory,{.call=lu::UdpCall::socket,.domain=domain,
            .type=static_cast<i32>(lu::UdpConstants<P>::sock_dgram|lu::UdpConstants<P>::sock_nonblock|
                                  (cloexec?lu::UdpConstants<P>::sock_cloexec:0))}));
        CHECK(memory.resets==1);CHECK(value>=3);return static_cast<i32>(value);
    }
    u32 endpoint(i32 fd=3)
    {
        auto *slot=process().fd_table().entry(fd);CHECK(slot&&slot->used);
        return owner->open_description(slot->description)->native_udp_handle;
    }
    void enqueue(usize size)
    {
        std::array<std::byte,net::NativeUdpService::datagram_capacity> data{};
        for(usize at=0;at<size;++at)data[at]=std::byte{static_cast<u8>(at*17+3)};
        auto built=peer.build_datagram(sender,udp_link.local_ip,40000,{data.data(),size},incoming);CHECK(built);
        auto queued=network.ingest({incoming.data(),built.value()},reply);CHECK(queued);
        CHECK(queued.value().kind==net::NativeUdpFrameKind::udp_queued);
    }
    usize queued() { auto state=network.readiness(endpoint());CHECK(state);return state.value().queued_datagrams; }
};
template<lu::Profile P> i64 error(lu::UdpErrno value) { return -static_cast<i64>(*lu::UdpConstants<P>::errno_number(value)); }

template<lu::Profile P> void sockets()
{
    using C=lu::UdpConstants<P>;Fixture f;
    const auto run=[&](i32 domain,u32 type,i32 protocol,lu::UdpErrno expected) {
        Memory memory;auto count=f.network.active_sockets();CHECK(f.run<P>(memory,{.call=lu::UdpCall::socket,.domain=domain,
                .type=static_cast<i32>(type),.protocol=protocol}));CHECK(f.value==error<P>(expected));
        CHECK(memory.resets==1&&memory.calls==0&&f.network.active_sockets()==count);
    };
    run(1,C::sock_dgram|C::sock_nonblock,0,lu::UdpErrno::e_afnosupport);
    run(3,C::sock_dgram|C::sock_nonblock,0,lu::UdpErrno::e_afnosupport);
    // Without the attached interface an IPv6 socket is refused before install.
    run(10,C::sock_dgram|C::sock_nonblock,0,lu::UdpErrno::e_opnotsupp);
    run(2,C::sock_dgram|C::sock_nonblock|0x40000000,0,lu::UdpErrno::e_inval);
    run(2,C::sock_stream|C::sock_nonblock,0,lu::UdpErrno::e_socktnosupport);
    run(2,C::sock_dgram|C::sock_nonblock,6,lu::UdpErrno::e_protonosupport);
    Memory blocking;CHECK(f.run<P>(blocking,{.call=lu::UdpCall::socket,.domain=2,.type=static_cast<i32>(C::sock_dgram)}));
    CHECK(f.value==3&&blocking.resets==1&&blocking.calls==0);
    const auto *blocking_slot=f.process().fd_table().entry(3);CHECK(blocking_slot&&blocking_slot->used);
    auto *blocking_open=f.owner->open_description(blocking_slot->description);CHECK(blocking_open);
    CHECK(!(blocking_open->flags&posix::o_NONBLOCK));
    CHECK(f.owner->install_descriptor(f.process().resources(),4,blocking_slot->description));
    blocking_open->flags|=posix::o_NONBLOCK; // Hosted OFD status-change control.
    CHECK(f.owner->open_description(f.process().fd_table().entry(4)->description)->flags&posix::o_NONBLOCK);
    CHECK(f.owner->close_descriptor(f.process().resources(),4));CHECK(f.owner->close_descriptor(f.process().resources(),3));
    auto fd=f.socket<P>(true);auto *slot=f.process().fd_table().entry(fd);CHECK(slot->close_on_exec);
    auto *open=f.owner->open_description(slot->description);CHECK(open->flags&posix::o_NONBLOCK);
    CHECK(f.owner->install_descriptor(f.process().resources(),4,slot->description));
    CHECK(!f.process().fd_table().entry(4)->close_on_exec);CHECK(open->references==2);
    for(auto status:{Status::process_file_limit(),Status::system_file_limit(),Status::no_memory()})
    {
        f.installation=status;Memory memory;const auto count=f.network.active_sockets();
        CHECK(f.run<P>(memory,{.call=lu::UdpCall::socket,.domain=2,.type=static_cast<i32>(C::sock_dgram|C::sock_nonblock)}));
        const auto expected=status.code()==StatusCode::process_file_limit?lu::UdpErrno::e_mfile:
            status.code()==StatusCode::system_file_limit?lu::UdpErrno::e_nfile:lu::UdpErrno::e_nomem;
        CHECK(f.value==error<P>(expected));CHECK(f.network.active_sockets()==count);CHECK(!f.network.binding(f.created_endpoint));
    }
    f.installation={};while(f.network.active_sockets()<net::NativeUdpService::socket_capacity)f.socket<P>();
    Memory full;CHECK(f.run<P>(full,{.call=lu::UdpCall::socket,.domain=2,.type=static_cast<i32>(C::sock_dgram|C::sock_nonblock)}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_nomem));
    CHECK(f.links==0);
}

template<lu::Profile P> void configure_first_socket()
{
    using C=lu::UdpConstants<P>;
    const lu::UdpRequest request{.call=lu::UdpCall::socket,.domain=2,.type=static_cast<i32>(C::sock_dgram|C::sock_nonblock)};
    Fixture configured(false);Memory good;CHECK(configured.run<P>(good,request));
    CHECK(configured.value==3&&configured.links==1&&configured.network.configured()&&configured.installs==1);
    configured.socket<P>();CHECK(configured.links==1);
    for(auto status:{Status::not_initialized(),Status::io_error(),Status::fault()})
    {
        Fixture failed(false);failed.link_status=status;Memory memory;CHECK(failed.run<P>(memory,request));
        CHECK(failed.value==error<P>(status.code()==StatusCode::not_initialized?lu::UdpErrno::e_netdown:
                                   status.code()==StatusCode::fault?lu::UdpErrno::e_fault:lu::UdpErrno::e_io));
        CHECK(failed.links==1&&!failed.network.configured()&&failed.installs==0&&failed.network.active_sockets()==0);
    }
    Fixture retired(false);retired.executor=Status::io_error();Memory rejected;CHECK(retired.run<P>(rejected,request));
    CHECK(retired.value==error<P>(lu::UdpErrno::e_io)&&retired.links==0&&retired.installs==0);
    Fixture lost(false);Memory authority;lost.link_context=&authority;
    lost.link_hook=[](Fixture &f){static_cast<Memory *>(f.link_context)->active=false;};
    CHECK(lost.run<P>(authority,request).code()==StatusCode::denied);
    CHECK(!lost.network.configured()&&lost.network.active_sockets()==0&&lost.installs==0&&lost.published==0&&authority.resets==1);
}

template<lu::Profile P> void binds()
{
    Fixture f;f.socket<P>();
    const auto run=[&](u16 family,u16 port,net::wire::Ipv4 ip,i32 length,lu::UdpErrno expected,bool fail=false) {
        Memory memory;memory.address<P>(family,port,ip,128);if(fail)memory.fail=1;
        CHECK(f.run<P>(memory,{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=length}));
        CHECK(f.value==error<P>(expected));CHECK(!f.network.binding(f.endpoint()).value().bound);
        CHECK(memory.calls==static_cast<usize>(length>0&&length<=128));
    };
    run(2,40000,{},-1,lu::UdpErrno::e_inval);run(2,40000,{},129,lu::UdpErrno::e_inval);
    run(2,40000,{},0,lu::UdpErrno::e_inval);
    run(2,40000,{},15,lu::UdpErrno::e_inval);run(2,40000,{},128,lu::UdpErrno::e_fault,true);
    run(10,40000,{},16,lu::UdpErrno::e_afnosupport);run(0,40000,udp_link.local_ip,16,lu::UdpErrno::e_afnosupport);
    run(2,40000,udp_link.peer_ip,16,lu::UdpErrno::e_addrnotavail);
    run(2,100,{},16,lu::UdpErrno::e_acces);
    Memory good;good.address<P>(0,40000,{},128);CHECK(f.run<P>(good,{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=128}));
    CHECK(f.value==0);CHECK(good.events[0].size==128);
    Memory repeat;repeat.address<P>(2,40001,{});CHECK(f.run<P>(repeat,{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=16}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_inval));f.socket<P>();
    Memory conflict;conflict.address<P>(2,40000,{});CHECK(f.run<P>(conflict,{.call=lu::UdpCall::bind,.fd=4,.address=address_at,.address_length=16}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_addrinuse));
    Memory zero;CHECK(f.run<P>(zero,{.call=lu::UdpCall::bind,.fd=4,.address=lu::Codec<P>::word_mask,.address_length=0}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_inval)&&zero.calls==0);
    // The accepted autobind successor supports port zero; preserve this model
    // as a current positive ephemeral-bind check rather than a stale rejection.
    Fixture ephemeral;ephemeral.socket<P>();Memory dynamic;dynamic.address<P>(2,0,{});
    CHECK(ephemeral.run<P>(dynamic,{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=16}));
    CHECK(ephemeral.value==0&&dynamic.calls==1);
    const auto selected=ephemeral.network.binding(ephemeral.endpoint());CHECK(selected&&selected.value().bound&&selected.value().port>=1024);
}

template<lu::Profile P> void sends_and_range()
{
    Fixture f;f.socket<P>();
    for(usize size:{usize{0},usize{1},usize{1472}})
    {
        Memory memory;memory.address<P>(0,42000,udp_link.peer_ip);for(usize at=0;at<size;++at)memory.bytes[payload_at+at]=std::byte{static_cast<u8>(at)};
        CHECK(f.run<P>(memory,{.call=lu::UdpCall::sendto,.fd=3,.payload=payload_at,.count=size,
            .flags=lu::UdpConstants<P>::send_flags_mask(),.address=address_at,.address_length=16}));
        CHECK(f.value==static_cast<i64>(size));CHECK(memory.ranges==1&&memory.calls==2);CHECK(f.submits==usize(size==0?1:size==1?2:3));
        auto ethernet=net::wire::ethernet({f.transmitted.data(),f.transmitted_size});CHECK(ethernet);
        auto ip=net::wire::ipv4(ethernet.value().payload);CHECK(ip);auto packet=net::wire::udp(ip.value());CHECK(packet);
        CHECK(packet.value().payload.size()==size);for(usize at=0;at<size;++at)CHECK(packet.value().payload[at]==memory.bytes[payload_at+at]);
    }
    for(auto status:{Status::would_block(),Status::io_error(),Status::not_initialized()})
    {
        Memory memory;memory.address<P>(2,42000,udp_link.peer_ip);f.submission=status;
        CHECK(f.run<P>(memory,{.call=lu::UdpCall::sendto,.fd=3,.payload=payload_at,.count=1,.address=address_at,.address_length=16}));
        CHECK(f.value==error<P>(status.code()==StatusCode::would_block?lu::UdpErrno::e_again:
                               status.code()==StatusCode::not_initialized?lu::UdpErrno::e_netdown:lu::UdpErrno::e_io));
    }
    Memory null;CHECK(f.run<P>(null,{.call=lu::UdpCall::sendto,.fd=3,.payload=payload_at,.count=1,.address_length=-1}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_destaddrreq)&&null.calls==0);
    Memory invalid;CHECK(f.run<P>(invalid,{.call=lu::UdpCall::sendto,.fd=-1,.payload=6000,.count=1,.address=address_at,.address_length=16}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_badf)&&invalid.ranges==1&&invalid.calls==0);
    Memory wrap;CHECK(f.run<P>(wrap,{.call=lu::UdpCall::recvfrom,.fd=-1,.payload=lu::Codec<P>::word_mask,.count=2}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_fault)&&wrap.calls==0);
    Memory nonsocket;CHECK(f.run<P>(nonsocket,{.call=lu::UdpCall::bind,.fd=0,.address=address_at,.address_length=16}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_notsock)&&nonsocket.calls==0);
    for(auto item:{lu::UdpErrno::e_msgsize,lu::UdpErrno::e_netunreach,lu::UdpErrno::e_opnotsupp,lu::UdpErrno::e_inval})
    {
        Memory memory;memory.address<P>(2,item==lu::UdpErrno::e_inval?0:42000,item==lu::UdpErrno::e_netunreach?udp_link.local_ip:udp_link.peer_ip);
        const auto prior=f.submits;CHECK(f.run<P>(memory,{.call=lu::UdpCall::sendto,.fd=3,.payload=payload_at,
            .count=item==lu::UdpErrno::e_msgsize?1473u:1u,.flags=item==lu::UdpErrno::e_opnotsupp?1u:0u,
            .address=address_at,.address_length=16}));CHECK(f.value==error<P>(item)&&f.submits==prior);
    }
}

template<lu::Profile P> void receives()
{
    Fixture f;f.socket<P>();CHECK(f.network.bind(f.endpoint(),{},40000));
    for(u32 flags:{0u,2u,32u,34u,64u,66u,96u,98u})for(usize size:{usize{0},usize{1},usize{1472}})
    {
        f.enqueue(size);Memory memory;memory.length<P>(8);
        CHECK(f.run<P>(memory,{.call=lu::UdpCall::recvfrom,.fd=3,.payload=payload_at,.count=13,.flags=flags,
            .address=address_at,.source_length=length_at}));CHECK(f.value==static_cast<i64>((flags&32)?size:(size<13?size:13)));
        CHECK(f.queued()==((flags&2)?1u:0u));CHECK(memory.events[memory.calls-1].address==length_at);
        auto length=lu::UdpWire<P>::decode_socklen({reinterpret_cast<const u8 *>(memory.bytes.data()+length_at),4});CHECK(length&&length.value()==16);
        if(flags&2)CHECK(f.network.consume(f.endpoint()));
    }
    for(usize fail:{usize{1},usize{2},usize{3},usize{4}})for(bool peek:{false,true})
    {
        f.enqueue(23);Memory memory;memory.length<P>(16);memory.fail=fail;
        CHECK(f.run<P>(memory,{.call=lu::UdpCall::recvfrom,.fd=3,.payload=payload_at,.count=13,.flags=peek?2u:0u,
            .address=address_at,.source_length=length_at}));CHECK(f.value==error<P>(lu::UdpErrno::e_fault));
        CHECK(f.queued()==static_cast<usize>(peek));if(peek)CHECK(f.network.consume(f.endpoint()));
    }
    f.enqueue(1);Memory negative;for(usize at=0;at<4;++at)negative.bytes[length_at+at]=std::byte{0xff};
    CHECK(f.run<P>(negative,{.call=lu::UdpCall::recvfrom,.fd=3,.payload=payload_at,.count=1,.address=address_at,.source_length=length_at}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_inval)&&f.queued()==0&&negative.calls==2);
    f.enqueue(1);Memory flags;CHECK(f.run<P>(flags,{.call=lu::UdpCall::recvfrom,.fd=3,.payload=payload_at,.count=1,.flags=1}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_opnotsupp)&&f.queued()==1&&flags.calls==0);CHECK(f.network.consume(f.endpoint()));
    Memory empty;CHECK(f.run<P>(empty,{.call=lu::UdpCall::recvfrom,.fd=3,.payload=payload_at,.count=1}));CHECK(f.value==error<P>(lu::UdpErrno::e_again));
    f.executor=Status::io_error();Memory failed;CHECK(f.run<P>(failed,{.call=lu::UdpCall::recvfrom,.fd=3,.payload=payload_at,.count=1}));CHECK(f.value==error<P>(lu::UdpErrno::e_io));
    f.enqueue(0);Memory drain;CHECK(f.run<P>(drain,{.call=lu::UdpCall::recvfrom,.fd=3}));CHECK(f.value==0&&f.queued()==0);
}

template<lu::Profile P> void lifetime()
{
    Fixture f;f.socket<P>();const auto endpoint=f.endpoint();f.held_description=f.process().fd_table().entry(3)->description;
    struct Context { Fixture &fixture; u32 endpoint; usize recursive{}; } context{f,endpoint};
    Memory memory;memory.address<P>(2,40000,{});memory.context=&context;
    memory.hook=[](Memory &m) {
        auto &c=*static_cast<Context *>(m.context);auto &f=c.fixture;
        CHECK(f.owner->open_description(f.held_description)->references==2);
        CHECK(f.owner->close_descriptor(f.process().resources(),3));
        auto replacement=f.owner->allocate_description({.readable=true});CHECK(replacement);CHECK(replacement.value()!=f.held_description);
        CHECK(f.owner->install_descriptor(f.process().resources(),3,replacement.value()));
        auto decode=[]() -> Result<lu::UdpRequest> { CHECK(false);return Status::io_error(); };
        auto publish=[](i64) -> Status { CHECK(false);return {}; };
        CHECK(f.scratch.template run_backend<P>(f,f.process(),m,decode,publish,f.release_failure).code()==StatusCode::busy);
        CHECK(m.live()&&m.resets==0);++c.recursive;m.hook=nullptr;
    };
    memory.reset_hook=[](Memory &m) { auto &c=*static_cast<Context *>(m.context);CHECK(c.fixture.network.binding(c.endpoint));CHECK(!m.live()); };
    CHECK(f.run<P>(memory,{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=16}));
    CHECK(f.value==0&&memory.resets==1&&context.recursive==1);CHECK(!f.network.binding(endpoint));
    CHECK(!f.owner->open_description(f.held_description));f.held_description=sched::invalid_description;
    for(usize revoke:{usize{1},usize{2}})
    {
        Fixture other;other.socket<P>();Memory lost;lost.address<P>(2,42000,udp_link.peer_ip);lost.revoke=revoke;
        const auto prior=other.published;CHECK(other.run<P>(lost,{.call=lu::UdpCall::sendto,.fd=3,.payload=payload_at,.count=1,
            .address=address_at,.address_length=16}).code()==StatusCode::denied);
        CHECK(other.published==prior&&other.submits==0&&lost.resets==1);
    }
    Fixture final;final.socket<P>();const auto last=final.endpoint();final.close_on_publish=true;
    Memory publish;publish.address<P>(2,40000,{});CHECK(final.run<P>(publish,{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=16}));
    CHECK(!final.network.binding(last)&&publish.resets==1);
    Fixture decode;Memory denied;CHECK(decode.run<P>(denied,{},Status::denied()).code()==StatusCode::denied);
    CHECK(decode.published==0&&denied.resets==1);
    Memory malformed;CHECK(decode.run<P>(malformed,{},Status::invalid_argument()));CHECK(decode.value==error<P>(lu::UdpErrno::e_inval));
}

template<lu::Profile P> void bounded_description_and_nonblock()
{
    Fixture f;f.socket<P>();auto *slot=f.process().fd_table().entry(3);auto *open=f.owner->open_description(slot->description);
    for(usize shape=0;shape<5;++shape)
    {
        const auto saved=*open;
        if(shape==0)open->references=0;
        if(shape==1)open->node=reinterpret_cast<fs::Node *>(uptr{0x1234});
        if(shape==2)open->pipe=0;
        if(shape==3)open->console=true;
        if(shape==4)open->input_console=true;
        Memory memory;memory.address<P>(2,40000,{});CHECK(f.run<P>(memory,{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=16}));
        CHECK(f.value==error<P>(lu::UdpErrno::e_io)&&memory.calls==0&&f.submits==0&&open->references==saved.references-(shape==0?1u:0u));
        CHECK(!f.network.binding(saved.native_udp_handle).value().bound);*open=saved;
    }
    CHECK(f.network.bind(f.endpoint(),{},40000));open->flags&=~posix::o_NONBLOCK;
    f.enqueue(1);Memory blocking;CHECK(f.run<P>(blocking,{.call=lu::UdpCall::recvfrom,.fd=3,.payload=payload_at,.count=1}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_opnotsupp)&&f.queued()==1&&blocking.calls==0);
    Memory dontwait;CHECK(f.run<P>(dontwait,{.call=lu::UdpCall::recvfrom,.fd=3,.payload=payload_at,.count=1,
            .flags=*lu::UdpConstants<P>::message(lu::UdpMessage::dontwait)}));CHECK(f.value==1&&f.queued()==0);
}

template<lu::Profile P> void copy_endpoint_stability()
{
    for(bool receive:{false,true})for(usize callback=1;callback<=(receive?4u:2u);++callback)
    {
        Fixture f;f.socket<P>();const auto endpoint=f.endpoint();CHECK(f.network.bind(endpoint,{},40000));
        f.held_description=f.process().fd_table().entry(3)->description;if(receive)f.enqueue(23);
        struct Context { Fixture &fixture;u32 endpoint;usize callback;bool closed{}; } context{f,endpoint,callback};
        Memory memory;memory.context=&context;memory.address<P>(2,42000,udp_link.peer_ip);memory.length<P>(16);
        memory.hook=[](Memory &m) {
            auto &c=*static_cast<Context *>(m.context);if(m.calls!=c.callback)return;
            auto &f=c.fixture;CHECK(f.owner->open_description(f.held_description)->references==2);
            CHECK(f.owner->close_descriptor(f.process().resources(),3));
            auto replacement=f.owner->allocate_description({.readable=true});CHECK(replacement);CHECK(replacement.value()!=f.held_description);
            CHECK(f.owner->install_descriptor(f.process().resources(),3,replacement.value()));
            CHECK(f.network.binding(c.endpoint));c.closed=true;
        };
        CHECK(f.run<P>(memory,{.call=receive?lu::UdpCall::recvfrom:lu::UdpCall::sendto,.fd=3,.payload=payload_at,.count=13,
            .address=address_at,.address_length=16,.source_length=length_at}));CHECK(context.closed&&f.value==13);
        CHECK(!f.network.binding(endpoint)&&!f.owner->open_description(f.held_description));
        CHECK(f.owner->open_description(f.process().fd_table().entry(3)->description)->references==1);
        f.held_description=sched::invalid_description;
    }
    Fixture f;f.socket<P>();Memory memory;memory.address<P>(2,42000,udp_link.peer_ip);f.link_context=&memory;
    f.submit_hook=[](Fixture &f) { static_cast<Memory *>(f.link_context)->active=false; };
    const auto published=f.published;CHECK(f.run<P>(memory,{.call=lu::UdpCall::sendto,.fd=3,.payload=payload_at,.count=13,
            .address=address_at,.address_length=16}).code()==StatusCode::denied);
    CHECK(f.submits==1&&f.published==published&&memory.resets==1);
}

template<lu::Profile P> void completion_failure_order()
{
    Fixture f;f.socket<P>();const auto endpoint=f.endpoint();const auto description=f.process().fd_table().entry(3)->description;
    Memory memory;memory.address<P>(2,40000,{});
    auto decode=[]() -> Result<lu::UdpRequest> { return lu::UdpRequest{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=16}; };
    usize publications{};auto publish=[&](i64 result) -> Status {
        ++publications;CHECK(memory.live()&&memory.resets==0&&result==0);
        CHECK(f.owner->open_description(description)->references==2);
        CHECK(f.owner->close_descriptor(f.process().resources(),3));
        CHECK(f.network.close(endpoint));return Status::fault("synthetic publication failure");
    };
    CHECK(f.scratch.run_backend<P>(f,f.process(),memory,decode,publish,f.release_failure).code()==StatusCode::fault);
    CHECK(publications==1&&memory.resets==1&&f.release_failure.code()==StatusCode::bad_descriptor);
    CHECK(!f.owner->open_description(description)&&!f.network.binding(endpoint));
    Fixture success;success.socket<P>();Memory revoked;revoked.address<P>(2,40000,{});
    auto completed=[&](i64 result) -> Status { CHECK(result==0&&revoked.live());revoked.active=false;return {}; };
    CHECK(success.scratch.run_backend<P>(success,success.process(),revoked,decode,completed,success.release_failure));
    CHECK(revoked.resets==1);
}

template<lu::Profile P> lu::Ipv6SocketAddress name6(const Memory &memory)
{
    auto length=lu::UdpWire<P>::decode_socklen({reinterpret_cast<const u8 *>(memory.bytes.data()+length_at),4});
    CHECK(length&&length.value()==28);
    auto value=lu::Ipv6SocketAddressCodec<P>::decode({reinterpret_cast<const u8 *>(memory.bytes.data()+address_at),28});
    CHECK(value&&value.value().family==10&&value.value().flowinfo==0);return value.value();
}

// AF_INET6 sockets are v6-only. Family mismatches follow Linux with
// IPV6_V6ONLY set: bind/connect EAFNOSUPPORT, sendto AF_INET ENETUNREACH.
// Mapped addresses: bind EINVAL, connect/sendto ENETUNREACH.
template<lu::Profile P> void ipv6_operations()
{
    namespace wire=net::wire;
    const auto self=wire::link_local_from_mac(udp_link.local_mac),router=wire::link_local_from_mac(udp_link.peer_mac);
    const wire::Ipv6 remote{0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,9};
    const wire::Ipv6 mapped{0,0,0,0,0,0,0,0,0,0,0xff,0xff,192,0,2,1};
    Fixture f(true,true);const auto fd=f.socket<P>(false,10);CHECK(fd==3);
    CHECK(f.network.family(f.endpoint()).value()==net::Family::ipv6);
    const auto bind=[&](u16 family,u16 port,const wire::Ipv6 &ip,i32 length,lu::UdpErrno expected) {
        Memory memory;memory.address6<P>(family,port,ip,128);
        CHECK(f.run<P>(memory,{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=length}));
        CHECK(f.value==error<P>(expected));CHECK(!f.network.binding(f.endpoint()).value().bound);
    };
    bind(10,40000,{},16,lu::UdpErrno::e_inval);bind(10,40000,{},23,lu::UdpErrno::e_inval);
    bind(10,40000,{},129,lu::UdpErrno::e_inval);
    bind(2,40000,{},28,lu::UdpErrno::e_afnosupport);bind(0,40000,{},28,lu::UdpErrno::e_afnosupport);
    bind(10,100,{},28,lu::UdpErrno::e_acces);bind(10,40000,mapped,28,lu::UdpErrno::e_inval);
    bind(10,40000,remote,28,lu::UdpErrno::e_addrnotavail);bind(10,40000,router,28,lu::UdpErrno::e_addrnotavail);
    {
        Memory memory;memory.address<P>(2,40000,{});
        CHECK(f.run<P>(memory,{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=16}));
        CHECK(f.value==error<P>(lu::UdpErrno::e_inval));
    }
    // An unbound name is the IPv6 any address.
    Memory unbound;unbound.length<P>(128);
    CHECK(f.run<P>(unbound,{.call=lu::UdpCall::getsockname,.fd=3,.address=address_at,.source_length=length_at}));
    CHECK(f.value==0);auto name=name6<P>(unbound);CHECK(name.port==0&&name.address==wire::Ipv6{}&&name.scope_id==0);
    // RFC 2133 length and ignored input scope_id.
    Memory good;good.address6<P>(10,40000,self,24);
    CHECK(f.run<P>(good,{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=24}));CHECK(f.value==0);
    Memory bound;bound.length<P>(28);
    CHECK(f.run<P>(bound,{.call=lu::UdpCall::getsockname,.fd=3,.address=address_at,.source_length=length_at}));
    name=name6<P>(bound);CHECK(f.value==0&&name.port==40000&&name.address==self&&name.scope_id==1);
    for(u32 capacity:{0u,1u,16u,27u})
    {
        Memory memory;memory.bytes.fill(std::byte{0xa5});memory.length<P>(capacity);
        CHECK(f.run<P>(memory,{.call=lu::UdpCall::getsockname,.fd=3,.address=address_at,.source_length=length_at}));
        CHECK(f.value==0&&memory.calls==(capacity?3u:2u));
        auto length=lu::UdpWire<P>::decode_socklen({reinterpret_cast<const u8 *>(memory.bytes.data()+length_at),4});
        CHECK(length&&length.value()==28);
        for(usize at=0;at<capacity;++at)CHECK(memory.bytes[address_at+at]==bound.bytes[address_at+at]);
        CHECK(memory.bytes[address_at+capacity]==std::byte{0xa5});
    }
    Memory again;again.address6<P>(10,40001,{});
    CHECK(f.run<P>(again,{.call=lu::UdpCall::bind,.fd=3,.address=address_at,.address_length=28}));
    CHECK(f.value==error<P>(lu::UdpErrno::e_inval));
    Memory none;none.length<P>(28);
    CHECK(f.run<P>(none,{.call=lu::UdpCall::getpeername,.fd=3,.address=address_at,.source_length=length_at}));
    CHECK(f.value==-static_cast<i64>(lu::UdpConstants<P>::not_connected_errno_number()));

    const auto send=[&](u16 family,u16 port,const wire::Ipv6 &ip,i32 length,lu::UdpErrno expected,bool v4=false) {
        Memory memory;if(v4)memory.address<P>(family,port,{192,0,2,1});else memory.address6<P>(family,port,ip,128);
        const auto prior=f.submits;CHECK(f.run<P>(memory,{.call=lu::UdpCall::sendto,.fd=3,.payload=payload_at,.count=3,
            .address=address_at,.address_length=length}));CHECK(f.value==error<P>(expected)&&f.submits==prior);
    };
    send(2,42000,{},16,lu::UdpErrno::e_netunreach,true);send(2,42000,{},28,lu::UdpErrno::e_netunreach);
    send(3,42000,router,28,lu::UdpErrno::e_inval);send(10,42000,router,20,lu::UdpErrno::e_inval);
    send(10,42000,router,1,lu::UdpErrno::e_inval);
    send(10,42000,mapped,28,lu::UdpErrno::e_netunreach);send(10,0,router,28,lu::UdpErrno::e_inval);
    send(10,42000,remote,28,lu::UdpErrno::e_netunreach);send(0,42000,router,28,lu::UdpErrno::e_destaddrreq);
    // The first datagram waits for neighbour discovery; the advert releases it.
    Memory held;held.address6<P>(10,42000,router,28);
    CHECK(f.run<P>(held,{.call=lu::UdpCall::sendto,.fd=3,.payload=payload_at,.count=3,.address=address_at,.address_length=28}));
    CHECK(f.value==3&&f.submits==0&&f.nic.neighbours().pending()==1);
    CHECK(f.nic.tick(1100));while(f.nic.control())f.nic.pop_control();
    std::array<std::byte,128> advert{};
    auto na=wire::build_neighbor_advertisement(std::span{advert}.subspan(14),router,self,router,true,true,true,udp_link.peer_mac);
    CHECK(na&&wire::build_ethernet(advert,udp_link.local_mac,udp_link.peer_mac,wire::ethertype_ipv6,na.value()));
    f.nic.receive({advert.data(),14+na.value()},1200);
    usize flushed{};f.nic.flush([&](wire::Bytes) { ++flushed;return true; });CHECK(flushed==1);
    Memory resolved;resolved.address6<P>(10,42000,router,28);
    for(usize at=0;at<3;++at)resolved.bytes[payload_at+at]=std::byte{static_cast<u8>(0x40+at)};
    CHECK(f.run<P>(resolved,{.call=lu::UdpCall::sendto,.fd=3,.payload=payload_at,.count=3,.address=address_at,.address_length=28}));
    CHECK(f.value==3&&f.submits==1);
    {
        auto ethernet=wire::ethernet({f.transmitted.data(),f.transmitted_size});CHECK(ethernet&&ethernet.value().destination==udp_link.peer_mac);
        auto ip=wire::ipv6(ethernet.value().payload);CHECK(ip&&ip.value().source==self&&ip.value().destination==router);
        auto packet=wire::udp<wire::Ipv6Family>(ip.value());CHECK(packet&&packet.value().source_port==40000&&packet.value().destination_port==42000);
        CHECK(packet.value().payload.size()==3&&packet.value().payload[2]==std::byte{0x42});
    }
    // Receive reports the 28-byte source name and length.
    const std::array<std::byte,4> data{std::byte{9},std::byte{8},std::byte{7},std::byte{6}};
    std::array<std::byte,128> inbound{};
    auto built=wire::build_udp<wire::Ipv6Family>(inbound,router,self,42000,40000,0,64,data);CHECK(built);
    auto queued=f.network.deliver({inbound.data(),built.value()});CHECK(queued&&queued.value().kind==net::NativeUdpFrameKind::udp_queued);
    Memory received;received.length<P>(128);
    CHECK(f.run<P>(received,{.call=lu::UdpCall::recvfrom,.fd=3,.payload=payload_at,.count=13,.address=address_at,.source_length=length_at}));
    CHECK(f.value==4&&received.bytes[payload_at+3]==std::byte{6});
    name=name6<P>(received);CHECK(name.port==42000&&name.address==router&&name.scope_id==1);

    const auto connect=[&](u16 family,u16 port,const wire::Ipv6 &ip,i32 length,lu::UdpErrno expected,bool v4=false) {
        Memory memory;if(v4)memory.address<P>(family,port,{192,0,2,1});else memory.address6<P>(family,port,ip,128);
        CHECK(f.run<P>(memory,{.call=lu::UdpCall::connect,.fd=3,.address=address_at,.address_length=length}));
        CHECK(f.value==error<P>(expected)&&!f.network.peer(f.endpoint()).value().associated);
    };
    connect(2,42000,{},16,lu::UdpErrno::e_afnosupport,true);connect(2,42000,{},28,lu::UdpErrno::e_afnosupport);
    connect(10,42000,router,23,lu::UdpErrno::e_inval);connect(10,42000,router,1,lu::UdpErrno::e_inval);
    connect(3,42000,router,28,lu::UdpErrno::e_afnosupport);connect(10,42000,mapped,28,lu::UdpErrno::e_netunreach);
    connect(10,42000,remote,28,lu::UdpErrno::e_netunreach);
    Memory peer;peer.address6<P>(10,42000,router,28);
    CHECK(f.run<P>(peer,{.call=lu::UdpCall::connect,.fd=3,.address=address_at,.address_length=28}));CHECK(f.value==0);
    Memory named;named.length<P>(28);
    CHECK(f.run<P>(named,{.call=lu::UdpCall::getpeername,.fd=3,.address=address_at,.source_length=length_at}));
    name=name6<P>(named);CHECK(f.value==0&&name.port==42000&&name.address==router&&name.scope_id==1);
    // Connected output with no address and with AF_UNSPEC both use the peer.
    for(bool unspecified:{false,true})
    {
        Memory memory;memory.address6<P>(0,1,remote,28);const auto prior=f.submits;
        CHECK(f.run<P>(memory,{.call=lu::UdpCall::sendto,.fd=3,.payload=payload_at,.count=2,
            .address=unspecified?address_at:0,.address_length=unspecified?28:0}));CHECK(f.value==2&&f.submits==prior+1);
    }
    Memory disconnect;disconnect.address6<P>(0,0,{},28);
    CHECK(f.run<P>(disconnect,{.call=lu::UdpCall::connect,.fd=3,.address=address_at,.address_length=2}));
    CHECK(f.value==0&&!f.network.peer(f.endpoint()).value().associated);

    // An IPv4 socket keeps its IPv4-only record rules for AF_INET6 input.
    const auto v4=f.socket<P>();CHECK(v4==4);
    for(auto call:{lu::UdpCall::bind,lu::UdpCall::connect,lu::UdpCall::sendto})
    {
        Memory memory;memory.address6<P>(10,42000,router,28);
        CHECK(f.run<P>(memory,{.call=call,.fd=4,.payload=payload_at,.count=1,.address=address_at,.address_length=28}));
        CHECK(f.value==error<P>(lu::UdpErrno::e_afnosupport));
    }
    Memory v4name;v4name.length<P>(128);
    CHECK(f.run<P>(v4name,{.call=lu::UdpCall::getsockname,.fd=4,.address=address_at,.source_length=length_at}));
    auto v4length=lu::UdpWire<P>::decode_socklen({reinterpret_cast<const u8 *>(v4name.bytes.data()+length_at),4});
    CHECK(f.value==0&&v4length&&v4length.value()==16);
}

template<lu::Profile P> void profile() { sockets<P>();configure_first_socket<P>();binds<P>();sends_and_range<P>();receives<P>();lifetime<P>();bounded_description_and_nonblock<P>();copy_endpoint_stability<P>();completion_failure_order<P>();ipv6_operations<P>(); }
int main()
{
    profile<lu::Profile::i386>();profile<lu::Profile::x86_64>();profile<lu::Profile::aarch64>();profile<lu::Profile::arm32>();
    profile<lu::Profile::rv64>();profile<lu::Profile::rv32>();profile<lu::Profile::loongarch64>();
    profile<lu::Profile::mips>();profile<lu::Profile::mips64>();profile<lu::Profile::ppc>();
    std::printf("LINUX_UDP_OPERATION_BODY_MODEL_PASS checks=%zu profiles=10 hosted=1 scheduler_service=actual image_device_publication=synthetic syscall=0 guest=0 nic=0\n",checks);
}
