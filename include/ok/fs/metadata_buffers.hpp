#pragma once

#include "ok/block/sessions.hpp"

#include <concepts>

namespace ok::fs
{
enum class MetadataOwnerState : u8 { unused, opening, ready, closing, closed, failed };
enum class MetadataBufferState : u8 { unused, loading, ready, failed, exhausted };
struct MetadataOwnerReport
{
    MetadataOwnerState state{MetadataOwnerState::unused};
    StatusCode first_error{StatusCode::ok};
    u64 device_identity{}, registration_generation{}, session_serial{}, first_sector{}, sectors{}, blocks{};
    usize read_pins{}, resident_blocks{};
    bool in_flight{}, transaction_active{};
};
struct MetadataBufferReport
{
    MetadataBufferState state{MetadataBufferState::unused};
    u64 block{}, generation{}, transaction_serial{};
    usize read_pins{};
    bool staged_dirty{};
};

template <class Guard, class Session>
concept MetadataReadDispatchGuard = requires(Guard &guard, Session &session, u64 sector, usize bytes, Status status)
{
    { guard.begin_read(session, sector, bytes) } -> std::same_as<Status>;
    { guard.finish_read(session, status) } -> std::same_as<Status>;
};

// Caller-provisioned ownership/private-version slice. No home writeback or commit.
// Owner, registry and transport storage outlive all handles and admitted calls.
// Quarantine retains backing until external fencing or transport lifetime end.
template <usize Capacity = 8, usize BlockBytes = 1024, usize DeviceCapacity = 4>
    requires (Capacity > 0 && (BlockBytes == 1024 || BlockBytes == 2048 || BlockBytes == 4096))
class MetadataBuffers final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4EmptyOrphanRecoveryOwner;
  public:
    using Registry = block::BlockSessions<DeviceCapacity>;
    class Pin final
    {
        friend class MetadataBuffers;
        template <usize, usize> friend class Ext4OwnedTransaction;
      public:
        Pin() = default;
        ~Pin() { if (owner_) static_cast<void>(owner_->release(*this)); }
        Pin(const Pin &) = delete;
        Pin &operator=(const Pin &) = delete;
        Pin(Pin &&) = delete;
        Pin &operator=(Pin &&) = delete;
        [[nodiscard]] Status read(usize offset, std::span<std::byte> out) const
        { return owner_ ? owner_->read(*this, offset, out) : Status::not_initialized("metadata pin is unbound"); }
        [[nodiscard]] Status release()
        { return owner_ ? owner_->release(*this) : Status::not_initialized("metadata pin is unbound"); }
        [[nodiscard]] Result<MetadataBufferReport> report() const
        { return owner_ ? owner_->inspect(*this) : Result<MetadataBufferReport>{Status::not_initialized("metadata pin is unbound")}; }
      private:
        MetadataBuffers *owner_{};
        usize slot_{};
        u64 generation_{};
        bool active_{};
    };
    class Transaction final
    {
        friend class MetadataBuffers;
        template <usize, usize> friend class Ext4OwnedTransaction;
      public:
        Transaction() = default;
        ~Transaction() { if (owner_) static_cast<void>(owner_->discard(*this)); }
        Transaction(const Transaction &) = delete;
        Transaction &operator=(const Transaction &) = delete;
        Transaction(Transaction &&) = delete;
        Transaction &operator=(Transaction &&) = delete;
        [[nodiscard]] Status edit(u64 block, usize offset, std::span<const std::byte> bytes)
        { return owner_ ? owner_->edit(*this, block, offset, bytes) : Status::not_initialized("metadata transaction is unbound"); }
        [[nodiscard]] Status read(u64 block, usize offset, std::span<std::byte> out) const
        { return owner_ ? owner_->read(*this, block, offset, out) : Status::not_initialized("metadata transaction is unbound"); }
        [[nodiscard]] Status discard()
        { return owner_ ? owner_->discard(*this) : Status::not_initialized("metadata transaction is unbound"); }
      private:
        MetadataBuffers *owner_{};
        u64 serial_{};
        bool active_{};
    };

    MetadataBuffers() = default;
    ~MetadataBuffers()
    {
        // Explicit close establishes drainage. Destruction is never a reset.
        if (report_.state == MetadataOwnerState::ready)
            static_cast<void>(abort(Status::interrupted("metadata owner destroyed without close")));
    }
    MetadataBuffers(const MetadataBuffers &) = delete;
    MetadataBuffers &operator=(const MetadataBuffers &) = delete;
    MetadataBuffers(MetadataBuffers &&) = delete;
    MetadataBuffers &operator=(MetadataBuffers &&) = delete;

    [[nodiscard]] Status open(Registry &registry, typename Registry::Registration key,
                              block::SessionBounds bounds, block::SessionAccess access)
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.state != MetadataOwnerState::unused) return Status::already_exists("metadata owner is one-shot");
            report_.state = MetadataOwnerState::opening;
        }
        auto status = registry.acquire(key, bounds, access, session_);
        if (!status)
        {
            smp::ScopedSpinLock guard(lock_);
            report_.state = MetadataOwnerState::closed; report_.first_error = status.code(); return status;
        }
        const auto geometry = session_.geometry();
        const auto identity = session_.report();
        const auto transfer_limit = session_.transfer_limit();
        const auto binding = session_.observe_binding();
        if (!identity) status = identity.status();
        else if (!transfer_limit) status = transfer_limit.status();
        else if (!binding) status = binding.status();
        else if (geometry.block_size > BlockBytes || !geometry.block_size ||
                 BlockBytes % geometry.block_size || bounds.count % (BlockBytes / geometry.block_size))
            status = Status::invalid_argument("metadata block and partition geometry");
        else if (transfer_limit.value() < BlockBytes)
            status = Status::unsupported("metadata block exceeds session transfer limit");
        if (!status)
        {
            const auto closed = session_.close();
            smp::ScopedSpinLock guard(lock_);
            report_.first_error = status.code();
            if (closed) report_.state = MetadataOwnerState::closed;
            else fail(closed.code());
            return status;
        }
        smp::ScopedSpinLock guard(lock_);
        report_.device_identity = identity.value().identity;
        report_.registration_generation = identity.value().registration_generation;
        report_.session_serial = identity.value().session_serial;
        report_.first_sector = bounds.first; report_.sectors = bounds.count;
        sectors_per_block_ = BlockBytes / geometry.block_size;
        report_.blocks = bounds.count / sectors_per_block_;
        writable_ = access == block::SessionAccess::writable;
        session_binding_ = binding.value();
        report_.state = MetadataOwnerState::ready;
        return {};
    }

    [[nodiscard]] Status pin(u64 block, Pin &pin)
    {
        UnguardedRead guard;
        return pin_impl(block, pin, guard);
    }

    // Hooks synchronously account for a miss; they must not perform transport
    // I/O, block or reenter this cache. Hits consume no transport allowance.
    template <class Guard> requires MetadataReadDispatchGuard<Guard, typename Registry::Session>
    [[nodiscard]] Status pin(u64 block, Pin &pin, Guard &guard)
    { return pin_impl(block, pin, guard); }

  private:
    struct UnguardedRead
    {
        Status begin_read(typename Registry::Session &, u64, usize) { return {}; }
        Status finish_read(typename Registry::Session &, Status) { return {}; }
    };

    template <class Guard>
    [[nodiscard]] Status pin_impl(u64 block, Pin &pin, Guard &read_guard)
    {
        Entry *entry{};
        constexpr bool deferred = [] {
            if constexpr (requires { Guard::defer_cache_begin; }) return Guard::defer_cache_begin;
            else return false;
        }();
        usize reserved_slot = Capacity;
        MetadataBufferReport reserved{};
        {
            smp::ScopedSpinLock guard(lock_);
            if (pin.owner_) return Status::already_exists("metadata pin is one-shot");
            if (auto status = admission(); !status) return status;
            if (block >= report_.blocks) return Status::invalid_argument("metadata block outside partition");
            if (report_.read_pins == ~usize{0}) return Status::overflow("metadata pin count exhausted");
            for (usize index = 0; index < Capacity; ++index)
            {
                auto &candidate = entries_[index];
                if ((candidate.report.state == MetadataBufferState::ready || candidate.report.state == MetadataBufferState::loading) &&
                    candidate.report.block == block)
                {
                    if (candidate.report.state != MetadataBufferState::ready) return Status::busy("metadata block is loading");
                    bind_pin(pin, candidate, index);
                    return {};
                }
            }
            if (report_.in_flight) return Status::busy("metadata read already in flight");
            usize selected = Capacity;
            for (usize index = 0; index < Capacity; ++index)
            {
                auto &candidate = entries_[index];
                if ((candidate.report.state != MetadataBufferState::unused && candidate.report.state != MetadataBufferState::ready) ||
                    candidate.report.read_pins || candidate.report.transaction_serial) continue;
                if (candidate.report.generation == ~u64{0}) continue;
                selected = index; break;
            }
            // Do not evict, advance generations or bind a pin for a refused
            // reservation. Exhaustion bookkeeping is also deferred until here.
            if constexpr (!deferred)
            if (selected != Capacity)
                if (auto status = read_guard.begin_read(session_, block * sectors_per_block_, BlockBytes); !status)
                {
                    // An accounting refusal can leave the Session owned. An
                    // integrity rejection can quarantine it; then cached hits
                    // and already-retained pins must stop exposing data.
                    const auto session = session_.report();
                    if (!session || session.value().state != block::DeviceState::owned)
                        fail(status.code());
                    return status;
                }
            if constexpr (deferred)
            {
                if (selected == Capacity) return Status::no_memory("no clean unpinned metadata buffer");
                // Reserve only the serialized miss; no slot, generation or Pin
                // is changed before the borrowed Owner guard can refuse.
                reserved_slot = selected; reserved = entries_[selected].report;
                report_.in_flight = true; entry = &entries_[selected];
            }
            else
            {
            for (usize index = 0; index < selected; ++index)
            {
                auto &candidate = entries_[index];
                if ((candidate.report.state == MetadataBufferState::unused || candidate.report.state == MetadataBufferState::ready) &&
                    !candidate.report.read_pins && !candidate.report.transaction_serial && candidate.report.generation == ~u64{0})
                {
                    if (candidate.report.state == MetadataBufferState::ready) --report_.resident_blocks;
                    candidate.report.state = MetadataBufferState::exhausted;
                }
            }
            if (selected != Capacity)
            {
                auto &candidate = entries_[selected];
                const auto generation = candidate.report.generation + 1;
                if (candidate.report.state == MetadataBufferState::unused) ++report_.resident_blocks;
                candidate.report = {}; candidate.report.block = block; candidate.report.generation = generation;
                candidate.report.state = MetadataBufferState::loading;
                candidate.report.read_pins = 1; ++report_.read_pins;
                pin.owner_ = this; pin.slot_ = selected; pin.generation_ = generation;
                report_.in_flight = true; entry = &candidate;
            }
            if (!entry) return Status::no_memory("no clean unpinned metadata buffer");
            }
        }
        if constexpr (deferred)
        {
            const auto admitted = [&]() -> Status {
                if constexpr (requires { read_guard.begin_cache_read(session_, block * sectors_per_block_,
                    BlockBytes, reserved_slot, reserved.generation + 1); })
                    return read_guard.begin_cache_read(session_, block * sectors_per_block_, BlockBytes,
                        reserved_slot, reserved.generation + 1);
                else return read_guard.begin_read(session_, block * sectors_per_block_, BlockBytes);
            }();
            smp::ScopedSpinLock guard(lock_);
            if (!admitted)
            {
                report_.in_flight = false;
                const auto session = session_.report();
                if (!session || session.value().state != block::DeviceState::owned) fail(admitted.code());
                return admitted;
            }
            const auto &actual = entry->report;
            if (!report_.in_flight || report_.state != MetadataOwnerState::ready || reserved_slot >= Capacity ||
                actual.state != reserved.state || actual.block != reserved.block || actual.generation != reserved.generation ||
                actual.read_pins != reserved.read_pins || actual.transaction_serial != reserved.transaction_serial ||
                actual.staged_dirty != reserved.staged_dirty)
            {
                const auto status = Status::fault("metadata deferred miss reservation changed");
                static_cast<void>(session_.abort(status)); report_.in_flight = false; fail(status.code()); return status;
            }
            const u64 generation = actual.generation + 1;
            if (actual.state == MetadataBufferState::unused) ++report_.resident_blocks;
            entry->report = {}; entry->report.block = block; entry->report.generation = generation;
            entry->report.state = MetadataBufferState::loading; entry->report.read_pins = 1; ++report_.read_pins;
            pin.owner_ = this; pin.slot_ = reserved_slot; pin.generation_ = generation;
        }
        // Entry backing is retained across the unlocked synchronous request.
        auto status = [&]() -> Status {
            if constexpr (requires { session_.read_blocks_guarded(block * sectors_per_block_, entry->canonical, read_guard); })
                return session_.read_blocks_guarded(block * sectors_per_block_, entry->canonical, read_guard);
            else return session_.read_blocks(block * sectors_per_block_, entry->canonical);
        }();
        const auto accounted = read_guard.finish_read(session_, status);
        if (status && !accounted) status = accounted;
        if (!status) static_cast<void>(session_.abort(status));
        smp::ScopedSpinLock guard(lock_);
        report_.in_flight = false;
        if (!status || report_.state == MetadataOwnerState::failed)
        {
            if (!status) fail(status.code());
            --entry->report.read_pins; --report_.read_pins;
            entry->report.state = MetadataBufferState::failed;
            return status ? Status::io_error("metadata owner aborted in flight") : status;
        }
        entry->report.state = MetadataBufferState::ready;
        pin.active_ = true;
        return {};
    }

  public:
    [[nodiscard]] Status begin(std::span<Pin *const> pins, Transaction &transaction)
    {
        smp::ScopedSpinLock guard(lock_);
        if (transaction.owner_) return Status::already_exists("metadata transaction is one-shot");
        if (auto status = admission(); !status) return status;
        if (!writable_) return Status::denied("readonly metadata owner");
        if (report_.in_flight || transaction_) return Status::busy("metadata owner has active work");
        if (pins.empty() || pins.size() > Capacity) return Status::invalid_argument("metadata transaction credit count");
        if (transaction_serial_ == ~u64{0}) return Status::overflow("metadata transaction generation exhausted");
        for (usize index = 0; index < pins.size(); ++index)
        {
            if (!pins[index] || !find(*pins[index])) return Status::bad_descriptor("foreign or released metadata participant");
            for (usize prior = 0; prior < index; ++prior)
                if (pins[prior]->slot_ == pins[index]->slot_)
                    return Status::invalid_argument("duplicate canonical metadata participant");
        }
        // Every participant and all caller-provisioned credits exist before edits.
        transaction.owner_ = this; transaction.serial_ = ++transaction_serial_; transaction.active_ = true;
        transaction_ = &transaction; report_.transaction_active = true;
        for (auto *pin : pins)
        {
            auto &entry = entries_[pin->slot_];
            entry.staged = entry.canonical;
            entry.report.transaction_serial = transaction.serial_; entry.report.staged_dirty = false;
        }
        return {};
    }

    [[nodiscard]] Status close()
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = admission(); !status) return status;
            if (report_.read_pins || report_.in_flight || transaction_)
                return Status::busy("metadata owner has retained work");
            report_.state = MetadataOwnerState::closing;
        }
        const auto status = session_.close();
        smp::ScopedSpinLock guard(lock_);
        if (!status) { fail(status.code()); return status; }
        report_.state = MetadataOwnerState::closed; return {};
    }
    [[nodiscard]] Status abort(Status reason)
    {
        if (reason) return Status::invalid_argument("metadata integrity abort requires error");
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = admission(); !status) return status;
            fail(reason.code());
        }
        static_cast<void>(session_.abort(reason));
        return {};
    }
    [[nodiscard]] MetadataOwnerReport report() const
    {
        smp::ScopedSpinLock guard(lock_);
        auto result = report_;
        if (result.state == MetadataOwnerState::ready)
        {
            const auto session = session_.report();
            if (!session || session.value().state != block::DeviceState::owned ||
                session.value().identity != result.device_identity ||
                session.value().registration_generation != result.registration_generation ||
                session.value().session_serial != result.session_serial)
            {
                result.state = MetadataOwnerState::failed;
                result.first_error = session && session.value().first_error != StatusCode::ok ?
                    session.value().first_error : StatusCode::fault;
            }
        }
        return result;
    }

  private:
    struct Entry
    {
        MetadataBufferReport report{};
        alignas(64) std::array<std::byte, BlockBytes> canonical{}, staged{};
    };
    enum class TypedCacheAction : u8 { unchanged, replace, retire };
    struct TypedCacheChange
    {
        u64 block{};
        TypedCacheAction action{TypedCacheAction::unchanged};
        std::span<const std::byte> expected{}, replacement{};
    };
    // Construction is confined to the canonical owner. This is retained private
    // state, not an exclusion lease or a public report carrying write authority.
    class TypedReconciliation final
    {
        friend class MetadataBuffers;
        template <usize, usize> friend class Ext4OwnedTransaction;
        TypedReconciliation() = default;
        ~TypedReconciliation() = default;
        TypedReconciliation(const TypedReconciliation &) = delete;
        TypedReconciliation &operator=(const TypedReconciliation &) = delete;
        TypedReconciliation(TypedReconciliation &&) = delete;
        TypedReconciliation &operator=(TypedReconciliation &&) = delete;
        enum class Phase : u8 { unused, prepared, normalized, published, failed };
        const TypedReconciliation *original_{};
        MetadataBuffers *cache_{};
        const typename Registry::Session *session_{};
        Phase phase_{Phase::unused};
        MetadataOwnerReport report_{};
        std::array<Entry, Capacity> entries_{};
        std::array<TypedCacheAction, Capacity> actions_{};
        std::array<MetadataBufferReport, Capacity> final_reports_{};
        std::array<std::array<std::byte, BlockBytes>, Capacity> final_images_{};
        std::array<u64, 32> targets_{};
        std::array<TypedCacheAction, 32> target_actions_{};
        usize targets_count_{}, resident_after_{}, sectors_per_block_{};
        u64 transaction_serial_{}, seal_{};
        bool writable_{};
        block::SessionBindingObservation session_binding_{};
    };
    [[nodiscard]] static bool same_report(const MetadataBufferReport &a, const MetadataBufferReport &b)
    {
        return a.state == b.state && a.block == b.block && a.generation == b.generation &&
            a.transaction_serial == b.transaction_serial && a.read_pins == b.read_pins &&
            a.staged_dirty == b.staged_dirty;
    }
    [[nodiscard]] static bool same_report(const MetadataOwnerReport &a, const MetadataOwnerReport &b)
    {
        return a.state == b.state && a.first_error == b.first_error &&
            a.device_identity == b.device_identity && a.registration_generation == b.registration_generation &&
            a.session_serial == b.session_serial && a.first_sector == b.first_sector && a.sectors == b.sectors &&
            a.blocks == b.blocks && a.read_pins == b.read_pins && a.resident_blocks == b.resident_blocks &&
            a.in_flight == b.in_flight && a.transaction_active == b.transaction_active;
    }
    [[nodiscard]] Status typed_cache_idle() const
    {
        if (report_.state != MetadataOwnerState::ready || report_.first_error != StatusCode::ok || !writable_)
            return Status::denied("typed cache reconciliation requires a healthy writable owner");
        if (report_.in_flight || report_.read_pins || transaction_ || report_.transaction_active)
            return Status::busy("typed cache reconciliation requires drained cache work");
        const auto binding = session_.observe_binding();
        if (!binding || binding.value() != session_binding_)
            return Status::fault("typed cache authoritative original Session binding changed");
        const auto &original = binding.value();
        if (report_.first_sector != original.first_sector || report_.sectors != original.sectors ||
            !original.sector_bytes || original.sector_bytes > BlockBytes ||
            BlockBytes % original.sector_bytes || sectors_per_block_ != BlockBytes / original.sector_bytes ||
            writable_ != original.writable)
            return Status::fault("typed cache prefix differs from its original Session binding");
        const auto current = session_.report();
        if (!current || current.value().state != block::DeviceState::owned ||
            current.value().first_error != StatusCode::ok || current.value().in_flight || current.value().pending_writes ||
            current.value().identity != report_.device_identity ||
            current.value().registration_generation != report_.registration_generation ||
            current.value().session_serial != report_.session_serial)
            return Status::fault("typed cache original Session identity or idle state changed");
        if (!sectors_per_block_ || !report_.sectors || report_.sectors % sectors_per_block_ ||
            report_.blocks != report_.sectors / sectors_per_block_)
            return Status::fault("typed cache geometry or block count changed");
        usize residents = 0;
        for (usize index = 0; index < Capacity; ++index)
        {
            const auto &entry = entries_[index].report;
            if (entry.read_pins || entry.transaction_serial || entry.staged_dirty)
                return Status::busy("typed cache slot retains pins or staged ownership");
            if (entry.state == MetadataBufferState::ready)
            {
                if (!entry.generation || entry.block >= report_.blocks)
                    return Status::fault("typed cache resident identity is invalid");
                for (usize prior = 0; prior < index; ++prior)
                    if (entries_[prior].report.state == MetadataBufferState::ready &&
                        entries_[prior].report.block == entry.block)
                        return Status::fault("typed cache has duplicate resident keys");
                ++residents;
            }
            else if (entry.state != MetadataBufferState::unused && entry.state != MetadataBufferState::exhausted)
                return Status::fault("typed cache has an undrained slot state");
            else if (entry.state == MetadataBufferState::exhausted && entry.generation != ~u64{0})
                return Status::fault("typed cache exhausted generation is invalid");
        }
        if (residents != report_.resident_blocks)
            return Status::fault("typed cache resident counter changed");
        return {};
    }
    [[nodiscard]] static bool overlaps_storage(const void *data, usize bytes, const void *storage, usize extent)
    {
        if (!bytes) return false;
        const uptr first = reinterpret_cast<uptr>(data), other = reinterpret_cast<uptr>(storage);
        if (!data || first > ~uptr{0} - bytes || other > ~uptr{0} - extent) return true;
        return first < other + extent && other < first + bytes;
    }
    [[nodiscard]] static u64 typed_plan_seal(const TypedReconciliation &plan)
    {
        u64 hash = 14695981039346656037ULL;
        const auto mix = [&](u64 value) {
            for (usize n = 0; n < sizeof(value); ++n)
            { hash ^= (value >> (8 * n)) & 0xff; hash *= 1099511628211ULL; }
        };
        const auto buffer = [&](const MetadataBufferReport &r) {
            mix(static_cast<u8>(r.state)); mix(r.block); mix(r.generation); mix(r.transaction_serial);
            mix(r.read_pins); mix(r.staged_dirty);
        };
        mix(reinterpret_cast<uptr>(plan.original_)); mix(reinterpret_cast<uptr>(plan.cache_));
        mix(reinterpret_cast<uptr>(plan.session_)); mix(static_cast<u8>(plan.phase_));
        const auto &r = plan.report_;
        mix(static_cast<u8>(r.state)); mix(static_cast<u32>(r.first_error)); mix(r.device_identity);
        mix(r.registration_generation); mix(r.session_serial); mix(r.first_sector); mix(r.sectors); mix(r.blocks);
        mix(r.read_pins); mix(r.resident_blocks); mix(r.in_flight); mix(r.transaction_active);
        mix(plan.targets_count_); mix(plan.resident_after_); mix(plan.sectors_per_block_);
        mix(plan.transaction_serial_); mix(plan.writable_);
        const auto &binding = plan.session_binding_;
        mix(reinterpret_cast<uptr>(binding.service)); mix(reinterpret_cast<uptr>(binding.session));
        mix(reinterpret_cast<uptr>(binding.device)); mix(binding.registration_slot); mix(binding.maximum_transfer);
        mix(binding.identity); mix(binding.registration_generation); mix(binding.session_serial);
        mix(binding.first_sector); mix(binding.sectors); mix(binding.device_sectors);
        mix(binding.sector_bytes); mix(binding.device_sector_bytes); mix(binding.writable); mix(binding.device_writable);
        for (usize index = 0; index < Capacity; ++index)
        {
            const auto &entry = plan.entries_[index];
            buffer(entry.report); mix(static_cast<u8>(plan.actions_[index])); buffer(plan.final_reports_[index]);
            for (usize byte = 0; byte < BlockBytes; ++byte)
            {
                mix(std::to_integer<u8>(entry.canonical[byte])); mix(std::to_integer<u8>(entry.staged[byte]));
                mix(std::to_integer<u8>(plan.final_images_[index][byte]));
            }
        }
        for (usize index = 0; index < plan.targets_.size(); ++index)
        { mix(plan.targets_[index]); mix(static_cast<u8>(plan.target_actions_[index])); }
        return hash;
    }
    [[nodiscard]] Status prepare_typed_reconciliation(std::span<const TypedCacheChange> changes,
                                                       TypedReconciliation &plan)
    {
        smp::ScopedSpinLock guard(lock_);
        if (plan.phase_ != TypedReconciliation::Phase::unused || plan.original_ || plan.cache_ || plan.session_)
            return Status::already_exists("typed cache plan is one-shot");
        if (auto status = typed_cache_idle(); !status) return status;
        if (changes.size() > plan.targets_.size())
            return Status::unsupported("typed cache reconciliation exceeds bounded target profile");
        if (overlaps_storage(&plan, sizeof(plan), this, sizeof(*this)) ||
            overlaps_storage(changes.data(), changes.size_bytes(), this, sizeof(*this)) ||
            overlaps_storage(changes.data(), changes.size_bytes(), &plan, sizeof(plan)))
            return Status::invalid_argument("typed cache request aliases retained storage");
        usize retirements = 0;
        for (usize target = 0; target < changes.size(); ++target)
        {
            const auto &change = changes[target];
            if (change.block >= report_.blocks ||
                (change.action != TypedCacheAction::replace && change.action != TypedCacheAction::retire) ||
                change.expected.size() != BlockBytes || !change.expected.data() ||
                (change.action == TypedCacheAction::replace &&
                    (change.replacement.size() != BlockBytes || !change.replacement.data())) ||
                (change.action == TypedCacheAction::retire && !change.replacement.empty()))
                return Status::invalid_argument("typed cache target image or bounds are invalid");
            for (usize prior = 0; prior < target; ++prior)
                if (changes[prior].block == change.block)
                    return Status::invalid_argument("typed cache has duplicate affected targets");
            for (const auto bytes : {change.expected, change.replacement})
                if (overlaps_storage(bytes.data(), bytes.size(), this, sizeof(*this)) ||
                    overlaps_storage(bytes.data(), bytes.size(), &plan, sizeof(plan)))
                    return Status::invalid_argument("typed cache image aliases retained storage");
            for (const auto &entry : entries_)
            {
                if (entry.report.state != MetadataBufferState::ready || entry.report.block != change.block) continue;
                for (usize byte = 0; byte < BlockBytes; ++byte)
                    if (entry.canonical[byte] != change.expected[byte])
                        return Status::fault("typed cache affected canonical image is stale");
                if (change.action == TypedCacheAction::replace && entry.report.generation == ~u64{0})
                    return Status::overflow("typed cache replacement generation exhausted");
                if (change.action == TypedCacheAction::retire) ++retirements;
            }
        }
        // Every possible refusal precedes writes to either cache or private plan.
        plan.original_ = &plan; plan.cache_ = this; plan.session_ = &session_;
        plan.phase_ = TypedReconciliation::Phase::prepared; plan.report_ = report_; plan.entries_ = entries_;
        plan.actions_ = {}; plan.final_reports_ = {}; plan.final_images_ = {};
        plan.targets_ = {}; plan.target_actions_ = {};
        plan.sectors_per_block_ = sectors_per_block_; plan.transaction_serial_ = transaction_serial_;
        plan.writable_ = writable_; plan.targets_count_ = changes.size();
        plan.session_binding_ = session_binding_;
        plan.resident_after_ = report_.resident_blocks - retirements;
        for (usize target = 0; target < changes.size(); ++target)
        { plan.targets_[target] = changes[target].block; plan.target_actions_[target] = changes[target].action; }
        for (usize slot = 0; slot < Capacity; ++slot)
        {
            const auto &entry = entries_[slot];
            plan.final_reports_[slot] = entry.report;
            if (entry.report.state != MetadataBufferState::ready) continue;
            for (const auto &change : changes)
            {
                if (entry.report.block != change.block) continue;
                plan.actions_[slot] = change.action;
                if (change.action == TypedCacheAction::replace)
                {
                    ++plan.final_reports_[slot].generation;
                    for (usize byte = 0; byte < BlockBytes; ++byte)
                        plan.final_images_[slot][byte] = change.replacement[byte];
                }
                else
                {
                    const u64 generation = entry.report.generation;
                    plan.final_reports_[slot] = {};
                    plan.final_reports_[slot].generation = generation == ~u64{0} ? generation : generation + 1;
                    plan.final_reports_[slot].state = generation == ~u64{0} ? MetadataBufferState::exhausted :
                                                                                          MetadataBufferState::unused;
                }
            }
        }
        plan.seal_ = typed_plan_seal(plan);
        return {};
    }
    [[nodiscard]] Status validate_typed_reconciliation(const TypedReconciliation &plan,
                                                       typename TypedReconciliation::Phase phase) const
    {
        if (plan.original_ != &plan || plan.cache_ != this || plan.session_ != &session_ || plan.phase_ != phase)
            return Status::bad_descriptor("typed cache plan lost its original binding or phase");
        if (plan.seal_ != typed_plan_seal(plan))
            return Status::fault("typed cache retained plan seal changed");
        if (auto status = typed_cache_idle(); !status) return status;
        if (!same_report(plan.report_, report_) || plan.sectors_per_block_ != sectors_per_block_ ||
            plan.transaction_serial_ != transaction_serial_ || plan.writable_ != writable_ ||
            plan.session_binding_ != session_binding_)
            return Status::fault("typed cache complete owner snapshot changed");
        for (usize index = 0; index < Capacity; ++index)
            if (!same_report(plan.entries_[index].report, entries_[index].report) ||
                plan.entries_[index].canonical != entries_[index].canonical ||
                plan.entries_[index].staged != entries_[index].staged)
                return Status::fault("typed cache complete resident snapshot changed");
        return {};
    }
    [[nodiscard]] Status refuse_typed_reconciliation(TypedReconciliation &plan, Status reason)
    {
        if (plan.original_ == &plan && plan.cache_ == this && plan.session_ == &session_ &&
            (plan.phase_ == TypedReconciliation::Phase::prepared || plan.phase_ == TypedReconciliation::Phase::normalized))
            plan.phase_ = TypedReconciliation::Phase::failed;
        return reason;
    }
    [[nodiscard]] Status normalize_typed_reconciliation(TypedReconciliation &plan)
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = validate_typed_reconciliation(plan, TypedReconciliation::Phase::prepared); !status)
            return refuse_typed_reconciliation(plan, status);
        for (usize index = 0; index < Capacity; ++index)
            if (plan.actions_[index] != TypedCacheAction::unchanged)
            {
                entries_[index].staged = entries_[index].canonical;
                plan.entries_[index].staged = entries_[index].staged;
            }
        plan.phase_ = TypedReconciliation::Phase::normalized; plan.seal_ = typed_plan_seal(plan);
        return {};
    }
    [[nodiscard]] Status publish_typed_reconciliation(TypedReconciliation &plan)
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = validate_typed_reconciliation(plan, TypedReconciliation::Phase::normalized); !status)
            return refuse_typed_reconciliation(plan, status);
        for (usize index = 0; index < Capacity; ++index)
        {
            if (plan.actions_[index] == TypedCacheAction::unchanged) continue;
            entries_[index].report = plan.final_reports_[index];
            if (plan.actions_[index] == TypedCacheAction::replace)
                entries_[index].canonical = entries_[index].staged = plan.final_images_[index];
            else entries_[index].canonical = entries_[index].staged = {};
        }
        report_.resident_blocks = plan.resident_after_;
        plan.phase_ = TypedReconciliation::Phase::published;
        return {};
    }
    [[nodiscard]] Status admission()
    {
        if (report_.state == MetadataOwnerState::failed) return Status::denied("metadata owner is quarantined");
        if (report_.state != MetadataOwnerState::ready) return Status::not_initialized("metadata owner is not open");
        const auto session = session_.report();
        if (!session || session.value().state != block::DeviceState::owned ||
            session.value().identity != report_.device_identity ||
            session.value().registration_generation != report_.registration_generation ||
            session.value().session_serial != report_.session_serial)
        {
            fail(session && session.value().first_error != StatusCode::ok ? session.value().first_error :
                 StatusCode::fault);
            return Status::denied("metadata owner Session lost authority");
        }
        return {};
    }
    // The owning transaction supplies a checked, bounded physical mapping after
    // all readers drain. Inspect the entire set before changing any slot so a
    // busy or malformed mapping cannot retire only a prefix. No transport I/O
    // or session transition occurs under this lock.
    [[nodiscard]] Result<usize> retire_clean_blocks(std::span<const u64> blocks)
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = admission(); !status) return status;
        if (!writable_) return Status::denied("readonly metadata owner cannot retire released blocks");
        if (report_.in_flight || report_.read_pins || transaction_ || report_.transaction_active)
            return Status::busy("metadata owner has retained work during retirement");
        if (blocks.size() > 16)
            return Status::unsupported("released metadata mapping exceeds bounded profile");
        // Keep a private copy: retirement clears slot storage, and even an
        // accidentally aliased caller span must not change the checked set.
        std::array<u64, 16> targets{};
        const usize target_count = blocks.size();
        for (usize index = 0; index < target_count; ++index) targets[index] = blocks[index];
        const auto released = std::span<const u64>{targets}.first(target_count);
        for (usize index = 0; index < target_count; ++index)
        {
            if (targets[index] >= report_.blocks)
                return Status::invalid_argument("released metadata block outside partition");
            for (usize prior = 0; prior < index; ++prior)
                if (targets[index] == targets[prior])
                    return Status::invalid_argument("duplicate released metadata block");
        }
        usize count = 0;
        for (usize index = 0; index < Capacity; ++index)
        {
            const auto &entry = entries_[index];
            if (entry.report.state != MetadataBufferState::ready) continue;
            bool requested = false;
            for (u64 block : released)
                if (entry.report.block == block) { requested = true; break; }
            if (!requested) continue;
            if (entry.report.read_pins || entry.report.transaction_serial || entry.report.staged_dirty)
                return Status::busy("released metadata block is retained or staged");
            if (!entry.report.generation)
                return Status::fault("released metadata slot has no generation");
            for (usize prior = 0; prior < index; ++prior)
                if (entries_[prior].report.state == MetadataBufferState::ready &&
                    entries_[prior].report.block == entry.report.block)
                    return Status::fault("released metadata block has duplicate resident identity");
            ++count;
        }
        if (count > report_.resident_blocks)
            return Status::fault("released metadata resident count underflow");
        for (auto &entry : entries_)
        {
            if (entry.report.state != MetadataBufferState::ready) continue;
            bool requested = false;
            for (u64 block : released)
                if (entry.report.block == block) { requested = true; break; }
            if (!requested) continue;
            const u64 generation = entry.report.generation;
            entry.canonical = {}; entry.staged = {};
            entry.report = {};
            entry.report.generation = generation == ~u64{0} ? generation : generation + 1;
            entry.report.state = generation == ~u64{0} ? MetadataBufferState::exhausted :
                                                        MetadataBufferState::unused;
        }
        report_.resident_blocks -= count;
        return count;
    }
    void fail(StatusCode reason)
    {
        if (report_.first_error == StatusCode::ok) report_.first_error = reason;
        report_.state = MetadataOwnerState::failed;
    }
    void bind_pin(Pin &pin, Entry &entry, usize slot)
    {
        pin.owner_ = this; pin.slot_ = slot; pin.generation_ = entry.report.generation; pin.active_ = true;
        ++entry.report.read_pins; ++report_.read_pins;
    }
    [[nodiscard]] Entry *find(const Pin &pin)
    {
        if (pin.owner_ != this || !pin.active_ || pin.slot_ >= Capacity) return nullptr;
        auto &entry = entries_[pin.slot_];
        return entry.report.generation == pin.generation_ && entry.report.read_pins ? &entry : nullptr;
    }
    [[nodiscard]] Entry *find(const Transaction &transaction, u64 block)
    {
        if (transaction.owner_ != this || !transaction.active_ || transaction_ != &transaction ||
            transaction.serial_ != transaction_serial_) return nullptr;
        for (auto &entry : entries_)
            if (entry.report.transaction_serial == transaction.serial_ && entry.report.block == block) return &entry;
        return nullptr;
    }
    [[nodiscard]] static bool valid_range(usize offset, usize size)
    { return size && offset < BlockBytes && size <= BlockBytes - offset; }
    [[nodiscard]] Status read(const Pin &pin, usize offset, std::span<std::byte> out)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(pin);
        if (!entry) return Status::bad_descriptor("released or stale metadata pin");
        if (auto status = admission(); !status) return status;
        if (!valid_range(offset, out.size())) return Status::invalid_argument("metadata byte range");
        for (usize index = 0; index < out.size(); ++index) out[index] = entry->canonical[offset + index];
        return {};
    }
    [[nodiscard]] Status read(const Transaction &transaction, u64 block, usize offset, std::span<std::byte> out)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(transaction, block);
        if (!entry) return Status::bad_descriptor("metadata transaction does not own block");
        if (auto status = admission(); !status) return status;
        if (!valid_range(offset, out.size())) return Status::invalid_argument("metadata byte range");
        for (usize index = 0; index < out.size(); ++index) out[index] = entry->staged[offset + index];
        return {};
    }
    [[nodiscard]] Status edit(Transaction &transaction, u64 block, usize offset, std::span<const std::byte> bytes)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(transaction, block);
        if (!entry) return Status::bad_descriptor("metadata transaction does not own block");
        if (auto status = admission(); !status) return status;
        if (!valid_range(offset, bytes.size())) return Status::invalid_argument("metadata byte range");
        for (usize index = 0; index < bytes.size(); ++index) entry->staged[offset + index] = bytes[index];
        entry->report.staged_dirty = true;
        return {};
    }
    [[nodiscard]] Status release(Pin &pin)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(pin);
        if (!entry) return Status::bad_descriptor("released or stale metadata pin");
        --entry->report.read_pins; --report_.read_pins; pin.active_ = false;
        return {};
    }
    [[nodiscard]] Status discard(Transaction &transaction)
    {
        smp::ScopedSpinLock guard(lock_);
        if (transaction.owner_ != this || !transaction.active_ || transaction_ != &transaction || transaction.serial_ != transaction_serial_)
            return Status::bad_descriptor("retired metadata transaction");
        for (auto &entry : entries_)
            if (entry.report.transaction_serial == transaction.serial_)
            { entry.report.transaction_serial = 0; entry.report.staged_dirty = false; }
        transaction.active_ = false; transaction_ = nullptr; report_.transaction_active = false;
        return {};
    }
    [[nodiscard]] Result<MetadataBufferReport> inspect(const Pin &pin)
    {
        smp::ScopedSpinLock guard(lock_);
        const auto *entry = find(pin);
        return entry ? Result<MetadataBufferReport>{entry->report} : Result<MetadataBufferReport>{Status::bad_descriptor("released metadata pin")};
    }
    typename Registry::Session session_{};
    std::array<Entry, Capacity> entries_{};
    MetadataOwnerReport report_{};
    usize sectors_per_block_{};
    u64 transaction_serial_{};
    Transaction *transaction_{};
    bool writable_{};
    block::SessionBindingObservation session_binding_{};
    mutable smp::SpinLock lock_{};
};
} // namespace ok::fs
