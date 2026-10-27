// Actual hosted VFS/provider and memory-device I/O; no guest execution.
#include "ext4_mounted_support.hpp"
#include "ext4_mounted_retention_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <memory>
#include <atomic>
#include <thread>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL mounted orphan:%d: %s\n", __LINE__, #expression); std::abort(); \
} } while (false)
static void checked(Status status, const char *label)
{
    if (!status) { std::fprintf(stderr, "FAIL %s: %.*s\n", label,
        int(status.message().size()), status.message().data()); std::abort(); }
}
using Mount = Ext4MountedMetadata<1>;
struct Mounted
{
    Disk disk;
    Mount::Registry registry;
    std::unique_ptr<Ext4MountedReclaimWorkspace> workspace = std::make_unique<Ext4MountedReclaimWorkspace>();
    std::unique_ptr<Mount> mount;
    std::unique_ptr<VirtualFileSystem> vfs = std::make_unique<VirtualFileSystem>();
    Mount::Registry::Registration registration{};
    VfsMountHandle attachment{};
    explicit Mounted(const Bytes &image) : disk(image), mount(std::make_unique<Mount>(
        Ext4MountedPolicy::ordered_data_dirty_noatime, workspace.get()))
    {
        auto registered = registry.register_device(disk, 201); CHECK(registered);
        registration = registered.value();
        checked(mount->mount(registry, registration, {0, image.size() / 512}), "mount");
        checked(vfs->create("/mnt", NodeType::directory), "mountpoint");
        auto publication = publish_owned_root(*mount, *vfs, "/mnt"); CHECK(publication);
        attachment = publication.value();
    }
    Status drain() { return detach_and_drain(*mount, *vfs, attachment); }
};
static Bytes load(const char *path)
{
    std::ifstream input(path, std::ios::binary); CHECK(input);
    Bytes result((std::istreambuf_iterator<char>(input)), {});
    CHECK(result.size() == 32 * 1024 * 1024); return result;
}
static bool allocated(const Bytes &image, u32 inode)
{
    const u32 group = (inode - 1) / 2048, within = (inode - 1) % 2048;
    const usize bitmap = r32(image, descriptor(image, group) + 4) * 1024;
    return image[bitmap + within / 8] & (1U << (within % 8));
}
static void quarantined(Mounted &h)
{
    CHECK(h.mount->report().phase == Ext4MountPhase::failed);
    CHECK(h.mount->buffers().state == MetadataOwnerState::failed);
    CHECK(h.registry.inspect(h.registration).value().state == block::DeviceState::quarantined);
    const auto writes = h.disk.writes, flushes = h.disk.flushes;
    CHECK(!h.mount->sync() && !h.mount->freeze_clean());
    CHECK(!h.vfs->create("/mnt/forbidden", NodeType::regular));
    CHECK(h.disk.writes == writes && h.disk.flushes == flushes);
}
struct Count { usize reads{}, writes{}, flushes{}; };
static void actual_pending_owner_precedes_seal(const Bytes &filled)
{
    for (usize variant = 0; variant < 3; ++variant)
    {
        auto h = std::make_unique<Mounted>(filled);
        auto held = h->vfs->resolve({}, "/mnt/seed"); CHECK(held);
        checked(h->vfs->unlink("/mnt/seed"), "pending owner unlink");
        const auto pending = h->mount->pending_orphan_; CHECK(pending);
        CHECK(h->mount->owner_.report().orphan_cleanup_pending);
        CHECK(h->mount->owner_.report().phase == Ext4OwnedPhase::orphan_cleanup_pending);
        const auto original_owner = h->mount->owner_.report_;
        const bool namespace_active = h->mount->owner_.namespace_active_;
        const bool orphan = h->mount->owner_.namespace_completion_.orphan_;
        // Erasing only the mount's deferred pointer cannot erase the actual Owner
        // obligation, even after every authentic descriptor and root was consumed.
        h->mount->pending_orphan_ = nullptr;
        if (variant)
        {
            h->mount->owner_.report_.phase = Ext4OwnedPhase::open;
            h->mount->owner_.report_.orphan_cleanup_pending = false;
        }
        if (variant == 2)
        {
            h->mount->owner_.namespace_active_ = false;
            h->mount->owner_.namespace_completion_.orphan_ = false;
        }
        checked(held.value().release(), "consume descriptor with missing deferred pointer");
        checked(h->vfs->detach(h->attachment), "detach pending owner root");
        CHECK(h->mount->lifetime_snapshot(12).value().total() == 0);
        const Count before{h->disk.reads, h->disk.writes, h->disk.flushes};
        CHECK(h->mount->begin_drain().code() == StatusCode::busy);
        CHECK(h->mount->freeze_clean().code() == StatusCode::busy);
        CHECK(h->mount->close().code() == StatusCode::busy);
        CHECK(h->mount->report().phase == Ext4MountPhase::readable);
        CHECK(h->disk.reads == before.reads && h->disk.writes == before.writes && h->disk.flushes == before.flushes);
        CHECK(r32(h->disk.durable, 1120) & 4);
        CHECK(allocated(h->disk.durable, 12) && r32(h->disk.durable, 1256) == 12);
        h->mount->owner_.report_ = original_owner;
        h->mount->owner_.namespace_active_ = namespace_active;
        h->mount->owner_.namespace_completion_.orphan_ = orphan;
        h->mount->pending_orphan_ = pending;
        checked(h->mount->sync(), "finish restored authentic pending owner");
        CHECK(!h->mount->lifetime_snapshot(12) && !h->mount->owner_.report().orphan_cleanup_pending);
        checked(h->mount->begin_drain(), "drain after actual owner reclaim");
        checked(h->mount->freeze_clean(), "seal after actual owner reclaim");
        CHECK(!(r32(h->disk.durable, 1120) & 4));
    }
    std::puts("PASS mounted orphan owner preflight: actual_pending=3 erased_deferred_pointer=1 erased_owner_summary=1 erased_namespace_flags=1 zero_leases=1 no_seal_io=1 restored_reclaim=3");
}
static Count retained_unlink(const Bytes &filled, const char *output)
{
    auto h = std::make_unique<Mounted>(filled);
    auto held = h->vfs->resolve({}, "/mnt/seed"); CHECK(held);
    auto *old = held.value().node();
    auto before = old->read(0, 8); CHECK(before);
    checked(h->vfs->unlink("/mnt/seed"), "unlink retained");
    CHECK(!h->vfs->resolve({}, "/mnt/seed"));
    CHECK(r32(h->disk.durable, 1024 + 232) == 12 && allocated(h->disk.durable, 12));
    auto detached = h->mount->lifetime_snapshot(12); CHECK(detached);
    CHECK(detached.value().phase == Ext4InodeLifetimePhase::retired);
    auto after = old->read(0, 8); CHECK(after && after.value().size == before.value().size);
    CHECK(after.value().data == before.value().data);
    auto meta = old->query_metadata(); CHECK(meta && meta.value().link_count == 0);
    checked(old->retain(), "clone detached descriptor");
    checked(old->release(), "release detached clone");
    CHECK(old->write(0, std::span<const std::byte>{before.value().data}.first(1)).code() == StatusCode::busy);
    CHECK(h->mount->freeze_clean().code() == StatusCode::busy);
    const Count start{h->disk.reads, h->disk.writes, h->disk.flushes};
    checked(held.value().release(), "final release reclaim");
    const Count counts{h->disk.reads - start.reads, h->disk.writes - start.writes, h->disk.flushes - start.flushes};
    CHECK(!h->mount->lifetime_snapshot(12));
    CHECK(r32(h->disk.durable, 1024 + 232) == 0 && !allocated(h->disk.durable, 12));
    CHECK(h->disk.live == h->disk.durable && h->disk.pending.empty());
    CHECK(h->mount->report().phase == Ext4MountPhase::readable);
    checked(h->drain(), "detach after reclaim");
    checked(h->mount->freeze_clean(), "clean freeze after reclaim");
    save(output, h->disk.durable);
    return counts;
}
static void replacement_and_reuse(const Bytes &filled)
{
    auto h = std::make_unique<Mounted>(filled);
    auto source = h->vfs->resolve({}, "/mnt/basic-note"); CHECK(source);
    auto victim = h->vfs->resolve({}, "/mnt/second-note"); CHECK(victim);
    auto old = victim.value().node()->read(0, 8); CHECK(old);
    const auto generation = h->mount->lifetime_snapshot(15).value().key.generation;
    auto *retired_address = victim.value().node();
    checked(h->vfs->rename("/mnt/basic-note", "/mnt/second-note"), "replace existing target");
    CHECK(!h->vfs->resolve({}, "/mnt/basic-note"));
    auto target = h->vfs->resolve({}, "/mnt/second-note"); CHECK(target);
    CHECK(target.value().node() == source.value().node());
    CHECK(target.value().node() != retired_address);
    auto old_after = retired_address->read(0, 8); CHECK(old_after && old_after.value().data == old.value().data);
    CHECK(r32(h->disk.durable, 1256) == 15 && allocated(h->disk.durable, 15));
    checked(victim.value().release(), "replacement final release");
    CHECK(!h->mount->lifetime_snapshot(15) && !allocated(h->disk.durable, 15));
    checked(source.value().release(), "release source");
    checked(target.value().release(), "release new name");
    auto parent = h->vfs->resolve({}, "/mnt"); CHECK(parent);
    auto created = parent.value().node()->create_child_retained("replacement-reuse", {NodeType::regular, 0640, 1000, 1000});
    CHECK(created);
    auto m = created.value()->query_metadata(); CHECK(m);
    CHECK(m.value().attributes.inode == 15 && created.value() == retired_address);
    const auto new_key = h->mount->lifetime_snapshot(15).value().key;
    CHECK(new_key.generation != generation);
    const std::array<std::byte, 4> text{std::byte{'T'}, std::byte{'E'}, std::byte{'X'}, std::byte{'T'}};
    checked(created.value()->write(0, text), "write reused inode");
    auto content = created.value()->read(0, 4); CHECK(content && content.value().size == 4);
    CHECK(std::equal(text.begin(), text.end(), content.value().data.begin()));
    checked(created.value()->release(), "release reused inode");
    checked(parent.value().release(), "release parent");
    checked(h->drain(), "replacement detach and drain");
    checked(h->mount->freeze_clean(), "replacement clean freeze");
}
static void io_release_and_raw(const Bytes &filled)
{
    {
        auto h = std::make_unique<Mounted>(filled);
        auto held = h->vfs->resolve({}, "/mnt/seed"); CHECK(held);
        auto *old = held.value().node();
        checked(h->vfs->unlink("/mnt/seed"), "unlink before in-flight release");
        bool released{};
        h->disk.hook = [&](char operation) {
            const auto report = h->mount->report();
            if (operation == 'r' && !released && report.reading)
            {
                released = true;
                checked(held.value().release(), "final descriptor release during read");
                auto state = h->mount->lifetime_snapshot(12); CHECK(state);
                CHECK(state.value().holds[static_cast<usize>(Ext4InodeHold::io)] == 1);
                CHECK(allocated(h->disk.durable, 12));
            }
        };
        auto bytes = old->read(0, 8); CHECK(bytes && released);
        h->disk.hook = {};
        CHECK(!h->mount->lifetime_snapshot(12) && !allocated(h->disk.durable, 12));
    }
    {
        auto h = std::make_unique<Mounted>(filled);
        auto *raw = h->vfs->lookup("/mnt/seed"); CHECK(raw);
        checked(h->vfs->unlink("/mnt/seed"), "unlink raw exposure");
        CHECK(h->mount->lifetime_snapshot(12).value().phase == Ext4InodeLifetimePhase::ready);
        CHECK(allocated(h->disk.durable, 12) && r32(h->disk.durable, 1256) == 12);
        CHECK(h->mount->freeze_clean().code() == StatusCode::busy);
        CHECK(h->vfs->create("/mnt/later", NodeType::regular).code() == StatusCode::busy);
        const Count start{h->disk.reads, h->disk.writes, h->disk.flushes};
        CHECK(raw->query_metadata().status().code() == StatusCode::busy);
        CHECK(h->disk.reads == start.reads && h->disk.writes == start.writes && h->disk.flushes == start.flushes);
        CHECK(h->mount->report().phase == Ext4MountPhase::readable);
        CHECK(h->vfs->read_file("/mnt/basic-note"));
    }
}
static void immutable_names(const Bytes &filled)
{
    auto h = std::make_unique<Mounted>(filled);
    auto held = h->vfs->resolve({}, "/mnt/seed"); CHECK(held);
    auto *node = held.value().node();
    const auto original = node->name(); CHECK(original == "seed");
    std::atomic<bool> done{}, started{};
    std::atomic<usize> samples{};
    std::thread reader([&] {
        started.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire))
        {
            CHECK(original == "seed");
            const auto current = node->name();
            CHECK(current == "seed" || current == "aaaa" || current == "bbbb");
            ++samples;
        }
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    std::string_view prior = "/mnt/seed";
    for (usize iteration = 0; iteration < 15; ++iteration)
    {
        const auto next = iteration % 2 ? "/mnt/bbbb" : "/mnt/aaaa";
        checked(h->vfs->rename(prior, next), "concurrent name rename");
        prior = next;
    }
    const Count before{h->disk.reads, h->disk.writes, h->disk.flushes};
    CHECK(h->vfs->rename(prior, "/mnt/exhausted-name").code() == StatusCode::no_memory);
    CHECK(h->disk.writes == before.writes && h->disk.flushes == before.flushes);
    CHECK(h->mount->report().phase == Ext4MountPhase::readable);
    done.store(true, std::memory_order_release); reader.join();
    CHECK(samples > 0 && original == "seed" && node->name() == "aaaa");
    checked(held.value().release(), "immutable name release");
}
static void empty_and_workspace(const Bytes &filled)
{
    auto h = std::make_unique<Mounted>(filled);
    auto parent = h->vfs->resolve({}, "/mnt"); CHECK(parent);
    auto empty = parent.value().node()->create_child_retained("empty-runtime", {NodeType::regular, 0640, 1000, 1000});
    CHECK(empty);
    const auto inode = empty.value()->query_metadata().value().attributes.inode;
    auto *address = empty.value();
    checked(empty.value()->release(), "empty initial release");
    checked(parent.value().release(), "empty parent release");
    checked(h->vfs->unlink("/mnt/empty-runtime"), "unlink without an existing descriptor");
    CHECK(!h->mount->lifetime_snapshot(inode) && !allocated(h->disk.durable, inode));
    auto next_parent = h->vfs->resolve({}, "/mnt"); CHECK(next_parent);
    auto reuse = next_parent.value().node()->create_child_retained("empty-reused", {NodeType::regular, 0600, 1000, 1000});
    checked(reuse.status(), "empty reused create");
    CHECK(reuse.value() == address);
    CHECK(reuse.value()->query_metadata().value().attributes.inode == inode);
    checked(reuse.value()->release(), "empty reuse release");
    checked(next_parent.value().release(), "empty next parent release");

    // The supplied workspace is a mount-lifetime resource even after clean
    // shutdown. Another actual session cannot recycle its claimant address.
    checked(h->drain(), "workspace owner detach and drain");
    checked(h->mount->freeze_clean(), "workspace owner freeze");
    Disk other(filled); Mount::Registry registry;
    auto registered = registry.register_device(other, 202); CHECK(registered);
    auto rejected = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime, h->workspace.get());
    CHECK(rejected->mount(registry, registered.value(), {0, filled.size() / 512}).code() == StatusCode::busy);
    CHECK(other.reads == 0 && other.writes == 0 && other.flushes == 0);
    CHECK(registry.inspect(registered.value()).value().state == block::DeviceState::ready);
}
static void errors(const Bytes &filled, Count counts)
{
    usize write_faults{}, flush_faults{}, read_faults{};
    for (usize offset = 1; offset <= counts.writes; ++offset)
        for (usize cut : {usize{0}, usize{512}, usize{1024}})
        {
            auto h = std::make_unique<Mounted>(filled);
            auto held = h->vfs->resolve({}, "/mnt/seed"); CHECK(held);
            auto *old = held.value().node();
            checked(h->vfs->unlink("/mnt/seed"), "fault setup unlink");
            h->disk.fail_write = h->disk.writes + offset; h->disk.cut = cut; h->disk.persist_mask = 0xffffffffU;
            CHECK(!held.value().release());
            CHECK(!held.value().node()); // Error still consumes the reference.
            CHECK(old->release().code() == StatusCode::bad_descriptor);
            quarantined(*h);
            CHECK(h->mount->lifetime_snapshot(12).value().phase == Ext4InodeLifetimePhase::publishing);
            ++write_faults;
        }
    for (usize offset = 1; offset <= counts.flushes; ++offset)
        for (bool after : {false, true})
        {
            auto h = std::make_unique<Mounted>(filled);
            auto held = h->vfs->resolve({}, "/mnt/seed"); CHECK(held);
            checked(h->vfs->unlink("/mnt/seed"), "flush fault setup");
            if (after) h->disk.fail_flush_after = h->disk.flushes + offset;
            else { h->disk.fail_flush = h->disk.flushes + offset; h->disk.persist_mask = 0xffffffffU; }
            CHECK(!held.value().release()); quarantined(*h); ++flush_faults;
        }
    for (usize offset : {usize{1}, counts.reads / 2, counts.reads})
    {
        auto h = std::make_unique<Mounted>(filled);
        auto held = h->vfs->resolve({}, "/mnt/seed"); CHECK(held);
        checked(h->vfs->unlink("/mnt/seed"), "read fault setup");
        h->disk.fail_read = h->disk.reads + offset;
        CHECK(!held.value().release()); quarantined(*h); ++read_faults;
    }
    std::printf("PASS mounted orphan faults: write_cuts=%zu flush_failures=%zu read_failures=%zu\n",
                write_faults, flush_faults, read_faults);
}
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    auto filled = load(argv[1]);
    testing::ext4_clock::set({{1700000666, 123456789}, {200, 1}, 1,
                             time::RealtimeOrigin::administrator_software});
    actual_pending_owner_precedes_seal(filled);
    const auto counts = retained_unlink(filled, argv[2]);
    replacement_and_reuse(filled);
    io_release_and_raw(filled);
    empty_and_workspace(filled);
    immutable_names(filled);
    errors(filled, counts);
    std::puts("PASS mounted orphan runtime: retained unlink read clone durable final-release reclaim");
}
