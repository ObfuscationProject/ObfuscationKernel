#pragma once

#include <ok/net/ip.hpp>
#include <optional>

namespace ok::net
{
enum class RouteOrigin : u8 { connected, configured, router_advertisement };

template <class Address>
struct Route
{
    Address prefix{};
    u8 length{};
    std::optional<Address> gateway{};
    u16 interface{};
    u32 metric{};
    RouteOrigin origin{RouteOrigin::configured};
    // Monotonic milliseconds; ~0 never expires.
    u64 expires{~u64{0}};
};

template <class Address>
struct RouteLookup
{
    u16 interface{};
    Address next_hop{};
    bool gateway{};
    u8 length{};
    u32 metric{};
};

template <class Address>
inline constexpr Address mask_prefix(Address address, u8 length)
{
    for (usize i = 0; i < address.size(); ++i)
    {
        const usize bit = i * 8u;
        if (bit >= length) address[i] = 0;
        else if (length - bit < 8) address[i] &= static_cast<u8>(0xffu << (8u - (length - bit)));
    }
    return address;
}

// Longest-prefix match, then lowest metric, then the older route. One table
// per address family; the owner serializes every call.
template <class Address, usize Capacity = 16>
class RouteTable final
{
    static_assert(Capacity > 0);
  public:
    static constexpr u8 width = static_cast<u8>(sizeof(Address) * 8);
    RouteTable() = default;
    RouteTable(const RouteTable &) = delete;
    RouteTable &operator=(const RouteTable &) = delete;

    [[nodiscard]] usize size() const { usize n{}; for (const auto &s : slots_) n += s.used; return n; }
    template <class Visit> void each(Visit &&visit) const { for (const auto &s : slots_) if (s.used) visit(s.route); }

    // Adds a route or, for the same prefix, gateway and interface, refreshes
    // its metric, origin and lifetime (router advertisements re-announce).
    Status add(Route<Address> route)
    {
        if (route.length > width) return Status::invalid_argument("route prefix length exceeds the address width");
        if (route.gateway && (wire::multicast(*route.gateway) || wire::unspecified(*route.gateway)))
            return Status::invalid_argument("route gateway is not a unicast address");
        route.prefix = mask_prefix(route.prefix, route.length);
        if (auto *slot = exact(route.prefix, route.length, route.gateway, route.interface))
        { slot->route = route; return Status::success(); }
        for (auto &slot : slots_)
            if (!slot.used) { slot.used = true; slot.order = ++order_; slot.route = route; return Status::success(); }
        return Status::no_memory("route table is full");
    }
    Status remove(const Address &prefix, u8 length, std::optional<Address> gateway, u16 interface)
    {
        auto *slot = exact(mask_prefix(prefix, length), length, gateway, interface);
        if (!slot) return Status::not_found("route is unknown");
        *slot = {}; return Status::success();
    }
    template <class Match> usize remove_if(Match &&match)
    {
        usize removed = 0;
        for (auto &slot : slots_) if (slot.used && match(slot.route)) { slot = {}; ++removed; }
        return removed;
    }
    usize expire(u64 now) { return remove_if([now](const Route<Address> &r) { return now >= r.expires; }); }
    [[nodiscard]] std::optional<u64> next_expiry() const
    {
        std::optional<u64> next;
        for (const auto &s : slots_) if (s.used && s.route.expires != ~u64{0} && (!next || s.route.expires < *next)) next = s.route.expires;
        return next;
    }

    // A route is usable until its expiry; expire() reclaims it later.
    [[nodiscard]] Result<RouteLookup<Address>> lookup(const Address &destination, u64 now) const
    {
        const Slot *best = nullptr;
        for (const auto &slot : slots_)
        {
            if (!slot.used || now >= slot.route.expires || !wire::prefix_match(slot.route.prefix, destination, slot.route.length)) continue;
            if (!best || slot.route.length > best->route.length ||
                (slot.route.length == best->route.length && (slot.route.metric < best->route.metric ||
                    (slot.route.metric == best->route.metric && slot.order < best->order))))
                best = &slot;
        }
        if (!best) return Status::not_found("no route to destination");
        const auto &r = best->route;
        return RouteLookup<Address>{r.interface, r.gateway ? *r.gateway : destination, r.gateway.has_value(), r.length, r.metric};
    }

  private:
    struct Slot { bool used{}; u64 order{}; Route<Address> route{}; };
    Slot *exact(const Address &prefix, u8 length, const std::optional<Address> &gateway, u16 interface)
    {
        for (auto &slot : slots_)
            if (slot.used && slot.route.length == length && slot.route.prefix == prefix &&
                slot.route.gateway == gateway && slot.route.interface == interface) return &slot;
        return nullptr;
    }
    std::array<Slot, Capacity> slots_{};
    u64 order_{};
};
}
