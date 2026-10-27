// Private authenticated request selection behind the finite VFS dispatch.
enum class TypedRequestKind : u8 { zero, refill };
enum class TypedRequestPhase : u8 { copied, preflighted, admitted, classified, request_admitted, child, suffix, published, refused, failed };
struct TypedRequest final
{
    TypedRequestKind kind{};
    u32 offset{}, bytes{}, size{};
    friend bool operator==(const TypedRequest &, const TypedRequest &) = default;
};
struct TypedRequestOperation;
struct TypedClassifierReader final
{
    Ext4MountedMetadata &mount;
    TypedRequestOperation &operation;
    [[nodiscard]] Status read_block(u32 block, std::span<u8> output)
    { return mount.typed_request_read(operation, block, output); }
};
using TypedClassifierView = Ext4ReadOnlyView<TypedClassifierReader>;
struct TypedRequestOperation final
{
    TypedRequestOperation(Ext4MountedMetadata &mount, const NodeMutationContext &original,
        typename Owner::TypedOwnerWorkspace &workspace, u64 serial, const TypedRequest &request,
        const std::array<u8, 1024> &payload, Ext4MountedTypedOpening opening)
        : original_(this), request_(request), request_seal_(request), payload_(payload), payload_seal_(payload),
          suffix_(mount, original, workspace, serial), publication_(suffix_), reader_{mount, *this}, opening_(opening) {}
    TypedRequestOperation(const TypedRequestOperation &) = delete;
    TypedRequestOperation &operator=(const TypedRequestOperation &) = delete;
    TypedRequestOperation(TypedRequestOperation &&) = delete;
    TypedRequestOperation &operator=(TypedRequestOperation &&) = delete;
    const TypedRequestOperation *const original_;
    const TypedRequest request_, request_seal_;
    const std::array<u8, 1024> payload_, payload_seal_;
    TypedSuffixWorkspace suffix_;
    TypedPublicationWorkspace publication_;
    TypedClassifierReader reader_;
    TypedClassifierView view_{};
    const Ext4MountedTypedOpening opening_;
    Ext4RecoveryInfo binding_{};
    std::array<u8, 1024> raw_{};
    std::array<u32, 16> mapping_{};
    Ext4ReadOnlyInode inode_{};
    Metadata metadata_{};
    Ext4ZeroTruncateUpdate zero_{};
    Ext4AllocatedEmptyDataUpdate refill_{};
    TypedRequestPhase phase_{TypedRequestPhase::copied};
    u32 blocks_{};
    usize clock_samples_{};
};
// Observation only: this record grants no request, child, receipt or renewal.
struct TypedRequestReport final
{
    Status status{};
    TypedRequestPhase phase{TypedRequestPhase::copied};
    u64 classifier_reads{}, suffix_reads{}, operation_serial{};
    usize clock_samples{};
    u32 inode{}, generation{}, old_size{};
    bool child_entered{}, receipt_consumed{}, renewed{}, published{}, epoch_committed{}, classified{};
};
struct TypedMutationStorage final
{
    explicit TypedMutationStorage(Ext4MountedMetadata &mount) : mount_(mount), original_(this) {}
    TypedMutationStorage(const TypedMutationStorage &) = delete;
    TypedMutationStorage &operator=(const TypedMutationStorage &) = delete;
    TypedMutationStorage(TypedMutationStorage &&) = delete;
    TypedMutationStorage &operator=(TypedMutationStorage &&) = delete;
    Ext4MountedMetadata &mount_;
    const TypedMutationStorage *const original_;
    std::optional<TypedRequestOperation> operation_;
    TypedRequest request_{};
    std::array<u8, 1024> payload_{};
    TypedRequestReport report_{};
    smp::SpinLock lock_{};
    bool busy_{};
};
template <typename Counter = block::SessionCounter>
[[nodiscard]] static Status typed_request_family_capacity(const block::SessionReport &initial,
    u32 groups, u64 blocks, Ext4MountedTypedOpening opening)
{
    bool any{};
    for (auto route : {Ext4MountedTypedRoute::zero_fresh, Ext4MountedTypedRoute::zero_recovery,
        Ext4MountedTypedRoute::zero_noop, Ext4MountedTypedRoute::allocated_fresh, Ext4MountedTypedRoute::allocated_recovery})
    {
        if (!TypedSuffixCursor::permitted(Ext4MountedTypedOrigin::mutation, opening, route)) continue;
        const auto selected = Ext4MountedTypedBudget::capacity<Counter>(initial, groups, blocks, Ext4MountedTypedOrigin::mutation, route);
        if (!selected) return selected.status();
        if (auto status = Ext4MountedTypedBudget::counters<block::SessionCounter>(initial, selected.value()); !status) return status;
        any = true;
    }
    return any ? Status{} : Status::invalid_argument("typed request opening family");
}
static void typed_project_classifier_view(TypedClassifierView &view,
    const TypedPublicationViewState &saved, TypedClassifierReader &reader)
{
    view.reader_ = &reader; view.layout_ = saved.layout; view.uuid_ = saved.uuid;
    view.groups_ = {};
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
[[nodiscard]] Status typed_request_exact(const TypedRequestOperation &operation, bool projection = true) const
{
    if (operation.original_ != &operation || operation.request_ != operation.request_seal_ ||
        operation.payload_ != operation.payload_seal_ || &operation.reader_.mount != this ||
        &operation.reader_.operation != &operation || &operation.publication_.suffix_ != &operation.suffix_ ||
        !reclaim_history::same_info(operation.binding_, operation.publication_.old_binding_))
        return Status::fault("typed request original copied record differs");
    if (auto status = typed_suffix_original_exact(operation.suffix_); !status) return status;
    if (auto status = typed_publication_old_mount_exact(operation.publication_); !status) return status;
    if (projection && !typed_publication_view_exact(operation.view_, operation.publication_.old_view_, &operation.reader_))
        return Status::fault("typed request complete checked View projection differs");
    return {};
}
[[nodiscard]] Status typed_request_fail(TypedRequestOperation &operation, Status status)
{
    operation.phase_ = TypedRequestPhase::failed;
    return typed_suffix_stop(operation.suffix_, status);
}
[[nodiscard]] Status typed_request_read(TypedRequestOperation &operation, u32 block, std::span<u8> output)
{
    const auto &layout = operation.binding_.layout;
    if (operation.phase_ != TypedRequestPhase::admitted || output.size() != 1024 || layout.block_size != 1024 ||
        block >= layout.filesystem_blocks || u64{block} > (~u64{0} - layout.first_sector) / 2)
        return typed_request_fail(operation, Status::denied("typed request classifier READ domain differs"));
    if (auto status = typed_request_exact(operation); !status) return typed_request_fail(operation, status);
    auto &work = operation.suffix_;
    const auto status = work.cursor_.read(work.context_, layout.first_sector + u64{block} * 2, std::as_writable_bytes(output));
    if (!status) return status;
    if (auto exact = typed_request_exact(operation); !exact) return typed_request_fail(operation, exact);
    return {};
}
[[nodiscard]] Status typed_request_refuse(TypedRequestOperation &operation, Status reason)
{
    if (auto status = typed_request_exact(operation, false); !status) return typed_request_fail(operation, status);
    const auto status = owner_.typed_cancel_mounted_cursor_before_child(operation.suffix_.attachment_, reason);
    operation.phase_ = operation.suffix_.cursor_.control_.phase == TypedSuffixPhase::refused ?
        TypedRequestPhase::refused : TypedRequestPhase::failed;
    return status;
}
[[nodiscard]] Status typed_classify_request(TypedRequestOperation &operation)
{
    using namespace ext4_checked;
    const auto key = operation.suffix_.original_.key;
    auto inode = operation.view_.inode(key.inode);
    if (!inode) return inode.status();
    operation.inode_ = inode.value();
    auto metadata = operation.view_.query_stored_metadata(key.inode);
    if (!metadata) return metadata.status();
    operation.metadata_ = metadata.value();
    if (!operation.view_.inodes_per_group_ || !key.inode || key.inode > operation.view_.inodes_)
        return Status::fault("typed request original inode bounds differ");
    const u32 local = (key.inode - 1) % operation.view_.inodes_per_group_;
    const u32 group = (key.inode - 1) / operation.view_.inodes_per_group_;
    if (group >= operation.view_.group_count_) return Status::fault("typed request original inode group differs");
    const u32 table = operation.view_.groups_[group].table + local / 4;
    if (auto status = typed_request_read(operation, table, operation.raw_); !status) return status;
    const auto raw = std::span<const u8, 256>{operation.raw_.data() + (local % 4) * 256, 256};
    if (auto status = inode_checksum(operation.view_.seed_, key.inode, raw); !status) return status;
    const auto decoded = operation.view_.information(key.inode, raw);
    const auto &actual = operation.inode_;
    if (le32(raw, 100) != key.generation || actual.generation != key.generation || actual.number != key.inode ||
        actual.number != decoded.number || actual.uid != decoded.uid || actual.gid != decoded.gid ||
        actual.size != decoded.size || actual.generation != decoded.generation || actual.mode != decoded.mode || actual.links != decoded.links)
        return Status::io_error("typed request actual raw inode generation or decoded target differs");
    auto projected = operation.view_.project_metadata(key.inode, raw);
    projected.attributes.mark(Attribute::blocks512);
    if (auto status = decode_ext4_times(std::as_bytes(raw), projected.attributes); !status) return status;
    const auto &stored = operation.metadata_;
    if (!typed_suffix_same_metadata(stored, projected) || stored.attributes.inode != projected.attributes.inode ||
        stored.attributes.valid != projected.attributes.valid || stored.attributes.device != projected.attributes.device ||
        stored.attributes.access.seconds != projected.attributes.access.seconds ||
        stored.attributes.access.nanoseconds != projected.attributes.access.nanoseconds)
        return Status::io_error("typed request checked stored metadata differs from physical inode");
    if (stored.type != NodeType::regular || key.inode < operation.view_.first_ordinary_inode() || (stored.mode & 06000))
        return Status::unsupported("typed request requires an ordinary regular target");
    if (auto status = Ext4ZeroTruncatePlanner::inspect_inode(operation.binding_, key.inode, raw,
        operation.mapping_, operation.blocks_); !status) return status;
    operation.phase_ = TypedRequestPhase::classified;
    return {};
}
template <typename Counter = block::SessionCounter>
[[nodiscard]] Status typed_run_fresh_request(TypedRequestOperation &operation)
{
    auto &work = operation.suffix_;
    // Preparation is deliberately before actual Owner admission.
    if (auto status = typed_begin_suffix_publication(operation.publication_); !status) return status;
    operation.binding_ = operation.publication_.old_binding_;
    if (!operation.binding_.empty_log || !operation.binding_.needs_recovery || operation.binding_.layout.block_size != 1024)
        return Status::unsupported("typed fresh request requires original empty dirty binding");
    const auto initial = work.session_.report(); if (!initial) return fail(initial.status());
    if (auto status = typed_request_family_capacity<Counter>(initial.value(), operation.publication_.old_view_.group_count,
        operation.binding_.layout.filesystem_blocks, operation.opening_); !status) return status;
    operation.phase_ = TypedRequestPhase::preflighted;
    typed_project_classifier_view(operation.view_, operation.publication_.old_view_, operation.reader_);
    if (auto status = typed_request_exact(operation); !status) return fail(status);
    if (auto status = owner_.typed_admit_mounted_cursor(work.attachment_); !status) return status;
    if (auto status = work.cursor_.template reserve<Counter>(work.context_, operation.view_.group_count_,
        operation.binding_.layout.filesystem_blocks, Ext4MountedTypedOrigin::mutation, operation.opening_); !status)
        return typed_request_fail(operation, status);
    operation.phase_ = TypedRequestPhase::admitted;
    if (auto status = typed_classify_request(operation); !status)
        return status.code() == StatusCode::unsupported || status.code() == StatusCode::not_found || status.code() == StatusCode::no_memory ?
            typed_request_refuse(operation, status) : typed_request_fail(operation, status);
    const bool noop = operation.request_.kind == TypedRequestKind::zero && !operation.inode_.size;
    if (operation.opening_ == Ext4MountedTypedOpening::noop_only && !noop)
        return typed_request_refuse(operation, Status::overflow("typed request exhausted epoch admits only actual zero no-op"));
    if (operation.request_.kind == TypedRequestKind::refill && (operation.inode_.size || operation.metadata_.blocks))
        return typed_request_refuse(operation, Status::unsupported("typed refill requires actual allocated-empty target"));
    const auto route = noop ? Ext4MountedTypedRoute::zero_noop : operation.request_.kind == TypedRequestKind::zero ?
        Ext4MountedTypedRoute::zero_fresh : Ext4MountedTypedRoute::allocated_fresh;
    // Ordinary sequence refusal remains an authentic classifier endpoint.
    const auto sequence = Ext4MountedTypedBudget::final_sequence(route, operation.binding_.journal_sequence);
    if (!sequence) return typed_request_refuse(operation, sequence.status());
    if (auto status = work.cursor_.refine(work.context_, route, operation.binding_.journal_sequence); !status)
        return typed_request_fail(operation, status);
    operation.phase_ = TypedRequestPhase::request_admitted;
    u32 seconds{}, nanoseconds{};
    if (!noop)
    {
        ++operation.clock_samples_;
        if (auto status = ordered_clock(seconds, nanoseconds); !status) return typed_request_refuse(operation, status);
    }
    operation.zero_ = {work.original_.key.inode, work.original_.key.generation, operation.inode_.size, seconds, nanoseconds};
    operation.refill_ = {work.original_.key.inode, work.original_.key.generation, operation.request_.size, seconds, nanoseconds};
    if (auto status = typed_request_exact(operation); !status) return typed_request_fail(operation, status);
    if (auto status = work.cursor_.pause_child(work.context_); !status) return typed_request_fail(operation, status);
    operation.phase_ = TypedRequestPhase::child;
    const auto child = operation.request_.kind == TypedRequestKind::refill ?
        owner_.run_typed_allocated_with_authority(work.owner_workspace_, operation.binding_, operation.refill_,
            operation.request_.offset, operation.payload_, work.authority_) :
        owner_.run_typed_zero_with_authority(work.owner_workspace_, operation.binding_, operation.zero_, work.authority_);
    if (!child) return typed_request_fail(operation, child);
    if (auto status = owner_.typed_resume_mounted_cursor(work.attachment_); !status) return typed_request_fail(operation, status);
    operation.phase_ = TypedRequestPhase::suffix;
    if (auto status = typed_run_semantic_suffix(work); !status) return typed_request_fail(operation, status);
    // This helper performs the sole genuine renewal and publication.
    if (auto status = typed_publish_completed_suffix(operation.publication_); !status) return status;
    operation.phase_ = TypedRequestPhase::published;
    return {};
}
template <typename Counter = block::SessionCounter>
[[nodiscard]] Status typed_private_request(MountedNode &node, typename Owner::TypedOwnerWorkspace &workspace,
    TypedMutationStorage &storage, TypedRequestKind kind, usize offset, usize size, std::span<const std::byte> input)
{
    const auto overlaps = [](const void *a, usize as, const void *b, usize bs) {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return as && bs && (one <= two ? two - one < as : one - two < bs);
    };
    if (&storage.mount_ != this || storage.original_ != &storage || overlaps(&storage, sizeof(storage), this, sizeof(*this)) ||
        overlaps(&storage, sizeof(storage), &workspace, sizeof(workspace)) || overlaps(&storage, sizeof(storage), input.data(), input.size()))
        return Status::invalid_argument("typed request original off-stack storage overlaps");
    if ((kind != TypedRequestKind::zero && kind != TypedRequestKind::refill) ||
        (kind == TypedRequestKind::zero && (size || offset || !input.empty())) ||
        (kind == TypedRequestKind::refill && (input.empty() || input.size() > 1024 || offset > 1024 || input.size() > 1024 - offset)))
        return Status::unsupported("typed request zero/refill range unavailable");
    {
        smp::ScopedSpinLock guard(storage.lock_);
        if (storage.busy_ || storage.operation_) return Status::busy("typed request storage retains an original operation");
        storage.busy_ = true; storage.report_ = {}; storage.payload_ = {};
        storage.request_ = {kind, u32(offset), u32(input.size()), kind == TypedRequestKind::refill ? u32(offset + input.size()) : 0U};
        for (usize at = 0; at < input.size(); ++at) storage.payload_[offset + at] = std::to_integer<u8>(input[at]);
    }
    const auto selected = [kind](u64 epoch) { return epoch == ~u64{0} ? Ext4MountedTypedOpening::noop_only :
        kind == TypedRequestKind::zero ? Ext4MountedTypedOpening::zero_family : Ext4MountedTypedOpening::allocated_family; };
    const auto status = typed_node_mutation_scope_select(node, selected,
        [&](const NodeMutationContext &original, TypedOriginalNodeAuthority &, Ext4MountedTypedOpening opening) -> Status {
        u64 serial{};
        {
            smp::ScopedSpinLock guard(owner_.lock_);
            if (owner_.typed_operation_serial_ == ~u64{0}) return Status::overflow("typed request operation serial exhausted");
            serial = owner_.typed_operation_serial_ + 1;
        }
        storage.operation_.emplace(*this, original, workspace, serial, storage.request_, storage.payload_, opening);
        auto &operation = *storage.operation_;
        const auto result = typed_run_fresh_request<Counter>(operation);
        const auto &work = operation.suffix_;
        storage.report_ = {result, operation.phase_, work.cursor_.control_.classifier_reads, work.cursor_.control_.suffix_reads,
            work.attachment_.operation_serial_, operation.clock_samples_, original.key.inode, original.key.generation, operation.inode_.size,
            work.owner_workspace_.attempted_ || work.attachment_.resumed_, work.attachment_.receipt_consumed_,
            work.attachment_.renewed_, operation.publication_.completed_, operation.publication_.epoch_committed_,
            operation.phase_ >= TypedRequestPhase::classified && operation.inode_.number == original.key.inode};
        // Actual Attachment/Context destruction precedes the enclosing scope's
        // final release of the original local Node context and occupied Lease.
        storage.operation_.reset();
        return result;
    });
    { smp::ScopedSpinLock guard(storage.lock_); storage.report_.status = status; storage.busy_ = false; }
    return status;
}
[[nodiscard]] Status typed_private_zero(MountedNode &node, typename Owner::TypedOwnerWorkspace &workspace,
    TypedMutationStorage &storage, usize size)
{ return typed_private_request(node, workspace, storage, TypedRequestKind::zero, 0, size, {}); }
[[nodiscard]] Status typed_private_refill(MountedNode &node, typename Owner::TypedOwnerWorkspace &workspace,
    TypedMutationStorage &storage, usize offset, std::span<const std::byte> input)
{ return typed_private_request(node, workspace, storage, TypedRequestKind::refill, offset, 0, input); }
