// Included privately in Ext4OwnedTransaction; no second Owner or Session.
struct StartupColdSeal final
{
    StartupColdSeal() = default;
    StartupColdSeal(const StartupColdSeal &) = delete;
    StartupColdSeal &operator=(const StartupColdSeal &) = delete;
    const Ext4OwnedTransaction *owner{}, *canonical_owner{};
    const Cache *cache{};
    const typename Cache::Registry::Session *session{};
    const Ext4MountedStartupWorkspace *workspace{};
    const Ext4OrphanChainCaptureStorage *storage{};
    u64 identity{}, registration{}, session_serial{}, first_sector{}, sectors{}, domain{}, transaction{};
    std::array<usize, participant_capacity> slots{};
    std::array<u64, participant_capacity> generations{};
    u32 head{}, generation{}, sequence{}, next_head{};
    usize chain_members{};
    std::array<u8, 16> uuid{};
    bool active{};
};
class StartupReadView final : public driver::BlockDevice
{
  public:
    explicit StartupReadView(Ext4OwnedTransaction &owner) : owner_(owner) {}
    const Ext4OwnedTransaction *owner_identity() const { return &owner_; }
    driver::BlockGeometry geometry() const override { return owner_.cache_.session_.geometry(); }
    Status read_blocks(u64 first, std::span<std::byte> bytes) override
    { return owner_.startup_read_blocks(first, bytes); }
    Status write_blocks(u64, std::span<const std::byte>) override
    { return Status::denied("startup read view cannot write"); }
    Status flush() override { return Status::denied("startup read view cannot flush"); }
  private:
    Ext4OwnedTransaction &owner_;
};
class StartupOrdinaryView final : public driver::BlockDevice
{
  public:
    explicit StartupOrdinaryView(Ext4OwnedTransaction &owner) : owner_(owner) {}
    const Ext4OwnedTransaction *owner_identity() const { return &owner_; }
    driver::BlockGeometry geometry() const override { return owner_.cache_.session_.geometry(); }
    Status read_blocks(u64 first, std::span<std::byte> bytes) override
    { return owner_.startup_read_blocks(first, bytes); }
    Status write_blocks(u64 first, std::span<const std::byte> bytes) override
    { return owner_.startup_ordinary_write(first, bytes); }
    Status flush() override { return owner_.startup_ordinary_flush(); }
  private:
    Ext4OwnedTransaction &owner_;
};
// Call-only static dispatch authority. Session retains neither this guard nor
// its Owner reference after the physical request has drained.
class StartupOrdinaryDispatchGuard final
{
  public:
    StartupOrdinaryDispatchGuard(Ext4OwnedTransaction &owner, bool write)
        : owner_(owner), original_(&owner.cache_.session_), workspace_(owner.startup_workspace_),
          before_(owner.reclaim_io_), family_(owner.startup_report_.initial_family),
          legacy_(owner.startup_legacy_selected_), actions_(owner.startup_legacy_actions_), write_(write)
    { bound_ = anchor_.bind(*original_); }
    StartupOrdinaryDispatchGuard(const StartupOrdinaryDispatchGuard &) = delete;
    StartupOrdinaryDispatchGuard &operator=(const StartupOrdinaryDispatchGuard &) = delete;
    [[nodiscard]] Status in_flight_exact(typename Registry::Session &session) const
    {
        if (!bound_) return bound_;
        if (!anchor_.exact(&session) || &session != original_ || &owner_.cache_.session_ != original_ ||
            owner_.startup_workspace_ != workspace_ || owner_.startup_report_.initial_family != family_ ||
            owner_.startup_legacy_selected_ != legacy_ || owner_.startup_legacy_actions_ != actions_ ||
            owner_.startup_report_.phase != Ext4StartupPhase::admitting || !owner_.startup_ordinary_selected() ||
            owner_.startup_reclaim_seal_.minted || owner_.startup_seal_.active ||
            !exact_session(owner_.reclaim_io_, before_))
            return Status::fault("startup ordinary fixed dispatch authority differs");
        auto expected = before_;
        expected.in_flight = true;
        if (write_) { ++expected.writes; expected.pending_writes = true; }
        else ++expected.flushes;
        const auto actual = session.report();
        if (!actual) return actual.status();
        if (!exact_session(actual.value(), expected))
            return Status::fault("startup ordinary exact attempted Session prefix differs");
        return owner_.startup_domain_guard();
    }
  private:
    Ext4OwnedTransaction &owner_;
    typename Registry::Session *const original_;
    const Ext4MountedStartupWorkspace *const workspace_;
    const block::SessionReport before_;
    const Ext4StartupFamily family_;
    const bool legacy_;
    const usize actions_;
    const bool write_;
    reclaim_history::SessionAnchor<typename Registry::Session> anchor_{};
    Status bound_{};
};
[[nodiscard]] Status startup_ordinary_write(u64 first, std::span<const std::byte> bytes)
{
    if (startup_reclaim_seal_.minted || !startup_ordinary_selected() ||
        startup_report_.phase != Ext4StartupPhase::admitting || startup_seal_.active)
        return Status::denied("startup ordinary writer has no selected ordinary route");
    if (auto status = startup_domain_guard(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    if (first % 2 || bytes.size() != 1024 || first >= 65536 || reclaim_io_.writes == ~block::SessionCounter{0})
        return Status::overflow("startup ordinary write bounds or counters");
    StartupOrdinaryDispatchGuard dispatch(*this, true);
    if (auto status = cache_.session_.write_blocks_guarded(first, bytes, dispatch); !status) return status;
    ++reclaim_io_.writes; ++reclaim_io_.completed_writes; reclaim_io_.pending_writes = true;
    if (auto status = reclaim_session_exact(); !status) return status;
    return startup_domain_guard();
}
[[nodiscard]] Status startup_ordinary_flush()
{
    if (startup_reclaim_seal_.minted || !startup_ordinary_selected() ||
        startup_report_.phase != Ext4StartupPhase::admitting || startup_seal_.active)
        return Status::denied("startup ordinary flush has no selected ordinary route");
    if (auto status = startup_domain_guard(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    if (reclaim_io_.flushes == ~block::SessionCounter{0}) return Status::overflow("startup ordinary flush counter exhausted");
    StartupOrdinaryDispatchGuard dispatch(*this, false);
    if (auto status = cache_.session_.flush_guarded(dispatch); !status) return status;
    ++reclaim_io_.flushes; ++reclaim_io_.completed_flushes; reclaim_io_.pending_writes = false;
    if (auto status = reclaim_session_exact(); !status) return status;
    return startup_domain_guard();
}
[[nodiscard]] bool startup_domain_active() const
{ smp::ScopedSpinLock guard(lock_); return startup_active_; }
[[nodiscard]] Ext4StartupRecoveryReport startup_report() const
{ smp::ScopedSpinLock guard(lock_); return startup_report_; }
[[nodiscard]] bool startup_ordinary_selected() const
{
    return startup_legacy_selected_ ? startup_report_.initial_family == Ext4StartupFamily::legacy_active :
                                      startup_report_.initial_family == Ext4StartupFamily::ordinary;
}
[[nodiscard]] Status startup_domain_guard() const
{
    {
        smp::ScopedSpinLock guard(lock_);
        if (!startup_active_ || !startup_mount_ || !startup_workspace_ || !startup_reader_ || !startup_ordinary_reader_ ||
            startup_reader_->owner_identity() != this || startup_ordinary_reader_->owner_identity() != this || startup_epoch_ != 1 ||
            publication_domain_ != startup_epoch_ || admissions_ || report_.phase == Ext4OwnedPhase::failed)
            return Status::fault("startup continuing Owner opening domain differs");
    }
    {
        smp::ScopedSpinLock guard(startup_workspace_->lock_);
        if (startup_workspace_->claimant_ != startup_mount_ || startup_workspace_->owner_ != this)
            return Status::fault("startup permanent workspace claim differs");
        if ((startup_report_.initial_family == Ext4StartupFamily::legacy_active) != startup_legacy_selected_ ||
            (startup_legacy_selected_ && (!startup_legacy_actions_ || startup_legacy_actions_ > 8 ||
             (startup_legacy_actions_ > 6 &&
                (!Ext4DirectoryCreateWitnessCodec::present(startup_workspace_->legacy_descriptor_seal_) ||
                 startup_workspace_->legacy_descriptor_seal_[172] != startup_legacy_actions_)) ||
             startup_workspace_->locator_head_ != startup_workspace_->legacy_head_seal_ ||
             startup_workspace_->locator_descriptor_ != startup_workspace_->legacy_descriptor_seal_)))
            return Status::fault("startup original legacy selection or locator differs");
    }
    if constexpr (BufferCapacity == 9)
    {
        if (auto status = startup_mount_->startup_opening(this, startup_epoch_); !status) return status;
    }
    else return Status::unsupported("startup requires the mounted canonical cache profile");
    const auto now = cache_.report();
    if (now.state != MetadataOwnerState::ready || now.device_identity != startup_identity_.device_identity ||
        now.registration_generation != startup_identity_.registration_generation ||
        now.session_serial != startup_identity_.session_serial || now.first_sector != startup_identity_.first_sector ||
        now.sectors != startup_identity_.sectors || now.blocks != startup_identity_.blocks)
        return Status::fault("startup original canonical cache identity differs");
    if (startup_namespace_capture_.namespace_sealed_ || startup_workspace_->namespace_result_seal_.ready ||
        startup_workspace_->namespace_result_seal_.terminal)
    {
        if (startup_namespace_capture_.namespace_seal_ != &startup_workspace_->namespace_result_seal_ ||
            startup_namespace_capture_.storage_ != &startup_workspace_->namespace_result_ ||
            startup_namespace_capture_.scratch_ != &startup_workspace_->namespace_preparation_ ||
            startup_namespace_capture_.namespace_history_ != &startup_workspace_->namespace_history_ ||
            startup_namespace_capture_.session_ != &cache_.session_)
            return Status::fault("startup prospective retained capture identity differs");
        if (auto status = startup_namespace_capture_.namespace_storage_exact(); !status) return status;
        if (auto status = startup_workspace_->namespace_history_.stable(); !status) return status;
    }
    return common_retained_exact();
}
template <typename Hook> requires block::ReadDispatchGuard<Hook, typename Registry::Session>
[[nodiscard]] Status startup_read_blocks(u64 first, std::span<std::byte> bytes, Hook &hook)
{
    if (startup_regular_move_io_.active)
    {
        if (startup_regular_move_io_.phase != StartupRegularMovePhase::complete ||
            startup_report_.phase != Ext4StartupPhase::admitting)
            return Status::denied("active regular move reads require its original private cursor service");
        return startup_regular_move_read(first, bytes, hook);
    }
    if (startup_reclaim_seal_.minted)
        return startup_reclaim_read(first, bytes);
    if (auto status = startup_domain_guard(); !status) return status;
    if (startup_seal_.active)
        if (auto status = startup_guard(); !status) return status;
    if (startup_namespace_seal_.active)
        if (auto status = startup_namespace_guard(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    if ((reclaim_io_.pending_writes && !startup_legacy_selected_) || reclaim_io_.reads == ~block::SessionCounter{0} || first % 2 ||
        bytes.size() != 1024 || first >= 65536 || bytes.size() / 512 > 65536 - first)
        return Status::overflow("startup readonly request bounds or counters");
    const bool route_opening = startup_route_opening_;
    const bool startup_active = startup_seal_.active, namespace_active = startup_namespace_seal_.active;
    const bool legacy_selected = startup_legacy_selected_;
    const usize legacy_actions = startup_legacy_actions_;
    const auto *const original_session = &cache_.session_;
    const auto *const original_workspace = startup_workspace_;
    const auto *const original_mount = startup_mount_;
    const auto original_family = startup_report_.initial_family;
    const bool reclaim_minted = startup_reclaim_seal_.minted;
    auto authority = [this, original_session, original_workspace, original_mount, original_family,
                      reclaim_minted, route_opening, startup_active, namespace_active,
                      legacy_selected, legacy_actions]() -> Status
    {
        // Compare the selected originals before domain validation follows any
        // mutable workspace or mount attachment. This is the non-reclaim route.
        if (&cache_.session_ != original_session || startup_workspace_ != original_workspace ||
            startup_mount_ != original_mount || startup_report_.initial_family != original_family ||
            reclaim_minted || startup_reclaim_seal_.minted != reclaim_minted ||
            startup_route_opening_ != route_opening || startup_seal_.active != startup_active ||
            startup_namespace_seal_.active != namespace_active || startup_legacy_selected_ != legacy_selected ||
            startup_legacy_actions_ != legacy_actions)
            return Status::fault("startup direct read callback changed selected attachment");
        if (auto status = startup_domain_guard(); !status) return status;
        if (startup_active)
            if (auto status = startup_guard(); !status) return status;
        if (namespace_active)
            if (auto status = startup_namespace_guard(); !status) return status;
        return {};
    };
    if (route_opening)
    {
        if (auto status = block::guarded_read_blocks(cache_.session_, startup_route_budget_, first, bytes); !status)
            return status;
        if (auto status = authority(); !status)
        { static_cast<void>(cache_.session_.abort(status)); return status; }
        ++reclaim_io_.reads; ++reclaim_io_.completed_reads;
    }
    else if (legacy_selected)
    {
        // Legacy completion reads immutable journal payloads between completed
        // home writes and their FLUSH. Preserve that exact pending-write bit;
        // all attempt/completion counters and the selected Owner remain sealed.
        Ext4DirectReadScope<typename Registry::Session, decltype(authority), true>
            scope(cache_.session_, reclaim_io_, authority, first, bytes.size());
        if (auto status = scope.prepare(); !status) return status;
        Ext4DirectReadDispatch<decltype(scope), Hook, typename Registry::Session> dispatch{scope, hook};
        if (auto status = block::guarded_read_blocks(cache_.session_, dispatch, first, bytes); !status) return status;
        if (auto status = scope.adopt(cache_.session_); !status) return status;
    }
    else if (auto status = guarded_ext4_direct_read(cache_.session_, reclaim_io_, authority, first, bytes, hook); !status)
        return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    return authority();
}
[[nodiscard]] Status startup_read_blocks(u64 first, std::span<std::byte> bytes)
{ Ext4DirectReadHooks hook; return startup_read_blocks(first, bytes, hook); }
[[nodiscard]] Status startup_read(u32 physical, Block &bytes)
{ return startup_read_blocks(u64{physical} * 2, std::as_writable_bytes(std::span{bytes})); }
[[nodiscard]] Status startup_close_opening_route()
{
    if (!startup_route_opening_)
        return Status::fault("startup opening route is not attached");
    if (auto status = reclaim_session_exact(); !status) return status;
    if (auto status = startup_route_budget_.close(cache_.session_); !status) return status;
    startup_route_opening_ = false;
    return {};
}
// Inspect actual canonical slots under the Owner/cache lock order. Aggregate
// reports are observational copies and cannot prove that every slot is drained.
[[nodiscard]] Status startup_handoff_cache_quiescent() const
{
    smp::ScopedSpinLock owner_guard(lock_);
    smp::ScopedSpinLock cache_guard(cache_.lock_);
    if (authority_.active || admissions_ || transaction_.active_ || participant_count_ ||
        cache_.report_.state != MetadataOwnerState::ready || cache_.report_.in_flight ||
        cache_.report_.read_pins || cache_.report_.transaction_active || cache_.transaction_)
        return Status::busy("startup handoff canonical operation remains active");
    for (const auto &pin : pins_)
        if (pin.active_) return Status::busy("startup handoff retains a participant pin");
    for (const auto &entry : cache_.entries_)
    {
        if (entry.report.read_pins || entry.report.transaction_serial || entry.report.staged_dirty ||
            entry.report.state == MetadataBufferState::loading)
            return Status::busy("startup handoff canonical slot retains work");
        if (entry.report.state != MetadataBufferState::unused && entry.report.state != MetadataBufferState::ready &&
            entry.report.state != MetadataBufferState::exhausted)
            return Status::fault("startup handoff canonical slot is not drained");
    }
    return {};
}
// Unused until a separately qualified active-reclaim Owner attachment exists.
// Retain the same budget and Session; reports and prospective counts are not
// authority. This helper admits neither a transaction nor a writer phase.
[[nodiscard]] Status startup_handoff_opening_route()
{
    const auto reject = [this](Status status) -> Status {
        static_cast<void>(cache_.session_.abort(status));
        return status;
    };
    if (auto status = startup_domain_guard(); !status) return reject(status);
    if (auto status = reclaim_session_exact(); !status) return reject(status);
    if (auto status = startup_reclaim_composed_.retained_exact(); !status) return reject(status);
    const auto &history = startup_reclaim_history_;
    const auto &composed = startup_reclaim_composed_;
    const auto cache = cache_.report();
    if (!startup_route_opening_ || startup_seal_.active || startup_namespace_seal_.active ||
        authority_.active || admissions_ || transaction_.active_ || participant_count_ ||
        cache.in_flight || cache.read_pins || cache.transaction_active || cache_.transaction_ ||
        reclaim_io_.in_flight || reclaim_io_.pending_writes || history.reading_ || composed.reading_ ||
        !composed.complete_ || composed.ordinary_ || !composed.endpoint_ ||
        composed.origin_ != &history || composed.storage_ != &startup_workspace_->reclaim_composed_ ||
        composed.original_session_ != &cache_.session_ || history.original_session_ != &cache_.session_ ||
        history.binding_ != &startup_reclaim_binding_ || history.domain_.owner != this ||
        history.domain_.cache != &cache_ || history.domain_.workspace != startup_workspace_ ||
        history.domain_.mount != startup_mount_ || history.domain_.epoch != startup_epoch_ ||
        history.domain_.generation != startup_generation_)
        return reject(Status::fault("startup active reclaim quiescent original handoff differs"));
    if (auto status = startup_handoff_cache_quiescent(); !status) return reject(status);
    if (auto status = startup_route_budget_.quiescent_handoff(cache_.session_, this); !status) return status;
    startup_route_opening_ = false;
    return {};
}
// Deferred begin runs outside the cache lock; dispatch borrows the same
// original Owner, Session, loading slot and Pin for this synchronous miss.
class StartupPinReadGuard final
{
  public:
    static constexpr bool defer_cache_begin = true;
    StartupPinReadGuard(Ext4OwnedTransaction &owner, u64 block, Pin &pin)
        : owner_(owner), original_(&owner.cache_.session_), workspace_(owner.startup_workspace_),
          mount_(owner.startup_mount_), pin_(&pin), block_(block), before_(owner.reclaim_io_),
          family_(owner.startup_report_.initial_family), legacy_(owner.startup_legacy_selected_),
          actions_(owner.startup_legacy_actions_), cold_(owner.startup_seal_.active),
          namespace_(owner.startup_namespace_seal_.active), reclaim_minted_(owner.startup_reclaim_seal_.minted),
          regular_move_(owner.startup_regular_move_io_.active)
    { bound_ = anchor_.bind(*original_); }
    ~StartupPinReadGuard()
    {
        if (pending_ || pending_seal_)
            static_cast<void>(anchor_.abort(Status::interrupted("startup canonical miss ended before finish")));
    }
    StartupPinReadGuard(const StartupPinReadGuard &) = delete;
    StartupPinReadGuard &operator=(const StartupPinReadGuard &) = delete;
    StartupPinReadGuard(StartupPinReadGuard &&) = delete;
    StartupPinReadGuard &operator=(StartupPinReadGuard &&) = delete;
    [[nodiscard]] Status begin_read(typename Registry::Session &, u64, usize)
    { return Status::fault("startup canonical miss requires deferred cache selection"); }
    [[nodiscard]] Status begin_cache_read(typename Registry::Session &session, u64 first, usize bytes,
                                          usize slot, u64 generation)
    {
        if (pending_ || pending_seal_ || slot >= BufferCapacity || !generation ||
            block_ >= 32768 || first != block_ * 2 || bytes != 1024 || pin_->owner_)
            return Status::fault("startup canonical miss route or transfer differs");
        if (auto status = retained_exact(session); !status) return status;
        if (auto status = owner_.reclaim_session_exact(); !status) return status;
        if (regular_move_)
            if (auto status = owner_.startup_regular_move_cache_credit(); !status) return status;
        if (before_.pending_writes || before_.reads == ~block::SessionCounter{0} || before_.completed_reads == ~block::SessionCounter{0})
            return Status::overflow("startup canonical miss lacks read credit");
        slot_ = slot_seal_ = slot; generation_ = generation_seal_ = generation;
        pending_ = pending_seal_ = true;
        return {};
    }
    [[nodiscard]] Status in_flight_exact(typename Registry::Session &session) const
    {
        if (!pending_ || !pending_seal_)
            return Status::fault("startup canonical miss has no pending dispatch");
        if (auto status = retained_exact(session); !status) return status;
        if (auto status = loading_exact(); !status) return status;
        auto expected = before_; ++expected.reads; expected.in_flight = true;
        const auto actual = session.report();
        if (!actual) return actual.status();
        return exact_session(actual.value(), expected) ? Status{} :
            Status::fault("startup canonical miss exact attempted Session prefix differs");
    }
    [[nodiscard]] Status finish_read(typename Registry::Session &session, Status transport)
    {
        // Transport refusal remains the first error even after authority drifts.
        if (!transport) { pending_ = pending_seal_ = false; return transport; }
        if (!pending_ || !pending_seal_)
            return Status::fault("startup canonical miss finish lacks pending dispatch");
        if (auto status = retained_exact(session); !status) return status;
        if (auto status = loading_exact(); !status) return status;
        const auto actual = session.report();
        if (!actual) return actual.status();
        auto expected = before_; ++expected.reads; ++expected.completed_reads;
        if (!exact_session(actual.value(), expected))
            return Status::fault("startup canonical miss exact returned Session prefix differs");
        owner_.reclaim_io_ = expected; pending_ = pending_seal_ = false;
        return {};
    }
  private:
    [[nodiscard]] Status retained_exact(typename Registry::Session &session) const
    {
        if (!bound_) return bound_;
        if (!anchor_.exact(&session) || &session != original_ || &owner_.cache_.session_ != original_ ||
            owner_.startup_workspace_ != workspace_ || owner_.startup_mount_ != mount_ ||
            reclaim_minted_ || owner_.startup_reclaim_seal_.minted != reclaim_minted_ ||
            ((regular_move_ && owner_.startup_report_.phase != Ext4StartupPhase::regular_move_checkpointing &&
              owner_.startup_report_.phase != Ext4StartupPhase::admitting) ||
             (!regular_move_ && owner_.startup_report_.phase != Ext4StartupPhase::admitting)) || owner_.startup_route_opening_ ||
            owner_.startup_regular_move_io_.active != regular_move_ ||
            owner_.startup_report_.initial_family != family_ || owner_.startup_legacy_selected_ != legacy_ ||
            owner_.startup_legacy_actions_ != actions_ || owner_.startup_seal_.active != cold_ ||
            owner_.startup_namespace_seal_.active != namespace_ || !exact_session(owner_.reclaim_io_, before_))
            return Status::fault("startup canonical miss fixed retained authority differs");
        if (auto status = owner_.startup_domain_guard(); !status) return status;
        if (regular_move_)
            if (auto status = owner_.startup_regular_move_retained(); !status) return status;
        if (cold_)
            if (auto status = owner_.startup_guard(); !status) return status;
        if (namespace_)
            if (auto status = owner_.startup_namespace_guard(); !status) return status;
        return {};
    }
    [[nodiscard]] Status loading_exact() const
    {
        if (slot_ != slot_seal_ || slot_ >= BufferCapacity || !generation_ || generation_ != generation_seal_)
            return Status::fault("startup canonical miss paused slot selection differs");
        smp::ScopedSpinLock guard(owner_.cache_.lock_);
        const auto &entry = owner_.cache_.entries_[slot_].report;
        if (!owner_.cache_.report_.in_flight || entry.state != MetadataBufferState::loading ||
            entry.block != block_ || entry.generation != generation_ || entry.read_pins != 1 ||
            entry.transaction_serial || entry.staged_dirty || pin_->owner_ != &owner_.cache_ ||
            pin_->slot_ != slot_ || pin_->generation_ != generation_ || pin_->active_)
            return Status::fault("startup canonical miss original loading slot or Pin differs");
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
    const bool cold_, namespace_, reclaim_minted_, regular_move_;
    reclaim_history::SessionAnchor<typename Registry::Session> anchor_{};
    Status bound_{};
    usize slot_{BufferCapacity}, slot_seal_{BufferCapacity};
    u64 generation_{}, generation_seal_{};
    bool pending_{}, pending_seal_{};
};
[[nodiscard]] Status startup_pin(u64 block, Pin &pin)
{
    if (startup_report_.phase != Ext4StartupPhase::admitting)
        return Status::busy("startup canonical read precedes ordinary admission");
    if (auto status = startup_domain_guard(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    if (startup_reclaim_seal_.minted)
    {
        if (auto status = startup_reclaim_guard(); !status) return status;
        if (auto status = startup_handoff_cache_quiescent(); !status) return status;
        if (auto status = cache_.pin(block, pin, startup_reclaim_ledger_); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        return startup_reclaim_guard();
    }
    StartupPinReadGuard guard(*this, block, pin);
    if (auto status = cache_.pin(block, pin, guard); !status) return status;
    if (startup_regular_move_io_.active)
        if (auto status = startup_regular_move_adopt_cache_read(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    return startup_domain_guard();
}
void startup_failed(Status status)
{
    startup_seal_.active = false;
    const auto actual = cache_.session_.report();
    smp::ScopedSpinLock guard(lock_);
    startup_report_.status = status;
    if (startup_report_.phase != Ext4StartupPhase::blocked) startup_report_.phase = Ext4StartupPhase::failed;
    if (actual) startup_report_.observed = actual.value();
}
[[nodiscard]] Status startup_stop(Status status, bool blocked = false)
{
    if (blocked) { smp::ScopedSpinLock guard(lock_); startup_report_.phase = Ext4StartupPhase::blocked; }
    startup_failed(status); return fail(status);
}
#include "ok/fs/ext4_mounted_startup_locator.inc"
[[nodiscard]] Status startup_classify()
{
    if (auto status = startup_locator(); !status) return status;
    auto &w = *startup_workspace_;
    if (auto status = startup_read(startup_journal_first_, w.locator_head_); !status) return status;
    const auto codec = jbd2::Codec::open(w.locator_head_, startup_journal_blocks_, startup_locator_uuid_,
        jbd2::DescriptorUuidPolicy::linux_internal_zero);
    if (!codec) return codec.status();
    const auto &head = codec.value().superblock();
    if (head.block_size != 1024 || head.first_block != 1 || head.max_blocks != startup_journal_blocks_)
        return Status::unsupported("startup journal locator profile");
    startup_report_.initial_head = ext4_checked::le32(w.locator_super_, 232);
    startup_report_.initial_sequence = head.sequence;
    if (!head.start_block)
    {
        startup_report_.initial_family = startup_report_.initial_head ?
            Ext4StartupFamily::empty_orphan : Ext4StartupFamily::ordinary;
        if (auto status = select_common_endpoint(startup_report_.initial_head ? Ext4MountedCommonFamily::empty_orphan :
                Ext4MountedCommonFamily::ordinary, head.sequence, 4); !status) return status;
        return startup_close_opening_route();
    }
    if (head.start_block != 1) return Status::unsupported("startup active ring is unsupported");
    if (auto status = startup_read(startup_journal_first_ + 1, w.locator_descriptor_); !status) return status;
    if (Ext4RegularMoveWitnessCodec::present(w.locator_descriptor_))
    {
        if (!head.sequence || head.sequence > 0xfffffffdU)
            return Status::overflow("startup regular move active sequence has no cleanup headroom");
        startup_report_.initial_family = Ext4StartupFamily::regular_move_active;
        if (auto status = select_common_endpoint(Ext4MountedCommonFamily::regular_move, head.sequence, 5); !status) return status;
        if (auto status = startup_close_opening_route(); !status) return status;
        return startup_regular_move_checkpoint();
    }
    if (Ext4OrphanReclaimWitnessCodec::present(w.locator_descriptor_))
    {
        startup_report_.initial_family = Ext4StartupFamily::reclaim_active;
        if (auto status = select_common_endpoint(Ext4MountedCommonFamily::reclaim, head.sequence, 5); !status) return status;
        const auto before = reclaim_io_;
        if (auto status = startup_reclaim_binding_.inspect(cache_.session_, w.active_reclaim_, w.verification_, startup_route_budget_); !status)
            return status;
        const auto &proof = startup_reclaim_binding_.report();
        if (!exact_session(proof.original, before) || before.reads > ~block::SessionCounter{0} - proof.reads)
            return Status::fault("startup active reclaim original counter shadow differs");
        reclaim_io_.reads += proof.reads; reclaim_io_.completed_reads += proof.reads;
        if (auto status = reclaim_session_exact(); !status) return status;
        if (auto status = startup_domain_guard(); !status) return status;
        if (auto status = startup_reclaim_result_history(); !status) return status;
        return startup_reclaim_recover();
    }
    if (Ext4NamespaceWitnessCodec::present(w.locator_descriptor_))
    {
        startup_report_.initial_family = Ext4StartupFamily::namespace_active;
        if (auto status = select_common_endpoint(Ext4MountedCommonFamily::namespace_recovery, head.sequence, 5); !status) return status;
        if (auto status = startup_close_opening_route(); !status) return status;
        if (auto status = w.active_namespace_.prepare(*startup_reader_, bounds_); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        if (auto status = startup_namespace_history(); !status) return status;
        return startup_namespace_checkpoint();
    }
    const bool typed_zero = Ext4ZeroTruncateWitnessCodec::present(w.locator_descriptor_);
    const bool typed_allocated = Ext4AllocatedEmptyDataWitnessCodec::present(w.locator_descriptor_);
    if (typed_zero || typed_allocated)
    {
        if (!common_opening_) return Status::unsupported("startup typed witness requires distinct cold admission");
        if (typed_zero == typed_allocated) return Status::invalid_argument("common descriptor has no unique typed family");
        startup_report_.initial_family = typed_allocated ? Ext4StartupFamily::typed_allocated_active :
            Ext4StartupFamily::typed_zero_active;
        return select_common_endpoint(typed_allocated ? Ext4MountedCommonFamily::typed_allocated :
            Ext4MountedCommonFamily::typed_zero, head.sequence, 5);
    }
    // Only the checksum-validated descriptor selects a legacy proof. Recognized
    // Regular move, namespace, reclaim and typed families never fall through.
    std::array<u64, 8> targets{};
    usize actions{};
    if (auto status = codec.value().descriptor(w.locator_descriptor_, head.sequence,
            [&](const jbd2::Tag &tag) -> Status {
                if (actions == targets.size() || tag.target_block >= 32768 ||
                    (tag.target_block >= startup_journal_first_ &&
                     tag.target_block - startup_journal_first_ < startup_journal_blocks_))
                    return Status::unsupported("startup legacy action domain or capacity");
                for (usize index = 0; index < actions; ++index)
                    if (targets[index] == tag.target_block)
                        return Status::invalid_argument("startup legacy duplicate action target");
                targets[actions++] = tag.target_block;
                return {};
            }); !status) return status;
    const bool directory_create = Ext4DirectoryCreateWitnessCodec::present(w.locator_descriptor_);
    if (!actions || (directory_create ?
            ((actions != 7 && actions != 8) || w.locator_descriptor_[172] != actions) : actions > 6))
        return Status::unsupported("startup legacy action count or directory witness marker");
    if (auto status = select_common_endpoint(directory_create ? Ext4MountedCommonFamily::directory_create :
            Ext4MountedCommonFamily::legacy, head.sequence, 5); !status) return status;
    if (auto status = startup_close_opening_route(); !status) return status;
    w.legacy_head_seal_ = w.locator_head_;
    w.legacy_descriptor_seal_ = w.locator_descriptor_;
    startup_legacy_actions_ = actions;
    startup_legacy_selected_ = true;
    startup_report_.initial_family = Ext4StartupFamily::legacy_active;
    return startup_domain_guard();
}
[[nodiscard]] Status recover_startup(Ext4MountedStartupWorkspace &workspace,
                                    const Ext4MountedMetadata<DeviceCapacity> *mount, u64 epoch,
                                    StartupReadView &reader, StartupOrdinaryView &ordinary_reader)
{
    const auto initial = cache_.session_.report();
    if (!initial) return fail(initial.status());
    {
        smp::ScopedSpinLock guard(lock_);
        const auto cache = cache_.report();
        if (!mount || epoch != 1 || reader.owner_identity() != this || ordinary_reader.owner_identity() != this ||
            typed_native_public_blocked() || startup_active_ || startup_workspace_ || !mounted_dirty_ || BufferCapacity < 9 ||
            report_.phase != Ext4OwnedPhase::open || admissions_ || authority_.active ||
            cache.state != MetadataOwnerState::ready || cache.in_flight || cache.read_pins || cache.transaction_active ||
            initial.value().reads || initial.value().writes || initial.value().flushes ||
            initial.value().completed_reads || initial.value().completed_writes || initial.value().completed_flushes ||
            initial.value().in_flight || initial.value().pending_writes)
            return Status::busy("startup requires untouched continuing mounted Owner");
        startup_workspace_ = &workspace; startup_mount_ = mount; startup_epoch_ = epoch;
        startup_reader_ = &reader; startup_ordinary_reader_ = &ordinary_reader;
        startup_identity_ = cache; reclaim_io_ = initial.value(); publication_domain_ = epoch;
        startup_active_ = true; startup_generation_ = 1; report_.phase = Ext4OwnedPhase::preparing;
        startup_report_.phase = Ext4StartupPhase::classifying;
        startup_report_.original = startup_report_.observed = initial.value();
    }
    if (auto status = startup_domain_guard(); !status) return startup_stop(status);
    if (auto status = startup_route_budget_.reserve_owned_opening(cache_.session_, this); !status)
        return startup_stop(status);
    startup_route_opening_ = true;
    if (auto status = startup_classify(); !status)
        return startup_stop(status, status.code() == StatusCode::unsupported);
    if (common_typed_selected()) return {};
    if (startup_report_.initial_family == Ext4StartupFamily::regular_move_active)
    {
        if (startup_report_.phase != Ext4StartupPhase::admitting || startup_report_.regular_move_checkpoints != 1 ||
            !startup_report_.regular_move_result_validated)
            return startup_stop(Status::fault("startup regular move lacks complete admitting route"));
        return startup_regular_move_exact();
    }
    if (startup_reclaim_seal_.minted)
        return startup_reclaim_guard();
    if (startup_ordinary_selected())
    {
        smp::ScopedSpinLock guard(lock_);
        startup_report_.phase = Ext4StartupPhase::admitting; startup_report_.observed = reclaim_io_;
        return {};
    }
    { smp::ScopedSpinLock guard(lock_); startup_report_.phase = Ext4StartupPhase::draining; }
    u32 next{};
    for (usize member = 0; member < 8; ++member)
    {
        if (auto status = startup_domain_guard(); !status) return startup_stop(status);
        if (auto status = startup_prepare(member, next); !status) return startup_stop(status);
        if (auto status = startup_publish(); !status) return startup_stop(status);
        if (auto status = startup_checkpoint(); !status) return startup_stop(status);
        {
            smp::ScopedSpinLock guard(lock_);
            ++startup_report_.reclaimed; startup_report_.released_blocks += reclaim_plan_.reclaim.released_blocks;
            startup_report_.observed = reclaim_io_;
            startup_report_.final_sequence = jbd2::detail::be32(reclaim_empty_head_, 24);
        }
        startup_expected_empty_head_ = reclaim_empty_head_;
        startup_expected_directory_count_ = reclaim_disk_capture_.directory_count_;
        next = reclaim_plan_.reclaim.next_head;
        if (!next)
        {
            if (startup_report_.reclaimed != startup_report_.chain_members)
                return startup_stop(Status::fault("startup initial chain count differs"));
            smp::ScopedSpinLock guard(lock_); startup_report_.phase = Ext4StartupPhase::admitting; return {};
        }
        if (member + 1 >= startup_report_.chain_members || member + 1 == 8)
            return startup_stop(Status::fault("startup complete chain did not terminate"));
        const auto retained = reclaim_io_;
        if (auto status = startup_rearm(); !status) return startup_stop(status);
        reclaim_io_ = retained;
        if (startup_generation_ == ~u64{0}) return startup_stop(Status::overflow("startup seal generation exhausted"));
        ++startup_generation_; startup_seal_.active = false;
        { smp::ScopedSpinLock guard(lock_); report_.phase = Ext4OwnedPhase::preparing; }
    }
    return startup_stop(Status::fault("startup bounded chain exhausted"));
}
[[nodiscard]] Status startup_terminal_history()
{
    if (!startup_storage_exact()) return Status::fault("startup terminal original whole sources changed");
    const auto check = [&](u64 physical, const Block &before) -> Status {
        const Block *expected = &before;
        for (usize index = 0; index < reclaim_plan_.reclaim.count; ++index)
            if (physical == reclaim_plan_.reclaim.targets[index]) expected = &reclaim_plan_.reclaim.after[index];
        if (auto status = startup_read(u32(physical), reclaim_readback_); !status) return status;
        return reclaim_readback_ == *expected ? Status{} : Status::fault("startup terminal whole history differs");
    };
    const auto &old = startup_workspace_->original_;
    if (auto status = check(1, old.superblock); !status) return status;
    if (auto status = check(2, old.descriptors); !status) return status;
    for (usize group = 0; group < 4; ++group)
    {
        const auto &metadata = reclaim_capture_.binding_.metadata;
        if (auto status = check(metadata[group * 4 + 1].first, old.block_bitmaps[group]); !status) return status;
        if (auto status = check(metadata[group * 4 + 2].first, old.inode_bitmaps[group]); !status) return status;
        if (reclaim_capture_.initialized_[group])
            for (usize index = 0; index < 512; ++index)
                if (auto status = check(metadata[group * 4 + 3].first + index, old.inode_tables[group][index]); !status)
                    return status;
    }
    for (usize index = 0; index < reclaim_capture_.directory_count_; ++index)
        if (auto status = check(old.directories[index].physical, old.directories[index].bytes); !status) return status;
    if (auto status = startup_read(startup_journal_first_, reclaim_readback_); !status) return status;
    if (reclaim_readback_ != startup_expected_empty_head_ || !startup_storage_exact())
        return Status::fault("startup terminal journal or retained source seal differs");
    return {};
}
[[nodiscard]] Status startup_admit()
{
    if (startup_report_.phase != Ext4StartupPhase::admitting) return Status::busy("startup terminal admission is unavailable");
    if (startup_report_.initial_family == Ext4StartupFamily::regular_move_active)
        if (auto status = startup_regular_move_terminal(); !status) return startup_stop(status);
    if (startup_reclaim_seal_.minted)
    {
        if (auto status = startup_reclaim_guard(); !status) return startup_stop(status);
        if (startup_reclaim_seal_.control.phase != StartupReclaimPhase::admitting ||
            startup_reclaim_seal_.control.prefix.completed_rows != startup_reclaim_seal_.rows)
            return startup_stop(Status::fault("startup active reclaim private terminal cursor differs"));
        if (auto status = startup_reclaim_physical(); !status) return startup_stop(status);
        if (auto status = startup_handoff_cache_quiescent(); !status) return startup_stop(status);
    }
    if (startup_active_ && (startup_report_.initial_family == Ext4StartupFamily::empty_orphan ||
                            startup_report_.initial_family == Ext4StartupFamily::namespace_active))
    {
        if (auto status = startup_terminal_history(); !status) return startup_stop(status);
        const auto retained = reclaim_io_;
        if (auto status = startup_rearm(); !status) return startup_stop(status);
        reclaim_io_ = retained;
        if (auto status = reclaim_session_exact(); !status) return startup_stop(status);
    }
    if (auto status = startup_read(startup_journal_first_, reclaim_readback_); !status) return startup_stop(status);
    if (jbd2::detail::be32(reclaim_readback_, 28))
        return startup_stop(Status::fault("startup terminal journal is active"));
    if (startup_report_.initial_family == Ext4StartupFamily::regular_move_active &&
        (reclaim_readback_ != startup_expected_empty_head_ ||
         jbd2::detail::be32(reclaim_readback_, 24) != startup_report_.final_sequence))
        return startup_stop(Status::fault("startup regular move final journal differs from original completion"));
    if (auto status = reclaim_session_exact(); !status) return startup_stop(status);
    const auto actual = cache_.session_.report(); if (!actual) return startup_stop(actual.status());
    if (actual.value().state != block::DeviceState::owned || actual.value().in_flight || actual.value().pending_writes ||
        actual.value().identity != startup_report_.original.identity ||
        actual.value().registration_generation != startup_report_.original.registration_generation ||
        actual.value().session_serial != startup_report_.original.session_serial)
        return startup_stop(Status::fault("startup terminal continuing Session differs"));
    if (startup_reclaim_seal_.minted)
    {
        if (auto status = startup_reclaim_guard(); !status) return startup_stop(status);
        if (!startup_reclaim_ledger_.idle() || jbd2::detail::be32(reclaim_readback_, 24) != startup_reclaim_seal_.final_sequence)
            return startup_stop(Status::fault("startup active reclaim final Session or sequence differs"));
        if (auto status = startup_route_budget_.close(cache_.session_); !status) return startup_stop(status);
        startup_reclaim_seal_.control.phase = StartupReclaimPhase::complete; startup_reclaim_sync();
    }
    smp::ScopedSpinLock guard(lock_);
    startup_active_ = false; report_.phase = Ext4OwnedPhase::open;
    startup_report_.observed = actual.value(); startup_report_.final_head = 0;
    startup_report_.final_sequence = jbd2::detail::be32(reclaim_readback_, 24);
    startup_report_.phase = Ext4StartupPhase::complete; startup_report_.strict_ordinary_admission = true;
    if (common_opening_)
    {
        if (common_typed_selected() || common_opening_->phase_ != CommonOpeningPhase::selected)
            return Status::fault("common legacy terminal admission differs from original Selection");
        common_opening_->phase_ = CommonOpeningPhase::legacy_root;
    }
    return {};
}
#include "ok/fs/ext4_mounted_reclaim_checkpoint.inc"
#include "ok/fs/ext4_mounted_startup_owner.inc"
#include "ok/fs/ext4_mounted_reclaim_history.inc"
#include "ok/fs/ext4_mounted_namespace_history.inc"
#include "ok/fs/ext4_mounted_namespace_checkpoint.inc"
#include "ok/fs/ext4_mounted_regular_move_startup.inc"
Ext4MountedStartupWorkspace *startup_workspace_{};
const Ext4MountedMetadata<DeviceCapacity> *startup_mount_{};
MetadataOwnerReport startup_identity_{};
Ext4StartupRecoveryReport startup_report_{};
StartupColdSeal startup_seal_{};
Ext4ReclaimRecoveryBinding<DeviceCapacity> startup_reclaim_binding_{};
Ext4ReclaimResultHistory<DeviceCapacity> startup_reclaim_history_{};
Ext4ReclaimComposedHistory<DeviceCapacity> startup_reclaim_composed_{};
Ext4OrphanChainCapture<DeviceCapacity> startup_reclaim_result_capture_{};
Ext4OrphanChainCapture<DeviceCapacity> startup_namespace_capture_{};
std::array<std::array<u32, 2>, 8> startup_initial_members_{};
std::array<u8, 16> startup_initial_uuid_{}, startup_locator_uuid_{};
Block startup_expected_empty_head_{};
usize startup_expected_directory_count_{};
u32 startup_journal_first_{}, startup_journal_blocks_{}, startup_journal_table_{};
u64 startup_epoch_{}, startup_generation_{};
bool startup_active_{};
bool startup_legacy_selected_{};
usize startup_legacy_actions_{};
// The mount retains these typed same-Owner views for its entire lifetime.
StartupReadView *startup_reader_{};
StartupOrdinaryView *startup_ordinary_reader_{};
