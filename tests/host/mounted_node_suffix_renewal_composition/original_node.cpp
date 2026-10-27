// Actual mounted Node/Lease admission model; no typed filesystem completion.
#include "ext4_mounted_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace owned_fixture;
namespace {
using Mount = Ext4MountedMetadata<2>;
usize checks{}, scopes{}, clean_refusals{}, admission_refusals{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "original Node:%d: %s\n", __LINE__, #x); std::abort(); } } while (false)
struct Fixture final {
    Disk disk;
    Mount::Registry registry;
    std::unique_ptr<Mount> mount{new Mount(Ext4MountedPolicy::ordered_data_dirty_noatime)};
    Mount::MountedNode *node{};
    explicit Fixture(const Bytes &image) : disk(image) {
        const auto registered = registry.register_device(disk, 62001, 65536); CHECK(registered);
        CHECK(mount->mount(registry, registered.value(), {0, image.size() / 512}));
        const auto found = mount->nodes_[0].lookup_child_retained("seed"); CHECK(found);
        for (auto &slot : mount->nodes_) if (&slot == found.value()) node = &slot;
        CHECK(node);
    }
    void released() {
        const auto active = mount->lifetime_.node_mutation_active(mount.get(), node->lifetime_key_.session);
        CHECK(active && !active.value());
        const auto held = mount->lifetime_.snapshot(node->lifetime_key_); CHECK(held);
        CHECK(!held.value().holds[static_cast<usize>(Ext4InodeHold::io)]);
        CHECK(!mount->mutation_context_ && !mount->mutation_lease_ && !mount->mutation_node_);
        CHECK(mount->report().phase == Ext4MountPhase::readable);
        CHECK(node->release());
    }
};
void original_roles(const Bytes &image) {
    for (bool maximum_epoch : {false, true}) for (bool refuse : {false, true}) {
        Fixture f(image);
        if (maximum_epoch) f.mount->report_.epoch = ~u64{0};
        const auto epoch = f.mount->report().epoch;
        const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
        const auto result = f.mount->typed_node_mutation_scope(*f.node,
            maximum_epoch ? Ext4MountedTypedOpening::noop_only : Ext4MountedTypedOpening::zero_family,
            [&](const auto &original, auto &authority) -> Status {
                CHECK(&original.node == f.node && &original.lease == f.mount->mutation_lease_);
                CHECK(&original == f.mount->mutation_context_ && original.key == f.node->lifetime_key_);
                CHECK(original.epoch == epoch);
                CHECK(f.mount->lifetime_.validate_node_mutation(original.lease, original.key, &original, epoch));
                CHECK(f.mount->admission().code() == StatusCode::busy);
                for (auto phase : {Ext4MountedTypedCursorPhase::classifier,
                        Ext4MountedTypedCursorPhase::paused_child, Ext4MountedTypedCursorPhase::suffix})
                    CHECK(authority.validate(phase, Ext4MountedTypedSuffixStage::none));
                CHECK(authority.complete_stage(Ext4MountedTypedSuffixStage::noop_terminal).code() == StatusCode::unsupported);
                ++scopes;
                if (refuse) { ++clean_refusals; return Status::unsupported("legal unprepared model refusal"); }
                return {};
            });
        CHECK(refuse ? result.code() == StatusCode::unsupported : bool(result));
        CHECK(f.mount->report().epoch == epoch);
        CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes);
        f.released();
    }
}
// Execute cancellation while the actual local Node role and Lease are held.
void composed_cancellations(const Bytes &image) {
    for (bool read_classifier : {false, true}) {
        Fixture f(image);
        auto workspace = std::make_unique<Mount::Owner::TypedOwnerWorkspace>();
        const auto epoch = f.mount->report().epoch;
        const auto writes = f.disk.writes, flushes = f.disk.flushes;
        for (u64 at = 0; at < 3; ++at) {
            const auto result = f.mount->typed_node_mutation_scope(*f.node,
                Ext4MountedTypedOpening::noop_only,
                [&](const auto &original, auto &) -> Status {
                    auto work = std::make_unique<Mount::TypedSuffixWorkspace>(*f.mount, original, *workspace, 730 + at);
                    auto &owner = f.mount->owner_;
                    CHECK(owner.typed_admit_mounted_cursor(work->attachment_));
                    CHECK(work->cursor_.reserve(work->context_, f.mount->view_.group_count_,
                        f.mount->binding_.layout.filesystem_blocks, Ext4MountedTypedOrigin::mutation,
                        Ext4MountedTypedOpening::noop_only));
                    if (read_classifier) {
                        const auto sector = f.mount->binding_.layout.first_sector +
                            u64{f.mount->view_.groups_[0].table} * 2;
                        CHECK(work->cursor_.read(work->context_, sector,
                            std::as_writable_bytes(std::span{work->raw_})));
                    }
                    CHECK(f.mount->lifetime_.validate_node_mutation(original.lease, original.key, &original, epoch));
                    const auto cancelled = owner.typed_cancel_mounted_cursor_before_child(work->attachment_,
                        Status::unsupported("actual original Node classifier request refused"));
                    CHECK(cancelled.code() == StatusCode::unsupported);
                    CHECK(!owner.typed_mounted_pending_ && !work->attachment_.admitted_);
                    CHECK(owner.typed_operation_serial_ == at + 1 && owner.typed_handoff_serial_ == 0);
                    CHECK(owner.typed_renewed_serial_ == 0 && !workspace->attempted_);
                    CHECK(f.mount->lifetime_.validate_node_mutation(original.lease, original.key, &original, epoch));
                    work.reset();
                    const auto actual = owner.typed_mounted_original_session().report(); CHECK(actual);
                    CHECK(actual.value().state == block::DeviceState::owned && actual.value().first_error == StatusCode::ok);
                    ++clean_refusals; ++scopes;
                    return cancelled;
                });
            CHECK(result.code() == StatusCode::unsupported && f.mount->report().epoch == epoch);
            CHECK(f.mount->report().phase == Ext4MountPhase::readable);
            const auto active = f.mount->lifetime_.node_mutation_active(f.mount.get(), f.node->lifetime_key_.session);
            CHECK(active && !active.value());
            CHECK(!f.mount->mutation_context_ && !f.mount->mutation_lease_ && !f.mount->mutation_node_);
        }
        CHECK(f.disk.writes == writes && f.disk.flushes == flushes);
        f.released();
    }
}
void admission_failures(const Bytes &image) {
    for (auto opening : {Ext4MountedTypedOpening::zero_family, Ext4MountedTypedOpening::allocated_family,
                         Ext4MountedTypedOpening::startup_recovery_family}) {
        Fixture f(image); f.mount->report_.epoch = ~u64{0};
        const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
        bool called{};
        const auto status = f.mount->typed_node_mutation_scope(*f.node, opening,
            [&](const auto &, auto &) -> Status { called = true; return {}; });
        CHECK(!status && !called);
        CHECK(status.code() == (opening == Ext4MountedTypedOpening::startup_recovery_family ?
            StatusCode::unsupported : StatusCode::overflow));
        CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes);
        f.released(); ++admission_refusals;
    }
    Fixture f(image);
    Mount::MountedNode copied = *f.node;
    const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
    const auto status = f.mount->typed_node_mutation_scope(copied, Ext4MountedTypedOpening::noop_only,
        [](const auto &, auto &) -> Status { std::abort(); });
    CHECK(status.code() == StatusCode::bad_descriptor);
    CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes);
    f.released(); ++admission_refusals;
}
}
int main(int argc, char **argv) {
    CHECK(argc == 2); std::ifstream stream(argv[1], std::ios::binary); CHECK(stream);
    const Bytes image((std::istreambuf_iterator<char>(stream)), {});
    CHECK(image.size() == 32 * 1024 * 1024);
    original_roles(image); admission_failures(image); composed_cancellations(image);
    std::printf("PASS actual original Node authority: checks=%zu scopes=%zu clean_refusals=%zu admission_refusals=%zu; admission-only, typed filesystem completion uncomposed\n",
        checks, scopes, clean_refusals, admission_refusals);
}
