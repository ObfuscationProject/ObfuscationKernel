// Safe real ext4/native Session fixture. Reuse image creation only; none of the
// retained birth comparison functions or inherited main is executed.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main retained_birth_main
#include "../completed_ownership_birth/model.cpp"
#undef main
#pragma GCC diagnostic pop

namespace
{
using Suffix = Mount::TypedSuffixWorkspace;
using Stage = Ext4MountedTypedSuffixStage;
using CursorPhase = Ext4MountedTypedCursorPhase;
using Route = Ext4MountedTypedRoute;
using Opening = Ext4MountedTypedOpening;
usize positives{}, failures{}, native_suffix_reads{}, semantic_completions{};
struct SuffixDevice final : driver::BlockDevice
{
    std::array<Block, 8705> &blocks;
    Suffix *suffix{};
    const u64 origin{37};
    usize reads{}, writes{}, flushes{}, denied_reads{};
    std::array<usize, 10> stage_reads{};
    Stage fail_stage{Stage::none};
    bool invalid_raw_response{};
    explicit SuffixDevice(std::array<Block, 8705> &value) : blocks(value) {}
    driver::BlockGeometry geometry() const override { return {origin + blocks.size() * 2, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> output) override
    {
        ++reads;
        CHECK(sector >= origin && (sector - origin) % 2 == 0 && output.size() == 1024);
        const auto block = (sector - origin) / 2; CHECK(block < blocks.size());
        Stage stage = Stage::none;
        if (suffix && suffix->cursor_.report().phase == CursorPhase::suffix)
        {
            stage = suffix->cursor_.report().stage; CHECK(stage != Stage::none);
            ++stage_reads[usize(stage)]; ++native_suffix_reads;
            CHECK(suffix->mount_.typed_suffix_original_exact(*suffix));
            CHECK(suffix->mount_.owner_.typed_mounted_cursor_exact(suffix->attachment_));
            const auto report = suffix->session_.report(); CHECK(report && report.value().in_flight);
            CHECK(report.value().completed_reads + 1 == report.value().reads);
            if (stage == fail_stage) { ++denied_reads; return Status::io_error("declared semantic stage media failure"); }
        }
        for (usize at = 0; at < output.size(); ++at) output[at] = std::byte(blocks[block][at]);
        // A declared invalid transport response, never a changed kernel record
        // or medium. The real checksum helper must refuse this successful READ.
        if (invalid_raw_response && stage == Stage::raw_target) output[100] ^= std::byte{1};
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> input) override
    {
        ++writes; CHECK(sector >= origin && (sector - origin) % 2 == 0 && input.size() == 1024);
        const auto block = (sector - origin) / 2; CHECK(block < blocks.size());
        for (usize at = 0; at < input.size(); ++at) blocks[block][at] = std::to_integer<u8>(input[at]);
        return {};
    }
    Status flush() override { ++flushes; return {}; }
};
struct SemanticFixture final
{
    Image image;
    SuffixDevice device;
    Owner::Registry registry;
    std::unique_ptr<Mount> mount{new Mount(Ext4MountedPolicy::ordered_data_dirty_noatime)};
    std::unique_ptr<Workspace> owner_workspace{new Workspace};
    std::optional<Mount::Lease> lease;
    std::optional<Mount::NodeMutationContext> original;
    std::unique_ptr<Suffix> suffix;
    Route route;
    std::array<std::byte, 1024> classifier{};
    explicit SemanticFixture(bool allocated, bool recovery, bool cross, u32 count, u8 mask = 0)
        : image(allocated ? 0 : count, cross), device(image.blocks),
          route(allocated ? recovery ? Route::allocated_recovery : Route::allocated_fresh :
              recovery ? Route::zero_recovery : count ? Route::zero_fresh : Route::zero_noop)
    {
        // Establish the actual mounted Node/lifetime under the existing empty
        // checksum-only startup. The selected typed fixture is then declared
        // before its first classifier/child READ, including revoke-capable logs.
        media(image, false, false, 0, allocated ? 0 : count);
        const auto key = registry.register_device(device, 17000 + fixtures); CHECK(key);
        CHECK(mount->mount(registry, key.value(), {device.origin, image.blocks.size() * 2}));
        CHECK(mount->view_.valid());
        const auto node = mount->node(13, "target"); CHECK(node);
        auto &actual_node = *static_cast<Mount::MountedNode *>(node.value());
        image.blocks[1000] = {};
        media(image, allocated, recovery, mask, count);
        // Complete fixture setup before original mutation admission. A recovery
        // profile can have different declared home images; ordinary cache misses
        // evict the former mounting entries without editing any cache record.
        for (u32 block = 600; block < 616; ++block)
        { Owner::Pin pin; CHECK(mount->owner_.pin(block, pin)); CHECK(pin.release()); }
        {
            smp::ScopedSpinLock lock(mount->lock_);
            const auto admitted = mount->update_admission_locked(Ext4MountedPolicy::ordered_data_dirty_noatime); CHECK(admitted);
            CHECK(mount->authentic_node_locked(actual_node));
            const auto identity = actual_node.lifetime_key_;
            auto retained = mount->retain_existing(identity, Ext4InodeHold::io); CHECK(retained);
            lease.emplace(std::move(retained).value());
            original.emplace(actual_node, identity, *lease, admitted.value());
            mount->mutation_node_ = &actual_node; mount->mutation_key_ = identity;
            mount->mutation_lease_ = &*lease; mount->mutation_context_ = &*original;
            mount->report_.phase = Ext4MountPhase::updating;
            CHECK(mount->lifetime_.claim_node_mutation(*lease, identity, &*original, admitted.value()));
        }
        mount->owner_.publication_domain_ = original->epoch;
        suffix.reset(new Suffix(*mount, *original, *owner_workspace, 37 + fixtures));
        device.suffix = suffix.get();
        CHECK(mount->typed_suffix_original_exact(*suffix));
        CHECK(mount->owner_.typed_admit_mounted_cursor(suffix->attachment_));
        CHECK(suffix->cursor_.reserve(suffix->context_, u32(image.f.binding.metadata_count / 4),
            image.f.binding.layout.filesystem_blocks, Ext4MountedTypedOrigin::mutation,
            allocated ? Opening::allocated_family : route == Route::zero_noop ? Opening::noop_only : Opening::zero_family));
        CHECK(suffix->cursor_.read(suffix->context_, 16, classifier));
        CHECK(suffix->cursor_.refine(suffix->context_, route, image.f.binding.journal_sequence));
        CHECK(suffix->cursor_.pause_child(suffix->context_));
        const auto child_status = allocated ? mount->owner_.run_typed_allocated(*owner_workspace, image.f.binding,
            {13, 9, 73, 1700000222, 123456789}, 7, data, recovery) :
            mount->owner_.run_typed_zero(*owner_workspace, image.f.binding, image.update(), recovery);
        if (!child_status) std::fprintf(stderr, "semantic fixture child failed route=%u code=%u message=%.*s\n",
            unsigned(route), unsigned(child_status.code()), int(child_status.message().size()), child_status.message().data());
        CHECK(child_status);
        CHECK(mount->owner_.typed_resume_mounted_cursor(suffix->attachment_));
        ++fixtures;
    }
    ~SemanticFixture()
    {
        device.suffix = nullptr; suffix.reset();
        // Terminal cleanup consumes the actual original local role. It grants
        // no rearm/renewal or epoch commit; the pending Owner stays quarantined.
        CHECK(mount->lifetime_.release_node_mutation(*lease, original->key, &*original, original->epoch));
        mount->mutation_node_ = nullptr; mount->mutation_key_ = {};
        mount->mutation_lease_ = nullptr; mount->mutation_context_ = nullptr;
    }
};
void successful_semantics(bool allocated, bool recovery, bool cross, u32 count, u8 mask = 0)
{
    SemanticFixture f(allocated, recovery, cross, count, mask);
    auto &work = *f.suffix; const auto child = work.attachment_.child_terminal_;
    const auto before = work.session_.report().value(); const auto epoch = f.mount->report_.epoch;
    const auto writes = f.device.writes, flushes = f.device.flushes;
    const Bytes original_view(f.mount->view_);
    CHECK(f.mount->typed_run_semantic_suffix(work));
    const auto after = work.session_.report().value();
    CHECK(work.finished_ && work.receipt_.minted_ && work.receipt_.consumed_ && work.attachment_.receipt_consumed_);
    CHECK(work.cursor_.report().phase == CursorPhase::closed);
    CHECK(work.cursor_.report().suffix_reads == after.reads - before.reads);
    CHECK(after.reads == after.completed_reads && !after.in_flight && !after.pending_writes);
    CHECK(after.writes == before.writes && after.completed_writes == before.completed_writes);
    CHECK(after.flushes == before.flushes && after.completed_flushes == before.completed_flushes);
    CHECK(f.device.writes == writes && f.device.flushes == flushes);
    CHECK(reclaim_history::same_session(child, work.attachment_.child_terminal_));
    CHECK(f.mount->typed_suffix_original_exact(work));
    CHECK(f.mount->owner_.typed_mounted_consumed_receipt_exact(work.attachment_, work.receipt_));
    CHECK(f.mount->report_.epoch == epoch && original_view.same(f.mount->view_));
    if (f.route == Route::zero_noop)
    {
        CHECK(after.reads - before.reads == 1 && work.semantic_mask_ == (u16{1} << usize(Stage::noop_terminal)));
        CHECK(!work.view_.attempted_ && work.inspection_.report().phase == Ext4CompletionPhase::idle);
        CHECK(f.device.stage_reads[usize(Stage::noop_terminal)] == 1); ++semantic_completions;
    }
    else
    {
        CHECK(work.view_.valid() && work.view_.reader_ == &work.reader_ && work.semantic_mask_ == 254);
        CHECK(work.inspection_.report().phase == Ext4CompletionPhase::preflight);
        CHECK(work.inspection_.report().write_attempts == 0 && work.inspection_.report().flush_attempts == 0);
        for (Stage stage : {Stage::inspect_empty, Stage::reopen_view, Stage::stored_metadata,
            Stage::target_inode, Stage::raw_target, Stage::target_metadata, Stage::final_wrapper})
        { CHECK(f.device.stage_reads[usize(stage)] != 0); ++semantic_completions; }
        CHECK(f.device.stage_reads[usize(Stage::raw_target)] == 1 && f.device.stage_reads[usize(Stage::final_wrapper)] == 1);
    }
    const auto stable = after;
    CHECK(f.mount->typed_run_semantic_suffix(work).code() == StatusCode::already_exists);
    CHECK(reclaim_history::same_session(stable, work.session_.report().value()));
    Owner::Pin denied; CHECK(!f.mount->owner_.pin(8, denied)); CHECK(!f.mount->owner_.rearm());
    CHECK(reclaim_history::same_session(stable, work.session_.report().value())); ++positives;
}
void declared_failure(Stage selected, bool invalid_response = false)
{
    const bool noop = selected == Stage::noop_terminal;
    SemanticFixture f(false, false, false, noop ? 0U : 3U);
    auto &work = *f.suffix;
    const auto before = work.session_.report().value();
    const auto writes = f.device.writes, flushes = f.device.flushes;
    if (invalid_response) f.device.invalid_raw_response = true; else f.device.fail_stage = selected;
    CHECK(f.mount->typed_run_semantic_suffix(work).code() == StatusCode::io_error);
    const auto after = work.session_.report().value();
    CHECK(!work.finished_ && !work.receipt_.minted_ && !work.receipt_.consumed_);
    CHECK(after.state == block::DeviceState::quarantined && after.first_error == StatusCode::io_error && !after.in_flight);
    CHECK(after.reads == after.completed_reads + usize(!invalid_response));
    CHECK(after.writes == before.writes && after.flushes == before.flushes && !after.pending_writes);
    CHECK(f.device.writes == writes && f.device.flushes == flushes);
    CHECK(!(work.semantic_mask_ & (u16{1} << usize(selected))));
    CHECK(f.device.denied_reads == usize(!invalid_response)); ++failures;
}
}
int main()
{
    for (bool cross : {false, true})
    {
        successful_semantics(false, false, cross, 0);
        successful_semantics(false, false, cross, 3);
        successful_semantics(true, false, cross, 0);
        for (u8 mask : {u8{0}, u8{5}, u8{15}})
        {
            successful_semantics(false, true, cross, 3, mask);
            successful_semantics(true, true, cross, 0, mask);
        }
    }
    for (Stage stage : {Stage::inspect_empty, Stage::reopen_view, Stage::stored_metadata,
        Stage::target_inode, Stage::raw_target, Stage::target_metadata, Stage::final_wrapper, Stage::noop_terminal})
        declared_failure(stage);
    declared_failure(Stage::raw_target, true);
    std::printf("MOUNTED_TYPED_SUFFIX_SEMANTICS positives=%zu failures=%zu checks=%zu native_suffix_reads=%zu semantic_completions=%zu epoch_commits=0 writer_calls=0 renewal=0\n",
        positives, failures, checks, native_suffix_reads, semantic_completions);
    return 0;
}
