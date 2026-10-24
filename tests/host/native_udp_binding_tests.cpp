// Copied endpoint state models; no hardware, user mappings or syscall executes.
#include "ok/net/native_udp.hpp"
#include <cstdio>
#include <cstdlib>
#include <type_traits>

using namespace ok;
using namespace ok::net;
static usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "UDP_BINDING_FAIL line=%d checks=%zu: %s\n", __LINE__, checks, #x); std::abort(); } } while (false)
static_assert(std::is_trivially_copyable_v<NativeUdpBindingSnapshot>);
static_assert(!std::is_pointer_v<decltype(NativeUdpBindingSnapshot::address)>);
constexpr NativeUdpLink udp_link{{2, 1, 2, 3, 4, 5}, {2, 6, 7, 8, 9, 10},
                                {192, 0, 2, 2}, {192, 0, 2, 1}};
constexpr std::array<std::byte, 3> payload{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};

void pure_query(const NativeUdpService &service, u32 handle, NativeUdpBindingSnapshot wanted)
{
    const auto before = service.readiness(handle); CHECK(before);
    const auto drops = service.dropped_frames(), sockets = service.active_sockets();
    for (unsigned repeat = 0; repeat < 16; ++repeat)
    {
        const auto value = service.binding(handle); CHECK(value);
        CHECK(value.value() == wanted);
    }
    const auto after = service.readiness(handle); CHECK(after);
    CHECK(before.value().queued_datagrams == after.value().queued_datagrams);
    CHECK(before.value().sequence == after.value().sequence);
    CHECK(before.value().terminal == after.value().terminal);
    CHECK(service.dropped_frames() == drops && service.active_sockets() == sockets);
}

void explicit_and_autobind()
{
    NativeUdpService service;
    CHECK(service.configure(udp_link));
    const auto first = service.open(), second = service.open(); CHECK(first && second);
    pure_query(service, first.value(), {});
    const auto before = service.binding(first.value()); CHECK(before);
    CHECK(service.bind(first.value(), {}, 40000));
    CHECK(!before.value().bound && before.value().port == 0 && before.value().address == wire::Ipv4{});
    const NativeUdpBindingSnapshot any{true, {}, 40000};
    pure_query(service, first.value(), any);
    CHECK(service.bind(first.value(), udp_link.local_ip, 40001).code() == StatusCode::already_exists);
    CHECK(service.bind(second.value(), {}, 40000).code() == StatusCode::already_exists);
    pure_query(service, first.value(), any);
    pure_query(service, second.value(), {});
    CHECK(service.bind(second.value(), udp_link.local_ip, 40001));
    pure_query(service, second.value(), {true, udp_link.local_ip, 40001});
    const auto automatic = service.open(); CHECK(automatic);
    std::array<std::byte, NativeUdpService::frame_capacity> bytes{};
    CHECK(!service.build_datagram(automatic.value(), udp_link.local_ip, 40000, payload, bytes));
    CHECK(!service.build_datagram(automatic.value(), udp_link.peer_ip, 0, payload, {bytes.data(), 1}));
    CHECK(!service.build_datagram(automatic.value(), udp_link.peer_ip, 40000, payload, {bytes.data(), 1}));
    pure_query(service, automatic.value(), {});
    const auto built = service.build_datagram(automatic.value(), udp_link.peer_ip, 40000, payload, bytes);
    CHECK(built);
    const auto binding = service.binding(automatic.value()); CHECK(binding);
    CHECK(binding.value().bound && binding.value().address == udp_link.local_ip);
    CHECK(binding.value().port >= 49152);
    CHECK(wire::be16({bytes.data(), built.value()}, 34) == binding.value().port);
    pure_query(service, automatic.value(), binding.value());
    CHECK(service.close(first.value())); CHECK(service.close(second.value())); CHECK(service.close(automatic.value()));
    CHECK(service.active_sockets() == 0);
}

void generation_and_failed_bind()
{
    NativeUdpService service;
    const auto before_config = service.open(); CHECK(before_config);
    CHECK(!service.bind(before_config.value(), {}, 40000));
    pure_query(service, before_config.value(), {});
    CHECK(service.configure(udp_link));
    CHECK(!service.bind(before_config.value(), {}, 80));
    CHECK(!service.bind(before_config.value(), udp_link.peer_ip, 40000));
    pure_query(service, before_config.value(), {});
    CHECK(service.bind(before_config.value(), udp_link.local_ip, 40000));
    const auto copied = service.binding(before_config.value()); CHECK(copied);
    CHECK(service.close(before_config.value()));
    CHECK(service.binding(before_config.value()).status().code() == StatusCode::bad_descriptor);
    const auto reused = service.open(); CHECK(reused);
    CHECK(reused.value() != before_config.value() && (reused.value() & 255) == (before_config.value() & 255));
    CHECK(service.binding(before_config.value()).status().code() == StatusCode::bad_descriptor);
    CHECK((copied.value() == NativeUdpBindingSnapshot{true, udp_link.local_ip, 40000}));
    pure_query(service, reused.value(), {});
    for (u32 invalid : {u32{0}, u32{17}, u32{256}, u32{0xffffff01}})
        CHECK(service.binding(invalid).status().code() == StatusCode::bad_descriptor);
    CHECK(service.close(reused.value()));
}

void terminal_query()
{
    NativeUdpService service{NativeUdpReadinessCapacity{1}}, peer;
    CHECK(service.configure(udp_link));
    CHECK(peer.configure({udp_link.peer_mac, udp_link.local_mac, udp_link.peer_ip, udp_link.local_ip}));
    const auto target = service.open(), sender = peer.open(); CHECK(target && sender);
    CHECK(service.bind(target.value(), {}, 40000));
    std::array<std::byte, NativeUdpService::frame_capacity> bytes{}, reply{};
    const auto built = peer.build_datagram(sender.value(), udp_link.local_ip, 40000, {}, bytes); CHECK(built);
    const auto received = service.ingest({bytes.data(), built.value()}, reply); CHECK(received);
    pure_query(service, target.value(), {true, {}, 40000});
    CHECK(service.peek(target.value()) && service.peek(target.value()).value().payload.empty());
    CHECK(service.consume(target.value()));
    CHECK(!service.status() && service.readiness(target.value()).value().terminal);
    pure_query(service, target.value(), {true, {}, 40000});
    CHECK(service.close(target.value())); CHECK(peer.close(sender.value()));
    CHECK(service.binding(target.value()).status().code() == StatusCode::bad_descriptor);
}

int main()
{
    explicit_and_autobind(); generation_and_failed_bind(); terminal_query();
    std::printf("NATIVE_UDP_BINDING_MODEL_PASS checks=%zu hosted=1 guest=0 actual_nic=0\n", checks);
}
