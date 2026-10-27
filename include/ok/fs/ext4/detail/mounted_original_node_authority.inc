// Original Node authority and Lease lifetime for privately selected typed work.
class TypedOriginalNodeAuthority final
{
    friend class Ext4MountedMetadata;
    TypedOriginalNodeAuthority(Ext4MountedMetadata &mount, const NodeMutationContext &original)
        : mount_(mount), original_(original) {}
    TypedOriginalNodeAuthority(const TypedOriginalNodeAuthority &) = delete;
    TypedOriginalNodeAuthority &operator=(const TypedOriginalNodeAuthority &) = delete;
    TypedOriginalNodeAuthority(TypedOriginalNodeAuthority &&) = delete;
    TypedOriginalNodeAuthority &operator=(TypedOriginalNodeAuthority &&) = delete;
  public:
    [[nodiscard]] Status validate(Ext4MountedTypedCursorPhase, Ext4MountedTypedSuffixStage) const
    { return mount_.identity(original_.epoch, &original_); }
    [[nodiscard]] Status complete_stage(Ext4MountedTypedSuffixStage) const
    { return Status::unsupported("original Node guard has no suffix semantic completion"); }
  private:
    Ext4MountedMetadata &mount_;
    const NodeMutationContext &original_;
};

// This exception supplies no writer admission or successful no-op proof.
// The selected finite cursor still classifies and proves the actual no-op.
[[nodiscard]] Result<u64> typed_node_admission_locked(Ext4MountedTypedOpening opening)
{
    if (opening != Ext4MountedTypedOpening::zero_family &&
        opening != Ext4MountedTypedOpening::allocated_family &&
        opening != Ext4MountedTypedOpening::noop_only)
        return Status::unsupported("typed Node admission requires a mutation opening");
    if (opening != Ext4MountedTypedOpening::noop_only)
        return update_admission_locked(Ext4MountedPolicy::ordered_data_dirty_noatime);
    if (auto status = admission(); !status) return status;
    if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime)
        return Status::unsupported("typed no-op mutation policy unavailable");
    if (pending_orphan_) return Status::busy("mounted orphan cleanup precedes typed no-op");
    if (report_.reading) return Status::busy("mounted reader precedes typed no-op");
    const auto buffers = owner_.buffers();
    if (buffers.read_pins || buffers.in_flight)
        return Status::busy("mounted canonical reader precedes typed no-op");
    return report_.epoch;
}

template <typename Operation>
[[nodiscard]] Status typed_node_mutation_scope(MountedNode &node,
    Ext4MountedTypedOpening opening, Operation operation)
{
    return typed_node_mutation_scope_select(node, [opening](u64) { return opening; },
        [&](const NodeMutationContext &original, TypedOriginalNodeAuthority &authority,
            Ext4MountedTypedOpening) { return operation(original, authority); });
}

// Select the request opening under the actual admission lock. Fixed-opening
// callers retain their original behavior through the wrapper above.
template <typename Selection, typename Operation>
[[nodiscard]] Status typed_node_mutation_scope_select(MountedNode &node,
    Selection select, Operation operation)
{
    std::optional<Lease> held;
    std::optional<NodeMutationContext> original;
    u64 epoch{};
    Ext4MountedTypedOpening opening{};
    {
        smp::ScopedSpinLock guard(lock_);
        opening = select(report_.epoch);
        const auto admitted = typed_node_admission_locked(opening);
        if (!admitted) return admitted.status();
        if (auto status = authentic_node_locked(node); !status) return status;
        const auto key = node.lifetime_key_;
        auto retained = retain_existing(key, Ext4InodeHold::io);
        if (!retained) return retained.status();
        held.emplace(std::move(retained).value());
        epoch = admitted.value();
        original.emplace(node, key, *held, epoch);
        mutation_node_ = &node; mutation_key_ = key; mutation_lease_ = &*held;
        mutation_context_ = &*original; report_.phase = Ext4MountPhase::updating;
        if (auto status = lifetime_.claim_node_mutation(*held, key, &*original, epoch); !status)
        {
            const auto released = lifetime_.release(*held);
            mutation_node_ = nullptr; mutation_key_ = {}; mutation_lease_ = nullptr;
            mutation_context_ = nullptr;
            return fail_locked(released ? status : released);
        }
    }
    owner_.publication_domain_ = epoch;
    TypedOriginalNodeAuthority authority(*this, *original);
    auto status = authority.validate(Ext4MountedTypedCursorPhase::unused,
                                     Ext4MountedTypedSuffixStage::none);
    if (!status) status = fail(status);
    else status = operation(*original, authority, opening);
    {
        smp::ScopedSpinLock guard(lock_);
        auto integrity = mutation_integrity_locked(&*original);
        if (report_.reading) integrity = Status::fault("reader entered original typed Node mutation");
        if (report_.phase != Ext4MountPhase::failed)
        {
            const bool same_epoch = report_.epoch == epoch;
            const bool next_epoch = opening != Ext4MountedTypedOpening::noop_only &&
                epoch != ~u64{0} && report_.epoch == epoch + 1;
            if (report_.phase != Ext4MountPhase::updating || (!same_epoch && !next_epoch))
                integrity = Status::fault("original typed Node terminal epoch changed");
            if (integrity)
            {
                // No pending Attachment may outlive the actual local Lease.
                const auto drained = owner_.mounted_teardown_preflight();
                if (!drained) integrity = drained;
            }
        }
        const auto released = lifetime_.release_node_mutation(*held, original->key,
                                                             &*original, original->epoch);
        mutation_node_ = nullptr; mutation_key_ = {}; mutation_lease_ = nullptr;
        mutation_context_ = nullptr;
        if (!integrity || !released) status = fail_locked(!integrity ? integrity : released);
        else if (report_.phase != Ext4MountPhase::failed) report_.phase = Ext4MountPhase::readable;
    }
    const auto cleanup = drain_pending_orphan();
    return cleanup ? status : cleanup;
}
