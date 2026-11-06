#pragma once

#include <ok/net/ip.hpp>
#include <optional>

namespace ok::net
{
// RFC 4861 section 7.3.2 states. ARP shares them as Linux does: an ARP
// request names its sender like a solicitation with a source link-layer
// address, an ARP reply confirms like a solicited advertisement.
enum class NeighbourState : u8 { incomplete, reachable, stale, delay, probe, permanent };

struct NeighbourTiming
{
    u64 reachable_ms{30000}, retransmit_ms{1000}, delay_ms{5000};
    u8 multicast_probes{3}, unicast_probes{3};
};

enum class NeighbourUpdate : u8
{
    // The peer named itself (NS/RS/RA source option, ARP request or claim).
    announcement,
    // A neighbour advertisement or ARP reply; flags as in RFC 4861 section 7.2.5.
    advertisement,
};
struct NeighbourAdvertisement { bool solicited{}, override_flag{true}, router{}; };

template <class Address>
struct Solicitation
{
    Address target{};
    // A multicast probe resolves an incomplete entry; a unicast probe
    // verifies a known address in PROBE state.
    std::optional<wire::Mac> unicast{};
};

template <class Address>
struct NeighbourSnapshot
{
    Address address{};
    wire::Mac mac{};
    NeighbourState state{};
    bool router{};
    u8 probes{};
    u64 deadline{};
    usize pending{};
};

struct NeighbourStats { u64 resolved{}, failed{}, evicted{}, pending_dropped{}, ignored{}; };

// One serialized owner (the interface). Entries and held packets live in this
// object; flushed packets are borrowed only for the duration of the sink call.
template <class Address, usize Capacity = 16, usize Pending = 4, usize PacketBytes = 1500>
class NeighbourCache final
{
    static_assert(Capacity > 0 && Pending > 0 && PacketBytes > 0);
  public:
    static constexpr usize per_neighbour_pending = Pending < 3 ? Pending : 3;
    explicit NeighbourCache(NeighbourTiming timing = {}) : timing_{timing} {}
    NeighbourCache(const NeighbourCache &) = delete;
    NeighbourCache &operator=(const NeighbourCache &) = delete;

    [[nodiscard]] const NeighbourStats &stats() const { return stats_; }
    [[nodiscard]] const NeighbourTiming &timing() const { return timing_; }
    void set_reachable_time(u64 ms) { if (ms) timing_.reachable_ms = ms; }
    void set_retransmit_time(u64 ms) { if (ms) timing_.retransmit_ms = ms; }
    [[nodiscard]] usize size() const { usize n{}; for (const auto &e : entries_) n += e.used; return n; }
    [[nodiscard]] usize pending() const { usize n{}; for (const auto &p : pending_) n += p.used; return n; }

    [[nodiscard]] std::optional<NeighbourSnapshot<Address>> find(const Address &address) const
    {
        const auto *entry = lookup(address); if (!entry) return {};
        return NeighbourSnapshot<Address>{entry->address, entry->mac, entry->state, entry->router, entry->probes,
                                          entry->deadline, held(address)};
    }
    template <class Visit> void each(Visit &&visit) const
    { for (const auto &e : entries_) if (e.used) visit(NeighbourSnapshot<Address>{e.address, e.mac, e.state, e.router, e.probes, e.deadline, held(e.address)}); }

    // Link-layer address for a transmission now. A missing neighbour becomes
    // INCOMPLETE and its first multicast solicitation is due on the next tick;
    // the caller then holds the packet. STALE starts the delay timer.
    Result<wire::Mac> resolve(const Address &address, u64 now)
    {
        auto *entry = lookup(address);
        if (!entry)
        {
            auto created = create(address, now); if (!created) return created.status();
            entry = created.value(); entry->state = NeighbourState::incomplete; entry->deadline = now;
            return Status::would_block("neighbour resolution started");
        }
        entry->used_at = now;
        if (entry->state == NeighbourState::incomplete) return Status::would_block("neighbour resolution is pending");
        if (entry->state == NeighbourState::stale) { entry->state = NeighbourState::delay; entry->deadline = deadline(now, timing_.delay_ms); }
        return entry->mac;
    }

    // Holds a network-layer packet for an INCOMPLETE neighbour. Each neighbour
    // keeps at most three; a fourth replaces its oldest, as Linux does.
    Status hold(const Address &address, wire::Bytes packet)
    {
        const auto *entry = lookup(address);
        if (!entry || entry->state != NeighbourState::incomplete) return Status::invalid_argument("neighbour is not resolving");
        if (packet.empty() || packet.size() > PacketBytes) return Status::overflow("held packet exceeds its slot");
        Packet *slot = nullptr;
        if (held(address) >= per_neighbour_pending) slot = oldest(&address);
        if (!slot) for (auto &p : pending_) if (!p.used) { slot = &p; break; }
        if (!slot) { slot = oldest(nullptr); }
        if (!slot) return Status::no_memory("neighbour pending pool is exhausted");
        if (slot->used) add(stats_.pending_dropped);
        slot->used = true; slot->address = address; slot->length = packet.size(); slot->order = ++order_;
        for (usize i = 0; i < packet.size(); ++i) slot->bytes[i] = packet[i];
        return Status::success();
    }

    // Applies received link-layer information (RFC 4861 sections 7.2.3-7.2.5).
    // Returns whether the entry is usable now; held packets are then flushable.
    Result<bool> update(const Address &address, std::optional<wire::Mac> mac, NeighbourUpdate kind,
                        NeighbourAdvertisement flags, u64 now)
    {
        if (mac && wire::multicast(*mac)) { add(stats_.ignored); return Status::invalid_argument("neighbour link address is multicast"); }
        auto *entry = lookup(address);
        if (kind == NeighbourUpdate::announcement)
        {
            if (!mac) return false;
            if (!entry)
            {
                auto created = create(address, now); if (!created) return created.status();
                entry = created.value(); entry->mac = *mac; to(*entry, NeighbourState::stale, now); return true;
            }
            if (entry->state == NeighbourState::permanent) return true;
            if (entry->state == NeighbourState::incomplete || entry->mac != *mac)
            {
                if (entry->state == NeighbourState::incomplete) add(stats_.resolved);
                entry->mac = *mac; to(*entry, NeighbourState::stale, now);
            }
            return true;
        }
        if (!entry) { add(stats_.ignored); return false; }
        if (entry->state == NeighbourState::permanent) return true;
        if (entry->state == NeighbourState::incomplete)
        {
            if (!mac) { add(stats_.ignored); return false; }
            entry->mac = *mac; entry->router = flags.router; add(stats_.resolved);
            to(*entry, flags.solicited ? NeighbourState::reachable : NeighbourState::stale, now);
            return true;
        }
        const bool changed = mac && *mac != entry->mac;
        if (!flags.override_flag && changed)
        {
            if (entry->state == NeighbourState::reachable) to(*entry, NeighbourState::stale, now);
            return true;
        }
        if (changed) entry->mac = *mac;
        if (flags.solicited) to(*entry, NeighbourState::reachable, now);
        else if (changed) to(*entry, NeighbourState::stale, now);
        entry->router = flags.router;
        return true;
    }

    // Forward progress from an upper layer, e.g. a TCP acknowledgment.
    void confirm(const Address &address, u64 now)
    {
        auto *entry = lookup(address);
        if (entry && entry->state != NeighbourState::incomplete && entry->state != NeighbourState::permanent)
            to(*entry, NeighbourState::reachable, now);
    }

    Status add_permanent(const Address &address, wire::Mac mac, u64 now)
    {
        if (wire::multicast(mac)) return Status::invalid_argument("neighbour link address is multicast");
        auto *entry = lookup(address);
        if (!entry) { auto created = create(address, now); if (!created) return created.status(); entry = created.value(); }
        entry->mac = mac; entry->state = NeighbourState::permanent; entry->deadline = 0; entry->probes = 0;
        return Status::success();
    }
    Status remove(const Address &address)
    {
        auto *entry = lookup(address); if (!entry) return Status::not_found("neighbour is unknown");
        drop(*entry); return Status::success();
    }

    // Moves timed states and reports the solicitations now due; a neighbour
    // whose probes are exhausted is deleted with its held packets.
    Result<usize> tick(u64 now, std::span<Solicitation<Address>> out)
    {
        usize count = 0;
        for (auto &entry : entries_)
        {
            if (!entry.used || entry.state == NeighbourState::permanent || now < entry.deadline) continue;
            switch (entry.state)
            {
            case NeighbourState::reachable: to(entry, NeighbourState::stale, now); break;
            case NeighbourState::stale: entry.deadline = ~u64{0}; break;
            case NeighbourState::delay:
                entry.state = NeighbourState::probe; entry.probes = 0; [[fallthrough]];
            case NeighbourState::probe:
            case NeighbourState::incomplete:
            {
                const bool multicast = entry.state == NeighbourState::incomplete;
                if (entry.probes >= (multicast ? timing_.multicast_probes : timing_.unicast_probes))
                { add(stats_.failed); drop(entry); break; }
                if (count == out.size()) break;
                out[count++] = {entry.address, multicast ? std::optional<wire::Mac>{} : std::optional<wire::Mac>{entry.mac}};
                ++entry.probes; entry.deadline = deadline(now, timing_.retransmit_ms);
                break;
            }
            case NeighbourState::permanent: break;
            }
        }
        return count;
    }
    // Earliest timer of any entry, for the interface's wake deadline.
    [[nodiscard]] std::optional<u64> next_deadline() const
    {
        std::optional<u64> next;
        for (const auto &e : entries_)
            if (e.used && e.state != NeighbourState::permanent && e.state != NeighbourState::stale && (!next || e.deadline < *next))
                next = e.deadline;
        return next;
    }

    // Offers held packets of resolved neighbours in arrival order. The sink
    // returns false to keep a packet (e.g. no transmit credit) and stop.
    template <class Sink>
        requires requires(Sink &sink, const wire::Mac &mac, const Address &address, wire::Bytes packet)
        { { sink(mac, address, packet) } -> std::same_as<bool>; }
    usize flush(Sink &&sink)
    {
        usize sent = 0;
        for (;;)
        {
            Packet *next = nullptr;
            for (auto &p : pending_)
            {
                if (!p.used) continue;
                const auto *entry = lookup(p.address);
                if (!entry || entry->state == NeighbourState::incomplete) continue;
                if (!next || p.order < next->order) next = &p;
            }
            if (!next) return sent;
            const auto *entry = lookup(next->address);
            if (!sink(entry->mac, next->address, wire::Bytes{next->bytes.data(), next->length})) return sent;
            next->used = false; ++sent;
        }
    }

  private:
    struct Entry
    {
        bool used{}, router{}; Address address{}; wire::Mac mac{}; NeighbourState state{};
        u8 probes{}; u64 deadline{}, used_at{};
    };
    struct Packet { bool used{}; Address address{}; usize length{}; u64 order{}; std::array<std::byte, PacketBytes> bytes{}; };
    static void add(u64 &value) { if (value != ~u64{0}) ++value; }
    static constexpr u64 deadline(u64 now, u64 delay) { return now > ~u64{0} - delay ? ~u64{0} : now + delay; }
    Entry *lookup(const Address &address)
    { for (auto &e : entries_) if (e.used && e.address == address) return &e; return nullptr; }
    const Entry *lookup(const Address &address) const
    { for (const auto &e : entries_) if (e.used && e.address == address) return &e; return nullptr; }
    usize held(const Address &address) const
    { usize n{}; for (const auto &p : pending_) n += p.used && p.address == address; return n; }
    Packet *oldest(const Address *address)
    {
        Packet *found = nullptr;
        for (auto &p : pending_)
            if (p.used && (!address || p.address == *address) && (!found || p.order < found->order)) found = &p;
        return found;
    }
    void to(Entry &entry, NeighbourState state, u64 now)
    {
        entry.state = state; entry.probes = 0;
        entry.deadline = state == NeighbourState::reachable ? deadline(now, timing_.reachable_ms) : ~u64{0};
    }
    void drop(Entry &entry)
    {
        for (auto &p : pending_) if (p.used && p.address == entry.address) { p.used = false; add(stats_.pending_dropped); }
        entry = {};
    }
    // Replaces the least recently used dynamic entry that holds no packets.
    Result<Entry *> create(const Address &address, u64 now)
    {
        Entry *slot = nullptr;
        for (auto &e : entries_) if (!e.used) { slot = &e; break; }
        if (!slot)
            for (auto &e : entries_)
                if (e.state != NeighbourState::permanent && e.state != NeighbourState::incomplete && !held(e.address) &&
                    (!slot || e.used_at < slot->used_at)) slot = &e;
        if (!slot) return Status::no_memory("neighbour cache is exhausted");
        if (slot->used) { add(stats_.evicted); drop(*slot); }
        slot->used = true; slot->address = address; slot->used_at = now;
        return slot;
    }
    NeighbourTiming timing_{};
    std::array<Entry, Capacity> entries_{};
    std::array<Packet, Pending> pending_{};
    u64 order_{};
    NeighbourStats stats_{};
};
}
