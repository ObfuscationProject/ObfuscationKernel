// Private active-reclaim port in the continuing, unpublished mounted Owner.
using StartupReclaimHistory = Ext4ReclaimComposedHistory<DeviceCapacity>;
using StartupReclaimPrefix = typename StartupReclaimHistory::PhysicalPrefix;
using StartupReclaimHead = typename StartupReclaimHistory::PrefixHead;
enum class StartupReclaimPhase : u8 { unused, attaching, attached, checkpointing, drained, admitting, complete, failed };
struct StartupReclaimSeal final
{
    StartupReclaimSeal() = default;
    StartupReclaimSeal(const StartupReclaimSeal &) = delete;
    StartupReclaimSeal &operator=(const StartupReclaimSeal &) = delete;
    StartupReclaimSeal(StartupReclaimSeal &&) = delete;
    StartupReclaimSeal &operator=(StartupReclaimSeal &&) = delete;
    const Ext4OwnedTransaction *owner{};
    const Cache *cache{};
    const typename Registry::Session *session{};
    const Ext4MountedStartupWorkspace *workspace{};
    const Ext4MountedMetadata<DeviceCapacity> *mount{};
    const StartupReadView *reader{};
    const StartupOrdinaryView *ordinary_reader{};
    const void *live_workspace{};
    const Ext4ReclaimRecoveryBinding<DeviceCapacity> *binding{};
    const Ext4ReclaimResultHistory<DeviceCapacity> *result{};
    const StartupReclaimHistory *composed{};
    MetadataOwnerReport identity{};
    u64 epoch{}, origin_generation{}, initial_transaction{};
    usize rows{};
    u32 active_sequence{}, final_sequence{};
    bool minted{};
    struct Control
    {
        StartupReclaimPhase phase{StartupReclaimPhase::unused};
        StartupReclaimPrefix prefix{};
        u64 attachment_generation{}, transaction{};
        usize pinned{}, publications{};
        bool capture_ready{}, plan_ready{};
        std::array<usize, participant_capacity> slots{};
        std::array<u64, participant_capacity> generations{};
    } control{}, retained{};
};
[[nodiscard]] static bool startup_reclaim_same_control(const typename StartupReclaimSeal::Control &a,
                                                       const typename StartupReclaimSeal::Control &b)
{
    return a.phase == b.phase && a.prefix.completed_rows == b.prefix.completed_rows &&
        a.prefix.wal_writes == b.prefix.wal_writes && a.prefix.home_after_mask == b.prefix.home_after_mask &&
        a.prefix.head == b.prefix.head && a.attachment_generation == b.attachment_generation &&
        a.transaction == b.transaction && a.pinned == b.pinned && a.publications == b.publications &&
        a.capture_ready == b.capture_ready && a.plan_ready == b.plan_ready && a.slots == b.slots && a.generations == b.generations;
}
void startup_reclaim_sync() { startup_reclaim_seal_.retained = startup_reclaim_seal_.control; }
[[nodiscard]] Status startup_reclaim_poison(Status status)
{
    static_cast<void>(startup_reclaim_anchor_.abort(status));
    startup_reclaim_seal_.control.phase = StartupReclaimPhase::failed;
    startup_reclaim_sync();
    return status;
}
// Original read-only histories remain frozen. Their retained_exact methods
// prove bytes and identities without rebasing their consumed Session equations.
[[nodiscard]] Status startup_reclaim_guard()
{
    const auto &seal = startup_reclaim_seal_; const auto &state = seal.control;
    if (!seal.minted || seal.owner != this || seal.cache != &cache_ || seal.session != &cache_.session_ ||
        seal.workspace != startup_workspace_ || seal.mount != startup_mount_ || seal.reader != startup_reader_ ||
        seal.ordinary_reader != startup_ordinary_reader_ ||
        startup_mount_->startup_workspace_ != seal.workspace || startup_mount_->reclaim_workspace_ != seal.live_workspace ||
        seal.binding != &startup_reclaim_binding_ ||
        seal.result != &startup_reclaim_history_ || seal.composed != &startup_reclaim_composed_ ||
        !startup_reclaim_anchor_.exact(&cache_.session_) || !startup_reclaim_same_control(state, seal.retained) ||
        state.phase == StartupReclaimPhase::unused || state.phase == StartupReclaimPhase::failed ||
        state.phase == StartupReclaimPhase::complete ||
        startup_report_.initial_family != Ext4StartupFamily::reclaim_active ||
        startup_report_.chain_members != seal.rows || startup_report_.reclaimed != state.prefix.completed_rows ||
        startup_report_.prospective_members + 1 != seal.rows || !startup_report_.root_unpublished ||
        startup_report_.phase != (state.phase == StartupReclaimPhase::admitting ?
            Ext4StartupPhase::admitting : Ext4StartupPhase::reclaim_checkpointing) || seal.epoch != startup_epoch_ ||
        seal.origin_generation != startup_generation_ || seal.origin_generation != 1 ||
        state.attachment_generation != seal.origin_generation + state.prefix.completed_rows ||
        startup_route_opening_ || startup_seal_.active || startup_namespace_seal_.active || authority_.active ||
        admissions_ || reclaim_lifetime_ || reclaim_ticket_.valid())
        return Status::fault("startup active reclaim original attachment differs");
    if (auto status = startup_domain_guard(); !status) return status;
    if (auto status = startup_reclaim_composed_.retained_exact(); !status) return status;
    const auto &history = startup_reclaim_history_; const auto &composed = startup_reclaim_composed_;
    const auto &domain = history.domain_; const auto cache = cache_.report();
    if (domain.owner != this || domain.cache != &cache_ || domain.workspace != seal.workspace ||
        domain.mount != seal.mount || domain.epoch != seal.epoch || domain.generation != seal.origin_generation ||
        domain.guard != &startup_reclaim_history_domain || domain.context != this ||
        composed.origin_ != &history || composed.storage_ != &startup_workspace_->reclaim_composed_ ||
        composed.original_session_ != &cache_.session_ || !composed.complete_ || !composed.endpoint_ ||
        composed.reading_ || composed.ordinary_ || composed.row_count_ != seal.rows ||
        seal.rows != history.member_count_ + 1 || seal.rows < 1 || seal.rows > 8 ||
        seal.active_sequence != startup_reclaim_binding_.capture_.retained_.report.semantic.active_sequence ||
        seal.final_sequence != seal.active_sequence + 2 + u32(3 * (seal.rows - 1)) ||
        cache.device_identity != seal.identity.device_identity ||
        cache.registration_generation != seal.identity.registration_generation ||
        cache.session_serial != seal.identity.session_serial || cache.first_sector != seal.identity.first_sector ||
        cache.sectors != seal.identity.sectors || cache.blocks != seal.identity.blocks ||
        cache.state != MetadataOwnerState::ready || state.prefix.completed_rows > seal.rows ||
        state.publications > 2 || state.pinned > participant_capacity)
        return Status::fault("startup active reclaim retained domain or route differs");
    if (auto status = composed.prefix_exact(state.prefix); !status) return status;
    if (state.capture_ready)
        if (auto status = startup_reclaim_capture_exact<false>(); !status) return status;
    if (state.plan_ready)
    {
        if (state.prefix.completed_rows == seal.rows) return Status::fault("startup active reclaim plan outlived row");
        const auto &row = startup_workspace_->reclaim_composed_.rows_[state.prefix.completed_rows];
        if (!reclaim_plan_.valid || !reclaim_history::same_plan(reclaim_plan_.reclaim, row.plan.reclaim) ||
            !reclaim_history::same_directory(reclaim_plan_.directory, row.plan.directory) ||
            !reclaim_history::same_journal(reclaim_journal_, row.wal))
            return Status::fault("startup active reclaim attached physical plan or WAL changed");
    }
    if (state.phase == StartupReclaimPhase::drained || state.phase == StartupReclaimPhase::admitting)
    {
        if (state.pinned || state.transaction || state.publications || participant_count_ || transaction_.active_)
            return Status::fault("startup active reclaim drained handles differ");
        // Ordinary mounted validation borrows one temporary cache Pin per
        // read. A guarded miss is loading while its Session call is in flight;
        // final admission separately requires every raw slot to be drained.
        if (state.phase != StartupReclaimPhase::admitting) return startup_handoff_cache_quiescent();
        smp::ScopedSpinLock cache_guard(cache_.lock_);
        for (const auto &entry : cache_.entries_)
        {
            if (entry.report.state == MetadataBufferState::unused || entry.report.state == MetadataBufferState::exhausted ||
                entry.report.state == MetadataBufferState::loading) continue;
            if (entry.report.state != MetadataBufferState::ready || entry.report.transaction_serial || entry.report.staged_dirty)
                return Status::fault("startup active reclaim ordinary cache slot is not canonical");
            const Block *expected{};
            if (auto status = composed.resolve_retained_physical_prefix(state.prefix, u32(entry.report.block), expected); !status)
                return status;
            if (!expected) return Status::fault("startup active reclaim ordinary cache source is absent");
            for (usize byte = 0; byte < 1024; ++byte)
                if (entry.canonical[byte] != std::byte((*expected)[byte]))
                    return Status::fault("startup active reclaim whole ordinary canonical cache source differs");
        }
        return {};
    }
    if (state.prefix.completed_rows == seal.rows || report_.phase != Ext4OwnedPhase::preparing)
        return Status::fault("startup active reclaim current row or Owner phase differs");
    const auto &row = startup_workspace_->reclaim_composed_.rows_[state.prefix.completed_rows];
    const auto &plan = row.plan.reclaim;
    if (participant_count_ != plan.count + 1 || state.pinned > participant_count_ ||
        state.transaction != (state.phase == StartupReclaimPhase::attaching ? 0 : transaction_.serial_))
        return Status::fault("startup active reclaim participant attachment differs");
    smp::ScopedSpinLock cache_guard(cache_.lock_);
    if (state.phase != StartupReclaimPhase::attaching)
    {
        if (state.pinned != participant_count_ || state.transaction != cache_.transaction_serial_ ||
            state.transaction != seal.initial_transaction + state.prefix.completed_rows + 1)
            return Status::fault("startup active reclaim original transaction serial differs");
        if (auto status = prepared(true); !status) return status;
    }
    for (usize index = 0; index < state.pinned; ++index)
    {
        const auto &pin = pins_[index]; const auto *entry = cache_.find(pin);
        const u64 physical = index ? plan.targets[index - 1] : startup_reclaim_binding_.journal_first_;
        const Block *current{};
        if (index)
        {
            if (!state.prefix.completed_rows) current = &startup_reclaim_binding_.capture_.retained_.current[index - 1];
            else current = &plan.before[index - 1];
        }
        else if (!state.prefix.completed_rows) current = &row.wal.journal_head;
        else current = &startup_workspace_->reclaim_composed_.rows_[state.prefix.completed_rows - 1].cleanup;
        const Block &staged = index ? plan.after[index - 1] : (state.publications == 2 ? row.cleanup : row.wal.journal_head);
        const Block &canonical = state.publications ? staged : *current;
        if (!entry || pin.owner_ != &cache_ || !pin.active_ || state.slots[index] != pin.slot_ ||
            state.generations[index] > ~u64{0} - 2 || pin.generation_ != state.generations[index] + state.publications ||
            blocks_[index] != physical || sealed_[index] != staged || entry->report.block != physical ||
            entry->report.state != MetadataBufferState::ready)
            return Status::fault("startup active reclaim canonical pin identity differs");
        for (usize byte = 0; byte < 1024; ++byte)
            if (entry->canonical[byte] != std::byte(canonical[byte]))
                return Status::fault("startup active reclaim complete canonical source differs");
    }
    return {};
}
// This permanent ledger cannot be copied, moved or replaced between rows.
// The Owner shadow is advanced only from the matching successful return.
class StartupReclaimLedger final
{
    using Operation = Ext4ReclaimRouteOperation;
  public:
    static constexpr bool defer_cache_begin = true;
    explicit StartupReclaimLedger(Ext4OwnedTransaction &owner) : owner_(owner) {}
    ~StartupReclaimLedger()
    { if (operation_ != Operation::none) static_cast<void>(owner_.startup_reclaim_anchor_.abort(
        Status::interrupted("startup active reclaim unfinished dispatch"))); }
    StartupReclaimLedger(const StartupReclaimLedger &) = delete;
    StartupReclaimLedger &operator=(const StartupReclaimLedger &) = delete;
    StartupReclaimLedger(StartupReclaimLedger &&) = delete;
    StartupReclaimLedger &operator=(StartupReclaimLedger &&) = delete;
    [[nodiscard]] Status begin_read(typename Registry::Session &session, u64 first, usize bytes)
    { return begin(session, Operation::read, first, bytes); }
    [[nodiscard]] Status begin_cache_read(typename Registry::Session &session, u64 first, usize bytes,
                                          usize slot, u64 generation)
    {
        if (slot >= BufferCapacity || !generation)
            return owner_.startup_reclaim_poison(Status::fault("startup active reclaim canonical miss selection differs"));
        if (auto status = begin(session, Operation::read, first, bytes); !status) return status;
        cache_slot_ = cache_slot_seal_ = slot; cache_generation_ = cache_generation_seal_ = generation;
        return {};
    }
    [[nodiscard]] Status finish_read(typename Registry::Session &session, Status transport)
    { return finish(session, Operation::read, transport); }
    [[nodiscard]] Status begin_write(typename Registry::Session &session, u64 first, usize bytes)
    { return begin(session, Operation::write, first, bytes); }
    [[nodiscard]] Status finish_write(typename Registry::Session &session, Status transport)
    { return finish(session, Operation::write, transport); }
    [[nodiscard]] Status begin_flush(typename Registry::Session &session)
    { return begin(session, Operation::flush, 0, 0); }
    [[nodiscard]] Status finish_flush(typename Registry::Session &session, Status transport)
    { return finish(session, Operation::flush, transport); }
    [[nodiscard]] Status in_flight_exact(typename Registry::Session &session)
    {
        if (&session != &owner_.cache_.session_ || operation_ == Operation::none || operation_ != operation_seal_ ||
            operation_ != owner_.startup_route_budget_.control_.operation || !exact_session(before_, before_seal_) ||
            !exact_session(owner_.reclaim_io_, before_))
            return owner_.startup_reclaim_poison(Status::fault("startup active reclaim paused ledger changed"));
        if (auto status = cache_miss_exact(); !status) return owner_.startup_reclaim_poison(status);
        if (auto status = owner_.startup_reclaim_guard(); !status) return owner_.startup_reclaim_poison(status);
        return owner_.startup_route_budget_.in_flight_exact(session);
    }
    [[nodiscard]] bool idle() const
    { return operation_ == Operation::none && operation_seal_ == Operation::none && exact_session(before_, before_seal_); }
  private:
    [[nodiscard]] Status cache_miss_exact() const
    {
        if (cache_slot_ != cache_slot_seal_ || cache_generation_ != cache_generation_seal_)
            return Status::fault("startup active reclaim paused canonical miss identity changed");
        if (cache_slot_ == BufferCapacity) return {};
        smp::ScopedSpinLock cache_guard(owner_.cache_.lock_);
        const auto &entry = owner_.cache_.entries_[cache_slot_].report;
        const auto &route = owner_.startup_route_budget_.control_;
        if (operation_ != Operation::read || !owner_.cache_.report_.in_flight ||
            entry.state != MetadataBufferState::loading || entry.block != route.first_sector / 2 ||
            entry.generation != cache_generation_ || entry.read_pins != 1 || entry.transaction_serial || entry.staged_dirty)
            return Status::fault("startup active reclaim original canonical loading slot changed");
        return {};
    }
    [[nodiscard]] Status begin(typename Registry::Session &session, Operation operation, u64 first, usize bytes)
    {
        if (&session != &owner_.cache_.session_ || operation_ != Operation::none || operation_seal_ != Operation::none ||
            !exact_session(before_, before_seal_))
            return owner_.startup_reclaim_poison(Status::fault("startup active reclaim dispatch reentered"));
        if (auto status = owner_.startup_reclaim_guard(); !status) return owner_.startup_reclaim_poison(status);
        if (auto status = owner_.reclaim_session_exact(); !status) return owner_.startup_reclaim_poison(status);
        auto &budget = owner_.startup_route_budget_;
        const auto status = operation == Operation::read ? budget.begin_read(session, first, bytes) :
            operation == Operation::write ? budget.begin_write(session, first, bytes) : budget.begin_flush(session);
        if (!status) return owner_.startup_reclaim_poison(status);
        before_ = before_seal_ = owner_.reclaim_io_; operation_ = operation_seal_ = operation;
        cache_slot_ = cache_slot_seal_ = BufferCapacity; cache_generation_ = cache_generation_seal_ = 0;
        return {};
    }
    [[nodiscard]] Status finish(typename Registry::Session &session, Operation operation, Status transport)
    {
        const bool paused = &session == &owner_.cache_.session_ && operation_ == operation &&
            operation != Operation::none && operation_ == operation_seal_ && exact_session(before_, before_seal_) &&
            exact_session(owner_.reclaim_io_, before_);
        const auto miss = cache_miss_exact();
        const auto authority = owner_.startup_reclaim_guard();
        auto &budget = owner_.startup_route_budget_;
        // Failed transport attempts still belong to the original exact ledger.
        // Record their attempt/completion split even when the callback already
        // poisoned the outer attachment; never adopt them as Owner progress.
        const auto accounted = operation == Operation::read ? budget.finish_read(session, transport) :
            operation == Operation::write ? budget.finish_write(session, transport) : budget.finish_flush(session, transport);
        if (!transport || !accounted || !paused || !miss || !authority)
        {
            operation_ = operation_seal_ = Operation::none;
            cache_slot_ = cache_slot_seal_ = BufferCapacity; cache_generation_ = cache_generation_seal_ = 0;
            return owner_.startup_reclaim_poison(!transport ? transport : !accounted ? accounted : !paused ?
                Status::fault("startup active reclaim successful-return attachment changed") : !miss ? miss : authority);
        }
        auto expected = before_;
        if (operation == Operation::read) { ++expected.reads; ++expected.completed_reads; }
        if (operation == Operation::write) { ++expected.writes; ++expected.completed_writes; expected.pending_writes = true; }
        if (operation == Operation::flush) { ++expected.flushes; ++expected.completed_flushes; expected.pending_writes = false; }
        const auto actual = session.report();
        if (!actual || !exact_session(actual.value(), expected))
            return owner_.startup_reclaim_poison(Status::fault("startup active reclaim exact returned Session prefix differs"));
        owner_.reclaim_io_ = expected; operation_ = operation_seal_ = Operation::none;
        cache_slot_ = cache_slot_seal_ = BufferCapacity; cache_generation_ = cache_generation_seal_ = 0;
        return {};
    }
    Ext4OwnedTransaction &owner_;
    block::SessionReport before_{}, before_seal_{};
    Operation operation_{Operation::none}, operation_seal_{Operation::none};
    usize cache_slot_{BufferCapacity}, cache_slot_seal_{BufferCapacity};
    u64 cache_generation_{}, cache_generation_seal_{};
};
[[nodiscard]] Status startup_reclaim_read(u64 first, std::span<std::byte> bytes)
{ return block::guarded_read_blocks(cache_.session_, startup_reclaim_ledger_, first, bytes); }
[[nodiscard]] Status startup_reclaim_write(u32 physical, const Block &bytes)
{
    if (auto status = startup_reclaim_ledger_.begin_write(cache_.session_, u64{physical} * 2, 1024); !status) return status;
    const auto transport = cache_.session_.write_blocks_guarded(u64{physical} * 2,
        std::as_bytes(std::span{bytes}), startup_reclaim_ledger_);
    return startup_reclaim_ledger_.finish_write(cache_.session_, transport);
}
[[nodiscard]] Status startup_reclaim_flush()
{
    if (auto status = startup_reclaim_ledger_.begin_flush(cache_.session_); !status) return status;
    const auto transport = cache_.session_.flush_guarded(startup_reclaim_ledger_);
    return startup_reclaim_ledger_.finish_flush(cache_.session_, transport);
}
[[nodiscard]] Status startup_reclaim_physical()
{
    if (auto status = startup_reclaim_guard(); !status) return status;
    usize count{};
    const auto compare = [&](u32 physical, const Block &) -> Status {
        if (++count > 2335) return Status::overflow("startup active reclaim whole enumeration allowance");
        const Block *expected{};
        if (auto status = startup_reclaim_composed_.resolve_retained_physical_prefix(startup_reclaim_seal_.control.prefix,
            physical, expected); !status) return status;
        if (!expected) return Status::fault("startup active reclaim expected physical block is absent");
        if (auto status = startup_reclaim_read(u64{physical} * 2, std::as_writable_bytes(std::span{reclaim_readback_})); !status)
            return status;
        return reclaim_readback_ == *expected ? Status{} :
            startup_reclaim_poison(Status::fault("startup active reclaim whole physical prefix differs"));
    };
    if (auto status = startup_reclaim_history_.each_original(compare); !status) return status;
    if (startup_reclaim_composed_.extension_)
        if (auto status = compare(startup_reclaim_binding_.journal_first_ + 8,
            startup_workspace_->reclaim_composed_.extension_); !status) return status;
    return startup_reclaim_guard();
}
[[nodiscard]] Status startup_reclaim_mint()
{
    if (startup_reclaim_seal_.minted)
        return startup_reclaim_poison(Status::already_exists("startup active reclaim attachment is permanent"));
    if (auto status = startup_domain_guard(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    if (auto status = startup_reclaim_composed_.retained_exact(); !status) return status;
    const auto &composed = startup_reclaim_composed_; const usize rows = composed.row_count_;
    const auto &active = startup_reclaim_binding_.capture_.retained_;
    const auto capacity = Ext4ReclaimRouteBudget<typename Registry::Session>::capacity(
        startup_route_budget_.initial_, rows - 1, active.report.semantic.active_sequence);
    if (!capacity) return capacity.status();
    if (!composed.complete_ || !composed.endpoint_ || composed.reading_ || composed.ordinary_ ||
        rows < 1 || rows > 8 || startup_generation_ != 1 || startup_reclaim_history_.domain_.generation != 1 ||
        participant_count_ || transaction_.active_ || reclaim_active_ || reclaim_lifetime_ || reclaim_ticket_.valid() ||
        reclaim_storage_ || reclaim_scratch_ || reclaim_io_.pending_writes || !startup_reclaim_ledger_.idle() ||
        startup_workspace_->claimant_ != startup_mount_ || startup_workspace_->owner_ != this ||
        rows > ~u64{0} - startup_generation_ || cache_.transaction_serial_ > ~u64{0} - rows)
        return Status::fault("startup active reclaim complete original attachment unavailable");
    // Each leaf READ can be a mounted canonical miss. Every row can also
    // retire at most 16 clean slots and publish twice. Reserve the full READ
    // allowance plus those advances for every slot before the first mutation.
    const u64 generation_reservation = u64(capacity.value().reads) + u64(rows) * 18;
    {
        smp::ScopedSpinLock cache_guard(cache_.lock_);
        for (const auto &entry : cache_.entries_)
            if (entry.report.generation > ~u64{0} - generation_reservation)
                return Status::overflow("startup active reclaim whole cache generation reservation exhausted");
    }
    if (auto status = startup_handoff_opening_route(); !status) return status;
    if (auto status = startup_reclaim_anchor_.bind(cache_.session_); !status) return status;
    auto &seal = startup_reclaim_seal_;
    seal.owner = this; seal.cache = &cache_; seal.session = &cache_.session_; seal.workspace = startup_workspace_;
    seal.mount = startup_mount_; seal.reader = startup_reader_; seal.ordinary_reader = startup_ordinary_reader_;
    seal.live_workspace = startup_mount_->reclaim_workspace_;
    seal.binding = &startup_reclaim_binding_; seal.result = &startup_reclaim_history_; seal.composed = &composed;
    seal.identity = cache_.report(); seal.epoch = startup_epoch_; seal.origin_generation = startup_generation_;
    seal.initial_transaction = cache_.transaction_serial_; seal.rows = rows;
    seal.active_sequence = active.report.semantic.active_sequence;
    seal.final_sequence = capacity.value().projected_sequence_end;
    seal.control.phase = StartupReclaimPhase::attaching;
    seal.control.attachment_generation = seal.origin_generation;
    seal.control.prefix.head = StartupReclaimHead::active;
    for (usize home = 0; home < active.count; ++home)
        if (active.current[home] == active.plan.reclaim.after[home])
            seal.control.prefix.home_after_mask |= u8(1U << home);
    participant_count_ = active.count + 1;
    seal.minted = true; startup_reclaim_sync();
    { smp::ScopedSpinLock owner_guard(lock_);
        startup_report_.phase = Ext4StartupPhase::reclaim_checkpointing;
        startup_report_.chain_members = rows; report_.phase = Ext4OwnedPhase::preparing; }
    return startup_reclaim_guard();
}
// A later real C1 capture is checked against the exact completed original row
// prefix. Its ordinary validity and its report never replace this comparison.
template <bool CheckGuard = true>
[[nodiscard]] Status startup_reclaim_capture_exact()
{
    if constexpr (CheckGuard)
        if (auto status = startup_reclaim_guard(); !status) return status;
    const auto &state = startup_reclaim_seal_.control;
    const auto &capture = reclaim_capture_; const auto &images = startup_workspace_->original_;
    const auto &row = startup_workspace_->reclaim_composed_.rows_[state.prefix.completed_rows];
    if (!state.prefix.completed_rows || state.prefix.completed_rows >= startup_reclaim_seal_.rows ||
        capture.session_ != &cache_.session_ || capture.storage_ != &images ||
        capture.scratch_ != &startup_workspace_->preparation_ || !capture.report_.valid ||
        capture.chain_count_ != startup_reclaim_seal_.rows - state.prefix.completed_rows ||
        capture.report_.head != row.inode || capture.report_.generation != row.generation ||
        capture.binding_.uuid != startup_reclaim_binding_.uuid_ || !capture.binding_.empty_log ||
        capture.binding_.journal_sequence != startup_workspace_->reclaim_composed_.rows_[state.prefix.completed_rows - 1].cleanup_sequence)
        return Status::fault("startup active reclaim physical C1 identity differs from retained result");
    const StartupReclaimPrefix preceding{state.prefix.completed_rows, 0, 0, StartupReclaimHead::predecessor};
    const auto compare = [&](u32 physical, const Block &bytes) -> Status {
        const Block *expected{};
        if (auto status = startup_reclaim_composed_.resolve_retained_physical_prefix(preceding, physical, expected); !status)
            return status;
        return expected && bytes == *expected ? Status{} :
            Status::fault("startup active reclaim physical C1 whole source differs from retained result");
    };
    if (auto status = compare(1, images.superblock); !status) return status;
    if (auto status = compare(2, images.descriptors); !status) return status;
    if (auto status = compare(startup_reclaim_binding_.journal_first_, images.journal); !status) return status;
    for (usize group = 0; group < 4; ++group)
    {
        if (auto status = compare(u32(capture.binding_.metadata[group * 4 + 1].first), images.block_bitmaps[group]); !status)
            return status;
        if (auto status = compare(u32(capture.binding_.metadata[group * 4 + 2].first), images.inode_bitmaps[group]); !status)
            return status;
        if (capture.initialized_[group])
            for (usize index = 0; index < 512; ++index)
                if (auto status = compare(u32(capture.binding_.metadata[group * 4 + 3].first + index),
                    images.inode_tables[group][index]); !status) return status;
    }
    const auto &original = startup_reclaim_binding_.capture_.retained_;
    if (capture.initialized_ != original.initialized || capture.directory_count_ != original.directories)
        return Status::fault("startup active reclaim physical C1 table or directory geometry differs");
    for (usize index = 0; index < capture.directory_count_; ++index)
    {
        const auto &a = images.directories[index]; const auto &b = original.storage.directories[index];
        if (a.inode != b.inode || a.logical != b.logical || a.physical != b.physical)
            return Status::fault("startup active reclaim physical C1 directory identity differs");
        if (auto status = compare(a.physical, a.bytes); !status) return status;
    }
    for (usize index = 0; index < capture.bootstrap_count_; ++index)
        if (auto status = compare(images.bootstrap_reads[index].physical, images.bootstrap_reads[index].bytes); !status)
            return status;
    for (usize index = 0; index < capture.chain_count_; ++index)
    {
        const auto &member = startup_reclaim_composed_.members_[state.prefix.completed_rows - 1 + index];
        const auto &actual = images.chain[index];
        if (actual.number != member.inode || actual.expected_generation != member.generation)
            return Status::fault("startup active reclaim physical C1 remaining vector differs");
        if (auto status = compare(actual.table_block, actual.table); !status) return status;
    }
    if constexpr (CheckGuard) return startup_reclaim_guard();
    return {};
}
[[nodiscard]] Status startup_reclaim_attach()
{
    auto &state = startup_reclaim_seal_.control;
    if (auto status = startup_reclaim_guard(); !status) return status;
    if (!startup_reclaim_ledger_.idle() || state.prefix.completed_rows >= startup_reclaim_seal_.rows)
        return Status::fault("startup active reclaim row attachment cursor differs");
    const auto &row = startup_workspace_->reclaim_composed_.rows_[state.prefix.completed_rows];
    const auto &plan = row.plan.reclaim;
    if (state.prefix.completed_rows)
    {
        if (state.phase != StartupReclaimPhase::drained)
            return Status::fault("startup active reclaim later row has retained handles");
        reclaim_storage_ = &startup_workspace_->original_; reclaim_scratch_ = &startup_workspace_->preparation_;
        if (auto status = reclaim_capture_.capture(cache_.session_, *reclaim_storage_, *reclaim_scratch_,
            startup_reclaim_ledger_); !status) return status;
        state.capture_ready = true; startup_reclaim_sync();
        if (auto status = startup_reclaim_capture_exact(); !status) return status;
        if (auto status = reclaim_capture_.plan_head(row.inode, row.generation, reclaim_plan_, startup_reclaim_ledger_); !status)
            return status;
        if (!reclaim_plan_.valid || !reclaim_history::same_plan(reclaim_plan_.reclaim, plan) ||
            !reclaim_history::same_directory(reclaim_plan_.directory, row.plan.directory))
            return Status::fault("startup active reclaim physical C1 plan differs from sealed dry row");
        if (auto status = Ext4OrphanReclaimJournalPlanner::plan(reclaim_capture_.binding_, reclaim_plan_.reclaim,
            startup_workspace_->original_.journal, row.seconds, row.nanoseconds, reclaim_format_, reclaim_journal_); !status)
            return status;
        if (!reclaim_history::same_journal(reclaim_journal_, row.wal))
            return Status::fault("startup active reclaim physical C1 WAL differs from sealed dry row");
        state.plan_ready = true; state.phase = StartupReclaimPhase::attaching;
        participant_count_ = plan.count + 1; startup_reclaim_sync();
    }
    std::array<u64, 16> released{};
    for (usize index = 0; index < plan.released_blocks; ++index) released[index] = plan.released_mapping[index];
    if (auto retired = cache_.retire_clean_blocks(std::span<const u64>{released}.first(plan.released_blocks)); !retired)
        return retired.status();
    blocks_[0] = startup_reclaim_binding_.journal_first_; sealed_[0] = row.wal.journal_head;
    for (usize index = 0; index < plan.count; ++index)
    { blocks_[index + 1] = plan.targets[index]; sealed_[index + 1] = plan.after[index]; }
    for (usize index = 0; index < participant_count_; ++index)
    {
        if (auto status = startup_reclaim_guard(); !status) return status;
        if (auto status = cache_.pin(blocks_[index], pins_[index], startup_reclaim_ledger_); !status) return status;
        state.slots[index] = pins_[index].slot_; state.generations[index] = pins_[index].generation_;
        ++state.pinned; startup_reclaim_sync();
        if (auto status = startup_reclaim_guard(); !status) return status;
    }
    std::array<Pin *, participant_capacity> pins{};
    for (usize index = 0; index < participant_count_; ++index) pins[index] = &pins_[index];
    if (auto status = cache_.begin(std::span<Pin *const>{pins}.first(participant_count_), transaction_); !status) return status;
    for (usize index = 0; index < participant_count_; ++index)
        if (auto status = transaction_.edit(blocks_[index], 0, std::as_bytes(std::span{sealed_[index]})); !status) return status;
    state.transaction = transaction_.serial_; state.phase = StartupReclaimPhase::attached; startup_reclaim_sync();
    if (auto status = startup_reclaim_guard(); !status) return status;
    // Re-read the entire original physical domain before private publication.
    if (auto status = startup_reclaim_physical(); !status) return status;
    {
        smp::ScopedSpinLock cache_guard(cache_.lock_);
        if (auto status = prepared(true); !status) return status;
        publish(sealed_); state.publications = 1; state.phase = StartupReclaimPhase::checkpointing;
        startup_reclaim_sync();
    }
    if (!state.prefix.completed_rows)
        if (auto status = startup_route_budget_.enter_writer_phase(cache_.session_, this); !status) return status;
    return startup_reclaim_guard();
}
[[nodiscard]] Status startup_reclaim_checkpoint_row()
{
    if (auto status = startup_reclaim_attach(); !status) return status;
    auto &state = startup_reclaim_seal_.control;
    const auto &row = startup_workspace_->reclaim_composed_.rows_[state.prefix.completed_rows];
    const auto &plan = row.plan.reclaim; const u32 first = startup_reclaim_binding_.journal_first_;
    if (auto status = startup_reclaim_flush(); !status) return status;
    if (state.prefix.completed_rows)
    {
        if (auto status = startup_reclaim_write(first + 1, row.wal.descriptor); !status) return status;
        ++state.prefix.wal_writes; startup_reclaim_sync();
        for (usize index = 0; index < row.wal.count; ++index)
        {
            if (auto status = startup_reclaim_write(first + 2 + u32(index), row.wal.payloads[index]); !status) return status;
            ++state.prefix.wal_writes; startup_reclaim_sync();
        }
        if (auto status = startup_reclaim_write(first + 3 + u32(row.wal.count), row.wal.end); !status) return status;
        ++state.prefix.wal_writes; startup_reclaim_sync();
        if (auto status = startup_reclaim_flush(); !status) return status;
        if (auto status = startup_reclaim_write(first + 2 + u32(row.wal.count), row.wal.commit); !status) return status;
        ++state.prefix.wal_writes; startup_reclaim_sync();
        if (auto status = startup_reclaim_flush(); !status) return status;
        if (auto status = startup_reclaim_write(first, row.wal.journal_head); !status) return status;
        state.prefix.head = StartupReclaimHead::active; startup_reclaim_sync();
        if (auto status = startup_reclaim_flush(); !status) return status;
        if (auto status = startup_reclaim_physical(); !status) return status;
    }
    if (!(state.prefix.home_after_mask & 1))
    {
        if (auto status = startup_reclaim_write(plan.targets[0], plan.after[0]); !status) return status;
        state.prefix.home_after_mask |= 1; startup_reclaim_sync();
    }
    if (auto status = startup_reclaim_flush(); !status) return status;
    if (auto status = startup_reclaim_physical(); !status) return status;
    for (usize index = 1; index + 1 < plan.count; ++index)
        if (!(state.prefix.home_after_mask & (1U << index)))
        {
            if (auto status = startup_reclaim_write(plan.targets[index], plan.after[index]); !status) return status;
            state.prefix.home_after_mask |= u8(1U << index); startup_reclaim_sync();
        }
    if (auto status = startup_reclaim_flush(); !status) return status;
    if (auto status = startup_reclaim_physical(); !status) return status;
    const usize last = plan.count - 1;
    if (!(state.prefix.home_after_mask & (1U << last)))
    {
        if (auto status = startup_reclaim_write(plan.targets[last], plan.after[last]); !status) return status;
        state.prefix.home_after_mask |= u8(1U << last); startup_reclaim_sync();
    }
    if (auto status = startup_reclaim_flush(); !status) return status;
    if (auto status = startup_reclaim_physical(); !status) return status;
    if (auto status = startup_reclaim_write(first, row.cleanup); !status) return status;
    state.prefix.head = StartupReclaimHead::cleanup; startup_reclaim_sync();
    if (auto status = startup_reclaim_flush(); !status) return status;
    if (auto status = startup_reclaim_physical(); !status) return status;
    {
        smp::ScopedSpinLock cache_guard(cache_.lock_);
        if (auto status = prepared(true); !status) return status;
        sealed_[0] = row.cleanup; publish(sealed_); state.publications = 2; startup_reclaim_sync();
    }
    if (auto status = startup_reclaim_guard(); !status) return status;
    if (auto status = retire(); !status) return status;
    if (!startup_reclaim_ledger_.idle() || reclaim_io_.pending_writes)
        return Status::fault("startup active reclaim drained row retains I/O");
    {
        smp::ScopedSpinLock owner_guard(lock_);
        ++startup_report_.reclaimed; startup_report_.released_blocks += plan.released_blocks;
        ++startup_report_.verified_publications; startup_report_.final_sequence = row.cleanup_sequence;
        startup_report_.observed = reclaim_io_; report_.phase = Ext4OwnedPhase::checkpointed;
    }
    state.prefix = {state.prefix.completed_rows + 1, 0, 0, StartupReclaimHead::predecessor};
    state.attachment_generation = startup_reclaim_seal_.origin_generation + state.prefix.completed_rows;
    state.transaction = 0; state.pinned = state.publications = 0; state.capture_ready = state.plan_ready = false;
    state.slots = {}; state.generations = {};
    participant_count_ = 0; state.phase = StartupReclaimPhase::drained; startup_reclaim_sync();
    if (auto status = startup_reclaim_guard(); !status) return status;
    // Rearm only drained operation handles. The permanent origin, ledger,
    // Session, cache versions and attachment generation stay in this Owner.
    if (auto status = startup_rearm(); !status) return status;
    { smp::ScopedSpinLock owner_guard(lock_); report_.phase = Ext4OwnedPhase::preparing; }
    return startup_reclaim_guard();
}
[[nodiscard]] Status startup_reclaim_recover()
{
    if (auto status = startup_reclaim_mint(); !status) return status;
    while (startup_reclaim_seal_.control.prefix.completed_rows < startup_reclaim_seal_.rows)
        if (auto status = startup_reclaim_checkpoint_row(); !status) return status;
    if (auto status = startup_reclaim_physical(); !status) return status;
    startup_reclaim_seal_.control.phase = StartupReclaimPhase::admitting; startup_reclaim_sync();
    { smp::ScopedSpinLock owner_guard(lock_); startup_report_.phase = Ext4StartupPhase::admitting; }
    return startup_reclaim_guard();
}
StartupReclaimSeal startup_reclaim_seal_{};
reclaim_history::SessionAnchor<typename Registry::Session> startup_reclaim_anchor_{};
StartupReclaimLedger startup_reclaim_ledger_{*this};
