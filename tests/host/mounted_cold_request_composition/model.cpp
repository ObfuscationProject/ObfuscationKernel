// Hosted private composition; all media comes from genuine native writers.
#include "../mounted_typed_cold_startup/fixture.hpp"
#include "../ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>

using namespace typed_cold_startup_fixture;
namespace {
using Mount = Ext4MountedMetadata<2>;
using Work = Mount::TypedColdWorkspace;
using Publication = Mount::TypedColdPublication;
using Storage = Mount::TypedMutationStorage;
usize checks{}, recoveries{}, mutations{}, noops{}, refusals{}, read_bytes{};
#define CHECK(x) do { ++checks; require(bool(x), #x); } while (false)

void clock_ready()
{
    testing::ext4_clock::reset();
    testing::ext4_clock::set({{1700000333, 123456789}, {1700000333000000000ULL, 1}, 1,
        time::RealtimeOrigin::administrator_software});
}

struct Fixture final {
    Device device;
    Owner::Registry registry{};
    std::unique_ptr<Ext4MountedStartupWorkspace> startup{new Ext4MountedStartupWorkspace};
    // The first child address remains permanently borrowed until Mount dies.
    std::unique_ptr<Owner::TypedOwnerWorkspace> child{new Owner::TypedOwnerWorkspace};
    std::unique_ptr<Mount> mount{new Mount(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, startup.get())};
    std::unique_ptr<Storage> mutation{new Storage(*mount)};
    explicit Fixture(const std::vector<u8> &bytes) : device(bytes) {}
    void recover()
    {
        auto key = registry.register_device(device, 47000 + recoveries); CHECK(key);
        require(mount->typed_cold_mount_scope(registry, key.value(), {0, image_bytes / 512}, *child,
            [&](const auto &original) {
                std::unique_ptr<Work> work{new Work(*mount, original, *child)};
                std::unique_ptr<Publication> publication{new Publication(*work)};
                return mount->typed_run_cold_operation(*work, *publication);
            }), "composed cold recovery");
        CHECK(!mount->typed_cold_opening_);
        CHECK(child->owner_ == &mount->owner_ && child->phase_ == Owner::TypedOwnerPhase::ready);
        ++recoveries;
    }
    void endpoint()
    {
        auto actual = mount->owner_.session_report(); CHECK(actual);
        CHECK(actual.value().state == block::DeviceState::owned);
        CHECK(actual.value().reads == device.reads && actual.value().completed_reads == device.reads);
        CHECK(actual.value().writes == device.write_attempts && actual.value().completed_writes == device.write_attempts);
        CHECK(actual.value().flushes == device.flush_attempts && actual.value().completed_flushes == device.flush_attempts);
        CHECK(!actual.value().in_flight && !actual.value().pending_writes);
        CHECK(actual.value().session_serial == mount->identity_.session_serial);
        CHECK(child->owner_ == &mount->owner_ && child->original_ == child.get());
        CHECK(mount->owner_.typed_attachment_->workspace_.owner_ == &mount->owner_);
        CHECK(&mount->owner_.typed_attachment_->workspace_ == child.get());
        CHECK(!mount->mutation_context_ && !mount->mutation_lease_);
        CHECK(!mutation->busy_ && !mutation->operation_);
        CHECK(mount->owner_.mounted_teardown_preflight());
    }
    void zero(Mount::MountedNode &node, bool noop)
    {
        clock_ready(); const auto epoch = mount->report().epoch;
        const auto before = mount->owner_.session_report(); CHECK(before);
        require(mount->typed_private_zero(node, *child, *mutation, 0), "composed zero request");
        CHECK(mount->report().epoch == epoch + usize(!noop));
        CHECK(mutation->report_.receipt_consumed && mutation->report_.renewed && mutation->report_.published);
        CHECK(testing::ext4_clock::calls() == usize(!noop));
        const auto after = mount->owner_.session_report(); CHECK(after);
        CHECK(after.value().session_serial == before.value().session_serial);
        if (noop) {
            CHECK(after.value().writes == before.value().writes && after.value().flushes == before.value().flushes);
            ++noops;
        } else ++mutations;
        endpoint();
        const auto metadata = node.query_metadata(); CHECK(metadata && metadata.value().size == 0);
        endpoint();
    }
    void refill(Mount::MountedNode &node)
    {
        std::array<std::byte, 66> text{};
        for (usize at = 0; at < text.size(); ++at) text[at] = std::byte('A' + at % 26);
        const auto epoch = mount->report().epoch; clock_ready();
        require(mount->typed_private_refill(node, *child, *mutation, 7, text), "composed refill request");
        CHECK(mount->report().epoch == epoch + 1 && testing::ext4_clock::calls() == 1);
        CHECK(mutation->report_.receipt_consumed && mutation->report_.renewed && mutation->report_.published);
        endpoint();
        const auto metadata = node.query_metadata(); CHECK(metadata && metadata.value().size == 73);
        const auto read = node.read(0, 73); CHECK(read && read.value().size == 73);
        for (usize at = 0; at < 7; ++at) CHECK(read.value().data[at] == std::byte{});
        for (usize at = 0; at < text.size(); ++at) CHECK(read.value().data[at + 7] == text[at]);
        read_bytes += 73; ++mutations; endpoint();
    }
};

void lifecycle(const NativePreparation &prepared, u8 mask)
{
    CHECK(prepared.writer_crash_reachable(mask));
    Fixture f(prepared.cut(mask)); f.recover();
    CHECK(f.device.bytes == prepared.completed());
    auto retained = f.mount->retain_root(); CHECK(retained);
    auto root = retained.value().node_for_mount(); CHECK(root);
    auto lookup = root.value()->lookup_child_retained(prepared.allocated ? "empty" : "zero"); CHECK(lookup);
    auto &node = *static_cast<Mount::MountedNode *>(lookup.value());
    CHECK(node.inode_ == (prepared.allocated ? allocated_inode : zero_inode));
    f.endpoint();
    f.zero(node, !prepared.allocated);
    f.zero(node, true);
    for (usize cycle = 0; cycle < 2; ++cycle) {
        f.refill(node); f.zero(node, false); f.zero(node, true);
    }
    auto foreign = std::unique_ptr<Owner::TypedOwnerWorkspace>(new Owner::TypedOwnerWorkspace);
    const auto before = f.mount->owner_.session_report(); CHECK(before);
    const auto rejected = f.mount->typed_private_zero(node, *foreign, *f.mutation, 0);
    CHECK(!rejected && rejected.code() == StatusCode::bad_descriptor);
    const auto after = f.mount->owner_.session_report(); CHECK(after);
    CHECK(reclaim_history::same_session(before.value(), after.value()));
    CHECK(!foreign->original_ && !foreign->attempted_); ++refusals;
    f.endpoint();
    CHECK(node.release()); CHECK(retained.value().release());
    CHECK(f.mount->close());
    CHECK(f.mount->report().phase == Ext4MountPhase::closed);
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const auto input = load(argv[1]);
    for (bool allocated : {false, true}) {
        const auto native = prepare(input, allocated);
        lifecycle(native, 0); lifecycle(native, 15);
    }
    std::printf("{\"passed\":true,\"recoveries\":%zu,\"mutations\":%zu,\"noops\":%zu,"
        "\"foreign_workspace_refusals\":%zu,\"read_bytes\":%zu,\"checks\":%zu}\n",
        recoveries, mutations, noops, refusals, read_bytes, checks);
}
