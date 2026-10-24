// Read-only prospective result, retained independently from physical C1 capture.
[[nodiscard]] Status startup_namespace_records(const Ext4NamespaceResultHistory &history)
{
    if (auto status = startup_domain_guard(); !status) return status;
    for (usize index = 0; index < history.count_ + 4; ++index)
    {
        if (auto status = history.stable(); !status) return status;
        const u32 physical = history.journal_first_ + u32(index);
        const auto *expected = history.physical(physical);
        if (!expected) return Status::fault("namespace history original journal record absent");
        if (auto status = startup_read(physical, reclaim_readback_); !status) return status;
        if (reclaim_readback_ != *expected)
            return Status::io_error("namespace history original journal record changed");
        if (auto status = history.stable(); !status) return status;
    }
    return startup_domain_guard();
}
[[nodiscard]] Status startup_namespace_history()
{
    if (auto status = startup_domain_guard(); !status) return status;
    auto &w = *startup_workspace_;
    const auto &binding = w.active_namespace_;
    const auto *info = binding.info();
    if (!binding.ready() || !info || binding.device_ != startup_reader_ ||
        binding.super_ != w.locator_super_ || binding.descriptors_ != w.locator_descriptors_ ||
        binding.table_block_ != w.locator_table_ || binding.head_ != w.locator_head_ ||
        binding.descriptor_ != w.locator_descriptor_ || info->uuid != startup_locator_uuid_ ||
        info->layout.first_sector != bounds_.first_sector || binding.bounds_.first_sector != bounds_.first_sector ||
        binding.bounds_.sector_count != bounds_.sector_count || info->layout.block_size != 1024 ||
        info->layout.filesystem_blocks != 32768 || info->layout.journal_first_block != startup_journal_first_ ||
        info->layout.journal_blocks != startup_journal_blocks_)
        return Status::fault("namespace history differs from original startup locator");
    if (auto status = w.namespace_history_.prepare(binding); !status) return status;
    auto &history = w.namespace_history_;
    if (auto status = startup_namespace_records(history); !status) return status;
    const auto before = reclaim_io_;
    const auto captured_status = startup_namespace_capture_.capture_namespace_result(cache_.session_,
        w.namespace_result_, w.namespace_preparation_, history, w.namespace_result_seal_);
    const auto captured = startup_namespace_capture_.report();
    if (!exact_session(captured.original_io, before) || before.reads > ~block::SessionCounter{0} - captured.session_reads)
        return Status::fault("namespace prospective original Session shadow differs");
    // Direct physical reads are counted by capture, then folded into this shadow once.
    reclaim_io_.reads += captured.session_reads; reclaim_io_.completed_reads += captured.session_reads;
    if (!captured_status) return captured_status;
    if (auto status = reclaim_session_exact(); !status) return status;
    if (auto status = startup_domain_guard(); !status) return status;
    if (auto status = history.stable(); !status) return status;
    if (auto status = startup_namespace_capture_.namespace_storage_exact(); !status) return status;
    if (!captured.valid || !captured.prospective || captured.chain_members != 1 ||
        captured.head != history.proof_.removed_inode || captured.sequence != history.cleanup_sequence_ ||
        startup_namespace_capture_.binding_.uuid != history.active_info_.uuid ||
        w.namespace_result_.journal != history.empty_head_)
        return Status::fault("namespace prospective complete result identity differs");
    Ext4OrphanChainProof chain{};
    if (auto status = Ext4OrphanChainValidator::validate(startup_namespace_capture_.binding_,
        startup_namespace_capture_.source().ownership.chain, chain); !status) return status;
    if (chain.count() != captured.chain_members)
        return Status::fault("namespace prospective complete chain count differs");
    const auto *member = chain.at(0);
    const auto &entry = w.namespace_result_.chain[0];
    if (!member || member->number != history.proof_.removed_inode || member->number != entry.number ||
        member->generation != entry.expected_generation || member->next)
        return Status::fault("namespace prospective victim identity differs");
    if (member->generation == ~u32{0})
        return Status::overflow("namespace prospective victim generation exhausted");
    const auto inode = std::span<const u8>{entry.table}.subspan(((entry.number - 1) % 4) * 256, 256);
    const u32 seconds = ext4_checked::le32(inode, 12), extra = ext4_checked::le32(inode, 132);
    if (seconds > 0x7fffffffU || (extra & 3) || (extra >> 2) >= 1000000000U)
        return Status::unsupported("namespace prospective victim ctime outside journal profile");
    const u32 data_group = member->data_count ? (member->data_blocks[0] - 1) / 8192 : 0;
    for (usize index = 0; index < member->data_count; ++index)
        if ((member->data_blocks[index] - 1) / 8192 != data_group)
            return Status::unsupported("namespace prospective victim spans data allocation groups");
    if (auto status = startup_namespace_records(history); !status) return status;
    if (auto status = startup_namespace_capture_.namespace_storage_exact(); !status) return status;
    if (auto status = reclaim_session_exact(); !status) return status;
    if (auto status = startup_domain_guard(); !status) return status;
    smp::ScopedSpinLock guard(lock_);
    startup_report_.prospective_members = captured.chain_members;
    startup_report_.namespace_result_validated = true;
    startup_report_.observed = reclaim_io_;
    return {};
}
