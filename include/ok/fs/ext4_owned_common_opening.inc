// Private original common opening; observations alone cannot mint Selection.
enum class CommonOpeningPhase : u8
{ unused, classifying, selected, typed_adopted, legacy_root, complete, failed };
struct CommonOpening;
class CommonSelection;
struct CommonOpening final
{
    friend class Ext4OwnedTransaction;
    friend class CommonSelection;
    template <usize> friend class Ext4MountedMetadata;
    CommonOpening(Ext4OwnedTransaction &owner, const Ext4MountedMetadata<DeviceCapacity> &mount,
        Ext4MountedStartupWorkspace &startup, TypedOwnerWorkspace &workspace)
        : owner_(owner), mount_(mount), session_(owner.cache_.session_), startup_(startup), workspace_(workspace),
          original_(this), initial_(session_.report()), binding_(session_.observe_binding()) {}
    CommonOpening(const CommonOpening &) = delete;
    CommonOpening &operator=(const CommonOpening &) = delete;
    CommonOpening(CommonOpening &&) = delete;
    CommonOpening &operator=(CommonOpening &&) = delete;
    Ext4OwnedTransaction &owner_;
    const Ext4MountedMetadata<DeviceCapacity> &mount_;
    TypedSession &session_;
    Ext4MountedStartupWorkspace &startup_;
    TypedOwnerWorkspace &workspace_;
    const CommonOpening *const original_;
    const Result<block::SessionReport> initial_;
    const Result<block::SessionBindingObservation> binding_;
    const Ext4MountedCommonCapacity whole_{Ext4MountedCommonOpeningBudget::whole_union()};
    CommonSelection *selection_{};
    CommonOpeningPhase phase_{CommonOpeningPhase::unused};
    u64 root_reads_{};
};
class CommonSelection final
{
    friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4MountedMetadata;
    explicit CommonSelection(CommonOpening &opening) : opening_(opening), original_(this) { if (!opening.selection_) opening.selection_ = this; }
    CommonSelection(const CommonSelection &) = delete;
    CommonSelection &operator=(const CommonSelection &) = delete;
    CommonSelection(CommonSelection &&) = delete;
    CommonSelection &operator=(CommonSelection &&) = delete;
  public:
    // The private cursor receives this original object from its concrete Owner.
    // These inert facts cannot manufacture it or authenticate another object.
    template <typename Context>
    [[nodiscard]] Status authenticate_predecessor(const Context &context) const
    { return opening_.owner_.common_predecessor_context_exact(*this, context); }
    [[nodiscard]] const block::SessionReport &baseline() const { return opening_.initial_.value(); }
    [[nodiscard]] const block::SessionReport &endpoint() const { return endpoint_; }
    [[nodiscard]] Ext4MountedCommonFamily family() const { return family_; }
    [[nodiscard]] Ext4MountedCommonCapacity whole_union() const { return opening_.whole_; }
  private:
    CommonOpening &opening_;
    const CommonSelection *const original_;
    std::array<Block, 5> sources_{};
    block::SessionReport endpoint_{};
    Ext4MountedCommonCapacity remaining_{};
    Ext4MountedCommonFamily family_{Ext4MountedCommonFamily::ordinary};
    u32 sequence_{};
    u8 reads_{};
    bool minted_{};
};
CommonOpening *common_opening_{};
// The Owner is retained inside the original Mount. The Selection lives in its
// permanent storage, never as a five-block automatic object on a kernel stack.
alignas(CommonSelection) std::array<std::byte, sizeof(CommonSelection)> common_selection_storage_{};
bool common_selection_constructed_{};
[[nodiscard]] Status construct_common_selection(CommonOpening &opening)
{
    if (common_selection_constructed_ || opening.selection_) return Status::already_exists("common Selection storage is one-shot");
    static_cast<void>(new (common_selection_storage_.data()) CommonSelection(opening));
    common_selection_constructed_ = true; return {};
}
void destroy_common_selection(CommonOpening &opening)
{
    if (opening.selection_) { opening.selection_->~CommonSelection(); opening.selection_ = nullptr; }
}
[[nodiscard]] static Ext4MountedCommonOpeningBudget::Counts common_counts(const block::SessionReport &s)
{ return {s.reads, s.completed_reads, s.writes, s.completed_writes, s.flushes, s.completed_flushes}; }
[[nodiscard]] Status claim_common_workspace(TypedOwnerWorkspace &workspace)
{
    if (Cache::overlaps_storage(&workspace, sizeof(workspace), this, sizeof(*this)))
        return Status::invalid_argument("common permanent child workspace overlaps Owner");
    // Claim lock is released before Owner/cache locking or device opening.
    if (auto status = workspace.claim_common(*this, cache_, cache_.session_); !status) return status;
    smp::ScopedSpinLock guard(lock_);
    if (report_.phase != Ext4OwnedPhase::unused || typed_attachment_ || typed_workspace_ || typed_workspace_seal_ ||
        workspace.original_ || workspace.owner_ || workspace.cache_ || workspace.session_ || workspace.attempted_ ||
        workspace.consumed_ || workspace.renewal_quarantined_ || workspace.phase_ != TypedOwnerPhase::unused ||
        workspace.phase_seal_ != TypedOwnerPhase::unused || workspace.operation_serial_ || workspace.operation_serial_seal_ ||
        workspace.handoff_serial_ || workspace.handoff_serial_seal_)
        return Status::busy("common opening requires its unclaimed permanent child workspace");
    typed_attachment_.emplace(*this, workspace, cache_.session_);
    typed_workspace_ = typed_workspace_seal_ = &workspace;
    return {};
}
[[nodiscard]] Status reserve_common_opening(CommonOpening &opening)
{
    if (opening.original_ != &opening || &opening.owner_ != this || &opening.session_ != &cache_.session_ ||
        opening.phase_ != CommonOpeningPhase::unused || !opening.initial_ || !opening.binding_ || !opening.selection_)
        return Status::invalid_argument("common opening lacks its actual original native observations");
    smp::ScopedSpinLock guard(lock_);
    smp::ScopedSpinLock cache_guard(cache_.lock_);
    if (common_opening_ || startup_active_ || startup_route_opening_ || typed_public_blocked() ||
        !typed_workspace_anchors_exact_locked() || !typed_attachment_ ||
        opening.workspace_.common_claim_original_ != &opening.workspace_ || opening.workspace_.common_claim_owner_ != this ||
        opening.workspace_.common_claim_cache_ != &cache_ || opening.workspace_.common_claim_session_ != &cache_.session_ ||
        &typed_attachment_->workspace_ != &opening.workspace_ || report_.phase != Ext4OwnedPhase::open ||
        !report_.status || !mounted_dirty_ || admissions_ || authority_.active || participant_count_ || transaction_.active_)
        return Status::busy("common opening requires its exclusive original Owner");
    if (auto status = cache_.typed_cache_idle(); !status) return status;
    const auto actual = cache_.session_.report(); const auto binding = cache_.session_.observe_binding();
    if (!actual || !binding) return !actual ? actual.status() : binding.status();
    if (!exact_session(actual.value(), opening.initial_.value()) || binding.value() != opening.binding_.value() ||
        actual.value().state != block::DeviceState::owned || actual.value().first_error != StatusCode::ok ||
        actual.value().in_flight || actual.value().pending_writes)
        return Status::fault("common original baseline or native binding differs");
    if (auto status = Ext4MountedCommonOpeningBudget::reserve(common_counts(actual.value())); !status) return status;
    if (actual.value().reads != actual.value().completed_reads || actual.value().writes != actual.value().completed_writes ||
        actual.value().flushes != actual.value().completed_flushes)
        return Status::fault("common original baseline lacks completed parity");
    common_opening_ = &opening;
    opening.phase_ = CommonOpeningPhase::classifying;
    return {};
}
[[nodiscard]] Status common_retained_exact() const
{
    const auto *opening = common_opening_;
    if (!opening) return {};
    if (opening->original_ != opening || &opening->owner_ != this || &opening->session_ != &cache_.session_ ||
        &opening->mount_ != startup_mount_ || &opening->startup_ != startup_workspace_ || !opening->initial_ ||
        !opening->binding_ || !opening->selection_ || !typed_attachment_ || &typed_attachment_->workspace_ != &opening->workspace_ ||
        !typed_workspace_anchors_exact_locked() || opening->phase_ == CommonOpeningPhase::unused ||
        opening->phase_ == CommonOpeningPhase::complete || opening->phase_ == CommonOpeningPhase::failed)
        return Status::fault("common opening lost its original retained context");
    const auto binding = cache_.session_.observe_binding(); const auto actual = cache_.session_.report();
    if (!binding || !actual) return !binding ? binding.status() : actual.status();
    if (binding.value() != opening->binding_.value())
        return Status::fault("common continuing native binding differs");
    auto capacity = opening->whole_;
    if (opening->selection_->minted_)
    {
        const auto &selection = *opening->selection_;
        if (selection.original_ != &selection || &selection.opening_ != opening ||
            selection.sources_[0] != startup_workspace_->locator_super_ ||
            selection.sources_[1] != startup_workspace_->locator_descriptors_ ||
            selection.sources_[2] != startup_workspace_->locator_table_ ||
            selection.sources_[3] != startup_workspace_->locator_head_ ||
            selection.sources_[4] != startup_workspace_->locator_descriptor_)
            return Status::fault("common original Selection or locator sources differ");
        const auto route = Ext4MountedCommonOpeningBudget::route(selection.family_);
        if (!route) return route.status();
        capacity = route.value();
    }
    else capacity = {5, 0, 0, 0, 0};
    const auto baseline = common_counts(opening->initial_.value()), endpoint = common_counts(actual.value());
    const std::array<block::SessionCounter, 6> limits{capacity.reads, capacity.reads, capacity.writes,
        capacity.writes, capacity.flushes, capacity.flushes};
    for (usize index = 0; index < limits.size(); ++index)
        if (endpoint[index] < baseline[index] || endpoint[index] - baseline[index] > limits[index])
            return Status::overflow("common selected native counter reservation exceeded");
    return {};
}
[[nodiscard]] Status select_common_endpoint(Ext4MountedCommonFamily family, u32 sequence, u8 reads)
{
    if (!common_opening_) return {};
    auto &opening = *common_opening_; auto &selection = *opening.selection_;
    if (auto status = common_retained_exact(); !status) return status;
    if (opening.phase_ != CommonOpeningPhase::classifying || selection.minted_ || !startup_route_opening_ ||
        !startup_active_ || startup_report_.phase != Ext4StartupPhase::classifying ||
        !startup_route_budget_.retained_exact() || startup_route_budget_.original_ != &cache_.session_ ||
        startup_route_budget_.owner_ != this || startup_route_budget_.control_.phase != Ext4ReclaimRoutePhase::opening ||
        startup_route_budget_.control_.operation != Ext4ReclaimRouteOperation::none ||
        startup_route_budget_.control_.attempts != std::array<block::SessionCounter, 3>{reads, 0, 0} ||
        startup_route_budget_.control_.completions != std::array<block::SessionCounter, 3>{reads, 0, 0} ||
        !exact_session(startup_route_budget_.initial_, opening.initial_.value()) ||
        !exact_session(startup_route_budget_.progress_, reclaim_io_))
        return Status::fault("common Selection lacks its actual completed original locator ledger");
    const auto actual = cache_.session_.report(); if (!actual) return actual.status();
    if (!exact_session(actual.value(), reclaim_io_)) return Status::fault("common actual locator endpoint differs");
    const auto narrowed = Ext4MountedCommonOpeningBudget::narrow(common_counts(opening.initial_.value()),
        common_counts(actual.value()), family, reads);
    if (!narrowed) return narrowed.status();
    const auto route = Ext4MountedCommonOpeningBudget::route(family); if (!route) return route.status();
    // Orphan/reclaim sequence capacity is refined by the existing genuine
    // complete-chain proof before its writer, using the actual chain length.
    const bool deferred_sequence = family == Ext4MountedCommonFamily::ordinary ||
        family == Ext4MountedCommonFamily::empty_orphan || family == Ext4MountedCommonFamily::reclaim;
    if (!sequence || (!deferred_sequence && sequence > ~u32{0} - route.value().sequence_delta))
        return Status::overflow("common authenticated selected sequence headroom exhausted");
    selection.sources_[0] = startup_workspace_->locator_super_;
    selection.sources_[1] = startup_workspace_->locator_descriptors_;
    selection.sources_[2] = startup_workspace_->locator_table_;
    selection.sources_[3] = startup_workspace_->locator_head_;
    selection.sources_[4] = startup_workspace_->locator_descriptor_;
    selection.endpoint_ = actual.value(); selection.remaining_ = narrowed.value();
    selection.family_ = family; selection.sequence_ = sequence; selection.reads_ = reads; selection.minted_ = true;
    opening.phase_ = CommonOpeningPhase::selected;
    return {};
}
[[nodiscard]] bool common_typed_selected() const
{
    return common_opening_ && common_opening_->selection_->minted_ &&
        (common_opening_->selection_->family_ == Ext4MountedCommonFamily::typed_zero ||
         common_opening_->selection_->family_ == Ext4MountedCommonFamily::typed_allocated);
}
template <typename Context>
[[nodiscard]] Status common_predecessor_context_exact(const CommonSelection &selection, const Context &context) const
{
    if (auto status = common_retained_exact(); !status) return status;
    if (!common_typed_selected() || &selection != common_opening_->selection_ || selection.original_ != &selection ||
        common_opening_->phase_ != CommonOpeningPhase::selected || selection.reads_ != 5 ||
        context.owner_ != this || context.mount_ != &common_opening_->mount_ || context.slot_ != &common_opening_->workspace_ ||
        &context.session_ != &cache_.session_ || context.serial_ != selection.endpoint_.session_serial ||
        !startup_active_ || !startup_route_opening_ || startup_report_.phase != Ext4StartupPhase::classifying ||
        !startup_route_budget_.retained_exact() || startup_route_budget_.control_.phase != Ext4ReclaimRoutePhase::opening ||
        startup_route_budget_.control_.operation != Ext4ReclaimRouteOperation::none ||
        startup_route_budget_.control_.attempts != std::array<block::SessionCounter, 3>{5, 0, 0} ||
        startup_route_budget_.control_.completions != std::array<block::SessionCounter, 3>{5, 0, 0} ||
        !exact_session(startup_route_budget_.initial_, common_opening_->initial_.value()) ||
        !exact_session(startup_route_budget_.progress_, selection.endpoint_) ||
        !exact_session(reclaim_io_, selection.endpoint_))
        return Status::fault("common cursor predecessor is not the actual original five-read Selection");
    const auto actual = cache_.session_.report();
    return actual && exact_session(actual.value(), selection.endpoint_) ? Status{} :
        actual ? Status::fault("common predecessor actual endpoint differs") : actual.status();
}
template <typename Cursor>
[[nodiscard]] Status adopt_common_typed_cursor(CommonSelection &selection, TypedMountedCursorAttachment<Cursor> &a)
{
    if (auto status = common_predecessor_context_exact(selection, a.context_); !status) return status;
    if (&a.owner_ != this || &a.session_ != &cache_.session_ || a.original_ != &a ||
        &a.workspace_ != &selection.opening_.workspace_ || a.admitted_ || a.cursor_.attempted_ || a.workspace_.attempted_ ||
        a.context_.bridge_check_ != &typed_mounted_bridge_thunk<Cursor> ||
        Cache::overlaps_storage(&a, sizeof(a), this, sizeof(*this)) ||
        Cache::overlaps_storage(&a, sizeof(a), &a.workspace_, sizeof(a.workspace_)) ||
        Cache::overlaps_storage(&a, sizeof(a), &a.cursor_, sizeof(a.cursor_)))
        return Status::invalid_argument("common typed predecessor has no disjoint original Attachment");
    if (auto status = startup_handoff_cache_quiescent(); !status) return status;
    if (auto status = a.cursor_.adopt_common_predecessor(a.context_, selection); !status) return status;
    {
        smp::ScopedSpinLock guard(lock_);
        smp::ScopedSpinLock cache_guard(cache_.lock_);
        if (auto status = common_predecessor_context_exact(selection, a.context_); !status) return status;
        if (typed_mounted_attachment_ || typed_mounted_pending_ || typed_attempted_ || typed_context_ ||
            authority_.active || admissions_ || participant_count_ || transaction_.active_ || has_data_ ||
            rename_active_ || create_active_ || first_data_active_ || next_data_active_ || namespace_active_ ||
            namespace_recovery_active_ || regular_move_active_ || regular_move_recovery_active_ ||
            startup_regular_move_io_.active || reclaim_prepared_ || reclaim_active_ ||
            !a.operation_serial_ || typed_operation_serial_ == ~u64{0} ||
            typed_operation_serial_ != typed_operation_serial_seal_ || a.operation_serial_ != typed_operation_serial_ + 1)
            return Status::busy("common typed transfer lost exclusive original Owner admission");
        if (auto status = cache_.typed_cache_idle(); !status) return status;
        if (auto status = startup_route_budget_.close(cache_.session_); !status) return status;
        // Install continuing exclusion while the original startup domain is held.
        typed_operation_serial_ = typed_operation_serial_seal_ = a.operation_serial_;
        a.workspace_.operation_serial_ = a.workspace_.operation_serial_seal_ = a.operation_serial_;
        typed_mounted_attachment_ = &a; typed_mounted_workspace_ = &a.workspace_;
        typed_mounted_pending_ = true; a.admitted_ = true;
        startup_route_opening_ = false; startup_active_ = false;
        report_.phase = Ext4OwnedPhase::open;
        selection.opening_.phase_ = CommonOpeningPhase::typed_adopted;
    }
    return a.cursor_.idle(a.context_);
}
[[nodiscard]] Status common_retained_locator(const CommonSelection &selection, u32 physical, Block &bytes) const
{
    if (auto status = common_retained_exact(); !status) return status;
    if (!common_typed_selected() || common_opening_->selection_ != &selection ||
        common_opening_->phase_ != CommonOpeningPhase::typed_adopted || selection.reads_ != 5)
        return Status::bad_descriptor("typed retained locator lacks its original adopted Selection");
    const std::array<u32, 5> blocks{1, 2, startup_journal_table_, startup_journal_first_, startup_journal_first_ + 1};
    const std::array<const Block *, 5> sources{&startup_workspace_->locator_super_, &startup_workspace_->locator_descriptors_,
        &startup_workspace_->locator_table_, &startup_workspace_->locator_head_, &startup_workspace_->locator_descriptor_};
    for (usize index = 0; index < blocks.size(); ++index)
        if (physical == blocks[index])
            return sources[index] == &bytes && selection.sources_[index] == bytes ? Status{} :
                Status::fault("typed retained physical locator storage differs");
    return Status::bad_descriptor("typed retained locator requested an unauthenticated source block");
}
[[nodiscard]] bool common_root_domain_active() const
{ return common_opening_ && common_opening_->phase_ == CommonOpeningPhase::legacy_root; }
[[nodiscard]] Status common_root_exact() const
{
    if (!common_root_domain_active() || common_typed_selected() || startup_active_ || startup_route_opening_ ||
        startup_report_.phase != Ext4StartupPhase::complete || report_.phase != Ext4OwnedPhase::open ||
        typed_mounted_attachment_ || typed_mounted_pending_ || typed_attempted_ || typed_context_ ||
        authority_.active || admissions_ || participant_count_ || transaction_.active_ || common_opening_->root_reads_ > 18)
        return Status::fault("common legacy root lacks exclusive authenticated terminal admission");
    return common_retained_exact();
}
[[nodiscard]] Status finish_common_opening(CommonOpening &opening, Status status, u32 verified_final_sequence)
{
    if (common_opening_ != &opening || opening.original_ != &opening)
        return Status::fault("common opening terminal context differs");
    if (status)
    {
        if (auto exact = common_retained_exact(); !exact) status = exact;
        if (status && opening.phase_ != CommonOpeningPhase::legacy_root && opening.phase_ != CommonOpeningPhase::typed_adopted)
            status = Status::fault("common opening lacks its selected terminal continuation");
        if (status && opening.phase_ == CommonOpeningPhase::typed_adopted)
        {
            const auto actual = opening.session_.report();
            const auto route = Ext4MountedCommonOpeningBudget::route(opening.selection_->family_);
            smp::ScopedSpinLock guard(lock_);
            if (!actual || !route) status = !actual ? actual.status() : route.status();
            else if (!common_typed_selected() || startup_active_ || startup_route_opening_ ||
                startup_report_.phase != Ext4StartupPhase::classifying || typed_native_public_blocked() ||
                !typed_workspace_anchors_exact_locked() || opening.workspace_.phase_ != TypedOwnerPhase::ready ||
                actual.value().session_serial != opening.initial_.value().session_serial ||
                actual.value().state != block::DeviceState::owned || actual.value().first_error != StatusCode::ok ||
                actual.value().in_flight || actual.value().pending_writes ||
                actual.value().reads != actual.value().completed_reads || actual.value().writes != actual.value().completed_writes ||
                actual.value().flushes != actual.value().completed_flushes ||
                verified_final_sequence != opening.selection_->sequence_ + route.value().sequence_delta)
                status = Status::fault("common typed terminal startup observation lacks actual publication and renewal");
            else
            {
                // This observation is published only after the original scope
                // validated the real readable root and operation objects ended.
                startup_report_.observed = actual.value(); startup_report_.final_head = 0;
                startup_report_.final_sequence = verified_final_sequence;
                startup_report_.root_unpublished = false;
                startup_report_.phase = Ext4StartupPhase::complete;
            }
        }
    }
    if (!status)
    {
        startup_failed(status);
        static_cast<void>(opening.session_.abort(status));
    }
    opening.phase_ = status ? CommonOpeningPhase::complete : CommonOpeningPhase::failed;
    common_opening_ = nullptr;
    destroy_common_selection(opening);
    return status;
}

class CommonRootPinReadGuard final
{
  public:
    static constexpr bool defer_cache_begin = true;
    CommonRootPinReadGuard(Ext4OwnedTransaction &owner, u64 block, Pin &pin)
        : owner_(owner), original_(&owner.cache_.session_), workspace_(owner.startup_workspace_),
          mount_(owner.startup_mount_), pin_(&pin), block_(block), before_(owner.reclaim_io_),
          family_(owner.startup_report_.initial_family), legacy_(owner.startup_legacy_selected_),
          actions_(owner.startup_legacy_actions_), cold_(owner.startup_seal_.active),
          namespace_(owner.startup_namespace_seal_.active), reclaim_minted_(owner.startup_reclaim_seal_.minted)
    { bound_ = anchor_.bind(*original_); }
    ~CommonRootPinReadGuard()
    {
        if (pending_ || pending_seal_)
            static_cast<void>(anchor_.abort(Status::interrupted("common legacy root canonical miss ended before finish")));
    }
    CommonRootPinReadGuard(const CommonRootPinReadGuard &) = delete;
    CommonRootPinReadGuard &operator=(const CommonRootPinReadGuard &) = delete;
    CommonRootPinReadGuard(CommonRootPinReadGuard &&) = delete;
    CommonRootPinReadGuard &operator=(CommonRootPinReadGuard &&) = delete;
    [[nodiscard]] Status begin_read(typename Registry::Session &, u64, usize)
    { return Status::fault("common legacy root canonical miss requires deferred cache selection"); }
    [[nodiscard]] Status begin_cache_read(typename Registry::Session &session, u64 first, usize bytes,
                                          usize slot, u64 generation)
    {
        if (pending_ || pending_seal_ || slot >= BufferCapacity || !generation ||
            block_ >= 32768 || first != block_ * 2 || bytes != 1024 || pin_->owner_)
            return Status::fault("common legacy root canonical miss route or transfer differs");
        if (auto status = retained_exact(session); !status) return status;
        if (auto status = owner_.common_root_exact(); !status) return status;
        if (owner_.common_opening_->root_reads_ == 18) return Status::overflow("common root decoder whole READ allowance exhausted");
        if (before_.pending_writes || before_.reads == ~block::SessionCounter{0} || before_.completed_reads == ~block::SessionCounter{0})
            return Status::overflow("common legacy root canonical miss lacks read credit");
        slot_ = slot_seal_ = slot; generation_ = generation_seal_ = generation;
        pending_ = pending_seal_ = true;
        return {};
    }
    [[nodiscard]] Status in_flight_exact(typename Registry::Session &session) const
    {
        if (!pending_ || !pending_seal_)
            return Status::fault("common legacy root canonical miss has no pending dispatch");
        if (auto status = retained_exact(session); !status) return status;
        if (auto status = loading_exact(); !status) return status;
        auto expected = before_; ++expected.reads; expected.in_flight = true;
        const auto actual = session.report();
        if (!actual) return actual.status();
        return exact_session(actual.value(), expected) ? Status{} :
            Status::fault("common legacy root canonical miss exact attempted Session prefix differs");
    }
    [[nodiscard]] Status finish_read(typename Registry::Session &session, Status transport)
    {
        // Transport refusal remains the first error even after authority drifts.
        if (!transport) { pending_ = pending_seal_ = false; return transport; }
        if (!pending_ || !pending_seal_)
            return Status::fault("common legacy root canonical miss finish lacks pending dispatch");
        if (auto status = retained_exact(session); !status) return status;
        if (auto status = loading_exact(); !status) return status;
        const auto actual = session.report();
        if (!actual) return actual.status();
        auto expected = before_; ++expected.reads; ++expected.completed_reads;
        if (!exact_session(actual.value(), expected))
            return Status::fault("common legacy root canonical miss exact returned Session prefix differs");
        owner_.reclaim_io_ = expected; ++owner_.common_opening_->root_reads_; pending_ = pending_seal_ = false;
        return {};
    }
  private:
    [[nodiscard]] Status retained_exact(typename Registry::Session &session) const
    {
        if (!bound_) return bound_;
        if (!anchor_.exact(&session) || &session != original_ || &owner_.cache_.session_ != original_ ||
            owner_.startup_workspace_ != workspace_ || owner_.startup_mount_ != mount_ ||
            !exact_session(owner_.reclaim_io_, before_))
            return Status::fault("common root canonical miss lost its original fixed authority");
        return owner_.common_root_exact();
    }
    [[nodiscard]] Status loading_exact() const
    {
        if (slot_ != slot_seal_ || slot_ >= BufferCapacity || !generation_ || generation_ != generation_seal_)
            return Status::fault("common legacy root canonical miss paused slot selection differs");
        smp::ScopedSpinLock guard(owner_.cache_.lock_);
        const auto &entry = owner_.cache_.entries_[slot_].report;
        if (!owner_.cache_.report_.in_flight || entry.state != MetadataBufferState::loading ||
            entry.block != block_ || entry.generation != generation_ || entry.read_pins != 1 ||
            entry.transaction_serial || entry.staged_dirty || pin_->owner_ != &owner_.cache_ ||
            pin_->slot_ != slot_ || pin_->generation_ != generation_ || pin_->active_)
            return Status::fault("common legacy root canonical miss original loading slot or Pin differs");
        return {};
    }
    Ext4OwnedTransaction &owner_;
    typename Registry::Session *const original_;
    const Ext4MountedStartupWorkspace *const workspace_;
    const Ext4MountedMetadata<DeviceCapacity> *const mount_;
    Pin *const pin_;
    const u64 block_;
    const block::SessionReport before_;
    const Ext4StartupFamily family_;
    const bool legacy_;
    const usize actions_;
    const bool cold_, namespace_, reclaim_minted_;
    reclaim_history::SessionAnchor<typename Registry::Session> anchor_{};
    Status bound_{};
    usize slot_{BufferCapacity}, slot_seal_{BufferCapacity};
    u64 generation_{}, generation_seal_{};
    bool pending_{}, pending_seal_{};
};
[[nodiscard]] Status common_root_pin(u64 block, Pin &pin)
{
    if (auto status = common_root_exact(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    CommonRootPinReadGuard guard(*this, block, pin);
    if (auto status = cache_.pin(block, pin, guard); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    return common_root_exact();
}
