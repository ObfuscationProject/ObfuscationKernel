// Private same-Owner service edge for the distinct active OKMVW001 family.
// All physical I/O still goes through the one original canonical Session.
enum class StartupRegularMovePhase : u8 { unused, preparing, staged, executing, complete, retired };
class StartupRegularMoveView;
struct StartupRegularMoveIo final
{
    StartupRegularMoveIo() = default;
    StartupRegularMoveIo(Ext4OwnedTransaction &o, const block::SessionReport &initial,
                         const block::SessionBindingObservation &observed)
        : owner(&o), cache(&o.cache_), session(&o.cache_.session_), workspace(o.startup_workspace_),
          completion(&o.regular_move_completion_), view(&o.startup_regular_move_view_),
          baseline(initial), binding(observed), domain(o.startup_epoch_),
          progress(initial), progress_seal(initial), phase(StartupRegularMovePhase::preparing),
          phase_seal(phase), active(true), active_seal(true) {}
    StartupRegularMoveIo(const StartupRegularMoveIo &) = delete;
    StartupRegularMoveIo &operator=(const StartupRegularMoveIo &) = delete;
    StartupRegularMoveIo(StartupRegularMoveIo &&) = delete;
    StartupRegularMoveIo &operator=(StartupRegularMoveIo &&) = delete;
    const StartupRegularMoveIo *const original{this};
    const Ext4OwnedTransaction *const owner{};
    const Cache *const cache{};
    const typename Registry::Session *const session{};
    const Ext4MountedStartupWorkspace *const workspace{};
    const Ext4RegularMoveCompletion *const completion{};
    const StartupRegularMoveView *const view{};
    const block::SessionReport baseline{};
    const block::SessionBindingObservation binding{};
    const u64 domain{};
    block::SessionReport progress{}, progress_seal{};
    StartupRegularMovePhase phase{StartupRegularMovePhase::unused}, phase_seal{phase};
    bool active{}, active_seal{};
};
class StartupRegularMoveView final : public driver::BlockDevice
{
  public:
    explicit StartupRegularMoveView(Ext4OwnedTransaction &owner) : owner_(owner), session_(&owner.cache_.session_) {}
    StartupRegularMoveView(const StartupRegularMoveView &) = delete;
    StartupRegularMoveView &operator=(const StartupRegularMoveView &) = delete;
    StartupRegularMoveView(StartupRegularMoveView &&) = delete;
    StartupRegularMoveView &operator=(StartupRegularMoveView &&) = delete;
    driver::BlockGeometry geometry() const override { return session_->geometry(); }
    Status read_blocks(u64 first, std::span<std::byte> bytes) override
    { return owner_.startup_regular_move_read(first, bytes); }
    Status write_blocks(u64 first, std::span<const std::byte> bytes) override
    { return owner_.startup_regular_move_write(first, bytes); }
    Status flush() override { return owner_.startup_regular_move_flush(); }
  private:
    Ext4OwnedTransaction &owner_;
    const typename Registry::Session *const session_;
};
// 12 generic binding READs + 25 observed snapshots + 25 initial bookends,
// six canonical misses, two 25-snapshot execution passes and a 25-snapshot
// terminal pass. Aliases can reduce these physical reads, never increase them.
static constexpr block::SessionCounter startup_regular_move_recovery_reads = 143;
// The original cold cursor also guards the existing strict ordinary decoder
// before startup_admit. Keep its source-derived complete terminal/root bound;
// cache hits do not consume physical credit, every real miss does.
static constexpr block::SessionCounter startup_regular_move_reads =
    startup_regular_move_recovery_reads + 11'762'118'846ULL;
static constexpr block::SessionCounter startup_regular_move_writes = 6;
static constexpr block::SessionCounter startup_regular_move_flushes = 3;
[[nodiscard]] Status startup_regular_move_retained(bool shadow = true) const
{
    const auto &io = startup_regular_move_io_;
    if (!io.active || io.active != io.active_seal || io.original != &io || io.owner != this || io.cache != &cache_ ||
        io.session != &cache_.session_ || io.workspace != startup_workspace_ || io.completion != &regular_move_completion_ ||
        io.view != &startup_regular_move_view_ || io.domain != startup_epoch_ || io.domain != publication_domain_ ||
        io.phase != io.phase_seal || io.phase == StartupRegularMovePhase::unused || io.phase == StartupRegularMovePhase::retired ||
        !exact_session(io.progress, io.progress_seal) || (shadow && !exact_session(io.progress, reclaim_io_)) ||
        !startup_active_ || startup_route_opening_ || startup_legacy_selected_ || startup_reclaim_seal_.minted ||
        startup_seal_.active || startup_namespace_seal_.active || !regular_move_active_ || !regular_move_recovery_active_ ||
        startup_report_.initial_family != Ext4StartupFamily::regular_move_active ||
        (startup_report_.phase != Ext4StartupPhase::regular_move_checkpointing && startup_report_.phase != Ext4StartupPhase::admitting))
        return Status::fault("startup regular move retained original cursor differs");
    const auto binding = cache_.session_.observe_binding();
    if (!binding) return binding.status();
    if (binding.value() != io.binding || io.baseline.identity != startup_identity_.device_identity ||
        io.baseline.registration_generation != startup_identity_.registration_generation ||
        io.baseline.session_serial != startup_identity_.session_serial)
        return Status::fault("startup regular move original Session binding differs");
    return startup_domain_guard();
}
[[nodiscard]] Status startup_regular_move_capacity(const block::SessionReport &now) const
{
    const auto &base = startup_regular_move_io_.baseline;
    const auto a = common_counts(base), b = common_counts(now);
    const std::array<block::SessionCounter, 6> limits{startup_regular_move_reads, startup_regular_move_reads,
        startup_regular_move_writes, startup_regular_move_writes, startup_regular_move_flushes, startup_regular_move_flushes};
    for (usize index = 0; index < limits.size(); ++index)
        if (b[index] < a[index] || b[index] - a[index] > limits[index])
            return Status::overflow("startup regular move original cursor allowance exhausted");
    return {};
}
[[nodiscard]] Status startup_regular_move_exact() const
{
    if (auto status = startup_regular_move_retained(); !status) return status;
    const auto actual = cache_.session_.report();
    if (!actual) return actual.status();
    if (!exact_session(actual.value(), startup_regular_move_io_.progress) ||
        actual.value().state != block::DeviceState::owned || actual.value().in_flight ||
        actual.value().first_error != StatusCode::ok || actual.value().reads != actual.value().completed_reads ||
        actual.value().writes != actual.value().completed_writes || actual.value().flushes != actual.value().completed_flushes)
        return Status::fault("startup regular move actual Session endpoint differs");
    return startup_regular_move_capacity(actual.value());
}
[[nodiscard]] Status startup_regular_move_reserve()
{
    if (startup_regular_move_io_.phase != StartupRegularMovePhase::unused || startup_regular_move_io_.active)
        return Status::already_exists("startup regular move cursor is one-shot");
    if (auto status = startup_domain_guard(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    const auto binding = cache_.session_.observe_binding(); const auto actual = cache_.session_.report();
    if (!binding || !actual) return binding ? actual.status() : binding.status();
    if (actual.value().pending_writes || actual.value().in_flight || binding.value().session != &cache_.session_ ||
        binding.value().sector_bytes != 512 || !binding.value().writable || binding.value().maximum_transfer < 1024)
        return Status::fault("startup regular move reservation lacks writable completed original Session");
    const auto fields = common_counts(actual.value());
    const std::array<block::SessionCounter, 6> limits{startup_regular_move_reads, startup_regular_move_reads,
        startup_regular_move_writes, startup_regular_move_writes, startup_regular_move_flushes, startup_regular_move_flushes};
    for (usize index = 0; index < fields.size(); ++index)
        if (!block::session_counter_fits<block::SessionCounter>(fields[index], limits[index]))
            return Status::overflow("startup regular move whole six-counter reservation exhausted");
    startup_regular_move_io_.~StartupRegularMoveIo();
    ::new (static_cast<void *>(&startup_regular_move_io_)) StartupRegularMoveIo(*this, actual.value(), binding.value());
    return startup_regular_move_exact();
}
// One immutable call-only selection. Neither public reports nor copied service
// views can authorize a Session operation or change its selected bytes/phase.
class StartupRegularMoveDispatch final
{
  public:
    StartupRegularMoveDispatch(Ext4OwnedTransaction &owner, Ext4ReclaimRouteOperation operation)
        : owner_(owner), session_(&owner.cache_.session_), cursor_(&owner.startup_regular_move_io_),
          before_(cursor_->progress), phase_(cursor_->phase), completion_phase_(owner.regular_move_completion_.report().phase),
          operation_(operation) { bound_ = anchor_.bind(*session_); }
    StartupRegularMoveDispatch(const StartupRegularMoveDispatch &) = delete;
    StartupRegularMoveDispatch &operator=(const StartupRegularMoveDispatch &) = delete;
    [[nodiscard]] Status in_flight_exact(typename Registry::Session &session) const
    {
        if (!bound_) return bound_;
        if (!anchor_.exact(&session) || &session != session_ || &owner_.cache_.session_ != session_ ||
            &owner_.startup_regular_move_io_ != cursor_ || cursor_->phase != phase_ ||
            owner_.regular_move_completion_.report().phase != completion_phase_ ||
            !exact_session(cursor_->progress, before_))
            return Status::fault("startup regular move call selection changed");
        if (auto status = owner_.startup_regular_move_retained(); !status) return status;
        auto expected = before_; expected.in_flight = true;
        if (operation_ == Ext4ReclaimRouteOperation::write) { ++expected.writes; expected.pending_writes = true; }
        else if (operation_ == Ext4ReclaimRouteOperation::flush) ++expected.flushes;
        else return Status::fault("startup regular move dispatch is not a writer operation");
        const auto actual = session.report(); if (!actual) return actual.status();
        if (!exact_session(actual.value(), expected))
            return Status::fault("startup regular move exact attempted Session prefix differs");
        smp::ScopedSpinLock guard(owner_.cache_.lock_);
        return owner_.authorized();
    }
    [[nodiscard]] Status finish(Status transport)
    {
        if (!transport) { static_cast<void>(anchor_.abort(transport)); return transport; }
        if (auto status = owner_.startup_regular_move_retained(); !status) return status;
        if (cursor_->phase != phase_ || owner_.regular_move_completion_.report().phase != completion_phase_ ||
            !exact_session(cursor_->progress, before_))
            return Status::fault("startup regular move returned call selection changed");
        auto expected = before_;
        if (operation_ == Ext4ReclaimRouteOperation::write)
        { ++expected.writes; ++expected.completed_writes; expected.pending_writes = true; }
        else { ++expected.flushes; ++expected.completed_flushes; expected.pending_writes = false; }
        const auto actual = session_->report(); if (!actual) return actual.status();
        if (!exact_session(actual.value(), expected))
            return Status::fault("startup regular move exact returned Session prefix differs");
        if (auto status = owner_.startup_regular_move_capacity(expected); !status) return status;
        owner_.reclaim_io_ = expected;
        owner_.startup_regular_move_io_.progress = owner_.startup_regular_move_io_.progress_seal = expected;
        return owner_.startup_regular_move_exact();
    }
  private:
    Ext4OwnedTransaction &owner_;
    typename Registry::Session *const session_;
    const StartupRegularMoveIo *const cursor_;
    const block::SessionReport before_;
    const StartupRegularMovePhase phase_;
    const Ext4RegularMoveCompletionPhase completion_phase_;
    const Ext4ReclaimRouteOperation operation_;
    reclaim_history::SessionAnchor<typename Registry::Session> anchor_{};
    Status bound_{};
};
template <typename Hook> requires block::ReadDispatchGuard<Hook, typename Registry::Session>
[[nodiscard]] Status startup_regular_move_read(u64 first, std::span<std::byte> bytes, Hook &hook)
{
    if (auto status = startup_regular_move_exact(); !status) return status;
    if (first % 2 || first >= 65536 || bytes.size() != 1024 || reclaim_io_.pending_writes)
        return Status::invalid_argument("startup regular move read outside bounded completed route");
    auto projected = reclaim_io_; ++projected.reads; ++projected.completed_reads;
    if (auto status = startup_regular_move_capacity(projected); !status) return status;
    const auto *const cursor = &startup_regular_move_io_;
    const auto phase = cursor->phase;
    const auto completion_phase = regular_move_completion_.report().phase;
    auto authority = [this, cursor, phase, completion_phase]() -> Status {
        if (&startup_regular_move_io_ != cursor || cursor->phase != phase ||
            regular_move_completion_.report().phase != completion_phase)
            return Status::fault("startup regular move read selection changed");
        return startup_regular_move_retained();
    };
    if (auto status = guarded_ext4_direct_read(cache_.session_, reclaim_io_, authority, first, bytes, hook); !status) return status;
    // The dispatcher physically verified this one read before shadow adoption.
    startup_regular_move_io_.progress = startup_regular_move_io_.progress_seal = reclaim_io_;
    return startup_regular_move_exact();
}
[[nodiscard]] Status startup_regular_move_read(u64 first, std::span<std::byte> bytes)
{
    Ext4DirectReadHooks hook;
    return startup_regular_move_read(first, bytes, hook);
}
[[nodiscard]] Status startup_regular_move_write(u64 first, std::span<const std::byte> bytes)
{
    if (auto status = startup_regular_move_exact(); !status) return status;
    if (startup_regular_move_io_.phase != StartupRegularMovePhase::executing || first % 2 ||
        first >= 65536 || bytes.size() != 1024 || !authority_.active)
        return Status::denied("startup regular move write lacks original executing authority");
    const auto &binding = regular_move_completion_.binding_;
    const Block *selected{};
    const auto phase = regular_move_completion_.report().phase;
    if (phase == Ext4RegularMoveCompletionPhase::homes)
        for (usize index = 0; index < binding.validation_.count; ++index)
            if (first == u64{binding.scratch_.canonical.targets[index]} * 2 && !binding.validation_.home_after[index])
                selected = &binding.scratch_.canonical.after[index];
    if (phase == Ext4RegularMoveCompletionPhase::journal_head && first == u64{binding.journal_first_} * 2)
        selected = &binding.empty_head_;
    if (!selected) return Status::denied("startup regular move write has no exact completion image");
    for (usize byte = 0; byte < 1024; ++byte)
        if (bytes[byte] != std::byte((*selected)[byte])) return Status::fault("startup regular move selected write bytes differ");
    auto projected = reclaim_io_; ++projected.writes; ++projected.completed_writes; projected.pending_writes = true;
    if (auto status = startup_regular_move_capacity(projected); !status) return status;
    StartupRegularMoveDispatch dispatch(*this, Ext4ReclaimRouteOperation::write);
    const auto status = cache_.session_.write_blocks_guarded(first, bytes, dispatch);
    const auto finished = dispatch.finish(status);
    if (!finished) static_cast<void>(cache_.session_.abort(finished));
    return finished;
}
[[nodiscard]] Status startup_regular_move_flush()
{
    if (auto status = startup_regular_move_exact(); !status) return status;
    const auto phase = regular_move_completion_.report().phase;
    if (startup_regular_move_io_.phase != StartupRegularMovePhase::executing || !authority_.active ||
        (phase != Ext4RegularMoveCompletionPhase::preflight_flush && phase != Ext4RegularMoveCompletionPhase::home_flush &&
         phase != Ext4RegularMoveCompletionPhase::journal_flush))
        return Status::denied("startup regular move flush lacks exact original completion phase");
    auto projected = reclaim_io_; ++projected.flushes; ++projected.completed_flushes; projected.pending_writes = false;
    if (auto status = startup_regular_move_capacity(projected); !status) return status;
    StartupRegularMoveDispatch dispatch(*this, Ext4ReclaimRouteOperation::flush);
    const auto status = cache_.session_.flush_guarded(dispatch);
    const auto finished = dispatch.finish(status);
    if (!finished) static_cast<void>(cache_.session_.abort(finished));
    return finished;
}
[[nodiscard]] Status startup_regular_move_cache_credit() const
{
    if (auto status = startup_regular_move_exact(); !status) return status;
    if (startup_regular_move_io_.phase != StartupRegularMovePhase::preparing &&
        startup_regular_move_io_.phase != StartupRegularMovePhase::complete)
        return Status::busy("startup regular move cache read is outside preparation or strict admission");
    auto projected = reclaim_io_; ++projected.reads; ++projected.completed_reads;
    return startup_regular_move_capacity(projected);
}
[[nodiscard]] Status startup_regular_move_adopt_cache_read()
{
    if (auto status = startup_regular_move_retained(false); !status) return status;
    const auto actual = cache_.session_.report(); if (!actual) return actual.status();
    auto expected = startup_regular_move_io_.progress;
    if (!exact_session(actual.value(), expected)) { ++expected.reads; ++expected.completed_reads; }
    if (!exact_session(actual.value(), expected) || !exact_session(actual.value(), reclaim_io_))
        return Status::fault("startup regular move canonical pin endpoint differs");
    if (auto status = startup_regular_move_capacity(expected); !status) return status;
    startup_regular_move_io_.progress = startup_regular_move_io_.progress_seal = expected;
    return startup_regular_move_exact();
}
[[nodiscard]] Status startup_regular_move_checkpoint()
{
    startup_report_.phase = Ext4StartupPhase::regular_move_checkpointing;
    regular_move_active_ = regular_move_recovery_active_ = true;
    if (auto status = startup_regular_move_reserve(); !status) return status;
    if (auto status = regular_move_completion_.prepare(startup_regular_move_view_, bounds_); !status) return status;
    const auto &binding = regular_move_completion_.binding_;
    if (binding.super_ != startup_workspace_->locator_super_ || binding.descriptors_ != startup_workspace_->locator_descriptors_ ||
        binding.journal_table_ != startup_workspace_->locator_table_ || binding.journal_[0] != startup_workspace_->locator_head_ ||
        binding.journal_[1] != startup_workspace_->locator_descriptor_ ||
        regular_move_completion_.report().active_sequence != startup_report_.initial_sequence)
        return Status::fault("startup regular move complete proof differs from original five-read classifier");
    if (auto status = regular_move_completion_.logical_images(blocks_, sealed_, participant_count_); !status) return status;
    if (auto status = stage_prepared(true); !status) return status;
    startup_regular_move_io_.phase = startup_regular_move_io_.phase_seal = StartupRegularMovePhase::executing;
    if (auto status = checkpoint_original(true); !status) return status;
    if (auto status = startup_regular_move_exact(); !status) return status;
    startup_regular_move_io_.phase = startup_regular_move_io_.phase_seal = StartupRegularMovePhase::complete;
    startup_report_.regular_move_checkpoints = 1; startup_report_.regular_move_result_validated = true;
    startup_expected_empty_head_ = binding.empty_head_;
    startup_report_.verified_publications = 1; startup_report_.final_sequence = regular_move_completion_.report().empty_sequence;
    startup_report_.observed = reclaim_io_; startup_report_.phase = Ext4StartupPhase::admitting;
    return {};
}
[[nodiscard]] Status startup_regular_move_terminal()
{
    if (auto status = startup_regular_move_exact(); !status) return status;
    if (startup_regular_move_io_.phase != StartupRegularMovePhase::complete || startup_report_.regular_move_checkpoints != 1 ||
        !startup_report_.regular_move_result_validated || !regular_move_completion_.report().complete())
        return Status::fault("startup regular move lacks original complete terminal history");
    if (auto status = regular_move_completion_.verify_terminal(); !status) return status;
    Status checked;
    { smp::ScopedSpinLock guard(cache_.lock_); checked = regular_move_terminal_cache_exact(); }
    if (!checked) return checked;
    if (auto status = startup_regular_move_exact(); !status) return status;
    startup_regular_move_io_.phase = startup_regular_move_io_.phase_seal = StartupRegularMovePhase::retired;
    startup_regular_move_io_.active = startup_regular_move_io_.active_seal = false;
    const auto retained = reclaim_io_;
    if (auto status = startup_rearm(); !status) return status;
    reclaim_io_ = retained;
    return reclaim_session_exact();
}
StartupRegularMoveView startup_regular_move_view_{*this};
StartupRegularMoveIo startup_regular_move_io_{};
