// Private publication consumes the genuine suffix and renewal path. It never
// opens a second view or transports I/O after the final reserved physical READ.
struct TypedPublicationViewState final
{
    struct Group { u32 block_bitmap{}, inode_bitmap{}, table{}; u16 block_checksum{}, inode_checksum{}, flags{};
        friend bool operator==(const Group &, const Group &) = default; };
    struct Range { u32 first{}, count{}; friend bool operator==(const Range &, const Range &) = default; };
    const void *reader{};
    jbd2::RecoveryLayout layout{};
    std::array<u8, 16> uuid{};
    std::array<Group, Ext4RecoveryInfo::group_capacity> groups{};
    u32 seed{}, first_data{}, blocks_per_group{}, inodes_per_group{}, group_count{}, inodes{}, first_inode{}, table_blocks{};
    std::array<Range, 128 * 4> ranges{};
    usize range_count{};
    u32 retained_orphan{}, retained_orphan_generation{}, retained_orphan_next{};
    bool attempted{}, ready{}, failed{};
};
struct TypedPublicationWorkspace final
{
    explicit TypedPublicationWorkspace(TypedSuffixWorkspace &suffix) : suffix_(suffix), original_(this) {}
    TypedPublicationWorkspace(const TypedPublicationWorkspace &) = delete;
    TypedPublicationWorkspace &operator=(const TypedPublicationWorkspace &) = delete;
    TypedPublicationWorkspace(TypedPublicationWorkspace &&) = delete;
    TypedPublicationWorkspace &operator=(TypedPublicationWorkspace &&) = delete;
    TypedSuffixWorkspace &suffix_;
    const TypedPublicationWorkspace *const original_;
    TypedPublicationViewState old_view_{}, fresh_view_{};
    Ext4RecoveryInfo old_binding_{}, final_binding_{};
    block::SessionReport endpoint_{};
    u64 epoch_{}, operation_serial_{};
    bool published_{}, begun_{}, attempted_{}, renewed_{}, completed_{}, epoch_committed_{};
};
template <typename ViewReader>
static void typed_capture_publication_view(const Ext4ReadOnlyView<ViewReader> &view, TypedPublicationViewState &saved)
{
    saved.reader = view.reader_; saved.layout = view.layout_; saved.uuid = view.uuid_;
    saved.seed = view.seed_; saved.first_data = view.first_data_; saved.blocks_per_group = view.blocks_per_group_;
    saved.inodes_per_group = view.inodes_per_group_; saved.group_count = view.group_count_;
    saved.inodes = view.inodes_; saved.first_inode = view.first_inode_; saved.table_blocks = view.table_blocks_;
    saved.range_count = view.ownership_range_count_; saved.retained_orphan = view.retained_orphan_;
    saved.retained_orphan_generation = view.retained_orphan_generation_; saved.retained_orphan_next = view.retained_orphan_next_;
    saved.attempted = view.attempted_; saved.ready = view.ready_; saved.failed = view.failed_;
    for (usize at = 0; at < saved.groups.size(); ++at)
    {
        const auto &group = view.groups_[at];
        saved.groups[at] = {group.block_bitmap, group.inode_bitmap, group.table, group.block_checksum, group.inode_checksum, group.flags};
    }
    for (usize at = 0; at < saved.ranges.size(); ++at)
        saved.ranges[at] = {view.ownership_ranges_[at].first, view.ownership_ranges_[at].count};
}
template <typename ViewReader>
[[nodiscard]] static bool typed_publication_view_exact(const Ext4ReadOnlyView<ViewReader> &view,
    const TypedPublicationViewState &saved, const void *reader)
{
    const auto &a = view.layout_; const auto &b = saved.layout;
    if (view.reader_ != reader || a.first_sector != b.first_sector || a.filesystem_blocks != b.filesystem_blocks ||
        a.journal_first_block != b.journal_first_block || a.journal_blocks != b.journal_blocks || a.block_size != b.block_size ||
        view.uuid_ != saved.uuid || view.seed_ != saved.seed || view.first_data_ != saved.first_data ||
        view.blocks_per_group_ != saved.blocks_per_group || view.inodes_per_group_ != saved.inodes_per_group ||
        view.group_count_ != saved.group_count || view.inodes_ != saved.inodes || view.first_inode_ != saved.first_inode ||
        view.table_blocks_ != saved.table_blocks || view.ownership_range_count_ != saved.range_count ||
        view.retained_orphan_ != saved.retained_orphan || view.retained_orphan_generation_ != saved.retained_orphan_generation ||
        view.retained_orphan_next_ != saved.retained_orphan_next || view.attempted_ != saved.attempted ||
        view.ready_ != saved.ready || view.failed_ != saved.failed) return false;
    for (usize at = 0; at < saved.groups.size(); ++at)
    {
        const auto &group = view.groups_[at];
        if (saved.groups[at] != typename TypedPublicationViewState::Group{group.block_bitmap, group.inode_bitmap,
            group.table, group.block_checksum, group.inode_checksum, group.flags}) return false;
    }
    for (usize at = 0; at < saved.ranges.size(); ++at)
        if (saved.ranges[at] != typename TypedPublicationViewState::Range{
            view.ownership_ranges_[at].first, view.ownership_ranges_[at].count}) return false;
    return true;
}
static void typed_install_publication_view(View &view, const TypedPublicationViewState &saved, Reader &reader)
{
    // Every field of the fresh checked View is retained; only its Reader domain
    // changes from finite suffix transport to the mount's original generic Reader.
    view.~View(); ::new (static_cast<void *>(&view)) View;
    view.reader_ = &reader; view.layout_ = saved.layout; view.uuid_ = saved.uuid;
    view.seed_ = saved.seed; view.first_data_ = saved.first_data; view.blocks_per_group_ = saved.blocks_per_group;
    view.inodes_per_group_ = saved.inodes_per_group; view.group_count_ = saved.group_count;
    view.inodes_ = saved.inodes; view.first_inode_ = saved.first_inode; view.table_blocks_ = saved.table_blocks;
    view.ownership_range_count_ = saved.range_count; view.retained_orphan_ = saved.retained_orphan;
    view.retained_orphan_generation_ = saved.retained_orphan_generation; view.retained_orphan_next_ = saved.retained_orphan_next;
    view.attempted_ = saved.attempted; view.ready_ = saved.ready; view.failed_ = saved.failed;
    for (usize at = 0; at < saved.groups.size(); ++at)
    {
        const auto &group = saved.groups[at];
        view.groups_[at] = {group.block_bitmap, group.inode_bitmap, group.table, group.block_checksum, group.inode_checksum, group.flags};
    }
    for (usize at = 0; at < saved.ranges.size(); ++at)
        view.ownership_ranges_[at] = {saved.ranges[at].first, saved.ranges[at].count};
}
[[nodiscard]] Status typed_begin_suffix_publication(TypedPublicationWorkspace &publication)
{
    auto &work = publication.suffix_;
    if (publication.original_ != &publication || publication.begun_ || publication.attempted_)
        return Status::already_exists("typed publication original preparation is one-shot");
    if (&work.mount_ != this || work.attempted_ || work.cursor_.report().phase != TypedSuffixPhase::unused)
        return Status::bad_descriptor("typed publication must precede its actual finite cursor");
    if (auto status = typed_suffix_original_exact(work); !status) return status;
    smp::ScopedSpinLock guard(lock_);
    if (!view_.valid() || view_.reader_ != &reader_ || !view_.attempted_ ||
        view_.ownership_range_count_ > view_.ownership_ranges_.size() || !binding_.empty_log ||
        report_.epoch != work.original_.epoch || report_.phase != Ext4MountPhase::updating || report_.reading)
        return Status::fault("typed publication original mounted view unavailable");
    if (auto status = mutation_integrity_locked(&work.original_); !status) return status;
    typed_capture_publication_view(view_, publication.old_view_);
    publication.old_binding_ = binding_; publication.epoch_ = report_.epoch;
    publication.published_ = report_.published; publication.begun_ = true;
    return {};
}
[[nodiscard]] Status typed_publication_old_mount_exact(const TypedPublicationWorkspace &publication) const
{
    const auto &work = publication.suffix_;
    if (publication.original_ != &publication || !publication.begun_ || &work.mount_ != this ||
        publication.epoch_ != work.original_.epoch || publication.published_ != work.published_)
        return Status::fault("typed publication original storage differs");
    smp::ScopedSpinLock guard(lock_);
    if (!report_.status || report_.epoch != publication.epoch_ || report_.phase != Ext4MountPhase::updating || report_.reading ||
        report_.published != publication.published_ || !reclaim_history::same_info(binding_, publication.old_binding_) ||
        !typed_publication_view_exact(view_, publication.old_view_, &reader_))
        return Status::fault("typed publication original full mounted view changed");
    return mutation_integrity_locked(&work.original_);
}
[[nodiscard]] Status typed_publication_renewed_exact(const TypedPublicationWorkspace &publication) const
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
[[nodiscard]] Status typed_publish_completed_suffix(TypedPublicationWorkspace &publication)
{
    auto &work = publication.suffix_;
    if (publication.attempted_) return Status::already_exists("typed publication is one-shot");
    if (auto status = typed_publication_old_mount_exact(publication); !status) return status;
    if (auto status = typed_suffix_original_exact(work); !status) return status;
    const bool noop = work.cursor_.capacity_.route == Ext4MountedTypedRoute::zero_noop;
    const auto final = noop ? TypedSuffixStage::noop_terminal : TypedSuffixStage::final_wrapper;
    const u16 required = noop ? u16{1} << static_cast<u8>(TypedSuffixStage::noop_terminal) : u16{254};
    if (!work.attempted_ || !work.finished_ || !work.semantic_status_ || work.semantic_stage_ != final ||
        work.semantic_mask_ != required || work.executing_ != TypedSuffixStage::none ||
        work.cursor_.origin_ != Ext4MountedTypedOrigin::mutation || work.cursor_.control_.phase != TypedSuffixPhase::closed ||
        work.cursor_.control_.stage != TypedSuffixStage::none || work.cursor_.control_.stage_index != (noop ? 1U : 7U))
        return Status::busy("typed publication requires its complete actual semantic suffix");
    if (!noop && publication.epoch_ == ~u64{0}) return Status::overflow("typed publication epoch exhausted");
    if (auto status = owner_.typed_mounted_consumed_receipt_exact(work.attachment_, work.receipt_); !status) return status;
    auto capture = [&](const auto &child, const auto &proof, const auto &binding, u32 table,
        const auto &image, u32 sequence, bool allocated, bool actual_noop) -> Status {
        if (actual_noop != noop) return Status::fault("typed publication actual child route differs");
        if (auto status = typed_suffix_result_exact(work, final, child, proof, binding, table, image, sequence, allocated, actual_noop); !status)
            return status;
        if (noop)
        {
            if (!reclaim_history::same_info(binding, publication.old_binding_) || work.view_.attempted_)
                return Status::fault("typed publication no-op changed its original binding or opened a view");
        }
        else
        {
            if (!work.view_.valid() || work.view_.reader_ != &work.reader_ ||
                !work.view_.attempted_ || work.view_.ownership_range_count_ || work.view_.retained_orphan_ ||
                work.view_.retained_orphan_generation_ || work.view_.retained_orphan_next_)
                return Status::fault("typed publication fresh complete view unavailable");
            // A fresh mounted metadata View has not run the child's global
            // ownership proof. Authenticate its actual initial range state;
            // child ownership remains independently retained by the lineage.
            for (const auto &range : work.view_.ownership_ranges_)
                if (range.first || range.count)
                    return Status::fault("typed publication fresh ownership state differs");
            if (!typed_suffix_same_open_view(work, child))
                return Status::fault("typed publication fresh decoded view differs from actual child");
            typed_capture_publication_view(work.view_, publication.fresh_view_);
        }
        publication.final_binding_ = binding;
        return {};
    };
    if (auto status = owner_.typed_mounted_visit_lineage(work.attachment_, capture); !status) return status;
    if (auto status = typed_publication_old_mount_exact(publication); !status) return status;
    if (auto status = typed_suffix_original_exact(work); !status) return status;
    if (auto status = owner_.typed_mounted_consumed_receipt_exact(work.attachment_, work.receipt_); !status) return status;
    publication.endpoint_ = work.receipt_.endpoint_; publication.operation_serial_ = work.attachment_.operation_serial_;
    publication.attempted_ = true;
    if (auto status = owner_.typed_renew_mounted_workspace(work.attachment_, work.receipt_); !status) return fail(status);
    publication.renewed_ = true;
    if (auto status = typed_publication_renewed_exact(publication); !status) return fail(status);
    if (auto status = typed_publication_old_mount_exact(publication); !status) return fail(status);
    if (auto status = typed_suffix_original_exact(work); !status) return fail(status);
    Status status;
    {
        smp::ScopedSpinLock guard(lock_);
        if (!report_.status || report_.epoch != publication.epoch_ || report_.phase != Ext4MountPhase::updating || report_.reading ||
            report_.published != publication.published_ || !reclaim_history::same_info(binding_, publication.old_binding_) ||
            !typed_publication_view_exact(view_, publication.old_view_, &reader_))
            status = Status::fault("typed publication original mount changed before installation");
        else status = mutation_integrity_locked(&work.original_);
        if (status && !noop && (publication.fresh_view_.reader != &work.reader_ ||
            !typed_publication_view_exact(work.view_, publication.fresh_view_, &work.reader_)))
            status = Status::fault("typed publication full fresh view changed after renewal");
        if (status)
        {
            if (!noop)
            {
                typed_install_publication_view(view_, publication.fresh_view_, reader_);
                binding_ = publication.final_binding_; ++report_.epoch; publication.epoch_committed_ = true;
            }
            publication.completed_ = true;
        }
    }
    return status ? Status{} : fail(status);
}
