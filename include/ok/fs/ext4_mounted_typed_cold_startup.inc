// Distinct private original cold-startup authority. No runtime Node exists
// until the finite suffix, original Receipt and one-use renewal have completed.
struct TypedColdOpening final
{
    TypedColdOpening(Ext4MountedMetadata &value, Ext4MountedStartupWorkspace &startup_storage,
        typename Owner::TypedOwnerWorkspace &typed_storage, const typename Owner::CommonSelection *common = nullptr)
        : mount(value), owner(value.owner_), session(value.owner_.typed_mounted_original_session()),
          startup(startup_storage), workspace(typed_storage), serial(value.identity_.session_serial), common_(common) {}
    TypedColdOpening(const TypedColdOpening &) = delete;
    TypedColdOpening &operator=(const TypedColdOpening &) = delete;
    TypedColdOpening(TypedColdOpening &&) = delete;
    TypedColdOpening &operator=(TypedColdOpening &&) = delete;
    Ext4MountedMetadata &mount;
    Owner &owner;
    typename Owner::TypedSession &session;
    Ext4MountedStartupWorkspace &startup;
    typename Owner::TypedOwnerWorkspace &workspace;
    const u64 serial;
    const typename Owner::CommonSelection *const common_;
};
const TypedColdOpening *typed_cold_opening_{};
// Original cold suffix owns no Node/context/Lease. Its continuing Attachment
// and exact local opening context remain held through root publication.
using TypedColdStage = Ext4MountedTypedSuffixStage;
using TypedColdPhase = Ext4MountedTypedCursorPhase;
using TypedColdSession = typename Owner::TypedSession;
struct TypedColdWorkspace;
struct TypedColdAuthority final
{
    Ext4MountedMetadata &mount;
    TypedColdWorkspace &workspace;
    [[nodiscard]] Status validate(TypedColdPhase phase, TypedColdStage stage)
    { return mount.typed_cold_validate(workspace, phase, stage); }
    [[nodiscard]] Status complete_stage(TypedColdStage stage)
    { return mount.typed_cold_complete_stage(workspace, stage); }
};
using TypedColdCursor = Ext4MountedTypedCursor<TypedColdSession, TypedColdAuthority>;
using TypedColdAttachment = typename Owner::template TypedMountedCursorAttachment<TypedColdCursor>;
struct TypedColdReader final
{
    Ext4MountedMetadata &mount;
    TypedColdWorkspace &workspace;
    [[nodiscard]] Status read_block(u32 block, std::span<u8> output)
    { return mount.typed_cold_read_block(workspace, block, output); }
};
using TypedColdView = Ext4ReadOnlyView<TypedColdReader>;
struct TypedColdDevice final : driver::BlockDevice
{
    Ext4MountedMetadata &mount;
    TypedColdWorkspace &workspace;
    TypedColdDevice(Ext4MountedMetadata &value, TypedColdWorkspace &storage) : mount(value), workspace(storage) {}
    [[nodiscard]] driver::BlockGeometry geometry() const override
    { return mount.typed_cold_geometry(workspace); }
    [[nodiscard]] Status read_blocks(u64 sector, std::span<std::byte> output) override
    { return mount.typed_cold_read_sector(workspace, sector, output); }
    [[nodiscard]] Status write_blocks(u64, std::span<const std::byte>) override
    { return Status::denied("typed suffix inspection cannot write"); }
    [[nodiscard]] Status flush() override
    { return Status::denied("typed suffix inspection cannot flush"); }
};
struct TypedColdWorkspace final
{
    TypedColdWorkspace(Ext4MountedMetadata &mount, const TypedColdOpening &original,
        typename Owner::TypedOwnerWorkspace &workspace)
        : mount_(mount), original_(original), owner_workspace_(workspace), session_(mount.owner_.typed_mounted_original_session()),
          authority_{mount, *this}, cursor_(session_),
          context_(session_, authority_, mount.owner_, mount, original.serial, &workspace,
              &Owner::template typed_mounted_bridge_thunk<TypedColdCursor>),
          attachment_(mount.owner_, workspace, cursor_, context_), reader_{mount, *this}, device_(mount, *this),
          inspection_(true, true) {}
    TypedColdWorkspace(const TypedColdWorkspace &) = delete;
    TypedColdWorkspace &operator=(const TypedColdWorkspace &) = delete;
    TypedColdWorkspace(TypedColdWorkspace &&) = delete;
    TypedColdWorkspace &operator=(TypedColdWorkspace &&) = delete;
    Ext4MountedMetadata &mount_;
    const TypedColdOpening &original_;
    typename Owner::TypedOwnerWorkspace &owner_workspace_;
    TypedColdSession &session_;
    TypedColdAuthority authority_;
    TypedColdCursor cursor_;
    typename TypedColdCursor::Context context_;
    TypedColdAttachment attachment_;
    TypedColdReader reader_;
    TypedColdDevice device_;
    TypedColdView view_{};
    Ext4ReadOnlyOpenScratch opening_{};
    Ext4RecoveryCompletion<6, 2> inspection_;
    typename TypedColdCursor::Receipt receipt_{};
    Ext4RecoveryInfo binding_{};
    std::array<u8, 1024> raw_{};
    Ext4ReadOnlyInode target_{}, root_{}, root_seal_{};
    bool root_checked_{};
    Metadata metadata_{};
    TypedColdStage executing_{TypedColdStage::none}, semantic_stage_{TypedColdStage::none};
    u16 semantic_mask_{};
    Status semantic_status_{};
    bool attempted_{}, finished_{};
};
[[nodiscard]] Status typed_cold_original_exact(const TypedColdWorkspace &work) const
{
    if (&work.mount_ != this || &work.original_ != typed_cold_opening_ ||
        &work.original_.mount != this || &work.original_.owner != &owner_ ||
        &work.original_.session != &owner_.typed_mounted_original_session() ||
        &work.original_.startup != startup_workspace_ ||
        &work.original_.workspace != &work.owner_workspace_ ||
        &work.session_ != &owner_.typed_mounted_original_session() ||
        &work.authority_.mount != this || &work.authority_.workspace != &work ||
        &work.reader_.mount != this || &work.reader_.workspace != &work ||
        &work.device_.mount != this || &work.device_.workspace != &work ||
        &work.attachment_.owner_ != &owner_ || &work.attachment_.workspace_ != &work.owner_workspace_ ||
        &work.attachment_.cursor_ != &work.cursor_ || &work.attachment_.context_ != &work.context_ ||
        policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime || !startup_workspace_)
        return Status::fault("typed cold original concrete storage differs");
    if (auto status = startup_opening(&owner_, 1); !status) return status;
    {
        smp::ScopedSpinLock guard(lock_);
        if (!report_.status || mutation_node_ || mutation_context_ || mutation_lease_ || pending_orphan_)
            return Status::fault("typed cold opening gained a mutation or orphan domain");
    }
    {
        smp::ScopedSpinLock guard(startup_workspace_->lock_);
        if (startup_workspace_->claimant_ != this || startup_workspace_->owner_ != &owner_)
            return Status::fault("typed cold permanent startup workspace claim differs");
    }
    if (owner_.publication_domain_ != 1 || owner_.startup_active_ || owner_.startup_route_opening_ ||
        work.original_.serial != identity_.session_serial)
        return Status::fault("typed cold original publication domain differs");
    if (work.original_.common_)
    {
        if (!owner_.common_opening_ || owner_.common_opening_->selection_ != work.original_.common_ ||
            owner_.common_opening_->phase_ != Owner::CommonOpeningPhase::typed_adopted)
            return Status::fault("typed common operation lost its original adopted Selection");
        if (auto status = owner_.common_retained_exact(); !status) return status;
    }
    return identity();
}
[[nodiscard]] Status typed_cold_validate(TypedColdWorkspace &work,
    TypedColdPhase phase, TypedColdStage stage)
{
    if (auto status = typed_cold_original_exact(work); !status) return status;
    if (phase == TypedColdPhase::suffix && stage != TypedColdStage::none && stage != work.executing_)
        return Status::fault("typed semantic suffix selected stage differs");
    if (phase == TypedColdPhase::closed && work.executing_ != TypedColdStage::none)
        return Status::fault("typed semantic suffix closed with a helper active");
    return {};
}
[[nodiscard]] Status typed_cold_stop(TypedColdWorkspace &work, Status status)
{
    if (status) status = Status::fault("typed semantic suffix stop requires error");
    {
        smp::ScopedSpinLock guard(lock_);
        if (report_.phase == Ext4MountPhase::failed && !report_.status) return report_.status;
    }
    static_cast<void>(owner_.typed_mounted_cursor_stop(work.attachment_, status));
    return fail(status);
}
[[nodiscard]] driver::BlockGeometry typed_cold_geometry(TypedColdWorkspace &work)
{
    if (auto status = typed_cold_original_exact(work); !status)
    { static_cast<void>(typed_cold_stop(work, status)); return {}; }
    if (auto status = owner_.typed_mounted_cursor_exact(work.attachment_); !status)
    { static_cast<void>(typed_cold_stop(work, status)); return {}; }
    // This is the original immutable native volume geometry, including writable
    // capability required by prepare(). Adapter mutation methods still refuse.
    const auto geometry = work.session_.geometry();
    if (auto status = typed_cold_original_exact(work); !status)
    { static_cast<void>(typed_cold_stop(work, status)); return {}; }
    return geometry;
}
[[nodiscard]] Status typed_cold_read_sector(TypedColdWorkspace &work, u64 sector,
    std::span<std::byte> output)
{
    if (work.executing_ == TypedColdStage::none || work.cursor_.control_.stage != work.executing_ || output.size() != 1024)
        return typed_cold_stop(work, Status::denied("typed semantic suffix lacks its selected 1 KiB helper"));
    if (auto status = typed_cold_original_exact(work); !status) return typed_cold_stop(work, status);
    const auto status = work.cursor_.read(work.context_, sector, output);
    if (!status) return status;
    if (auto exact = typed_cold_original_exact(work); !exact) return typed_cold_stop(work, exact);
    return {};
}
[[nodiscard]] Status typed_cold_read_block(TypedColdWorkspace &work, u32 block, std::span<u8> output)
{
    if (work.binding_.layout.block_size != 1024 || block >= work.binding_.layout.filesystem_blocks ||
        u64{block} > (~u64{0} - work.binding_.layout.first_sector) / 2)
        return typed_cold_stop(work, Status::invalid_argument("typed semantic suffix filesystem block bounds"));
    return typed_cold_read_sector(work, work.binding_.layout.first_sector + u64{block} * 2,
        std::as_writable_bytes(output));
}
template <typename ChildReader>
[[nodiscard]] static bool typed_cold_same_open_view(const TypedColdWorkspace &work,
    const Ext4ReadOnlyView<ChildReader> &child)
{
    const auto &fresh = work.view_;
    if (!fresh.valid() || fresh.reader_ != &work.reader_ || !fresh.attempted_ || !fresh.ready_ || fresh.failed_ ||
        fresh.layout_.first_sector != child.layout_.first_sector || fresh.layout_.filesystem_blocks != child.layout_.filesystem_blocks ||
        fresh.layout_.journal_first_block != child.layout_.journal_first_block || fresh.layout_.journal_blocks != child.layout_.journal_blocks ||
        fresh.layout_.block_size != child.layout_.block_size || fresh.uuid_ != child.uuid_ ||
        fresh.seed_ != child.seed_ || fresh.first_data_ != child.first_data_ ||
        fresh.blocks_per_group_ != child.blocks_per_group_ || fresh.inodes_per_group_ != child.inodes_per_group_ ||
        fresh.group_count_ != child.group_count_ || fresh.inodes_ != child.inodes_ ||
        fresh.first_inode_ != child.first_inode_ || fresh.table_blocks_ != child.table_blocks_) return false;
    for (usize at = 0; at < fresh.groups_.size(); ++at)
    {
        const auto &a = fresh.groups_[at]; const auto &b = child.groups_[at];
        if (a.block_bitmap != b.block_bitmap || a.inode_bitmap != b.inode_bitmap || a.table != b.table ||
            a.block_checksum != b.block_checksum || a.inode_checksum != b.inode_checksum || a.flags != b.flags) return false;
    }
    return true;
}
[[nodiscard]] static bool typed_cold_same_metadata(const Metadata &a, const Metadata &b)
{
    return a.type == b.type && a.mode == b.mode && a.uid == b.uid && a.gid == b.gid && a.size == b.size &&
        a.blocks == b.blocks && a.link_count == b.link_count && a.block_size == b.block_size &&
        a.attributes.modification.seconds == b.attributes.modification.seconds &&
        a.attributes.modification.nanoseconds == b.attributes.modification.nanoseconds &&
        a.attributes.change.seconds == b.attributes.change.seconds && a.attributes.change.nanoseconds == b.attributes.change.nanoseconds;
}
[[nodiscard]] static bool typed_cold_same_root(const Ext4ReadOnlyInode &a, const Ext4ReadOnlyInode &b)
{
    return a.number == b.number && a.generation == b.generation && a.size == b.size &&
        a.mode == b.mode && a.uid == b.uid && a.gid == b.gid && a.links == b.links;
}
template <typename ChildReader>
[[nodiscard]] Status typed_cold_result_exact(TypedColdWorkspace &work, TypedColdStage stage,
    const Ext4ReadOnlyView<ChildReader> &child, const Ext4ZeroTruncateOwnership &proof,
    const Ext4RecoveryInfo &binding, u32 table, const Ext4ZeroTruncateBlock &image,
    u32 sequence, bool allocated, bool noop)
{
    using namespace ext4_checked;
    const Ext4InodeLifetimeKey key{this, identity_.session_serial, proof.inode, proof.generation};
    if (!child.valid() || child.layout_.block_size != 1024 || proof.inode != key.inode || proof.generation != key.generation ||
        !child.inodes_per_group_ || key.inode > child.inodes_ || !key.inode ||
        binding.journal_sequence != sequence || !binding.empty_log || !binding.needs_recovery ||
        binding.metadata_count > binding.metadata.size())
        return Status::fault("typed semantic suffix actual child lineage differs");
    const u32 local = (key.inode - 1) % child.inodes_per_group_;
    const u32 group = (key.inode - 1) / child.inodes_per_group_;
    if (group >= child.group_count_ || table != child.groups_[group].table + local / 4)
        return Status::fault("typed semantic suffix actual child table differs");
    const auto expected = std::span<const u8>{image}.subspan((local % 4) * 256, 256);
    if (auto status = inode_checksum(child.seed_, key.inode, expected); !status) return status;
    if (le32(expected, 100) != key.generation || le32(expected, 4) != proof.old_size ||
        proof.count != (allocated ? 1U : 0U) || (!allocated && proof.old_size))
        return Status::fault("typed semantic suffix actual child target image differs");
    if (stage == TypedColdStage::inspect_empty)
    {
        const auto *actual = work.inspection_.binding();
        if (!actual || !work.inspection_.report().status || !work.inspection_.report().empty_log ||
            !reclaim_history::same_info(*actual, binding) || !reclaim_history::same_info(work.binding_, binding))
            return Status::fault("typed semantic suffix strict empty preparation differs");
        return {};
    }
    if (stage != TypedColdStage::noop_terminal && !typed_cold_same_open_view(work, child))
        return Status::fault("typed semantic suffix fresh checked view differs from actual child");
    if (stage == TypedColdStage::startup_root)
    {
        if (!work.root_checked_ || work.root_.number != 2 || !typed_cold_same_root(work.root_, work.root_seal_) ||
            (work.root_.mode & 0170000) != 0040000)
            return Status::fault("typed cold checked provisional root differs");
    }
    if (stage == TypedColdStage::target_inode)
    {
        const auto value = child.information(key.inode, expected);
        const auto &actual = work.target_;
        if (actual.number != value.number || actual.generation != value.generation || actual.size != value.size ||
            actual.mode != value.mode || actual.uid != value.uid || actual.gid != value.gid || actual.links != value.links)
            return Status::fault("typed semantic suffix checked target inode differs");
    }
    if (stage == TypedColdStage::raw_target || stage == TypedColdStage::final_wrapper || stage == TypedColdStage::noop_terminal)
    {
        if (noop != (stage == TypedColdStage::noop_terminal))
            return Status::fault("typed semantic suffix no-op stage selection differs");
        const auto actual = std::span<const u8>{work.raw_}.subspan((local % 4) * 256, 256);
        if (auto status = inode_checksum(child.seed_, key.inode, actual); !status) return status;
        if (le32(actual, 100) != key.generation || !std::equal(actual.begin(), actual.end(), expected.begin()))
            return Status::io_error("typed semantic suffix physical target bytes differ from authentic child");
    }
    if (stage == TypedColdStage::target_metadata)
    {
        auto value = child.project_metadata(key.inode, expected);
        if (auto status = decode_ext4_times(std::as_bytes(expected), value.attributes); !status) return status;
        if (!typed_cold_same_metadata(work.metadata_, value))
            return Status::fault("typed semantic suffix checked metadata differs from authentic child image");
    }
    return {};
}
[[nodiscard]] Status typed_cold_complete_stage(TypedColdWorkspace &work, TypedColdStage stage)
{
    if (auto status = typed_cold_original_exact(work); !status) return status;
    if (stage == TypedColdStage::none || work.executing_ != stage || work.semantic_stage_ != stage ||
        !work.semantic_status_ || !(work.semantic_mask_ & (u16{1} << static_cast<u8>(stage))))
        return Status::fault("typed semantic suffix helper success was not retained");
    auto verify = [&](const auto &view, const auto &proof, const auto &binding, u32 table,
        const auto &image, u32 sequence, bool allocated, bool noop) -> Status {
        return typed_cold_result_exact(work, stage, view, proof, binding, table, image, sequence, allocated, noop);
    };
    const auto status = owner_.typed_mounted_visit_lineage(work.attachment_, verify);
    if (auto exact = typed_cold_original_exact(work); !exact) return exact;
    return status;
}
[[nodiscard]] Status typed_cold_execute_stage(TypedColdWorkspace &work, TypedColdStage stage)
{
    work.executing_ = stage;
    if (auto status = work.cursor_.begin_stage(work.context_, stage); !status) return status;
    auto execute = [&](const auto &child, const auto &proof, const Ext4RecoveryInfo &binding, u32 table,
        const auto &image, u32 sequence, bool allocated, bool noop) -> Status {
        if (auto status = typed_cold_original_exact(work); !status) return status;
        Status status;
        switch (stage)
        {
        case TypedColdStage::inspect_empty:
            status = work.inspection_.prepare_typed_empty(work.device_, owner_.bounds_);
            if (status) work.binding_ = *work.inspection_.binding();
            break;
        case TypedColdStage::reopen_view: status = work.view_.open(work.reader_, work.binding_, work.opening_); break;
        case TypedColdStage::stored_metadata: status = work.view_.validate_stored_metadata(); break;
        case TypedColdStage::target_inode:
        {
            auto result = work.view_.inode(proof.inode);
            status = result ? Status{} : result.status(); if (result) work.target_ = result.value(); break;
        }
        case TypedColdStage::raw_target: case TypedColdStage::final_wrapper: case TypedColdStage::noop_terminal:
            work.binding_ = binding; status = typed_cold_read_block(work, table, work.raw_); break;
        case TypedColdStage::target_metadata:
        {
            auto result = work.view_.query_stored_metadata(proof.inode);
            status = result ? Status{} : result.status(); if (result) work.metadata_ = result.value(); break;
        }
        case TypedColdStage::startup_root:
        {
            auto result = work.view_.inode(2);
            status = result ? Status{} : result.status();
            if (result) { work.root_ = work.root_seal_ = result.value(); work.root_checked_ = true; }
            break;
        }
        default: return Status::unsupported("typed cold suffix stage is outside original startup domain");
        }
        if (!status) return status;
        if (auto exact = typed_cold_original_exact(work); !exact) return exact;
        return typed_cold_result_exact(work, stage, child, proof, binding, table, image, sequence, allocated, noop);
    };
    const auto status = owner_.typed_mounted_visit_lineage(work.attachment_, execute);
    work.semantic_status_ = status;
    if (!status) return typed_cold_stop(work, status);
    work.semantic_stage_ = stage; work.semantic_mask_ |= u16{1} << static_cast<u8>(stage);
    if (auto ended = work.cursor_.end_stage(work.context_); !ended) return typed_cold_stop(work, ended);
    work.executing_ = TypedColdStage::none;
    return {};
}
// The actual child bridge precedes every cold semantic READ. Consuming the
// Receipt leaves Owner exclusion intact until the genuine renewal and root commit.
[[nodiscard]] Status typed_run_cold_suffix(TypedColdWorkspace &work)
{
    if (work.attempted_) return Status::already_exists("typed semantic suffix is one-shot");
    work.attempted_ = true;
    if (auto status = typed_cold_original_exact(work); !status) return typed_cold_stop(work, status);
    if (work.cursor_.origin_ != Ext4MountedTypedOrigin::startup)
        return typed_cold_stop(work, Status::unsupported("typed cold suffix requires its original startup origin"));
    if (auto status = owner_.typed_mounted_cursor_exact(work.attachment_); !status) return typed_cold_stop(work, status);
    for (auto stage : {TypedColdStage::inspect_empty, TypedColdStage::reopen_view,
        TypedColdStage::stored_metadata, TypedColdStage::target_inode, TypedColdStage::raw_target,
        TypedColdStage::target_metadata, TypedColdStage::final_wrapper, TypedColdStage::startup_root})
        if (auto status = typed_cold_execute_stage(work, stage); !status) return status;
    if (auto status = typed_cold_original_exact(work); !status) return typed_cold_stop(work, status);
    if (auto status = work.cursor_.close_suffix(work.context_, work.receipt_); !status) return typed_cold_stop(work, status);
    if (auto status = owner_.typed_accept_mounted_cursor_receipt(work.attachment_, work.receipt_); !status)
        return typed_cold_stop(work, status);
    if (auto status = typed_cold_original_exact(work); !status) return typed_cold_stop(work, status);
    work.finished_ = true;
    return {};
}

[[nodiscard]] static bool typed_cold_workspace_unused(const typename Owner::TypedOwnerWorkspace &workspace)
{
    return !workspace.original_ && !workspace.owner_ && !workspace.cache_ && !workspace.session_ &&
        workspace.phase_ == Owner::TypedOwnerPhase::unused && workspace.phase_seal_ == Owner::TypedOwnerPhase::unused &&
        !workspace.operation_serial_ && !workspace.operation_serial_seal_ &&
        !workspace.handoff_serial_ && !workspace.handoff_serial_seal_ && !workspace.attempted_ &&
        !workspace.consumed_ && !workspace.renewal_quarantined_ && !workspace.allocated_route_ && !workspace.recovery_;
}
// This admission is distinct from recover_startup(): no legacy locator budget
// is opened or reset. The continuing actual Attachment owns public exclusion.
[[nodiscard]] Status typed_admit_cold_cursor(TypedColdWorkspace &work)
{
    if (auto status = typed_cold_original_exact(work); !status) return status;
    const auto initial = work.session_.report();
    if (!initial) return initial.status();
    const auto &s = initial.value();
    if (s.reads || s.completed_reads || s.writes || s.completed_writes ||
        s.flushes || s.completed_flushes || s.in_flight || s.pending_writes ||
        work.context_.serial_ != work.original_.serial || work.cursor_.attempted_ ||
        work.attempted_ || !typed_cold_workspace_unused(work.owner_workspace_))
        return Status::busy("typed cold admission requires its untouched original Session");
    const auto overlaps = [](const void *a, usize na, const void *b, usize nb) {
        const auto x = reinterpret_cast<usize>(a), y = reinterpret_cast<usize>(b);
        return x <= y ? y - x < na : x - y < nb;
    };
    if (overlaps(&work, sizeof(work), this, sizeof(*this)) ||
        overlaps(&work, sizeof(work), startup_workspace_, sizeof(*startup_workspace_)) ||
        overlaps(&work, sizeof(work), &work.owner_workspace_, sizeof(work.owner_workspace_)))
        return Status::invalid_argument("typed cold operation storage overlaps original borrows");
    // General admission still refuses every active legacy startup domain.
    if (auto status = owner_.typed_admit_mounted_cursor(work.attachment_); !status) return status;
    // Union reservation authenticates all six real counters before READ1.
    const auto status = work.cursor_.reserve(work.context_, 4, 32768,
        Ext4MountedTypedOrigin::startup, Ext4MountedTypedOpening::startup_recovery_family);
    return status ? Status{} : typed_cold_stop(work, status);
}
template <typename Read>
[[nodiscard]] Status typed_classify_cold_sources(TypedColdWorkspace &work, Read &read)
{
    using namespace ext4_checked;
    const auto overlap = [](u64 a, u64 na, u64 b, u64 nb) { return a <= b ? b - a < na : a - b < nb; };
    if (auto status = typed_cold_original_exact(work); !status) return status;
    auto &w = *startup_workspace_;
    if (auto status = read(1, w.locator_super_); !status) return status;
    const auto super = std::span<const u8>{w.locator_super_};
    if (le16(super, 56) != 0xef53 || le32(super, 24) || le32(super, 76) != 1 || le32(super, 72) ||
        le32(super, 4) != 32768 || le32(super, 0) != 8192 || le32(super, 20) != 1 ||
        le32(super, 32) != 8192 || le32(super, 40) != 2048 || le32(super, 28) || le32(super, 36) != 8192 ||
        le16(super, 88) != 256 || le16(super, 90) || le32(super, 92) != 4 || le32(super, 96) != 0x46 ||
        le32(super, 100) != 0x400 || le32(super, 224) != 8 || super[373] != 1 || le32(super, 232) ||
        le16(super, 58) != 1 || le32(super, 228) || !jbd2::detail::all_zero(super.subspan(208, 16)) ||
        (le16(super, 254) && le16(super, 254) != 32) || le16(super, 206) ||
        le32(super, 336) || le32(super, 340) || le32(super, 344) ||
        le32(super, 84) < 11 || le32(super, 84) > 8192 || le32(super, 12) > 32767 || le32(super, 16) > 8192 ||
        owner_.bounds_.first_sector || owner_.bounds_.sector_count < 65536)
        return Status::unsupported("typed cold fixed checksummed superblock profile");
    if (jbd2::detail::crc32c(~u32{0}, super.first(1020)) != le32(super, 1020))
        return Status::io_error("typed cold physical superblock checksum");
    work.binding_ = {};
    auto &binding = work.binding_;
    binding.layout = {0, 32768, 0, 0, 1024}; binding.needs_recovery = true; binding.journal_inode = 8;
    for (usize at = 0; at < binding.uuid.size(); ++at) binding.uuid[at] = super[104 + at];
    const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
    if (auto status = read(2, w.locator_descriptors_); !status) return status;
    u32 journal_table{};
    for (u32 group = 0; group < 4; ++group)
    {
        auto descriptor = std::span<u8>{w.locator_descriptors_}.subspan(group * 32, 32);
        const auto checksum = le16(descriptor, 30);
        descriptor[30] = descriptor[31] = 0;
        const auto actual = jbd2::detail::crc32c(jbd2::detail::crc32c(seed, encoded(group)), descriptor);
        descriptor[30] = u8(checksum); descriptor[31] = u8(checksum >> 8);
        if (u16(actual) != checksum) return Status::io_error("typed cold physical group descriptor checksum");
        const u32 begin = 1 + group * 8192, end = group == 3 ? 32768 : begin + 8192;
        const u32 block_bitmap = le32(descriptor, 0), inode_bitmap = le32(descriptor, 4), table = le32(descriptor, 8);
        const u16 flags = le16(descriptor, 18);
        if (block_bitmap < begin || block_bitmap >= end || inode_bitmap < begin || inode_bitmap >= end ||
            table < begin || table >= end || 512 > end - table || le16(descriptor, 12) > end - begin ||
            le16(descriptor, 14) > 2048 || le16(descriptor, 16) > 2048 - le16(descriptor, 14) ||
            le16(descriptor, 28) > 2048 || (flags & ~7U) ||
            ((flags & 1) && (le16(descriptor, 14) != 2048 || le16(descriptor, 16) ||
                le16(descriptor, 28) != 2048 || le16(descriptor, 26))) ||
            ((flags & 2) && (le16(descriptor, 12) != end - begin - 516 || le16(descriptor, 24))))
            return Status::invalid_argument("typed cold physical group placement or counts");
        for (const auto range : std::array<jbd2::BlockRange, 4>{{{begin, 2}, {block_bitmap, 1}, {inode_bitmap, 1}, {table, 512}}})
        {
            for (usize at = 0; at < binding.metadata_count; ++at)
                if (overlap(range.first, range.count, binding.metadata[at].first, binding.metadata[at].count))
                    return Status::invalid_argument("typed cold structural metadata ranges overlap");
            binding.metadata[binding.metadata_count++] = range;
        }
        if (!group) journal_table = table + 1;
    }
    if (auto status = read(journal_table, w.locator_table_); !status) return status;
    const auto inode = std::span<const u8>{w.locator_table_}.subspan(3 * 256, 256);
    if (auto status = inode_checksum(seed, 8, inode); !status) return status;
    const u32 length = le16(inode, 56), first = le32(inode, 60);
    if (le16(inode, 0) != 0100600 || le16(inode, 26) != 1 || le32(inode, 20) || le32(inode, 32) != 0x80000 ||
        le16(inode, 40) != 0xf30a || le16(inode, 42) != 1 || le16(inode, 44) != 4 || le16(inode, 46) ||
        le32(inode, 52) || le16(inode, 58) || length < 1024 || length > 32768 || first < 3 || first >= 32768 ||
        length > 32768 - first || le32(inode, 4) != length * 1024 || le32(inode, 28) != length * 2)
        return Status::unsupported("typed cold physical contiguous journal inode");
    for (usize at = 0; at < binding.metadata_count; ++at)
        if (overlap(first, length, binding.metadata[at].first, binding.metadata[at].count))
            return Status::invalid_argument("typed cold journal overlaps structural metadata");
    binding.layout.journal_first_block = first; binding.layout.journal_blocks = length;
    if (auto status = read(first, w.locator_head_); !status) return status;
    const auto codec = jbd2::Codec::open(w.locator_head_, length, binding.uuid,
        jbd2::DescriptorUuidPolicy::linux_internal_zero);
    if (!codec) return codec.status();
    const auto &head = codec.value().superblock();
    if (head.block_size != 1024 || head.first_block != 1 || head.max_blocks != length || head.start_block != 1)
        return Status::unsupported("typed cold requires an active original journal at ring one");
    binding.journal_sequence = head.sequence; binding.empty_log = false;
    if (auto status = read(first + 1, w.locator_descriptor_); !status) return status;
    // Recognized families can only be selected explicitly; no legacy parser is
    // reachable on this route, including malformed recognized descriptors.
    const bool zero = Ext4ZeroTruncateWitnessCodec::present(w.locator_descriptor_);
    const bool allocated = Ext4AllocatedEmptyDataWitnessCodec::present(w.locator_descriptor_);
    if (zero == allocated) return Status::unsupported("typed cold descriptor has no unique recognized recovery family");
    usize actions{};
    const auto status = codec.value().descriptor(w.locator_descriptor_, head.sequence,
        [&](const jbd2::Tag &tag) -> Status {
            if (actions == 4 || tag.target_block >= 32768 ||
                overlap(tag.target_block, 1, first, length))
                return Status::invalid_argument("typed cold active descriptor target domain");
            ++actions; return {};
        });
    if (!status) return status;
    if (actions != 4) return Status::unsupported("typed cold recovery needs four authentic home tags");
    return work.cursor_.refine(work.context_, allocated ? Ext4MountedTypedRoute::allocated_recovery :
        Ext4MountedTypedRoute::zero_recovery, head.sequence);
}
[[nodiscard]] Status typed_classify_cold(TypedColdWorkspace &work)
{
    if (work.cursor_.control_.phase != TypedColdPhase::classifier || work.cursor_.control_.classifier_reads || work.original_.common_)
        return Status::bad_descriptor("typed cold classifier requires its original untouched reservation");
    auto read = [&](u32 block, auto &bytes) {
        return work.cursor_.read(work.context_, u64{block} * 2, std::as_writable_bytes(std::span{bytes}));
    };
    return typed_classify_cold_sources(work, read);
}
[[nodiscard]] Status typed_classify_common_sources(TypedColdWorkspace &work)
{
    if (!work.original_.common_ || work.cursor_.control_.phase != TypedColdPhase::classifier ||
        work.cursor_.control_.classifier_reads != 5 || !work.cursor_.common_adopted_)
        return Status::bad_descriptor("typed common classifier lacks its genuine adopted locator");
    auto read = [&](u32 block, auto &bytes) {
        return owner_.common_retained_locator(*work.original_.common_, block, bytes);
    };
    return typed_classify_cold_sources(work, read);
}
[[nodiscard]] Status typed_run_cold_child(TypedColdWorkspace &work)
{
    if (auto status = typed_cold_original_exact(work); !status) return status;
    if (auto status = work.cursor_.pause_child(work.context_); !status) return status;
    Status status;
    if (work.cursor_.capacity_.route == Ext4MountedTypedRoute::allocated_recovery)
        status = owner_.run_typed_allocated_with_authority(work.owner_workspace_, work.binding_, {}, 0,
            work.raw_, work.authority_, true);
    else if (work.cursor_.capacity_.route == Ext4MountedTypedRoute::zero_recovery)
        status = owner_.run_typed_zero_with_authority(work.owner_workspace_, work.binding_, {}, work.authority_, true);
    else return typed_cold_stop(work, Status::unsupported("typed cold child lacks explicit recovery family"));
    // Preserve the actual child's sticky failure: no resume or suffix follows.
    if (!status) return typed_cold_stop(work, status);
    return owner_.typed_resume_mounted_cursor(work.attachment_);
}

struct TypedColdPublication final
{
    explicit TypedColdPublication(TypedColdWorkspace &work) : suffix_(work), original_(this) {}
    TypedColdPublication(const TypedColdPublication &) = delete;
    TypedColdPublication &operator=(const TypedColdPublication &) = delete;
    TypedColdPublication(TypedColdPublication &&) = delete;
    TypedColdPublication &operator=(TypedColdPublication &&) = delete;
    TypedColdWorkspace &suffix_;
    const TypedColdPublication *const original_;
    TypedPublicationViewState fresh_view_{};
    Ext4RecoveryInfo final_binding_{};
    block::SessionReport endpoint_{};
    u64 operation_serial_{};
    bool attempted_{}, renewed_{}, completed_{};
};
[[nodiscard]] Status typed_cold_renewed_exact(const TypedColdPublication &publication) const
{
    const auto &work = publication.suffix_; const auto &a = work.attachment_; const auto &receipt = work.receipt_;
    if (!publication.renewed_ || !publication.operation_serial_ || a.operation_serial_ != publication.operation_serial_ ||
        &a.owner_ != &owner_ || &a.workspace_ != &work.owner_workspace_ || &a.session_ != &work.session_ ||
        a.original_ != &a || !a.renewed_ || a.admitted_ || !a.resumed_ || !a.receipt_consumed_ || a.receipt_ != &receipt ||
        !receipt.minted_ || !receipt.consumed_ || receipt.cursor_ != &work.cursor_ || receipt.context_ != &work.context_ ||
        receipt.session_ != &work.session_ || receipt.owner_ != &owner_ || receipt.mount_ != this ||
        receipt.serial_ != a.serial_ || receipt.sequence_ != publication.final_binding_.journal_sequence ||
        !work.cursor_.retained(work.context_, false, true) ||
        !reclaim_history::same_session(receipt.endpoint_, publication.endpoint_) ||
        !reclaim_history::same_session(work.cursor_.progress_, publication.endpoint_) ||
        !reclaim_history::same_session(a.suffix_terminal_, publication.endpoint_))
        return Status::fault("typed publication genuine renewal receipt differs");
    const auto session = work.session_.report();
    if (!session || !reclaim_history::same_session(session.value(), publication.endpoint_))
        return session ? Status::fault("typed publication renewed native endpoint changed") : session.status();
    smp::ScopedSpinLock guard(owner_.lock_);
    if (!owner_.typed_workspace_anchors_exact_locked() || owner_.typed_renewal_quarantined_ ||
        owner_.typed_operation_serial_ != publication.operation_serial_ ||
        owner_.typed_operation_serial_seal_ != publication.operation_serial_ ||
        owner_.typed_handoff_serial_ != publication.operation_serial_ || owner_.typed_renewed_serial_ != publication.operation_serial_ ||
        owner_.typed_mounted_pending_ || owner_.typed_mounted_attachment_ || owner_.typed_mounted_workspace_ ||
        owner_.typed_context_ || owner_.typed_attempted_ || owner_.typed_completed_ ||
        owner_.report_.phase != Ext4OwnedPhase::open || !owner_.report_.status ||
        work.owner_workspace_.phase_ != Owner::TypedOwnerPhase::ready ||
        work.owner_workspace_.phase_ != work.owner_workspace_.phase_seal_ || work.owner_workspace_.attempted_ ||
        work.owner_workspace_.renewal_quarantined_ || work.owner_workspace_.operation_serial_ != publication.operation_serial_ ||
        work.owner_workspace_.operation_serial_seal_ != publication.operation_serial_ ||
        work.owner_workspace_.handoff_serial_ != publication.operation_serial_ ||
        work.owner_workspace_.handoff_serial_seal_ != publication.operation_serial_)
        return Status::fault("typed publication original renewed Owner differs");
    return {};
}
[[nodiscard]] Status typed_publish_cold_root(TypedColdPublication &publication)
{
    auto &work = publication.suffix_;
    if (publication.original_ != &publication || publication.attempted_)
        return Status::already_exists("typed cold publication is original and one-shot");
    if (auto status = typed_cold_original_exact(work); !status) return status;
    if (!work.attempted_ || !work.finished_ || !work.semantic_status_ || !work.root_checked_ ||
        work.semantic_stage_ != TypedColdStage::startup_root || work.semantic_mask_ != u16{510} ||
        work.executing_ != TypedColdStage::none || work.cursor_.origin_ != Ext4MountedTypedOrigin::startup ||
        work.cursor_.control_.phase != TypedColdPhase::closed || work.cursor_.control_.stage != TypedColdStage::none ||
        work.cursor_.control_.stage_index != 8)
        return Status::busy("typed cold root requires all actual startup semantics");
    if (auto status = owner_.typed_mounted_consumed_receipt_exact(work.attachment_, work.receipt_); !status) return status;
    auto capture = [&](const auto &child, const auto &proof, const auto &binding, u32 table,
        const auto &image, u32 sequence, bool allocated, bool noop) -> Status {
        if (noop) return Status::fault("typed cold lineage cannot be a no-op");
        if (auto status = typed_cold_result_exact(work, TypedColdStage::startup_root,
            child, proof, binding, table, image, sequence, allocated, noop); !status) return status;
        if (!work.view_.valid() || work.view_.reader_ != &work.reader_ || !work.view_.attempted_ ||
            work.view_.ownership_range_count_ || work.view_.retained_orphan_ ||
            work.view_.retained_orphan_generation_ || work.view_.retained_orphan_next_)
            return Status::fault("typed cold publication fresh checked View differs");
        for (const auto &range : work.view_.ownership_ranges_)
            if (range.first || range.count) return Status::fault("typed cold publication fresh ownership state differs");
        typed_capture_publication_view(work.view_, publication.fresh_view_);
        publication.final_binding_ = binding;
        return {};
    };
    if (auto status = owner_.typed_mounted_visit_lineage(work.attachment_, capture); !status) return status;
    if (auto status = typed_cold_original_exact(work); !status) return status;
    publication.endpoint_ = work.receipt_.endpoint_;
    publication.operation_serial_ = work.attachment_.operation_serial_;
    publication.attempted_ = true;
    if (auto status = owner_.typed_renew_mounted_workspace(work.attachment_, work.receipt_); !status)
        return typed_cold_stop(work, status);
    publication.renewed_ = true;
    if (auto status = typed_cold_renewed_exact(publication); !status) return typed_cold_stop(work, status);
    if (auto status = typed_cold_original_exact(work); !status) return typed_cold_stop(work, status);
    Status status;
    {
        smp::ScopedSpinLock guard(lock_);
        if (report_.phase != Ext4MountPhase::opening || report_.epoch || report_.published || report_.reading ||
            work.root_.number != 2 || !typed_cold_same_root(work.root_, work.root_seal_) || (work.root_.mode & 0170000) != 0040000 ||
            !typed_publication_view_exact(work.view_, publication.fresh_view_, &work.reader_))
            status = Status::fault("typed cold terminal root or full fresh View changed");
        for (const auto &node : nodes_)
            if (node.inode_) status = Status::fault("typed cold runtime Node appeared before final publication");
        auto &root = nodes_[0];
        if (status && (root.borrowed_identity_exposed_ || root.mount_ || root.lifetime_key_.inode))
            status = Status::no_memory("typed cold original root slot unavailable");
        const Ext4InodeLifetimeKey key{this, identity_.session_serial, 2, work.root_.generation};
        if (status) status = root.name_.assign("/");
        if (status) status = lifetime_.observe(key);
        // A retention binding is constructed only after the last physical READ
        // and real renewal. Admission remains closed under the mount guard.
        if (status) status = root_retention_.bind(&root, this, identity_.session_serial,
                                                identity_.registration_generation);
        if (status)
        {
            typed_install_publication_view(view_, publication.fresh_view_, reader_);
            binding_ = publication.final_binding_;
            root.mount_ = this; root.inode_ = 2; root.lifetime_key_ = key;
            report_.epoch = 1; report_.phase = Ext4MountPhase::readable;
            publication.completed_ = true;
        }
        else root.name_ = {};
    }
    return status ? Status{} : typed_cold_stop(work, status);
}
[[nodiscard]] Status typed_run_cold_operation(TypedColdWorkspace &work, TypedColdPublication &publication)
{
    if (&publication.suffix_ != &work) return Status::bad_descriptor("typed cold publication requires original operation");
    if (auto status = typed_admit_cold_cursor(work); !status) return status;
    if (auto status = typed_classify_cold(work); !status) return typed_cold_stop(work, status);
    if (auto status = typed_run_cold_child(work); !status) return typed_cold_stop(work, status);
    if (auto status = typed_run_cold_suffix(work); !status) return status;
    return typed_publish_cold_root(publication);
}
template <typename Operation>
[[nodiscard]] Status typed_cold_mount_scope(typename Owner::Registry &registry,
    typename Owner::Registry::Registration key, block::SessionBounds bounds,
    typename Owner::TypedOwnerWorkspace &workspace, Operation operation)
{
    const auto overlaps = [](const void *a, usize na, const void *b, usize nb) {
        const auto x = reinterpret_cast<usize>(a), y = reinterpret_cast<usize>(b);
        return x <= y ? y - x < na : x - y < nb;
    };
    {
        smp::ScopedSpinLock guard(lock_);
        if (report_.phase != Ext4MountPhase::unused) return Status::already_exists("typed cold mount is one-shot");
        if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime || !startup_workspace_ ||
            !typed_cold_workspace_unused(workspace) ||
            overlaps(startup_workspace_, sizeof(*startup_workspace_), this, sizeof(*this)) ||
            overlaps(&workspace, sizeof(workspace), this, sizeof(*this)) ||
            overlaps(&workspace, sizeof(workspace), startup_workspace_, sizeof(*startup_workspace_)) ||
            (reclaim_workspace_ && (overlaps(reclaim_workspace_, sizeof(*reclaim_workspace_), this, sizeof(*this)) ||
             overlaps(reclaim_workspace_, sizeof(*reclaim_workspace_), startup_workspace_, sizeof(*startup_workspace_)) ||
             overlaps(reclaim_workspace_, sizeof(*reclaim_workspace_), &workspace, sizeof(workspace)))))
            return Status::invalid_argument("typed cold original storage or policy");
        report_.phase = Ext4MountPhase::opening;
    }
    if (auto status = startup_workspace_->claim(this, &owner_); !status) return fail(status);
    if (reclaim_workspace_)
        if (auto status = reclaim_workspace_->claim(this); !status) return fail(status);
    if (auto status = owner_.open(registry, key, bounds); !status) return fail(status);
    identity_ = owner_.buffers(); owner_.publication_domain_ = 1;
    const TypedColdOpening original(*this, *startup_workspace_, workspace);
    typed_cold_opening_ = &original;
    const auto status = operation(original);
    typed_cold_opening_ = nullptr;
    if (!status) return fail(status);
    {
        smp::ScopedSpinLock guard(lock_);
        if (report_.phase != Ext4MountPhase::readable || report_.epoch != 1 || report_.reading ||
            !nodes_[0].inode_ || nodes_[0].lifetime_key_.mount != this || nodes_[0].lifetime_key_.session != identity_.session_serial)
            return fail_locked(Status::fault("typed cold scope has no authenticated terminal root"));
    }
    return {};
}
