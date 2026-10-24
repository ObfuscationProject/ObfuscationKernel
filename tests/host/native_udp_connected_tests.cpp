// Serialized endpoint association models only; no syscall, guest or NIC execution.
#include "ok/net/native_udp.hpp"
#include <cstdio>
#include <cstdlib>
#include <type_traits>
using namespace ok;
using namespace ok::net;
namespace {
usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "UDP_CONNECTED_FAIL line=%d checks=%zu: %s\n", __LINE__, checks, #x); std::abort(); } } while (false)
static_assert(std::is_trivially_copyable_v<NativeUdpPeerSnapshot>);
constexpr NativeUdpLink link{{2,1,2,3,4,5},{2,6,7,8,9,10},{192,0,2,2},{192,0,2,1}};
constexpr std::array<std::byte,1> payload{std::byte{'q'}};
std::array<std::byte,NativeUdpService::frame_capacity> packet{}, reply{};
Result<NativeUdpFrame> ingress(NativeUdpService &service,u16 local,u16 source,
                              wire::Ipv4 source_ip=link.peer_ip) {
    auto built=wire::build_udp(std::span(packet).subspan(14),source_ip,link.local_ip,source,local,1,64,payload);
    CHECK(built);
    for(usize i=0;i<6;++i) { packet[i]=std::byte{link.local_mac[i]};packet[6+i]=std::byte{link.peer_mac[i]}; }
    wire::be16(packet,12,0x0800);
    return service.ingest({packet.data(),14+built.value()},reply);
}
void snapshot(const NativeUdpService &service,u32 endpoint,NativeUdpPeerSnapshot expected) {
    const auto before=service.readiness(endpoint);CHECK(before);
    const auto binding=service.binding(endpoint);CHECK(binding);
    const auto drops=service.dropped_frames();
    for(usize i=0;i<8;++i) { auto peer=service.peer(endpoint);CHECK(peer);CHECK(peer.value()==expected); }
    auto after=service.readiness(endpoint);CHECK(after);
    CHECK(after.value().sequence==before.value().sequence && after.value().queued_datagrams==before.value().queued_datagrams);
    CHECK(service.binding(endpoint).value()==binding.value() && service.dropped_frames()==drops);
}
void bindings() {
    NativeUdpService service;auto socket=service.open();CHECK(socket);
    snapshot(service,socket.value(),{});
    CHECK(service.connect(socket.value(),link.peer_ip,40000).code()==StatusCode::not_initialized);
    CHECK(service.disconnect(socket.value()).code()==StatusCode::not_initialized);
    CHECK(service.configure(link));
    for(usize mode=0;mode<5;++mode) {
        const auto endpoint=socket.value();
        if(mode==1) CHECK(service.bind(endpoint,{},40001));
        if(mode==2) CHECK(service.bind(endpoint,link.local_ip,40002));
        if(mode==3) CHECK(service.bind_ephemeral(endpoint,link.local_ip));
        if(mode==4) CHECK(service.bind_ephemeral(endpoint,{}));
        const auto original=service.binding(endpoint).value();
        CHECK(service.connect(endpoint,link.peer_ip,0));
        const auto selected=service.binding(endpoint).value();
        CHECK(selected.bound && selected.address==link.local_ip && selected.port>=1024);
        if(original.bound) CHECK(selected.port==original.port);
        snapshot(service,endpoint,{true,link.peer_ip,0});
        CHECK(!service.connect(endpoint,link.local_ip,41000));
        snapshot(service,endpoint,{true,link.peer_ip,0});
        CHECK(service.binding(endpoint).value()==selected);
        CHECK(service.connect(endpoint,link.peer_ip,41000));
        snapshot(service,endpoint,{true,link.peer_ip,41000});
        CHECK(service.binding(endpoint).value()==selected);
        CHECK(service.disconnect(endpoint));CHECK(service.disconnect(endpoint));
        snapshot(service,endpoint,{});
        const bool port_locked=mode==1 || mode==2, address_locked=mode==2 || mode==3;
        CHECK((service.binding(endpoint).value()==NativeUdpBindingSnapshot{port_locked,address_locked?link.local_ip:wire::Ipv4{},port_locked?original.port:u16{0}}));
        CHECK(service.autobind(endpoint));
        CHECK(service.binding(endpoint).value().address==(address_locked?link.local_ip:wire::Ipv4{}));
        CHECK(service.close(endpoint));
        CHECK(service.peer(endpoint).status().code()==StatusCode::bad_descriptor);
        CHECK(service.connect(endpoint,link.peer_ip,1).code()==StatusCode::bad_descriptor);
        CHECK(service.disconnect(endpoint).code()==StatusCode::bad_descriptor);
        socket=service.open();CHECK(socket);CHECK(socket.value()!=endpoint);
    }
    CHECK(service.close(socket.value()));CHECK(service.active_sockets()==0);
}
void queue_and_wire() {
    NativeUdpService service;CHECK(service.configure(link));auto socket=service.open();CHECK(socket);
    const auto endpoint=socket.value();CHECK(service.bind(endpoint,{},40000));
    CHECK(ingress(service,40000,40001).value().kind==NativeUdpFrameKind::udp_queued);
    const auto before=service.readiness(endpoint).value();
    CHECK(service.connect(endpoint,link.peer_ip,40002));
    CHECK(service.readiness(endpoint).value().sequence==before.sequence);
    CHECK(service.peek(endpoint).value().source_port==40001); // Already admitted queue is preserved.
    CHECK(ingress(service,40000,40001).value().kind==NativeUdpFrameKind::dropped);
    CHECK(service.readiness(endpoint).value().sequence==before.sequence);
    CHECK(ingress(service,40000,40002).value().kind==NativeUdpFrameKind::udp_queued);
    const auto full=service.readiness(endpoint).value();CHECK(full.queued_datagrams==2);
    const auto drops=service.dropped_frames();
    CHECK(ingress(service,40000,40003).value().kind==NativeUdpFrameKind::dropped);
    CHECK(ingress(service,40000,40002).value().kind==NativeUdpFrameKind::dropped);
    CHECK(service.dropped_frames()==drops+2 && service.readiness(endpoint).value().sequence==full.sequence);
    CHECK(service.consume(endpoint));CHECK(service.peek(endpoint).value().source_port==40002);CHECK(service.consume(endpoint));
    CHECK(service.connect(endpoint,link.peer_ip,0));
    CHECK(ingress(service,40000,0).value().kind==NativeUdpFrameKind::udp_queued);
    CHECK(ingress(service,40000,65535).value().kind==NativeUdpFrameKind::udp_queued);
    CHECK(service.peek(endpoint).value().source_port==0);
    CHECK(service.connect(endpoint,link.peer_ip,40002));
    CHECK(service.peek(endpoint).value().source_port==0); // A transition does not refilter admitted bytes.
    CHECK(service.consume(endpoint));CHECK(service.peek(endpoint).value().source_port==65535);CHECK(service.consume(endpoint));
    const auto constrained=service.readiness(endpoint).value();
    CHECK(ingress(service,40000,0).value().kind==NativeUdpFrameKind::dropped);
    CHECK(ingress(service,40000,65535).value().kind==NativeUdpFrameKind::dropped);
    CHECK(service.readiness(endpoint).value().sequence==constrained.sequence);
    CHECK(ingress(service,40000,40002).value().kind==NativeUdpFrameKind::udp_queued);
    CHECK(service.consume(endpoint));CHECK(service.connect(endpoint,link.peer_ip,0));
    CHECK(ingress(service,40000,0).value().kind==NativeUdpFrameKind::udp_queued);
    CHECK(ingress(service,40000,65535).value().kind==NativeUdpFrameKind::udp_queued);
    const auto zero=service.binding(endpoint).value();
    const auto built_zero=service.build_datagram(endpoint,link.peer_ip,0,payload,packet);CHECK(built_zero);
    const auto zero_ip=wire::ipv4({packet.data()+14,built_zero.value()-14});CHECK(zero_ip);
    CHECK(wire::be16(zero_ip.value().payload,0)==zero.port && wire::be16(zero_ip.value().payload,2)==0);
    CHECK(wire::sum(zero_ip.value().payload,wire::pseudo_sum(link.local_ip,link.peer_ip,17,static_cast<u16>(zero_ip.value().payload.size())))==0xffffu);
    CHECK(service.binding(endpoint).value()==zero);
    snapshot(service,endpoint,{true,link.peer_ip,0});
    CHECK(service.build_datagram(endpoint,link.peer_ip,42000,{},packet));
    snapshot(service,endpoint,{true,link.peer_ip,0});
    CHECK(service.disconnect(endpoint));CHECK(service.peek(endpoint).value().source_port==0);
    CHECK(service.consume(endpoint));CHECK(service.peek(endpoint).value().source_port==65535);CHECK(service.consume(endpoint));
    const auto empty=service.readiness(endpoint).value();
    CHECK(ingress(service,40000,40001,{192,0,2,3}).value().kind==NativeUdpFrameKind::dropped);
    CHECK(service.readiness(endpoint).value().sequence==empty.sequence);
    // A zero-destination outgoing packet cannot enter a local receive queue.
    const auto zero_built=wire::build_udp(std::span(packet).subspan(14),link.peer_ip,link.local_ip,0,0,1,64,{});CHECK(zero_built);
    for(usize i=0;i<6;++i) { packet[i]=std::byte{link.local_mac[i]};packet[6+i]=std::byte{link.peer_mac[i]}; }
    const auto zero_parsed=wire::ipv4({packet.data()+14,zero_built.value()});CHECK(zero_parsed);
    CHECK(wire::udp(zero_parsed.value()).status().code()==StatusCode::invalid_argument);
    CHECK(service.ingest({packet.data(),14+zero_built.value()},packet).value().kind==NativeUdpFrameKind::dropped);
    CHECK(service.close(endpoint));
}
void terminal() {
    NativeUdpService service{NativeUdpReadinessCapacity{1}};CHECK(service.configure(link));
    auto socket=service.open();CHECK(socket);const auto endpoint=socket.value();
    CHECK(service.bind(endpoint,{},40000));CHECK(service.connect(endpoint,link.peer_ip,40001));
    CHECK(ingress(service,40000,40001));
    CHECK(service.consume(endpoint)); // Draining advances past the capacity and enters terminal state.
    CHECK(service.readiness(endpoint).value().terminal);
    snapshot(service,endpoint,{true,link.peer_ip,40001});
    CHECK(service.connect(endpoint,link.peer_ip,0).code()==StatusCode::io_error);
    CHECK(service.disconnect(endpoint).code()==StatusCode::io_error);
    CHECK(service.autobind(endpoint).code()==StatusCode::io_error);
    CHECK(service.close(endpoint));
}
}
int main() { bindings();queue_and_wire();terminal();std::printf("NATIVE_UDP_CONNECTED_MODEL_PASS checks=%zu hosted=1 guest=0 nic=0\n",checks); }
