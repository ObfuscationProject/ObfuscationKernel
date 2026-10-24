// Reuse image helpers only. The inherited field-comparison functions and main
// are never executed; no live records, pointers or transport responses change.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main retained_birth_main
#include "../completed_ownership_birth/model.cpp"
#undef main
#pragma GCC diagnostic pop

namespace
{
using Suffix = Mount::TypedSuffixWorkspace;
using Publication = Mount::TypedPublicationWorkspace;
using Route = Ext4MountedTypedRoute;
using Opening = Ext4MountedTypedOpening;
usize publications{}, epochs{}, noops{}, quiet_refusals{}, repeated{}, original_roles{};
struct PublicationDevice final : driver::BlockDevice
{
    std::array<Block, 8705> &blocks;
    const u64 origin{37};
    usize reads{}, writes{}, flushes{};
    explicit PublicationDevice(std::array<Block, 8705> &value) : blocks(value) {}
    driver::BlockGeometry geometry() const override { return {origin + blocks.size() * 2, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> output) override
    {
        ++reads; CHECK(sector >= origin && (sector - origin) % 2 == 0 && output.size() == 1024);
        const auto block = (sector - origin) / 2; CHECK(block < blocks.size());
        for (usize at = 0; at < output.size(); ++at) output[at] = std::byte(blocks[block][at]);
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
struct PublicationFixture final
{
    Image image;
    PublicationDevice device;
    Owner::Registry registry;
    std::unique_ptr<Mount> mount{new Mount(Ext4MountedPolicy::ordered_data_dirty_noatime)};
    std::unique_ptr<Workspace> owner_workspace{new Workspace};
    Mount::MountedNode *node{};
    u64 operation{};
    explicit PublicationFixture(bool allocated, bool recovery, bool cross, u32 count, u8 mask, bool maximum_epoch = false)
        : image(allocated ? 0 : count, cross), device(image.blocks)
    {
        media(image, false, false, 0, allocated ? 0 : count);
        const auto key = registry.register_device(device, 21000 + fixtures); CHECK(key);
        CHECK(mount->mount(registry, key.value(), {device.origin, image.blocks.size() * 2}));
        const auto target = mount->node(13, "target"); CHECK(target);
        node = static_cast<Mount::MountedNode *>(target.value());
        // Declare the selected media fixture before original mutation admission.
        image.blocks[1000] = {}; media(image, allocated, recovery, mask, count);
        for (u32 block = 600; block < 616; ++block)
        { Owner::Pin pin; CHECK(mount->owner_.pin(block, pin)); CHECK(pin.release()); }
        // Declare exhausted mount epoch before the first original mutation role.
        // Native Session history, workspace serial and callbacks are untouched.
        if (maximum_epoch) mount->report_.epoch = ~u64{0};
        ++fixtures;
    }
    void run(bool allocated, bool recovery, bool noop, bool again = false)
    {
        const auto input = again ? mount->binding_ : image.f.binding;
        const auto route = allocated ? recovery ? Route::allocated_recovery : Route::allocated_fresh :
            recovery ? Route::zero_recovery : noop ? Route::zero_noop : Route::zero_fresh;
        const auto opening = allocated ? Opening::allocated_family : noop ? Opening::noop_only : Opening::zero_family;
        const auto old_epoch = mount->report_.epoch;
        const auto old_binding = mount->binding_;
        const Bytes old_view(mount->view_);
        const auto role_before = mount->lifetime_.node_mutation_active(mount.get(), mount->identity_.session_serial);
        CHECK(role_before && !role_before.value());
        const auto status = mount->typed_node_mutation_scope(*node, opening,
            [&](const Mount::NodeMutationContext &original, auto &authority) -> Status {
            CHECK(authority.validate(Ext4MountedTypedCursorPhase::unused, Ext4MountedTypedSuffixStage::none));
            auto work = std::unique_ptr<Suffix>(new Suffix(*mount, original, *owner_workspace, 700 + ++operation));
            auto publication = std::unique_ptr<Publication>(new Publication(*work));
            CHECK(mount->typed_begin_suffix_publication(*publication));
            CHECK(mount->typed_begin_suffix_publication(*publication).code() == StatusCode::already_exists); ++quiet_refusals;
            const auto early = work->session_.report().value();
            CHECK(mount->typed_publish_completed_suffix(*publication).code() == StatusCode::busy); ++quiet_refusals;
            CHECK(reclaim_history::same_session(early, work->session_.report().value()));
            CHECK(mount->owner_.typed_admit_mounted_cursor(work->attachment_));
            CHECK(work->cursor_.reserve(work->context_, u32(input.metadata_count / 4), input.layout.filesystem_blocks,
                Ext4MountedTypedOrigin::mutation, opening));
            std::array<std::byte, 1024> classifier{};
            CHECK(work->cursor_.read(work->context_, 16, classifier));
            CHECK(work->cursor_.refine(work->context_, route, input.journal_sequence));
            CHECK(work->cursor_.pause_child(work->context_));
            const auto child = allocated ? mount->owner_.run_typed_allocated_with_authority(*owner_workspace, input,
                {13, 9, 73, 1700000222, 123456789}, 7, data, work->authority_, recovery) :
                mount->owner_.run_typed_zero_with_authority(*owner_workspace, input, image.update(), work->authority_, recovery);
            if (!child) std::fprintf(stderr, "publication child route=%u code=%u message=%.*s\n", unsigned(route),
                unsigned(child.code()), int(child.message().size()), child.message().data());
            CHECK(child); CHECK(mount->owner_.typed_resume_mounted_cursor(work->attachment_));
            const auto pending = work->session_.report().value();
            CHECK(mount->typed_publish_completed_suffix(*publication).code() == StatusCode::busy); ++quiet_refusals;
            CHECK(reclaim_history::same_session(pending, work->session_.report().value()));
            CHECK(mount->typed_run_semantic_suffix(*work));
            const auto suffix = work->session_.report().value();
            const auto reads = device.reads, writes = device.writes, flushes = device.flushes;
            CHECK(mount->typed_publish_completed_suffix(*publication));
            CHECK(publication->completed_ && publication->renewed_ && publication->epoch_committed_ == !noop);
            CHECK(work->attachment_.renewed_ && !work->attachment_.admitted_);
            CHECK(mount->typed_publication_renewed_exact(*publication));
            CHECK(reclaim_history::same_session(suffix, work->session_.report().value()));
            CHECK(device.reads == reads && device.writes == writes && device.flushes == flushes);
            CHECK(mount->report_.phase == Ext4MountPhase::updating && mount->report_.epoch == old_epoch + usize(!noop));
            CHECK(mount->owner_.publication_domain_ == old_epoch && original.epoch == old_epoch);
            CHECK(original.lease.valid() && mount->mutation_context_ == &original && mount->mutation_lease_ == &original.lease);
            { smp::ScopedSpinLock guard(mount->lock_); CHECK(mount->mutation_integrity_locked(&original)); }
            const auto active = mount->lifetime_.node_mutation_active(mount.get(), mount->identity_.session_serial);
            CHECK(active && active.value()); ++original_roles;
            CHECK(mount->view_.reader_ == &mount->reader_ && mount->view_.valid());
            if (noop)
            {
                CHECK(old_view.same(mount->view_) && reclaim_history::same_info(old_binding, mount->binding_));
                CHECK(!work->view_.attempted_ && work->cursor_.report().suffix_reads == 1); ++noops;
            }
            else
            {
                CHECK(mount->typed_publication_view_exact(mount->view_, publication->fresh_view_, &mount->reader_));
                CHECK(reclaim_history::same_info(mount->binding_, publication->final_binding_));
                CHECK(mount->view_.ownership_range_count_ == 0); ++epochs;
            }
            CHECK(mount->typed_publish_completed_suffix(*publication).code() == StatusCode::already_exists); ++quiet_refusals;
            CHECK(reclaim_history::same_session(suffix, work->session_.report().value()));
            CHECK(device.reads == reads && device.writes == writes && device.flushes == flushes);
            CHECK(mount->owner_.mounted_teardown_preflight());
            return {};
        });
        if (!status) std::fprintf(stderr, "publication scope route=%u code=%u message=%.*s\n", unsigned(route),
            unsigned(status.code()), int(status.message().size()), status.message().data());
        CHECK(status);
        CHECK(mount->report_.phase == Ext4MountPhase::readable && mount->report_.epoch == old_epoch + usize(!noop));
        CHECK(!mount->mutation_context_ && !mount->mutation_lease_ && !mount->mutation_node_);
        const auto active = mount->lifetime_.node_mutation_active(mount.get(), mount->identity_.session_serial);
        CHECK(active && !active.value());
        const auto metadata = node->query_metadata(); CHECK(metadata && metadata.value().size == (allocated ? 73U : 0U));
        ++publications; if (again) ++repeated;
    }
};
void run_publication(bool allocated, bool recovery, bool cross, u32 count, u8 mask = 0)
{
    PublicationFixture fixture(allocated, recovery, cross, count, mask);
    fixture.run(allocated, recovery, !allocated && !recovery && !count);
    // Ordinary later no-op uses the genuinely published binding and renewed
    // same Owner workspace, without replacing medium or retained records.
    if (!allocated) fixture.run(false, false, true, true);
}
}
int main()
{
    for (bool cross : {false, true})
    {
        {
            PublicationFixture exhausted(false, false, cross, 0, 0, true);
            const auto writes = exhausted.device.writes, flushes = exhausted.device.flushes;
            exhausted.run(false, false, true);
            exhausted.run(false, false, true, true);
            CHECK(exhausted.mount->report_.epoch == ~u64{0});
            CHECK(exhausted.device.writes == writes && exhausted.device.flushes == flushes);
        }
        {
            PublicationFixture nonempty(false, false, cross, 3, 0, true);
            bool called{}; const auto reads = nonempty.device.reads;
            const auto writes = nonempty.device.writes, flushes = nonempty.device.flushes;
            const auto refused = nonempty.mount->typed_node_mutation_scope(*nonempty.node,
                Opening::zero_family, [&](const auto &, auto &) -> Status { called = true; return {}; });
            CHECK(refused.code() == StatusCode::overflow && !called);
            CHECK(nonempty.device.reads == reads && nonempty.device.writes == writes && nonempty.device.flushes == flushes);
            CHECK(!nonempty.mount->mutation_context_ && !nonempty.mount->mutation_lease_);
            CHECK(nonempty.mount->report_.epoch == ~u64{0} && nonempty.mount->report_.phase == Ext4MountPhase::readable);
        }
        run_publication(false, false, cross, 0);
        run_publication(false, false, cross, 3);
        run_publication(true, false, cross, 0);
        for (u8 mask : {u8{0}, u8{5}, u8{15}})
        {
            run_publication(false, true, cross, 3, mask);
            run_publication(true, true, cross, 0, mask);
        }
    }
    std::printf("MOUNTED_SUFFIX_VIEW_PUBLICATION publications=%zu epochs=%zu noops=%zu repeated=%zu quiet_refusals=%zu original_roles=%zu checks=%zu publication_reads=0\n",
        publications, epochs, noops, repeated, quiet_refusals, original_roles, checks);
    return 0;
}
