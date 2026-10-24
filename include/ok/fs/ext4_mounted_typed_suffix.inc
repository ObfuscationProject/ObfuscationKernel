// Private concrete mutation suffix. Construction grants no mounted admission;
// the original Node/context/Lease and actual Owner bridge must already be held.
using TypedSuffixStage = Ext4MountedTypedSuffixStage;
using TypedSuffixPhase = Ext4MountedTypedCursorPhase;
using TypedSuffixSession = typename Owner::TypedSession;
struct TypedSuffixWorkspace;
struct TypedSuffixAuthority final
{
    Ext4MountedMetadata &mount;
    TypedSuffixWorkspace &workspace;
    [[nodiscard]] Status validate(TypedSuffixPhase phase, TypedSuffixStage stage)
    { return mount.typed_suffix_validate(workspace, phase, stage); }
    [[nodiscard]] Status complete_stage(TypedSuffixStage stage)
    { return mount.typed_suffix_complete_stage(workspace, stage); }
};
using TypedSuffixCursor = Ext4MountedTypedCursor<TypedSuffixSession, TypedSuffixAuthority>;
using TypedSuffixAttachment = typename Owner::template TypedMountedCursorAttachment<TypedSuffixCursor>;
struct TypedSuffixReader final
{
    Ext4MountedMetadata &mount;
    TypedSuffixWorkspace &workspace;
    [[nodiscard]] Status read_block(u32 block, std::span<u8> output)
    { return mount.typed_suffix_read_block(workspace, block, output); }
};
using TypedSuffixView = Ext4ReadOnlyView<TypedSuffixReader>;
struct TypedSuffixDevice final : driver::BlockDevice
{
    Ext4MountedMetadata &mount;
    TypedSuffixWorkspace &workspace;
    TypedSuffixDevice(Ext4MountedMetadata &value, TypedSuffixWorkspace &storage) : mount(value), workspace(storage) {}
    [[nodiscard]] driver::BlockGeometry geometry() const override
    { return mount.typed_suffix_geometry(workspace); }
    [[nodiscard]] Status read_blocks(u64 sector, std::span<std::byte> output) override
    { return mount.typed_suffix_read_sector(workspace, sector, output); }
    [[nodiscard]] Status write_blocks(u64, std::span<const std::byte>) override
    { return Status::denied("typed suffix inspection cannot write"); }
    [[nodiscard]] Status flush() override
    { return Status::denied("typed suffix inspection cannot flush"); }
};
struct TypedSuffixWorkspace final
{
    TypedSuffixWorkspace(Ext4MountedMetadata &mount, const NodeMutationContext &original,
        typename Owner::TypedOwnerWorkspace &workspace, u64 serial)
        : mount_(mount), original_(original), owner_workspace_(workspace), session_(mount.owner_.typed_mounted_original_session()),
          published_(mount.report_.published), authority_{mount, *this}, cursor_(session_),
          context_(session_, authority_, mount.owner_, mount, serial, &workspace,
              &Owner::template typed_mounted_bridge_thunk<TypedSuffixCursor>),
          attachment_(mount.owner_, workspace, cursor_, context_), reader_{mount, *this}, device_(mount, *this),
          inspection_(true, true) {}
    TypedSuffixWorkspace(const TypedSuffixWorkspace &) = delete;
    TypedSuffixWorkspace &operator=(const TypedSuffixWorkspace &) = delete;
    TypedSuffixWorkspace(TypedSuffixWorkspace &&) = delete;
    TypedSuffixWorkspace &operator=(TypedSuffixWorkspace &&) = delete;
    Ext4MountedMetadata &mount_;
    const NodeMutationContext &original_;
    typename Owner::TypedOwnerWorkspace &owner_workspace_;
    TypedSuffixSession &session_;
    const bool published_;
    TypedSuffixAuthority authority_;
    TypedSuffixCursor cursor_;
    typename TypedSuffixCursor::Context context_;
    TypedSuffixAttachment attachment_;
    TypedSuffixReader reader_;
    TypedSuffixDevice device_;
    TypedSuffixView view_{};
    Ext4ReadOnlyOpenScratch opening_{};
    Ext4RecoveryCompletion<6, 2> inspection_;
    typename TypedSuffixCursor::Receipt receipt_{};
    Ext4RecoveryInfo binding_{};
    std::array<u8, 1024> raw_{};
    Ext4ReadOnlyInode target_{};
    Metadata metadata_{};
    TypedSuffixStage executing_{TypedSuffixStage::none}, semantic_stage_{TypedSuffixStage::none};
    u16 semantic_mask_{};
    Status semantic_status_{};
    bool attempted_{}, finished_{};
};
[[nodiscard]] Status typed_suffix_original_exact(const TypedSuffixWorkspace &work) const
{
    if (&work.mount_ != this || &work.session_ != &owner_.typed_mounted_original_session() ||
        &work.authority_.mount != this || &work.authority_.workspace != &work ||
        &work.reader_.mount != this || &work.reader_.workspace != &work ||
        &work.device_.mount != this || &work.device_.workspace != &work ||
        &work.attachment_.owner_ != &owner_ || &work.attachment_.workspace_ != &work.owner_workspace_ ||
        &work.attachment_.cursor_ != &work.cursor_ || &work.attachment_.context_ != &work.context_ ||
        policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime)
        return Status::fault("typed semantic suffix original concrete storage differs");
    {
        smp::ScopedSpinLock guard(lock_);
        if (!report_.status || report_.published != work.published_ || report_.epoch != work.original_.epoch ||
            report_.phase != Ext4MountPhase::updating || report_.reading)
            return Status::fault("typed semantic suffix original mounted publication differs");
        if (auto status = mutation_integrity_locked(&work.original_); !status) return status;
    }
    // identity observes Owner/cache only after the mount guard is released.
    return identity(work.original_.epoch, &work.original_);
}
[[nodiscard]] Status typed_suffix_validate(TypedSuffixWorkspace &work,
    TypedSuffixPhase phase, TypedSuffixStage stage)
{
    if (auto status = typed_suffix_original_exact(work); !status) return status;
    if (phase == TypedSuffixPhase::suffix && stage != TypedSuffixStage::none && stage != work.executing_)
        return Status::fault("typed semantic suffix selected stage differs");
    if (phase == TypedSuffixPhase::closed && work.executing_ != TypedSuffixStage::none)
        return Status::fault("typed semantic suffix closed with a helper active");
    return {};
}
[[nodiscard]] Status typed_suffix_stop(TypedSuffixWorkspace &work, Status status)
{
    if (status) status = Status::fault("typed semantic suffix stop requires error");
    static_cast<void>(owner_.typed_mounted_cursor_stop(work.attachment_, status));
    return fail(status);
}
[[nodiscard]] driver::BlockGeometry typed_suffix_geometry(TypedSuffixWorkspace &work)
{
    if (auto status = typed_suffix_original_exact(work); !status)
    { static_cast<void>(typed_suffix_stop(work, status)); return {}; }
    if (auto status = owner_.typed_mounted_cursor_exact(work.attachment_); !status)
    { static_cast<void>(typed_suffix_stop(work, status)); return {}; }
    // This is the original immutable native volume geometry, including writable
    // capability required by prepare(). Adapter mutation methods still refuse.
    const auto geometry = work.session_.geometry();
    if (auto status = typed_suffix_original_exact(work); !status)
    { static_cast<void>(typed_suffix_stop(work, status)); return {}; }
    return geometry;
}
[[nodiscard]] Status typed_suffix_read_sector(TypedSuffixWorkspace &work, u64 sector,
    std::span<std::byte> output)
{
    if (work.executing_ == TypedSuffixStage::none || work.cursor_.control_.stage != work.executing_ || output.size() != 1024)
        return typed_suffix_stop(work, Status::denied("typed semantic suffix lacks its selected 1 KiB helper"));
    if (auto status = typed_suffix_original_exact(work); !status) return typed_suffix_stop(work, status);
    const auto status = work.cursor_.read(work.context_, sector, output);
    if (!status) return status;
    if (auto exact = typed_suffix_original_exact(work); !exact) return typed_suffix_stop(work, exact);
    return {};
}
[[nodiscard]] Status typed_suffix_read_block(TypedSuffixWorkspace &work, u32 block, std::span<u8> output)
{
    if (work.binding_.layout.block_size != 1024 || block >= work.binding_.layout.filesystem_blocks ||
        u64{block} > (~u64{0} - work.binding_.layout.first_sector) / 2)
        return typed_suffix_stop(work, Status::invalid_argument("typed semantic suffix filesystem block bounds"));
    return typed_suffix_read_sector(work, work.binding_.layout.first_sector + u64{block} * 2,
        std::as_writable_bytes(output));
}
template <typename ChildReader>
[[nodiscard]] static bool typed_suffix_same_open_view(const TypedSuffixWorkspace &work,
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
[[nodiscard]] static bool typed_suffix_same_metadata(const Metadata &a, const Metadata &b)
{
    return a.type == b.type && a.mode == b.mode && a.uid == b.uid && a.gid == b.gid && a.size == b.size &&
        a.blocks == b.blocks && a.link_count == b.link_count && a.block_size == b.block_size &&
        a.attributes.modification.seconds == b.attributes.modification.seconds &&
        a.attributes.modification.nanoseconds == b.attributes.modification.nanoseconds &&
        a.attributes.change.seconds == b.attributes.change.seconds && a.attributes.change.nanoseconds == b.attributes.change.nanoseconds;
}
template <typename ChildReader>
[[nodiscard]] Status typed_suffix_result_exact(TypedSuffixWorkspace &work, TypedSuffixStage stage,
    const Ext4ReadOnlyView<ChildReader> &child, const Ext4ZeroTruncateOwnership &proof,
    const Ext4RecoveryInfo &binding, u32 table, const Ext4ZeroTruncateBlock &image,
    u32 sequence, bool allocated, bool noop)
{
    using namespace ext4_checked;
    const auto key = work.original_.key;
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
    if (stage == TypedSuffixStage::inspect_empty)
    {
        const auto *actual = work.inspection_.binding();
        if (!actual || !work.inspection_.report().status || !work.inspection_.report().empty_log ||
            !reclaim_history::same_info(*actual, binding) || !reclaim_history::same_info(work.binding_, binding))
            return Status::fault("typed semantic suffix strict empty preparation differs");
        return {};
    }
    if (stage != TypedSuffixStage::noop_terminal && !typed_suffix_same_open_view(work, child))
        return Status::fault("typed semantic suffix fresh checked view differs from actual child");
    if (stage == TypedSuffixStage::target_inode)
    {
        const auto value = child.information(key.inode, expected);
        const auto &actual = work.target_;
        if (actual.number != value.number || actual.generation != value.generation || actual.size != value.size ||
            actual.mode != value.mode || actual.uid != value.uid || actual.gid != value.gid || actual.links != value.links)
            return Status::fault("typed semantic suffix checked target inode differs");
    }
    if (stage == TypedSuffixStage::raw_target || stage == TypedSuffixStage::final_wrapper || stage == TypedSuffixStage::noop_terminal)
    {
        if (noop != (stage == TypedSuffixStage::noop_terminal))
            return Status::fault("typed semantic suffix no-op stage selection differs");
        const auto actual = std::span<const u8>{work.raw_}.subspan((local % 4) * 256, 256);
        if (auto status = inode_checksum(child.seed_, key.inode, actual); !status) return status;
        if (le32(actual, 100) != key.generation || !std::equal(actual.begin(), actual.end(), expected.begin()))
            return Status::io_error("typed semantic suffix physical target bytes differ from authentic child");
    }
    if (stage == TypedSuffixStage::target_metadata)
    {
        auto value = child.project_metadata(key.inode, expected);
        if (auto status = decode_ext4_times(std::as_bytes(expected), value.attributes); !status) return status;
        if (!typed_suffix_same_metadata(work.metadata_, value))
            return Status::fault("typed semantic suffix checked metadata differs from authentic child image");
    }
    return {};
}
[[nodiscard]] Status typed_suffix_complete_stage(TypedSuffixWorkspace &work, TypedSuffixStage stage)
{
    if (auto status = typed_suffix_original_exact(work); !status) return status;
    if (stage == TypedSuffixStage::none || work.executing_ != stage || work.semantic_stage_ != stage ||
        !work.semantic_status_ || !(work.semantic_mask_ & (u16{1} << static_cast<u8>(stage))))
        return Status::fault("typed semantic suffix helper success was not retained");
    auto verify = [&](const auto &view, const auto &proof, const auto &binding, u32 table,
        const auto &image, u32 sequence, bool allocated, bool noop) -> Status {
        return typed_suffix_result_exact(work, stage, view, proof, binding, table, image, sequence, allocated, noop);
    };
    const auto status = owner_.typed_mounted_visit_lineage(work.attachment_, verify);
    if (auto exact = typed_suffix_original_exact(work); !exact) return exact;
    return status;
}
[[nodiscard]] Status typed_suffix_execute_stage(TypedSuffixWorkspace &work, TypedSuffixStage stage)
{
    work.executing_ = stage;
    if (auto status = work.cursor_.begin_stage(work.context_, stage); !status) return status;
    auto execute = [&](const auto &child, const auto &proof, const Ext4RecoveryInfo &binding, u32 table,
        const auto &image, u32 sequence, bool allocated, bool noop) -> Status {
        if (auto status = typed_suffix_original_exact(work); !status) return status;
        Status status;
        switch (stage)
        {
        case TypedSuffixStage::inspect_empty:
            status = work.inspection_.prepare_typed_empty(work.device_, owner_.bounds_);
            if (status) work.binding_ = *work.inspection_.binding();
            break;
        case TypedSuffixStage::reopen_view: status = work.view_.open(work.reader_, work.binding_, work.opening_); break;
        case TypedSuffixStage::stored_metadata: status = work.view_.validate_stored_metadata(); break;
        case TypedSuffixStage::target_inode:
        {
            auto result = work.view_.inode(work.original_.key.inode);
            status = result ? Status{} : result.status(); if (result) work.target_ = result.value(); break;
        }
        case TypedSuffixStage::raw_target: case TypedSuffixStage::final_wrapper: case TypedSuffixStage::noop_terminal:
            work.binding_ = binding; status = typed_suffix_read_block(work, table, work.raw_); break;
        case TypedSuffixStage::target_metadata:
        {
            auto result = work.view_.query_stored_metadata(work.original_.key.inode);
            status = result ? Status{} : result.status(); if (result) work.metadata_ = result.value(); break;
        }
        default: return Status::unsupported("typed semantic suffix startup is a separate original domain");
        }
        if (!status) return status;
        if (auto exact = typed_suffix_original_exact(work); !exact) return exact;
        return typed_suffix_result_exact(work, stage, child, proof, binding, table, image, sequence, allocated, noop);
    };
    const auto status = owner_.typed_mounted_visit_lineage(work.attachment_, execute);
    work.semantic_status_ = status;
    if (!status) return typed_suffix_stop(work, status);
    work.semantic_stage_ = stage; work.semantic_mask_ |= u16{1} << static_cast<u8>(stage);
    if (auto ended = work.cursor_.end_stage(work.context_); !ended) return typed_suffix_stop(work, ended);
    work.executing_ = TypedSuffixStage::none;
    return {};
}
// Suffix-only composition. The actual bridge must already have authenticated
// child completion. It consumes the receipt and leaves Owner exclusion intact;
// publication, role release, epoch commit and renewal are later separate work.
[[nodiscard]] Status typed_run_semantic_suffix(TypedSuffixWorkspace &work)
{
    if (work.attempted_) return Status::already_exists("typed semantic suffix is one-shot");
    work.attempted_ = true;
    if (auto status = typed_suffix_original_exact(work); !status) return typed_suffix_stop(work, status);
    if (work.cursor_.origin_ == Ext4MountedTypedOrigin::startup)
        return typed_suffix_stop(work, Status::unsupported("typed startup suffix requires its provisional root domain"));
    if (auto status = owner_.typed_mounted_cursor_exact(work.attachment_); !status) return typed_suffix_stop(work, status);
    if (work.cursor_.capacity_.route == Ext4MountedTypedRoute::zero_noop)
    {
        if (auto status = typed_suffix_execute_stage(work, TypedSuffixStage::noop_terminal); !status) return status;
    }
    else for (auto stage : {TypedSuffixStage::inspect_empty, TypedSuffixStage::reopen_view,
        TypedSuffixStage::stored_metadata, TypedSuffixStage::target_inode, TypedSuffixStage::raw_target,
        TypedSuffixStage::target_metadata, TypedSuffixStage::final_wrapper})
        if (auto status = typed_suffix_execute_stage(work, stage); !status) return status;
    if (auto status = typed_suffix_original_exact(work); !status) return typed_suffix_stop(work, status);
    if (auto status = work.cursor_.close_suffix(work.context_, work.receipt_); !status) return typed_suffix_stop(work, status);
    if (auto status = owner_.typed_accept_mounted_cursor_receipt(work.attachment_, work.receipt_); !status)
        return typed_suffix_stop(work, status);
    if (auto status = typed_suffix_original_exact(work); !status) return typed_suffix_stop(work, status);
    work.finished_ = true;
    return {};
}
