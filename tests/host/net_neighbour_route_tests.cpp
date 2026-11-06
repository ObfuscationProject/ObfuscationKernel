#include <ok/net/neighbour.hpp>
#include <ok/net/route.hpp>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ok;
namespace wire = ok::net::wire;
using net::NeighbourState;
using net::NeighbourUpdate;

namespace
{
usize checks{};
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "NET_NEIGHBOUR_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #condition); \
    std::abort(); } } while (false)

const wire::Mac mac_a{2, 0, 0, 0, 0, 0xa}, mac_b{2, 0, 0, 0, 0, 0xb};
const wire::Ipv4 gateway{10, 0, 2, 2}, peer{10, 0, 2, 3}, other{10, 0, 2, 4};
using Cache = net::NeighbourCache<wire::Ipv4, 4, 4, 64>;
using Solicitations = std::array<net::Solicitation<wire::Ipv4>, 4>;

std::vector<std::byte> packet(u8 tag, usize size = 20)
{ return std::vector<std::byte>(size, std::byte{tag}); }
NeighbourState state(const Cache &cache, wire::Ipv4 address) { return cache.find(address)->state; }

void resolution_and_hold()
{
    Cache cache; Solicitations out{};
    CHECK(cache.resolve(gateway, 100).status().code() == StatusCode::would_block);
    CHECK(state(cache, gateway) == NeighbourState::incomplete);
    CHECK(cache.resolve(gateway, 100).status().code() == StatusCode::would_block);
    for (u8 tag = 1; tag <= 4; ++tag) { auto p = packet(tag); CHECK(cache.hold(gateway, p)); }
    // At most three per neighbour: the fourth replaced the oldest.
    CHECK(cache.pending() == 3 && cache.stats().pending_dropped == 1);
    CHECK(!cache.hold(peer, packet(9)));
    auto due = cache.tick(100, out); CHECK(due && due.value() == 1 && out[0].target == gateway && !out[0].unicast);
    due = cache.tick(500, out); CHECK(due && due.value() == 0);
    due = cache.tick(1100, out); CHECK(due && due.value() == 1);
    // An unsolicited advertisement resolves to STALE; a solicited one to REACHABLE.
    auto resolved = cache.update(gateway, mac_a, NeighbourUpdate::advertisement, {true, false, true}, 1200);
    CHECK(resolved && resolved.value() && state(cache, gateway) == NeighbourState::reachable);
    CHECK(cache.find(gateway)->router && cache.stats().resolved == 1);
    std::vector<u8> order;
    auto flushed = cache.flush([&](const wire::Mac &mac, const wire::Ipv4 &address, wire::Bytes bytes) {
        CHECK(mac == mac_a && address == gateway && bytes.size() == 20);
        order.push_back(std::to_integer<u8>(bytes[0])); return order.size() < 2; });
    CHECK(flushed == 1 && cache.pending() == 2 && (order == std::vector<u8>{2, 3}));
    flushed = cache.flush([&](const wire::Mac &, const wire::Ipv4 &, wire::Bytes bytes) {
        order.push_back(std::to_integer<u8>(bytes[0])); return true; });
    CHECK(flushed == 2 && cache.pending() == 0 && (order == std::vector<u8>{2, 3, 3, 4}));
    auto mac = cache.resolve(gateway, 1300); CHECK(mac && mac.value() == mac_a);
}

void failure_after_multicast_probes()
{
    Cache cache; Solicitations out{};
    CHECK(!cache.resolve(peer, 0));
    CHECK(cache.hold(peer, packet(1)));
    u64 now = 0; usize sent = 0;
    for (int i = 0; i < 5; ++i, now += 1000) { auto due = cache.tick(now, out); CHECK(due); sent += due.value(); }
    CHECK(sent == 3 && !cache.find(peer) && cache.pending() == 0);
    CHECK(cache.stats().failed == 1 && cache.stats().pending_dropped == 1);
    CHECK(!cache.next_deadline());
}

void reachability_lifecycle()
{
    Cache cache; Solicitations out{};
    // An ARP request or a solicitation with a source address creates STALE.
    auto learned = cache.update(peer, mac_a, NeighbourUpdate::announcement, {}, 0);
    CHECK(learned && learned.value() && state(cache, peer) == NeighbourState::stale);
    CHECK(!cache.next_deadline());
    // Use starts DELAY; no confirmation within 5 s moves to PROBE with unicast probes.
    auto mac = cache.resolve(peer, 10); CHECK(mac && mac.value() == mac_a && state(cache, peer) == NeighbourState::delay);
    CHECK(cache.next_deadline() == 5010u);
    auto due = cache.tick(5010, out);
    CHECK(due && due.value() == 1 && out[0].unicast && *out[0].unicast == mac_a);
    CHECK(state(cache, peer) == NeighbourState::probe);
    // Still usable while probing.
    CHECK(cache.resolve(peer, 5020));
    auto confirmed = cache.update(peer, mac_a, NeighbourUpdate::advertisement, {true, false, false}, 5100);
    CHECK(confirmed && state(cache, peer) == NeighbourState::reachable && cache.find(peer)->probes == 0);
    due = cache.tick(35100, out); CHECK(due && due.value() == 0 && state(cache, peer) == NeighbourState::stale);
    // Upper-layer confirmation restores REACHABLE without a probe.
    CHECK(cache.resolve(peer, 35200)); cache.confirm(peer, 35300);
    CHECK(state(cache, peer) == NeighbourState::reachable);
    // Unanswered unicast probes delete the entry.
    CHECK(cache.tick(65300, out) && state(cache, peer) == NeighbourState::stale);
    CHECK(cache.resolve(peer, 65400));
    usize probes = 0;
    for (u64 now = 70400; now < 80000; now += 1000) { auto d = cache.tick(now, out); CHECK(d); probes += d.value(); }
    CHECK(probes == 3 && !cache.find(peer));
}

void advertisement_rules()
{
    Cache cache;
    // RFC 4861 7.2.5: an advertisement for an unknown neighbour creates nothing.
    auto ignored = cache.update(peer, mac_a, NeighbourUpdate::advertisement, {true, true, false}, 0);
    CHECK(ignored && !ignored.value() && !cache.find(peer) && cache.stats().ignored == 1);
    CHECK(!cache.resolve(peer, 0));
    CHECK(cache.update(peer, {}, NeighbourUpdate::advertisement, {true, true, false}, 1) && state(cache, peer) == NeighbourState::incomplete);
    CHECK(cache.update(peer, mac_a, NeighbourUpdate::advertisement, {true, true, false}, 2) && state(cache, peer) == NeighbourState::reachable);
    // Without override a different address only demotes REACHABLE to STALE.
    CHECK(cache.update(peer, mac_b, NeighbourUpdate::advertisement, {true, false, false}, 3));
    CHECK(state(cache, peer) == NeighbourState::stale && cache.find(peer)->mac == mac_a);
    CHECK(cache.update(peer, mac_b, NeighbourUpdate::advertisement, {false, false, false}, 4));
    CHECK(state(cache, peer) == NeighbourState::stale && cache.find(peer)->mac == mac_a);
    // Override replaces it; unsolicited leaves STALE.
    CHECK(cache.update(peer, mac_b, NeighbourUpdate::advertisement, {false, true, false}, 5));
    CHECK(state(cache, peer) == NeighbourState::stale && cache.find(peer)->mac == mac_b);
    // An announcement with a new address replaces it and demotes to STALE.
    CHECK(cache.update(peer, mac_b, NeighbourUpdate::advertisement, {true, true, false}, 6) && state(cache, peer) == NeighbourState::reachable);
    CHECK(cache.update(peer, mac_b, NeighbourUpdate::announcement, {}, 7) && state(cache, peer) == NeighbourState::reachable);
    CHECK(cache.update(peer, mac_a, NeighbourUpdate::announcement, {}, 8) && state(cache, peer) == NeighbourState::stale);
    CHECK(!cache.update(peer, wire::broadcast_mac, NeighbourUpdate::announcement, {}, 9));
    // Router flag follows advertisements.
    CHECK(cache.update(peer, {}, NeighbourUpdate::advertisement, {true, false, true}, 10) && cache.find(peer)->router);
    CHECK(cache.update(peer, {}, NeighbourUpdate::advertisement, {true, false, false}, 11) && !cache.find(peer)->router);
}

void permanent_and_eviction()
{
    Cache cache; Solicitations out{};
    CHECK(cache.add_permanent(gateway, mac_a, 0));
    CHECK(cache.update(gateway, mac_b, NeighbourUpdate::announcement, {}, 1) && cache.find(gateway)->mac == mac_a);
    CHECK(cache.update(gateway, mac_b, NeighbourUpdate::advertisement, {true, true, false}, 1) && cache.find(gateway)->mac == mac_a);
    CHECK(cache.tick(1000000, out) && state(cache, gateway) == NeighbourState::permanent);
    CHECK(!cache.add_permanent(peer, wire::broadcast_mac, 0));
    CHECK(cache.update(peer, mac_a, NeighbourUpdate::announcement, {}, 10));
    CHECK(cache.update(other, mac_a, NeighbourUpdate::announcement, {}, 20));
    CHECK(!cache.resolve(wire::Ipv4{10, 0, 2, 5}, 30));
    CHECK(cache.hold(wire::Ipv4{10, 0, 2, 5}, packet(1)));
    CHECK(cache.size() == 4);
    // Full: the least recently used dynamic entry without held packets goes.
    CHECK(cache.update(wire::Ipv4{10, 0, 2, 6}, mac_b, NeighbourUpdate::announcement, {}, 40));
    CHECK(!cache.find(peer) && cache.find(other) && cache.find(gateway) && cache.stats().evicted == 1);
    CHECK(cache.remove(other) && !cache.remove(other));
    // Nothing evictable: permanent, incomplete and the newest remain.
    net::NeighbourCache<wire::Ipv4, 2, 2, 64> small;
    CHECK(small.add_permanent(gateway, mac_a, 0) && !small.resolve(peer, 0));
    CHECK(small.resolve(other, 0).status().code() == StatusCode::no_memory);
}

void ipv6_instantiation()
{
    net::NeighbourCache<wire::Ipv6> cache; std::array<net::Solicitation<wire::Ipv6>, 2> out{};
    const auto router = wire::link_local_from_mac({0x52, 0x56, 0, 0, 0, 2});
    CHECK(!cache.resolve(router, 0));
    auto due = cache.tick(0, out); CHECK(due && due.value() == 1 && out[0].target == router);
    CHECK(cache.update(router, wire::Mac{0x52, 0x56, 0, 0, 0, 2}, NeighbourUpdate::advertisement, {true, true, true}, 1));
    CHECK(cache.find(router)->state == NeighbourState::reachable && cache.find(router)->router);
}

void routes()
{
    net::RouteTable<wire::Ipv4, 4> table;
    CHECK(table.add({{10, 0, 2, 99}, 24, {}, 1, 0, net::RouteOrigin::connected}));
    CHECK(table.add({{0, 0, 0, 0}, 0, wire::Ipv4{10, 0, 2, 2}, 1, 100}));
    table.each([&](const net::Route<wire::Ipv4> &r) { if (r.length == 24) CHECK((r.prefix == wire::Ipv4{10, 0, 2, 0})); });
    auto local = table.lookup({10, 0, 2, 3}, 0);
    CHECK(local && !local.value().gateway && (local.value().next_hop == wire::Ipv4{10, 0, 2, 3}) && local.value().length == 24);
    auto remote = table.lookup({93, 184, 216, 34}, 0);
    CHECK(remote && remote.value().gateway && remote.value().next_hop == gateway && remote.value().interface == 1);
    // A second default route with a lower metric wins; equal metrics keep the older.
    CHECK(table.add({{}, 0, wire::Ipv4{10, 0, 2, 3}, 2, 50}));
    remote = table.lookup({8, 8, 8, 8}, 0); CHECK(remote && remote.value().next_hop == peer && remote.value().interface == 2);
    CHECK(table.add({{}, 0, wire::Ipv4{10, 0, 2, 3}, 2, 100}));
    remote = table.lookup({8, 8, 8, 8}, 0); CHECK(remote && remote.value().next_hop == gateway);
    CHECK(table.size() == 3);
    CHECK(table.add({{192, 168, 0, 0}, 16, {}, 3, 0}));
    CHECK(table.add({{172, 16, 0, 0}, 12, {}, 3, 0}).code() == StatusCode::no_memory);
    CHECK(!table.add({{}, 33, {}, 1, 0}));
    CHECK(!table.add({{}, 0, wire::Ipv4{224, 0, 0, 1}, 1, 0}));
    CHECK(table.remove({8, 8, 8, 8}, 0, gateway, 1));
    remote = table.lookup({8, 8, 8, 8}, 0); CHECK(remote && remote.value().next_hop == peer);
    CHECK(table.remove_if([](const net::Route<wire::Ipv4> &r) { return r.interface == 2; }) == 1);
    CHECK(!table.lookup({8, 8, 8, 8}, 0) && table.lookup({8, 8, 8, 8}, 0).status().code() == StatusCode::not_found);
    CHECK(!table.remove({8, 8, 8, 8}, 0, gateway, 1));

    // Router-advertised IPv6 routes expire and refresh in place.
    net::RouteTable<wire::Ipv6> six;
    const auto router = wire::link_local_from_mac({0x52, 0x56, 0, 0, 0, 2});
    wire::Ipv6 prefix{0x20, 0x01, 0x0d, 0xb8, 0, 1}; prefix[15] = 0x55;
    CHECK(six.add({prefix, 64, {}, 1, 256, net::RouteOrigin::router_advertisement, 86400000}));
    CHECK(six.add({{}, 0, router, 1, 1024, net::RouteOrigin::router_advertisement, 1800000}));
    CHECK(six.add({{0xfe, 0x80}, 64, {}, 1, 256, net::RouteOrigin::connected}));
    wire::Ipv6 global = prefix; global[15] = 7;
    auto onlink = six.lookup(global, 0); CHECK(onlink && !onlink.value().gateway && onlink.value().next_hop == global);
    wire::Ipv6 internet{0x26, 0x06, 0x47, 0}; internet[15] = 1;
    auto via = six.lookup(internet, 0); CHECK(via && via.value().gateway && via.value().next_hop == router);
    CHECK(six.next_expiry() == 1800000u);
    CHECK(!six.lookup(internet, 1800000));
    CHECK(six.add({{}, 0, router, 1, 1024, net::RouteOrigin::router_advertisement, 3600000}) && six.size() == 3);
    CHECK(six.lookup(internet, 1800000));
    CHECK(six.expire(3600000) == 1 && six.size() == 2 && !six.lookup(internet, 0));
    CHECK(net::mask_prefix(wire::Ipv4{10, 0, 3, 255}, 23) == (wire::Ipv4{10, 0, 2, 0}));
    CHECK(net::mask_prefix(wire::Ipv4{255, 255, 255, 255}, 0) == wire::Ipv4{});
    CHECK(net::mask_prefix(wire::Ipv4{1, 2, 3, 4}, 32) == (wire::Ipv4{1, 2, 3, 4}));
}
}

int main()
{
    resolution_and_hold();
    failure_after_multicast_probes();
    reachability_lifecycle();
    advertisement_rules();
    permanent_and_eviction();
    ipv6_instantiation();
    routes();
    std::printf("PASS net neighbour/route scope=model checks=%zu\n", checks);
}
