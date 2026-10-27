// Included in Ext4OwnedTransaction; distinct regular-move admission only.
[[nodiscard]] Status prepare_regular_move(Ext4RegularMoveUpdate update)
{
    if (!mounted_dirty_ || BufferCapacity < Ext4RegularMovePlan::capacity + 1)
        return Status::unsupported("owned regular move requires mounted-dirty six-buffer policy");
    {
        smp::ScopedSpinLock guard(lock_);
        if (startup_active_ || typed_public_blocked()) return Status::busy("owned startup domain excludes regular move");
        if (report_.phase != Ext4OwnedPhase::open) return phase_error();
        if (admissions_) return Status::busy("owned regular move reader admission in progress");
        const auto cache = cache_.report();
        if (cache.in_flight || cache.read_pins || cache.transaction_active)
            return Status::busy("owned regular move requires drained canonical readers");
        report_.phase = Ext4OwnedPhase::preparing;
    }
    regular_move_active_ = true;
    if (auto status = regular_move_writer_.prepare(cache_.session_, bounds_, update); !status) return reject(status);
    if (auto status = regular_move_writer_.logical_images(blocks_, sealed_, participant_count_); !status) return fail(status);
    return stage_prepared();
}
// Caller retains the actual exclusive continuing Session across cold admission
// and completion. This path never interprets OKMVW001 as a namespace witness.
[[nodiscard]] Status prepare_regular_move_recovery()
{
    if (!mounted_dirty_ || BufferCapacity < Ext4RegularMovePlan::capacity + 1)
        return Status::unsupported("owned cold regular move requires mounted-dirty six-buffer policy");
    {
        smp::ScopedSpinLock guard(lock_);
        if (startup_active_ || typed_public_blocked()) return Status::busy("owned startup domain excludes public cold move");
        if (report_.phase != Ext4OwnedPhase::open) return phase_error();
        if (admissions_) return Status::busy("owned cold regular move reader admission in progress");
        const auto cache = cache_.report();
        if (cache.in_flight || cache.read_pins || cache.transaction_active)
            return Status::busy("owned cold regular move requires drained canonical readers");
        report_.phase = Ext4OwnedPhase::preparing;
    }
    regular_move_active_ = regular_move_recovery_active_ = true;
    if (auto status = regular_move_completion_.prepare(cache_.session_, bounds_); !status) return reject(status);
    if (auto status = regular_move_completion_.logical_images(blocks_, sealed_, participant_count_); !status) return fail(status);
    return stage_prepared(true);
}
[[nodiscard]] Status recover_regular_move()
{
    if (auto status = prepare_regular_move_recovery(); !status) return status;
    return checkpoint();
}

private:
[[nodiscard]] const Block *regular_move_preimage(u64 block) const
{
    if (regular_move_recovery_active_) return regular_move_completion_.observed_preimage(block);
    const auto &writer = regular_move_writer_;
    if (writer.report().phase != Ext4TransactionPhase::preflight) return nullptr;
    if (const auto *home = writer.original_preimage(block)) return home;
    // The writer owns these whole ancillary inputs too. Match their physical
    // homes before any canonical pin can evict an older resident observation.
    if (block == 1) return &writer.super_;
    if (block == 2) return &writer.descriptors_;
    if (block == writer.journal_table_block_) return &writer.journal_table_;
    for (usize group = 0; group < 4; ++group)
    {
        if (block == writer.inode_bitmap_blocks_[group]) return &writer.inode_bitmaps_[group];
        if (block == writer.block_bitmap_blocks_[group]) return &writer.block_bitmaps_[group];
    }
    return nullptr;
}
// Examine every resident observed participant before even the first pin can
// evict one. Cold homes may be wholly old or new; use the exact sealed observed
// image, never a guessed old image or an all-after overlay.
[[nodiscard]] Status reconcile_regular_move_cache() const
{
    if (cache_.report_.state != MetadataOwnerState::ready || cache_.report_.in_flight ||
        cache_.report_.read_pins || cache_.report_.transaction_active || cache_.transaction_)
        return Status::busy("regular move candidate cache has retained work");
    for (const auto &entry : cache_.entries_)
    {
        if (entry.report.state != MetadataBufferState::ready) continue;
        const auto *before = regular_move_preimage(entry.report.block);
        if (!before) continue;
        if (entry.report.read_pins || entry.report.transaction_serial || entry.report.staged_dirty || !entry.report.generation)
            return Status::fault("regular move resident preimage identity differs");
        for (usize byte = 0; byte < before->size(); ++byte)
            if (entry.canonical[byte] != std::byte((*before)[byte]))
                return Status::fault("regular move resident preimage bytes differ");
    }
    return {};
}
[[nodiscard]] Status regular_move_pinned_preimages()
{
    for (usize index = 0; index < participant_count_; ++index)
    {
        const auto *entry = cache_.find(pins_[index]);
        const auto *before = regular_move_preimage(blocks_[index]);
        if (!entry || !before) return Status::fault("regular move pinned preimage identity absent");
        for (usize byte = 0; byte < before->size(); ++byte)
            if (entry->canonical[byte] != std::byte((*before)[byte]))
                return Status::fault("regular move pinned preimage bytes differ");
    }
    return {};
}
// The cache lock is held after retirement. The private successful completion
// retains its immutable physical proof until terminal startup verification.
[[nodiscard]] Status regular_move_terminal_cache_exact() const
{
    if (!regular_move_completion_.report().complete() || report_.phase != Ext4OwnedPhase::checkpointed ||
        authority_.active || transaction_.active_ || cache_.transaction_ || cache_.report_.transaction_active ||
        cache_.report_.in_flight || cache_.report_.read_pins || cache_.report_.state != MetadataOwnerState::ready)
        return Status::fault("regular move terminal cache is not drained");
    for (const auto &entry : cache_.entries_)
    {
        if (entry.report.state != MetadataBufferState::ready) continue;
        for (usize index = 0; index < participant_count_; ++index)
            if (entry.report.block == blocks_[index])
            {
                if (entry.report.read_pins || entry.report.transaction_serial || entry.report.staged_dirty || !entry.report.generation)
                    return Status::fault("regular move terminal resident identity differs");
                for (usize byte = 0; byte < 1024; ++byte)
                    if (entry.canonical[byte] != std::byte(sealed_[index][byte]))
                        return Status::fault("regular move terminal resident image differs");
            }
    }
    return {};
}
public:
