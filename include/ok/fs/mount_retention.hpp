#pragma once

#include "ok/core/types.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <utility>

namespace ok::fs
{
class Node;
class VirtualFileSystem;
template <usize> class Ext4MountedMetadata;
class MountedRootRetentionDomain;

// The issuer and its external transport storage must outlive every reference.
// Calls and final storage destruction are externally serialized. This token
// retains a publication obligation; it cannot extend destroyed stack storage.
class MountedRootReference final
{
    friend class MountedRootRetentionDomain;
    friend class VirtualFileSystem;
  public:
    MountedRootReference() = default;
    MountedRootReference(const MountedRootReference &) = delete;
    MountedRootReference &operator=(const MountedRootReference &) = delete;
    MountedRootReference &operator=(MountedRootReference &&) = delete;
    MountedRootReference(MountedRootReference &&other) noexcept;
    ~MountedRootReference() { if (original_anchor_) static_cast<void>(release()); }
    [[nodiscard]] bool valid() const { return original_anchor_ && serial_; }
    [[nodiscard]] Result<MountedRootReference> clone() const;
    [[nodiscard]] Status release();
  private:
    MountedRootReference(MountedRootRetentionDomain &domain, Node *root,
                         const void *provider, u64 session, u64 generation, u64 serial)
        : original_anchor_(&domain), domain_(&domain), root_(root), provider_(provider),
          session_(session), generation_(generation), serial_(serial) {}
    [[nodiscard]] Status validate_for_mount() const;
    [[nodiscard]] Result<Node *> node_for_mount() const;
    void clear()
    {
        original_anchor_ = nullptr; domain_ = nullptr; root_ = nullptr; provider_ = nullptr;
        session_ = generation_ = serial_ = 0;
    }
    // Keep original failure attribution independent of the checked attachment.
    MountedRootRetentionDomain *original_anchor_{};
    MountedRootRetentionDomain *domain_{};
    Node *root_{};
    const void *provider_{};
    u64 session_{}, generation_{}, serial_{};
};

// Concrete, bounded bookkeeping; only mounted providers can issue references.
// Each record also names its actual token object, so a copied byte image cannot
// consume the original object's record. Move relocates that record explicitly.
class MountedRootRetentionDomain final
{
    template <usize> friend class Ext4MountedMetadata;
    friend class MountedRootReference;
  public:
    static constexpr usize capacity = 32;
    MountedRootRetentionDomain() = default;
    MountedRootRetentionDomain(const MountedRootRetentionDomain &) = delete;
    MountedRootRetentionDomain &operator=(const MountedRootRetentionDomain &) = delete;
    MountedRootRetentionDomain(MountedRootRetentionDomain &&) = delete;
    MountedRootRetentionDomain &operator=(MountedRootRetentionDomain &&) = delete;
  private:
    struct Record { u64 serial{}; const MountedRootReference *holder{}; };
    [[nodiscard]] Status latch(Status reason) const
    { if (failure_) failure_ = reason; return failure_; }
    [[nodiscard]] Status audit_locked() const
    {
        if (!failure_) return failure_;
        if (live_slots_ >> capacity) return latch(Status::fault("mounted root occupancy exceeds capacity"));
        for (usize i = 0; i < records_.size(); ++i)
        {
            const auto &record = records_[i];
            const bool attached = live_slots_ & (u64{1} << i);
            if (bool(record.serial) != attached || bool(record.holder) != attached || record.serial > last_serial_)
                return latch(Status::fault("mounted root record differs"));
            if (record.serial)
                for (usize j = i + 1; j < records_.size(); ++j)
                    if (records_[j].serial == record.serial || records_[j].holder == record.holder)
                        return latch(Status::fault("mounted root record is duplicated"));
        }
        return {};
    }
    [[nodiscard]] Status bind(Node *root, const void *provider, u64 session, u64 generation)
    {
        smp::ScopedSpinLock guard(lock_);
        if (root_ || !root || !provider || !session || !generation)
            return latch(Status::fault("mounted root domain binding differs"));
        root_ = root; provider_ = provider; session_ = session; generation_ = generation;
        accepting_ = true; return {};
    }
    [[nodiscard]] Status binding(Node *root, const void *provider, u64 session, u64 generation) const
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = audit_locked(); !status) return status;
        if (root_ != root || provider_ != provider || session_ != session || generation_ != generation)
            return latch(Status::fault("mounted root original binding differs"));
        return {};
    }
    [[nodiscard]] Record *record_locked(const MountedRootReference &reference)
    {
        if (reference.original_anchor_ != this || reference.domain_ != this ||
            reference.root_ != root_ || reference.provider_ != provider_ ||
            reference.session_ != session_ || reference.generation_ != generation_ || !reference.serial_)
        { static_cast<void>(latch(Status::fault("mounted root reference attachment differs"))); return nullptr; }
        for (auto &record : records_)
            if (record.serial == reference.serial_ && record.holder == &reference) return &record;
        static_cast<void>(latch(Status::fault("mounted root reference has no exact record"))); return nullptr;
    }
    [[nodiscard]] Status validate(const MountedRootReference &reference)
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = audit_locked(); !status) return status;
        return record_locked(reference) ? Status{} : failure_;
    }
    [[nodiscard]] Result<MountedRootReference> issue()
    {
        MountedRootReference reference;
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = audit_locked(); !status) return status;
            if (!accepting_) return Status::busy("mounted root domain is draining");
            if (last_serial_ == ~u64{0}) return Status::overflow("mounted root serial exhausted");
            for (usize slot = 0; slot < records_.size(); ++slot)
                if (!records_[slot].serial)
                {
                    reference.original_anchor_ = reference.domain_ = this;
                    reference.root_ = root_; reference.provider_ = provider_;
                    reference.session_ = session_; reference.generation_ = generation_;
                    reference.serial_ = ++last_serial_;
                    records_[slot] = {reference.serial_, &reference};
                    live_slots_ |= u64{1} << slot; break;
                }
            if (!reference.valid()) return Status::no_memory("mounted root reference capacity");
        }
        // Moves relocate the actual holder after releasing the domain lock.
        return reference;
    }
    [[nodiscard]] Status audit() const
    { smp::ScopedSpinLock guard(lock_); return audit_locked(); }
    [[nodiscard]] Result<MountedRootReference> clone(const MountedRootReference &reference)
    {
        if (auto status = validate(reference); !status) return status;
        return issue();
    }
    void relocate(const MountedRootReference &source, const MountedRootReference &destination)
    {
        smp::ScopedSpinLock guard(lock_);
        if (!audit_locked()) return;
        if (auto *record = record_locked(source)) record->holder = &destination;
    }
    [[nodiscard]] Status consume(const MountedRootReference &reference)
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = audit_locked(); !status) return status;
        auto *record = record_locked(reference);
        if (!record) return failure_;
        live_slots_ &= ~(u64{1} << usize(record - records_.data()));
        *record = {}; return {};
    }
    [[nodiscard]] Status empty() const
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = audit_locked(); !status) return status;
        for (const auto &record : records_)
            if (record.serial) return Status::busy("mounted root reference retained");
        return {};
    }
    [[nodiscard]] Status begin_drain()
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = audit_locked(); !status) return status;
        for (const auto &record : records_)
            if (record.serial) return Status::busy("mounted root reference retained");
        accepting_ = false; return {};
    }
    void stop_admission()
    {
        smp::ScopedSpinLock guard(lock_);
        // A media/service failure closes admission while authentic existing
        // leases still consume normally. Only domain corruption latches the
        // failure that preserves unconsumable publication evidence.
        accepting_ = false;
    }
    Node *root_{};
    const void *provider_{};
    u64 session_{}, generation_{}, last_serial_{};
    bool accepting_{};
    std::array<Record, capacity> records_{};
    // Independent attachment occupancy detects an erased/rebuilt lease record.
    u64 live_slots_{};
    mutable Status failure_{};
    mutable smp::SpinLock lock_{};
};

inline MountedRootReference::MountedRootReference(MountedRootReference &&other) noexcept
    : original_anchor_(other.original_anchor_), domain_(other.domain_), root_(other.root_),
      provider_(other.provider_), session_(other.session_), generation_(other.generation_), serial_(other.serial_)
{
    if (original_anchor_) original_anchor_->relocate(other, *this);
    other.clear();
}
inline Result<MountedRootReference> MountedRootReference::clone() const
{
    if (!original_anchor_) return Status::bad_descriptor("mounted root reference is consumed");
    return original_anchor_->clone(*this);
}
inline Status MountedRootReference::release()
{
    if (!original_anchor_) return Status::bad_descriptor("mounted root reference is consumed");
    const auto status = original_anchor_->consume(*this);
    clear(); return status;
}
inline Status MountedRootReference::validate_for_mount() const
{
    if (!original_anchor_) return Status::bad_descriptor("mounted root reference is consumed");
    return original_anchor_->validate(*this);
}
inline Result<Node *> MountedRootReference::node_for_mount() const
{
    if (auto status = validate_for_mount(); !status) return status;
    return root_;
}
} // namespace ok::fs
