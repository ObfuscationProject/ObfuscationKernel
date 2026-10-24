#pragma once

#include "ok/block/session_counters.hpp"
#include "ok/driver/driver.hpp"
#include "ok/smp/smp.hpp"

#include <array>

namespace ok::block
{
enum class SessionAccess : u8 { readonly, writable };
enum class DeviceState : u8 { unused, registering, ready, owned, quarantined, exhausted };
struct SessionBounds { u64 first{}, count{}; };
struct SessionReport
{
    using counter_type = SessionCounter;
    DeviceState state{DeviceState::unused};
    StatusCode first_error{StatusCode::ok};
    u64 identity{}, registration_generation{}, session_serial{};
    SessionCounter reads{}, writes{}, flushes{}, completed_reads{}, completed_writes{}, completed_flushes{};
    bool in_flight{}, pending_writes{};
};

// An inert immutable observation, never a capability or a retained device lease.
struct SessionBindingObservation
{
    const void *service{}, *session{};
    const driver::BlockDevice *device{};
    usize registration_slot{}, maximum_transfer{};
    u64 identity{}, registration_generation{}, session_serial{}, first_sector{}, sectors{}, device_sectors{};
    u32 sector_bytes{}, device_sector_bytes{};
    bool writable{}, device_writable{};
    friend bool operator==(const SessionBindingObservation &, const SessionBindingObservation &) = default;
};

// One authoritative registration/publication service for these raw devices.
// Service storage outlives all registration keys, sessions and requests.
// Raw device/backing storage remains owned through quarantine.
// Raw aliases and separately instantiated services are excluded by the owner.
template <usize Capacity = 4>
    requires (Capacity > 0)
class BlockSessions final
{
  public:
    class Registration final
    {
        friend class BlockSessions;
      public:
        Registration() = default;
      private:
        Registration(const BlockSessions *owner, usize slot, u64 generation)
            : owner_(owner), slot_(slot), generation_(generation) {}
        const BlockSessions *owner_{};
        usize slot_{};
        u64 generation_{};
    };
    class Session final : public driver::BlockDevice
    {
        friend class BlockSessions;
      public:
        using counter_type = SessionCounter;
        Session() = default;
        ~Session() override { if (service_) static_cast<void>(service_->abandon(*this)); }
        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;
        Session(Session &&) = delete;
        Session &operator=(Session &&) = delete;
        [[nodiscard]] driver::BlockGeometry geometry() const override { return geometry_; }
        [[nodiscard]] SessionBounds bounds() const { return {first_, geometry_.block_count}; }
        Status read_blocks(u64 first, std::span<std::byte> out) override
        {
            return service_ ? service_->template io<Operation::read>(*this, first, out, {}) :
                   Status::not_initialized("block session is unbound");
        }
        Status write_blocks(u64 first, std::span<const std::byte> in) override
        {
            return service_ ? service_->template io<Operation::write>(*this, first, {}, in) :
                   Status::not_initialized("block session is unbound");
        }
        Status flush() override
        {
            return service_ ? service_->template io<Operation::flush>(*this, 0, {}, {}) :
                   Status::not_initialized("block session is unbound");
        }
        // The borrowed guard is statically selected and is never retained by
        // the Session. It observes the exact attempted prefix while in flight,
        // before and after the runtime device boundary.
        template <typename Guard> requires requires(Guard &guard, Session &session)
            { guard.in_flight_exact(session); }
        [[nodiscard]] Status read_blocks_guarded(u64 first, std::span<std::byte> out, Guard &guard)
        { return service_ ? service_->template io<Operation::read>(*this, first, out, {}, guard) :
                Status::not_initialized("block session is unbound"); }
        template <typename Guard> requires requires(Guard &guard, Session &session)
            { guard.in_flight_exact(session); }
        [[nodiscard]] Status write_blocks_guarded(u64 first, std::span<const std::byte> in, Guard &guard)
        { return service_ ? service_->template io<Operation::write>(*this, first, {}, in, guard) :
                Status::not_initialized("block session is unbound"); }
        template <typename Guard> requires requires(Guard &guard, Session &session)
            { guard.in_flight_exact(session); }
        [[nodiscard]] Status flush_guarded(Guard &guard)
        { return service_ ? service_->template io<Operation::flush>(*this, 0, {}, {}, guard) :
                Status::not_initialized("block session is unbound"); }
        [[nodiscard]] Status close()
        { return service_ ? service_->close(*this) : Status::not_initialized("block session is unbound"); }
        [[nodiscard]] Status abandon()
        { return service_ ? service_->abandon(*this) : Status::not_initialized("block session is unbound"); }
        [[nodiscard]] Status abort(Status reason)
        { return service_ ? service_->abort(*this, reason) : Status::not_initialized("block session is unbound"); }
        [[nodiscard]] Result<SessionReport> report() const
        { return service_ ? service_->inspect(*this) : Result<SessionReport>{Status::not_initialized("block session is unbound")}; }
        [[nodiscard]] Result<usize> transfer_limit() const
        { return service_ ? service_->transfer_limit(*this) : Result<usize>{Status::not_initialized("block session is unbound")}; }
        [[nodiscard]] Result<SessionBindingObservation> observe_binding() const
        { return service_ ? service_->observe_binding(*this) :
                 Result<SessionBindingObservation>{Status::not_initialized("block session is unbound")}; }
        // Inert terminal bookkeeping only. Quarantine revokes ownership and
        // all transport; this observation grants no request or close authority.
        [[nodiscard]] Result<SessionBindingObservation> observe_terminal_binding() const
        { return service_ ? service_->observe_terminal_binding(*this) :
                 Result<SessionBindingObservation>{Status::not_initialized("block session is unbound")}; }
      private:
        BlockSessions *service_{};
        Registration registration_{};
        u64 serial_{}, first_{};
        driver::BlockGeometry geometry_{};
    };

    BlockSessions() = default;
    BlockSessions(const BlockSessions &) = delete;
    BlockSessions &operator=(const BlockSessions &) = delete;
    BlockSessions(BlockSessions &&) = delete;
    BlockSessions &operator=(BlockSessions &&) = delete;

    [[nodiscard]] Result<Registration> register_device(driver::BlockDevice &device, u64 identity,
                                                       usize maximum_transfer = 65536)
    {
        if (!identity) return Status::invalid_argument("block registration identity");
        Entry *reserved{};
        usize index{};
        {
            smp::ScopedSpinLock guard(lock_);
            for (const auto &entry : entries_)
                if (entry.device == &device || (entry.report.state != DeviceState::unused && entry.report.identity == identity))
                    return Status::already_exists("block device or identity already registered");
            for (; index < Capacity; ++index)
            {
                auto &entry = entries_[index];
                if (entry.report.state != DeviceState::unused) continue;
                if (entry.report.registration_generation == ~u64{0})
                { entry.report.state = DeviceState::exhausted; continue; }
                const u64 generation = entry.report.registration_generation + 1;
                entry = {}; entry.device = &device; entry.report.identity = identity;
                entry.report.registration_generation = generation;
                entry.report.state = DeviceState::registering; reserved = &entry; break;
            }
            if (!reserved) return Status::no_memory("block registration capacity exhausted");
        }
        // Reserve identity before the unlocked device call: even concurrent
        // duplicate registration must not query owned/quarantined transports.
        const auto geometry = device.geometry();
        smp::ScopedSpinLock guard(lock_);
        const auto generation = reserved->report.registration_generation;
        if (!geometry.block_count || geometry.block_size < 512 || geometry.block_size > 4096 ||
            (geometry.block_size & (geometry.block_size - 1)) || maximum_transfer < geometry.block_size ||
            maximum_transfer % geometry.block_size)
        {
            *reserved = {}; reserved->report.registration_generation = generation;
            return Status::invalid_argument("block registration geometry or transfer bound");
        }
        reserved->geometry = geometry; reserved->maximum_transfer = maximum_transfer;
        reserved->report.state = DeviceState::ready;
        return Registration{this, index, generation};
    }

    [[nodiscard]] Status acquire(Registration key, SessionBounds bounds, SessionAccess access, Session &session)
    {
        smp::ScopedSpinLock guard(lock_);
        if (session.service_) return Status::already_exists("block session object is one-shot");
        auto *entry = find(key);
        if (!entry) return Status::not_found("stale block registration");
        if (entry->report.state == DeviceState::quarantined)
            return Status::denied("block device is quarantined");
        if (entry->report.state != DeviceState::ready || entry->report.in_flight)
            return Status::busy("block device already has an owner");
        if ((access != SessionAccess::readonly && access != SessionAccess::writable) || !bounds.count ||
            bounds.first >= entry->geometry.block_count || bounds.count > entry->geometry.block_count - bounds.first)
            return Status::invalid_argument("block session partition or access");
        if (access == SessionAccess::writable && !entry->geometry.writable)
            return Status::denied("block device is readonly");
        if (entry->report.session_serial == ~u64{0})
            return Status::overflow("block session serial exhausted");
        const auto identity = entry->report.identity, generation = entry->report.registration_generation;
        const auto serial = entry->report.session_serial + 1;
        entry->report = {};
        entry->report.state = DeviceState::owned; entry->report.identity = identity;
        entry->report.registration_generation = generation; entry->report.session_serial = serial;
        entry->owner = &session;
        session.service_ = this; session.registration_ = key; session.serial_ = serial; session.first_ = bounds.first;
        session.geometry_ = {bounds.count, entry->geometry.block_size, access == SessionAccess::writable};
        return {};
    }

    [[nodiscard]] Status detach(Registration key)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(key);
        if (!entry) return Status::not_found("stale block registration");
        if (entry->report.state == DeviceState::quarantined)
            return Status::denied("quarantined device lifetime must remain pinned");
        if (entry->owner || entry->report.in_flight || entry->report.state != DeviceState::ready)
            return Status::busy("block registration is owned or in flight");
        const auto generation = entry->report.registration_generation;
        *entry = {}; entry->report.registration_generation = generation;
        return {};
    }

    [[nodiscard]] Result<SessionReport> inspect(Registration key) const
    {
        smp::ScopedSpinLock guard(lock_);
        const auto *entry = find(key);
        return entry ? Result<SessionReport>{entry->report} : Result<SessionReport>{Status::not_found("stale block registration")};
    }

  private:
    enum class Operation : u8 { read, write, flush };
    struct Entry
    {
        driver::BlockDevice *device{};
        Session *owner{};
        driver::BlockGeometry geometry{};
        usize maximum_transfer{};
        SessionReport report{};
    };
    [[nodiscard]] Entry *find(Registration key)
    { return const_cast<Entry *>(static_cast<const BlockSessions *>(this)->find(key)); }
    [[nodiscard]] const Entry *find(Registration key) const
    {
        if (key.owner_ != this || key.slot_ >= Capacity) return nullptr;
        const auto &entry = entries_[key.slot_];
        return entry.device && entry.report.registration_generation == key.generation_ ? &entry : nullptr;
    }
    [[nodiscard]] Entry *find(const Session &session)
    {
        auto *entry = find(session.registration_);
        return entry && entry->report.session_serial == session.serial_ ? entry : nullptr;
    }
    [[nodiscard]] Result<SessionReport> inspect(const Session &session)
    {
        smp::ScopedSpinLock guard(lock_);
        const auto *entry = find(session);
        return entry ? Result<SessionReport>{entry->report} : Result<SessionReport>{Status::bad_descriptor("stale block session")};
    }
    [[nodiscard]] Result<usize> transfer_limit(const Session &session)
    {
        smp::ScopedSpinLock guard(lock_);
        const auto *entry = find(session);
        if (!entry) return Status::bad_descriptor("stale block session");
        if (entry->report.state == DeviceState::quarantined) return Status::denied("block session is quarantined");
        if (entry->owner != &session || entry->report.state != DeviceState::owned)
            return Status::bad_descriptor("closed block session");
        return entry->maximum_transfer;
    }
    [[nodiscard]] Result<SessionBindingObservation> observe_binding(const Session &session)
    {
        smp::ScopedSpinLock guard(lock_);
        const auto *entry = find(session);
        if (!entry || entry->owner != &session || entry->report.state != DeviceState::owned)
            return Status::bad_descriptor("block binding lacks its actual continuing Session owner");
        return SessionBindingObservation{this, &session, entry->device,
            session.registration_.slot_, entry->maximum_transfer,
            entry->report.identity, entry->report.registration_generation, entry->report.session_serial,
            session.first_, session.geometry_.block_count, entry->geometry.block_count,
            session.geometry_.block_size, entry->geometry.block_size,
            session.geometry_.writable, entry->geometry.writable};
    }
    [[nodiscard]] Result<SessionBindingObservation> observe_terminal_binding(const Session &session)
    {
        smp::ScopedSpinLock guard(lock_);
        const auto *entry = find(session);
        if (session.service_ != this || !entry || entry->owner || entry->report.in_flight ||
            entry->report.state != DeviceState::quarantined)
            return Status::bad_descriptor("block terminal binding lacks its original quarantined Session");
        return SessionBindingObservation{this, &session, entry->device,
            session.registration_.slot_, entry->maximum_transfer,
            entry->report.identity, entry->report.registration_generation, entry->report.session_serial,
            session.first_, session.geometry_.block_count, entry->geometry.block_count,
            session.geometry_.block_size, entry->geometry.block_size,
            session.geometry_.writable, entry->geometry.writable};
    }
    static void quarantine(Entry &entry, StatusCode reason)
    {
        if (entry.report.first_error == StatusCode::ok) entry.report.first_error = reason;
        entry.report.state = DeviceState::quarantined; entry.owner = nullptr;
    }
    [[nodiscard]] Status close(Session &session)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(session);
        if (!entry) return Status::bad_descriptor("stale block session");
        if (entry->report.state == DeviceState::quarantined) return Status::denied("block session is quarantined");
        if (entry->owner != &session) return Status::bad_descriptor("closed block session");
        if (entry->report.in_flight || entry->report.pending_writes)
            return Status::busy("block session has in-flight or unflushed work");
        entry->owner = nullptr; entry->report.state = DeviceState::ready;
        return {};
    }
    [[nodiscard]] Status abandon(Session &session)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(session);
        if (!entry || entry->owner != &session) return Status::bad_descriptor("closed or stale block session");
        if (entry->report.in_flight || entry->report.pending_writes)
        {
            quarantine(*entry, StatusCode::interrupted);
            return Status::io_error("block session abandoned unresolved work");
        }
        entry->owner = nullptr; entry->report.state = DeviceState::ready;
        return {};
    }
    [[nodiscard]] Status abort(Session &session, Status reason)
    {
        if (reason) return Status::invalid_argument("block integrity abort requires an error");
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(session);
        if (!entry || entry->owner != &session) return Status::bad_descriptor("closed or stale block session");
        quarantine(*entry, reason.code()); return {};
    }
    [[nodiscard]] static bool same_geometry(driver::BlockGeometry one, driver::BlockGeometry two)
    { return one.block_count == two.block_count && one.block_size == two.block_size && one.writable == two.writable; }

    template <Operation Kind>
    [[nodiscard]] static bool counters_have_room(const SessionReport &report)
    {
        constexpr SessionCounter maximum = session_counter_limit<>;
        if constexpr (Kind == Operation::read)
            return report.reads != maximum && report.completed_reads != maximum;
        else if constexpr (Kind == Operation::write)
            return report.writes != maximum && report.completed_writes != maximum;
        else
            return report.flushes != maximum && report.completed_flushes != maximum;
    }

    struct NoDispatchGuard
    { [[nodiscard]] Status in_flight_exact(Session &) const { return {}; } };
    template <Operation Kind>
    [[nodiscard]] Status io(Session &session, u64 first, std::span<std::byte> out, std::span<const std::byte> in)
    {
        NoDispatchGuard guard;
        return io<Kind>(session, first, out, in, guard);
    }
    template <Operation Kind, typename Guard>
    [[nodiscard]] Status io(Session &session, u64 first, std::span<std::byte> out,
                            std::span<const std::byte> in, Guard &guard)
    {
        Entry *entry{};
        driver::BlockDevice *device{};
        driver::BlockGeometry expected{};
        u64 absolute{};
        {
            smp::ScopedSpinLock guard(lock_);
            entry = find(session);
            if (!entry) return Status::bad_descriptor("stale block session");
            if (entry->report.state == DeviceState::quarantined) return Status::denied("block device is quarantined");
            if (entry->owner != &session || entry->report.state != DeviceState::owned)
                return Status::bad_descriptor("closed block session");
            if (entry->report.in_flight) return Status::busy("block session request already in flight");
            if constexpr (Kind != Operation::read)
                if (!session.geometry_.writable) return Status::denied("readonly block session");
            if constexpr (Kind != Operation::flush)
            {
                const usize size = Kind == Operation::read ? out.size() : in.size();
                const auto block_size = session.geometry_.block_size;
                if (!size || size % block_size || size > entry->maximum_transfer || first >= session.geometry_.block_count ||
                    size / block_size > session.geometry_.block_count - first)
                    return Status::invalid_argument("block session request bounds or transfer size");
                absolute = session.first_ + first;
            }
            if (!counters_have_room<Kind>(entry->report))
                return Status::overflow("block session operation counter exhausted");
            entry->report.in_flight = true;
            if constexpr (Kind == Operation::read) ++entry->report.reads;
            if constexpr (Kind == Operation::write) { ++entry->report.writes; entry->report.pending_writes = true; }
            if constexpr (Kind == Operation::flush) ++entry->report.flushes;
            device = entry->device; expected = entry->geometry;
        }
        Status result = guard.in_flight_exact(session);
        if (!result) static_cast<void>(abort(session, result));
        else if (!same_geometry(device->geometry(), expected))
            result = Status::io_error("block device geometry changed before request");
        else
        {
            // geometry() is another runtime device callback. Re-check the
            // borrowed authority after it and before the selected transport.
            result = guard.in_flight_exact(session);
            if (!result) static_cast<void>(abort(session, result));
            else
            {
                if constexpr (Kind == Operation::read) result = device->read_blocks(absolute, out);
                if constexpr (Kind == Operation::write) result = device->write_blocks(absolute, in);
                if constexpr (Kind == Operation::flush) result = device->flush();
            }
            // A successful transport has no completion credit yet. Prove its
            // borrowed authority before another runtime device callback.
            if (result)
            {
                result = guard.in_flight_exact(session);
                if (!result) static_cast<void>(abort(session, result));
                else
                {
                    if (!same_geometry(device->geometry(), expected))
                        result = Status::io_error("block device geometry changed after request");
                    // Geometry is also a callback. Re-check before granting
                    // completion, while failed transport retains precedence
                    // and performs no further device observation.
                    if (result)
                        if (auto exact = guard.in_flight_exact(session); !exact)
                        { static_cast<void>(abort(session, exact)); result = exact; }
                }
            }
        }
        {
            smp::ScopedSpinLock guard(lock_);
            // The request pin prevents detach/reuse even after reentrant abort.
            entry->report.in_flight = false;
            if (!result) { quarantine(*entry, result.code()); return result; }
            if (entry->report.state == DeviceState::quarantined)
                return Status::io_error("block session was aborted in flight");
            if constexpr (Kind == Operation::read) ++entry->report.completed_reads;
            if constexpr (Kind == Operation::write) ++entry->report.completed_writes;
            if constexpr (Kind == Operation::flush)
            { ++entry->report.completed_flushes; entry->report.pending_writes = false; }
        }
        return {};
    }
    std::array<Entry, Capacity> entries_{};
    mutable smp::SpinLock lock_{};
};
} // namespace ok::block
