#pragma once

// ext4 alloc module: inode allocation and lifetime, regular and directory create plans, orphan
// chains and orphan reclaim planning.

#include "ok/fs/ext4/layout.hpp"
#include "ok/fs/ext4/extent.hpp"
#include "ok/core/types.hpp"
#include "ok/smp/smp.hpp"
#include "ok/fs/ext4_directory_create_sdk_profile.hpp"
#include "ok/block/sessions.hpp"
#include <array>
#include <utility>
#include <new>

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

    // Drop the record of a linked inode that nothing holds, so its identity
    // can leave a provider's node cache. A later lookup observes it afresh.
    [[nodiscard]] Status forget(Ext4InodeLifetimeKey key)
    {
        smp::ScopedSpinLock guard(lock_);
        const usize index = find(key);
        if (index == RecordCapacity) return Status::not_found("inode lifetime forget key absent");
        if (records_[index].phase != Phase::linked || total(records_[index]))
            return Status::busy("inode lifetime record is held or detached");
        records_[index] = {};
        return {};
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

// Exact private free-slot format. A retained generation is advanced by reclaim
// and consumed by create; the allocated record never retains this marker.
class Ext4FreeInode final
{
    static void store16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    static void store32(std::span<u8> bytes, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) bytes[at + i] = u8(value >> (i * 8)); }
  public:
    static constexpr u32 marker = 0x3146524fU;
    [[nodiscard]] static Status encode(std::span<u8> inode, u32 number, u32 seed, u32 generation)
    {
        if (inode.size() != 256 || !number || !generation)
            return Status::invalid_argument("free inode marker identity or size");
        for (u8 &byte : inode) byte = 0;
        store32(inode, 100, generation); store16(inode, 128, 32);
        store32(inode, 160, marker);
        const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(crc)); store16(inode, 130, u16(crc >> 16));
        return {};
    }
    [[nodiscard]] static Result<u32> inspect(std::span<const u8> inode, u32 number, u32 seed)
    {
        if (inode.size() != 256 || !number)
            return Status::invalid_argument("free inode slot identity or size");
        if (jbd2::detail::all_zero(inode)) return u32{0};
        const u32 generation = ext4_checked::le32(inode, 100);
        if (!generation || ext4_checked::le32(inode, 160) != marker)
            return Status::unsupported("free inode marker or generation differs");
        if (auto status = ext4_checked::inode_checksum(seed, number, inode); !status) return status;
        std::array<u8, 256> expected{};
        if (auto status = encode(expected, number, seed, generation); !status) return status;
        for (usize i = 0; i < expected.size(); ++i)
            if (inode[i] != expected[i])
                return Status::unsupported("free inode marker has noncanonical bytes");
        // Keep one generation available for retirement of the next identity.
        if (generation == ~u32{0}) return Status::overflow("free inode generation exhausted");
        return generation;
    }
};

using Ext4CreateBlock = std::array<u8, 1024>;

struct Ext4CreateUpdate
{
    u32 parent{}, seconds{}, nanoseconds{};
    std::string_view name{};
    InitialNodeAttributes attributes{};
};

// The owner obtains these six old images from one qualified, exclusive view.
// Parent and new inode tables may name the same physical block, in which case
// their images must be identical. No disk access is performed by this planner.
struct Ext4CreateSource
{
    u32 inode{}, group{}, parent_table_block{}, new_table_block{}, directory_block{};
    const Ext4CreateBlock &superblock;
    const Ext4CreateBlock &descriptors;
    const Ext4CreateBlock &inode_bitmap;
    const Ext4CreateBlock &parent_table;
    const Ext4CreateBlock &new_table;
    const Ext4CreateBlock &directory;
};

struct Ext4CreatePlan
{
    u32 inode{}, parent{}, directory_block{}, allocated_directory_block{}, block_group{};
    // The directory successor adds a block bitmap and one initialized data
    // block. Empty regular-file plans retain their original five/six homes.
    std::array<u32, ext4_directory_create_home_capacity> targets{};
    std::array<Ext4CreateBlock, ext4_directory_create_home_capacity> before{}, after{};
    usize count{};
};

// Bounded 1 KiB empty regular-file allocation. This is an image planner, not
// journal authority, a mounted namespace operation, or a generic allocator.
class Ext4CreatePlanner final
{
    using Block = Ext4CreateBlock;
    struct Geometry
    {
        u32 groups{}, inodes{}, inodes_per_group{}, first_inode{}, table_blocks{}, seed{};
    };
    struct Group
    {
        u32 bitmap{}, table{};
        u16 free_inodes{}, unused_inodes{}, flags{};
    };

    static constexpr void store16(std::span<u8> bytes, usize at, u16 value)
    {
        bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8);
    }
    static constexpr void store32(std::span<u8> bytes, usize at, u32 value)
    {
        for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8));
    }
    [[nodiscard]] static constexpr usize minimum_record(usize name_length)
    { return (8 + name_length + 3) & ~usize{3}; }
    [[nodiscard]] static bool valid_name(std::string_view name)
    {
        if (name.empty() || name.size() > 31 || name == "." || name == "..") return false;
        for (char byte : name) if (!byte || byte == '/') return false;
        return true;
    }
    [[nodiscard]] static u16 descriptor_checksum(u32 seed, u32 number, std::span<const u8> bytes)
    {
        std::array<u8, 32> copy{};
        for (usize index = 0; index < copy.size(); ++index) copy[index] = bytes[index];
        copy[30] = copy[31] = 0;
        return u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed, ext4_checked::encoded(number)), copy));
    }
    static void stamp_inode(std::span<u8> inode, u32 number, u32 seed)
    {
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(crc)); store16(inode, 130, u16(crc >> 16));
    }
    [[nodiscard]] static Status inspect(const Ext4RecoveryInfo &binding, const Block &superblock,
                                        const Block &descriptors, Geometry &geometry)
    {
        using namespace ext4_checked;
        const auto s = std::span<const u8>{superblock};
        if (binding.layout.block_size != 1024 || binding.layout.filesystem_blocks > 0xffffffffULL ||
            binding.layout.filesystem_blocks < 3 || binding.journal_inode != 8 ||
            binding.layout.journal_blocks < 1024 ||
            binding.layout.journal_first_block >= binding.layout.filesystem_blocks ||
            binding.layout.journal_blocks >
                binding.layout.filesystem_blocks - binding.layout.journal_first_block ||
            binding.metadata_count > binding.metadata.size() ||
            !binding.metadata_count || binding.metadata_count % 4 ||
            !binding.structural_metadata().valid())
            return Status::unsupported("bounded create binding layout");
        for (usize index = 0; index < binding.metadata_count; ++index)
        {
            const auto one = binding.metadata[index];
            if (one.first < binding.layout.journal_first_block + binding.layout.journal_blocks &&
                binding.layout.journal_first_block < one.first + one.count)
                return Status::io_error("bounded create metadata aliases journal");
            for (usize previous = 0; previous < index; ++previous)
            {
                const auto other = binding.metadata[previous];
                if (one.first < other.first + other.count && other.first < one.first + one.count)
                    return Status::io_error("bounded create metadata aliases metadata");
            }
        }
        if (jbd2::detail::crc32c(~u32{0}, s.first(1020)) != le32(s, 1020))
            return Status::io_error("bounded create superblock checksum");
        if (le16(s, 56) != 0xef53 || le32(s, 24) || le32(s, 20) != 1 ||
            le32(s, 4) != binding.layout.filesystem_blocks || le32(s, 76) != 1 || le32(s, 72) ||
            le32(s, 92) != 4 || le32(s, 96) != (binding.needs_recovery ? 0x46U : 0x42U) ||
            le32(s, 100) != 0x400 || le16(s, 88) != 256 || le16(s, 90) || le16(s, 58) != 1 ||
            le16(s, 206) || le32(s, 224) != 8 || le32(s, 228) || le32(s, 232) ||
            s[373] != 1 || (le16(s, 254) != 0 && le16(s, 254) != 32) ||
            le32(s, 336) || le32(s, 340) || le32(s, 344) ||
            !jbd2::detail::all_zero(s.subspan(208, 16)))
            return Status::unsupported("bounded create superblock profile");
        for (usize index = 0; index < binding.uuid.size(); ++index)
            if (s[104 + index] != binding.uuid[index])
                return Status::io_error("bounded create filesystem UUID");
        const u32 blocks_per_group = le32(s, 32);
        geometry.inodes = le32(s, 0);
        geometry.inodes_per_group = le32(s, 40);
        geometry.first_inode = le32(s, 84);
        if (!blocks_per_group || blocks_per_group > 8192 || blocks_per_group % 8 ||
            !geometry.inodes_per_group || geometry.inodes_per_group > 8192 ||
            geometry.inodes_per_group % 8 || geometry.inodes_per_group % 4 ||
            le32(s, 28) != 0 || le32(s, 36) != blocks_per_group ||
            geometry.first_inode < 11 || geometry.first_inode > geometry.inodes ||
            le32(s, 12) > binding.layout.filesystem_blocks - 1 ||
            le32(s, 16) > geometry.inodes)
            return Status::io_error("bounded create group geometry");
        geometry.groups = u32((binding.layout.filesystem_blocks - 2) / blocks_per_group + 1);
        geometry.table_blocks = geometry.inodes_per_group / 4;
        geometry.seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        if (geometry.groups > Ext4RecoveryInfo::group_capacity ||
            geometry.groups * 4 != binding.metadata_count ||
            u64{geometry.groups} * geometry.inodes_per_group != geometry.inodes)
            return Status::unsupported("bounded create group capacity");
        u32 free_total = 0;
        for (u32 number = 0; number < geometry.groups; ++number)
        {
            const auto d = std::span<const u8>{descriptors}.subspan(number * 32, 32);
            if (descriptor_checksum(geometry.seed, number, d) != le16(d, 30))
                return Status::io_error("bounded create descriptor checksum");
            const u32 begin = 1 + number * blocks_per_group;
            const u32 count = u32(binding.layout.filesystem_blocks - begin < blocks_per_group
                ? binding.layout.filesystem_blocks - begin : blocks_per_group);
            const u32 bitmap = le32(d, 4), table = le32(d, 8);
            const std::array<jbd2::BlockRange, 4> expected{{{begin, 2}, {le32(d, 0), 1},
                                                              {bitmap, 1}, {table, geometry.table_blocks}}};
            for (usize index = 0; index < expected.size(); ++index)
                if (binding.metadata[number * 4 + index].first != expected[index].first ||
                    binding.metadata[number * 4 + index].count != expected[index].count)
                    return Status::unsupported("bounded create group placement changed");
            const u16 flags = le16(d, 18);
            if (count < 2 || (flags & ~u16{7}) || le16(d, 12) > count ||
                le16(d, 14) > geometry.inodes_per_group ||
                le16(d, 16) > geometry.inodes_per_group - le16(d, 14) ||
                le16(d, 28) > le16(d, 14) ||
                ((flags & 1) && (le16(d, 14) != geometry.inodes_per_group ||
                                 le16(d, 16) || le16(d, 28) != geometry.inodes_per_group || le16(d, 26))) ||
                ((flags & 2) && (count < geometry.table_blocks + 4 ||
                                 le16(d, 12) != count - geometry.table_blocks - 4 || le16(d, 24))))
                return Status::io_error("bounded create group counters or flags");
            free_total += le16(d, 14);
        }
        if (free_total != le32(s, 16))
            return Status::io_error("bounded create free inode counters disagree");
        return {};
    }
    [[nodiscard]] static Status group(const Block &descriptors, u32 number, const Geometry &geometry,
                                      Group &result)
    {
        const auto d = std::span<const u8>{descriptors}.subspan(number * 32, 32);
        result = {ext4_checked::le32(d, 4), ext4_checked::le32(d, 8),
                  ext4_checked::le16(d, 14), ext4_checked::le16(d, 28), ext4_checked::le16(d, 18)};
        if (result.flags & 1)
            return Status::unsupported("bounded create uninitialized inode bitmap");
        if (result.unused_inodes > result.free_inodes || result.free_inodes > geometry.inodes_per_group)
            return Status::io_error("bounded create inode group counters");
        return {};
    }
    [[nodiscard]] static bool ordinary_block(const Ext4RecoveryInfo &binding, u32 block)
    {
        if (block < 3 || block >= binding.layout.filesystem_blocks) return false;
        if (block >= binding.layout.journal_first_block &&
            u64{block} - binding.layout.journal_first_block < binding.layout.journal_blocks) return false;
        for (usize index = 0; index < binding.metadata_count; ++index)
        {
            const auto range = binding.metadata[index];
            if (block >= range.first && u64{block} - range.first < range.count) return false;
        }
        return true;
    }
    [[nodiscard]] static Status append(Ext4CreatePlan &output, u32 block, const Block &before, usize &index)
    {
        for (index = 0; index < output.count; ++index)
            if (output.targets[index] == block)
                return output.before[index] == before ? Status{} :
                    Status::io_error("bounded create aliased block images disagree");
        if (output.count == output.targets.size())
            return Status::no_memory("bounded create participant capacity");
        index = output.count++;
        output.targets[index] = block;
        output.before[index] = before;
        output.after[index] = before;
        return {};
    }

  public:
    [[nodiscard]] static Result<u32> select_inode(const Ext4RecoveryInfo &binding,
                                                  const Block &superblock, const Block &descriptors,
                                                  const Block &inode_bitmap, u32 group_number)
    {
        using namespace ext4_checked;
        Geometry geometry{};
        if (auto status = inspect(binding, superblock, descriptors, geometry); !status) return status;
        if (group_number >= geometry.groups)
            return Status::invalid_argument("bounded create inode group number");
        Group selected{};
        if (auto status = group(descriptors, group_number, geometry, selected); !status) return status;
        const auto bitmap = std::span<const u8>{inode_bitmap}.first(geometry.inodes_per_group / 8);
        const auto d = std::span<const u8>{descriptors}.subspan(group_number * 32, 32);
        if (u16(jbd2::detail::crc32c(geometry.seed, bitmap)) != le16(d, 26))
            return Status::io_error("bounded create inode bitmap checksum");
        u32 free_count = 0, candidate = 0;
        const u32 unused_start = geometry.inodes_per_group - selected.unused_inodes;
        for (u32 local = 0; local < geometry.inodes_per_group; ++local)
        {
            const bool allocated = (bitmap[local / 8] & (1U << (local % 8))) != 0;
            const u32 number = group_number * geometry.inodes_per_group + local + 1;
            if (number < geometry.first_inode && !allocated)
                return Status::io_error("bounded create reserved inode bit is clear");
            if (local >= unused_start && allocated)
                return Status::io_error("bounded create unused inode suffix is allocated");
            if (!allocated)
            {
                ++free_count;
                if (!candidate && number >= geometry.first_inode) candidate = number;
            }
        }
        if (free_count != selected.free_inodes)
            return Status::io_error("bounded create inode bitmap count differs from descriptor");
        if (!candidate) return Status::no_space("bounded create no ordinary inode in group");
        return candidate;
    }

    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding, const Ext4CreateUpdate &update,
                                     const Ext4CreateSource &source, Ext4CreatePlan &output)
    {
        using namespace ext4_checked;
        output.count = 0;
        if (update.attributes.type != NodeType::regular)
            return Status::unsupported("bounded create requires a regular file");
        if (!valid_name(update.name) || (update.attributes.mode & ~u32{0777}) ||
            update.seconds > 0x7fffffffU ||
            update.nanoseconds >= 1000000000U)
            return Status::invalid_argument("bounded create name, regular mode or timestamp");
        Geometry geometry{};
        if (auto status = inspect(binding, source.superblock, source.descriptors, geometry); !status) return status;
        const auto chosen = select_inode(binding, source.superblock, source.descriptors,
                                         source.inode_bitmap, source.group);
        if (!chosen) return chosen.status();
        if (source.inode != chosen.value() || update.parent < 2 || update.parent > geometry.inodes ||
            update.parent == source.inode)
            return Status::invalid_argument("bounded create inode identity");
        Group selected{};
        if (auto status = group(source.descriptors, source.group, geometry, selected); !status) return status;
        const u32 parent_group = (update.parent - 1) / geometry.inodes_per_group;
        const u32 parent_local = (update.parent - 1) % geometry.inodes_per_group;
        const u32 new_local = (source.inode - 1) % geometry.inodes_per_group;
        const u32 parent_table = le32(std::span<const u8>{source.descriptors}.subspan(parent_group * 32, 32), 8) +
                                 parent_local / 4;
        const u32 new_table = selected.table + new_local / 4;
        if (source.parent_table_block != parent_table || source.new_table_block != new_table ||
            !ordinary_block(binding, source.directory_block) ||
            (parent_table == new_table && source.parent_table != source.new_table))
            return Status::unsupported("bounded create participant placement or aliased table images");
        const usize parent_offset = (parent_local % 4) * 256;
        const usize new_offset = (new_local % 4) * 256;
        const auto parent_inode = std::span<const u8>{source.parent_table}.subspan(parent_offset, 256);
        const auto empty_inode = std::span<const u8>{source.new_table}.subspan(new_offset, 256);
        if (auto status = inode_checksum(geometry.seed, update.parent, parent_inode); !status) return status;
        if ((le16(parent_inode, 0) & 0170000) != 0040000 || le16(parent_inode, 26) < 2 ||
            le32(parent_inode, 4) != 1024 || le32(parent_inode, 28) != 2 ||
            le32(parent_inode, 32) != 0x80000 || le16(parent_inode, 40) != 0xf30a ||
            le16(parent_inode, 42) != 1 || le16(parent_inode, 44) != 4 ||
            le16(parent_inode, 46) || le32(parent_inode, 52) || le16(parent_inode, 56) != 1 ||
            le16(parent_inode, 58) || le32(parent_inode, 60) != source.directory_block ||
            (le16(parent_inode, 0) & 0300) != 0300)
            return Status::unsupported("bounded create parent directory profile");
        const auto generation = Ext4FreeInode::inspect(empty_inode, source.inode, geometry.seed);
        if (!generation) return generation.status();
        const u64 version = u64{le32(parent_inode, 36)} | (u64{le32(parent_inode, 152)} << 32);
        if (version == ~u64{0}) return Status::overflow("bounded create parent version exhausted");
        constexpr usize tail = 1012;
        const auto directory = std::span<const u8>{source.directory};
        if (le32(directory, tail) || le16(directory, tail + 4) != 12 || directory[tail + 6] ||
            directory[tail + 7] != 0xde ||
            jbd2::detail::crc32c(inode_seed(geometry.seed, update.parent, parent_inode),
                                 directory.first(tail)) != le32(directory, tail + 8))
            return Status::io_error("bounded create directory tail checksum");
        usize slot = tail, split = 0;
        const usize needed = minimum_record(update.name.size());
        for (usize offset = 0; offset < tail;)
        {
            if (tail - offset < 8) return Status::io_error("bounded create short directory record");
            const u32 number = le32(directory, offset);
            const usize record = le16(directory, offset + 4), length = directory[offset + 6];
            const u8 type = directory[offset + 7];
            if (record < 8 || record % 4 || record > tail - offset || length > record - 8 ||
                number > geometry.inodes || (number && (!length || type > 7)))
                return Status::io_error("bounded create dirent bounds");
            if (number)
            {
                const std::string_view name{reinterpret_cast<const char *>(directory.data() + offset + 8), length};
                for (char byte : name)
                    if (!byte || byte == '/') return Status::io_error("bounded create invalid dirent name");
                if (name == update.name) return Status::already_exists("bounded create name exists");
                if (number == source.inode)
                    return Status::io_error("bounded create free inode already named");
                const usize minimum = minimum_record(length);
                if (slot == tail && record - minimum >= needed)
                {
                    // A zero predecessor lets ordered replay invert an already
                    // checkpointed directory insertion exactly.
                    bool zero_slack = true;
                    for (usize index = offset + minimum; index < offset + record; ++index)
                        if (directory[index]) { zero_slack = false; break; }
                    if (zero_slack) { slot = offset + minimum; split = minimum; }
                }
            }
            else if (slot == tail && record >= needed && !length && !type)
            {
                // Namespace unlink leaves one canonical empty record. The
                // complete create witness retains its original record length
                // and bytes; no inverse-directory guess supplies that history.
                bool empty_record = true;
                for (usize index = offset; index < offset + record; ++index)
                    if (index != offset + 4 && index != offset + 5 && directory[index])
                    { empty_record = false; break; }
                if (empty_record) { slot = offset; split = 0; }
            }
            offset += record;
        }
        if (slot == tail) return Status::no_space("bounded create directory block has no slot");
        usize parent_index{}, new_index{}, bitmap_index{}, descriptor_index{}, super_index{}, directory_index{};
        if (auto status = append(output, parent_table, source.parent_table, parent_index); !status) return status;
        if (auto status = append(output, new_table, source.new_table, new_index); !status) return status;
        if (auto status = append(output, selected.bitmap, source.inode_bitmap, bitmap_index); !status) return status;
        if (auto status = append(output, 2, source.descriptors, descriptor_index); !status) return status;
        if (auto status = append(output, 1, source.superblock, super_index); !status) return status;
        if (auto status = append(output, source.directory_block, source.directory, directory_index); !status)
            return status;
        if (output.count < 5 || output.count > 6 ||
            bitmap_index == parent_index || bitmap_index == new_index ||
            descriptor_index == parent_index || descriptor_index == new_index ||
            super_index == parent_index || super_index == new_index ||
            directory_index != output.count - 1)
            return Status::unsupported("bounded create prohibited participant alias");
        auto parent_after = std::span<u8>{output.after[parent_index]}.subspan(parent_offset, 256);
        store32(parent_after, 12, update.seconds); store32(parent_after, 132, update.nanoseconds << 2);
        store32(parent_after, 16, update.seconds); store32(parent_after, 136, update.nanoseconds << 2);
        store32(parent_after, 36, u32(version + 1)); store32(parent_after, 152, u32((version + 1) >> 32));
        stamp_inode(parent_after, update.parent, geometry.seed);
        auto new_after = std::span<u8>{output.after[new_index]}.subspan(new_offset, 256);
        for (u8 &byte : new_after) byte = 0;
        store32(new_after, 100, generation.value());
        store16(new_after, 0, u16(0100000 | update.attributes.mode));
        store16(new_after, 2, u16(update.attributes.uid));
        store16(new_after, 120, u16(update.attributes.uid >> 16));
        store16(new_after, 24, u16(update.attributes.gid));
        store16(new_after, 122, u16(update.attributes.gid >> 16));
        store16(new_after, 26, 1);
        store32(new_after, 8, update.seconds); store32(new_after, 140, update.nanoseconds << 2);
        store32(new_after, 12, update.seconds); store32(new_after, 132, update.nanoseconds << 2);
        store32(new_after, 16, update.seconds); store32(new_after, 136, update.nanoseconds << 2);
        store32(new_after, 144, update.seconds); store32(new_after, 148, update.nanoseconds << 2);
        store32(new_after, 32, 0x80000);
        store16(new_after, 40, 0xf30a); store16(new_after, 44, 4);
        store16(new_after, 128, 32);
        stamp_inode(new_after, source.inode, geometry.seed);
        auto &bitmap_after = output.after[bitmap_index];
        bitmap_after[new_local / 8] |= u8(1U << (new_local % 8));
        auto descriptor_after = std::span<u8>{output.after[descriptor_index]}.subspan(source.group * 32, 32);
        store16(descriptor_after, 14, u16(selected.free_inodes - 1));
        const u32 unused_start = geometry.inodes_per_group - selected.unused_inodes;
        if (new_local >= unused_start)
            store16(descriptor_after, 28, u16(geometry.inodes_per_group - new_local - 1));
        store16(descriptor_after, 26, u16(jbd2::detail::crc32c(
            geometry.seed, std::span<const u8>{bitmap_after}.first(geometry.inodes_per_group / 8))));
        store16(descriptor_after, 30, 0);
        store16(descriptor_after, 30, descriptor_checksum(geometry.seed, source.group, descriptor_after));
        auto &super_after = output.after[super_index];
        store32(super_after, 16, le32(source.superblock, 16) - 1);
        store32(super_after, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super_after}.first(1020)));
        auto &directory_after = output.after[directory_index];
        if (split)
        {
            const usize previous = slot - split;
            const u16 old_record = le16(source.directory, previous + 4);
            store16(directory_after, previous + 4, u16(split));
            store16(directory_after, slot + 4, u16(old_record - split));
        }
        store32(directory_after, slot, source.inode);
        if (!split) store16(directory_after, slot + 4, le16(source.directory, slot + 4));
        directory_after[slot + 6] = u8(update.name.size());
        directory_after[slot + 7] = 1;
        for (usize index = 0; index < update.name.size(); ++index)
            directory_after[slot + 8 + index] = u8(update.name[index]);
        store32(directory_after, tail + 8, jbd2::detail::crc32c(
            inode_seed(geometry.seed, update.parent, parent_after),
            std::span<const u8>{directory_after}.first(tail)));
        output.inode = source.inode; output.parent = update.parent; output.directory_block = source.directory_block;
        output.allocated_directory_block = output.block_group = 0;
        return {};
    }
};

struct Ext4CreateWitness
{
    Ext4CreateUpdate update{};
    u32 inode{};
    usize count{}, runs{}, bytes{};
};

// Private reversible descriptor padding, protected by the JBD2 descriptor CRC.
// Exact preimages survive destruction of free slots, parent times and counters.
// These image bytes grant no device/session provenance or recovery authority.
class Ext4CreateWitnessCodec final
{
    using Block = Ext4CreateBlock;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'C', 'R', 'W', '0', '0', '1'};
    static void store16(Block &block, usize at, u16 value)
    { block[at] = u8(value); block[at + 1] = u8(value >> 8); }
    static void store32(Block &block, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) block[at + i] = u8(value >> (8 * i)); }
  public:
    static constexpr usize begin = 128, patch_begin = 232, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize i = 0; i < magic.size(); ++i)
            if (descriptor[begin + i] != magic[i]) return false;
        return true;
    }
    // Leaves descriptor unchanged on any error; source images must not alias it.
    [[nodiscard]] static Status encode(const Ext4CreateUpdate &update,
                                      const Ext4CreatePlan &plan, Block &descriptor)
    {
        if ((plan.count != 5 && plan.count != 6) || update.parent != plan.parent ||
            update.attributes.type != NodeType::regular || update.name.empty() || update.name.size() > 31)
            return Status::invalid_argument("create witness plan or name");
        Block proposed = descriptor;
        for (usize at = begin; at < end; ++at) proposed[at] = 0;
        for (usize i = 0; i < magic.size(); ++i) proposed[begin + i] = magic[i];
        store16(proposed, 136, 1);
        proposed[140] = u8(plan.count); proposed[141] = u8(update.name.size());
        store32(proposed, 144, update.parent); store32(proposed, 148, plan.inode);
        store32(proposed, 152, update.seconds); store32(proposed, 156, update.nanoseconds);
        store32(proposed, 160, update.attributes.mode); store32(proposed, 164, update.attributes.uid);
        store32(proposed, 168, update.attributes.gid);
        for (usize i = 0; i < update.name.size(); ++i) proposed[200 + i] = u8(update.name[i]);
        usize cursor = patch_begin, runs = 0;
        for (usize home = 0; home < plan.count; ++home)
        {
            store32(proposed, 172 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            for (usize at = 0; at < 1024;)
            {
                if (plan.before[home][at] == plan.after[home][at]) { ++at; continue; }
                const usize first = at;
                while (at < 1024 && plan.before[home][at] != plan.after[home][at]) ++at;
                const usize length = at - first;
                if (cursor > end || end - cursor < 5 || length > end - cursor - 5)
                    return Status::unsupported("create complete before witness exceeds descriptor capacity");
                proposed[cursor] = u8(home);
                store16(proposed, cursor + 1, u16(first)); store16(proposed, cursor + 3, u16(length));
                cursor += 5;
                for (usize i = 0; i < length; ++i) proposed[cursor++] = plan.before[home][first + i];
                ++runs;
            }
        }
        store16(proposed, 138, u16(cursor)); store16(proposed, 142, u16(runs));
        descriptor = proposed;
        return {};
    }
    // Output name borrows descriptor storage. All input/output images must be
    // disjoint and descriptor must remain immutable while the name is used.
    [[nodiscard]] static Status decode(const Block &descriptor, std::span<const Block> after,
                                      std::span<Block> before, Ext4CreateWitness &output)
    {
        using namespace ext4_checked;
        output = {};
        if (!present(descriptor) || le16(descriptor, 136) != 1)
            return Status::unsupported("create durable witness absent or version unknown");
        const usize count = descriptor[140], length = descriptor[141];
        const usize limit = le16(descriptor, 138), runs = le16(descriptor, 142);
        if ((count != 5 && count != 6) || after.size() != count || before.size() != count ||
            !length || length > 31 || limit < patch_begin || limit > end)
            return Status::invalid_argument("create witness header geometry");
        for (usize at = limit; at < end; ++at)
            if (descriptor[at]) return Status::io_error("create witness trailing bytes");
        for (usize at = 172 + count * 4; at < 200; ++at)
            if (descriptor[at]) return Status::io_error("create witness unused preimage CRC");
        for (usize at = 200 + length; at < patch_begin; ++at)
            if (descriptor[at]) return Status::io_error("create witness name padding");
        for (usize home = 0; home < count; ++home) before[home] = after[home];
        usize cursor = patch_begin, prior_home = 0, prior_end = 0;
        for (usize run = 0; run < runs; ++run)
        {
            if (cursor > limit || limit - cursor < 5)
                return Status::io_error("create witness truncated patch");
            const usize home = descriptor[cursor], offset = le16(descriptor, cursor + 1);
            const usize size = le16(descriptor, cursor + 3);
            cursor += 5;
            if (home >= count || offset >= 1024 || !size || size > 1024 - offset ||
                size > limit - cursor ||
                (run && (home < prior_home || (home == prior_home && offset <= prior_end))))
                return Status::io_error("create witness patch bounds or canonical order");
            for (usize i = 0; i < size; ++i)
            {
                if (descriptor[cursor + i] == after[home][offset + i])
                    return Status::io_error("create witness patch includes unchanged byte");
                before[home][offset + i] = descriptor[cursor + i];
            }
            cursor += size; prior_home = home; prior_end = offset + size;
        }
        if (cursor != limit) return Status::io_error("create witness patch length mismatch");
        for (usize home = 0; home < count; ++home)
            if (le32(descriptor, 172 + home * 4) != jbd2::detail::crc32c(~u32{0}, before[home]))
                return Status::io_error("create witnessed preimage checksum");
        output.update = {le32(descriptor, 144), le32(descriptor, 152), le32(descriptor, 156),
            {reinterpret_cast<const char *>(descriptor.data() + 200), length},
            {NodeType::regular, le32(descriptor, 160), le32(descriptor, 164), le32(descriptor, 168)}};
        output.inode = le32(descriptor, 148); output.count = count;
        output.runs = runs; output.bytes = limit - begin;
        return {};
    }
};

// One exclusive old view supplies inode allocation, parent namespace, and the
// first supported free block. The owning transaction proves no allocated
// inode references physical before it can publish any image.
struct Ext4DirectoryCreateSource
{
    const Ext4CreateSource &inode;
    u32 block_group{}, physical{};
    const Ext4CreateBlock &block_bitmap;
    const Ext4CreateBlock &data;
};

// Pure planning for a single-block ordinary directory. Reuse the empty-create
// inode/namespace checks, then add checked block allocation, dot entries, link
// counts and used-directory accounting. No journal or device authority lives
// here. Nonzero old free-block bytes are refused by this bounded successor.
class Ext4DirectoryCreatePlanner final
{
    using Block = Ext4CreateBlock;
    static constexpr void store16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    static constexpr void store32(std::span<u8> bytes, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) bytes[at + i] = u8(value >> (8 * i)); }
    static void stamp_inode(std::span<u8> inode, u32 number, u32 seed)
    {
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(crc)); store16(inode, 130, u16(crc >> 16));
    }
    static void stamp_descriptor(std::span<u8> descriptor, u32 group, u32 seed)
    {
        store16(descriptor, 30, 0);
        store16(descriptor, 30, u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)), descriptor)));
    }

  public:
    // Read-only candidate admission never constructs a directory witness or
    // claims a zero preimage. Its images are the exact old participants; the
    // owner may stage them to reserve cache identity before initialization.
    [[nodiscard]] static Status admit_candidate(const Ext4RecoveryInfo &binding,
        const Ext4CreateUpdate &update, const Ext4DirectoryCreateSource &source, Ext4CreatePlan &output)
    {
        using namespace ext4_checked;
        output.count = 0;
        if (update.attributes.type != NodeType::directory || (update.attributes.mode & ~u32{07777}))
            return Status::unsupported("directory candidate type or permission profile");
        auto regular = update;
        regular.attributes.type = NodeType::regular; regular.attributes.mode &= 0777;
        Ext4CreatePlan admitted{};
        if (auto status = Ext4CreatePlanner::plan(binding, regular, source.inode, admitted); !status)
            return status;
        const auto selected = Ext4FirstDataPlanner::select_block(binding, source.inode.superblock,
            source.inode.descriptors, source.block_bitmap, source.block_group);
        if (!selected) return selected.status();
        if (selected.value() != source.physical)
            return Status::invalid_argument("directory candidate is not first supported free block");
        const u32 per_group = le32(source.inode.superblock, 40);
        const usize parent_offset = ((update.parent - 1) % per_group % 4) * 256;
        if (le16(source.inode.parent_table, parent_offset + 26) == 0xffff ||
            le16(source.inode.descriptors, source.inode.group * 32 + 16) == 0xffff)
            return Status::overflow("directory candidate link or used-directory count exhausted");
        const u32 bitmap = le32(source.inode.descriptors, source.block_group * 32);
        for (usize index = 0; index < admitted.count; ++index)
            if (admitted.targets[index] == bitmap || admitted.targets[index] == source.physical)
                return Status::unsupported("directory candidate aliases an existing participant");
        if (bitmap == source.physical || admitted.count + 2 > admitted.targets.size())
            return Status::unsupported("directory candidate participant capacity or alias");
        const usize namespace_index = admitted.count - 1;
        const u32 namespace_block = admitted.targets[namespace_index];
        const Block namespace_before = admitted.before[namespace_index];
        admitted.targets[namespace_index] = bitmap;
        admitted.before[namespace_index] = source.block_bitmap;
        admitted.targets[admitted.count] = source.physical;
        admitted.before[admitted.count] = source.data;
        admitted.targets[admitted.count + 1] = namespace_block;
        admitted.before[admitted.count + 1] = namespace_before;
        admitted.count += 2;
        admitted.after = admitted.before;
        admitted.allocated_directory_block = source.physical;
        admitted.block_group = source.block_group;
        output = admitted;
        return {};
    }

    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding, const Ext4CreateUpdate &update,
                                     const Ext4DirectoryCreateSource &source, Ext4CreatePlan &output)
    {
        using namespace ext4_checked;
        output.count = 0;
        if (update.attributes.type != NodeType::directory)
            return Status::unsupported("bounded mkdir requires a directory");
        if (update.attributes.mode & ~u32{07777})
            return Status::invalid_argument("bounded mkdir permission bits");
        auto regular = update;
        regular.attributes.type = NodeType::regular;
        regular.attributes.mode &= 0777;
        Ext4CreatePlan prepared{};
        if (auto status = Ext4CreatePlanner::plan(binding, regular, source.inode, prepared); !status)
            return status;
        const auto block = Ext4FirstDataPlanner::select_block(binding, source.inode.superblock,
            source.inode.descriptors, source.block_bitmap, source.block_group);
        if (!block) return block.status();
        if (block.value() != source.physical)
            return Status::invalid_argument("bounded mkdir block is not first supported free block");
        if (!jbd2::detail::all_zero(source.data))
            return Status::unsupported("bounded mkdir requires zeroed free block preimage");
        const u32 per_group = le32(source.inode.superblock, 40);
        const u32 blocks_per_group = le32(source.inode.superblock, 32);
        const usize parent_offset = ((update.parent - 1) % per_group % 4) * 256;
        const usize new_offset = ((source.inode.inode - 1) % per_group % 4) * 256;
        const auto old_parent = std::span<const u8>{source.inode.parent_table}.subspan(parent_offset, 256);
        if (le16(old_parent, 26) == 0xffff)
            return Status::overflow("bounded mkdir parent link count exhausted");
        const auto old_inode_descriptor = std::span<const u8>{source.inode.descriptors}.subspan(source.inode.group * 32, 32);
        if (le16(old_inode_descriptor, 16) == 0xffff)
            return Status::overflow("bounded mkdir used-directory count exhausted");
        const auto old_block_descriptor = std::span<const u8>{source.inode.descriptors}.subspan(source.block_group * 32, 32);
        const u32 bitmap_block = le32(old_block_descriptor, 0);
        for (usize i = 0; i < prepared.count; ++i)
            if (prepared.targets[i] == bitmap_block || prepared.targets[i] == source.physical)
                return Status::unsupported("bounded mkdir new participants alias inode or namespace images");
        if (bitmap_block == source.physical || prepared.count + 2 > prepared.targets.size())
            return Status::unsupported("bounded mkdir participant capacity or alias");
        const usize directory_index = prepared.count - 1;
        const Block parent_directory_before = prepared.before[directory_index];
        const Block parent_directory_after = prepared.after[directory_index];
        const u32 parent_directory_block = prepared.targets[directory_index];
        prepared.targets[directory_index] = bitmap_block;
        prepared.before[directory_index] = source.block_bitmap;
        prepared.after[directory_index] = source.block_bitmap;
        const usize data_index = prepared.count;
        prepared.targets[data_index] = source.physical;
        prepared.before[data_index] = source.data;
        prepared.after[data_index] = {};
        prepared.targets[data_index + 1] = parent_directory_block;
        prepared.before[data_index + 1] = parent_directory_before;
        prepared.after[data_index + 1] = parent_directory_after;
        prepared.count += 2;
        const usize parent_index = 0;
        const usize new_index = source.inode.parent_table_block == source.inode.new_table_block ? 0 : 1;
        const usize descriptor_index = new_index + 2;
        const usize super_index = new_index + 3;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        auto parent_after = std::span<u8>{prepared.after[parent_index]}.subspan(parent_offset, 256);
        store16(parent_after, 26, u16(le16(old_parent, 26) + 1));
        stamp_inode(parent_after, update.parent, seed);
        auto new_after = std::span<u8>{prepared.after[new_index]}.subspan(new_offset, 256);
        store16(new_after, 0, u16(0040000 | update.attributes.mode));
        store16(new_after, 26, 2);
        store32(new_after, 4, 1024); store32(new_after, 28, 2);
        store16(new_after, 42, 1); store16(new_after, 56, 1);
        store32(new_after, 60, source.physical);
        stamp_inode(new_after, source.inode.inode, seed);
        auto &bitmap_after = prepared.after[directory_index];
        const u32 local = source.physical - (1 + source.block_group * blocks_per_group);
        bitmap_after[local / 8] |= u8(1U << (local % 8));
        auto block_descriptor = std::span<u8>{prepared.after[descriptor_index]}.subspan(source.block_group * 32, 32);
        store16(block_descriptor, 12, u16(le16(old_block_descriptor, 12) - 1));
        store16(block_descriptor, 24, u16(jbd2::detail::crc32c(seed,
            std::span<const u8>{bitmap_after}.first(blocks_per_group / 8))));
        auto inode_descriptor = std::span<u8>{prepared.after[descriptor_index]}.subspan(source.inode.group * 32, 32);
        store16(inode_descriptor, 16, u16(le16(old_inode_descriptor, 16) + 1));
        stamp_descriptor(block_descriptor, source.block_group, seed);
        if (source.block_group != source.inode.group)
            stamp_descriptor(inode_descriptor, source.inode.group, seed);
        auto &super_after = prepared.after[super_index];
        store32(super_after, 12, le32(source.inode.superblock, 12) - 1);
        store32(super_after, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super_after}.first(1020)));
        auto &data_after = prepared.after[data_index];
        store32(data_after, 0, source.inode.inode); store16(data_after, 4, 12);
        data_after[6] = 1; data_after[7] = 2; data_after[8] = '.';
        store32(data_after, 12, update.parent); store16(data_after, 16, 1000);
        data_after[18] = 2; data_after[19] = 2; data_after[20] = data_after[21] = '.';
        store16(data_after, 1016, 12); data_after[1019] = 0xde;
        store32(data_after, 1020, jbd2::detail::crc32c(
            inode_seed(seed, source.inode.inode, new_after), std::span<const u8>{data_after}.first(1012)));
        auto &parent_directory = prepared.after[prepared.count - 1];
        bool inserted = false;
        for (usize offset = 0; offset < 1012; offset += le16(parent_directory, offset + 4))
            if (le32(parent_directory, offset) == source.inode.inode)
            { parent_directory[offset + 7] = 2; inserted = true; }
        if (!inserted) return Status::fault("bounded mkdir canonical insertion absent");
        store32(parent_directory, 1020, jbd2::detail::crc32c(
            inode_seed(seed, update.parent, parent_after), std::span<const u8>{parent_directory}.first(1012)));
        prepared.allocated_directory_block = source.physical;
        prepared.block_group = source.block_group;
        output = prepared;
        return {};
    }
};

struct Ext4DirectoryCreateWitness
{
    Ext4CreateUpdate update{};
    u32 inode{}, physical{};
    usize count{}, runs{}, bytes{};
};

// Private reversible descriptor padding, protected by the JBD2 descriptor CRC.
// Exact preimages survive destruction of free slots, parent times and counters.
// These image bytes grant no device/session provenance or recovery authority.
class Ext4DirectoryCreateWitnessCodec final
{
    using Block = Ext4CreateBlock;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'D', 'R', 'W', '0', '0', '1'};
    static void store16(Block &block, usize at, u16 value)
    { block[at] = u8(value); block[at + 1] = u8(value >> 8); }
    static void store32(Block &block, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) block[at + i] = u8(value >> (8 * i)); }
  public:
    static constexpr usize begin = 160, patch_begin = 280, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize i = 0; i < magic.size(); ++i)
            if (descriptor[begin + i] != magic[i]) return false;
        return true;
    }
    // Leaves descriptor unchanged on any error; source images must not alias it.
    [[nodiscard]] static Status encode(const Ext4CreateUpdate &update,
                                      const Ext4CreatePlan &plan, Block &descriptor)
    {
        if ((plan.count != 7 && plan.count != 8) || update.parent != plan.parent ||
            update.attributes.type != NodeType::directory || update.name.empty() || update.name.size() > 31)
            return Status::invalid_argument("mkdir witness plan or name");
        Block proposed = descriptor;
        const usize tag_end = 44 + (plan.count - 1) * 16;
        for (usize at = tag_end; at < begin; ++at) proposed[at] = 0;
        for (usize at = begin; at < end; ++at) proposed[at] = 0;
        for (usize i = 0; i < magic.size(); ++i) proposed[begin + i] = magic[i];
        store16(proposed, 168, 1);
        proposed[172] = u8(plan.count); proposed[173] = u8(update.name.size());
        store32(proposed, 176, update.parent); store32(proposed, 180, plan.inode);
        store32(proposed, 184, update.seconds); store32(proposed, 188, update.nanoseconds);
        store32(proposed, 192, update.attributes.mode); store32(proposed, 196, update.attributes.uid);
        store32(proposed, 200, update.attributes.gid);
        const u32 physical = plan.targets[plan.count - 2];
        store32(proposed, 272, physical);
        for (usize i = 0; i < update.name.size(); ++i) proposed[240 + i] = u8(update.name[i]);
        usize cursor = patch_begin, runs = 0;
        for (usize home = 0; home < plan.count; ++home)
        {
            store32(proposed, 204 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            for (usize at = 0; at < 1024;)
            {
                if (plan.before[home][at] == plan.after[home][at]) { ++at; continue; }
                const usize first = at;
                while (at < 1024 && plan.before[home][at] != plan.after[home][at]) ++at;
                const usize length = at - first;
                if (cursor > end || end - cursor < 5 || length > end - cursor - 5)
                    return Status::unsupported("mkdir complete before witness exceeds descriptor capacity");
                proposed[cursor] = u8(home);
                store16(proposed, cursor + 1, u16(first)); store16(proposed, cursor + 3, u16(length));
                cursor += 5;
                for (usize i = 0; i < length; ++i) proposed[cursor++] = plan.before[home][first + i];
                ++runs;
            }
        }
        store16(proposed, 170, u16(cursor)); store16(proposed, 174, u16(runs));
        descriptor = proposed;
        return {};
    }
    // Output name borrows descriptor storage. All input/output images must be
    // disjoint and descriptor must remain immutable while the name is used.
    [[nodiscard]] static Status decode(const Block &descriptor, std::span<const Block> after,
                                      std::span<Block> before, Ext4DirectoryCreateWitness &output)
    {
        using namespace ext4_checked;
        output = {};
        if (!present(descriptor) || le16(descriptor, 168) != 1)
            return Status::unsupported("mkdir durable witness absent or version unknown");
        const usize count = descriptor[172], length = descriptor[173];
        const usize limit = le16(descriptor, 170), runs = le16(descriptor, 174);
        if ((count != 7 && count != 8) || after.size() != count || before.size() != count ||
            !length || length > 31 || limit < patch_begin || limit > end)
            return Status::invalid_argument("mkdir witness header geometry");
        for (usize at = 44 + (count - 1) * 16; at < begin; ++at)
            if (descriptor[at]) return Status::io_error("mkdir witness tag padding");
        for (usize at = limit; at < end; ++at)
            if (descriptor[at]) return Status::io_error("mkdir witness trailing bytes");
        for (usize at = 204 + count * 4; at < 240; ++at)
            if (descriptor[at]) return Status::io_error("mkdir witness unused preimage CRC");
        for (usize at = 240 + length; at < 272; ++at)
            if (descriptor[at]) return Status::io_error("mkdir witness name padding");
        for (usize at = 276; at < patch_begin; ++at)
            if (descriptor[at]) return Status::io_error("mkdir witness reserved bytes");
        for (usize home = 0; home < count; ++home) before[home] = after[home];
        usize cursor = patch_begin, prior_home = 0, prior_end = 0;
        for (usize run = 0; run < runs; ++run)
        {
            if (cursor > limit || limit - cursor < 5)
                return Status::io_error("mkdir witness truncated patch");
            const usize home = descriptor[cursor], offset = le16(descriptor, cursor + 1);
            const usize size = le16(descriptor, cursor + 3);
            cursor += 5;
            if (home >= count || offset >= 1024 || !size || size > 1024 - offset ||
                size > limit - cursor ||
                (run && (home < prior_home || (home == prior_home && offset <= prior_end))))
                return Status::io_error("mkdir witness patch bounds or canonical order");
            for (usize i = 0; i < size; ++i)
            {
                if (descriptor[cursor + i] == after[home][offset + i])
                    return Status::io_error("mkdir witness patch includes unchanged byte");
                before[home][offset + i] = descriptor[cursor + i];
            }
            cursor += size; prior_home = home; prior_end = offset + size;
        }
        if (cursor != limit) return Status::io_error("mkdir witness patch length mismatch");
        for (usize home = 0; home < count; ++home)
            if (le32(descriptor, 204 + home * 4) != jbd2::detail::crc32c(~u32{0}, before[home]))
                return Status::io_error("mkdir witnessed preimage checksum");
        output.update = {le32(descriptor, 176), le32(descriptor, 184), le32(descriptor, 188),
            {reinterpret_cast<const char *>(descriptor.data() + 240), length},
            {NodeType::directory, le32(descriptor, 192), le32(descriptor, 196), le32(descriptor, 200)}};
        output.inode = le32(descriptor, 180); output.physical = le32(descriptor, 272); output.count = count;
        output.runs = runs; output.bytes = limit - begin;
        return {};
    }
};

using Ext4OrphanBlock = std::array<u8, 1024>;

struct Ext4OrphanInodeImage
{
    u32 number{}, expected_generation{}, table_block{};
    Ext4OrphanBlock table{};
};

// All images must come from one owner-exclusive, stable filesystem view. This
// borrowed input does not attest device provenance or authorize any write.
struct Ext4OrphanChainSource
{
    const Ext4OrphanBlock &superblock;
    const Ext4OrphanBlock &descriptors;
    const std::array<Ext4OrphanBlock, 4> &inode_bitmaps;
    const std::array<Ext4OrphanBlock, 4> &block_bitmaps;
    std::span<const Ext4OrphanInodeImage> chain{};
};

struct Ext4OrphanMember
{
    u32 number{}, generation{}, size{}, next{};
    std::array<u32, 16> data_blocks{};
    u32 data_count{};
};

// A model proof about supplied bytes, not a pathname decoder, mount admission,
// replay permission, reclaim proof or ownership proof against other inodes.
class Ext4OrphanChainProof final
{
  public:
    static constexpr usize capacity = 8;
    [[nodiscard]] usize count() const { return count_; }
    [[nodiscard]] const std::array<u8, 16> &filesystem_uuid() const { return filesystem_uuid_; }
    [[nodiscard]] u32 observed_head() const { return observed_head_; }

    [[nodiscard]] const Ext4OrphanMember *member(u32 number, u32 generation) const
    {
        for (usize index = 0; index < count_; ++index)
            if (members_[index].number == number && members_[index].generation == generation)
                return &members_[index];
        return nullptr;
    }
    [[nodiscard]] const Ext4OrphanMember *at(usize index) const
    { return index < count_ ? &members_[index] : nullptr; }

  private:
    friend class Ext4OrphanChainValidator;
    std::array<Ext4OrphanMember, capacity> members_{};
    usize count_{};
    std::array<u8, 16> filesystem_uuid_{};
    u32 observed_head_{};
};

class Ext4OrphanChainValidator final
{
    using Block = Ext4OrphanBlock;
    struct Geometry
    {
        u32 seed{};
        std::array<u32, 4> block_bitmap{}, inode_bitmap{}, table{};
        std::array<u16, 4> flags{}, block_checksum{}, inode_checksum{};
    };

    [[nodiscard]] static u16 descriptor_checksum(u32 seed, u32 group, std::span<const u8> descriptor)
    {
        std::array<u8, 32> copy{};
        for (usize index = 0; index < copy.size(); ++index) copy[index] = descriptor[index];
        copy[30] = copy[31] = 0;
        return u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)), copy));
    }
    [[nodiscard]] static bool ordinary_block(const Ext4RecoveryInfo &binding, u32 block)
    {
        if (block < 3 || block >= binding.layout.filesystem_blocks) return false;
        if (block >= binding.layout.journal_first_block &&
            u64{block} - binding.layout.journal_first_block < binding.layout.journal_blocks) return false;
        for (usize index = 0; index < binding.metadata_count; ++index)
        {
            const auto range = binding.metadata[index];
            if (block >= range.first && u64{block} - range.first < range.count) return false;
        }
        return true;
    }
    [[nodiscard]] static Status inspect(const Ext4RecoveryInfo &binding,
                                        const Ext4OrphanChainSource &source,
                                        Geometry &geometry)
    {
        using namespace ext4_checked;
        const auto s = std::span<const u8>{source.superblock};
        if (binding.layout.block_size != 1024 || binding.layout.filesystem_blocks != 32768 ||
            binding.journal_inode != 8 || binding.layout.journal_blocks < 1024 ||
            binding.metadata_count != 16 || !binding.structural_metadata().valid() ||
            binding.layout.journal_first_block < 3 ||
            binding.layout.journal_first_block >= binding.layout.filesystem_blocks ||
            binding.layout.journal_blocks >
                binding.layout.filesystem_blocks - binding.layout.journal_first_block)
            return Status::unsupported("orphan chain bounded binding");
        if (jbd2::detail::crc32c(~u32{0}, s.first(1020)) != le32(s, 1020))
            return Status::io_error("orphan chain superblock checksum");
        if (le16(s, 56) != 0xef53 || le32(s, 0) != 8192 || le32(s, 4) != 32768 ||
            le32(s, 20) != 1 || le32(s, 24) || le32(s, 32) != 8192 ||
            le32(s, 40) != 2048 || le32(s, 84) < 11 || le16(s, 88) != 256 ||
            le32(s, 92) != 4 ||
            le32(s, 96) != (binding.needs_recovery ? 0x46U : 0x42U) ||
            le32(s, 100) != 0x400 || le32(s, 224) != 8 || s[373] != 1)
            return Status::unsupported("orphan chain superblock profile");
        for (usize index = 0; index < binding.uuid.size(); ++index)
            if (s[104 + index] != binding.uuid[index])
                return Status::io_error("orphan chain UUID differs");
        geometry.seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        u32 free_inodes = 0;
        for (u32 group = 0; group < 4; ++group)
        {
            const auto d = std::span<const u8>{source.descriptors}.subspan(group * 32, 32);
            if (descriptor_checksum(geometry.seed, group, d) != le16(d, 30))
                return Status::io_error("orphan chain descriptor checksum");
            const u32 begin = 1 + group * 8192;
            const u32 blocks = group == 3 ? 8191 : 8192;
            const u32 block_bitmap = le32(d, 0), inode_bitmap = le32(d, 4), table = le32(d, 8);
            const std::array<jbd2::BlockRange, 4> expected{{
                {begin, 2}, {block_bitmap, 1}, {inode_bitmap, 1}, {table, 512}}};
            for (usize index = 0; index < expected.size(); ++index)
                if (binding.metadata[group * 4 + index].first != expected[index].first ||
                    binding.metadata[group * 4 + index].count != expected[index].count)
                    return Status::unsupported("orphan chain descriptor placement");
            geometry.block_bitmap[group] = block_bitmap;
            geometry.inode_bitmap[group] = inode_bitmap;
            geometry.table[group] = table;
            geometry.flags[group] = le16(d, 18);
            geometry.block_checksum[group] = le16(d, 24);
            geometry.inode_checksum[group] = le16(d, 26);
            if ((geometry.flags[group] & ~u16{7}) || le16(d, 12) > blocks ||
                le16(d, 14) > 2048 || le16(d, 16) > 2048 - le16(d, 14) ||
                le16(d, 28) > 2048 ||
                ((geometry.flags[group] & 1) &&
                 (le16(d, 14) != 2048 || le16(d, 16) || le16(d, 28) != 2048 ||
                  geometry.inode_checksum[group])) ||
                ((geometry.flags[group] & 2) &&
                 (le16(d, 12) != blocks - 516 || geometry.block_checksum[group])))
                return Status::io_error("orphan chain descriptor counters or flags");
            free_inodes += le16(d, 14);
        }
        if (free_inodes != le32(s, 16))
            return Status::io_error("orphan chain free inode counters differ");
        return {};
    }
    [[nodiscard]] static Status decode(const Ext4RecoveryInfo &binding,
                                       const Ext4OrphanChainSource &source,
                                       const Geometry &geometry,
                                       const Ext4OrphanInodeImage &image,
                                       Ext4OrphanMember &member)
    {
        using namespace ext4_checked;
        const u32 number = image.number;
        if (number < 11 || number > 8192)
            return Status::invalid_argument("orphan chain inode number");
        const u32 group = (number - 1) / 2048, local = (number - 1) % 2048;
        if (geometry.flags[group] & 1)
            return Status::unsupported("orphan chain inode group uninitialized");
        if (image.table_block != geometry.table[group] + local / 4)
            return Status::io_error("orphan chain table placement");
        const auto bitmap = std::span<const u8>{source.inode_bitmaps[group]}.first(256);
        if (u16(jbd2::detail::crc32c(geometry.seed, bitmap)) != geometry.inode_checksum[group])
            return Status::io_error("orphan chain inode bitmap checksum");
        if (!(bitmap[local / 8] & (1U << (local % 8))))
            return Status::io_error("orphan chain inode is not allocated");
        const auto inode = std::span<const u8>{image.table}.subspan((local % 4) * 256, 256);
        if (auto status = inode_checksum(geometry.seed, number, inode); !status) return status;
        if (le32(inode, 100) != image.expected_generation)
            return Status::io_error("orphan chain inode generation differs");
        const u32 size = le32(inode, 4);
        if ((le16(inode, 0) & 0170000) != 0100000 || (le16(inode, 0) & 07000) ||
            le16(inode, 26) || size > 16384 || le32(inode, 32) != 0x80000 ||
            le16(inode, 40) != 0xf30a || le16(inode, 42) > 4 ||
            le16(inode, 44) != 4 || le16(inode, 46) || le32(inode, 48) ||
            le32(inode, 104) || le32(inode, 108) || le32(inode, 112) ||
            le16(inode, 116) || le16(inode, 118) || le16(inode, 126) ||
            le32(inode, 156) || le32(inode, 8) > 0x7fffffffU ||
            le32(inode, 12) > 0x7fffffffU || le32(inode, 16) > 0x7fffffffU ||
            (le32(inode, 140) >> 2) >= 1000000000U ||
            (le32(inode, 132) >> 2) >= 1000000000U ||
            (le32(inode, 136) >> 2) >= 1000000000U)
            return Status::unsupported("orphan chain regular inode profile");
        for (usize at = 160; at < inode.size(); ++at)
            if (inode[at]) return Status::unsupported("orphan chain inode extra fields");
        const u32 expected_blocks = u32((u64{size} + 1023) / 1024);
        if (le32(inode, 28) != expected_blocks * 2 || (!size && le16(inode, 42)))
            return Status::unsupported("orphan chain inode sectors or empty extents");
        for (usize at = 52 + le16(inode, 42) * 12; at < 100; ++at)
            if (inode[at]) return Status::unsupported("orphan chain unused extent bytes");
        member = {number, image.expected_generation, size, le32(inode, 20), {}, 0};
        for (u32 record = 0; record < le16(inode, 42); ++record)
        {
            const usize at = 52 + record * 12;
            const u32 logical = le32(inode, at), length = le16(inode, at + 4);
            const u32 physical = le32(inode, at + 8);
            if (!length || length > expected_blocks - member.data_count ||
                logical != member.data_count || le16(inode, at + 6) ||
                !physical || physical > 32768 - length)
                return Status::unsupported("orphan chain requires initialized inline extents");
            for (u32 part = 0; part < length; ++part)
            {
                const u32 block = physical + part;
                if (!ordinary_block(binding, block))
                    return Status::unsupported("orphan chain data block is reserved");
                const u32 data_group = (block - 1) / 8192, local_block = (block - 1) % 8192;
                if (geometry.flags[data_group] & 2)
                    return Status::unsupported("orphan chain data group uninitialized");
                const auto data_bitmap = std::span<const u8>{source.block_bitmaps[data_group]};
                if (u16(jbd2::detail::crc32c(geometry.seed, data_bitmap)) !=
                    geometry.block_checksum[data_group])
                    return Status::io_error("orphan chain block bitmap checksum");
                if (!(data_bitmap[local_block / 8] & (1U << (local_block % 8))))
                    return Status::io_error("orphan chain data block is not allocated");
                for (u32 prior = 0; prior < member.data_count; ++prior)
                    if (member.data_blocks[prior] == block)
                        return Status::io_error("orphan chain extent aliases itself");
                member.data_blocks[member.data_count++] = block;
            }
        }
        if (member.data_count != expected_blocks)
            return Status::unsupported("orphan chain extent coverage");
        return {};
    }

  public:
    [[nodiscard]] static Status validate(const Ext4RecoveryInfo &binding,
                                         const Ext4OrphanChainSource &source,
                                         Ext4OrphanChainProof &output)
    {
        output = {};
        if (source.chain.size() > Ext4OrphanChainProof::capacity)
            return Status::unsupported("orphan chain exceeds bounded capacity");
        Geometry geometry{};
        if (auto status = inspect(binding, source, geometry); !status) return status;
        const u32 head = ext4_checked::le32(source.superblock, 232);
        if ((head == 0) != source.chain.empty())
            return Status::io_error("orphan chain head and supplied images differ");
        Ext4OrphanChainProof proposed{};
        proposed.filesystem_uuid_ = binding.uuid;
        proposed.observed_head_ = head;
        u32 cursor = head;
        for (usize index = 0; index < source.chain.size(); ++index)
        {
            const auto &image = source.chain[index];
            if (!cursor || image.number != cursor)
                return Status::io_error("orphan chain order or early terminator");
            for (usize prior = 0; prior < index; ++prior)
            {
                if (source.chain[prior].number == image.number)
                    return Status::io_error("orphan chain cycle");
                if (source.chain[prior].table_block == image.table_block &&
                    source.chain[prior].table != image.table)
                    return Status::io_error("orphan chain aliased table views differ");
            }
            if (auto status = decode(binding, source, geometry, image,
                                     proposed.members_[index]); !status) return status;
            for (usize earlier = 0; earlier < index; ++earlier)
                for (u32 one = 0; one < proposed.members_[earlier].data_count; ++one)
                    for (u32 two = 0; two < proposed.members_[index].data_count; ++two)
                        if (proposed.members_[earlier].data_blocks[one] ==
                            proposed.members_[index].data_blocks[two])
                            return Status::io_error("orphan chain members share data");
            cursor = proposed.members_[index].next;
            ++proposed.count_;
        }
        if (cursor)
            return Status::io_error("orphan chain unprovided successor or cycle");
        output = proposed;
        return {};
    }
};

struct Ext4OrphanAccessKey
{
    std::array<u8, 16> filesystem_uuid{};
    u32 observed_head{}, number{}, generation{};
};

// A value copied from a checked, complete orphan-chain image. It does not
// retain a device, descriptor, mount, writer or reclamation authority.
class Ext4OrphanReadOnlySnapshot final
{
  public:
    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] const std::array<u8, 16> &filesystem_uuid() const { return filesystem_uuid_; }
    [[nodiscard]] u32 observed_head() const { return observed_head_; }
    [[nodiscard]] u32 number() const { return number_; }
    [[nodiscard]] u32 generation() const { return generation_; }
    [[nodiscard]] u32 size() const { return size_; }
    [[nodiscard]] u32 block_count() const { return block_count_; }

    [[nodiscard]] Result<u32> physical_block(u32 logical) const
    {
        if (!valid_) return Status::not_initialized("orphan snapshot is unavailable");
        if (logical >= block_count_) return Status::not_found("orphan logical block is absent");
        return data_blocks_[logical];
    }

  private:
    friend class Ext4OrphanReadOnlyDecoder;
    bool valid_{};
    std::array<u8, 16> filesystem_uuid_{};
    u32 observed_head_{}, number_{}, generation_{}, size_{}, block_count_{};
    std::array<u32, 16> data_blocks_{};
};

// The owner must supply one exclusive, stable old or replay-completed image
// epoch. This pure decoder checks supplied bytes; it cannot establish their
// device provenance, active-descriptor lifetime or global block ownership.
class Ext4OrphanReadOnlyDecoder final
{
  public:
    [[nodiscard]] static Status capture(const Ext4RecoveryInfo &binding,
                                        const Ext4OrphanChainSource &source,
                                        const Ext4OrphanAccessKey &key,
                                        Ext4OrphanReadOnlySnapshot &output)
    {
        output = {};
        if (!key.observed_head || key.number < 11 || key.number > 8192)
            return Status::invalid_argument("orphan access key bounds");
        if (key.filesystem_uuid != binding.uuid)
            return Status::io_error("orphan access filesystem UUID differs");
        Ext4OrphanChainProof proof{};
        if (auto status = Ext4OrphanChainValidator::validate(binding, source, proof); !status)
            return status;
        if (proof.observed_head() != key.observed_head ||
            proof.filesystem_uuid() != key.filesystem_uuid)
            return Status::io_error("orphan access chain identity differs");
        const auto *member = proof.member(key.number, key.generation);
        if (!member) return Status::not_found("orphan access member is absent");
        Ext4OrphanReadOnlySnapshot proposed{};
        proposed.filesystem_uuid_ = proof.filesystem_uuid();
        proposed.observed_head_ = proof.observed_head();
        proposed.number_ = member->number;
        proposed.generation_ = member->generation;
        proposed.size_ = member->size;
        proposed.block_count_ = member->data_count;
        proposed.data_blocks_ = member->data_blocks;
        proposed.valid_ = true;
        output = proposed;
        return {};
    }
};

// Initialized groups supply all 512 inode-table blocks in descriptor order.
// An uninitialized group supplies no table blocks and a zero inode bitmap.
struct Ext4OrphanOwnershipSource
{
    Ext4OrphanChainSource chain;
    std::array<std::span<const Ext4OrphanBlock>, 4> inode_tables{};
};

struct Ext4OrphanOwnershipScratch
{
    struct Range { u32 first{}, count{}, inode{}; };
    static constexpr usize inode_capacity = 128;
    static constexpr usize range_capacity = inode_capacity * 4;
    std::array<Range, range_capacity> ranges{};
    std::array<bool, Ext4OrphanChainProof::capacity> seen{};
    usize range_count{}, inode_count{};
};

// A result about one complete set of supplied images, not reclaim authority.
struct Ext4OrphanOwnershipProof
{
    std::array<u8, 16> filesystem_uuid{};
    u32 observed_head{};
    usize orphan_count{}, allocated_inodes{}, ordinary_ranges{}, owned_blocks{};
    bool valid{};
};

class Ext4OrphanOwnershipValidator final
{
    using Block = Ext4OrphanBlock;
    using Range = Ext4OrphanOwnershipScratch::Range;

    [[nodiscard]] static bool overlaps(u32 first, u32 count, u32 other, u32 length)
    {
        return first < u64{other} + length && other < u64{first} + count;
    }
    [[nodiscard]] static bool in_range(u32 block, u32 first, u32 count)
    { return block >= first && u64{block} - first < count; }
    [[nodiscard]] static bool metadata(const Ext4RecoveryInfo &binding, u32 block)
    {
        for (usize index = 0; index < binding.metadata_count; ++index)
            if (in_range(block, binding.metadata[index].first, binding.metadata[index].count))
                return true;
        return false;
    }
    [[nodiscard]] static bool journal(const Ext4RecoveryInfo &binding, u32 block)
    { return in_range(block, u32(binding.layout.journal_first_block), u32(binding.layout.journal_blocks)); }
    [[nodiscard]] static bool bit(const Block &bitmap, u32 local)
    { return bitmap[local / 8] & (1U << (local % 8)); }

    [[nodiscard]] static Status reserved(u32 seed, u32 number, std::span<const u8> inode)
    {
        if (jbd2::detail::all_zero(inode)) return {};
        for (usize at = 0; at < inode.size(); ++at)
            if (!(at >= 8 && at < 20) && at != 124 && at != 125 && inode[at])
                return Status::unsupported("orphan ownership opaque reserved inode");
        auto crc = ext4_checked::inode_seed(seed, number, inode);
        for (usize at = 0; at < inode.size(); ++at)
            crc = jbd2::detail::crc_byte(crc, at == 124 || at == 125 ? 0 : inode[at]);
        return u16(crc) == ext4_checked::le16(inode, 124) ? Status{} :
            Status::io_error("orphan ownership reserved inode checksum");
    }

    [[nodiscard]] static Status append(const Ext4RecoveryInfo &binding,
                                       const Ext4OrphanOwnershipSource &source,
                                       Ext4OrphanOwnershipScratch &scratch,
                                       u32 number, u32 first, u32 count)
    {
        if (!count || first < 1 || first >= binding.layout.filesystem_blocks ||
            count > binding.layout.filesystem_blocks - first)
            return Status::unsupported("orphan ownership extent bounds");
        for (u32 part = 0; part < count; ++part)
        {
            const u32 physical = first + part;
            if (metadata(binding, physical) || journal(binding, physical))
                return Status::io_error("orphan ownership extent aliases reserved block");
            const u32 group = (physical - 1) / 8192, local = (physical - 1) % 8192;
            if (ext4_checked::le16(source.chain.descriptors, group * 32 + 18) & 2)
                return Status::unsupported("orphan ownership extent in uninitialized block group");
            if (!bit(source.chain.block_bitmaps[group], local))
                return Status::io_error("orphan ownership extent block is unallocated");
        }
        for (usize prior = 0; prior < scratch.range_count; ++prior)
            if (overlaps(first, count, scratch.ranges[prior].first,
                         scratch.ranges[prior].count))
                return Status::io_error("orphan ownership physical extents overlap");
        if (scratch.range_count == scratch.ranges.size())
            return Status::no_memory("orphan ownership range budget");
        scratch.ranges[scratch.range_count++] = {first, count, number};
        return {};
    }

    [[nodiscard]] static Status journal_inode(const Ext4RecoveryInfo &binding, u32 seed,
                                               std::span<const u8> inode)
    {
        using namespace ext4_checked;
        if (auto status = inode_checksum(seed, 8, inode); !status) return status;
        const u32 count = u32(binding.layout.journal_blocks);
        if (count > 32768 || u64{count} * 1024 > 0xffffffffULL ||
            (le16(inode, 0) & 0170000) != 0100000 || le16(inode, 26) != 1 ||
            le32(inode, 4) != count * 1024 || le32(inode, 28) != count * 2 ||
            le32(inode, 32) != 0x80000 || le16(inode, 40) != 0xf30a ||
            le16(inode, 42) != 1 || le16(inode, 44) != 4 || le16(inode, 46) ||
            le32(inode, 52) || le16(inode, 56) != count || le16(inode, 58) ||
            le32(inode, 60) != binding.layout.journal_first_block ||
            le32(inode, 20) || le32(inode, 104) || le32(inode, 108) ||
            le32(inode, 112) || le16(inode, 116) || le16(inode, 118) ||
            !jbd2::detail::all_zero(inode.subspan(64, 36)))
            return Status::unsupported("orphan ownership journal inode mapping differs");
        return {};
    }

    [[nodiscard]] static Status ordinary_inode(const Ext4RecoveryInfo &binding,
                                               const Ext4OrphanOwnershipSource &source,
                                               const Ext4OrphanChainProof &chain,
                                               Ext4OrphanOwnershipScratch &scratch,
                                               u32 seed, u32 number,
                                               std::span<const u8> inode)
    {
        using namespace ext4_checked;
        if (auto status = inode_checksum(seed, number, inode); !status) return status;
        const u16 type = le16(inode, 0) & 0170000;
        const u32 size = le32(inode, 4), generation = le32(inode, 100);
        const auto *member = chain.member(number, generation);
        if (type == 0120000)
        {
            // Linked fast symlinks own only their checked inline target bytes.
            // They never become orphan members or contribute physical ranges.
            if (member || !le16(inode, 26) || le32(inode, 20) || !size || size >= 60 ||
                le32(inode, 28) || le32(inode, 32) || le32(inode, 104) ||
                le32(inode, 108) || le32(inode, 112) || le16(inode, 116) ||
                le16(inode, 118) || le16(inode, 126) || le32(inode, 156))
                return Status::unsupported("orphan ownership linked fast symlink profile");
            for (usize at = 40; at < 40 + size; ++at)
                if (!inode[at])
                    return Status::unsupported("orphan ownership fast symlink target contains zero");
            for (usize at = 40 + size; at < 100; ++at)
                if (inode[at])
                    return Status::unsupported("orphan ownership fast symlink unused target bytes");
            for (usize at = 160; at < inode.size(); ++at)
                if (inode[at])
                    return Status::unsupported("orphan ownership fast symlink extra fields");
            return {};
        }
        // The reclaim victim and directory budgets remain unchanged. A linked
        // regular nonmember may map larger initialized inline extents, bounded
        // by the complete checked 32 MiB filesystem and exact owner bitmap.
        const u32 block_limit = member || type == 0040000 ? 64U :
            u32(binding.layout.filesystem_blocks);
        if ((type != 0100000 && type != 0040000) || u64{size} > u64{block_limit} * 1024 ||
            le32(inode, 32) != 0x80000 || le16(inode, 40) != 0xf30a ||
            le16(inode, 42) > 4 || le16(inode, 44) != 4 || le16(inode, 46) ||
            le32(inode, 48) || le32(inode, 104) || le32(inode, 108) ||
            le32(inode, 112) || le16(inode, 116) || le16(inode, 118) ||
            le16(inode, 126) || le32(inode, 156) ||
            (member && (type != 0100000 || le16(inode, 26) ||
                        size != member->size || le32(inode, 20) != member->next)) ||
            (!member && (!le16(inode, 26) || le32(inode, 20))))
            return Status::unsupported("orphan ownership ordinary inode profile");
        for (usize at = 160; at < inode.size(); ++at)
            if (inode[at]) return Status::unsupported("orphan ownership inode extra fields");
        for (usize at = 52 + le16(inode, 42) * 12; at < 100; ++at)
            if (inode[at]) return Status::unsupported("orphan ownership unused extent bytes");
        const u32 logical_blocks = u32((u64{size} + 1023) / 1024);
        u32 next_logical = 0, mapped = 0, matched = 0;
        for (u32 record = 0; record < le16(inode, 42); ++record)
        {
            const usize at = 52 + record * 12;
            const u32 logical = le32(inode, at), length = le16(inode, at + 4);
            const u32 physical = le32(inode, at + 8);
            if (!length || length > 32768 || logical < next_logical ||
                logical >= logical_blocks || length > logical_blocks - logical ||
                le16(inode, at + 6) || !physical || length > block_limit - mapped ||
                (member && logical != next_logical))
                return Status::unsupported("orphan ownership unsupported inline extent");
            if (auto status = append(binding, source, scratch, number, physical, length); !status)
                return status;
            if (member)
                for (u32 part = 0; part < length; ++part)
                    if (matched >= member->data_count ||
                        physical + part != member->data_blocks[matched++])
                        return Status::io_error("orphan ownership chain mapping differs");
            next_logical = logical + length;
            mapped += length;
        }
        if (le32(inode, 28) != mapped * 2 ||
            (type == 0040000 && mapped != logical_blocks) ||
            (member && (matched != member->data_count || mapped != logical_blocks)) ||
            (!size && le16(inode, 42)))
            return Status::unsupported("orphan ownership mapped-sector coverage");
        if (member)
        {
            bool found = false;
            for (usize index = 0; index < chain.count(); ++index)
                if (chain.at(index)->number == number &&
                    chain.at(index)->generation == generation)
                {
                    if (scratch.seen[index])
                        return Status::io_error("orphan ownership repeated chain member");
                    scratch.seen[index] = true; found = true; break;
                }
            if (!found) return Status::fault("orphan ownership member indexing");
        }
        return {};
    }

  public:
    [[nodiscard]] static Status validate(const Ext4RecoveryInfo &binding,
                                         const Ext4OrphanOwnershipSource &source,
                                         Ext4OrphanOwnershipScratch &scratch,
                                         Ext4OrphanOwnershipProof &output)
    {
        using namespace ext4_checked;
        output = {}; scratch = {};
        Ext4OrphanChainProof chain{};
        if (auto status = Ext4OrphanChainValidator::validate(binding, source.chain, chain); !status)
            return status;
        if (!chain.count())
            return Status::invalid_argument("orphan ownership requires a nonempty chain");
        if (binding.layout.filesystem_blocks != 32768 || binding.layout.block_size != 1024 ||
            binding.metadata_count != 16 || binding.layout.journal_first_block > 32767 ||
            binding.layout.journal_blocks > 32768 ||
            le32(source.chain.superblock, 336) || le32(source.chain.superblock, 340) ||
            le32(source.chain.superblock, 344))
            return Status::unsupported("orphan ownership bounded geometry");
        for (usize one = 0; one < binding.metadata_count; ++one)
        {
            const auto current = binding.metadata[one];
            if (!current.count || overlaps(u32(current.first), u32(current.count),
                                           u32(binding.layout.journal_first_block),
                                           u32(binding.layout.journal_blocks)))
                return Status::unsupported("orphan ownership reserved intervals overlap");
            for (usize earlier = 0; earlier < one; ++earlier)
                if (overlaps(u32(current.first), u32(current.count),
                             u32(binding.metadata[earlier].first),
                             u32(binding.metadata[earlier].count)))
                    return Status::unsupported("orphan ownership metadata intervals overlap");
        }
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        u32 free_blocks = 0;
        for (u32 group = 0; group < 4; ++group)
        {
            const auto descriptor = std::span<const u8>{source.chain.descriptors}.subspan(group * 32, 32);
            const u16 flags = le16(descriptor, 18);
            const auto &inode_bitmap = source.chain.inode_bitmaps[group];
            const auto &block_bitmap = source.chain.block_bitmaps[group];
            const u32 blocks = group == 3 ? 8191 : 8192;
            free_blocks += le16(descriptor, 12);
            if (flags & 1)
            {
                if (!source.inode_tables[group].empty())
                    return Status::invalid_argument("orphan ownership uninitialized table supplied");
                for (usize index = 0; index < 256; ++index)
                    if (inode_bitmap[index])
                        return Status::unsupported("orphan ownership uninitialized inode bitmap has bits");
            }
            else
            {
                if (source.inode_tables[group].size() != 512)
                    return Status::invalid_argument("orphan ownership incomplete inode table");
                if (u16(jbd2::detail::crc32c(seed, std::span<const u8>{inode_bitmap}.first(256))) !=
                    le16(descriptor, 26))
                    return Status::io_error("orphan ownership inode bitmap checksum");
                u32 allocated = 0;
                for (u32 local = 0; local < 2048; ++local)
                    allocated += bit(inode_bitmap, local);
                if (allocated != 2048U - le16(descriptor, 14))
                    return Status::io_error("orphan ownership inode bitmap count");
            }
            if (flags & 2)
            {
                if (overlaps(1 + group * 8192, blocks,
                             u32(binding.layout.journal_first_block),
                             u32(binding.layout.journal_blocks)))
                    return Status::unsupported("orphan ownership journal bitmap is uninitialized");
                for (u8 byte : block_bitmap)
                    if (byte) return Status::unsupported("orphan ownership uninitialized block bitmap has bits");
            }
            else
            {
                if (u16(jbd2::detail::crc32c(seed, block_bitmap)) != le16(descriptor, 24))
                    return Status::io_error("orphan ownership block bitmap checksum");
                u32 allocated = 0;
                for (u32 local = 0; local < blocks; ++local)
                    allocated += bit(block_bitmap, local);
                if (allocated != blocks - le16(descriptor, 12))
                    return Status::io_error("orphan ownership block bitmap count");
            }
        }
        if (free_blocks != le32(source.chain.superblock, 12))
            return Status::io_error("orphan ownership free block counters differ");
        for (u32 number = 1; number <= 10; ++number)
            if (!bit(source.chain.inode_bitmaps[0], number - 1))
                return Status::unsupported("orphan ownership reserved inode is unallocated");
        for (const auto &image : source.chain.chain)
        {
            const u32 group = (image.number - 1) / 2048;
            const u32 local = (image.number - 1) % 2048;
            const u32 table = le32(source.chain.descriptors, group * 32 + 8);
            if (source.inode_tables[group].size() != 512 ||
                image.table_block != table + local / 4 ||
                source.inode_tables[group][local / 4] != image.table)
                return Status::io_error("orphan ownership chain and complete table differ");
        }
        bool journal_seen = false;
        for (u32 group = 0; group < 4; ++group)
        {
            if (source.inode_tables[group].empty()) continue;
            const auto &bitmap = source.chain.inode_bitmaps[group];
            for (u32 local = 0; local < 2048; ++local)
            {
                if (!bit(bitmap, local)) continue;
                if (++scratch.inode_count > scratch.inode_capacity)
                    return Status::no_memory("orphan ownership allocated inode budget");
                const u32 number = group * 2048 + local + 1;
                const auto &table = source.inode_tables[group][local / 4];
                const auto inode = std::span<const u8>{table}.subspan((local % 4) * 256, 256);
                if (number == 8)
                {
                    if (auto status = journal_inode(binding, seed, inode); !status) return status;
                    journal_seen = true;
                }
                else if (number != 2 && number < 11)
                {
                    if (auto status = reserved(seed, number, inode); !status) return status;
                }
                else if (auto status = ordinary_inode(binding, source, chain, scratch,
                                                      seed, number, inode); !status)
                    return status;
            }
        }
        if (!journal_seen)
            return Status::io_error("orphan ownership journal inode is absent");
        for (usize index = 0; index < chain.count(); ++index)
            if (!scratch.seen[index])
                return Status::io_error("orphan ownership chain member was not scanned");
        usize blocks_owned = 0;
        for (u32 group = 0; group < 4; ++group)
        {
            const u16 flags = le16(source.chain.descriptors, group * 32 + 18);
            if (flags & 2) continue;
            const u32 blocks = group == 3 ? 8191 : 8192;
            for (u32 local = 0; local < blocks; ++local)
            {
                const u32 physical = 1 + group * 8192 + local;
                usize ordinary_owners = 0;
                for (usize index = 0; index < scratch.range_count; ++index)
                    ordinary_owners += in_range(physical, scratch.ranges[index].first,
                                                scratch.ranges[index].count);
                const usize reserved_owners = metadata(binding, physical) + journal(binding, physical);
                if (reserved_owners + ordinary_owners > 1 ||
                    bit(source.chain.block_bitmaps[group], local) !=
                        bool(reserved_owners + ordinary_owners))
                    return Status::io_error("orphan ownership exact block bitmap owner differs");
                blocks_owned += ordinary_owners;
            }
        }
        output.filesystem_uuid = chain.filesystem_uuid();
        output.observed_head = chain.observed_head();
        output.orphan_count = chain.count();
        output.allocated_inodes = scratch.inode_count;
        output.ordinary_ranges = scratch.range_count;
        output.owned_blocks = blocks_owned;
        output.valid = true;
        return {};
    }
};

// Prototype, pure-model input. Records are ordered by allocated directory
// inode number, then logical block number. Every mapped directory block in
// the checked inode tables must have exactly one caller-supplied record.
struct Ext4OrphanDirectoryBlockImage
{
    u32 inode{}, logical{}, physical{};
    Ext4OrphanBlock bytes{};
};

struct Ext4OrphanDirectoryReferenceSource
{
    Ext4OrphanOwnershipSource ownership;
    std::span<const Ext4OrphanDirectoryBlockImage> directory_blocks{};
};

struct Ext4OrphanDirectoryReferenceScratch
{
    struct Name
    {
        u32 directory{}, block_index{};
        u16 offset{};
        u8 length{};
    };
    static constexpr usize block_capacity = 256;
    static constexpr usize entry_capacity = 4096;
    Ext4OrphanOwnershipScratch ownership{};
    std::array<Name, entry_capacity> names{};
    usize name_count{};
};

struct Ext4OrphanDirectoryReferenceProof
{
    std::array<u8, 16> filesystem_uuid{};
    u32 observed_head{};
    usize directory_inodes{}, directory_blocks{}, live_entries{};
    bool valid{};
};

// This prototype proves that no parsed name in one complete supplied image
// points to an orphan-chain member. It does not prove image provenance, live
// descriptor/cache exclusion, mount coherence, or permission to free anything.
class Ext4OrphanDirectoryReferenceValidator final
{
    using Scratch = Ext4OrphanDirectoryReferenceScratch;

    [[nodiscard]] static bool inode_bit(const Ext4OrphanOwnershipSource &source, u32 number)
    {
        const u32 index = number - 1;
        const auto &bitmap = source.chain.inode_bitmaps[index / 2048];
        const u32 local = index % 2048;
        return (bitmap[local / 8] & (1U << (local % 8))) != 0;
    }

    [[nodiscard]] static std::span<const u8> inode_bytes(
        const Ext4OrphanOwnershipSource &source, u32 number)
    {
        const u32 index = number - 1, group = index / 2048, local = index % 2048;
        const auto &block = source.inode_tables[group][local / 4];
        return std::span<const u8>{block}.subspan((local % 4) * 256, 256);
    }

    [[nodiscard]] static u32 mapped(std::span<const u8> inode, u32 logical)
    {
        using namespace ext4_checked;
        for (u32 record = 0; record < le16(inode, 42); ++record)
        {
            const usize at = 52 + record * 12;
            const u32 start = le32(inode, at), length = le16(inode, at + 4);
            if (logical >= start && logical - start < length)
                return le32(inode, at + 8) + logical - start;
        }
        return 0;
    }

    [[nodiscard]] static bool orphan(const Ext4OrphanOwnershipSource &source, u32 child)
    {
        for (const auto &member : source.chain.chain)
            if (member.number == child) return true;
        return false;
    }

    [[nodiscard]] static bool name_is(std::span<const u8> bytes, usize offset,
                                      u8 length, const char *name, usize size)
    {
        if (length != size) return false;
        for (usize at = 0; at < size; ++at)
            if (bytes[offset + 8 + at] != u8(name[at])) return false;
        return true;
    }

    [[nodiscard]] static Status block_entries(
        const Ext4OrphanDirectoryReferenceSource &source,
        Ext4OrphanDirectoryReferenceScratch &scratch,
        u32 seed, u32 number, std::span<const u8> inode,
        usize block_index, u32 &dots, u32 &parents)
    {
        using namespace ext4_checked;
        const auto &image = source.directory_blocks[block_index];
        const auto bytes = std::span<const u8>{image.bytes};
        constexpr usize tail = 1024 - 12;
        if (le32(bytes, tail) || le16(bytes, tail + 4) != 12 || bytes[tail + 6] ||
            bytes[tail + 7] != 0xde ||
            jbd2::detail::crc32c(inode_seed(seed, number, inode), bytes.first(tail)) !=
                le32(bytes, tail + 8))
            return Status::io_error("orphan directory tail or checksum");

        for (usize offset = 0; offset < tail;)
        {
            if (tail - offset < 8)
                return Status::io_error("orphan directory short record");
            const u32 child = le32(bytes, offset);
            const u16 length = le16(bytes, offset + 4);
            const u8 name_length = bytes[offset + 6], type = bytes[offset + 7];
            if (length < 8 || (length & 3) || length > tail - offset ||
                name_length > length - 8 || child > 8192 ||
                (child && (!name_length || (type != 1 && type != 2 && type != 7))))
                return Status::io_error("orphan directory record bounds or type");
            if (child)
            {
                for (usize at = 0; at < name_length; ++at)
                    if (!bytes[offset + 8 + at] || bytes[offset + 8 + at] == '/')
                        return Status::io_error("orphan directory invalid name");
                if (orphan(source.ownership, child))
                    return Status::io_error("orphan directory name references orphan");
                if (!inode_bit(source.ownership, child))
                    return Status::io_error("orphan directory name references unallocated inode");
                const auto target = inode_bytes(source.ownership, child);
                const u16 target_type = le16(target, 0) & 0170000;
                if (type != (target_type == 0040000 ? 2U : target_type == 0100000 ? 1U :
                             target_type == 0120000 ? 7U : 0U))
                    return Status::io_error("orphan directory name and inode type differ");

                const bool dot = name_is(bytes, offset, name_length, ".", 1);
                const bool dotdot = name_is(bytes, offset, name_length, "..", 2);
                if (dot)
                {
                    if (child != number || type != 2 || ++dots != 1)
                        return Status::io_error("orphan directory invalid dot");
                }
                else if (dotdot)
                {
                    if (type != 2 || (number == 2 && child != 2) || ++parents != 1)
                        return Status::io_error("orphan directory invalid dotdot");
                }
                else if (child < 11)
                    return Status::unsupported("orphan directory names reserved inode");

                for (usize prior = 0; prior < scratch.name_count; ++prior)
                {
                    const auto &name = scratch.names[prior];
                    if (name.directory != number || name.length != name_length) continue;
                    const auto previous = std::span<const u8>{
                        source.directory_blocks[name.block_index].bytes};
                    bool same = true;
                    for (usize at = 0; at < name_length; ++at)
                        if (previous[name.offset + 8 + at] != bytes[offset + 8 + at])
                        { same = false; break; }
                    if (same) return Status::io_error("orphan directory duplicate name");
                }
                if (scratch.name_count == scratch.names.size())
                    return Status::unsupported("orphan directory entry budget");
                scratch.names[scratch.name_count++] = {
                    number, u32(block_index), u16(offset), name_length};
            }
            offset += length;
        }
        return {};
    }

  public:
    [[nodiscard]] static Status validate(const Ext4RecoveryInfo &binding,
                                         const Ext4OrphanDirectoryReferenceSource &source,
                                         Ext4OrphanDirectoryReferenceScratch &scratch,
                                         Ext4OrphanDirectoryReferenceProof &output)
    {
        using namespace ext4_checked;
        output = {}; scratch = {};
        if (source.directory_blocks.size() > Scratch::block_capacity)
            return Status::unsupported("orphan directory block budget");
        Ext4OrphanOwnershipProof ownership{};
        if (auto status = Ext4OrphanOwnershipValidator::validate(
                binding, source.ownership, scratch.ownership, ownership); !status)
            return status;
        if (!ownership.valid)
            return Status::fault("orphan directory ownership proof absent");
        for (usize one = 0; one < source.directory_blocks.size(); ++one)
            for (usize earlier = 0; earlier < one; ++earlier)
                if ((source.directory_blocks[one].inode == source.directory_blocks[earlier].inode &&
                     source.directory_blocks[one].logical == source.directory_blocks[earlier].logical) ||
                    source.directory_blocks[one].physical == source.directory_blocks[earlier].physical)
                    return Status::io_error("orphan directory duplicate block image");

        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        usize cursor = 0, directory_inodes = 0;
        for (u32 number = 1; number <= 8192; ++number)
        {
            if (!inode_bit(source.ownership, number)) continue;
            const auto inode = inode_bytes(source.ownership, number);
            if ((le16(inode, 0) & 0170000) != 0040000)
            {
                if (number == 2)
                    return Status::unsupported("orphan directory root inode is not a directory");
                continue;
            }
            if (number != 2 && number < 11)
                return Status::unsupported("orphan directory reserved inode profile");
            const u32 size = le32(inode, 4);
            if (!size || size > 64 * 1024 || size % 1024)
                return Status::unsupported("orphan directory size profile");
            ++directory_inodes;
            u32 dots = 0, parents = 0;
            for (u32 logical = 0; logical < size / 1024; ++logical)
            {
                if (cursor == Scratch::block_capacity)
                    return Status::unsupported("orphan directory block budget");
                if (cursor == source.directory_blocks.size())
                    return Status::invalid_argument("orphan directory block image missing");
                const u32 physical = mapped(inode, logical);
                const auto &image = source.directory_blocks[cursor];
                if (!physical || image.inode != number || image.logical != logical ||
                    image.physical != physical)
                    return Status::io_error("orphan directory block mapping differs");
                if (auto status = block_entries(source, scratch, seed, number, inode,
                                                cursor, dots, parents); !status)
                    return status;
                ++cursor;
            }
            if (dots != 1 || parents != 1)
                return Status::io_error("orphan directory dot entries missing");
        }
        if (cursor != source.directory_blocks.size())
            return Status::invalid_argument("orphan directory unexpected extra block image");
        output.filesystem_uuid = ownership.filesystem_uuid;
        output.observed_head = ownership.observed_head;
        output.directory_inodes = directory_inodes;
        output.directory_blocks = cursor;
        output.live_entries = scratch.name_count;
        output.valid = true;
        return {};
    }
};

struct Ext4OrphanReclaimPlan
{
    static constexpr usize capacity = 5;
    u32 inode{}, old_generation{}, retained_generation{}, next_head{};
    u32 released_blocks{};
    std::array<u32, 16> released_mapping{};
    std::array<u32, capacity> targets{};
    std::array<Ext4OrphanBlock, capacity> before{}, after{};
    usize count{};
};

// This is a pure old-image transformer. The caller must separately prove a
// stable, owner-exclusive device epoch and all runtime descriptor/pin lifetimes.
// A plan is neither journal authority nor permission to reclaim an inode.
class Ext4OrphanReclaimPlanner final
{
    using Block = Ext4OrphanBlock;

    static constexpr void store16(std::span<u8> bytes, usize at, u16 value)
    {
        bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8);
    }
    static constexpr void store32(std::span<u8> bytes, usize at, u32 value)
    {
        for (usize index = 0; index < 4; ++index)
            bytes[at + index] = u8(value >> (index * 8));
    }
    [[nodiscard]] static u16 descriptor_checksum(u32 seed, u32 group,
                                                  std::span<const u8> descriptor)
    {
        std::array<u8, 32> copy{};
        for (usize index = 0; index < copy.size(); ++index) copy[index] = descriptor[index];
        copy[30] = copy[31] = 0;
        return u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)), copy));
    }
    static void stamp_inode(std::span<u8> inode, u32 number, u32 seed)
    {
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(crc)); store16(inode, 130, u16(crc >> 16));
    }
    [[nodiscard]] static Status append(Ext4OrphanReclaimPlan &plan, u32 target,
                                       const Block &before, usize &index)
    {
        for (index = 0; index < plan.count; ++index)
            if (plan.targets[index] == target)
                return plan.before[index] == before ? Status{} :
                    Status::io_error("orphan reclaim aliased home images differ");
        if (plan.count == plan.targets.size())
            return Status::no_memory("orphan reclaim home capacity");
        index = plan.count++;
        plan.targets[index] = target;
        plan.before[index] = before;
        plan.after[index] = before;
        return {};
    }

  public:
    // Legacy pure transformer API retained for callers with an adequate stack.
    // Writable cold recovery uses the explicit caller-owned form below.
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding,
        const Ext4OrphanOwnershipSource &source, Ext4OrphanOwnershipScratch &scratch,
        u32 expected_head, u32 expected_generation, Ext4OrphanReclaimPlan &output)
    {
        Ext4OrphanReclaimPlan proposed{};
        return transform(binding, source, scratch, expected_head, expected_generation, proposed, output);
    }
    [[nodiscard]] static Status plan_off_stack(const Ext4RecoveryInfo &binding,
        const Ext4OrphanOwnershipSource &source, Ext4OrphanOwnershipScratch &scratch,
        u32 expected_head, u32 expected_generation, Ext4OrphanReclaimPlan &proposed,
        Ext4OrphanReclaimPlan &output)
    {
        const auto a = reinterpret_cast<usize>(&proposed), b = reinterpret_cast<usize>(&output);
        if (a <= b ? b - a < sizeof(proposed) : a - b < sizeof(output))
            return Status::invalid_argument("orphan reclaim scratch/output overlap");
        return transform(binding, source, scratch, expected_head, expected_generation, proposed, output);
    }
  private:
    [[nodiscard]] static Status transform(const Ext4RecoveryInfo &binding,
                                     const Ext4OrphanOwnershipSource &source,
                                     Ext4OrphanOwnershipScratch &scratch,
                                     u32 expected_head, u32 expected_generation,
                                     Ext4OrphanReclaimPlan &proposed, Ext4OrphanReclaimPlan &output)
    {
        using namespace ext4_checked;
        output = {}; proposed = {};
        if (!expected_head || expected_head < 11 || expected_head > 8192)
            return Status::invalid_argument("orphan reclaim expected head");
        Ext4OrphanOwnershipProof ownership{};
        if (auto status = Ext4OrphanOwnershipValidator::validate(
                binding, source, scratch, ownership); !status)
            return status;
        Ext4OrphanChainProof chain{};
        if (auto status = Ext4OrphanChainValidator::validate(binding, source.chain, chain); !status)
            return status;
        const auto *head = chain.at(0);
        if (!ownership.valid || ownership.filesystem_uuid != binding.uuid ||
            ownership.observed_head != expected_head ||
            chain.observed_head() != expected_head || !head ||
            head->number != expected_head || head->generation != expected_generation)
            return Status::io_error("orphan reclaim stale head or generation");
        if (head->generation == ~u32{0})
            return Status::overflow("orphan reclaim generation exhausted");
        if (head->data_count > head->data_blocks.size() ||
            head->data_count != (u64{head->size} + 1023) / 1024)
            return Status::unsupported("orphan reclaim bounded data mapping");

        const u32 inode_group = (head->number - 1) / 2048;
        const u32 inode_local = (head->number - 1) % 2048;
        const u32 data_group = head->data_count ? (head->data_blocks[0] - 1) / 8192 : 0;
        for (u32 index = 0; index < head->data_count; ++index)
            if ((head->data_blocks[index] - 1) / 8192 != data_group)
                return Status::unsupported("orphan reclaim spans data allocation groups");

        const auto &descriptors = source.chain.descriptors;
        const auto &superblock = source.chain.superblock;
        const u32 inode_table = le32(descriptors, inode_group * 32 + 8) + inode_local / 4;
        const u32 inode_bitmap = le32(descriptors, inode_group * 32 + 4);
        const u32 block_bitmap = head->data_count ? le32(descriptors, data_group * 32) : 0;
        const auto &table_before = source.inode_tables[inode_group][inode_local / 4];
        const u16 free_inodes = le16(descriptors, inode_group * 32 + 14);
        const u16 free_blocks = head->data_count ? le16(descriptors, data_group * 32 + 12) : 0;
        const u32 super_free_inodes = le32(superblock, 16);
        const u32 super_free_blocks = le32(superblock, 12);
        if (free_inodes >= 2048 || super_free_inodes >= 8192 ||
            (head->data_count &&
             (u32{free_blocks} + head->data_count > (data_group == 3 ? 8191U : 8192U) ||
              super_free_blocks > 32767U - head->data_count)))
            return Status::io_error("orphan reclaim free counters overflow");

        usize table_index{}, inode_bitmap_index{}, block_bitmap_index{};
        usize descriptor_index{}, super_index{};
        if (auto status = append(proposed, inode_table, table_before, table_index); !status)
            return status;
        if (auto status = append(proposed, inode_bitmap,
                                 source.chain.inode_bitmaps[inode_group],
                                 inode_bitmap_index); !status)
            return status;
        if (head->data_count)
            if (auto status = append(proposed, block_bitmap,
                                     source.chain.block_bitmaps[data_group],
                                     block_bitmap_index); !status)
                return status;
        if (auto status = append(proposed, 2, descriptors, descriptor_index); !status)
            return status;
        if (auto status = append(proposed, 1, superblock, super_index); !status)
            return status;
        if (proposed.count != (head->data_count ? 5U : 4U))
            return Status::unsupported("orphan reclaim prohibited home alias");

        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        auto inode = std::span<u8>{proposed.after[table_index]}.subspan(
            (inode_local % 4) * 256, 256);
        for (u8 &byte : inode) byte = 0;
        store32(inode, 100, head->generation + 1);
        store16(inode, 128, 32);
        // The exact private marker retains the next allocated identity. Create
        // must validate it, consume that generation and clear the entire slot.
        store32(inode, 160, 0x3146524fU); // "ORF1" in little-endian bytes.
        stamp_inode(inode, head->number, seed);

        auto &inode_bitmap_after = proposed.after[inode_bitmap_index];
        inode_bitmap_after[inode_local / 8] &= u8(~(1U << (inode_local % 8)));
        auto descriptor_after = std::span<u8>{proposed.after[descriptor_index]};
        store16(descriptor_after, inode_group * 32 + 14, u16(free_inodes + 1));
        store16(descriptor_after, inode_group * 32 + 26,
                u16(jbd2::detail::crc32c(seed, std::span<const u8>{inode_bitmap_after}.first(256))));

        if (head->data_count)
        {
            auto &block_bitmap_after = proposed.after[block_bitmap_index];
            for (u32 index = 0; index < head->data_count; ++index)
            {
                const u32 local = (head->data_blocks[index] - 1) % 8192;
                block_bitmap_after[local / 8] &= u8(~(1U << (local % 8)));
            }
            store16(descriptor_after, data_group * 32 + 12,
                    u16(free_blocks + head->data_count));
            store16(descriptor_after, data_group * 32 + 24,
                    u16(jbd2::detail::crc32c(seed, std::span<const u8>{block_bitmap_after})));
        }
        // A single descriptor block may hold both changed groups. Stamp each
        // affected descriptor once after all counters and bitmaps are final.
        store16(descriptor_after, inode_group * 32 + 30, 0);
        store16(descriptor_after, inode_group * 32 + 30,
                descriptor_checksum(seed, inode_group,
                                    descriptor_after.subspan(inode_group * 32, 32)));
        if (head->data_count && data_group != inode_group)
        {
            store16(descriptor_after, data_group * 32 + 30, 0);
            store16(descriptor_after, data_group * 32 + 30,
                    descriptor_checksum(seed, data_group,
                                        descriptor_after.subspan(data_group * 32, 32)));
        }

        auto &super_after = proposed.after[super_index];
        store32(super_after, 16, super_free_inodes + 1);
        if (head->data_count)
            store32(super_after, 12, super_free_blocks + head->data_count);
        store32(super_after, 232, head->next);
        store32(super_after, 1020,
                jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super_after}.first(1020)));
        proposed.inode = head->number;
        proposed.old_generation = head->generation;
        proposed.retained_generation = head->generation + 1;
        proposed.next_head = head->next;
        proposed.released_blocks = head->data_count;
        for (u32 index = 0; index < head->data_count; ++index)
            proposed.released_mapping[index] = head->data_blocks[index];
        output = proposed;
        return {};
    }
};

struct Ext4OrphanDirectoryQualifiedReclaimScratch
{
    Ext4OrphanDirectoryReferenceScratch directory{};
    Ext4OrphanOwnershipScratch reclaim{};
    // Full home images stay in caller storage instead of nested 10 KiB frames.
    Ext4OrphanReclaimPlan candidate{}, transformation{};
};

struct Ext4OrphanDirectoryQualifiedReclaimPlan
{
    Ext4OrphanDirectoryReferenceProof directory{};
    Ext4OrphanReclaimPlan reclaim{};
    bool valid{};
};

// Pure composition over one immutable caller-supplied image set. A valid
// result proves no parsed directory name references any orphan in that set;
// it does not grant device, cache, descriptor-lifetime, journal or freeing
// authority. Scratch and output must not alias the borrowed input images.
class Ext4OrphanDirectoryQualifiedReclaimPlanner final
{
  public:
    [[nodiscard]] static Status plan(
        const Ext4RecoveryInfo &binding,
        const Ext4OrphanDirectoryReferenceSource &source,
        Ext4OrphanDirectoryQualifiedReclaimScratch &scratch,
        u32 expected_head, u32 expected_generation,
        Ext4OrphanDirectoryQualifiedReclaimPlan &output)
    {
        output = {};
        Ext4OrphanDirectoryReferenceProof directory{};
        if (auto status = Ext4OrphanDirectoryReferenceValidator::validate(
                binding, source, scratch.directory, directory); !status)
            return status;
        if (!directory.valid || directory.filesystem_uuid != binding.uuid ||
            directory.observed_head != expected_head)
            return Status::io_error("directory qualified reclaim identity differs");

        auto &reclaim = scratch.candidate;
        if (auto status = Ext4OrphanReclaimPlanner::plan_off_stack(
                binding, source.ownership, scratch.reclaim,
                expected_head, expected_generation, scratch.transformation, reclaim); !status)
            return status;
        if (reclaim.inode != expected_head ||
            reclaim.old_generation != expected_generation || !reclaim.count)
            return Status::fault("directory qualified reclaim plan identity differs");

        output.directory = directory;
        output.reclaim = reclaim;
        output.valid = true;
        return {};
    }
};

struct Ext4OrphanReclaimWitness
{
    std::array<u8, 16> uuid{};
    std::array<u32, Ext4OrphanReclaimPlan::capacity> targets{};
    std::array<u32, 16> released_mapping{};
    u32 inode{}, old_generation{}, retained_generation{}, next_head{}, sequence{};
    usize count{}, released_blocks{}, runs{}, bytes{};
};

// Private descriptor-padding extension. It reconstructs exact old bytes but
// supplies neither device/session provenance nor permission to replay or free.
class Ext4OrphanReclaimWitnessCodec final
{
    using Block = Ext4OrphanBlock;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'O', 'R', 'W', '0', '0', '1'};
    static void store16(Block &block, usize at, u16 value)
    { block[at] = u8(value); block[at + 1] = u8(value >> 8); }
    static void store32(Block &block, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) block[at + i] = u8(value >> (8 * i)); }
    [[nodiscard]] static u16 descriptor_checksum(u32 seed, u32 group,
                                                  std::span<const u8> descriptor)
    {
        std::array<u8, 32> copy{};
        for (usize i = 0; i < copy.size(); ++i) copy[i] = descriptor[i];
        copy[30] = copy[31] = 0;
        return u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)), copy));
    }
    [[nodiscard]] static bool same_uuid(const Block &block, const std::array<u8, 16> &uuid)
    {
        for (usize i = 0; i < uuid.size(); ++i)
            if (block[104 + i] != uuid[i]) return false;
        return true;
    }
    [[nodiscard]] static Status mapped_inode(const std::array<u8, 16> &uuid,
        std::span<const u8> inode_bytes, u32 inode, u32 next,
        usize released, std::span<const u32> mapping)
    {
        using namespace ext4_checked;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, uuid);
        if (auto status = inode_checksum(seed, inode, inode_bytes); !status) return status;
        const u32 size = le32(inode_bytes, 4);
        const usize blocks = usize((u64{size} + 1023) / 1024);
        const usize records = le16(inode_bytes, 42);
        if (blocks != released || released > 16 || mapping.size() < released ||
            le32(inode_bytes, 20) != next ||
            (le16(inode_bytes, 0) & 0170000) != 0100000 ||
            le16(inode_bytes, 26) || le32(inode_bytes, 28) != blocks * 2 ||
            le32(inode_bytes, 32) != 0x80000 ||
            le16(inode_bytes, 40) != 0xf30a || records > 4 ||
            le16(inode_bytes, 44) != 4 || le16(inode_bytes, 46) ||
            le32(inode_bytes, 48) || (!blocks && records))
            return Status::io_error("orphan reclaim witnessed old extent profile");
        usize mapped = 0;
        for (usize record = 0; record < records; ++record)
        {
            const usize at = 52 + record * 12;
            const u32 logical = le32(inode_bytes, at);
            const usize length = le16(inode_bytes, at + 4);
            const u32 physical = le32(inode_bytes, at + 8);
            if (!length || length > released - mapped || logical != mapped ||
                le16(inode_bytes, at + 6) || !physical ||
                physical > 32768 - length)
                return Status::io_error("orphan reclaim witnessed extent shape");
            for (usize part = 0; part < length; ++part)
                if (mapping[mapped++] != physical + part)
                    return Status::io_error("orphan reclaim witnessed released mapping");
        }
        return mapped == released ? Status{} :
            Status::io_error("orphan reclaim witnessed extent coverage");
    }
    [[nodiscard]] static Status identity(const Ext4RecoveryInfo &binding,
                                          const Ext4OrphanReclaimPlan &plan)
    {
        using namespace ext4_checked;
        if ((plan.count != 4 && plan.count != 5) || plan.count != (plan.released_blocks ? 5U : 4U) ||
            plan.released_blocks > plan.released_mapping.size() ||
            plan.inode < 11 || plan.inode > 8192 ||
            plan.old_generation == ~u32{0} || plan.retained_generation != plan.old_generation + 1 ||
            plan.next_head > 8192 || plan.next_head == plan.inode)
            return Status::invalid_argument("orphan reclaim witness identity");
        for (usize i = plan.released_blocks; i < plan.released_mapping.size(); ++i)
            if (plan.released_mapping[i])
                return Status::invalid_argument("orphan reclaim unused mapping");
        for (usize i = 0; i < plan.count; ++i)
        {
            if (!plan.targets[i] || plan.targets[i] >= binding.layout.filesystem_blocks ||
                plan.before[i] == plan.after[i])
                return Status::invalid_argument("orphan reclaim witness home");
            for (usize prior = 0; prior < i; ++prior)
                if (plan.targets[prior] == plan.targets[i])
                    return Status::invalid_argument("orphan reclaim duplicate home");
        }
        if (plan.targets[plan.count - 2] != 2 || plan.targets[plan.count - 1] != 1 ||
            !same_uuid(plan.before[plan.count - 1], binding.uuid) ||
            !same_uuid(plan.after[plan.count - 1], binding.uuid) ||
            le32(plan.before[plan.count - 1], 232) != plan.inode ||
            le32(plan.after[plan.count - 1], 232) != plan.next_head)
            return Status::invalid_argument("orphan reclaim superblock binding");
        const u32 group = (plan.inode - 1) / 2048;
        const u32 local = (plan.inode - 1) % 2048;
        const auto &descriptors = plan.before[plan.count - 2];
        if (plan.targets[0] != le32(descriptors, group * 32 + 8) + local / 4 ||
            plan.targets[1] != le32(descriptors, group * 32 + 4))
            return Status::invalid_argument("orphan reclaim table or inode bitmap target");
        const auto old_inode = std::span<const u8>{plan.before[0]}.subspan((local % 4) * 256, 256);
        const auto new_inode = std::span<const u8>{plan.after[0]}.subspan((local % 4) * 256, 256);
        if (le32(old_inode, 100) != plan.old_generation ||
            le32(new_inode, 100) != plan.retained_generation ||
            le32(new_inode, 160) != 0x3146524fU)
            return Status::invalid_argument("orphan reclaim inode generation");
        if (auto status = mapped_inode(binding.uuid, old_inode, plan.inode, plan.next_head,
                plan.released_blocks, plan.released_mapping); !status)
            return status;
        for (usize i = 0; i < 1024; ++i)
            if ((i < (local % 4) * 256 || i >= (local % 4 + 1) * 256) &&
                plan.before[0][i] != plan.after[0][i])
                return Status::invalid_argument("orphan reclaim sibling changed");
        Block bitmap = plan.before[1];
        const u32 inode_byte = local / 8;
        const u8 inode_mask = u8(1U << (local % 8));
        if (!(bitmap[inode_byte] & inode_mask))
            return Status::invalid_argument("orphan reclaim inode was already free");
        bitmap[inode_byte] &= u8(~inode_mask);
        if (bitmap != plan.after[1])
            return Status::invalid_argument("orphan reclaim inode bitmap delta");
        if (plan.released_blocks)
        {
            const u32 first = plan.released_mapping[0];
            if (!first) return Status::invalid_argument("orphan reclaim zero data block");
            const u32 data_group = (first - 1) / 8192;
            if (data_group >= 4 ||
                plan.targets[2] != le32(descriptors, data_group * 32))
                return Status::invalid_argument("orphan reclaim block bitmap target");
            bitmap = plan.before[2];
            for (usize i = 0; i < plan.released_blocks; ++i)
            {
                const u32 block = plan.released_mapping[i];
                if (!block || block >= binding.layout.filesystem_blocks ||
                    (block - 1) / 8192 != data_group)
                    return Status::invalid_argument("orphan reclaim released mapping");
                for (usize prior = 0; prior < i; ++prior)
                    if (plan.released_mapping[prior] == block)
                        return Status::invalid_argument("orphan reclaim duplicate released block");
                const u32 bit = (block - 1) % 8192;
                const u8 mask = u8(1U << (bit % 8));
                if (!(bitmap[bit / 8] & mask))
                    return Status::invalid_argument("orphan reclaim block was already free");
                bitmap[bit / 8] &= u8(~mask);
            }
            if (bitmap != plan.after[2])
                return Status::invalid_argument("orphan reclaim block bitmap delta");
        }
        return {};
    }
    [[nodiscard]] static Status decoded_identity(const Block &descriptor,
        const std::array<u8, 16> &uuid, std::span<const Block> after,
        std::span<const Block> before, usize count, usize released)
    {
        using namespace ext4_checked;
        const u32 inode = le32(descriptor, 160), generation = le32(descriptor, 164);
        const u32 retained = le32(descriptor, 168), next = le32(descriptor, 172);
        if (inode < 11 || inode > 8192 || generation == ~u32{0} ||
            retained != generation + 1 || next > 8192 || next == inode ||
            le32(before[count - 1], 232) != inode ||
            le32(after[count - 1], 232) != next ||
            !same_uuid(before[count - 1], uuid) ||
            !same_uuid(after[count - 1], uuid) ||
            le32(descriptor, 180 + (count - 2) * 4) != 2 ||
            le32(descriptor, 180 + (count - 1) * 4) != 1)
            return Status::io_error("orphan reclaim decoded identity");
        const u32 group = (inode - 1) / 2048, local = (inode - 1) % 2048;
        const auto &descriptors = before[count - 2];
        if (le32(descriptor, 180) != le32(descriptors, group * 32 + 8) + local / 4 ||
            le32(descriptor, 184) != le32(descriptors, group * 32 + 4))
            return Status::io_error("orphan reclaim decoded home targets");
        const auto old_inode = std::span<const u8>{before[0]}.subspan((local % 4) * 256, 256);
        const auto new_inode = std::span<const u8>{after[0]}.subspan((local % 4) * 256, 256);
        if (le32(old_inode, 100) != generation || le32(new_inode, 100) != retained ||
            le32(new_inode, 160) != 0x3146524fU)
            return Status::io_error("orphan reclaim decoded inode generation");
        std::array<u32, 16> mapping{};
        for (usize i = 0; i < released; ++i)
            mapping[i] = le32(descriptor, 240 + i * 4);
        if (auto status = mapped_inode(uuid, old_inode, inode, next, released, mapping); !status)
            return status;
        for (usize i = 0; i < 1024; ++i)
            if ((i < (local % 4) * 256 || i >= (local % 4 + 1) * 256) &&
                before[0][i] != after[0][i])
                return Status::io_error("orphan reclaim decoded sibling changed");
        Block bitmap = before[1];
        const u8 inode_mask = u8(1U << (local % 8));
        if (!(bitmap[local / 8] & inode_mask))
            return Status::io_error("orphan reclaim decoded inode already free");
        bitmap[local / 8] &= u8(~inode_mask);
        if (bitmap != after[1])
            return Status::io_error("orphan reclaim decoded inode bitmap");
        u32 data_group = 0;
        if (released)
        {
            const u32 first = le32(descriptor, 240);
            if (!first) return Status::io_error("orphan reclaim decoded zero block");
            data_group = (first - 1) / 8192;
            if (data_group >= 4 ||
                le32(descriptor, 188) != le32(descriptors, data_group * 32))
                return Status::io_error("orphan reclaim decoded block bitmap target");
            bitmap = before[2];
            for (usize i = 0; i < released; ++i)
            {
                const u32 block = le32(descriptor, 240 + i * 4);
                if (!block || (block - 1) / 8192 != data_group)
                    return Status::io_error("orphan reclaim decoded released mapping");
                for (usize prior = 0; prior < i; ++prior)
                    if (le32(descriptor, 240 + prior * 4) == block)
                        return Status::io_error("orphan reclaim decoded duplicate block");
                const u32 bit = (block - 1) % 8192;
                const u8 mask = u8(1U << (bit % 8));
                if (!(bitmap[bit / 8] & mask))
                    return Status::io_error("orphan reclaim decoded block already free");
                bitmap[bit / 8] &= u8(~mask);
            }
            if (bitmap != after[2])
                return Status::io_error("orphan reclaim decoded block bitmap delta");
        }
        const u32 seed = jbd2::detail::crc32c(~u32{0}, uuid);
        Block expected = before[0];
        auto slot = std::span<u8>{expected}.subspan((local % 4) * 256, 256);
        for (u8 &byte : slot) byte = 0;
        for (usize i = 0; i < 4; ++i) slot[100 + i] = u8(retained >> (8 * i));
        slot[128] = 32;
        slot[160] = 'O'; slot[161] = 'R'; slot[162] = 'F'; slot[163] = '1';
        const u32 inode_crc = jbd2::detail::crc32c(
            ext4_checked::inode_seed(seed, inode, slot), slot);
        slot[124] = u8(inode_crc); slot[125] = u8(inode_crc >> 8);
        slot[130] = u8(inode_crc >> 16); slot[131] = u8(inode_crc >> 24);
        if (expected != after[0])
            return Status::io_error("orphan reclaim decoded table image");

        const u16 free_inodes = le16(descriptors, group * 32 + 14);
        const u32 super_free_inodes = le32(before[count - 1], 16);
        const u16 free_blocks = released ? le16(descriptors, data_group * 32 + 12) : 0;
        const u32 super_free_blocks = le32(before[count - 1], 12);
        if (free_inodes >= 2048 || super_free_inodes >= 8192 ||
            (released && (u32{free_blocks} + released >
                (data_group == 3 ? 8191U : 8192U) ||
                super_free_blocks > 32767U - released)))
            return Status::io_error("orphan reclaim decoded free counter overflow");
        expected = descriptors;
        store16(expected, group * 32 + 14, u16(free_inodes + 1));
        store16(expected, group * 32 + 26, u16(jbd2::detail::crc32c(
            seed, std::span<const u8>{after[1]}.first(256))));
        if (released)
        {
            store16(expected, data_group * 32 + 12, u16(free_blocks + released));
            store16(expected, data_group * 32 + 24,
                u16(jbd2::detail::crc32c(seed, after[2])));
        }
        store16(expected, group * 32 + 30, 0);
        store16(expected, group * 32 + 30,
            descriptor_checksum(seed, group,
                std::span<const u8>{expected}.subspan(group * 32, 32)));
        if (released && data_group != group)
        {
            store16(expected, data_group * 32 + 30, 0);
            store16(expected, data_group * 32 + 30,
                descriptor_checksum(seed, data_group,
                    std::span<const u8>{expected}.subspan(data_group * 32, 32)));
        }
        if (expected != after[count - 2])
            return Status::io_error("orphan reclaim decoded descriptor image");
        expected = before[count - 1];
        store32(expected, 16, super_free_inodes + 1);
        if (released) store32(expected, 12, super_free_blocks + u32(released));
        store32(expected, 232, next);
        store32(expected, 1020, jbd2::detail::crc32c(
            ~u32{0}, std::span<const u8>{expected}.first(1020)));
        if (expected != after[count - 1])
            return Status::io_error("orphan reclaim decoded superblock image");
        return {};
    }

  public:
    static constexpr usize begin = 128, patch_begin = 304, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize i = 0; i < magic.size(); ++i)
            if (descriptor[begin + i] != magic[i]) return false;
        return true;
    }
    // The descriptor is unchanged when validation or full preimage encoding
    // fails. The caller supplies the already checked canonical reclaim plan.
    [[nodiscard]] static Status encode(const Ext4RecoveryInfo &binding,
        const Ext4OrphanReclaimPlan &plan, u32 sequence, Block &descriptor)
    {
        if (auto status = identity(binding, plan); !status) return status;
        if (!sequence || sequence != jbd2::detail::be32(descriptor, 8))
            return Status::invalid_argument("orphan reclaim witness sequence");
        Block proposed = descriptor;
        for (usize at = begin; at < end; ++at) proposed[at] = 0;
        for (usize i = 0; i < magic.size(); ++i) proposed[begin + i] = magic[i];
        store16(proposed, 136, 1);
        proposed[140] = u8(plan.count); proposed[141] = u8(plan.released_blocks);
        for (usize i = 0; i < binding.uuid.size(); ++i) proposed[144 + i] = binding.uuid[i];
        store32(proposed, 160, plan.inode); store32(proposed, 164, plan.old_generation);
        store32(proposed, 168, plan.retained_generation); store32(proposed, 172, plan.next_head);
        store32(proposed, 176, sequence);
        for (usize home = 0; home < plan.count; ++home)
        {
            store32(proposed, 180 + home * 4, plan.targets[home]);
            store32(proposed, 200 + home * 4,
                 jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            store32(proposed, 220 + home * 4,
                 jbd2::detail::crc32c(~u32{0}, plan.after[home]));
        }
        for (usize i = 0; i < plan.released_blocks; ++i)
            store32(proposed, 240 + i * 4, plan.released_mapping[i]);
        usize cursor = patch_begin, runs = 0;
        for (usize home = 0; home < plan.count; ++home)
            for (usize at = 0; at < 1024;)
            {
                if (plan.before[home][at] == plan.after[home][at]) { ++at; continue; }
                const usize first = at;
                while (at < 1024 && plan.before[home][at] != plan.after[home][at]) ++at;
                const usize length = at - first;
                if (cursor > end || end - cursor < 5 || length > end - cursor - 5)
                    return Status::unsupported("orphan reclaim complete witness exceeds descriptor");
                proposed[cursor] = u8(home);
                store16(proposed, cursor + 1, u16(first)); store16(proposed, cursor + 3, u16(length));
                cursor += 5;
                for (usize i = 0; i < length; ++i) proposed[cursor++] = plan.before[home][first + i];
                ++runs;
            }
        store16(proposed, 138, u16(cursor)); store16(proposed, 142, u16(runs));
        descriptor = proposed;
        return {};
    }
    // Decodes only a fully checksummed descriptor and every supplied logical
    // after-image. before may be partially overwritten on error and must be
    // discarded unless success is returned. All buffers must be disjoint.
    [[nodiscard]] static Status decode(const Block &descriptor,
        const std::array<u8, 16> &expected_uuid, std::span<const Block> after,
        std::span<Block> before, Ext4OrphanReclaimWitness &output)
    {
        using namespace ext4_checked;
        output = {};
        if (!present(descriptor) || le16(descriptor, 136) != 1)
            return Status::unsupported("orphan reclaim witness absent or version unknown");
        const usize count = descriptor[140], released = descriptor[141];
        const usize limit = le16(descriptor, 138), runs = le16(descriptor, 142);
        if ((count != 4 && count != 5) || count != (released ? 5U : 4U) ||
            released > 16 || after.size() != count || before.size() != count ||
            limit < patch_begin || limit > end)
            return Status::invalid_argument("orphan reclaim witness geometry");
        for (usize i = 0; i < expected_uuid.size(); ++i)
            if (descriptor[144 + i] != expected_uuid[i])
                return Status::io_error("orphan reclaim witness UUID");
        const u32 sequence = le32(descriptor, 176);
        if (!sequence || jbd2::detail::be32(descriptor, 0) != jbd2::magic ||
            jbd2::detail::be32(descriptor, 4) != u32(jbd2::RecordType::descriptor) ||
            jbd2::detail::be32(descriptor, 8) != sequence)
            return Status::io_error("orphan reclaim witness descriptor sequence");
        const u32 seed = jbd2::detail::crc32c(~u32{0}, expected_uuid);
        Block checked = descriptor;
        for (usize i = 1020; i < 1024; ++i) checked[i] = 0;
        if (jbd2::detail::be32(descriptor, 1020) != jbd2::detail::crc32c(seed, checked))
            return Status::io_error("orphan reclaim descriptor checksum");
        for (usize at = count == 4 ? 92U : 108U; at < begin; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim tag padding");
        for (usize at = limit; at < end; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim witness trailing bytes");
        for (usize at = 180 + count * 4; at < 200; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim unused targets");
        for (usize at = 200 + count * 4; at < 220; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim unused before CRC");
        for (usize at = 220 + count * 4; at < 240; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim unused after CRC");
        for (usize at = 240 + released * 4; at < patch_begin; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim unused mapping");
        for (usize at = 28; at < 44; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim first tag UUID");
        const auto sequence_bytes = std::span<const u8>{descriptor}.subspan(8, 4);
        for (usize home = 0; home < count; ++home)
        {
            const u32 target = le32(descriptor, 180 + home * 4);
            const usize at = home ? 44 + (home - 1) * 16 : 12;
            const bool escaped = jbd2::detail::be32(after[home], 0) == jbd2::magic;
            const u32 flags = (home ? jbd2::flag_same_uuid : 0) |
                (home + 1 == count ? jbd2::flag_last_tag : 0) |
                (escaped ? jbd2::flag_escape : 0);
            if (!target || jbd2::detail::be32(descriptor, at) != target ||
                jbd2::detail::be32(descriptor, at + 4) != flags ||
                jbd2::detail::be32(descriptor, at + 8) ||
                le32(descriptor, 220 + home * 4) !=
                    jbd2::detail::crc32c(~u32{0}, after[home]))
                return Status::io_error("orphan reclaim target or whole after-home binding");
            for (usize prior = 0; prior < home; ++prior)
                if (le32(descriptor, 180 + prior * 4) == target)
                    return Status::io_error("orphan reclaim duplicate target");
            checked = after[home];
            if (escaped)
                for (usize i = 0; i < 4; ++i) checked[i] = 0;
            if (jbd2::detail::be32(descriptor, at + 12) !=
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence_bytes), checked))
                return Status::io_error("orphan reclaim payload tag checksum");
            before[home] = after[home];
        }
        usize cursor = patch_begin, prior_home = 0, prior_end = 0;
        for (usize run = 0; run < runs; ++run)
        {
            if (cursor > limit || limit - cursor < 5)
                return Status::io_error("orphan reclaim truncated patch");
            const usize home = descriptor[cursor], offset = le16(descriptor, cursor + 1);
            const usize size = le16(descriptor, cursor + 3);
            cursor += 5;
            if (home >= count || offset >= 1024 || !size || size > 1024 - offset ||
                size > limit - cursor ||
                (run && (home < prior_home || (home == prior_home && offset <= prior_end))))
                return Status::io_error("orphan reclaim patch bounds or order");
            for (usize i = 0; i < size; ++i)
            {
                if (descriptor[cursor + i] == after[home][offset + i])
                    return Status::io_error("orphan reclaim patch includes unchanged byte");
                before[home][offset + i] = descriptor[cursor + i];
            }
            cursor += size; prior_home = home; prior_end = offset + size;
        }
        if (cursor != limit) return Status::io_error("orphan reclaim patch length");
        for (usize home = 0; home < count; ++home)
            if (le32(descriptor, 200 + home * 4) !=
                jbd2::detail::crc32c(~u32{0}, before[home]))
                return Status::io_error("orphan reclaim whole before-home checksum");
        if (auto status = decoded_identity(descriptor, expected_uuid, after,
                std::span<const Block>{before}, count, released); !status)
            return status;
        output.uuid = expected_uuid;
        output.inode = le32(descriptor, 160);
        output.old_generation = le32(descriptor, 164);
        output.retained_generation = le32(descriptor, 168);
        output.next_head = le32(descriptor, 172);
        output.sequence = sequence;
        output.count = count; output.released_blocks = released;
        output.runs = runs; output.bytes = limit - begin;
        for (usize i = 0; i < count; ++i) output.targets[i] = le32(descriptor, 180 + i * 4);
        for (usize i = 0; i < released; ++i)
            output.released_mapping[i] = le32(descriptor, 240 + i * 4);
        return {};
    }
};

struct Ext4OrphanReclaimJournalPlan
{
    using Block = Ext4OrphanBlock;
    Block journal_head{}, descriptor{}, commit{}, end{};
    std::array<Block, Ext4OrphanReclaimPlan::capacity> payloads{};
    std::array<u32, Ext4OrphanReclaimPlan::capacity> targets{};
    usize count{};
    u32 sequence{};
    bool ready{};
};

struct Ext4OrphanReclaimJournalScratch
{
    using Block = Ext4OrphanBlock;
    std::array<jbd2::ReplayAction, Ext4OrphanReclaimPlan::capacity> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    std::array<Block, Ext4OrphanReclaimPlan::capacity> before{};
    Block record{}, payload{};
};

// Pure, bounded format preparation for one head-reclaim transaction. It does
// not read a device, confer owner authority, or authorize runtime/cold replay.
class Ext4OrphanReclaimJournalPlanner final
{
    using Block = Ext4OrphanBlock;
    static void be32(std::span<u8> bytes, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) bytes[at + i] = u8(value >> (24 - i * 8)); }
    static void header(Block &bytes, jbd2::RecordType type, u32 sequence)
    {
        be32(bytes, 0, jbd2::magic); be32(bytes, 4, u32(type));
        be32(bytes, 8, sequence);
    }
    struct Records
    {
        const Ext4OrphanReclaimJournalPlan &plan;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024)
                return Status::invalid_argument("orphan reclaim journal model read size");
            const Block *record = nullptr;
            if (number == 0) record = &plan.journal_head;
            else if (number == 1) record = &plan.descriptor;
            else if (number >= 2 && number < plan.count + 2)
                record = &plan.payloads[number - 2];
            else if (number == plan.count + 2) record = &plan.commit;
            else if (number == plan.count + 3) record = &plan.end;
            if (!record) return Status::io_error("orphan reclaim journal model read bounds");
            for (usize i = 0; i < bytes.size(); ++i) bytes[i] = (*record)[i];
            return {};
        }
    };

  public:
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding,
        const Ext4OrphanReclaimPlan &reclaim, const Block &old_journal_head,
        u32 seconds, u32 nanoseconds, Ext4OrphanReclaimJournalScratch &scratch,
        Ext4OrphanReclaimJournalPlan &output)
    {
        output = {}; scratch = {};
        const auto reject = [&](Status status) { output = {}; return status; };
        if (!binding.needs_recovery || !binding.empty_log ||
            seconds > 0x7fffffffU || nanoseconds >= 1000000000U)
            return Status::unsupported("orphan reclaim journal admission or time");
        const auto codec = jbd2::Codec::open(old_journal_head,
            u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &journal = codec.value().superblock();
        if (journal.block_size != 1024 || journal.first_block != 1 ||
            journal.start_block || journal.sequence != binding.journal_sequence ||
            journal.sequence > 0xfffffffcU ||
            (journal.features != jbd2::feature_checksum_v3 &&
             journal.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)) ||
            (reclaim.count != 4 && reclaim.count != 5) ||
            journal.max_blocks <= reclaim.count + 3)
            return Status::unsupported("orphan reclaim journal ring or record capacity");
        output.count = reclaim.count;
        output.sequence = journal.sequence + 1;
        output.journal_head = old_journal_head;
        header(output.descriptor, jbd2::RecordType::descriptor, output.sequence);
        header(output.commit, jbd2::RecordType::commit, output.sequence);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const std::array<u8, 4> sequence{u8(output.sequence >> 24), u8(output.sequence >> 16),
                                         u8(output.sequence >> 8), u8(output.sequence)};
        for (usize index = 0; index < output.count; ++index)
        {
            output.targets[index] = reclaim.targets[index];
            auto &payload = output.payloads[index];
            payload = reclaim.after[index];
            u32 flags = index ? jbd2::flag_same_uuid : 0;
            if (index + 1 == output.count) flags |= jbd2::flag_last_tag;
            if (jbd2::detail::be32(payload, 0) == jbd2::magic)
            { be32(payload, 0, 0); flags |= jbd2::flag_escape; }
            const usize at = index ? 44 + (index - 1) * 16 : 12;
            be32(output.descriptor, at, output.targets[index]);
            be32(output.descriptor, at + 4, flags);
            be32(output.descriptor, at + 12,
                 jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), payload));
        }
        if (auto status = Ext4OrphanReclaimWitnessCodec::encode(
                binding, reclaim, output.sequence, output.descriptor); !status)
            return reject(status);
        be32(output.descriptor, 1020, jbd2::detail::crc32c(seed, output.descriptor));
        be32(output.commit, 52, seconds); be32(output.commit, 56, nanoseconds);
        be32(output.commit, 16, jbd2::detail::crc32c(seed, output.commit));
        be32(output.journal_head, 24, output.sequence);
        be32(output.journal_head, 28, 1);
        be32(output.journal_head, 252, 0);
        be32(output.journal_head, 252,
             jbd2::detail::crc32c(~u32{0}, output.journal_head));
        Ext4OrphanReclaimWitness witness{};
        if (auto status = Ext4OrphanReclaimWitnessCodec::decode(output.descriptor,
                binding.uuid, std::span<const Block>{reclaim.after}.first(output.count),
                std::span<Block>{scratch.before}.first(output.count), witness); !status)
            return reject(status);
        if (witness.sequence != output.sequence || witness.inode != reclaim.inode ||
            witness.old_generation != reclaim.old_generation ||
            witness.retained_generation != reclaim.retained_generation ||
            witness.next_head != reclaim.next_head ||
            witness.released_blocks != reclaim.released_blocks ||
            witness.count != reclaim.count)
            return reject(Status::fault("orphan reclaim reconstructed witness identity"));
        for (usize i = 0; i < output.count; ++i)
            if (witness.targets[i] != reclaim.targets[i] ||
                scratch.before[i] != reclaim.before[i])
                return reject(Status::fault("orphan reclaim reconstructed home differs"));
        for (usize i = 0; i < reclaim.released_blocks; ++i)
            if (witness.released_mapping[i] != reclaim.released_mapping[i])
                return reject(Status::fault("orphan reclaim reconstructed mapping differs"));
        const auto new_codec = jbd2::Codec::open(output.journal_head,
            u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!new_codec) return reject(new_codec.status());
        std::array<jbd2::BlockRange, Ext4OrphanReclaimPlan::capacity> allowed{};
        for (usize i = 0; i < output.count; ++i) allowed[i] = {output.targets[i], 1};
        Records records{output};
        const auto parsed = jbd2::build_replay_plan(new_codec.value(), records,
            {binding.layout.filesystem_blocks,
             std::span<const jbd2::BlockRange>{allowed}.first(output.count)},
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return reject(parsed.status());
        const auto &replay = parsed.value();
        if (replay.actions.size() != output.count || replay.committed_transactions != 1 ||
            replay.revoked_actions || replay.revoke_records || replay.incomplete_tail || replay.empty_log ||
            replay.next_sequence != output.sequence + 1)
            return reject(Status::fault("orphan reclaim generated journal shape"));
        for (usize i = 0; i < replay.actions.size(); ++i)
        {
            const auto &action = replay.actions[i];
            if (action.target_block != output.targets[i] ||
                action.journal_block != i + 2 || action.sequence != output.sequence ||
                action.transaction_order)
                return reject(Status::fault("orphan reclaim generated action identity"));
            for (usize byte = 0; byte < 1024; ++byte)
            {
                const u8 restored = action.restore_magic && byte < 4 ?
                    u8(jbd2::magic >> (24 - byte * 8)) : output.payloads[i][byte];
                if (restored != reclaim.after[i][byte])
                    return reject(Status::fault("orphan reclaim generated payload differs"));
            }
        }
        output.ready = true;
        return {};
    }
};

struct Ext4OrphanReclaimColdRecords
{
    const Ext4OrphanBlock &head, &descriptor, &commit, &end;
    std::span<const Ext4OrphanBlock> payloads{};
};
// Complete, immutable supplied observations. This borrowing contract does not
// establish device provenance; no caller-supplied orphan chain is trusted.
struct Ext4OrphanReclaimColdObserved
{
    const Ext4OrphanBlock &superblock, &descriptors;
    const std::array<Ext4OrphanBlock, 4> &inode_bitmaps, &block_bitmaps;
    std::array<std::span<const Ext4OrphanBlock>, 4> inode_tables{};
    std::span<const Ext4OrphanDirectoryBlockImage> directories{};
};

// Large caller-owned storage, disjoint from all input and never on kernel stack.
struct Ext4OrphanReclaimColdScratch
{
    std::array<std::array<Ext4OrphanBlock, 512>, 4> tables{};
    Ext4OrphanBlock superblock{}, descriptors{}, normalized{}, old_head{}, record{}, payload{};
    std::array<Ext4OrphanBlock, 4> inode_bitmaps{}, block_bitmaps{};
    std::array<Ext4OrphanInodeImage, Ext4OrphanChainProof::capacity> chain{};
    std::array<Ext4OrphanBlock, 5> before{}, after{}, current{};
    std::array<jbd2::ReplayAction, 5> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    std::array<std::span<const Ext4OrphanBlock>, 4> table_spans{};
    Ext4OrphanDirectoryQualifiedReclaimScratch qualified{};
    Ext4OrphanDirectoryQualifiedReclaimPlan reclaim{};
    Ext4OrphanReclaimJournalScratch formatter{};
    Ext4OrphanReclaimJournalPlan canonical{};
    Ext4RecoveryBinding bootstrap{};
    usize chain_count{};
};

struct Ext4OrphanReclaimColdResult
{
    std::array<u32, 5> targets{};
    std::array<bool, 5> home_after{};
    u32 inode{}, old_generation{}, retained_generation{}, next_head{};
    u32 active_sequence{}, parsed_next_sequence{}, cleanup_sequence{};
    usize count{}, old_homes{}, new_homes{}, witness_runs{}, witness_bytes{};
    bool valid{}, all_before{}, all_after{}, complete_global_before_proof{};
};

// Pure semantic proof of a declared preimage and an allowed observed mixture.
// CRC witnesses do not authenticate overwritten history. Live owners must bind
// original publication, and cold execution must seal actual whole device bytes.
class Ext4OrphanReclaimColdVerifier final
{
    using Block = Ext4OrphanBlock;
    struct Reader
    {
        const Ext4OrphanReclaimColdRecords &records;
        Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024) return Status::invalid_argument("reclaim cold journal read size");
            const Block *image{};
            if (!number) image = &records.head;
            else if (number == 1) image = &records.descriptor;
            else if (number >= 2 && number < records.payloads.size() + 2) image = &records.payloads[number - 2];
            else if (number == records.payloads.size() + 2) image = &records.commit;
            else if (number == records.payloads.size() + 3) image = &records.end;
            if (!image) return Status::io_error("reclaim cold journal read bounds");
            for (usize at = 0; at < 1024; ++at) bytes[at] = (*image)[at];
            return {};
        }
    };
    // Strict ordinary bootstrap sees only declared-before images and a private
    // zero-head superblock. The actual orphan bytes remain separately proved.
    class BeforeDevice final : public driver::BlockDevice
    {
      public:
        BeforeDevice(const Ext4RecoveryInfo &binding, const Ext4OrphanReclaimColdScratch &scratch)
            : binding_(binding), scratch_(scratch) {}
        driver::BlockGeometry geometry() const override { return {65536, 512, false}; }
        Status read_blocks(u64 first, std::span<std::byte> out) override
        {
            if (first % 2 || out.size() != 1024 || first / 2 >= 32768)
                return Status::invalid_argument("reclaim cold before bootstrap bounds");
            const u32 physical = u32(first / 2);
            const Block *image{};
            if (physical == 1) image = &scratch_.normalized;
            else if (physical == 2) image = &scratch_.descriptors;
            else if (physical == binding_.layout.journal_first_block) image = &scratch_.old_head;
            else for (usize group = 0; group < 4; ++group)
            {
                const auto &metadata = binding_.metadata;
                if (physical == metadata[group * 4 + 1].first) image = &scratch_.block_bitmaps[group];
                if (physical == metadata[group * 4 + 2].first) image = &scratch_.inode_bitmaps[group];
                const auto table = metadata[group * 4 + 3];
                if (physical >= table.first && physical - table.first < scratch_.table_spans[group].size())
                    image = &scratch_.table_spans[group][physical - table.first];
            }
            if (!image) return Status::io_error("reclaim cold before bootstrap image absent");
            for (usize at = 0; at < 1024; ++at) out[at] = std::byte((*image)[at]);
            return {};
        }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("reclaim cold before bootstrap is readonly"); }
        Status flush() override { return Status::denied("reclaim cold before bootstrap is readonly"); }
      private:
        const Ext4RecoveryInfo &binding_;
        const Ext4OrphanReclaimColdScratch &scratch_;
    };
    static void little32(Block &block, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) block[at + index] = u8(value >> (8 * index)); }
    static void big32(Block &block, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) block[at + index] = u8(value >> (24 - 8 * index)); }
    [[nodiscard]] static Status chain(Ext4OrphanReclaimColdScratch &scratch)
    {
        u32 number = ext4_checked::le32(scratch.superblock, 232);
        scratch.chain_count = 0;
        while (number)
        {
            if (number < 11 || number > 8192 || scratch.chain_count == scratch.chain.size())
                return Status::unsupported("reclaim cold declared chain bounds");
            for (usize prior = 0; prior < scratch.chain_count; ++prior)
                if (scratch.chain[prior].number == number)
                    return Status::io_error("reclaim cold declared chain cycle");
            const u32 group = (number - 1) / 2048, local = (number - 1) % 2048;
            if (scratch.table_spans[group].size() != 512)
                return Status::invalid_argument("reclaim cold declared chain table absent");
            auto &image = scratch.chain[scratch.chain_count++];
            image.number = number; image.table_block = ext4_checked::le32(scratch.descriptors, group * 32 + 8) + local / 4;
            image.table = scratch.tables[group][local / 4];
            const auto inode = std::span<const u8>{image.table}.subspan((local % 4) * 256, 256);
            image.expected_generation = ext4_checked::le32(inode, 100);
            number = ext4_checked::le32(inode, 20);
        }
        return {};
    }
    [[nodiscard]] static const Block *observed_home(const Ext4RecoveryInfo &binding,
        const Ext4OrphanReclaimColdObserved &observed, u32 physical)
    {
        if (physical == 1) return &observed.superblock;
        if (physical == 2) return &observed.descriptors;
        for (usize group = 0; group < 4; ++group)
        {
            if (physical == binding.metadata[group * 4 + 1].first) return &observed.block_bitmaps[group];
            if (physical == binding.metadata[group * 4 + 2].first) return &observed.inode_bitmaps[group];
            const auto table = binding.metadata[group * 4 + 3];
            if (physical >= table.first && physical - table.first < observed.inode_tables[group].size())
                return &observed.inode_tables[group][physical - table.first];
        }
        return nullptr;
    }
    [[nodiscard]] static Block *before_home(const Ext4RecoveryInfo &binding,
        Ext4OrphanReclaimColdScratch &scratch, u32 physical)
    {
        if (physical == 1) return &scratch.superblock;
        if (physical == 2) return &scratch.descriptors;
        for (usize group = 0; group < 4; ++group)
        {
            if (physical == binding.metadata[group * 4 + 1].first) return &scratch.block_bitmaps[group];
            if (physical == binding.metadata[group * 4 + 2].first) return &scratch.inode_bitmaps[group];
            const auto table = binding.metadata[group * 4 + 3];
            if (physical >= table.first && physical - table.first < scratch.table_spans[group].size())
                return &scratch.tables[group][physical - table.first];
        }
        return nullptr;
    }

  public:
    [[nodiscard]] static Status verify(const Ext4RecoveryInfo &binding,
        const Ext4OrphanReclaimColdRecords &records, const Ext4OrphanReclaimColdObserved &observed,
        Ext4OrphanReclaimColdScratch &scratch, Ext4OrphanReclaimColdResult &output)
    {
        output = {};
        scratch.chain_count = 0; scratch.table_spans = {}; scratch.reclaim = {}; scratch.canonical = {};
        const usize count = records.payloads.size();
        const auto &layout = binding.layout;
        if (!binding.needs_recovery || binding.empty_log || count < 4 || count > 5 ||
            layout.first_sector != 0 || layout.block_size != 1024 || layout.filesystem_blocks != 32768 || binding.journal_inode != 8 ||
            binding.metadata_count != 16 || !binding.structural_metadata().valid() ||
            layout.journal_first_block < 3 || layout.journal_first_block >= 32768 ||
            layout.journal_blocks < 1024 || layout.journal_blocks > 32768 - layout.journal_first_block ||
            observed.directories.size() > Ext4OrphanDirectoryReferenceScratch::block_capacity)
            return Status::invalid_argument("reclaim cold input profile");
        for (usize group = 0; group < 4; ++group)
            if (!observed.inode_tables[group].empty() && observed.inode_tables[group].size() != 512)
                return Status::invalid_argument("reclaim cold incomplete observed table");
        const auto codec = jbd2::Codec::open(records.head, u32(layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &head = codec.value().superblock();
        if (head.block_size != 1024 || head.first_block != 1 || head.start_block != 1 ||
            head.sequence < 2 || head.sequence > 0xfffffffdU || head.sequence != binding.journal_sequence)
            return Status::unsupported("reclaim cold active journal sequence or ring");
        const u64 journal_end = layout.journal_first_block + layout.journal_blocks;
        const std::array<jbd2::BlockRange, 2> domain{{{0, layout.journal_first_block},
            {journal_end, layout.filesystem_blocks - journal_end}}};
        Reader reader{records};
        const auto parsed = jbd2::build_replay_plan(codec.value(), reader,
            {32768, std::span<const jbd2::BlockRange>{domain}.first(journal_end < 32768 ? 2 : 1)},
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return parsed.status();
        const auto &replay = parsed.value();
        if (replay.empty_log || replay.incomplete_tail || replay.committed_transactions != 1 || replay.revoked_actions || replay.revoke_records ||
            replay.actions.size() != count || replay.next_sequence != head.sequence + 1)
            return Status::io_error("reclaim cold journal grammar");
        for (usize index = 0; index < count; ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block > 0xffffffffULL || action.journal_block != index + 2 ||
                action.sequence != head.sequence || action.transaction_order)
                return Status::io_error("reclaim cold action identity");
            scratch.after[index] = records.payloads[index];
            if (action.restore_magic) big32(scratch.after[index], 0, jbd2::magic);
        }
        Ext4OrphanReclaimWitness witness{};
        if (auto status = Ext4OrphanReclaimWitnessCodec::decode(records.descriptor, binding.uuid,
                std::span<const Block>{scratch.after}.first(count), std::span<Block>{scratch.before}.first(count), witness); !status)
            return status;
        if (witness.count != count || witness.sequence != head.sequence)
            return Status::io_error("reclaim cold witnessed sequence or count");

        scratch.superblock = observed.superblock; scratch.descriptors = observed.descriptors;
        scratch.inode_bitmaps = observed.inode_bitmaps; scratch.block_bitmaps = observed.block_bitmaps;
        for (usize group = 0; group < 4; ++group)
            if (!observed.inode_tables[group].empty())
            {
                for (usize index = 0; index < 512; ++index) scratch.tables[group][index] = observed.inode_tables[group][index];
                scratch.table_spans[group] = scratch.tables[group];
            }
        Ext4OrphanReclaimColdResult result{};
        result.count = count;
        for (usize index = 0; index < count; ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block != witness.targets[index]) return Status::io_error("reclaim cold witnessed target differs");
            const auto *current = observed_home(binding, observed, witness.targets[index]);
            auto *old = before_home(binding, scratch, witness.targets[index]);
            if (!current || !old) return Status::io_error("reclaim cold observed home absent");
            scratch.current[index] = *current;
            if (*current == scratch.after[index]) { result.home_after[index] = true; ++result.new_homes; }
            else if (*current == scratch.before[index]) ++result.old_homes;
            else return Status::io_error("reclaim cold foreign whole home");
            result.targets[index] = witness.targets[index]; *old = scratch.before[index];
        }
        // A tombstone must be durable before either allocation bitmap may free
        // its identity. The superblock head advances only after allocation homes.
        for (usize index = 1; index < count - 1; ++index)
            if (result.home_after[index] && !result.home_after[0])
                return Status::io_error("reclaim cold allocation precedes tombstone barrier");
        if (result.home_after[count - 1])
            for (usize index = 0; index < count - 1; ++index)
                if (!result.home_after[index]) return Status::io_error("reclaim cold head precedes allocation barrier");

        Ext4RecoveryInfo old_binding = binding;
        old_binding.empty_log = true; old_binding.journal_sequence = head.sequence - 1;
        scratch.old_head = records.head;
        big32(scratch.old_head, 24, old_binding.journal_sequence); big32(scratch.old_head, 28, 0);
        big32(scratch.old_head, 252, 0);
        big32(scratch.old_head, 252, jbd2::detail::crc32c(~u32{0}, scratch.old_head));
        scratch.normalized = scratch.superblock;
        little32(scratch.normalized, 232, 0);
        little32(scratch.normalized, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{scratch.normalized}.first(1020)));
        scratch.bootstrap.~Ext4RecoveryBinding();
        ::new (static_cast<void *>(&scratch.bootstrap)) Ext4RecoveryBinding;
        BeforeDevice before_device(old_binding, scratch);
        if (auto status = scratch.bootstrap.inspect(before_device, {0, 65536}); !status) return status;
        const auto *strict = scratch.bootstrap.info();
        if (!strict || strict->uuid != old_binding.uuid || strict->journal_inode != 8 || !strict->needs_recovery || !strict->empty_log ||
            strict->journal_sequence != old_binding.journal_sequence || strict->metadata_count != 16 ||
            strict->layout.journal_first_block != layout.journal_first_block || strict->layout.journal_blocks != layout.journal_blocks)
            return Status::io_error("reclaim cold strict before binding differs");
        for (usize index = 0; index < 16; ++index)
            if (strict->metadata[index].first != old_binding.metadata[index].first ||
                strict->metadata[index].count != old_binding.metadata[index].count)
                return Status::io_error("reclaim cold strict before metadata differs");
        if (auto status = chain(scratch); !status) return status;
        const Ext4OrphanDirectoryReferenceSource old_source{{{scratch.superblock, scratch.descriptors,
            scratch.inode_bitmaps, scratch.block_bitmaps,
            std::span<const Ext4OrphanInodeImage>{scratch.chain}.first(scratch.chain_count)}, scratch.table_spans}, observed.directories};
        if (auto status = Ext4OrphanDirectoryQualifiedReclaimPlanner::plan(old_binding, old_source, scratch.qualified,
                witness.inode, witness.old_generation, scratch.reclaim); !status) return status;
        if (!scratch.reclaim.valid || scratch.reclaim.reclaim.count != count)
            return Status::fault("reclaim cold complete before plan absent");
        const auto &homes = scratch.reclaim.reclaim;
        if (auto status = Ext4OrphanReclaimJournalPlanner::plan(old_binding, homes, scratch.old_head,
                jbd2::detail::be32(records.commit, 52), jbd2::detail::be32(records.commit, 56), scratch.formatter, scratch.canonical); !status)
            return status;
        const auto &canonical = scratch.canonical;
        if (!canonical.ready || canonical.count != count || canonical.sequence != head.sequence ||
            canonical.journal_head != records.head || canonical.descriptor != records.descriptor ||
            canonical.commit != records.commit || canonical.end != records.end)
            return Status::io_error("reclaim cold canonical journal differs");
        for (usize index = 0; index < count; ++index)
            if (homes.targets[index] != witness.targets[index] || canonical.targets[index] != witness.targets[index] ||
                homes.before[index] != scratch.before[index] || homes.after[index] != scratch.after[index] ||
                canonical.payloads[index] != records.payloads[index])
                return Status::io_error("reclaim cold canonical whole images differ");
        if (homes.released_blocks != witness.released_blocks || homes.released_mapping != witness.released_mapping ||
            homes.retained_generation != witness.retained_generation || homes.next_head != witness.next_head)
            return Status::io_error("reclaim cold canonical released identity differs");
        result.inode = witness.inode; result.old_generation = witness.old_generation;
        result.retained_generation = witness.retained_generation; result.next_head = witness.next_head;
        result.active_sequence = head.sequence; result.parsed_next_sequence = replay.next_sequence;
        result.cleanup_sequence = replay.next_sequence + 1;
        result.witness_runs = witness.runs; result.witness_bytes = witness.bytes;
        result.all_before = result.old_homes == count; result.all_after = result.new_homes == count;
        result.complete_global_before_proof = true; result.valid = true;
        output = result;
        return {};
    }
};

template <typename> class Ext4ZeroTruncateTransaction;
template <typename> class Ext4AllocatedEmptyDataTransaction;
}

namespace ok::fs::reclaim_history
{
// Independent borrow anchors are never copied from a public report or a result seal.
// One drifting pointer cannot redirect rejection to an unrelated live Session.
template <typename Session> class SessionAnchor final
{
    template <typename> friend class ::ok::fs::Ext4ZeroTruncateTransaction;
    template <typename> friend class ::ok::fs::Ext4AllocatedEmptyDataTransaction;
  public:
    SessionAnchor() = default;
    SessionAnchor(const SessionAnchor &) = delete;
    SessionAnchor &operator=(const SessionAnchor &) = delete;
    SessionAnchor(SessionAnchor &&) = delete;
    SessionAnchor &operator=(SessionAnchor &&) = delete;
    [[nodiscard]] Status bind(Session &session)
    {
        if (retired_ || first_ || second_ || third_) return Status::already_exists("original Session anchor is one-shot");
        first_ = second_ = third_ = &session; return {};
    }
    [[nodiscard]] bool exact(const Session *session) const
    { return !retired_ && session && first_ == session && second_ == session && third_ == session; }
    [[nodiscard]] Result<block::SessionBindingObservation> observe_binding() const
        requires requires(Session &session) { session.observe_binding(); }
    {
        Session *original = agreed();
        return original ? original->observe_binding() :
            Result<block::SessionBindingObservation>{Status::fault("original Session anchors have no agreement")};
    }
    // Inert post-quarantine observation uses the independently agreed actual
    // Session, never a drifting transaction pointer or an observation as a lease.
    [[nodiscard]] Result<block::SessionBindingObservation> observe_terminal_binding() const
        requires requires(Session &session) { session.observe_terminal_binding(); }
    {
        Session *original = agreed();
        return original ? original->observe_terminal_binding() :
            Result<block::SessionBindingObservation>{Status::fault("original Session anchors have no agreement")};
    }
    [[nodiscard]] Status abort(Status status) const
    {
        Session *original = agreed();
        return original ? original->abort(status) : Status::fault("original Session anchors have no agreement");
    }
    // Rejection diagnostics use the same independently agreed original for
    // both quarantine/drain and final readback. Minority pointer drift never
    // authorizes further dispatch or exposes an alternate route to the caller.
    [[nodiscard]] Result<block::SessionReport> abort_observe(Status status) const
    {
        Session *original = agreed();
        if (!original) return Status::fault("original Session anchors have no agreement");
        static_cast<void>(original->abort(status));
        return original->report();
    }
  private:
    // Only the concrete failed typed child retires a proven drained original.
    // This removes destruction-time borrows without resetting failed history
    // or making the anchor available for a second Session.
    [[nodiscard]] Status retire_aborted_original(Session &session)
        requires requires(Session &value) { value.observe_terminal_binding(); }
    {
        if (!exact(&session)) return Status::bad_descriptor("terminal retirement lacks its original Session");
        const auto actual = session.report();
        if (!actual) return actual.status();
        const auto &report = actual.value();
        if (report.state != block::DeviceState::quarantined || report.first_error == StatusCode::ok || report.in_flight)
            return Status::busy("terminal retirement requires a quarantined drained Session");
        const auto binding = session.observe_terminal_binding();
        if (!binding) return binding.status();
        if (binding.value().session != &session || binding.value().identity != report.identity ||
            binding.value().registration_generation != report.registration_generation ||
            binding.value().session_serial != report.session_serial)
            return Status::fault("terminal retirement original native binding differs");
        first_ = second_ = third_ = nullptr;
        retired_ = true;
        return {};
    }
    [[nodiscard]] Session *agreed() const
    { return retired_ ? nullptr : first_ == second_ || first_ == third_ ? first_ : second_ == third_ ? second_ : nullptr; }
    Session *first_{}, *second_{}, *third_{};
    bool retired_{};
};

// Compare declared fields, including unused retained entries; never padding.
inline bool same_status(const Status &a, const Status &b)
{
    return a.code() == b.code() && a.message().data() == b.message().data() &&
        a.message().size() == b.message().size();
}
inline bool same_session(const block::SessionReport &a, const block::SessionReport &b)
{
    return a.state == b.state && a.first_error == b.first_error && a.identity == b.identity &&
        a.registration_generation == b.registration_generation && a.session_serial == b.session_serial &&
        a.reads == b.reads && a.writes == b.writes && a.flushes == b.flushes &&
        a.completed_reads == b.completed_reads && a.completed_writes == b.completed_writes &&
        a.completed_flushes == b.completed_flushes && a.in_flight == b.in_flight &&
        a.pending_writes == b.pending_writes;
}
inline bool same_info(const Ext4RecoveryInfo &a, const Ext4RecoveryInfo &b)
{
    if (a.metadata_count > a.metadata.size() || b.metadata_count > b.metadata.size() ||
        a.layout.first_sector != b.layout.first_sector || a.layout.filesystem_blocks != b.layout.filesystem_blocks ||
        a.layout.journal_first_block != b.layout.journal_first_block || a.layout.journal_blocks != b.layout.journal_blocks ||
        a.layout.block_size != b.layout.block_size || a.uuid != b.uuid || a.metadata_count != b.metadata_count ||
        a.journal_inode != b.journal_inode || a.journal_sequence != b.journal_sequence ||
        a.needs_recovery != b.needs_recovery || a.empty_log != b.empty_log) return false;
    for (usize index = 0; index < a.metadata.size(); ++index)
        if (a.metadata[index].first != b.metadata[index].first || a.metadata[index].count != b.metadata[index].count)
            return false;
    return true;
}
inline bool same_result(const Ext4OrphanReclaimColdResult &a, const Ext4OrphanReclaimColdResult &b)
{
    return a.count <= a.targets.size() && b.count <= b.targets.size() && a.targets == b.targets &&
        a.home_after == b.home_after && a.inode == b.inode && a.old_generation == b.old_generation &&
        a.retained_generation == b.retained_generation && a.next_head == b.next_head &&
        a.active_sequence == b.active_sequence && a.parsed_next_sequence == b.parsed_next_sequence &&
        a.cleanup_sequence == b.cleanup_sequence && a.count == b.count && a.old_homes == b.old_homes &&
        a.new_homes == b.new_homes && a.witness_runs == b.witness_runs && a.witness_bytes == b.witness_bytes &&
        a.valid == b.valid && a.all_before == b.all_before && a.all_after == b.all_after &&
        a.complete_global_before_proof == b.complete_global_before_proof;
}
inline bool same_plan(const Ext4OrphanReclaimPlan &a, const Ext4OrphanReclaimPlan &b)
{
    return a.count <= a.targets.size() && b.count <= b.targets.size() && a.inode == b.inode &&
        a.old_generation == b.old_generation && a.retained_generation == b.retained_generation &&
        a.next_head == b.next_head && a.released_blocks == b.released_blocks &&
        a.released_mapping == b.released_mapping && a.targets == b.targets &&
        a.before == b.before && a.after == b.after && a.count == b.count;
}
inline bool same_journal(const Ext4OrphanReclaimJournalPlan &a, const Ext4OrphanReclaimJournalPlan &b)
{
    return a.count <= a.targets.size() && b.count <= b.targets.size() && a.journal_head == b.journal_head &&
        a.descriptor == b.descriptor && a.commit == b.commit && a.end == b.end && a.payloads == b.payloads &&
        a.targets == b.targets && a.count == b.count && a.sequence == b.sequence && a.ready == b.ready;
}
inline bool same_directory(const Ext4OrphanDirectoryReferenceProof &a, const Ext4OrphanDirectoryReferenceProof &b)
{
    return a.filesystem_uuid == b.filesystem_uuid && a.observed_head == b.observed_head &&
        a.directory_inodes == b.directory_inodes && a.directory_blocks == b.directory_blocks &&
        a.live_entries == b.live_entries && a.valid == b.valid;
}
} // namespace ok::fs::reclaim_history
