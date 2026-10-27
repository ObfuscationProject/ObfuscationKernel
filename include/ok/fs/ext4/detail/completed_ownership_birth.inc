// Private declaration inside the ownership validator. Existing view friendship
// is sufficient; this record conveys no transport or mounted admission right.
template <typename> friend class Ext4ZeroTruncateTransaction;
template <typename> friend class Ext4AllocatedEmptyDataTransaction;
class CompletedBirthSeal final
{
    friend class Ext4ZeroTruncateOwnershipValidator;
    struct Group
    {
        u32 block_bitmap{}, inode_bitmap{}, table{};
        u16 block_checksum{}, inode_checksum{}, flags{};
        friend bool operator==(const Group &, const Group &) = default;
    };
    struct Range
    {
        u32 first{}, count{};
        friend bool operator==(const Range &, const Range &) = default;
    };
  public:
    CompletedBirthSeal() = default;
    CompletedBirthSeal(const CompletedBirthSeal &) = delete;
    CompletedBirthSeal &operator=(const CompletedBirthSeal &) = delete;
    CompletedBirthSeal(CompletedBirthSeal &&) = delete;
    CompletedBirthSeal &operator=(CompletedBirthSeal &&) = delete;
  private:
    const void *child_{}, *view_{}, *reader_{}, *proof_{};
    const Ext4RecoveryInfo *original_binding_{};
    Ext4RecoveryInfo binding_{};
    jbd2::RecoveryLayout layout_{};
    std::array<u8, 16> uuid_{};
    std::array<Group, Ext4RecoveryInfo::group_capacity> groups_{};
    std::array<Range, 128 * 4> ranges_{};
    usize range_count_{};
    u32 seed_{}, first_data_{}, blocks_per_group_{}, inodes_per_group_{}, group_count_{}, inodes_{}, first_inode_{}, table_blocks_{};
    u32 retained_orphan_{}, retained_orphan_generation_{}, retained_orphan_next_{};
    Ext4ZeroTruncateOwnership target_{};
    bool attempted_{}, ready_{}, failed_{}, global_minted_{}, target_minted_{};
};
// Child-held progression is separate from the copied birth observations. Its
// one-way stages bind the actual child and seal before the first proof callback.
class CompletedBirthProgress final
{
    friend class Ext4ZeroTruncateOwnershipValidator;
  public:
    CompletedBirthProgress() = default;
    CompletedBirthProgress(const CompletedBirthProgress &) = delete;
    CompletedBirthProgress &operator=(const CompletedBirthProgress &) = delete;
    CompletedBirthProgress(CompletedBirthProgress &&) = delete;
    CompletedBirthProgress &operator=(CompletedBirthProgress &&) = delete;
  private:
    enum class Stage : u8 { unused, proving, global, target };
    Stage stage_{};
    const void *child_{};
    const CompletedBirthSeal *seal_{};
};
[[nodiscard]] static bool completed_birth_progress_exact(const CompletedBirthSeal &seal,
    const CompletedBirthProgress &progress, const void *child)
{
    using Stage = CompletedBirthProgress::Stage;
    if (progress.stage_ == Stage::unused)
        return !progress.child_ && !progress.seal_ && !seal.global_minted_ && !seal.target_minted_;
    if (!child || progress.child_ != child || progress.seal_ != &seal) return false;
    switch (progress.stage_)
    {
    case Stage::proving: return !seal.global_minted_ && !seal.target_minted_;
    case Stage::global: return seal.global_minted_ && !seal.target_minted_;
    case Stage::target: return seal.global_minted_ && seal.target_minted_;
    case Stage::unused: break;
    }
    return false;
}
[[nodiscard]] static bool completed_birth_uncaptured(const CompletedBirthSeal &seal,
    const CompletedBirthProgress &progress, const void *child)
{
    return completed_birth_progress_exact(seal, progress, child) &&
        (progress.stage_ == CompletedBirthProgress::Stage::unused ||
         progress.stage_ == CompletedBirthProgress::Stage::proving);
}
[[nodiscard]] static bool completed_birth_started(const CompletedBirthSeal &seal)
{ return seal.global_minted_ || seal.target_minted_; }
[[nodiscard]] static bool completed_birth_complete(const CompletedBirthSeal &seal)
{ return seal.global_minted_ && seal.target_minted_; }
[[nodiscard]] static bool same_birth_layout(const jbd2::RecoveryLayout &a, const jbd2::RecoveryLayout &b)
{
    return a.first_sector == b.first_sector && a.filesystem_blocks == b.filesystem_blocks &&
        a.journal_first_block == b.journal_first_block && a.journal_blocks == b.journal_blocks && a.block_size == b.block_size;
}
[[nodiscard]] static bool same_birth_binding(const Ext4RecoveryInfo &a, const Ext4RecoveryInfo &b)
{
    if (!same_birth_layout(a.layout, b.layout) || a.uuid != b.uuid || a.metadata_count != b.metadata_count ||
        a.metadata_count > a.metadata.size() || b.metadata_count > b.metadata.size() ||
        a.journal_inode != b.journal_inode || a.journal_sequence != b.journal_sequence ||
        a.needs_recovery != b.needs_recovery || a.empty_log != b.empty_log) return false;
    for (usize at = 0; at < a.metadata.size(); ++at)
        if (a.metadata[at].first != b.metadata[at].first || a.metadata[at].count != b.metadata[at].count) return false;
    return true;
}
template <jbd2::BlockReader Reader>
[[nodiscard]] static bool completed_birth_exact(const Ext4ReadOnlyView<Reader> &view, const Reader &reader,
    const Ext4RecoveryInfo &binding, const Ext4ZeroTruncateOwnership &proof,
    const CompletedBirthSeal &seal, const void *child, bool require_target)
{
    if (!seal.global_minted_ || (require_target && !seal.target_minted_) ||
        seal.child_ != child || seal.view_ != &view || seal.reader_ != &reader || view.reader_ != &reader ||
        seal.proof_ != &proof || seal.original_binding_ != &binding || !same_birth_binding(binding, seal.binding_) ||
        !same_birth_layout(view.layout_, seal.layout_) || view.uuid_ != seal.uuid_ ||
        view.attempted_ != seal.attempted_ || view.ready_ != seal.ready_ || view.failed_ != seal.failed_ ||
        !view.attempted_ || !view.ready_ || view.failed_ ||
        view.seed_ != seal.seed_ || view.first_data_ != seal.first_data_ ||
        view.blocks_per_group_ != seal.blocks_per_group_ || view.inodes_per_group_ != seal.inodes_per_group_ ||
        view.group_count_ != seal.group_count_ || view.inodes_ != seal.inodes_ ||
        view.first_inode_ != seal.first_inode_ || view.table_blocks_ != seal.table_blocks_ ||
        view.ownership_range_count_ != seal.range_count_ || view.ownership_range_count_ > view.ownership_ranges_.size() ||
        view.retained_orphan_ != seal.retained_orphan_ || view.retained_orphan_generation_ != seal.retained_orphan_generation_ ||
        view.retained_orphan_next_ != seal.retained_orphan_next_) return false;
    if (seal.target_minted_ && (proof.inode != seal.target_.inode || proof.generation != seal.target_.generation ||
        proof.old_size != seal.target_.old_size || proof.count != seal.target_.count || proof.group != seal.target_.group ||
        proof.mapping != seal.target_.mapping)) return false;
    for (usize at = 0; at < seal.groups_.size(); ++at)
    {
        const auto &actual = view.groups_[at];
        if (seal.groups_[at] != CompletedBirthSeal::Group{actual.block_bitmap, actual.inode_bitmap, actual.table,
                actual.block_checksum, actual.inode_checksum, actual.flags}) return false;
    }
    for (usize at = 0; at < seal.ranges_.size(); ++at)
        if (seal.ranges_[at] != CompletedBirthSeal::Range{view.ownership_ranges_[at].first, view.ownership_ranges_[at].count}) return false;
    return true;
}
template <jbd2::BlockReader Reader>
[[nodiscard]] static Status capture_completed_global(const Ext4ReadOnlyView<Reader> &view, const Reader &reader,
    const Ext4RecoveryInfo &binding, const Ext4ZeroTruncateOwnership &proof, CompletedBirthSeal &seal,
    CompletedBirthProgress &progress, const void *child)
{
    if (progress.stage_ != CompletedBirthProgress::Stage::proving ||
        !completed_birth_progress_exact(seal, progress, child) || !child || view.reader_ != &reader ||
        !view.attempted_ || !view.ready_ || view.failed_ || view.group_count_ > view.groups_.size() ||
        view.ownership_range_count_ > view.ownership_ranges_.size() || !same_birth_layout(view.layout_, binding.layout) ||
        view.uuid_ != binding.uuid || binding.metadata_count > binding.metadata.size())
        return Status::fault("completed ownership global birth lacks its actual checked view");
    seal.child_ = child; seal.view_ = &view; seal.reader_ = &reader; seal.proof_ = &proof;
    seal.original_binding_ = &binding; seal.binding_ = binding; seal.layout_ = view.layout_; seal.uuid_ = view.uuid_;
    seal.seed_ = view.seed_; seal.first_data_ = view.first_data_; seal.blocks_per_group_ = view.blocks_per_group_;
    seal.inodes_per_group_ = view.inodes_per_group_; seal.group_count_ = view.group_count_; seal.inodes_ = view.inodes_;
    seal.first_inode_ = view.first_inode_; seal.table_blocks_ = view.table_blocks_;
    seal.retained_orphan_ = view.retained_orphan_; seal.retained_orphan_generation_ = view.retained_orphan_generation_;
    seal.retained_orphan_next_ = view.retained_orphan_next_;
    seal.attempted_ = view.attempted_; seal.ready_ = view.ready_; seal.failed_ = view.failed_;
    seal.range_count_ = view.ownership_range_count_;
    for (usize at = 0; at < seal.groups_.size(); ++at)
    {
        const auto &group = view.groups_[at];
        seal.groups_[at] = {group.block_bitmap, group.inode_bitmap, group.table, group.block_checksum, group.inode_checksum, group.flags};
    }
    for (usize at = 0; at < seal.ranges_.size(); ++at)
        seal.ranges_[at] = {view.ownership_ranges_[at].first, view.ownership_ranges_[at].count};
    seal.global_minted_ = true;
    progress.stage_ = CompletedBirthProgress::Stage::global;
    return completed_birth_exact(view, reader, binding, proof, seal, child, false) ? Status{} :
        Status::fault("completed ownership global birth fields differ");
}
template <jbd2::BlockReader Reader>
[[nodiscard]] static Status capture_completed_target(const Ext4ReadOnlyView<Reader> &view, const Reader &reader,
    const Ext4RecoveryInfo &binding, const Ext4ZeroTruncateOwnership &proof, CompletedBirthSeal &seal,
    CompletedBirthProgress &progress, const void *child)
{
    if (progress.stage_ != CompletedBirthProgress::Stage::global ||
        !completed_birth_progress_exact(seal, progress, child) ||
        !completed_birth_exact(view, reader, binding, proof, seal, child, false))
        return Status::fault("completed ownership target birth lost its actual global view");
    seal.target_ = proof; seal.target_minted_ = true;
    progress.stage_ = CompletedBirthProgress::Stage::target;
    return completed_birth_exact(view, reader, binding, proof, seal, child, true) ? Status{} :
        Status::fault("completed ownership target birth fields differ");
}
