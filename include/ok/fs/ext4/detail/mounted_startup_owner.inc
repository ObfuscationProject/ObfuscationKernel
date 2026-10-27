// Private guarded C1 port into this continuing canonical Owner.
    [[nodiscard]] Status startup_prepare(usize member, u32 expected_head)
    {
        if (!startup_workspace_ || (startup_workspace_->claimant_ != startup_mount_ || startup_workspace_->owner_ != this))
            return Status::fault("empty orphan original workspace claim differs");
        if (auto status = reclaim_session_exact(); !status) return status;
        auto &capture = reclaim_capture_;
        auto &storage = startup_workspace_->original_;
        reclaim_storage_ = &storage; reclaim_scratch_ = &startup_workspace_->preparation_;
        if (auto status = with_capture_read_budget([&](auto &guard) { return capture.capture(cache_.session_, storage, startup_workspace_->preparation_, guard); }); !status) return status;
        const auto captured = capture.report();
        if (!exact_session(captured.original_io, reclaim_io_) ||
            !captured.valid || captured.chain_members == 0 || captured.chain_members > 8 ||
            (member && (captured.head != expected_head || captured.chain_members != startup_report_.chain_members - member)))
            return Status::fault("empty orphan captured original identity differs");
        if (!member)
        {
            if (startup_report_.initial_family == Ext4StartupFamily::namespace_active)
            {
                if (auto status = startup_namespace_first_c1_exact(); !status) return status;
            }
            else if (storage.superblock != startup_workspace_->locator_super_ ||
                storage.descriptors != startup_workspace_->locator_descriptors_ ||
                storage.journal != startup_workspace_->locator_head_ ||
                storage.inode_tables[0][1] != startup_workspace_->locator_table_)
                return Status::fault("startup initial sources differ from checked classifier");
            // Every retained member must fit the writable route before any earlier member is published.
            Ext4OrphanChainProof complete{};
            if (auto status = Ext4OrphanChainValidator::validate(capture.binding_, capture.source().ownership.chain, complete); !status)
                return status;
            if (complete.count() != captured.chain_members)
                return Status::fault("startup complete chain writable profile differs");
            for (usize index = 0; index < captured.chain_members; ++index)
            {
                const auto &entry = storage.chain[index];
                const auto *mapped = complete.at(index);
                if (!mapped || mapped->number != entry.number || mapped->generation != entry.expected_generation)
                    return Status::fault("startup complete chain writable identity differs");
                if (entry.expected_generation == ~u32{0})
                    return Status::overflow("startup complete chain generation reservation exhausted");
                const auto inode = std::span<const u8>{entry.table}.subspan(((entry.number - 1) % 4) * 256, 256);
                const u32 seconds = ext4_checked::le32(inode, 12), extra = ext4_checked::le32(inode, 132);
                if (seconds > 0x7fffffffU || (extra & 3) || (extra >> 2) >= 1000000000U)
                    return Status::unsupported("startup complete chain ctime outside journal profile");
                const u32 data_group = mapped->data_count ? (mapped->data_blocks[0] - 1) / 8192 : 0;
                for (usize data = 0; data < mapped->data_count; ++data)
                    if ((mapped->data_blocks[data] - 1) / 8192 != data_group)
                        return Status::unsupported("startup complete chain member spans data allocation groups");
            }
            smp::ScopedSpinLock guard(lock_);
            if (capture.binding_.journal_sequence > ~u32{0} - u32(captured.chain_members * 3))
                return Status::overflow("startup complete chain sequence reservation exhausted");
            if (startup_report_.initial_family == Ext4StartupFamily::empty_orphan) startup_report_.initial_head = captured.head;
            startup_report_.chain_members = captured.chain_members;
            startup_initial_uuid_ = capture.binding_.uuid;
            for (usize index = 0; index < captured.chain_members; ++index)
                startup_initial_members_[index] = {storage.chain[index].number, storage.chain[index].expected_generation};
        }
        if (capture.binding_.uuid != startup_initial_uuid_)
            return Status::fault("empty orphan initial filesystem identity differs");
        for (usize index = 0; index < captured.chain_members; ++index)
            if (storage.chain[index].number != startup_initial_members_[member + index][0] ||
                storage.chain[index].expected_generation != startup_initial_members_[member + index][1])
                return Status::fault("empty orphan initial remaining chain identity differs");
        if (member)
        {
            // The previous actual all-after capture is retained independently.
            // A new valid filesystem history cannot replace our own checkpoint.
            const auto &expected = startup_workspace_->observed_;
            if (storage.superblock != expected.superblock || storage.descriptors != expected.descriptors ||
                storage.inode_bitmaps != expected.inode_bitmaps || storage.block_bitmaps != expected.block_bitmaps ||
                storage.journal != startup_expected_empty_head_ || capture.directory_count_ != startup_expected_directory_count_)
                return Status::fault("empty orphan next complete sources differ from own checkpoint");
            for (usize group = 0; group < 4; ++group)
                if (capture.initialized_[group] && storage.inode_tables[group] != expected.tables[group])
                    return Status::fault("empty orphan next whole tables differ from own checkpoint");
            for (usize index = 0; index < capture.directory_count_; ++index)
            {
                const auto &a = storage.directories[index], &b = expected.directories[index];
                if (a.inode != b.inode || a.logical != b.logical || a.physical != b.physical || a.bytes != b.bytes)
                    return Status::fault("empty orphan next directories differ from own checkpoint");
            }
        }
        if (auto status = with_capture_read_budget([&](auto &guard) { return capture.plan_head(captured.head, captured.generation, reclaim_plan_, guard); }); !status) return status;
        const auto &homes = reclaim_plan_.reclaim;
        const usize inode = ((homes.inode - 1) % 4) * 256;
        const u32 seconds = ext4_checked::le32(homes.before[0], inode + 12);
        const u32 extra = ext4_checked::le32(homes.before[0], inode + 132);
        if (seconds > 0x7fffffffU || (extra & 3) || (extra >> 2) >= 1000000000U)
            return Status::unsupported("empty orphan checked ctime outside journal profile");
        if (auto status = Ext4OrphanReclaimJournalPlanner::plan(capture.binding_, homes,
            storage.journal, seconds, extra >> 2, reclaim_format_, reclaim_journal_); !status) return status;
        Status status;
        { smp::ScopedSpinLock guard(cache_.lock_); status = reconcile_orphan_cache(); }
        if (!status) return status;
        std::array<u64, 16> released{};
        for (usize index = 0; index < homes.released_blocks; ++index) released[index] = homes.released_mapping[index];
        if (auto retired = cache_.retire_clean_blocks(std::span<const u64>{released}.first(homes.released_blocks)); !retired)
            return retired.status();
        participant_count_ = homes.count + 1;
        blocks_[0] = capture.binding_.layout.journal_first_block;
        sealed_[0] = reclaim_journal_.journal_head;
        for (usize index = 0; index < homes.count; ++index)
        { blocks_[index + 1] = homes.targets[index]; sealed_[index + 1] = homes.after[index]; }
        for (usize index = 0; index < participant_count_; ++index)
        {
            if (status = capture.stable(); !status) return status;
            const auto before = cache_.session_.report(); if (!before) return before.status();
            if (status = cache_.pin(blocks_[index], pins_[index]); !status) return status;
            const auto after = cache_.session_.report(); if (!after) return after.status();
            if (after.value().reads < before.value().reads || after.value().reads - before.value().reads > 1 ||
                after.value().completed_reads != after.value().reads ||
                capture.report_.session_reads > ~block::SessionCounter{0} - (after.value().reads - before.value().reads))
                return Status::fault("empty orphan canonical pin exact read accounting");
            capture.report_.session_reads += after.value().reads - before.value().reads;
            if (status = capture.stable(); !status) return status;
            const auto &old = index ? homes.before[index - 1] : storage.journal;
            {
                smp::ScopedSpinLock guard(cache_.lock_);
                const auto *entry = cache_.find(pins_[index]);
                if (!entry || entry->report.block != blocks_[index] ||
                    entry->report.state != MetadataBufferState::ready || entry->report.staged_dirty)
                    return Status::fault("empty orphan canonical before identity differs");
                for (usize byte = 0; byte < 1024; ++byte)
                    if (entry->canonical[byte] != std::byte(old[byte]))
                        return Status::fault("empty orphan canonical before whole bytes differ");
            }
        }
        std::array<Pin *, participant_capacity> pins{};
        for (usize index = 0; index < participant_count_; ++index) pins[index] = &pins_[index];
        if (status = cache_.begin(std::span<Pin *const>{pins}.first(participant_count_), transaction_); !status)
            return status;
        for (usize index = 0; index < participant_count_; ++index)
            if (status = transaction_.edit(blocks_[index], 0,
                std::as_bytes(std::span{sealed_[index]})); !status) return status;
        if (status = with_capture_read_budget([&](auto &guard) { return capture.plan_head(captured.head, captured.generation, reclaim_check_, guard); }); !status) return status;
        if (!reclaim_check_.valid || !same_reclaim(reclaim_check_.reclaim, homes))
            return Status::fault("empty orphan final whole preparation differs");
        { smp::ScopedSpinLock guard(cache_.lock_); status = prepared(true); }
        if (!status) return status;
        if (!member && startup_report_.initial_family == Ext4StartupFamily::namespace_active)
            if (auto exact = startup_namespace_first_c1_exact(); !exact) return exact;
        startup_workspace_->sealed_original_ = storage;
        startup_seal_.owner = this; startup_seal_.canonical_owner = this; startup_seal_.cache = &cache_;
        startup_seal_.session = &cache_.session_; startup_seal_.workspace = startup_workspace_; startup_seal_.storage = &storage;
        const auto cache = cache_.report();
        startup_seal_.identity = cache.device_identity; startup_seal_.registration = cache.registration_generation;
        startup_seal_.session_serial = cache.session_serial; startup_seal_.first_sector = cache.first_sector; startup_seal_.sectors = cache.sectors;
        startup_seal_.transaction = transaction_.serial_;
        for (usize index = 0; index < participant_count_; ++index)
        { startup_seal_.slots[index] = pins_[index].slot_; startup_seal_.generations[index] = pins_[index].generation_; }
        startup_seal_.domain = startup_generation_; startup_seal_.head = captured.head; startup_seal_.generation = captured.generation;
        startup_seal_.sequence = reclaim_journal_.sequence; startup_seal_.next_head = homes.next_head;
        startup_seal_.chain_members = captured.chain_members; startup_seal_.uuid = capture.binding_.uuid; startup_seal_.active = true;
        reclaim_prepared_ = reclaim_active_ = true;
        // No runtime Ticket, lifetime record, or claimant is fabricated.
        reclaim_io_ = capture.report_.original_io;
        if (reclaim_io_.reads > ~block::SessionCounter{0} - capture.report_.session_reads)
            return Status::overflow("empty orphan original read counter exhausted");
        reclaim_io_.reads += capture.report_.session_reads;
        reclaim_io_.completed_reads = reclaim_io_.reads;
        if (auto exact = reclaim_session_exact(); !exact) return exact;
        return startup_guard();
    }
    [[nodiscard]] bool startup_storage_exact() const
    {
        if (!startup_workspace_) return false;
        const auto &a = startup_workspace_->original_, &b = startup_workspace_->sealed_original_;
        if (a.superblock != b.superblock || a.descriptors != b.descriptors || a.journal != b.journal ||
            a.inode_bitmaps != b.inode_bitmaps || a.block_bitmaps != b.block_bitmaps || a.inode_tables != b.inode_tables)
            return false;
        for (usize index = 0; index < a.bootstrap_reads.size(); ++index)
            if (a.bootstrap_reads[index].physical != b.bootstrap_reads[index].physical ||
                a.bootstrap_reads[index].bytes != b.bootstrap_reads[index].bytes) return false;
        for (usize index = 0; index < a.chain.size(); ++index)
            if (a.chain[index].number != b.chain[index].number ||
                a.chain[index].expected_generation != b.chain[index].expected_generation ||
                a.chain[index].table_block != b.chain[index].table_block || a.chain[index].table != b.chain[index].table)
                return false;
        for (usize index = 0; index < a.directories.size(); ++index)
            if (a.directories[index].inode != b.directories[index].inode ||
                a.directories[index].logical != b.directories[index].logical ||
                a.directories[index].physical != b.directories[index].physical || a.directories[index].bytes != b.directories[index].bytes)
                return false;
        return true;
    }
    [[nodiscard]] Status startup_guard()
    {
        if (auto status = startup_domain_guard(); !status) return status;
        const auto &capture = reclaim_capture_; const auto &homes = reclaim_plan_.reclaim;
        const auto &journal = reclaim_journal_; const auto cache = cache_.report();
        if (!startup_seal_.active || startup_seal_.owner != this || startup_seal_.canonical_owner != this || startup_seal_.cache != &cache_ ||
            startup_seal_.session != &cache_.session_ || startup_seal_.workspace != startup_workspace_ || !startup_workspace_ ||
            (startup_workspace_->claimant_ != startup_mount_ || startup_workspace_->owner_ != this) || !startup_storage_exact() || startup_seal_.storage != &startup_workspace_->original_ ||
            reclaim_storage_ != startup_seal_.storage || reclaim_scratch_ != &startup_workspace_->preparation_ ||
            reclaim_lifetime_ || reclaim_ticket_.valid() ||
            startup_seal_.identity != cache.device_identity || startup_seal_.registration != cache.registration_generation ||
            startup_seal_.session_serial != cache.session_serial || startup_seal_.first_sector != cache.first_sector || startup_seal_.sectors != cache.sectors ||
            startup_seal_.domain != startup_generation_ || startup_seal_.head != capture.report_.head ||
            startup_seal_.generation != capture.report_.generation || startup_seal_.chain_members != capture.report_.chain_members ||
            startup_seal_.head != homes.inode || startup_seal_.generation != homes.old_generation || startup_seal_.next_head != homes.next_head ||
            startup_seal_.uuid != capture.binding_.uuid || startup_seal_.sequence != journal.sequence ||
            capture.binding_.journal_sequence == ~u32{0} || capture.binding_.journal_sequence + 1 != journal.sequence ||
            !capture.report_.valid || !reclaim_plan_.valid || !journal.ready || !reclaim_prepared_ ||
            !reclaim_active_ || homes.count < 4 || homes.count > 5 || journal.count != homes.count ||
            participant_count_ != homes.count + 1 || blocks_[0] != capture.binding_.layout.journal_first_block ||
            sealed_[0] != journal.journal_head)
            return Status::fault("empty orphan private cold authority differs");
        for (usize index = 0; index < homes.count; ++index)
            if (blocks_[index + 1] != homes.targets[index] || journal.targets[index] != homes.targets[index] ||
                sealed_[index + 1] != homes.after[index])
                return Status::fault("empty orphan original canonical whole homes differ");
        smp::ScopedSpinLock guard(cache_.lock_);
        if (startup_seal_.transaction != transaction_.serial_ || transaction_.serial_ != cache_.transaction_serial_)
            return Status::fault("startup original canonical transaction serial differs");
        for (usize index = 0; index < participant_count_; ++index)
            if (startup_seal_.slots[index] != pins_[index].slot_ ||
                startup_seal_.generations[index] > ~u64{0} - 2 ||
                startup_seal_.generations[index] + (reclaim_published_ ? 1 : 0) != pins_[index].generation_)
                return Status::fault("startup original canonical pin history differs");
        return reclaim_published_ ? authorized() : prepared(true);
    }
    [[nodiscard]] Status startup_write(u32 physical, const Block &bytes)
    {
        if (auto status = startup_guard(); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        auto &io = reclaim_io_;
        if (physical >= reclaim_capture_.binding_.layout.filesystem_blocks || io.writes == ~block::SessionCounter{0})
            return Status::overflow("empty orphan write bounds or counter exhausted");
        if (auto status = cache_.session_.write_blocks(u64{physical} * 2, std::as_bytes(std::span{bytes})); !status)
            return status;
        ++io.writes; ++io.completed_writes; io.pending_writes = true;
        if (auto status = reclaim_session_exact(); !status) return status;
        return startup_guard();
    }
    [[nodiscard]] Status startup_flush()
    {
        if (auto status = startup_guard(); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        auto &io = reclaim_io_;
        if (io.flushes == ~block::SessionCounter{0}) return Status::overflow("empty orphan flush counter exhausted");
        if (auto status = cache_.session_.flush(); !status) return status;
        ++io.flushes; ++io.completed_flushes; io.pending_writes = false;
        if (auto status = reclaim_session_exact(); !status) return status;
        return startup_guard();
    }
    template <typename Hook> requires block::ReadDispatchGuard<Hook, typename Registry::Session>
    [[nodiscard]] Status startup_compare(u32 physical, const Block &bytes, Hook &hook)
    {
        if (auto status = startup_guard(); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        auto &io = reclaim_io_;
        if (io.pending_writes || io.reads == ~block::SessionCounter{0}) return Status::overflow("empty orphan unflushed readback or counter");
        auto authority = [this]() -> Status { return startup_guard(); };
        if (auto status = guarded_ext4_direct_read(cache_.session_, io, authority,
            u64{physical} * 2, std::as_writable_bytes(std::span{reclaim_readback_}), hook); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        if (reclaim_readback_ != bytes) return Status::io_error("empty orphan durable whole readback differs");
        return startup_guard();
    }
    [[nodiscard]] Status startup_compare(u32 physical, const Block &bytes)
    { Ext4DirectReadHooks hook; return startup_compare(physical, bytes, hook); }
    [[nodiscard]] Status startup_compare_homes(usize after_count)
    {
        const auto &homes = reclaim_plan_.reclaim;
        for (usize index = 0; index < homes.count; ++index)
            if (auto status = startup_compare(homes.targets[index], index < after_count ? homes.after[index] : homes.before[index]); !status)
                return status;
        return {};
    }
    [[nodiscard]] Status startup_verify_device(bool after)
    {
        if (auto status = startup_guard(); !status) return status;
        if (auto status = reclaim_session_exact(); !status) return status;
        auto &disk = reclaim_disk_capture_;
        disk.~Ext4OrphanReclaimCapture<DeviceCapacity>();
        ::new (static_cast<void *>(&disk)) Ext4OrphanReclaimCapture<DeviceCapacity>;
        auto binding = reclaim_capture_.binding_;
        binding.empty_log = false; binding.journal_sequence = reclaim_journal_.sequence;
        auto &storage = startup_workspace_->observed_; auto &scratch = startup_workspace_->verification_;
    if (auto status = with_capture_read_budget([&](auto &guard) -> Status {
        return disk.capture(cache_.session_, binding, storage, scratch, guard);
    }); !status) return status;
        const auto &captured = disk.report(); auto &io = reclaim_io_;
        if (!exact_session(captured.original, io) || io.reads > ~block::SessionCounter{0} - captured.reads)
            return Status::fault("empty orphan actual journal original counters differ");
        io.reads += captured.reads; io.completed_reads += captured.reads;
        if (auto status = reclaim_session_exact(); !status) return status;
        const auto &semantic = captured.semantic; const auto &homes = reclaim_plan_.reclaim;
        const auto &journal = scratch.canonical; const auto &original = reclaim_journal_;
        if (!captured.valid || !semantic.valid || !semantic.complete_global_before_proof ||
            (after ? !semantic.all_after : !semantic.all_before) || semantic.inode != startup_seal_.head ||
            semantic.old_generation != startup_seal_.generation || semantic.active_sequence != startup_seal_.sequence ||
            semantic.parsed_next_sequence != startup_seal_.sequence + 1 || semantic.cleanup_sequence != startup_seal_.sequence + 2 ||
            !scratch.reclaim.valid || !same_reclaim(scratch.reclaim.reclaim, homes) ||
            !journal.ready || journal.sequence != original.sequence || journal.count != original.count ||
            journal.journal_head != original.journal_head || journal.descriptor != original.descriptor ||
            journal.commit != original.commit || journal.end != original.end)
            return Status::fault("empty orphan actual full cold proof differs from private seal");
        for (usize index = 0; index < homes.count; ++index)
            if (journal.targets[index] != original.targets[index] || journal.payloads[index] != original.payloads[index])
                return Status::fault("empty orphan actual journal payload differs");
        const auto &old = startup_workspace_->original_;
        if (scratch.superblock != old.superblock || scratch.descriptors != old.descriptors ||
            scratch.inode_bitmaps != old.inode_bitmaps || scratch.block_bitmaps != old.block_bitmaps)
            return Status::fault("empty orphan reconstructed complete before controls differ");
        for (usize group = 0; group < 4; ++group)
            if (reclaim_capture_.initialized_[group] && scratch.tables[group] != old.inode_tables[group])
                return Status::fault("empty orphan reconstructed full before inode tables differ");
        if (disk.directory_count_ != reclaim_capture_.directory_count_)
            return Status::fault("empty orphan complete directory count differs");
        for (usize index = 0; index < disk.directory_count_; ++index)
        {
            const auto &a = old.directories[index], &b = storage.directories[index];
            if (a.inode != b.inode || a.logical != b.logical || a.physical != b.physical || a.bytes != b.bytes)
                return Status::fault("empty orphan complete before directory differs");
        }
        reclaim_empty_head_ = original.journal_head;
        const auto be32 = [&](usize at, u32 value) {
            for (usize byte = 0; byte < 4; ++byte) reclaim_empty_head_[at + byte] = u8(value >> (24 - 8 * byte));
        };
        be32(24, semantic.cleanup_sequence); be32(28, 0); be32(252, 0);
        be32(252, jbd2::detail::crc32c(~u32{0}, reclaim_empty_head_));
        { smp::ScopedSpinLock guard(lock_); ++startup_report_.verified_publications; }
        return startup_guard();
    }
    [[nodiscard]] Status startup_publish()
    {
        if (auto status = startup_guard(); !status) return status;
        // Complete final before proof immediately precedes the first FLUSH.
        if (auto status = with_capture_read_budget([&](auto &guard) { return reclaim_capture_.plan_head(startup_seal_.head, startup_seal_.generation, reclaim_check_, guard); }); !status) return status;
        if (!reclaim_check_.valid || !same_reclaim(reclaim_check_.reclaim, reclaim_plan_.reclaim))
            return Status::fault("empty orphan publication final whole before proof differs");
        const auto &capture = reclaim_capture_.report_;
        reclaim_io_ = capture.original_io;
        if (reclaim_io_.reads > ~block::SessionCounter{0} - capture.session_reads)
            return Status::overflow("empty orphan final capture counter exhausted");
        reclaim_io_.reads += capture.session_reads; reclaim_io_.completed_reads = reclaim_io_.reads;
        const auto &j = reclaim_journal_;
        const u32 first = u32(reclaim_capture_.binding_.layout.journal_first_block);
        if (auto status = startup_flush(); !status) return status;
        if (auto status = startup_write(first + 1, j.descriptor); !status) return status;
        for (usize index = 0; index < j.count; ++index)
            if (auto status = startup_write(first + 2 + u32(index), j.payloads[index]); !status) return status;
        if (auto status = startup_write(first + 3 + u32(j.count), j.end); !status) return status;
        if (auto status = startup_flush(); !status) return status;
        if (auto status = startup_write(first + 2 + u32(j.count), j.commit); !status) return status;
        if (auto status = startup_flush(); !status) return status;
        if (auto status = startup_write(first, j.journal_head); !status) return status;
        if (auto status = startup_flush(); !status) return status;
        if (auto status = startup_verify_device(false); !status) return status;
        Status status;
        {
            smp::ScopedSpinLock guard(cache_.lock_);
            status = prepared(true);
            if (status) { reclaim_published_ = true; activate(); }
        }
        return status;
    }
    [[nodiscard]] Status startup_checkpoint()
    {
        if (auto status = startup_verify_device(false); !status) return status;
        const auto &homes = reclaim_plan_.reclaim;
        if (auto status = startup_write(homes.targets[0], homes.after[0]); !status) return status;
        if (auto status = startup_flush(); !status) return status;
        if (auto status = startup_compare_homes(1); !status) return status;
        for (usize index = 1; index + 1 < homes.count; ++index)
            if (auto status = startup_write(homes.targets[index], homes.after[index]); !status) return status;
        if (auto status = startup_flush(); !status) return status;
        if (auto status = startup_compare_homes(homes.count - 1); !status) return status;
        if (auto status = startup_write(homes.targets[homes.count - 1], homes.after[homes.count - 1]); !status) return status;
        if (auto status = startup_flush(); !status) return status;
        if (auto status = startup_compare_homes(homes.count); !status) return status;
        if (auto status = startup_verify_device(true); !status) return status;
        const u32 journal = u32(reclaim_capture_.binding_.layout.journal_first_block);
        if (auto status = startup_write(journal, reclaim_empty_head_); !status) return status;
        if (auto status = startup_flush(); !status) return status;
        if (auto status = startup_compare_homes(homes.count); !status) return status;
        if (auto status = startup_compare(journal, reclaim_empty_head_); !status) return status;
        if (!homes.next_head)
        {
            if (auto status = startup_workspace_->terminal_binding_.inspect(*startup_reader_, {0, cache_.session_.geometry().block_count}); !status)
                return status;
            const auto *actual = startup_workspace_->terminal_binding_.info(); const auto &old = reclaim_capture_.binding_;
            if (!actual || !actual->empty_log || !actual->needs_recovery || actual->uuid != startup_seal_.uuid ||
                actual->journal_sequence != startup_seal_.sequence + 2 || actual->layout.journal_first_block != old.layout.journal_first_block ||
                actual->layout.journal_blocks != old.layout.journal_blocks || actual->metadata_count != old.metadata_count)
                return Status::fault("empty orphan terminal ordinary binding differs");
            for (usize index = 0; index < old.metadata_count; ++index)
                if (actual->metadata[index].first != old.metadata[index].first || actual->metadata[index].count != old.metadata[index].count)
                    return Status::fault("empty orphan terminal metadata placement differs");
        }
        Status status;
        {
            smp::ScopedSpinLock guard(cache_.lock_);
            status = authorized();
            if (status)
            {
                sealed_[0] = reclaim_empty_head_;
                publish(sealed_); authority_.active = false;
            }
        }
        if (!status) return status;
        if (auto retired = retire(); !retired) return retired;
        if (auto exact = reclaim_session_exact(); !exact) return exact;
        reclaim_prepared_ = false; reclaim_completed_ = true; startup_seal_.active = false;
        return finish(Ext4OwnedPhase::checkpointed, {});
    }
