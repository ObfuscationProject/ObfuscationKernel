#pragma once

#include "ok/core/types.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <utility>

namespace ok::fs
{
template <usize> class Ext4MountedMetadata;
// Session must identify one mounted-device lifetime. The on-disk generation
// may be zero; the caller must obtain it from a checked inode image.
struct Ext4InodeLifetimeKey
{
    const void *mount{};
    u64 session{};
    u32 inode{}, generation{};
    bool operator==(const Ext4InodeLifetimeKey &) const = default;
};

enum class Ext4InodeHold : u8 { descriptor, mapping, io, pin };
enum class Ext4InodeLifetimePhase : u8
{ linked, retired, ready, dispatched, publishing };

struct Ext4InodeLifetimeSnapshot
{
    Ext4InodeLifetimeKey key{};
    std::array<u32, 4> holds{};
    Ext4InodeLifetimePhase phase{Ext4InodeLifetimePhase::linked};
    [[nodiscard]] u32 total() const
    { return holds[0] + holds[1] + holds[2] + holds[3]; }
};

// Bounded bookkeeping for VFS path/fd references, mappings, in-flight I/O and
// other pins. Its own lock protects only counters and state; it never performs
// I/O, invokes a callback or allocates while locked. The owner must outlive all
// leases and dispatched tickets and must serialize the namespace publication
// decision with observe/retain/mark_orphan at the filesystem boundary.
template <usize RecordCapacity = 128, usize LeaseCapacity = 512>
class Ext4InodeLifetimeTable final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4MountedMetadata;
    static_assert(RecordCapacity > 0 && LeaseCapacity > 0);

  public:
    class Lease final
    {
        friend class Ext4InodeLifetimeTable;
      public:
        Lease() = default;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&other) noexcept
        { if (other.owner_) other.owner_->move_lease(*this, other); }
        // Overwriting a live lease would silently lose a counted hold. Transfer
        // into storage by move construction (for example optional::emplace).
        Lease &operator=(Lease &&) = delete;
        [[nodiscard]] bool valid() const { return owner_ != nullptr; }
      private:
        Lease(const Ext4InodeLifetimeTable *owner, usize slot, u64 serial)
            : owner_(owner), slot_(slot), serial_(serial) {}
        const Ext4InodeLifetimeTable *owner_{};
        usize slot_{};
        u64 serial_{};
    };
    class Ticket final
    {
        friend class Ext4InodeLifetimeTable;
      public:
        Ticket() = default;
        [[nodiscard]] bool valid() const { return owner_ != nullptr; }
        [[nodiscard]] Ext4InodeLifetimeKey key() const { return key_; }
      private:
        Ticket(const Ext4InodeLifetimeTable *owner, Ext4InodeLifetimeKey key,
               u64 record_serial, u64 dispatch_serial)
            : owner_(owner), key_(key), record_serial_(record_serial),
              dispatch_serial_(dispatch_serial) {}
        const Ext4InodeLifetimeTable *owner_{};
        Ext4InodeLifetimeKey key_{};
        u64 record_serial_{};
        u64 dispatch_serial_{};
    };

    Ext4InodeLifetimeTable() = default;
    Ext4InodeLifetimeTable(const Ext4InodeLifetimeTable &) = delete;
    Ext4InodeLifetimeTable &operator=(const Ext4InodeLifetimeTable &) = delete;
    Ext4InodeLifetimeTable(Ext4InodeLifetimeTable &&) = delete;
    Ext4InodeLifetimeTable &operator=(Ext4InodeLifetimeTable &&) = delete;

    // Observe only after the inode number and generation were checked against
    // the current mounted image. Re-observing the same linked key is harmless.
    [[nodiscard]] Status observe(Ext4InodeLifetimeKey key)
    {
        if (!valid_key(key)) return Status::invalid_argument("inode lifetime key");
        smp::ScopedSpinLock guard(lock_);
        usize free = RecordCapacity;
        for (usize index = 0; index < records_.size(); ++index)
        {
            const auto &record = records_[index];
            if (!record.used) { if (free == RecordCapacity) free = index; continue; }
            if (record.key.mount == key.mount && record.key.inode == key.inode)
                return record.key == key && record.phase == Phase::linked ? Status{} :
                    Status::busy("inode lifetime identity remains occupied");
        }
        if (free == RecordCapacity) return Status::no_memory("inode lifetime record capacity");
        if (record_serial_ == ~u64{0}) return Status::overflow("inode lifetime record serial exhausted");
        records_[free] = {key, ++record_serial_, 0, {}, Phase::linked, true};
        return {};
    }

    // A new pathname/open can acquire only while linked. Clones from an
    // existing lease remain legal after unlink, as with dup and mmap on an fd.
    [[nodiscard]] Result<Lease> retain(Ext4InodeLifetimeKey key, Ext4InodeHold kind)
    {
        if (!valid_kind(kind)) return Status::invalid_argument("inode lifetime hold kind");
        smp::ScopedSpinLock guard(lock_);
        const usize record = find(key);
        if (record == RecordCapacity) return Status::not_found("inode lifetime key absent");
        if (records_[record].phase != Phase::linked)
            return Status::busy("inode lifetime is detached");
        return acquire(record, kind);
    }
    [[nodiscard]] Result<Lease> clone(const Lease &source, Ext4InodeHold kind)
    {
        if (!valid_kind(kind)) return Status::invalid_argument("inode lifetime hold kind");
        smp::ScopedSpinLock guard(lock_);
        const LeaseSlot *held = live(source);
        if (!held) return Status::bad_descriptor("inode lifetime source lease is stale");
        const usize record = held->record;
        if (records_[record].phase != Phase::linked && records_[record].phase != Phase::retired)
            return Status::busy("inode lifetime already final");
        return acquire(record, kind);
    }
    [[nodiscard]] Status release(Lease &lease)
    {
        smp::ScopedSpinLock guard(lock_);
        LeaseSlot *held = live(lease);
        if (!held) return Status::bad_descriptor("inode lifetime release lease is stale");
        if (held->mutation_context || held->mutation_lease || held->mutation_epoch)
            return Status::busy("inode mutation role requires original terminal release");
        return release_locked(lease, *held);
    }

    // Call only after the last-link namespace transaction is durable. This
    // blocks fresh path references; existing holders can still clone a hold.
    [[nodiscard]] Status mark_orphan(Ext4InodeLifetimeKey key)
    {
        smp::ScopedSpinLock guard(lock_);
        const usize index = find(key);
        if (index == RecordCapacity) return Status::not_found("inode lifetime orphan key absent");
        Record &record = records_[index];
        if (record.phase != Phase::linked)
            return Status::already_exists("inode lifetime orphan already marked");
        record.phase = total(record) ? Phase::retired : Phase::ready;
        return {};
    }

    // Scheduler is called only after the table lock has been released. A
    // failed enqueue returns the ticket to ready for a later attempt.
    template <typename Scheduler>
    [[nodiscard]] Status schedule_one(Scheduler &&scheduler)
    {
        Ticket ticket{};
        {
            smp::ScopedSpinLock guard(lock_);
            for (auto &record : records_)
                if (record.used && record.phase == Phase::ready)
                {
                    if (dispatch_serial_ == ~u64{0})
                        return Status::overflow("inode lifetime dispatch serial exhausted");
                    record.phase = Phase::dispatched;
                    record.dispatch_serial = ++dispatch_serial_;
                    ticket = Ticket{this, record.key, record.serial, record.dispatch_serial};
                    break;
                }
        }
        if (!ticket.valid()) return Status::not_found("inode lifetime no ready orphan");
        const Status status = scheduler(ticket);
        if (!status)
        {
            smp::ScopedSpinLock guard(lock_);
            const usize index = find_ticket(ticket);
            if (index != RecordCapacity && records_[index].phase == Phase::dispatched)
                records_[index].phase = Phase::ready;
        }
        return status;
    }

    // Dispatch only the checked filesystem head. Callback execution remains
    // outside the table lock, and an old failed callback cannot roll back a
    // newer dispatch or a reused record with the same public disk identity.
    template <typename Scheduler>
    [[nodiscard]] Status schedule_exact(Ext4InodeLifetimeKey key, Scheduler &&scheduler)
    {
        if (!valid_key(key)) return Status::invalid_argument("inode lifetime exact schedule key");
        Ticket ticket{};
        {
            smp::ScopedSpinLock guard(lock_);
            const usize index = find(key);
            if (index == RecordCapacity)
                return Status::not_found("inode lifetime exact orphan key absent");
            auto &record = records_[index];
            if (record.phase != Phase::ready)
                return Status::busy("inode lifetime exact orphan is not ready");
            if (record.holds != std::array<u32, 4>{})
                return Status::fault("inode lifetime ready orphan still has holds");
            if (dispatch_serial_ == ~u64{0})
                return Status::overflow("inode lifetime dispatch serial exhausted");
            record.phase = Phase::dispatched;
            record.dispatch_serial = ++dispatch_serial_;
            ticket = Ticket{this, record.key, record.serial, record.dispatch_serial};
        }
        const Status status = scheduler(ticket);
        if (!status)
        {
            smp::ScopedSpinLock guard(lock_);
            const usize index = find_ticket(ticket);
            if (index != RecordCapacity && records_[index].phase == Phase::dispatched)
                records_[index].phase = Phase::ready;
        }
        return status;
    }

    // Worker failure can be retried. Only a caller that has completed durable
    // reclaim may acknowledge; this merely frees the bookkeeping record.
    // Read-only worker admission. A public key/snapshot cannot stand in for
    // the actual record and dispatch serials retained by a current ticket.
    // Success grants no durable-reclaim completion or acknowledgement.
    [[nodiscard]] Status validate_dispatched(const Ticket &ticket,
                                              Ext4InodeLifetimeKey expected) const
    {
        smp::ScopedSpinLock guard(lock_);
        const usize index = find_ticket(ticket);
        if (index == RecordCapacity || records_[index].key != expected ||
            records_[index].phase != Phase::dispatched ||
            records_[index].holds != std::array<u32, 4>{})
            return Status::bad_descriptor("inode lifetime dispatched ticket is stale or occupied");
        return {};
    }

    [[nodiscard]] Status retry(const Ticket &ticket)
    {
        smp::ScopedSpinLock guard(lock_);
        const usize index = find_ticket(ticket);
        if (index == RecordCapacity || records_[index].phase != Phase::dispatched)
            return Status::bad_descriptor("inode lifetime retry ticket is stale");
        records_[index].phase = Phase::ready;
        return {};
    }
    [[nodiscard]] Status acknowledge_reclaimed(const Ticket &ticket)
    {
        smp::ScopedSpinLock guard(lock_);
        const usize index = find_ticket(ticket);
        if (index == RecordCapacity || records_[index].phase != Phase::dispatched ||
            total(records_[index]))
            return Status::bad_descriptor("inode lifetime completion ticket is stale");
        records_[index] = {};
        return {};
    }

    [[nodiscard]] Result<Ext4InodeLifetimeSnapshot> snapshot(Ext4InodeLifetimeKey key) const
    {
        smp::ScopedSpinLock guard(lock_);
        const usize index = find(key);
        if (index == RecordCapacity) return Status::not_found("inode lifetime snapshot key absent");
        const auto &record = records_[index];
        return Ext4InodeLifetimeSnapshot{record.key, record.holds,
            static_cast<Ext4InodeLifetimePhase>(record.phase)};
    }
    [[nodiscard]] usize active_records() const
    {
        smp::ScopedSpinLock guard(lock_);
        usize count = 0;
        for (const auto &record : records_) count += record.used;
        return count;
    }

  private:
    // Provider-only cold teardown validation. Neither a snapshot nor an
    // optional Lease object's presence proves its actual slot authority.
    [[nodiscard]] Status validate_lease(const Lease &lease, Ext4InodeLifetimeKey expected,
                                        Ext4InodeHold kind) const
    {
        smp::ScopedSpinLock guard(lock_);
        const auto *held = live(lease);
        if (!held || !lease.serial_ || lease.serial_ > lease_serial_ || !held->record_serial ||
            held->record_serial > record_serial_ || !valid_key(expected) || !valid_kind(kind) ||
            held->kind != kind || records_[held->record].key != expected ||
            !records_[held->record].holds[static_cast<usize>(kind)])
            return Status::fault("mounted inode lease serial, key or hold differs");
        return {};
    }
    [[nodiscard]] static bool same_lease(const Lease &first, const Lease &second)
    { return first.owner_ == second.owner_ && first.slot_ == second.slot_ && first.serial_ == second.serial_; }
    // Provider-only exclusive mutation role on the actual occupied I/O slot.
    // Ordinary reader I/O holds intentionally have no such role: a qualified
    // retained lookup may acquire a descriptor while its read hold is live.
    [[nodiscard]] Status claim_node_mutation(const Lease &lease, Ext4InodeLifetimeKey key,
                                             const void *context, u64 epoch)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *held = live(lease);
        if (!held || held->kind != Ext4InodeHold::io || !valid_key(key) ||
            records_[held->record].key != key || !context || !epoch ||
            held->mutation_context || held->mutation_lease || held->mutation_epoch)
            return Status::fault("mounted mutation actual I/O claim differs");
        for (const auto &slot : leases_)
            if (slot.mutation_context || slot.mutation_lease || slot.mutation_epoch)
                return Status::fault("mounted mutation actual I/O claim is occupied");
        held->mutation_context = context; held->mutation_lease = &lease; held->mutation_epoch = epoch;
        return {};
    }
    [[nodiscard]] Status validate_node_mutation(const Lease &lease, Ext4InodeLifetimeKey key,
                                                const void *context, u64 epoch) const
    {
        smp::ScopedSpinLock guard(lock_);
        const auto *held = live(lease);
        if (!held || held->kind != Ext4InodeHold::io || records_[held->record].key != key ||
            held->mutation_context != context || held->mutation_lease != &lease ||
            !epoch || held->mutation_epoch != epoch)
            return Status::fault("mounted original mutation actual I/O role changed");
        return {};
    }
    // Only the owning provider's original stack context may consume a claimed
    // role at its terminal boundary; generic release and moves cannot do so.
    [[nodiscard]] Status release_node_mutation(Lease &lease, Ext4InodeLifetimeKey key,
                                               const void *context, u64 epoch)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *held = live(lease);
        if (!held || held->kind != Ext4InodeHold::io || records_[held->record].key != key ||
            !context || held->mutation_context != context || held->mutation_lease != &lease ||
            !epoch || held->mutation_epoch != epoch)
            return Status::fault("mounted mutation terminal role identity differs");
        return release_locked(lease, *held);
    }
    // No mutable mount pointer, diagnostic phase or summary hold count can
    // erase this authority. The role is consumed only by actual Lease release.
    [[nodiscard]] Result<bool> node_mutation_active(const void *mount, u64 session) const
    {
        smp::ScopedSpinLock guard(lock_);
        bool active{};
        for (const auto &held : leases_)
        {
            if (!held.mutation_context && !held.mutation_lease && !held.mutation_epoch) continue;
            if (active || !held.used || !held.mutation_context || !held.mutation_lease ||
                !held.mutation_epoch || !held.serial || held.serial > lease_serial_ ||
                held.kind != Ext4InodeHold::io || held.record >= RecordCapacity)
                return Status::fault("mounted actual mutation role is incomplete");
            const auto &record = records_[held.record];
            if (!record.used || !record.serial || record.serial > record_serial_ ||
                record.serial != held.record_serial || !valid_key(record.key) ||
                record.key.mount != mount || record.key.session != session)
                return Status::fault("mounted actual mutation role identity differs");
            active = true;
        }
        return active;
    }
    [[nodiscard]] Status audit_lease_records(const void *mount, u64 session) const
    {
        smp::ScopedSpinLock guard(lock_);
        for (usize slot = 0; slot < leases_.size(); ++slot)
            if (leases_[slot].used)
                for (usize other = slot + 1; other < leases_.size(); ++other)
                    if (leases_[other].used && leases_[slot].serial == leases_[other].serial)
                        return Status::fault("mounted inode actual lease serial is duplicated");
        for (usize index = 0; index < records_.size(); ++index)
        {
            const auto &record = records_[index];
            std::array<u32, 4> actual{};
            if (record.used && (!record.serial || record.serial > record_serial_ ||
                record.key.mount != mount || record.key.session != session || !valid_key(record.key)))
                return Status::fault("mounted inode record identity differs");
            for (usize slot = 0; slot < leases_.size(); ++slot)
            {
                const auto &held = leases_[slot];
                if (!held.used) continue;
                if (held.record >= records_.size() || !records_[held.record].used ||
                    held.record_serial != records_[held.record].serial || !held.serial ||
                    held.serial > lease_serial_ || !valid_kind(held.kind))
                    return Status::fault("mounted inode actual lease record differs");
                if (held.record == index) ++actual[static_cast<usize>(held.kind)];
            }
            if (record.holds != actual)
                return Status::fault("mounted inode counters differ from actual leases");
        }
        return {};
    }
    // Owner-only decision seal. No lifetime lock spans transport I/O. A
    // claimed dispatch cannot be retried, acknowledged or rolled back by a
    // scheduler callback; uncertain effects retain the occupied record.
    [[nodiscard]] Status claim_publication(const Ticket &ticket,
        Ext4InodeLifetimeKey expected, const void *publisher)
    {
        smp::ScopedSpinLock guard(lock_);
        const usize index = find_ticket(ticket);
        if (!publisher || index == RecordCapacity || records_[index].key != expected ||
            records_[index].phase != Phase::dispatched || records_[index].holds != std::array<u32, 4>{})
            return Status::bad_descriptor("inode lifetime publication claim is stale or occupied");
        records_[index].publisher = publisher;
        records_[index].phase = Phase::publishing;
        return {};
    }
    [[nodiscard]] Status validate_publication(const Ticket &ticket,
        Ext4InodeLifetimeKey expected, const void *publisher) const
    {
        smp::ScopedSpinLock guard(lock_);
        const usize index = find_ticket(ticket);
        if (!publisher || index == RecordCapacity || records_[index].key != expected ||
            records_[index].phase != Phase::publishing || records_[index].publisher != publisher ||
            records_[index].holds != std::array<u32, 4>{})
            return Status::bad_descriptor("inode lifetime publication seal is stale or occupied");
        return {};
    }
    [[nodiscard]] Status complete_publication(const Ticket &ticket,
        Ext4InodeLifetimeKey expected, const void *publisher)
    {
        smp::ScopedSpinLock guard(lock_);
        const usize index = find_ticket(ticket);
        if (!publisher || index == RecordCapacity || records_[index].key != expected ||
            records_[index].phase != Phase::publishing || records_[index].publisher != publisher ||
            records_[index].holds != std::array<u32, 4>{})
            return Status::bad_descriptor("inode lifetime durable completion seal is stale or occupied");
        records_[index] = {};
        return {};
    }
    enum class Phase : u8 { linked, retired, ready, dispatched, publishing };
    struct Record
    {
        Ext4InodeLifetimeKey key{};
        u64 serial{};
        u64 dispatch_serial{};
        std::array<u32, 4> holds{};
        Phase phase{Phase::linked};
        bool used{};
        const void *publisher{};
    };
    struct LeaseSlot
    {
        usize record{};
        u64 record_serial{}, serial{};
        Ext4InodeHold kind{Ext4InodeHold::descriptor};
        bool used{};
        const void *mutation_context{};
        const Lease *mutation_lease{};
        u64 mutation_epoch{};
    };
    [[nodiscard]] static bool valid_key(const Ext4InodeLifetimeKey &key)
    { return key.mount && key.session && key.inode; }
    [[nodiscard]] static bool valid_kind(Ext4InodeHold kind)
    { return static_cast<usize>(kind) < 4; }
    [[nodiscard]] static u32 total(const Record &record)
    { return record.holds[0] + record.holds[1] + record.holds[2] + record.holds[3]; }
    [[nodiscard]] usize find(Ext4InodeLifetimeKey key) const
    {
        for (usize index = 0; index < records_.size(); ++index)
            if (records_[index].used && records_[index].key == key) return index;
        return RecordCapacity;
    }
    [[nodiscard]] usize find_ticket(const Ticket &ticket) const
    {
        if (ticket.owner_ != this) return RecordCapacity;
        const usize index = find(ticket.key_);
        return index != RecordCapacity && records_[index].serial == ticket.record_serial_ &&
            records_[index].dispatch_serial == ticket.dispatch_serial_ ?
            index : RecordCapacity;
    }
    [[nodiscard]] LeaseSlot *live(const Lease &lease)
    { return const_cast<LeaseSlot *>(std::as_const(*this).live(lease)); }
    [[nodiscard]] const LeaseSlot *live(const Lease &lease) const
    {
        if (lease.owner_ != this || lease.slot_ >= LeaseCapacity) return nullptr;
        const auto &held = leases_[lease.slot_];
        if (!held.used || held.serial != lease.serial_ || held.record >= RecordCapacity ||
            !records_[held.record].used ||
            records_[held.record].serial != held.record_serial) return nullptr;
        return &held;
    }
    void move_lease(Lease &target, Lease &source) const
    {
        // The caller exclusively owns this token. During acquire the table
        // lock is already held; afterward only this token's owner may claim,
        // release or transfer its slot. Unrelated slots cannot change it.
        // Re-locking here would deadlock Result/optional construction in retain.
        const auto *held = live(source);
        // Refused transfer leaves the original token unchanged and the new
        // object invalid. Normal unclaimed leases retain move construction.
        if (!held || held->mutation_context || held->mutation_lease || held->mutation_epoch) return;
        target.owner_ = std::exchange(source.owner_, nullptr);
        target.slot_ = std::exchange(source.slot_, 0);
        target.serial_ = std::exchange(source.serial_, 0);
    }
    [[nodiscard]] Status release_locked(Lease &lease, LeaseSlot &held)
    {
        auto &record = records_[held.record];
        const usize kind = static_cast<usize>(held.kind);
        if (!valid_kind(held.kind) || !record.holds[kind]) return Status::fault("inode lifetime count underflow");
        --record.holds[kind]; held = {};
        lease.owner_ = nullptr; lease.slot_ = 0; lease.serial_ = 0;
        if (record.phase == Phase::retired && !total(record)) record.phase = Phase::ready;
        return {};
    }
    [[nodiscard]] Result<Lease> acquire(usize record, Ext4InodeHold kind)
    {
        usize slot = LeaseCapacity;
        for (usize index = 0; index < leases_.size(); ++index)
            if (!leases_[index].used) { slot = index; break; }
        if (slot == LeaseCapacity) return Status::no_memory("inode lifetime lease capacity");
        if (lease_serial_ == ~u64{0}) return Status::overflow("inode lifetime lease serial exhausted");
        auto &count = records_[record].holds[static_cast<usize>(kind)];
        if (count == ~u32{0}) return Status::overflow("inode lifetime hold count exhausted");
        ++count;
        leases_[slot] = {record, records_[record].serial, ++lease_serial_, kind, true};
        return Lease{this, slot, lease_serial_};
    }

    mutable smp::SpinLock lock_{};
    std::array<Record, RecordCapacity> records_{};
    std::array<LeaseSlot, LeaseCapacity> leases_{};
    u64 record_serial_{}, lease_serial_{}, dispatch_serial_{};
};
} // namespace ok::fs
