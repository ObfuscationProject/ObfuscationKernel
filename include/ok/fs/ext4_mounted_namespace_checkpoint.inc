// Included privately in the continuing mounted Owner.
struct StartupNamespaceSeal final
{
    StartupNamespaceSeal() = default;
    StartupNamespaceSeal(const StartupNamespaceSeal &) = delete;
    StartupNamespaceSeal &operator=(const StartupNamespaceSeal &) = delete;
    const Ext4OwnedTransaction *owner{};
    const Cache *cache{};
    const typename Cache::Registry::Session *session{};
    const Ext4MountedStartupWorkspace *workspace{};
    const Ext4NamespaceResultHistory *history{};
    const Ext4NamespaceSnapshotSeal *result{};
    u64 identity{}, registration{}, serial{}, first_sector{}, sectors{}, domain{}, transaction{};
    std::array<usize, participant_capacity> slots{};
    std::array<u64, participant_capacity> generations{};
    u32 sequence{}, cleanup_sequence{};
    usize count{}, publications{};
    bool active{};
};
[[nodiscard]] Status startup_namespace_guard()
{
    if (auto status = startup_domain_guard(); !status) return status;
    const auto &seal = startup_namespace_seal_;
    const auto &w = *startup_workspace_; const auto &history = w.namespace_history_;
    const auto cache = cache_.report();
    if (!seal.active || seal.owner != this || seal.cache != &cache_ || seal.session != &cache_.session_ ||
        seal.workspace != startup_workspace_ || seal.history != &history || seal.result != &w.namespace_result_seal_ ||
        !seal.result->ready || !seal.result->terminal || !startup_report_.namespace_result_validated ||
        startup_report_.initial_family != Ext4StartupFamily::namespace_active ||
        startup_report_.phase != Ext4StartupPhase::namespace_checkpointing ||
        seal.identity != cache.device_identity || seal.registration != cache.registration_generation ||
        seal.serial != cache.session_serial || seal.first_sector != cache.first_sector || seal.sectors != cache.sectors ||
        seal.domain != startup_generation_ || seal.sequence != history.sequence_ ||
        seal.cleanup_sequence != history.cleanup_sequence_ || seal.count != history.count_ + 1 ||
        seal.count < 3 || seal.count > 6 || participant_count_ != seal.count || seal.publications > 2 ||
        startup_seal_.active || authority_.active || reclaim_active_ || reclaim_lifetime_ || reclaim_ticket_.valid())
        return Status::fault("startup namespace private authority differs");
    smp::ScopedSpinLock guard(cache_.lock_);
    if (report_.phase != Ext4OwnedPhase::preparing || admissions_ ||
        seal.transaction != transaction_.serial_ || transaction_.serial_ != cache_.transaction_serial_)
        return Status::fault("startup namespace canonical transaction differs");
    if (auto status = prepared(true); !status) return status;
    for (usize index = 0; index < seal.count; ++index)
    {
        const auto &pin = pins_[index]; const auto *entry = cache_.find(pin);
        const u64 block = index ? history.targets_[index - 1] : history.journal_first_;
        const Block &expected = index ? history.after_[index - 1] :
            (seal.publications == 2 ? history.empty_head_ : history.head_);
        const Block &canonical = seal.publications ? expected : (index ? history.current_[index - 1] : history.head_);
        if (!entry || seal.slots[index] != pin.slot_ || seal.generations[index] > ~u64{0} - 2 ||
            pin.generation_ != seal.generations[index] + seal.publications || blocks_[index] != block ||
            sealed_[index] != expected || entry->report.block != block)
            return Status::fault("startup namespace pin or sealed image differs");
        for (usize byte = 0; byte < 1024; ++byte)
            if (entry->canonical[byte] != std::byte(canonical[byte]))
                return Status::fault("startup namespace complete canonical image differs");
    }
    return {};
}
[[nodiscard]] Status startup_namespace_prepare()
{
    if (auto status = startup_domain_guard(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    auto &w = *startup_workspace_; const auto &history = w.namespace_history_;
    const auto cache = cache_.report();
    if (!w.namespace_result_seal_.ready || !w.namespace_result_seal_.terminal ||
        !startup_report_.namespace_result_validated || startup_report_.prospective_members != 1 ||
        startup_report_.namespace_checkpoints || startup_namespace_seal_.active || startup_namespace_drained_ ||
        participant_count_ || transaction_.active_ || authority_.active || admissions_ ||
        cache.in_flight || cache.read_pins || cache.transaction_active ||
        history.count_ < 2 || history.count_ > 5 || history.count_ + 1 > BufferCapacity)
        return Status::fault("startup namespace preparation profile differs");
    if (startup_generation_ == ~u64{0} || cache_.transaction_serial_ > ~u64{0} - 2 ||
        reclaim_io_.reads > ~block::SessionCounter{0} - 65536 || reclaim_io_.writes > ~block::SessionCounter{0} - 64 ||
        reclaim_io_.flushes > ~block::SessionCounter{0} - 32)
        return Status::overflow("startup namespace complete route reservation exhausted");
    {
        smp::ScopedSpinLock guard(cache_.lock_);
        for (const auto &entry : cache_.entries_)
            if (entry.report.generation > ~u64{0} - 8192)
                return Status::overflow("startup namespace complete cache generation reservation exhausted");
    }
    participant_count_ = history.count_ + 1;
    blocks_[0] = history.journal_first_; sealed_[0] = history.head_;
    for (usize index = 0; index < history.count_; ++index)
    { blocks_[index + 1] = history.targets_[index]; sealed_[index + 1] = history.after_[index]; }
    for (usize index = 0; index < participant_count_; ++index)
    {
        if (auto status = startup_domain_guard(); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        const auto before = reclaim_io_;
        if (auto status = cache_.pin(blocks_[index], pins_[index]); !status) return status;
        const auto after = cache_.session_.report(); if (!after) return after.status();
        if (after.value().reads < before.reads || after.value().reads - before.reads > 1 ||
            after.value().completed_reads != after.value().reads)
            return Status::fault("startup namespace exact canonical pin read delta differs");
        reclaim_io_.reads += after.value().reads - before.reads; reclaim_io_.completed_reads = reclaim_io_.reads;
        if (auto status = reclaim_session_exact(); !status) return status;
        if (auto status = startup_domain_guard(); !status) return status;
        smp::ScopedSpinLock guard(cache_.lock_);
        const auto *entry = cache_.find(pins_[index]);
        const auto &old = index ? history.current_[index - 1] : history.head_;
        if (!entry || entry->report.block != blocks_[index] || entry->report.state != MetadataBufferState::ready ||
            entry->report.staged_dirty || entry->report.transaction_serial)
            return Status::fault("startup namespace original canonical pin differs");
        for (usize byte = 0; byte < 1024; ++byte)
            if (entry->canonical[byte] != std::byte(old[byte]))
                return Status::fault("startup namespace original complete canonical bytes differ");
    }
    std::array<Pin *, participant_capacity> pins{};
    for (usize index = 0; index < participant_count_; ++index) pins[index] = &pins_[index];
    if (auto status = cache_.begin(std::span<Pin *const>{pins}.first(participant_count_), transaction_); !status) return status;
    for (usize index = 0; index < participant_count_; ++index)
        if (auto status = transaction_.edit(blocks_[index], 0, std::as_bytes(std::span{sealed_[index]})); !status) return status;
    auto &seal = startup_namespace_seal_;
    seal.owner = this; seal.cache = &cache_; seal.session = &cache_.session_; seal.workspace = startup_workspace_;
    seal.history = &history; seal.result = &w.namespace_result_seal_;
    seal.identity = cache.device_identity; seal.registration = cache.registration_generation; seal.serial = cache.session_serial;
    seal.first_sector = cache.first_sector; seal.sectors = cache.sectors; seal.domain = startup_generation_;
    seal.transaction = transaction_.serial_; seal.sequence = history.sequence_; seal.cleanup_sequence = history.cleanup_sequence_;
    seal.count = participant_count_;
    for (usize index = 0; index < participant_count_; ++index)
    { seal.slots[index] = pins_[index].slot_; seal.generations[index] = pins_[index].generation_; }
    seal.active = true;
    { smp::ScopedSpinLock guard(lock_); startup_report_.phase = Ext4StartupPhase::namespace_checkpointing; }
    return startup_namespace_guard();
}
[[nodiscard]] Status startup_namespace_write(u32 physical, const Block &bytes)
{
    if (auto status = startup_namespace_guard(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    const auto &history = startup_workspace_->namespace_history_;
    if (startup_namespace_seal_.publications != 1 || physical >= 32768 || reclaim_io_.writes == ~block::SessionCounter{0})
        return Status::fault("startup namespace write stage, bounds or counter differs");
    bool selected = physical == history.journal_first_ && bytes == history.empty_head_;
    for (usize index = 0; index < history.count_; ++index)
        if (physical == history.targets_[index] && bytes == history.after_[index] && !history.proof_.homes.home_after[index]) selected = true;
    if (!selected) return Status::denied("startup namespace write is not a sealed checkpoint action");
    if (auto status = cache_.session_.write_blocks(u64{physical} * 2, std::as_bytes(std::span{bytes})); !status) return status;
    ++reclaim_io_.writes; ++reclaim_io_.completed_writes; reclaim_io_.pending_writes = true;
    if (auto status = reclaim_session_exact(); !status) return status;
    return startup_namespace_guard();
}
[[nodiscard]] Status startup_namespace_flush()
{
    if (auto status = startup_namespace_guard(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    if (startup_namespace_seal_.publications != 1 || reclaim_io_.flushes == ~block::SessionCounter{0})
        return Status::fault("startup namespace flush stage or counter differs");
    if (auto status = cache_.session_.flush(); !status) return status;
    ++reclaim_io_.flushes; ++reclaim_io_.completed_flushes; reclaim_io_.pending_writes = false;
    if (auto status = reclaim_session_exact(); !status) return status;
    return startup_namespace_guard();
}
[[nodiscard]] Status startup_namespace_compare(u32 physical, const Block &expected)
{
    if (auto status = startup_namespace_guard(); !status) return status;
    if (auto status = startup_read(physical, reclaim_readback_); !status) return status;
    if (reclaim_readback_ != expected) return Status::io_error("startup namespace complete physical history differs");
    return startup_namespace_guard();
}
// Stage zero is original current media, one is after homes with active head,
// and two is the exact prospective terminal image. No physical overlay is used.
[[nodiscard]] Status startup_namespace_physical_result(usize stage)
{
    if (stage > 2) return Status::invalid_argument("startup namespace history stage");
    if (auto status = startup_namespace_guard(); !status) return status;
    const auto &history = startup_workspace_->namespace_history_;
    const auto &seal = startup_workspace_->namespace_result_seal_; const auto &old = seal.storage;
    const auto check = [&](u32 physical, const Block &result) -> Status {
        const Block *expected = &result;
        if (physical == history.journal_first_) expected = stage == 2 ? &history.empty_head_ : &history.head_;
        else if (!stage)
            for (usize index = 0; index < history.count_; ++index)
                if (physical == history.targets_[index]) expected = &history.current_[index];
        return startup_namespace_compare(physical, *expected);
    };
    if (auto status = check(1, old.superblock); !status) return status;
    if (auto status = check(2, old.descriptors); !status) return status;
    if (auto status = check(history.journal_first_, old.journal); !status) return status;
    for (usize group = 0; group < 4; ++group)
    {
        const auto &metadata = seal.binding.metadata;
        if (auto status = check(u32(metadata[group * 4 + 1].first), old.block_bitmaps[group]); !status) return status;
        if (auto status = check(u32(metadata[group * 4 + 2].first), old.inode_bitmaps[group]); !status) return status;
        if (seal.initialized[group])
            for (usize index = 0; index < 512; ++index)
                if (auto status = check(u32(metadata[group * 4 + 3].first + index), old.inode_tables[group][index]); !status) return status;
    }
    for (usize index = 0; index < seal.directory_count; ++index)
        if (auto status = check(old.directories[index].physical, old.directories[index].bytes); !status) return status;
    for (usize index = 0; index < seal.bootstrap_count; ++index)
        if (auto status = check(old.bootstrap_reads[index].physical, old.bootstrap_reads[index].bytes); !status) return status;
    for (usize index = 1; index < history.count_ + 4; ++index)
    {
        const u32 physical = history.journal_first_ + u32(index); const auto *expected = history.physical(physical);
        if (!expected) return Status::fault("startup namespace retained journal record absent");
        if (auto status = startup_namespace_compare(physical, *expected); !status) return status;
    }
    return startup_namespace_guard();
}
[[nodiscard]] Status startup_namespace_checkpoint()
{
    if (auto status = startup_namespace_prepare(); !status) return status;
    {
        smp::ScopedSpinLock guard(cache_.lock_);
        if (auto status = prepared(true); !status) return status;
        publish(sealed_); startup_namespace_seal_.publications = 1;
    }
    if (auto status = startup_namespace_physical_result(0); !status) return status;
    if (auto status = startup_namespace_flush(); !status) return status;
    const auto &history = startup_workspace_->namespace_history_;
    bool wrote{};
    for (usize index = 0; index < history.count_; ++index)
        if (!history.proof_.homes.home_after[index])
        {
            if (auto status = startup_namespace_write(history.targets_[index], history.after_[index]); !status) return status;
            wrote = true;
        }
    if (wrote) if (auto status = startup_namespace_flush(); !status) return status;
    if (auto status = startup_namespace_physical_result(1); !status) return status;
    if (auto status = startup_namespace_write(history.journal_first_, history.empty_head_); !status) return status;
    if (auto status = startup_namespace_flush(); !status) return status;
    if (auto status = startup_namespace_physical_result(2); !status) return status;
    {
        smp::ScopedSpinLock guard(cache_.lock_);
        if (auto status = prepared(true); !status) return status;
        sealed_[0] = history.empty_head_; publish(sealed_); startup_namespace_seal_.publications = 2;
    }
    if (auto status = startup_namespace_guard(); !status) return status;
    { smp::ScopedSpinLock guard(lock_); ++startup_report_.namespace_checkpoints; startup_report_.observed = reclaim_io_; }
    if (auto status = retire(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    if (auto status = startup_domain_guard(); !status) return status;
    const auto cache = cache_.report();
    if (authority_.active || admissions_ || transaction_.active_ || cache.transaction_active || cache.read_pins || cache.in_flight ||
        cache_.transaction_ || reclaim_io_.pending_writes)
        return Status::fault("startup namespace terminal handles are not drained");
    for (const auto &pin : pins_) if (pin.active_) return Status::fault("startup namespace terminal pin is retained");
    startup_namespace_seal_.active = false; startup_namespace_drained_ = true;
    const auto retained = reclaim_io_;
    if (auto status = finish(Ext4OwnedPhase::checkpointed, {}); !status) return status;
    if (auto status = startup_rearm(); !status) return status;
    reclaim_io_ = retained; ++startup_generation_;
    { smp::ScopedSpinLock guard(lock_); report_.phase = Ext4OwnedPhase::preparing; }
    if (auto status = reclaim_session_exact(); !status) return status;
    return startup_domain_guard();
}
[[nodiscard]] Status startup_namespace_first_c1_exact() const
{
    if (auto status = startup_domain_guard(); !status) return status;
    const auto &seal = startup_workspace_->namespace_result_seal_;
    const auto &capture = reclaim_capture_; const auto &report = capture.report_;
    if (!startup_namespace_drained_ || startup_namespace_seal_.active || startup_report_.namespace_checkpoints != 1 ||
        !seal.ready || !seal.terminal || !report.valid || report.prospective || report.phase != Ext4OrphanSnapshotPhase::admitted ||
        capture.session_ != &cache_.session_ || capture.storage_ != &startup_workspace_->original_ ||
        capture.scratch_ != &startup_workspace_->preparation_ ||
        !ext4_same_snapshot_storage(startup_workspace_->original_, seal.storage) ||
        !Ext4NamespaceResultHistory::same_info(capture.binding_, seal.binding) || capture.normalized_ != seal.normalized ||
        capture.initialized_ != seal.initialized || capture.chain_count_ != seal.chain_count ||
        capture.directory_count_ != seal.directory_count || capture.bootstrap_count_ != seal.bootstrap_count ||
        report.head != seal.report.head || report.generation != seal.report.generation || report.sequence != seal.report.sequence ||
        report.table_blocks != seal.report.table_blocks || report.directory_blocks != seal.report.directory_blocks ||
        report.chain_members != seal.report.chain_members || report.identity != seal.report.identity ||
        report.registration_generation != seal.report.registration_generation || report.session_serial != seal.report.session_serial)
        return Status::fault("startup first physical C1 capture differs from retained namespace result");
    return {};
}
StartupNamespaceSeal startup_namespace_seal_{};
bool startup_namespace_drained_{};
