// Private native child/cursor bridge. Concrete mounted authority and renewal
// compose separately; public reports cannot construct this retained attachment.
[[nodiscard]] TypedSession &typed_mounted_original_session() { return cache_.session_; }
[[nodiscard]] const TypedSession &typed_mounted_original_session() const { return cache_.session_; }
template <typename Cursor>
class TypedMountedCursorAttachment final
{
    friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4MountedMetadata;
    TypedMountedCursorAttachment(Ext4OwnedTransaction &owner, TypedOwnerWorkspace &workspace,
        Cursor &cursor, typename Cursor::Context &context)
        : owner_(owner), workspace_(workspace), cursor_(cursor), context_(context),
          session_(owner.cache_.session_), original_(this), serial_(context.serial_),
          operation_serial_(owner.typed_operation_serial_ == ~u64{0} ? 0 : owner.typed_operation_serial_ + 1) {}
    ~TypedMountedCursorAttachment()
    {
        if (admitted_)
            static_cast<void>(session_.abort(Status::interrupted("mounted cursor attachment ended before renewal")));
    }
    TypedMountedCursorAttachment(const TypedMountedCursorAttachment &) = delete;
    TypedMountedCursorAttachment &operator=(const TypedMountedCursorAttachment &) = delete;
    TypedMountedCursorAttachment(TypedMountedCursorAttachment &&) = delete;
    TypedMountedCursorAttachment &operator=(TypedMountedCursorAttachment &&) = delete;
    Ext4OwnedTransaction &owner_;
    TypedOwnerWorkspace &workspace_;
    Cursor &cursor_;
    typename Cursor::Context &context_;
    TypedSession &session_;
    const TypedMountedCursorAttachment *const original_;
    const u64 serial_, operation_serial_;
    block::SessionReport child_terminal_{}, child_terminal_seal_{}, suffix_terminal_{};
    const void *child_{};
    const typename Cursor::Receipt *receipt_{};
    bool admitted_{}, resume_attempted_{}, resumed_{}, receipt_consumed_{}, renewed_{};
};
template <typename Cursor>
[[nodiscard]] Status typed_mounted_cursor_stop(TypedMountedCursorAttachment<Cursor> &attachment, Status error)
{
    if (error) error = Status::fault("mounted cursor stop requires an error");
    static_cast<void>(attachment.session_.abort(error));
    smp::ScopedSpinLock guard(attachment.owner_.lock_);
    return attachment.owner_.quarantine_typed_renewal_locked(error);
}
template <typename Cursor>
[[nodiscard]] static Status typed_mounted_bridge_thunk(const void *raw_owner, const void *raw_context,
    const void *raw_attachment)
{
    // raw_owner is the immutable concrete Owner identity captured by the private
    // Context constructor. Check its live admission slot before interpreting
    // the mutable attachment observation as a C++ object.
    if (!raw_owner) return Status::fault("mounted cursor original Owner callback absent");
    const auto &owner = *static_cast<const Ext4OwnedTransaction *>(raw_owner);
    {
        smp::ScopedSpinLock guard(owner.lock_);
        if (!raw_attachment || owner.typed_mounted_attachment_ != raw_attachment || !owner.typed_mounted_pending_)
            return Status::fault("mounted cursor original Owner callback attachment differs");
    }
    const auto &a = *static_cast<const TypedMountedCursorAttachment<Cursor> *>(raw_attachment);
    if (&a.owner_ != &owner || &a.context_ != raw_context)
        return Status::fault("mounted cursor original Owner callback context differs");
    return owner.typed_mounted_cursor_exact(a);
}
template <typename Cursor>
[[nodiscard]] Status typed_mounted_attachment_exact(const TypedMountedCursorAttachment<Cursor> &a) const
{
    smp::ScopedSpinLock guard(lock_);
    if (&a.owner_ != this || &a.session_ != &cache_.session_ || a.original_ != &a || !a.admitted_ ||
        !typed_workspace_anchors_exact_locked() || typed_renewal_quarantined_ || a.renewed_ ||
        !a.operation_serial_ || a.operation_serial_ != typed_operation_serial_ ||
        typed_operation_serial_ != typed_operation_serial_seal_ ||
        a.workspace_.operation_serial_ != a.operation_serial_ || a.workspace_.operation_serial_seal_ != a.operation_serial_ ||
        !a.serial_ || a.serial_ != a.context_.serial_ || &a.context_.session_ != &a.session_ ||
        &a.cursor_.original_anchor_ != &a.session_ || a.context_.owner_ != this || !a.context_.mount_ ||
        a.context_.slot_ != &a.workspace_ || a.context_.bridge_check_ != &typed_mounted_bridge_thunk<Cursor> ||
        typed_mounted_attachment_ != &a ||
        typed_mounted_workspace_ != &a.workspace_ || !typed_mounted_pending_ || !report_.status ||
        report_.phase == Ext4OwnedPhase::failed || report_.phase == Ext4OwnedPhase::closed)
        return Status::fault("mounted cursor lost its original admitted attachment");
    return {};
}
template <typename Cursor>
[[nodiscard]] Status typed_admit_mounted_cursor(TypedMountedCursorAttachment<Cursor> &a)
{
    static_assert(std::same_as<typename Cursor::NativeSession, TypedSession>);
    if (&a.owner_ != this || &a.session_ != &cache_.session_ || &a.cursor_.original_anchor_ != &a.session_ ||
        a.original_ != &a || !a.serial_ || a.context_.serial_ != a.serial_ ||
        &a.context_.session_ != &a.session_ || a.context_.owner_ != this || !a.context_.mount_ ||
        a.context_.slot_ != &a.workspace_ || a.context_.bridge_check_ != &typed_mounted_bridge_thunk<Cursor> ||
        a.admitted_ || a.cursor_.attempted_ || a.workspace_.attempted_ ||
        Cache::overlaps_storage(&a, sizeof(a), this, sizeof(*this)) ||
        Cache::overlaps_storage(&a, sizeof(a), &a.workspace_, sizeof(a.workspace_)) ||
        Cache::overlaps_storage(&a, sizeof(a), &a.cursor_, sizeof(a.cursor_)))
        return Status::invalid_argument("mounted cursor original disjoint admission record");
    if (auto status = a.workspace_.claim_for_child(*this, cache_, cache_.session_); !status) return status;
    smp::ScopedSpinLock guard(lock_);
    if (typed_renewal_quarantined_) return Status::fault("typed owner renewal is permanently quarantined");
    if (!typed_workspace_anchors_exact_locked())
        return quarantine_typed_renewal_locked(Status::fault("mounted admission permanent workspace anchor changed"));
    if (typed_attachment_ && &typed_attachment_->workspace_ != &a.workspace_)
        return Status::bad_descriptor("mounted admission requires its permanent original workspace");
    if (!a.operation_serial_ || typed_operation_serial_ == ~u64{0})
        return Status::overflow("typed owner operation serial exhausted before classifier");
    if (typed_operation_serial_ != typed_operation_serial_seal_ || a.operation_serial_ != typed_operation_serial_ + 1)
        return quarantine_typed_renewal_locked(Status::fault("mounted admission private operation serial changed"));
    if (typed_mounted_attachment_ || typed_mounted_pending_ || typed_attempted_ || startup_active_ ||
        startup_route_opening_ || admissions_ || authority_.active || report_.phase != Ext4OwnedPhase::open ||
        !report_.status || !mounted_dirty_ || participant_count_ || transaction_.active_ || has_data_ ||
        rename_active_ || create_active_ || first_data_active_ || next_data_active_ || namespace_active_ ||
        namespace_recovery_active_ || reclaim_prepared_ || reclaim_active_)
        return Status::busy("mounted cursor needs an exclusive original Owner before classifier");
    smp::ScopedSpinLock cache_guard(cache_.lock_);
    if (auto status = cache_.typed_cache_idle(); !status) return status;
    // Establish public exclusion before READ1 and before typed_handoff clears
    // its shorter-lived child context. No callback or transport occurs here.
    if (!typed_attachment_) typed_attachment_.emplace(*this, a.workspace_, a.session_);
    typed_workspace_ = typed_workspace_seal_ = &a.workspace_;
    typed_operation_serial_ = typed_operation_serial_seal_ = a.operation_serial_;
    a.workspace_.operation_serial_ = a.workspace_.operation_serial_seal_ = a.operation_serial_;
    typed_mounted_attachment_ = &a; typed_mounted_workspace_ = &a.workspace_;
    typed_mounted_pending_ = true; a.admitted_ = true;
    return {};
}
template <typename Cursor>
[[nodiscard]] Status typed_cancel_mounted_cursor_before_child(
    TypedMountedCursorAttachment<Cursor> &a, Status reason)
{
    if (reason) return Status::invalid_argument("mounted cursor clean cancellation needs a refusal");
    if (auto status = typed_mounted_attachment_exact(a); !status)
        return typed_mounted_cursor_stop(a, status);
    if (a.workspace_.attempted_ || a.resume_attempted_ || a.resumed_ || a.child_ ||
        a.receipt_ || a.receipt_consumed_ || a.cursor_.bridge_minted_)
        return Status::bad_descriptor("mounted cursor clean cancellation follows child admission");
    const auto cancelled = a.cursor_.cancel_before_child(a.context_, reason);
    if (a.cursor_.control_.phase != Ext4MountedTypedCursorPhase::refused)
        return typed_mounted_cursor_stop(a, cancelled ?
            Status::fault("mounted cursor cancellation has no refused terminal") : cancelled);
    const auto actual = a.session_.report();
    auto endpoint = a.cursor_.initial_;
    const auto reads = a.cursor_.control_.classifier_reads;
    if (reads > ~block::SessionCounter{0} - endpoint.reads || reads > ~block::SessionCounter{0} - endpoint.completed_reads)
        return typed_mounted_cursor_stop(a, Status::fault("mounted cursor cancellation counter overflow"));
    endpoint.reads += reads; endpoint.completed_reads += reads;
    if (!actual || !reclaim_history::same_session(actual.value(), endpoint) ||
        !reclaim_history::same_session(a.cursor_.progress_, endpoint) ||
        a.cursor_.control_ != a.cursor_.sealed_ || a.cursor_.control_.pending ||
        a.cursor_.control_.suffix_reads || a.cursor_.context_ || a.cursor_.context_seal_)
        return typed_mounted_cursor_stop(a, actual ?
            Status::fault("mounted cursor cancellation original native endpoint differs") : actual.status());
    smp::ScopedSpinLock guard(lock_);
    if (!typed_workspace_anchors_exact_locked() || typed_renewal_quarantined_ ||
        a.workspace_.renewal_quarantined_ || !typed_attachment_ ||
        &typed_attachment_->workspace_ != &a.workspace_ ||
        typed_mounted_attachment_ != &a || typed_mounted_workspace_ != &a.workspace_ ||
        !typed_mounted_pending_ || typed_attempted_ || typed_completed_ || typed_context_ ||
        !a.operation_serial_ || typed_operation_serial_ != a.operation_serial_ ||
        typed_operation_serial_seal_ != a.operation_serial_ ||
        a.workspace_.operation_serial_ != a.operation_serial_ ||
        a.workspace_.operation_serial_seal_ != a.operation_serial_ ||
        typed_handoff_serial_ != typed_renewed_serial_ || typed_renewed_serial_ >= a.operation_serial_ ||
        a.workspace_.handoff_serial_ != typed_handoff_serial_ ||
        a.workspace_.handoff_serial_seal_ != typed_handoff_serial_ ||
        report_.phase != Ext4OwnedPhase::open || !report_.status ||
        a.workspace_.attempted_ || a.workspace_.consumed_ || a.resume_attempted_ || a.resumed_ ||
        a.receipt_consumed_ || a.renewed_)
        return quarantine_typed_renewal_locked(Status::fault("mounted cursor cancellation original Owner admission changed"));
    Status cache_status;
    {
        smp::ScopedSpinLock cache_guard(cache_.lock_);
        cache_status = cache_.typed_cache_idle();
    }
    // Quarantine reacquires the cache lock; retain only the Owner lock here.
    if (!cache_status) return quarantine_typed_renewal_locked(cache_status);
    // Cancellation retires only this admission. Permanent borrows, consumed
    // operation serial and earlier genuine handoff/renewal history stay intact.
    typed_mounted_attachment_ = nullptr; typed_mounted_workspace_ = nullptr;
    typed_mounted_pending_ = false; a.admitted_ = false;
    return reason;
}
// Suffix transport may have an actual native READ in flight. Compare only
// inert Owner/cache/workspace state here; the finite cursor checks live counters.
[[nodiscard]] Status typed_mounted_workspace_retained_exact(const TypedOwnerWorkspace &workspace) const
{
    smp::ScopedSpinLock guard(lock_);
    if (!typed_workspace_anchors_exact_locked() || typed_renewal_quarantined_ || workspace.renewal_quarantined_ ||
        !typed_operation_serial_ || typed_operation_serial_ != typed_operation_serial_seal_ ||
        workspace.operation_serial_ != typed_operation_serial_ || workspace.operation_serial_seal_ != typed_operation_serial_ ||
        workspace.handoff_serial_ != typed_operation_serial_ || workspace.handoff_serial_seal_ != typed_operation_serial_ ||
        typed_handoff_serial_ != typed_operation_serial_ ||
        !typed_mounted_pending_ || typed_mounted_workspace_ != &workspace || !typed_mounted_attachment_ ||
        typed_workspace_ != &workspace || !typed_attempted_ || !typed_completed_ || typed_context_ ||
        report_.phase != Ext4OwnedPhase::checkpointed || !report_.status ||
        workspace.original_ != &workspace || workspace.owner_ != this || workspace.cache_ != &cache_ ||
        workspace.session_ != &cache_.session_ || !workspace.attempted_ || !workspace.consumed_ ||
        workspace.phase_ != TypedOwnerPhase::published || workspace.phase_seal_ != workspace.phase_ ||
        !reclaim_history::same_info(workspace.binding_, workspace.binding_seal_) ||
        !reclaim_history::same_session(workspace.initial_, workspace.initial_seal_) ||
        !reclaim_history::same_session(workspace.child_initial_, workspace.child_initial_seal_) ||
        workspace.prefix_reservation_ != BufferCapacity || workspace.prefix_reads_ > BufferCapacity ||
        workspace.data_request_ != workspace.data_request_seal_)
        return Status::fault("mounted cursor completed workspace changed during suffix");
    auto prefix = workspace.initial_;
    if (workspace.prefix_reads_ > ~block::SessionCounter{0} - prefix.reads ||
        workspace.prefix_reads_ > ~block::SessionCounter{0} - prefix.completed_reads)
        return Status::fault("mounted cursor original prefix counter overflow");
    prefix.reads += workspace.prefix_reads_; prefix.completed_reads += workspace.prefix_reads_;
    if (!reclaim_history::same_session(prefix, workspace.prefix_progress_) ||
        !reclaim_history::same_session(prefix, workspace.child_initial_))
        return Status::fault("mounted cursor original prefix endpoint changed");
    const auto binding = cache_.session_.observe_binding();
    if (!binding || binding.value() != workspace.session_binding_)
        return Status::fault("mounted cursor original native binding changed");
    smp::ScopedSpinLock cache_guard(cache_.lock_);
    return typed_published_cache_exact_locked(workspace);
}
template <typename Cursor, typename Child>
[[nodiscard]] Status typed_mounted_child_retained_exact(const TypedMountedCursorAttachment<Cursor> &a,
    const Child &child) const
{
    if (auto status = typed_mounted_attachment_exact(a); !status) return status;
    if (auto status = typed_mounted_workspace_retained_exact(a.workspace_); !status) return status;
    // Preflight the actual addresses and all callbacks before retained() can
    // inspect private state. This predicate invokes no child idle/transport.
    if (!a.resumed_ || a.child_ != &child || !child.status_ || child.control_.phase != Child::Phase::complete ||
        child.control_ != child.sealed_ || child.control_.pending || child.control_.pending_writes ||
        child.original_ != &a.session_ || child.original_seal_ != &a.session_ || !child.anchor_.exact(&a.session_) ||
        !a.workspace_.zero_.completed_owner_callbacks_retired() ||
        !a.workspace_.allocated_.completed_owner_callbacks_retired() || !child.completed_ownership_birth_valid(true) ||
        !child.retained(false) || !reclaim_history::same_info(child.binding_, a.workspace_.binding_) ||
        !reclaim_history::same_session(child.initial_, a.workspace_.child_initial_) ||
        !reclaim_history::same_session(child.progress_, a.child_terminal_) ||
        !reclaim_history::same_session(a.child_terminal_, a.child_terminal_seal_) ||
        report_.completion.journal_sequence != child.reserved_.final_sequence)
        return Status::fault("mounted cursor actual child terminal or ownership birth changed");
    const auto selected = [&] {
        if constexpr (std::same_as<Child, TypedAllocated>)
            return child.route_ == Child::Route::recovery ? Ext4MountedTypedRoute::allocated_recovery :
                Ext4MountedTypedRoute::allocated_fresh;
        else return child.route_ == Child::Route::noop ? Ext4MountedTypedRoute::zero_noop :
            child.route_ == Child::Route::recovery ? Ext4MountedTypedRoute::zero_recovery :
                Ext4MountedTypedRoute::zero_fresh;
    }();
    if (a.workspace_.allocated_route_ != std::same_as<Child, TypedAllocated> ||
        a.workspace_.recovery_ != (child.route_ == Child::Route::recovery) ||
        a.cursor_.capacity_.route != selected || !a.cursor_.bridge_minted_ ||
        a.cursor_.bridge_workspace_ != &a.workspace_ || a.cursor_.bridge_child_ != &child ||
        a.cursor_.bridge_attachment_ != &a ||
        !reclaim_history::same_session(a.cursor_.bridge_terminal_, a.child_terminal_) ||
        !reclaim_history::same_session(a.cursor_.bridge_terminal_seal_, a.child_terminal_) ||
        a.receipt_consumed_ != a.cursor_.control_.receipt_consumed ||
        !a.cursor_.retained(a.context_, false, a.receipt_consumed_))
        return Status::fault("mounted cursor original child bridge relation changed");
    return {};
}
template <typename Cursor>
[[nodiscard]] Status typed_mounted_cursor_exact(const TypedMountedCursorAttachment<Cursor> &a) const
{
    if (a.workspace_.allocated_route_) return typed_mounted_child_retained_exact(a, a.workspace_.allocated_);
    return typed_mounted_child_retained_exact(a, a.workspace_.zero_);
}
// Only the actual admitted bridge selects this lineage. The visitor runs with
// Owner/cache locks released and can perform the finite concrete suffix helper.
// Both checks are inert: they never give the completed child new idle credit.
template <typename Cursor, typename Child, typename Visitor>
[[nodiscard]] Status typed_mounted_visit_child_lineage(const TypedMountedCursorAttachment<Cursor> &a,
    const Child &child, Visitor &visitor) const
{
    if (auto status = typed_mounted_child_retained_exact(a, child); !status) return status;
    const auto invoke = [&]() -> Status {
        if constexpr (std::same_as<Child, TypedAllocated>)
            return visitor(child.final_view_, child.final_proof_, child.final_binding_,
                child.images_.plan.targets[3], child.images_.plan.after[3], child.reserved_.final_sequence, true, false);
        else if (child.route_ == Child::Route::noop)
            return visitor(child.old_view_, child.old_proof_, child.binding_,
                child.images_.plan.targets[0], child.images_.plan.after[0], child.reserved_.final_sequence, false, true);
        else return visitor(child.final_view_, child.final_proof_, child.final_binding_,
                child.images_.plan.targets[0], child.images_.plan.after[0], child.reserved_.final_sequence, false, false);
    };
    const auto status = invoke();
    const auto exact = typed_mounted_child_retained_exact(a, child);
    // Retain the first helper error when its quarantine invalidates the bookend.
    return status ? exact : status;
}
template <typename Cursor, typename Visitor>
[[nodiscard]] Status typed_mounted_visit_lineage(const TypedMountedCursorAttachment<Cursor> &a,
    Visitor &visitor) const
{
    if (a.workspace_.allocated_route_) return typed_mounted_visit_child_lineage(a, a.workspace_.allocated_, visitor);
    return typed_mounted_visit_child_lineage(a, a.workspace_.zero_, visitor);
}
template <typename Cursor, typename Child>
[[nodiscard]] Status typed_resume_mounted_child(TypedMountedCursorAttachment<Cursor> &a, Child &child)
{
    using Phase = Ext4MountedTypedCursorPhase;
    auto &cursor = a.cursor_;
    auto &workspace = a.workspace_;
    if (auto status = typed_completed_child_exact(workspace, child); !status) return typed_mounted_cursor_stop(a, status);
    if (auto status = cursor.guard(a.context_); !status) return typed_mounted_cursor_stop(a, status);
    // Concrete authority observations can call device/service code. Repeat the
    // complete actual child predicate before the callback-free attachment.
    if (auto status = typed_completed_child_exact(workspace, child); !status) return typed_mounted_cursor_stop(a, status);
    if (auto status = typed_mounted_attachment_exact(a); !status) return typed_mounted_cursor_stop(a, status);
    auto classifier = cursor.initial_;
    if (cursor.control_.classifier_reads > ~block::SessionCounter{0} - classifier.reads ||
        cursor.control_.classifier_reads > ~block::SessionCounter{0} - classifier.completed_reads)
        return typed_mounted_cursor_stop(a, Status::fault("mounted cursor classifier counter overflow"));
    classifier.reads += cursor.control_.classifier_reads;
    classifier.completed_reads += cursor.control_.classifier_reads;
    const auto route = [&] {
        if constexpr (std::same_as<Child, TypedAllocated>)
            return child.route_ == Child::Route::recovery ? Ext4MountedTypedRoute::allocated_recovery :
                Ext4MountedTypedRoute::allocated_fresh;
        else return child.route_ == Child::Route::noop ? Ext4MountedTypedRoute::zero_noop :
            child.route_ == Child::Route::recovery ? Ext4MountedTypedRoute::zero_recovery :
                Ext4MountedTypedRoute::zero_fresh;
    }();
    if (!cursor.retained(a.context_) || cursor.control_.phase != Phase::paused_child || cursor.control_.pending ||
        cursor.bridge_minted_ || cursor.bridge_attachment_ || cursor.bridge_workspace_ || cursor.bridge_child_ ||
        !reclaim_history::same_session(cursor.paused_, workspace.initial_) ||
        !reclaim_history::same_session(cursor.paused_, classifier) ||
        !reclaim_history::same_session(cursor.progress_, workspace.initial_) ||
        !reclaim_history::same_session(child.initial_, workspace.prefix_progress_) ||
        !child.completed_ownership_birth_valid(true) || cursor.capacity_.route != route ||
        cursor.control_.final_sequence != child.reserved_.final_sequence ||
        cursor.groups_ != workspace.binding_.metadata_count / 4 ||
        cursor.blocks_ != workspace.binding_.layout.filesystem_blocks ||
        !Cursor::same_geometry(cursor.geometry_, child.geometry_) || !Cursor::same_bounds(cursor.bounds_, child.bounds_) ||
        child.authoritative_binding_ != cursor.binding_ ||
        child.reserved_.reads > cursor.capacity_.child_reads || child.reserved_.writes > cursor.capacity_.writes ||
        child.reserved_.flushes > cursor.capacity_.flushes || workspace.prefix_reads_ > cursor.capacity_.owner_prefix_reads)
        return typed_mounted_cursor_stop(a, Status::fault("mounted cursor pause/prefix/selected actual child chain differs"));
    auto terminal = child.initial_;
    const auto add = [](block::SessionCounter &value, block::SessionCounter delta) {
        if (delta > ~block::SessionCounter{0} - value) return false;
        value += delta; return true;
    };
    const auto &delta = child.control_;
    if (!add(terminal.reads, delta.reads) || !add(terminal.completed_reads, delta.completed_reads) ||
        !add(terminal.writes, delta.writes) || !add(terminal.completed_writes, delta.completed_writes) ||
        !add(terminal.flushes, delta.flushes) || !add(terminal.completed_flushes, delta.completed_flushes) ||
        !reclaim_history::same_session(terminal, child.progress_))
        return typed_mounted_cursor_stop(a, Status::fault("mounted cursor actual six child deltas differ"));
    const auto actual = a.session_.report();
    if (!actual || !reclaim_history::same_session(actual.value(), terminal))
        return typed_mounted_cursor_stop(a, actual ? Status::fault("mounted cursor actual terminal endpoint differs") : actual.status());
    // No callback, transport, cache operation or old child progress write.
    a.child_ = &child; a.child_terminal_ = a.child_terminal_seal_ = terminal;
    cursor.bridge_workspace_ = &workspace; cursor.bridge_child_ = &child; cursor.bridge_attachment_ = &a;
    cursor.bridge_terminal_ = cursor.bridge_terminal_seal_ = terminal; cursor.bridge_minted_ = true;
    cursor.progress_ = cursor.progress_seal_ = cursor.observed_ = terminal;
    cursor.control_.phase = Phase::suffix; cursor.sync(); a.resumed_ = true;
    return {};
}
template <typename Cursor>
[[nodiscard]] Status typed_resume_mounted_cursor(TypedMountedCursorAttachment<Cursor> &a)
{
    if (auto status = typed_mounted_attachment_exact(a); !status) return typed_mounted_cursor_stop(a, status);
    if (a.resume_attempted_) return Status::already_exists("mounted cursor authentic resume is one-shot");
    a.resume_attempted_ = true;
    if (a.workspace_.allocated_route_) return typed_resume_mounted_child(a, a.workspace_.allocated_);
    return typed_resume_mounted_child(a, a.workspace_.zero_);
}
template <typename Cursor>
[[nodiscard]] Status typed_mounted_consumed_receipt_exact(const TypedMountedCursorAttachment<Cursor> &a,
    const typename Cursor::Receipt &receipt) const
{
    if (auto status = typed_mounted_cursor_exact(a); !status) return status;
    if (!a.receipt_consumed_ || a.receipt_ != &receipt || !receipt.minted_ || !receipt.consumed_ ||
        receipt.cursor_ != &a.cursor_ || receipt.context_ != &a.context_ || receipt.session_ != &a.session_ ||
        receipt.owner_ != this || receipt.mount_ != a.context_.mount_ || receipt.serial_ != a.serial_ ||
        receipt.sequence_ != a.cursor_.control_.final_sequence ||
        !reclaim_history::same_session(receipt.endpoint_, a.suffix_terminal_) ||
        !reclaim_history::same_session(a.cursor_.progress_, a.suffix_terminal_))
        return Status::fault("mounted cursor original consumed receipt differs");
    const auto actual = a.session_.report();
    if (!actual || !reclaim_history::same_session(actual.value(), a.suffix_terminal_))
        return actual ? Status::fault("mounted cursor consumed native endpoint differs") : actual.status();
    return {};
}
template <typename Cursor>
[[nodiscard]] Status typed_accept_mounted_cursor_receipt(TypedMountedCursorAttachment<Cursor> &a,
    typename Cursor::Receipt &receipt)
{
    if (a.receipt_consumed_) return Status::already_exists("mounted cursor original receipt already consumed");
    if (auto status = typed_mounted_cursor_exact(a); !status) return typed_mounted_cursor_stop(a, status);
    const auto reads = a.cursor_.control_.suffix_reads;
    auto terminal = a.child_terminal_;
    if (reads > ~block::SessionCounter{0} - terminal.reads || reads > ~block::SessionCounter{0} - terminal.completed_reads)
        return typed_mounted_cursor_stop(a, Status::fault("mounted cursor suffix terminal overflows original counters"));
    terminal.reads += reads; terminal.completed_reads += reads;
    if (!reclaim_history::same_session(terminal, a.cursor_.progress_))
        return typed_mounted_cursor_stop(a, Status::fault("mounted cursor suffix changes more than exact successful READs"));
    if (auto status = a.cursor_.consume_receipt(a.context_, receipt); !status) return typed_mounted_cursor_stop(a, status);
    const auto actual = a.session_.report();
    if (!actual || !reclaim_history::same_session(actual.value(), terminal))
        return typed_mounted_cursor_stop(a, actual ? Status::fault("mounted cursor native endpoint changed after receipt") : actual.status());
    a.suffix_terminal_ = terminal; a.receipt_ = &receipt; a.receipt_consumed_ = true;
    // Keep the original Owner exclusion: separately authenticated renewal must
    // consume this distinct endpoint before any child destruction or rearm.
    const auto retained = typed_mounted_consumed_receipt_exact(a, receipt);
    return retained ? Status{} : typed_mounted_cursor_stop(a, retained);
}
[[nodiscard]] static bool typed_renewal_serials_exact(u64 operation, u64 seal, u64 handoff,
    u64 renewed, u64 workspace_operation, u64 workspace_seal, u64 workspace_handoff,
    u64 workspace_handoff_seal, u64 attachment_operation)
{
    return operation && operation == seal && operation == handoff && renewed < operation &&
        operation == workspace_operation && operation == workspace_seal &&
        operation == workspace_handoff && operation == workspace_handoff_seal &&
        operation == attachment_operation;
}
// The actual child endpoint remains frozen. The consumed suffix endpoint is a
// distinct authenticated input; this predicate never calls child.idle().
template <typename Cursor>
[[nodiscard]] Status typed_mounted_renewal_retirement_locked(const TypedMountedCursorAttachment<Cursor> &a,
    const typename Cursor::Receipt &receipt) const
{
    const auto &workspace = a.workspace_;
    if (!typed_workspace_anchors_exact_locked() || typed_renewal_quarantined_ || workspace.renewal_quarantined_ ||
        &a.owner_ != this || &a.session_ != &cache_.session_ || a.original_ != &a || !a.admitted_ || a.renewed_ ||
        typed_mounted_attachment_ != &a || typed_mounted_workspace_ != &workspace || !typed_mounted_pending_ ||
        typed_workspace_ != &workspace || typed_context_ || !typed_attempted_ || !typed_completed_ ||
        report_.phase != Ext4OwnedPhase::checkpointed || !report_.status ||
        !typed_renewal_serials_exact(typed_operation_serial_, typed_operation_serial_seal_, typed_handoff_serial_,
            typed_renewed_serial_, workspace.operation_serial_, workspace.operation_serial_seal_,
            workspace.handoff_serial_, workspace.handoff_serial_seal_, a.operation_serial_) ||
        !workspace.zero_.owner_retirement_idle() || !workspace.allocated_.owner_retirement_idle() ||
        !a.resumed_ || !a.receipt_consumed_ || a.receipt_ != &receipt || !receipt.minted_ || !receipt.consumed_ ||
        receipt.cursor_ != &a.cursor_ || receipt.context_ != &a.context_ || receipt.session_ != &a.session_ ||
        receipt.owner_ != this || receipt.mount_ != a.context_.mount_ || !a.serial_ || receipt.serial_ != a.serial_ ||
        a.context_.serial_ != a.serial_ || receipt.sequence_ != a.cursor_.control_.final_sequence ||
        !a.cursor_.retained(a.context_, false, true) ||
        !reclaim_history::same_session(a.child_terminal_, a.child_terminal_seal_) ||
        !reclaim_history::same_session(receipt.endpoint_, a.suffix_terminal_) ||
        !reclaim_history::same_session(a.cursor_.progress_, a.suffix_terminal_))
        return Status::fault("mounted renewal original consumed serial or retirement history changed");
    const auto selected = [&]<typename Child>(const Child &child) {
        return a.child_ == &child && child.status_ && child.control_.phase == Child::Phase::complete &&
            child.control_ == child.sealed_ && child.original_ == &a.session_ && child.original_seal_ == &a.session_ &&
            child.anchor_.exact(&a.session_) && child.completed_owner_callbacks_retired() &&
            child.completed_ownership_birth_valid(true) && child.retained(false) &&
            reclaim_history::same_session(child.progress_, a.child_terminal_) &&
            reclaim_history::same_session(child.initial_, workspace.child_initial_) &&
            reclaim_history::same_info(child.binding_, workspace.binding_) &&
            report_.completion.journal_sequence == child.reserved_.final_sequence;
    };
    if (!(workspace.allocated_route_ ? selected(workspace.allocated_) : selected(workspace.zero_)))
        return Status::fault("mounted renewal actual completed child history changed");
    auto endpoint = a.child_terminal_;
    const auto reads = a.cursor_.control_.suffix_reads;
    if (reads > ~block::SessionCounter{0} - endpoint.reads || reads > ~block::SessionCounter{0} - endpoint.completed_reads)
        return Status::fault("mounted renewal suffix counters overflow");
    endpoint.reads += reads; endpoint.completed_reads += reads;
    const auto actual = a.session_.report(); const auto binding = a.session_.observe_binding();
    if (!actual || !binding || binding.value() != workspace.session_binding_ ||
        !reclaim_history::same_session(endpoint, a.suffix_terminal_) ||
        !reclaim_history::same_session(actual.value(), endpoint) || actual.value().in_flight || actual.value().pending_writes)
        return Status::fault("mounted renewal distinct original suffix endpoint changed");
    smp::ScopedSpinLock cache_guard(cache_.lock_);
    if (auto status = cache_.typed_cache_idle(); !status) return status;
    return typed_published_cache_exact_locked(workspace);
}
template <typename Cursor>
[[nodiscard]] Status typed_renew_mounted_workspace(TypedMountedCursorAttachment<Cursor> &a,
    typename Cursor::Receipt &receipt)
{
    // Ordinary retained-work and not-yet-consumed refusals precede inspection
    // of publication snapshots. They neither reconstruct nor quarantine.
    {
        smp::ScopedSpinLock guard(lock_);
        if (typed_renewal_quarantined_) return Status::fault("typed owner renewal is permanently quarantined");
        if (!typed_workspace_anchors_exact_locked())
            return quarantine_typed_renewal_locked(Status::fault("mounted renewal permanent original anchor changed"));
        if (&a.owner_ != this || &a.session_ != &cache_.session_ || typed_mounted_attachment_ != &a)
            return a.renewed_ ? Status::already_exists("mounted renewal original permit consumed") :
                Status::bad_descriptor("mounted renewal requires its original attachment");
        if (!a.receipt_consumed_) return Status::busy("mounted renewal requires actual Receipt consumption");
        const auto cache = cache_.report();
        if (admissions_ || authority_.active || participant_count_ || transaction_.active_ ||
            startup_active_ || startup_route_opening_ || has_data_ || rename_active_ || create_active_ ||
            first_data_active_ || next_data_active_ || namespace_active_ || namespace_recovery_active_ ||
            reclaim_prepared_ || reclaim_active_ || cache.in_flight || cache.read_pins || cache.transaction_active)
            return Status::busy("mounted renewal requires drained original Owner and cache");
    }
    if (auto status = typed_mounted_consumed_receipt_exact(a, receipt); !status)
        return typed_mounted_cursor_stop(a, status);
    smp::ScopedSpinLock guard(lock_);
    if (auto status = typed_mounted_renewal_retirement_locked(a, receipt); !status)
        return quarantine_typed_renewal_locked(status);
    // Exhaustion is a quiet capacity refusal only after every authentic
    // child, suffix, cache, binding and destructor-retirement proof succeeds.
    if (typed_operation_serial_ == ~u64{0}) return Status::overflow("typed owner operation serial exhausted");
    auto &workspace = typed_attachment_->workspace_;
    const auto reconstruct = []<typename T>(T &value) { value.~T(); ::new (static_cast<void *>(&value)) T; };
    reconstruct(workspace.zero_); reconstruct(workspace.allocated_);
    reconstruct(workspace.prefix_); reconstruct(workspace.reconciliation_);
    reconstruct(workspace.prefix_snapshot_); reconstruct(workspace.reconciliation_snapshot_);
    reconstruct(workspace.published_reconciliation_snapshot_); reconstruct(workspace.published_cache_);
    workspace.checking_ = {}; workspace.data_request_ = {}; workspace.data_request_seal_ = {};
    workspace.changes_ = {}; workspace.binding_ = {}; workspace.binding_seal_ = {};
    workspace.initial_ = {}; workspace.initial_seal_ = {}; workspace.prefix_progress_ = {};
    workspace.child_initial_ = {}; workspace.child_initial_seal_ = {};
    workspace.prefix_reads_ = workspace.prefix_reservation_ = workspace.changes_count_ = 0;
    workspace.attempted_ = workspace.allocated_route_ = workspace.recovery_ = workspace.consumed_ = false;
    typed_phase(workspace, TypedOwnerPhase::ready);
    static_cast<void>(typed_rearm_drained_locked());
    typed_renewed_serial_ = a.operation_serial_; a.renewed_ = true; a.admitted_ = false;
    // Receipt, Context and the two terminal reports stay intact for audit.
    typed_mounted_pending_ = false; typed_mounted_attachment_ = nullptr; typed_mounted_workspace_ = nullptr;
    return {};
}
const void *typed_mounted_attachment_{};
const TypedOwnerWorkspace *typed_mounted_workspace_{};
bool typed_mounted_pending_{};
