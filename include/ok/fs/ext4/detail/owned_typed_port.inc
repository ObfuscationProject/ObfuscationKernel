// Private canonical-owner attachment. All workspace storage is retained by the
// synchronous caller; Registry, device and raw-writer exclusion remain external.
using TypedSession = typename Registry::Session;
using TypedZero = Ext4ZeroTruncateTransaction<TypedSession>;
using TypedAllocated = Ext4AllocatedEmptyDataTransaction<TypedSession>;
using TypedPlan = typename Cache::TypedReconciliation;
using TypedChange = typename Cache::TypedCacheChange;
using TypedAction = typename Cache::TypedCacheAction;
enum class TypedOwnerPhase : u8 { unused, ready, prefix, child, prepared, terminal, published, failed };
// This off-stack after-state is minted only inside the actual successful
// publication handoff. Neither delayed observation nor a public report can
// replace the complete original cache records, including unused slot bytes.
class TypedPublishedCacheSnapshot final
{
    friend class Ext4OwnedTransaction;
    TypedPublishedCacheSnapshot() = default;
    ~TypedPublishedCacheSnapshot() = default;
    TypedPublishedCacheSnapshot(const TypedPublishedCacheSnapshot &) = delete;
    TypedPublishedCacheSnapshot &operator=(const TypedPublishedCacheSnapshot &) = delete;
    TypedPublishedCacheSnapshot(TypedPublishedCacheSnapshot &&) = delete;
    TypedPublishedCacheSnapshot &operator=(TypedPublishedCacheSnapshot &&) = delete;
    const void *workspace_{};
    const Ext4OwnedTransaction *owner_{};
    const Cache *cache_{};
    const TypedSession *session_{};
    const TypedPlan *reconciliation_{};
    MetadataOwnerReport report_{};
    std::array<typename Cache::Entry, BufferCapacity> entries_{};
    block::SessionBindingObservation binding_{};
    const typename Cache::Transaction *transaction_{};
    usize sectors_per_block_{};
    u64 transaction_serial_{};
    bool writable_{}, minted_{};
};
class TypedOwnerWorkspace final
{
    friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4MountedMetadata;
    TypedOwnerWorkspace() = default;
    ~TypedOwnerWorkspace() = default;
    TypedOwnerWorkspace(const TypedOwnerWorkspace &) = delete;
    TypedOwnerWorkspace &operator=(const TypedOwnerWorkspace &) = delete;
    TypedOwnerWorkspace(TypedOwnerWorkspace &&) = delete;
    TypedOwnerWorkspace &operator=(TypedOwnerWorkspace &&) = delete;
    TypedZero zero_{};
    TypedAllocated allocated_{};
    TypedPlan prefix_{}, reconciliation_{}, prefix_snapshot_{}, reconciliation_snapshot_{};
    TypedPlan published_reconciliation_snapshot_{};
    TypedPublishedCacheSnapshot published_cache_{};
    Block checking_{}, data_request_{}, data_request_seal_{};
    std::array<TypedChange, 32> changes_{};
    Ext4RecoveryInfo binding_{}, binding_seal_{};
    block::SessionReport initial_{}, initial_seal_{}, prefix_progress_{}, child_initial_{}, child_initial_seal_{};
    block::SessionBindingObservation session_binding_{};
    const TypedOwnerWorkspace *original_{};
    const Ext4OwnedTransaction *owner_{};
    const Cache *cache_{};
    const TypedSession *session_{};
    TypedOwnerPhase phase_{}, phase_seal_{};
    block::SessionCounter prefix_reads_{};
    usize prefix_reservation_{}, changes_count_{};
    u64 operation_serial_{}, operation_serial_seal_{}, handoff_serial_{}, handoff_serial_seal_{};
    bool attempted_{}, allocated_route_{}, recovery_{}, consumed_{};
    bool renewal_quarantined_{};
    // Independent permanent claim: child operation reset/renewal never erases
    // this original borrow, including when a common opening selects legacy.
    const TypedOwnerWorkspace *common_claim_original_{};
    const Ext4OwnedTransaction *common_claim_owner_{};
    const Cache *common_claim_cache_{};
    const TypedSession *common_claim_session_{};
    mutable smp::SpinLock common_claim_lock_{};
    [[nodiscard]] Status claim_common(const Ext4OwnedTransaction &owner, const Cache &cache, const TypedSession &session)
    {
        smp::ScopedSpinLock guard(common_claim_lock_);
        if (common_claim_original_ || common_claim_owner_ || common_claim_cache_ || common_claim_session_ ||
            original_ || owner_ || cache_ || session_ || attempted_ || consumed_ || renewal_quarantined_ ||
            phase_ != TypedOwnerPhase::unused || phase_seal_ != TypedOwnerPhase::unused ||
            operation_serial_ || operation_serial_seal_ || handoff_serial_ || handoff_serial_seal_)
            return Status::busy("common child workspace is already permanently claimed");
        common_claim_original_ = this; common_claim_owner_ = &owner;
        common_claim_cache_ = &cache; common_claim_session_ = &session; return {};
    }
    [[nodiscard]] Status common_claim_permits(const Ext4OwnedTransaction &owner, const Cache &cache,
                                             const TypedSession &session) const
    {
        smp::ScopedSpinLock guard(common_claim_lock_);
        if (!common_claim_original_ && !common_claim_owner_ && !common_claim_cache_ && !common_claim_session_) return {};
        return common_claim_original_ == this && common_claim_owner_ == &owner && common_claim_cache_ == &cache &&
            common_claim_session_ == &session ? Status{} : Status::bad_descriptor("child workspace belongs to another common opening");
    }
    [[nodiscard]] Status claim_for_child(const Ext4OwnedTransaction &owner, const Cache &cache, const TypedSession &session)
    {
        smp::ScopedSpinLock guard(common_claim_lock_);
        if (common_claim_original_ || common_claim_owner_ || common_claim_cache_ || common_claim_session_)
            return common_claim_original_ == this && common_claim_owner_ == &owner && common_claim_cache_ == &cache &&
                common_claim_session_ == &session ? Status{} : Status::bad_descriptor("child workspace belongs to another original Owner");
        if (original_ || owner_ || cache_ || session_ || attempted_ || consumed_ || renewal_quarantined_ ||
            phase_ != TypedOwnerPhase::unused || phase_seal_ != TypedOwnerPhase::unused ||
            operation_serial_ || operation_serial_seal_ || handoff_serial_ || handoff_serial_seal_)
            return Status::busy("child workspace has no unclaimed original operation storage");
        common_claim_original_ = this; common_claim_owner_ = &owner;
        common_claim_cache_ = &cache; common_claim_session_ = &session; return {};
    }
};
// These actual references cannot be erased by changing the retained workspace's
// mutable bindings. Their lifetime ends only after terminal handling returns.
class TypedOwnerContext final
{
    friend class Ext4OwnedTransaction;
    using OuterCheck = Status (*)(void *);
    TypedOwnerContext(Ext4OwnedTransaction &owner, TypedOwnerWorkspace &workspace,
                      TypedSession &session, bool allocated, bool recovery, const Block *data = nullptr,
                      void *outer = nullptr, OuterCheck outer_check = nullptr)
        : owner_(owner), workspace_(workspace), session_(session),
          allocated_(allocated), recovery_(recovery), data_(data),
          serial_(owner.typed_mounted_pending_ ? owner.typed_operation_serial_ :
              owner.typed_operation_serial_ == ~u64{0} ? 0 : owner.typed_operation_serial_ + 1),
          outer_(outer), outer_check_(outer_check) {}
    TypedOwnerContext(const TypedOwnerContext &) = delete;
    TypedOwnerContext &operator=(const TypedOwnerContext &) = delete;
    Ext4OwnedTransaction &owner_;
    TypedOwnerWorkspace &workspace_;
    TypedSession &session_;
    const bool allocated_, recovery_;
    const Block *const data_{};
    const u64 serial_{};
    void *const outer_{};
    const OuterCheck outer_check_{};
};
// Permanent original borrow; retained through all operation object renewals.
class TypedOwnerAttachment final
{
  public:
    TypedOwnerAttachment(Ext4OwnedTransaction &owner, TypedOwnerWorkspace &workspace, TypedSession &session)
        : owner_(owner), workspace_(workspace), session_(session) {}
    TypedOwnerAttachment(const TypedOwnerAttachment &) = delete;
    TypedOwnerAttachment &operator=(const TypedOwnerAttachment &) = delete;
    TypedOwnerAttachment(TypedOwnerAttachment &&) = delete;
    TypedOwnerAttachment &operator=(TypedOwnerAttachment &&) = delete;
    Ext4OwnedTransaction &owner_;
    TypedOwnerWorkspace &workspace_;
    TypedSession &session_;
};
[[nodiscard]] bool typed_workspace_anchors_exact_locked() const
{
    if (!typed_attachment_)
        return !typed_workspace_ && !typed_workspace_seal_ && !typed_context_ &&
            !typed_attempted_ && !typed_completed_ && !typed_operation_serial_ &&
            !typed_operation_serial_seal_ && !typed_handoff_serial_ && !typed_renewed_serial_;
    return &typed_attachment_->owner_ == this && &typed_attachment_->session_ == &cache_.session_ &&
        typed_workspace_ == &typed_attachment_->workspace_ && typed_workspace_seal_ == &typed_attachment_->workspace_ &&
        (!typed_workspace_->common_claim_original_ ||
            (typed_workspace_->common_claim_original_ == typed_workspace_ && typed_workspace_->common_claim_owner_ == this &&
             typed_workspace_->common_claim_cache_ == &cache_ && typed_workspace_->common_claim_session_ == &cache_.session_));
}
template <typename Authority>
[[nodiscard]] static Status typed_original_outer_guard(void *raw)
{
    if (!raw) return Status::fault("typed original outer authority is absent");
    return static_cast<Authority *>(raw)->validate(
        Ext4MountedTypedCursorPhase::paused_child, Ext4MountedTypedSuffixStage::none);
}
[[nodiscard]] bool typed_native_public_blocked() const
{ return typed_mounted_pending_ || typed_context_ || (typed_attempted_ && !typed_completed_); }
[[nodiscard]] bool typed_public_blocked() const
{ return common_opening_ || typed_native_public_blocked(); }
static void typed_phase(TypedOwnerWorkspace &workspace, TypedOwnerPhase phase)
{ workspace.phase_ = workspace.phase_seal_ = phase; }
// Retained snapshots are inert and cannot be submitted as plans: original_
// continues to identify the actual original plan, rather than this copy.
static void typed_snapshot(const TypedPlan &source, TypedPlan &out)
{
    out.original_ = source.original_; out.cache_ = source.cache_; out.session_ = source.session_;
    out.phase_ = source.phase_; out.report_ = source.report_; out.entries_ = source.entries_;
    out.actions_ = source.actions_; out.final_reports_ = source.final_reports_; out.final_images_ = source.final_images_;
    out.targets_ = source.targets_; out.target_actions_ = source.target_actions_;
    out.targets_count_ = source.targets_count_; out.resident_after_ = source.resident_after_;
    out.sectors_per_block_ = source.sectors_per_block_; out.transaction_serial_ = source.transaction_serial_;
    out.seal_ = source.seal_; out.writable_ = source.writable_; out.session_binding_ = source.session_binding_;
}
[[nodiscard]] static bool typed_snapshot_exact(const TypedPlan &source, const TypedPlan &snapshot)
{
    if (source.original_ != snapshot.original_ || source.cache_ != snapshot.cache_ || source.session_ != snapshot.session_ ||
        source.phase_ != snapshot.phase_ || !Cache::same_report(source.report_, snapshot.report_) ||
        source.actions_ != snapshot.actions_ || source.final_images_ != snapshot.final_images_ ||
        source.targets_ != snapshot.targets_ || source.target_actions_ != snapshot.target_actions_ ||
        source.targets_count_ != snapshot.targets_count_ || source.resident_after_ != snapshot.resident_after_ ||
        source.sectors_per_block_ != snapshot.sectors_per_block_ || source.transaction_serial_ != snapshot.transaction_serial_ ||
        source.seal_ != snapshot.seal_ || source.writable_ != snapshot.writable_ || source.session_binding_ != snapshot.session_binding_)
        return false;
    for (usize slot = 0; slot < BufferCapacity; ++slot)
        if (!Cache::same_report(source.entries_[slot].report, snapshot.entries_[slot].report) ||
            source.entries_[slot].canonical != snapshot.entries_[slot].canonical ||
            source.entries_[slot].staged != snapshot.entries_[slot].staged ||
            !Cache::same_report(source.final_reports_[slot], snapshot.final_reports_[slot])) return false;
    return true;
}
// Caller holds the original Owner then cache locks. Derive the published result
// from the authenticated reconciliation rather than sealing arbitrary live
// records observed after the operation returned. This method dispatches no I/O.
[[nodiscard]] Status typed_published_result_exact_locked(const TypedOwnerWorkspace &workspace) const
{
    const auto &plan = workspace.reconciliation_;
    if (plan.original_ != &plan || plan.cache_ != &cache_ || plan.session_ != &cache_.session_ ||
        plan.phase_ != TypedPlan::Phase::published ||
        plan.sectors_per_block_ != cache_.sectors_per_block_ ||
        plan.transaction_serial_ != cache_.transaction_serial_ ||
        plan.writable_ != cache_.writable_ || plan.session_binding_ != cache_.session_binding_)
        return Status::fault("typed published cache lost original reconciliation");
    auto expected = plan.report_;
    expected.resident_blocks = plan.resident_after_;
    if (!Cache::same_report(expected, cache_.report_) || cache_.transaction_)
        return Status::fault("typed published cache owner differs from reconciled result");
    for (usize slot = 0; slot < BufferCapacity; ++slot)
    {
        const auto &entry = cache_.entries_[slot];
        if (plan.actions_[slot] == TypedAction::unchanged)
        {
            if (!Cache::same_report(entry.report, plan.entries_[slot].report) ||
                entry.canonical != plan.entries_[slot].canonical || entry.staged != plan.entries_[slot].staged)
                return Status::fault("typed published unchanged cache slot differs");
        }
        else if (plan.actions_[slot] == TypedAction::replace)
        {
            if (!Cache::same_report(entry.report, plan.final_reports_[slot]) ||
                entry.canonical != plan.final_images_[slot] || entry.staged != plan.final_images_[slot])
                return Status::fault("typed published replaced cache slot differs");
        }
        else if (plan.actions_[slot] == TypedAction::retire)
        {
            if (!Cache::same_report(entry.report, plan.final_reports_[slot]))
                return Status::fault("typed published retired cache report differs");
            for (usize byte = 0; byte < entry.canonical.size(); ++byte)
                if (entry.canonical[byte] != std::byte{} || entry.staged[byte] != std::byte{})
                    return Status::fault("typed published retired cache bytes differ");
        }
        else return Status::fault("typed published cache action is invalid");
    }
    return {};
}
[[nodiscard]] Status typed_capture_published_cache(TypedOwnerContext &context)
{
    if (auto status = context.workspace_.common_claim_permits(*this, cache_, cache_.session_); !status) return status;
    smp::ScopedSpinLock owner_guard(lock_);
    smp::ScopedSpinLock cache_guard(cache_.lock_);
    auto &workspace = context.workspace_;
    auto &snapshot = workspace.published_cache_;
    if (&context.owner_ != this || &context.session_ != &cache_.session_ || typed_context_ != &context ||
        typed_workspace_ != &workspace || !typed_attempted_ || typed_completed_ ||
        workspace.phase_ != TypedOwnerPhase::terminal || workspace.phase_seal_ != workspace.phase_ ||
        workspace.original_ != &workspace || workspace.owner_ != this || workspace.cache_ != &cache_ ||
        workspace.session_ != &context.session_ || snapshot.minted_ || snapshot.workspace_ ||
        snapshot.owner_ || snapshot.cache_ || snapshot.session_ || snapshot.reconciliation_)
        return Status::fault("typed published cache capture lacks actual original handoff");
    if (auto status = cache_.typed_cache_idle(); !status) return status;
    if (auto status = typed_published_result_exact_locked(workspace); !status) return status;
    typed_snapshot(workspace.reconciliation_, workspace.published_reconciliation_snapshot_);
    snapshot.workspace_ = &workspace; snapshot.owner_ = this; snapshot.cache_ = &cache_;
    snapshot.session_ = &context.session_; snapshot.reconciliation_ = &workspace.reconciliation_;
    snapshot.report_ = cache_.report_; snapshot.entries_ = cache_.entries_;
    snapshot.binding_ = cache_.session_binding_; snapshot.transaction_ = cache_.transaction_;
    snapshot.sectors_per_block_ = cache_.sectors_per_block_;
    snapshot.transaction_serial_ = cache_.transaction_serial_; snapshot.writable_ = cache_.writable_;
    snapshot.minted_ = true;
    return {};
}
// Caller holds the original Owner then cache locks. Retained pointer fields are
// compared with actual storage addresses and are never dereferenced.
[[nodiscard]] Status typed_published_cache_exact_locked(const TypedOwnerWorkspace &workspace) const
{
    const auto &snapshot = workspace.published_cache_;
    if (!snapshot.minted_ || snapshot.workspace_ != &workspace || snapshot.owner_ != this ||
        snapshot.cache_ != &cache_ || snapshot.session_ != &cache_.session_ ||
        snapshot.reconciliation_ != &workspace.reconciliation_ ||
        !typed_snapshot_exact(workspace.reconciliation_, workspace.published_reconciliation_snapshot_) ||
        !Cache::same_report(snapshot.report_, cache_.report_) || snapshot.binding_ != cache_.session_binding_ ||
        snapshot.transaction_ != cache_.transaction_ || snapshot.transaction_ ||
        snapshot.sectors_per_block_ != cache_.sectors_per_block_ ||
        snapshot.transaction_serial_ != cache_.transaction_serial_ || snapshot.writable_ != cache_.writable_)
        return Status::fault("typed published cache complete original snapshot differs");
    for (usize slot = 0; slot < BufferCapacity; ++slot)
        if (!Cache::same_report(snapshot.entries_[slot].report, cache_.entries_[slot].report) ||
            snapshot.entries_[slot].canonical != cache_.entries_[slot].canonical ||
            snapshot.entries_[slot].staged != cache_.entries_[slot].staged)
            return Status::fault("typed published cache resident or unused slot differs");
    return typed_published_result_exact_locked(workspace);
}
[[nodiscard]] Status typed_context_exact(const TypedOwnerContext &context) const
{
    smp::ScopedSpinLock guard(lock_);
    const auto &workspace = context.workspace_;
    if (&context.owner_ != this || &context.session_ != &cache_.session_ ||
        !typed_workspace_anchors_exact_locked() || typed_renewal_quarantined_ ||
        !context.serial_ || context.serial_ != typed_operation_serial_ ||
        typed_operation_serial_ != typed_operation_serial_seal_ ||
        workspace.operation_serial_ != context.serial_ || workspace.operation_serial_seal_ != context.serial_ ||
        workspace.renewal_quarantined_ ||
        typed_context_ != &context || !typed_attempted_ || typed_completed_ || typed_workspace_ != &workspace ||
        !workspace.attempted_ || workspace.original_ != &workspace || workspace.owner_ != this ||
        workspace.cache_ != &cache_ || workspace.session_ != &context.session_ || workspace.consumed_ ||
        workspace.allocated_route_ != context.allocated_ || workspace.recovery_ != context.recovery_ ||
        (context.allocated_ && (context.data_ != &workspace.data_request_ ||
                               workspace.data_request_ != workspace.data_request_seal_)) ||
        workspace.phase_ != workspace.phase_seal_ || workspace.phase_ == TypedOwnerPhase::unused ||
        workspace.phase_ == TypedOwnerPhase::failed || workspace.phase_ == TypedOwnerPhase::published ||
        !reclaim_history::same_info(workspace.binding_, workspace.binding_seal_) ||
        !reclaim_history::same_session(workspace.initial_, workspace.initial_seal_) ||
        !reclaim_history::same_session(workspace.child_initial_, workspace.child_initial_seal_) ||
        workspace.prefix_reads_ > workspace.prefix_reservation_ || workspace.prefix_reservation_ != BufferCapacity)
        return Status::fault("typed owner actual continuing context differs");
    const auto binding = context.session_.observe_binding();
    if (!binding || binding.value() != workspace.session_binding_)
        return Status::fault("typed owner original authoritative Session binding differs");
    auto prefix = workspace.initial_;
    prefix.reads += workspace.prefix_reads_; prefix.completed_reads += workspace.prefix_reads_;
    if (!reclaim_history::same_session(prefix, workspace.prefix_progress_) ||
        (workspace.phase_ != TypedOwnerPhase::prefix &&
         !reclaim_history::same_session(prefix, workspace.child_initial_)))
        return Status::fault("typed owner retained prefix counter history differs");
    return {};
}
// Valid during an original Session dispatch: this checks only inert cache state,
// so it neither dispatches I/O nor requires the Session to be temporarily idle.
[[nodiscard]] Status typed_prefix_cache_exact(const TypedOwnerContext &context) const
{
    if (bool(context.outer_) != bool(context.outer_check_))
        return Status::fault("typed original outer authority binding is incomplete");
    if (!context.outer_check_) return typed_prefix_cache_state_exact(context);
    if (context.outer_check_)
        if (auto status = context.outer_check_(context.outer_); !status) return status;
    if (auto status = typed_prefix_cache_state_exact(context); !status) return status;
    if (context.outer_check_)
        if (auto status = context.outer_check_(context.outer_); !status) return status;
    return typed_prefix_cache_state_exact(context);
}
[[nodiscard]] Status typed_prefix_cache_state_exact(const TypedOwnerContext &context) const
{
    if (auto status = typed_context_exact(context); !status) return status;
    smp::ScopedSpinLock guard(cache_.lock_);
    const auto &plan = context.workspace_.prefix_;
    if (plan.original_ != &plan || plan.cache_ != &cache_ || plan.session_ != &context.session_ ||
        plan.phase_ != TypedPlan::Phase::prepared || !typed_snapshot_exact(plan, context.workspace_.prefix_snapshot_) ||
        !Cache::same_report(plan.report_, cache_.report_) ||
        plan.sectors_per_block_ != cache_.sectors_per_block_ ||
        plan.transaction_serial_ != cache_.transaction_serial_ || plan.writable_ != cache_.writable_ ||
        plan.session_binding_ != cache_.session_binding_)
        return Status::fault("typed owner original complete cache prefix differs");
    for (usize slot = 0; slot < BufferCapacity; ++slot)
        if (!Cache::same_report(plan.entries_[slot].report, cache_.entries_[slot].report) ||
            plan.entries_[slot].canonical != cache_.entries_[slot].canonical ||
            plan.entries_[slot].staged != cache_.entries_[slot].staged)
            return Status::fault("typed owner original resident cache prefix differs");
    const auto &reconciliation = context.workspace_.reconciliation_;
    if (context.workspace_.phase_ == TypedOwnerPhase::prepared ||
        context.workspace_.phase_ == TypedOwnerPhase::terminal)
    {
        if (reconciliation.original_ != &reconciliation || reconciliation.cache_ != &cache_ ||
            reconciliation.session_ != &context.session_ || reconciliation.phase_ != TypedPlan::Phase::prepared ||
            !typed_snapshot_exact(reconciliation, context.workspace_.reconciliation_snapshot_) ||
            !Cache::same_report(reconciliation.report_, plan.report_) ||
            reconciliation.session_binding_ != plan.session_binding_)
            return Status::fault("typed owner original affected reconciliation plan differs");
        for (usize slot = 0; slot < BufferCapacity; ++slot)
            if (!Cache::same_report(reconciliation.entries_[slot].report, plan.entries_[slot].report) ||
                reconciliation.entries_[slot].canonical != plan.entries_[slot].canonical ||
                reconciliation.entries_[slot].staged != plan.entries_[slot].staged)
                return Status::fault("typed owner affected plan lost its complete prefix");
    }
    return {};
}
// A retained canonical owner is an actual runtime service boundary. The child
// invokes this fixed private thunk at guarded dispatch boundaries, without raw
// callbacks or transport; no virtual implementation-sharing interface is added.
[[nodiscard]] static Status typed_child_guard(void *raw)
{
    auto &context = *static_cast<TypedOwnerContext *>(raw);
    return context.owner_.typed_prefix_cache_exact(context);
}
[[nodiscard]] Status typed_inputs_disjoint(const TypedOwnerWorkspace &workspace,
    const Ext4RecoveryInfo &binding, const Block *image = nullptr) const
{
    if (Cache::overlaps_storage(&workspace, sizeof(workspace), this, sizeof(*this)) ||
        Cache::overlaps_storage(&binding, sizeof(binding), this, sizeof(*this)) ||
        Cache::overlaps_storage(&binding, sizeof(binding), &workspace, sizeof(workspace)))
        return Status::invalid_argument("typed owner workspace or binding aliases original storage");
    if (image && (Cache::overlaps_storage(image, sizeof(*image), this, sizeof(*this)) ||
        Cache::overlaps_storage(image, sizeof(*image), &workspace, sizeof(workspace)) ||
        Cache::overlaps_storage(image, sizeof(*image), &binding, sizeof(binding))))
        return Status::invalid_argument("typed owner data request aliases retained storage");
    return {};
}
[[nodiscard]] Status typed_begin(TypedOwnerContext &context, const Ext4RecoveryInfo &binding,
    block::SessionCounter child_reads, block::SessionCounter child_writes, block::SessionCounter child_flushes, const Block *image = nullptr)
{
    auto &workspace = context.workspace_;
    if (auto status = typed_inputs_disjoint(workspace, binding, image); !status) return status;
    if (context.allocated_ != bool(image))
        return Status::invalid_argument("typed owner route lacks its original data request");
    if (workspace.attempted_) return Status::already_exists("typed owner workspace is one-shot");
    if (binding.layout.first_sector % 2)
        return Status::unsupported("typed owner cache requires a block-aligned volume");
    if (&context.owner_ != this || &context.session_ != &cache_.session_)
        return Status::invalid_argument("typed owner original context differs before workspace claim");
    if (auto status = workspace.claim_for_child(*this, cache_, cache_.session_); !status) return status;
    {
        smp::ScopedSpinLock guard(lock_);
        if (typed_renewal_quarantined_) return Status::fault("typed owner renewal is permanently quarantined");
        if (!typed_workspace_anchors_exact_locked())
            return quarantine_typed_renewal_locked(Status::fault("typed owner permanent workspace anchor changed"));
        if (typed_attachment_ && &typed_attachment_->workspace_ != &workspace)
            return Status::bad_descriptor("typed owner requires its permanent original workspace");
        if (!context.serial_ || (!typed_mounted_pending_ && typed_operation_serial_ == ~u64{0}))
            return Status::overflow("typed owner operation serial exhausted");
        if (typed_operation_serial_ != typed_operation_serial_seal_ ||
            context.serial_ != (typed_mounted_pending_ ? typed_operation_serial_ : typed_operation_serial_ + 1) ||
            workspace.renewal_quarantined_)
            return quarantine_typed_renewal_locked(Status::fault("typed owner private admission serial changed"));
        if (&context.owner_ != this || &context.session_ != &cache_.session_ || !mounted_dirty_ ||
            typed_attempted_ || (typed_mounted_pending_ && typed_mounted_workspace_ != &workspace) ||
            startup_active_ || startup_route_opening_ || admissions_ || authority_.active ||
            report_.phase != Ext4OwnedPhase::open || participant_count_ || transaction_.active_ ||
            has_data_ || rename_active_ || create_active_ || first_data_active_ || next_data_active_ ||
            namespace_active_ || namespace_recovery_active_ || reclaim_prepared_ || reclaim_active_)
            return Status::busy("typed owner requires an exclusive drained original owner");
        smp::ScopedSpinLock cache_guard(cache_.lock_);
        if (auto status = cache_.typed_cache_idle(); !status) return status;
        const auto actual = context.session_.report();
        if (!actual) return actual.status();
        const auto fits = [](block::SessionCounter initial, block::SessionCounter count) { return count <= ~block::SessionCounter{0} - initial; };
        if (child_reads > ~block::SessionCounter{0} - BufferCapacity ||
            !fits(actual.value().reads, child_reads + BufferCapacity) ||
            !fits(actual.value().completed_reads, child_reads + BufferCapacity) ||
            !fits(actual.value().writes, child_writes) || !fits(actual.value().completed_writes, child_writes) ||
            !fits(actual.value().flushes, child_flushes) || !fits(actual.value().completed_flushes, child_flushes))
            return Status::overflow("typed owner whole prefix and child counters exhausted");
        // All disjointness and admission checks precede the first workspace
        // write. Original callbacks consume only this retained admitted image.
        if (image) workspace.data_request_ = workspace.data_request_seal_ = *image;
        workspace.initial_ = workspace.initial_seal_ = workspace.prefix_progress_ = actual.value();
        workspace.session_binding_ = cache_.session_binding_;
        workspace.binding_ = workspace.binding_seal_ = binding;
        workspace.original_ = &workspace; workspace.owner_ = this; workspace.cache_ = &cache_;
        workspace.session_ = &context.session_; workspace.attempted_ = true;
        workspace.allocated_route_ = context.allocated_; workspace.recovery_ = context.recovery_;
        workspace.prefix_reservation_ = BufferCapacity;
        typed_phase(workspace, TypedOwnerPhase::prefix);
        if (!typed_attachment_) typed_attachment_.emplace(*this, workspace, context.session_);
        typed_workspace_ = typed_workspace_seal_ = &workspace;
        typed_operation_serial_ = typed_operation_serial_seal_ = context.serial_;
        workspace.operation_serial_ = workspace.operation_serial_seal_ = context.serial_;
        typed_attempted_ = true; typed_completed_ = false;
        typed_context_ = &context;
        report_.phase = Ext4OwnedPhase::preparing;
    }
    if (auto status = cache_.prepare_typed_reconciliation({}, workspace.prefix_); !status) return status;
    typed_snapshot(workspace.prefix_, workspace.prefix_snapshot_);
    auto authority = [&]() { return typed_prefix_cache_exact(context); };
    for (usize slot = 0; slot < BufferCapacity; ++slot)
    {
        const auto &entry = workspace.prefix_.entries_[slot];
        if (entry.report.state != MetadataBufferState::ready) continue;
        if (entry.report.block > (~u64{0} / 2)) return Status::overflow("typed owner prefix physical sector overflow");
        if (auto status = guarded_ext4_direct_read(context.session_, workspace.prefix_progress_, authority,
                entry.report.block * 2, std::as_writable_bytes(std::span{workspace.checking_})); !status) return status;
        ++workspace.prefix_reads_;
        if (!std::equal(workspace.checking_.begin(), workspace.checking_.end(), entry.canonical.begin(),
                [](u8 byte, std::byte expected) { return byte == std::to_integer<u8>(expected); }))
            return Status::fault("typed owner prefix resident image differs from actual media");
    }
    if (auto status = typed_prefix_cache_exact(context); !status) return status;
    workspace.child_initial_ = workspace.child_initial_seal_ = workspace.prefix_progress_;
    typed_phase(workspace, TypedOwnerPhase::child);
    if (context.allocated_)
        return workspace.allocated_.bind_owner_guard(&context, typed_child_guard);
    return workspace.zero_.bind_owner_guard(&context, typed_child_guard);
}
[[nodiscard]] Status typed_stop(TypedOwnerContext &context, Status status)
{
    // The immutable local Session reference remains available even if every
    // mutable retained owner/workspace binding is erased by an original callback.
    static_cast<void>(context.session_.abort(status));
    // The child may have failed before minting, during a dispatch or after
    // physical completion. No exit retains a callback to this local context.
    context.workspace_.zero_.retire_owner_guard();
    context.workspace_.allocated_.retire_owner_guard();
    // Permanent child storage outlives Owner/Mount. Retire destruction-time
    // Session borrows now, after the synchronous failure has drained and while
    // the actual local Session reference is still alive. Preserve the first
    // error and all child reports; terminal retirement performs no transport.
    static_cast<void>(context.workspace_.zero_.retire_aborted_owner_session(context.session_));
    static_cast<void>(context.workspace_.allocated_.retire_aborted_owner_session(context.session_));
    {
        smp::ScopedSpinLock guard(lock_);
        typed_attempted_ = true; typed_completed_ = false; typed_context_ = nullptr;
    }
    context.workspace_.consumed_ = true;
    typed_phase(context.workspace_, TypedOwnerPhase::failed);
    return fail(status);
}
[[nodiscard]] Status typed_add(TypedOwnerWorkspace &workspace, u64 physical, TypedAction action,
                              std::span<const std::byte> before, std::span<const std::byte> after = {})
{
    if (physical > (~u64{0} - workspace.binding_.layout.first_sector) / 2 ||
        workspace.binding_.layout.first_sector % 2)
        return Status::unsupported("typed owner cache requires a block-aligned volume");
    if (workspace.changes_count_ == workspace.changes_.size())
        return Status::overflow("typed owner affected target capacity exhausted");
    workspace.changes_[workspace.changes_count_++] = {
        physical + workspace.binding_.layout.first_sector / 2, action, before, after};
    return {};
}
template <typename Child>
[[nodiscard]] Status typed_derive(TypedOwnerContext &context, Child &child, bool noop = false)
{
    auto &workspace = context.workspace_;
    if (auto status = typed_prefix_cache_exact(context); !status) return status;
    if (auto status = child.idle(); !status) return status;
    if (child.original_ != &context.session_ || child.original_seal_ != &context.session_ ||
        !child.anchor_.exact(&context.session_) ||
        !reclaim_history::same_info(child.binding_, workspace.binding_) ||
        !reclaim_history::same_session(child.initial_, workspace.child_initial_) ||
        !child.images_.ready || !child.retained())
        return Status::fault("typed owner original prepared child attachment differs");
    const auto &images = child.images_;
    if (!noop)
    {
        for (u32 record = 0; record < 8; ++record)
        {
            const auto &final = !record ? images.journal.empty_head : child.formatted(record);
            if (auto status = typed_add(workspace, child.binding_.layout.journal_first_block + record,
                    TypedAction::replace, std::as_bytes(std::span{images.original_journal[record]}),
                    std::as_bytes(std::span{final})); !status) return status;
        }
        for (u32 home = 0; home < 4; ++home)
        {
            const auto &before = workspace.recovery_ ? images.actual_homes[home] : images.plan.before[home];
            if (auto status = typed_add(workspace, images.plan.targets[home], TypedAction::replace,
                    std::as_bytes(std::span{before}), std::as_bytes(std::span{images.plan.after[home]})); !status) return status;
        }
        if constexpr (std::same_as<Child, TypedAllocated>)
        {
            if (auto status = typed_add(workspace, images.plan.physical, TypedAction::replace,
                    std::as_bytes(std::span{images.original_data}),
                    std::as_bytes(std::span{images.data_image})); !status) return status;
        }
        else
        {
            for (u32 at = 0; at < images.plan.released_count; ++at)
            {
                const u64 physical = images.plan.released_mapping[at];
                const u64 key = physical + workspace.binding_.layout.first_sector / 2;
                for (const auto &entry : workspace.prefix_.entries_)
                    if (entry.report.state == MetadataBufferState::ready && entry.report.block == key)
                        if (auto status = typed_add(workspace, physical, TypedAction::retire,
                                std::span<const std::byte>{entry.canonical}); !status) return status;
            }
        }
    }
    if (auto status = cache_.prepare_typed_reconciliation(
            std::span<const TypedChange>{workspace.changes_.data(), workspace.changes_count_},
            workspace.reconciliation_); !status) return status;
    typed_snapshot(workspace.reconciliation_, workspace.reconciliation_snapshot_);
    typed_phase(workspace, TypedOwnerPhase::prepared);
    return {};
}
template <typename Child>
[[nodiscard]] Status typed_handoff(TypedOwnerContext &context, Child &child)
{
    auto &workspace = context.workspace_;
    if (auto status = typed_prefix_cache_exact(context); !status) return status;
    if (workspace.phase_ != TypedOwnerPhase::prepared || workspace.consumed_ || !child.status_ ||
        child.control_.phase != Child::Phase::complete || child.control_ != child.sealed_ ||
        child.control_.pending || child.control_.pending_writes || !child.retained() ||
        child.original_ != &context.session_ || child.original_seal_ != &context.session_ ||
        !child.anchor_.exact(&context.session_) ||
        !reclaim_history::same_session(child.initial_, workspace.child_initial_) ||
        !reclaim_history::same_info(child.binding_, workspace.binding_))
        return Status::fault("typed owner terminal handoff lacks original private completion");
    if (auto status = child.idle(); !status) return status;
    // No dispatcher or external callback occurs between private completion and
    // normalization/publication. Public diagnostics cannot substitute this call.
    if (!child.completed_ownership_birth_valid(true))
        return Status::fault("typed owner terminal handoff lost completed ownership birth");
    typed_phase(workspace, TypedOwnerPhase::terminal);
    if (auto status = cache_.normalize_typed_reconciliation(workspace.reconciliation_); !status) return status;
    if (auto status = cache_.publish_typed_reconciliation(workspace.reconciliation_); !status) return status;
    if (auto status = typed_capture_published_cache(context); !status) return status;
    workspace.zero_.retire_owner_guard();
    workspace.allocated_.retire_owner_guard();
    if (!workspace.zero_.owner_retirement_idle() || !workspace.allocated_.owner_retirement_idle())
        return Status::fault("typed owner terminal retirement is not drained");
    workspace.consumed_ = true;
    workspace.handoff_serial_ = workspace.handoff_serial_seal_ = context.serial_;
    typed_phase(workspace, TypedOwnerPhase::published);
    {
        smp::ScopedSpinLock guard(lock_);
        typed_completed_ = true;
        typed_handoff_serial_ = context.serial_;
        typed_context_ = nullptr;
        report_.phase = Ext4OwnedPhase::checkpointed; report_.status = {};
        report_.completion.journal_sequence = child.reserved_.final_sequence;
    }
    return {};
}
[[nodiscard]] Status run_typed_zero(TypedOwnerWorkspace &workspace, const Ext4RecoveryInfo &binding,
                                  Ext4ZeroTruncateUpdate update, bool recovery = false)
{ return typed_run_zero(workspace, binding, update, recovery, nullptr, nullptr); }
template <typename Authority>
[[nodiscard]] Status run_typed_zero_with_authority(TypedOwnerWorkspace &workspace,
    const Ext4RecoveryInfo &binding, Ext4ZeroTruncateUpdate update, Authority &original,
    bool recovery = false)
{
    return typed_run_zero(workspace, binding, update, recovery,
        &original, &typed_original_outer_guard<Authority>);
}
[[nodiscard]] Status typed_run_zero(TypedOwnerWorkspace &workspace, const Ext4RecoveryInfo &binding,
    Ext4ZeroTruncateUpdate update, bool recovery, void *outer,
    typename TypedOwnerContext::OuterCheck outer_check)
{
    if (auto status = typed_inputs_disjoint(workspace, binding); !status) return status;
    if (workspace.attempted_) return Status::already_exists("typed owner workspace is one-shot");
    const auto initial = cache_.session_.report(); if (!initial) return initial.status();
    if (!binding.metadata_count || binding.metadata_count % 4 || binding.metadata_count > binding.metadata.size())
        return Status::invalid_argument("typed owner zero metadata profile");
    const auto route = recovery ? Ext4ZeroTruncateRoute::recovery :
        update.old_size ? Ext4ZeroTruncateRoute::fresh : Ext4ZeroTruncateRoute::noop;
    const auto capacity = Ext4ZeroTruncateTransactionBudget::capacity(initial.value(),
        u32(binding.metadata_count / 4), route, binding.journal_sequence);
    if (!capacity) return capacity.status();
    TypedOwnerContext context(*this, workspace, cache_.session_, false, recovery, nullptr, outer, outer_check);
    if (outer_check) if (auto status = outer_check(outer); !status) return status;
    if (auto status = typed_begin(context, binding, capacity.value().reads,
            capacity.value().writes, capacity.value().flushes); !status)
        return workspace.attempted_ ? typed_stop(context, status) : status;
    auto &child = workspace.zero_;
    const auto prepared = recovery ? child.prepare_recovery(context.session_, workspace.binding_) :
        child.prepare(context.session_, workspace.binding_, update);
    if (!prepared) return typed_stop(context, prepared);
    const bool noop = route == Ext4ZeroTruncateRoute::noop;
    if (auto status = typed_derive(context, child, noop); !status) return typed_stop(context, status);
    if (!noop)
    {
        if (!recovery) if (auto status = child.publish(); !status) return typed_stop(context, status);
        if (auto status = child.checkpoint(); !status) return typed_stop(context, status);
    }
    if (auto status = typed_handoff(context, child); !status) return typed_stop(context, status);
    return {};
}
[[nodiscard]] Status run_typed_allocated(TypedOwnerWorkspace &workspace, const Ext4RecoveryInfo &binding,
    Ext4AllocatedEmptyDataUpdate update, u32 offset, const Block &image, bool recovery = false)
{ return typed_run_allocated(workspace, binding, update, offset, image, recovery, nullptr, nullptr); }
template <typename Authority>
[[nodiscard]] Status run_typed_allocated_with_authority(TypedOwnerWorkspace &workspace,
    const Ext4RecoveryInfo &binding, Ext4AllocatedEmptyDataUpdate update, u32 offset,
    const Block &image, Authority &original, bool recovery = false)
{
    return typed_run_allocated(workspace, binding, update, offset, image, recovery,
        &original, &typed_original_outer_guard<Authority>);
}
[[nodiscard]] Status typed_run_allocated(TypedOwnerWorkspace &workspace, const Ext4RecoveryInfo &binding,
    Ext4AllocatedEmptyDataUpdate update, u32 offset, const Block &image, bool recovery,
    void *outer, typename TypedOwnerContext::OuterCheck outer_check)
{
    if (auto status = typed_inputs_disjoint(workspace, binding, &image); !status) return status;
    if (workspace.attempted_) return Status::already_exists("typed owner workspace is one-shot");
    const auto initial = cache_.session_.report(); if (!initial) return initial.status();
    if (!binding.metadata_count || binding.metadata_count % 4 || binding.metadata_count > binding.metadata.size())
        return Status::invalid_argument("typed owner allocated metadata profile");
    const auto route = recovery ? Ext4AllocatedEmptyDataRoute::recovery : Ext4AllocatedEmptyDataRoute::fresh;
    const auto capacity = Ext4AllocatedEmptyDataTransactionBudget::capacity(initial.value(),
        u32(binding.metadata_count / 4), route, binding.journal_sequence);
    if (!capacity) return capacity.status();
    TypedOwnerContext context(*this, workspace, cache_.session_, true, recovery,
        &workspace.data_request_, outer, outer_check);
    if (outer_check) if (auto status = outer_check(outer); !status) return status;
    if (auto status = typed_begin(context, binding, capacity.value().reads,
            capacity.value().writes, capacity.value().flushes, &image); !status)
        return workspace.attempted_ ? typed_stop(context, status) : status;
    auto &child = workspace.allocated_;
    const auto prepared = recovery ? child.prepare_recovery(context.session_, workspace.binding_) :
        child.prepare(context.session_, workspace.binding_, update, offset, workspace.data_request_);
    if (!prepared) return typed_stop(context, prepared);
    if (auto status = typed_derive(context, child); !status) return typed_stop(context, status);
    if (!recovery) if (auto status = child.publish(); !status) return typed_stop(context, status);
    if (auto status = child.checkpoint(); !status) return typed_stop(context, status);
    if (auto status = typed_handoff(context, child); !status) return typed_stop(context, status);
    return {};
}
// A private, strict-terminal visitor for the actual selected completed child.
// This is a snapshot prerequisite, not permission to advance the Session.
// The mount visitor performs only declared-field copies and no callbacks/I/O.
[[nodiscard]] Status typed_completed_workspace_exact(const TypedOwnerWorkspace &workspace) const
{
    smp::ScopedSpinLock guard(lock_);
    if (typed_workspace_ != &workspace || !typed_attempted_ || !typed_completed_ || typed_context_ ||
        report_.phase != Ext4OwnedPhase::checkpointed || !report_.status ||
        workspace.original_ != &workspace || workspace.owner_ != this || workspace.cache_ != &cache_ ||
        workspace.session_ != &cache_.session_ || !workspace.attempted_ || !workspace.consumed_ ||
        workspace.phase_ != TypedOwnerPhase::published || workspace.phase_seal_ != workspace.phase_ ||
        !reclaim_history::same_info(workspace.binding_, workspace.binding_seal_) ||
        !reclaim_history::same_session(workspace.initial_, workspace.initial_seal_) ||
        !reclaim_history::same_session(workspace.child_initial_, workspace.child_initial_seal_) ||
        workspace.prefix_reservation_ != BufferCapacity || workspace.prefix_reads_ > BufferCapacity ||
        workspace.data_request_ != workspace.data_request_seal_)
        return Status::fault("typed completed snapshot lacks original published workspace");
    auto prefix = workspace.initial_;
    if (workspace.prefix_reads_ > ~block::SessionCounter{0} - prefix.reads ||
        workspace.prefix_reads_ > ~block::SessionCounter{0} - prefix.completed_reads)
        return Status::fault("typed completed snapshot prefix history overflows");
    prefix.reads += workspace.prefix_reads_; prefix.completed_reads += workspace.prefix_reads_;
    if (!reclaim_history::same_session(prefix, workspace.prefix_progress_) ||
        !reclaim_history::same_session(prefix, workspace.child_initial_))
        return Status::fault("typed completed snapshot lost original prefix history");
    const auto binding = cache_.session_.observe_binding();
    if (!binding || binding.value() != workspace.session_binding_)
        return Status::fault("typed completed snapshot original Session binding differs");
    smp::ScopedSpinLock cache_guard(cache_.lock_);
    if (auto status = cache_.typed_cache_idle(); !status) return status;
    return typed_published_cache_exact_locked(workspace);
}
template <typename Child>
[[nodiscard]] Status typed_completed_child_exact(TypedOwnerWorkspace &workspace, Child &child)
{
    if (auto status = typed_completed_workspace_exact(workspace); !status) return status;
    if (!child.status_ || child.control_.phase != Child::Phase::complete || child.control_ != child.sealed_ ||
        child.control_.pending || child.control_.pending_writes ||
        child.original_ != &cache_.session_ || child.original_seal_ != &cache_.session_ ||
        !child.anchor_.exact(&cache_.session_) ||
        !workspace.zero_.completed_owner_callbacks_retired() ||
        !workspace.allocated_.completed_owner_callbacks_retired() || !child.completed_ownership_birth_valid(true) || !child.retained() ||
        !reclaim_history::same_info(child.binding_, workspace.binding_) ||
        !reclaim_history::same_session(child.initial_, workspace.child_initial_) ||
        report_.completion.journal_sequence != child.reserved_.final_sequence)
        return Status::fault("typed completed snapshot lacks selected actual terminal child");
    if constexpr (std::same_as<Child, TypedAllocated>)
    {
        if (!workspace.allocated_route_ || workspace.recovery_ != (child.route_ == Child::Route::recovery))
            return Status::fault("typed completed allocated snapshot route differs");
    }
    else if (workspace.allocated_route_ || workspace.recovery_ != (child.route_ == Child::Route::recovery))
        return Status::fault("typed completed zero snapshot route differs");
    return child.idle();
}
template <typename Child, typename Visitor>
[[nodiscard]] Status typed_visit_completed_child(TypedOwnerWorkspace &workspace, Child &child, Visitor &visitor)
{
    if (auto status = typed_completed_child_exact(workspace, child); !status) return status;
    const bool noop = [&]() {
        if constexpr (std::same_as<Child, TypedAllocated>) return false;
        else return child.route_ == Child::Route::noop;
    }();
    const auto visit = [&](const auto &view, const auto &proof, const auto &binding) {
        return visitor(view, proof, binding, child.progress_, &child, workspace.allocated_route_,
                       noop);
    };
    Status status;
    if constexpr (std::same_as<Child, TypedAllocated>)
        status = visit(child.final_view_, child.final_proof_, child.final_binding_);
    else if (child.route_ == Child::Route::noop)
        status = visit(child.old_view_, child.old_proof_, child.binding_);
    else status = visit(child.final_view_, child.final_proof_, child.final_binding_);
    if (!status) return status;
    // The visitor is private, but its complete original attachment must still
    // be proved before any subsequent live dereference or retired callback.
    return typed_completed_child_exact(workspace, child);
}
template <typename Visitor>
[[nodiscard]] Status typed_visit_completed_view(TypedOwnerWorkspace &workspace, Visitor &visitor)
{
    if (auto status = typed_completed_workspace_exact(workspace); !status) return status;
    if (workspace.allocated_route_) return typed_visit_completed_child(workspace, workspace.allocated_, visitor);
    return typed_visit_completed_child(workspace, workspace.zero_, visitor);
}
// Quarantine uses only the permanent original borrow, never mutable aliases.
[[nodiscard]] Status quarantine_typed_renewal_locked(Status status)
{
    if (status) status = Status::fault("typed renewal quarantine requires an error");
    if (typed_attachment_ && &typed_attachment_->owner_ == this && &typed_attachment_->session_ == &cache_.session_)
    {
        auto &workspace = typed_attachment_->workspace_;
        workspace.renewal_quarantined_ = true; workspace.consumed_ = true;
        typed_phase(workspace, TypedOwnerPhase::failed);
    }
    typed_renewal_quarantined_ = true; typed_attempted_ = true; typed_completed_ = false;
    report_.phase = Ext4OwnedPhase::failed; report_.status = status;
    static_cast<void>(cache_.session_.abort(status)); static_cast<void>(cache_.abort(status));
    return status;
}
TypedOwnerWorkspace *typed_workspace_{};
TypedOwnerWorkspace *typed_workspace_seal_{};
std::optional<TypedOwnerAttachment> typed_attachment_{};
const TypedOwnerContext *typed_context_{};
bool typed_attempted_{}, typed_completed_{};
bool typed_renewal_quarantined_{};
u64 typed_operation_serial_{}, typed_operation_serial_seal_{}, typed_handoff_serial_{}, typed_renewed_serial_{};
