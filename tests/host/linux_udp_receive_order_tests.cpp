// Actual protocol queues with synthetic user access, not syscall execution.
#include "../../src/user/linux_udp_receive_order.hpp"
#include <cstdio>
#include <cstdlib>
#include <type_traits>
using namespace ok;
using namespace ok::net;
using namespace ok::abi::linux_user;
using namespace ok::user::native::linux_udp;
static usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "LINUX_UDP_RX_FAIL line=%d checks=%zu: %s\n", __LINE__, checks, #x); std::abort(); } } while (false)
static_assert(!std::is_copy_constructible_v<ReceiveScratch> && !std::is_move_constructible_v<ReceiveScratch>);
constexpr NativeUdpLink udp_link{{2, 1, 2, 3, 4, 5}, {2, 6, 7, 8, 9, 10}, {192, 0, 2, 2}, {192, 0, 2, 1}};
constexpr u64 payload_at=64, source_at=1800, length_at=1840;

struct Memory
{
    struct Event { bool write{}; u64 address{}; usize size{}; };
    std::array<std::byte, 4096> bytes{};
    std::array<Event, 8> events{};
    usize calls{}, fail{}, revoke{};
    bool active{true};
    void *context{};
    void (*hook)(Memory &){};
    Memory() { bytes.fill(std::byte{0xa5}); }
    bool live() const { return active; }
    Status copy(bool write, u64 address, std::span<std::byte> input, std::span<const std::byte> output)
    {
        const usize size=write ? output.size() : input.size();
        CHECK(calls < events.size()); events[calls++]={write,address,size};
        if (hook) hook(*this);
        if (!active || !address || address > bytes.size() || size > bytes.size()-address)
            return Status::fault("synthetic user range fault");
        const usize copied=calls==fail ? size/2 : size;
        for (usize at=0; at<copied; ++at)
            if (write) bytes[static_cast<usize>(address)+at]=output[at];
            else input[at]=bytes[static_cast<usize>(address)+at];
        if (calls==revoke) active=false;
        return calls==fail ? Status::fault("synthetic partial usercopy fault") : Status{};
    }
    Status read(u64 address, std::span<std::byte> input) { return copy(false,address,input,{}); }
    Status write(u64 address, std::span<const std::byte> output) { return copy(true,address,{},output); }
    template <Profile Target> void put_length(u64 address,u32 value)
    {
        for (usize at=0;at<4;++at)
        {
            const usize shift=Codec<Target>::profile.big_endian ? 3-at : at;
            bytes[static_cast<usize>(address)+at]=std::byte{static_cast<u8>(value>>(8*shift))};
        }
    }
    template <Profile Target> u32 get_length(u64 address) const
    {
        u32 value{};
        for (usize at=0;at<4;++at)
        {
            const usize shift=Codec<Target>::profile.big_endian ? 3-at : at;
            value|=u32{std::to_integer<u8>(bytes[static_cast<usize>(address)+at])}<<(8*shift);
        }
        return value;
    }
};
struct Fixture
{
    NativeUdpService service{},peer{};
    ReceiveScratch scratch{};
    u32 endpoint{},sender{};
    std::array<std::byte,NativeUdpService::frame_capacity> frame{},reply{};
    std::array<std::byte,NativeUdpService::datagram_capacity> data{};
    Fixture()
    {
        CHECK(service.configure(udp_link));
        CHECK(peer.configure({udp_link.peer_mac,udp_link.local_mac,udp_link.peer_ip,udp_link.local_ip}));
        const auto target=service.open(),from=peer.open(); CHECK(target && from);
        endpoint=target.value();sender=from.value();
        CHECK(service.bind(endpoint,{},40000));CHECK(peer.bind(sender,{},41000));
        for (usize at=0;at<data.size();++at) data[at]=std::byte{static_cast<u8>(at*17+3)};
    }
    void enqueue(usize size)
    {
        const auto built=peer.build_datagram(sender,udp_link.local_ip,40000,{data.data(),size},frame);CHECK(built);
        const auto received=service.ingest({frame.data(),built.value()},reply);CHECK(received);
        CHECK(received.value().kind==NativeUdpFrameKind::udp_queued);
    }
    usize queued() const { const auto ready=service.readiness(endpoint);CHECK(ready);return ready.value().queued_datagrams; }
};
template <Profile Target> void sizes()
{
    Fixture f;
    for (usize size:{usize{0},usize{1},usize{23},NativeUdpService::datagram_capacity})
        for (usize count:{usize{0},usize{1},usize{11},NativeUdpService::datagram_capacity})
            for (u32 flags:{u32{0},ReceiveScratch::dontwait,ReceiveScratch::peek,ReceiveScratch::trunc,
                           ReceiveScratch::peek|ReceiveScratch::trunc|ReceiveScratch::dontwait})
            {
                f.enqueue(size);Memory memory;
                const auto value=f.scratch.receive<Target>(f.service,f.endpoint,memory,{payload_at,count,0,~u64{0},flags});
                const usize copied=count<size?count:size;
                CHECK(value && value.value()==((flags&ReceiveScratch::trunc)?size:copied));
                CHECK(f.queued()==((flags&ReceiveScratch::peek)?1:0));
                CHECK(memory.calls==(copied?1:0));
                for (usize at=0;at<copied;++at) CHECK(memory.bytes[payload_at+at]==f.data[at]);
                CHECK(memory.bytes[payload_at+copied]==std::byte{0xa5});
                if (flags&ReceiveScratch::peek) CHECK(f.service.consume(f.endpoint));
            }
    for (u32 capacity:{u32{0},u32{1},u32{2},u32{15},u32{16},u32{17},u32{128},u32{2147483647}})
    {
        f.enqueue(5);Memory memory;memory.put_length<Target>(length_at,capacity);
        const auto value=f.scratch.receive<Target>(f.service,f.endpoint,memory,{payload_at,5,source_at,length_at,0});
        CHECK(value && value.value()==5 && f.queued()==0 && memory.get_length<Target>(length_at)==16);
        std::array<u8,16> expected{};CHECK(Ipv4SocketAddressCodec<Target>::encode(expected,{2,41000,udp_link.peer_ip}));
        const usize prefix=capacity<16?capacity:16;
        for (usize at=0;at<prefix;++at) CHECK(memory.bytes[source_at+at]==std::byte{expected[at]});
        CHECK(memory.bytes[source_at+prefix]==std::byte{0xa5} && memory.calls==(prefix?4:3));
        CHECK(memory.events[0].write && memory.events[0].address==payload_at);
        CHECK(!memory.events[1].write && memory.events[1].address==length_at && memory.events[1].size==4);
        CHECK(memory.events[memory.calls-1].address==length_at && memory.events[memory.calls-1].size==4);
    }
}
template <Profile Target> void faults()
{
    Fixture f;Memory empty;
    const auto missing=f.scratch.receive<Target>(f.service,f.endpoint,empty,{});
    CHECK(!missing && missing.status().code()==StatusCode::would_block && empty.calls==0);
    for (bool peek:{false,true}) for (usize fault=1;fault<=4;++fault)
    {
        f.enqueue(7);Memory memory;memory.fail=fault;memory.put_length<Target>(length_at,7);
        const auto value=f.scratch.receive<Target>(f.service,f.endpoint,memory,
            {payload_at,7,source_at,length_at,peek?ReceiveScratch::peek:0});
        CHECK(!value && value.status().code()==StatusCode::fault && memory.calls==fault && f.queued()==(peek?1:0));
        if (fault<4) CHECK(memory.get_length<Target>(length_at)==7);
        if (peek) CHECK(f.service.consume(f.endpoint));
    }
    f.enqueue(3);Memory unsupported;
    const auto flags=f.scratch.receive<Target>(f.service,f.endpoint,unsupported,{payload_at,3,0,0,1});
    CHECK(!flags && flags.status().code()==StatusCode::unsupported && f.queued()==1 && unsupported.calls==0);
    Memory revoked;revoked.active=false;
    const auto stale=f.scratch.receive<Target>(f.service,f.endpoint,revoked,{});
    CHECK(!stale && stale.status().code()==StatusCode::fault && f.queued()==1 && revoked.calls==0);
    CHECK(f.service.consume(f.endpoint));
    for (u64 pointer:{u64{0},u64{4095}})
    {
        f.enqueue(3);Memory memory;
        const auto value=f.scratch.receive<Target>(f.service,f.endpoint,memory,{payload_at,3,source_at,pointer,0});
        CHECK(!value && value.status().code()==StatusCode::fault && f.queued()==0 && memory.calls==2);
    }
    f.enqueue(3);Memory negative;negative.put_length<Target>(length_at,0xffffffffU);
    const auto bad=f.scratch.receive<Target>(f.service,f.endpoint,negative,{payload_at,3,source_at,length_at,0});
    CHECK(!bad && bad.status().code()==StatusCode::invalid_argument && negative.calls==2 && f.queued()==0);
    f.enqueue(3);Memory no_source;
    const auto ignored=f.scratch.receive<Target>(f.service,f.endpoint,no_source,{payload_at,3,0,4095,0});
    CHECK(ignored && ignored.value()==3 && no_source.calls==1);
    f.enqueue(3);Memory zero;zero.put_length<Target>(length_at,0);
    const auto skipped=f.scratch.receive<Target>(f.service,f.endpoint,zero,{payload_at,0,4095,length_at,0});
    CHECK(skipped && skipped.value()==0 && zero.calls==2 && f.queued()==0 && zero.get_length<Target>(length_at)==16);
}
template <Profile Target> void aliases_and_callbacks()
{
    Fixture f;Memory memory;memory.put_length<Target>(length_at,0xffffffffU);
    for (usize at=0;at<4;++at)
    {
        const usize shift=Codec<Target>::profile.big_endian?3-at:at;
        f.data[at]=std::byte{static_cast<u8>(1U>>(8*shift))};
    }
    f.enqueue(4);
    auto value=f.scratch.receive<Target>(f.service,f.endpoint,memory,{length_at,4,source_at,length_at,0});
    CHECK(value && memory.get_length<Target>(length_at)==16 && memory.calls==4 && memory.events[2].size==1);
    f.enqueue(4);Memory alias;alias.put_length<Target>(length_at,16);
    value=f.scratch.receive<Target>(f.service,f.endpoint,alias,{payload_at,4,length_at,length_at,0});
    CHECK(value && alias.get_length<Target>(length_at)==16 && alias.bytes[length_at+4]==std::byte{192} && alias.bytes[length_at+15]==std::byte{});
    struct Context { Fixture *fixture;StatusCode recursive{StatusCode::ok};usize observed{}; } context{&f};
    f.enqueue(4);f.enqueue(4);Memory recursive;recursive.context=&context;
    recursive.hook=[](Memory &access) {
        if (access.calls!=1) return;
        auto &ctx=*static_cast<Context *>(access.context);ctx.observed=ctx.fixture->queued();Memory other;
        const auto nested=ctx.fixture->scratch.template receive<Target>(ctx.fixture->service,ctx.fixture->endpoint,other,{});
        CHECK(!nested && other.calls==0);ctx.recursive=nested.status().code();
    };
    value=f.scratch.receive<Target>(f.service,f.endpoint,recursive,{payload_at,4,0,0,0});
    CHECK(value && context.observed==1 && context.recursive==StatusCode::busy && f.queued()==1);
    CHECK(f.service.consume(f.endpoint));
    f.data[0]=std::byte{0x5a};f.enqueue(9);f.enqueue(1);Memory overwrite;
    overwrite.put_length<Target>(length_at,16);overwrite.context=&f;
    overwrite.hook=[](Memory &access) {
        if (access.calls!=1) return;
        auto &fixture=*static_cast<Fixture *>(access.context);CHECK(fixture.queued()==1);
        fixture.data.fill(std::byte{0x11});fixture.enqueue(9);
    };
    value=f.scratch.receive<Target>(f.service,f.endpoint,overwrite,{payload_at,9,source_at,length_at,0});
    CHECK(value && overwrite.bytes[payload_at]==std::byte{0x5a} && f.queued()==2);
    CHECK(overwrite.bytes[source_at+2]==std::byte{static_cast<u8>(41000>>8)});
    CHECK(f.service.consume(f.endpoint));CHECK(f.service.consume(f.endpoint));
    for (usize revoke=1;revoke<=4;++revoke)
    {
        f.enqueue(9);Memory access;access.revoke=revoke;access.put_length<Target>(length_at,16);
        const auto failed=f.scratch.receive<Target>(f.service,f.endpoint,access,{payload_at,9,source_at,length_at,0});
        CHECK(!failed && failed.status().code()==StatusCode::fault && access.calls==revoke && f.queued()==0);
        f.enqueue(0);Memory fresh;CHECK(f.scratch.receive<Target>(f.service,f.endpoint,fresh,{}));
    }
}
template <Profile Target> void profile() { sizes<Target>();faults<Target>();aliases_and_callbacks<Target>(); }
int main()
{
    profile<Profile::i386>();profile<Profile::x86_64>();profile<Profile::aarch64>();profile<Profile::arm32>();
    profile<Profile::rv64>();profile<Profile::rv32>();profile<Profile::loongarch64>();
    profile<Profile::mips>();profile<Profile::mips64>();profile<Profile::ppc>();
    std::printf("LINUX_UDP_RECEIVE_ORDER_MODEL_PASS checks=%zu profiles=10 hosted=1 usercopy=synthetic syscall=0 guest=0 actual_nic=0\n",checks);
}
