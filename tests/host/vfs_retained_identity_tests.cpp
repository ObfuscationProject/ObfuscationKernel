// Hosted actual-VFS handoff and private mounted-bookkeeping tests. No inode,
// block, Node identity or device backing is freed by the mounted test below.
#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4_mounted_metadata.hpp"

#include <mutex>
#include <thread>
#include <memory>
#include <chrono>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "retained identity:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

namespace
{
usize races{}, blocked_reuse{}, raw_blocks{}, metadata_errors{}, ram_retry_checks{}, reconfigure_checks{};

class HandoffNode final : public Node
{
  public:
    explicit HandoffNode(NodeType type) : type_(type) {}
    std::string_view name() const override { return type_ == NodeType::directory ? "/" : "file"; }
    Metadata metadata() const override
    {
        Metadata result{.type = type_, .mode = mode_for(type_, 0777)};
        result.attributes.inode = identity; result.attributes.mark(Attribute::inode);
        return result;
    }
    Result<Metadata> query_metadata() const override
    {
        std::lock_guard guard(lock);
        if (type_ == NodeType::regular && !references)
            return Status::fault("handoff metadata lacked a retain");
        if (fail_metadata) return Status::io_error("metadata refusal");
        return metadata();
    }
    Status retain() override
    { std::lock_guard guard(lock); ++references; return {}; }
    Status release() override
    { std::lock_guard guard(lock); CHECK(references); --references; return {}; }
    Status expose_borrowed_identity() override
    {
        std::lock_guard guard(lock);
        if (!allow_raw) return Status::unsupported("model refuses raw identity");
        CHECK(references); raw_exposed = true; return {};
    }
    Node *lookup(std::string_view) override { ++bare_lookups; return nullptr; }
    Result<Node *> lookup_child(std::string_view name) override
    { return null_child ? Result<Node *>{static_cast<Node *>(nullptr)} : Node::lookup_child(name); }
    Result<Node *> lookup_child_retained(std::string_view name) override
    {
        auto *target = name == ".." ? this : name == "file" && linked ? child : nullptr;
        if (!target) return Status::not_found("model child absent");
        CHECK(target->retain());
        // Real host threads race provider retirement during the VFS handoff.
        // No provider mutex is held across this callback or worker join.
        std::thread worker([&] {
            ++races;
            if (!target->reuse()) ++blocked_reuse;
        });
        worker.join();
        return static_cast<Node *>(target);
    }
    Result<Node *> create_child(std::string_view, const InitialNodeAttributes &) override
    { ++bare_creates; return Status::fault("unretained create handoff reached"); }
    Result<Node *> create_child_retained(std::string_view name, const InitialNodeAttributes &) override
    {
        CHECK(name == "new" && child && !linked); linked = true;
        CHECK(child->retain());
        std::thread worker([&] {
            ++races; if (!child->reuse()) ++blocked_reuse;
        });
        worker.join();
        return static_cast<Node *>(child);
    }
    Status remove(std::string_view name, bool) override
    {
        CHECK(name == "file" && child && linked);
        CHECK(!child->reuse()); linked = false; return {};
    }
    Result<FileBuffer> read(usize, usize) const override { return FileBuffer{}; }
    Status write(usize, std::span<const std::byte>) override { return {}; }
    Status create(std::string_view, NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return {}; }
    Status chown(u32, u32) override { return {}; }
    bool reuse()
    {
        std::lock_guard guard(lock);
        if (references || raw_exposed) return false;
        ++identity; return true;
    }
    mutable std::mutex lock;
    HandoffNode *child{};
    usize references{}, bare_lookups{}, bare_creates{};
    u64 identity{41};
    bool linked{true}, raw_exposed{}, allow_raw{true}, fail_metadata{}, null_child{};
  private:
    NodeType type_;
};

void vfs_handoff_and_raw_model()
{
    auto storage = std::make_unique<VirtualFileSystem>(); auto &vfs = *storage;
    HandoffNode root(NodeType::directory), file(NodeType::regular); root.child = &file;
    CHECK(vfs.mount("/", root));
    auto resolved = vfs.resolve({}, "/file"); CHECK(resolved);
    CHECK(resolved.value().node() == &file && file.identity == 41 && file.references == 1);
    CHECK(!file.reuse()); CHECK(resolved.value().release()); CHECK(file.reuse());
    CHECK(file.identity == 42);
    file.fail_metadata = true;
    auto failure = vfs.resolve({}, "/file"); CHECK(!failure && failure.status().code() == StatusCode::io_error);
    CHECK(file.references == 0 && root.references == 0); ++metadata_errors;
    file.fail_metadata = false;
    file.allow_raw = false; CHECK(vfs.lookup("/file") == nullptr);
    CHECK(!file.references && !file.raw_exposed);
    file.allow_raw = true; auto *raw = vfs.lookup("/file"); CHECK(raw == &file && file.raw_exposed);
    CHECK(!file.references && !file.reuse()); ++raw_blocks;
    const auto identity = file.identity; CHECK(raw->retain()); CHECK(raw->release());
    CHECK(file.identity == identity && !file.reuse()); ++raw_blocks;
    CHECK(root.bare_lookups == 0 && file.bare_lookups == 0);

    HandoffNode second_root(NodeType::directory), second_file(NodeType::regular);
    second_root.child = &second_file; second_root.linked = false;
    auto create_storage = std::make_unique<VirtualFileSystem>();
    CHECK(create_storage->mount("/", second_root));
    auto created = create_storage->create_file({}, "/new", 0644, 0, 0, false);
    CHECK(created && created.value().node() == &second_file && second_file.references == 1);
    CHECK(!second_file.raw_exposed && second_root.bare_creates == 0);
    CHECK(created.value().release() && second_file.reuse());

    auto remove_storage = std::make_unique<VirtualFileSystem>();
    HandoffNode remove_root(NodeType::directory), victim(NodeType::regular); remove_root.child = &victim;
    CHECK(remove_storage->mount("/", remove_root)); CHECK(remove_storage->remove({}, "/file", false));
    CHECK(!victim.references && !victim.raw_exposed && victim.reuse());
    CHECK(remove_root.bare_lookups == 0);
    root.null_child = true;
    const auto null_child = root.Node::lookup_child_retained("file");
    CHECK(!null_child && null_child.status().code() == StatusCode::io_error);
    CHECK(!root.references && !file.references);
}

void ram_raw_slot_reuse_model()
{
    auto storage = std::make_unique<VirtualFileSystem>(); auto &vfs = *storage;
    CHECK(vfs.create("/raw", NodeType::regular)); auto *raw = vfs.lookup("/raw"); CHECK(raw);
    const auto identity = raw->query_metadata().value().attributes.inode;
    CHECK(vfs.unlink("/raw"));
    auto *ram_raw = static_cast<RamNode *>(raw);
    CHECK(ram_raw->configure("new-identity", NodeType::regular, nullptr).code() == StatusCode::busy);
    CHECK(ram_raw->configure_readonly("new-identity", {}, 0444).code() == StatusCode::busy);
    reconfigure_checks += 2;
    for (usize index = 0; index < 400; ++index)
    {
        CHECK(vfs.create("/next", NodeType::regular)); auto scoped = vfs.resolve({}, "/next"); CHECK(scoped);
        CHECK(scoped.value().node() != raw); CHECK(scoped.value().release()); CHECK(vfs.unlink("/next"));
    }
    CHECK(raw->query_metadata().value().attributes.inode == identity); ++raw_blocks;
    CHECK(!raw->retain());
    auto root = vfs.resolve({}, "/"); CHECK(root);
    CHECK(vfs.create("/direct", NodeType::regular)); auto direct = root.value().node()->lookup_child("direct"); CHECK(direct);
    CHECK(vfs.unlink("/direct")); CHECK(vfs.create("/replacement", NodeType::regular));
    auto replacement = vfs.resolve({}, "/replacement"); CHECK(replacement && replacement.value().node() != direct.value());
    ++raw_blocks;

    RamNode held; CHECK(held.configure("held", NodeType::regular, nullptr)); CHECK(held.retain());
    CHECK(held.configure("changed", NodeType::regular, nullptr).code() == StatusCode::busy);
    CHECK(held.configure_readonly("changed", {}, 0444).code() == StatusCode::busy);
    reconfigure_checks += 2; CHECK(held.release());

    RamNode parent, linked; CHECK(parent.configure("/", NodeType::directory, nullptr));
    CHECK(linked.configure("live", NodeType::regular, nullptr)); CHECK(parent.attach_child(linked));
    CHECK(!linked.references_ && !linked.borrowed_identity_exposed_);
    CHECK(linked.configure("different", NodeType::directory, nullptr).code() == StatusCode::busy);
    ++reconfigure_checks;
    auto preserved = parent.lookup_child_retained("live"); CHECK(preserved && preserved.value() == &linked);
    CHECK(linked.name() == "live" && linked.metadata().type == NodeType::regular && linked.metadata().link_count == 1);
    CHECK(preserved.value()->release() && !linked.references_);

    CHECK(vfs.create_readonly_file("/published", {}, 0444)); auto *published = vfs.lookup("/published"); CHECK(published);
    const auto published_identity = published->query_metadata().value().attributes.inode;
    const std::array<std::byte, 1> replacement_data{std::byte{'x'}};
    CHECK(vfs.create_readonly_file("/published", replacement_data, 0444, true));
    CHECK(published == vfs.lookup("/published") && published->query_metadata().value().attributes.inode == published_identity);
    CHECK(published->read(0, 1).value().data[0] == std::byte{'x'});
}

void ram_second_lock_retry_model()
{
    for (bool raw : {false, true})
    {
        RamNode parent, child; CHECK(parent.configure("/", NodeType::directory, nullptr));
        CHECK(child.configure("child", NodeType::directory, nullptr)); CHECK(parent.attach_child(child));
        child.lock_.lock();
        std::atomic<bool> started{};
        std::thread worker([&] {
            started.store(true, std::memory_order_release);
            if (raw) CHECK(parent.lookup("child") == &child);
            else
            {
                auto result = parent.lookup_child_retained("child"); CHECK(result && result.value() == &child);
                CHECK(result.value()->release());
            }
        });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
        bool observed_locked{}, observed_release{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (parent.lock_.try_lock())
            {
                parent.lock_.unlock();
                if (observed_locked) { observed_release = true; break; }
            }
            else observed_locked = true;
            std::this_thread::yield();
        }
        // The worker must drop the parent while the target lock is still held.
        // A blocking parent->target implementation deterministically fails here.
        CHECK(observed_locked && observed_release);
        child.lock_.unlock(); worker.join(); ++ram_retry_checks;
        CHECK(!child.references_ && child.borrowed_identity_exposed_ == raw);
    }
}

using Mount = Ext4MountedMetadata<1>;
struct Mounted
{
    Disk disk;
    Mount::Registry registry;
    Mount mount;
    explicit Mounted(const Bytes &image) : disk(image)
    {
        auto key = registry.register_device(disk, 121); CHECK(key);
        CHECK(mount.mount(registry, key.value(), {0, image.size() / 512}));
    }
};

void mounted_retirement_admission_model(const Bytes &image)
{
    auto fixture = std::make_unique<Mounted>(image);
    auto *root = fixture->mount.root(); CHECK(root);
    auto child = root->lookup_child_retained("seed"); CHECK(child);
    auto *slot = static_cast<Mount::MountedNode *>(child.value());
    CHECK(slot->references_ == 1 && !slot->borrowed_identity_exposed_);
    const auto key = slot->lifetime_key_;
    CHECK(!fixture->mount.report_.reading);
    // Synthetic bookkeeping retirement only. Disk namespace/link count is
    // deliberately unchanged; this test grants no actual unlink/free authority.
    CHECK(fixture->mount.lifetime_.mark_orphan(key));
    fixture->mount.report_.phase = Ext4MountPhase::updating;
    auto held = fixture->mount.lifetime_.schedule_exact(key, [](const auto &) -> Status { return {}; });
    CHECK(!held && held.code() == StatusCode::busy);
    CHECK(child.value()->release());
    Mount::Lifetime::Ticket ticket{};
    CHECK(fixture->mount.lifetime_.schedule_exact(key, [&](const auto &issued) -> Status { ticket = issued; return {}; }));
    CHECK(fixture->mount.validate_node_retirement(ticket));
    CHECK(slot->inode_ == 12 && fixture->disk.writes == 0 && fixture->disk.flushes == 0);

    auto legacy = std::make_unique<Mounted>(image); auto *legacy_root = legacy->mount.root(); CHECK(legacy_root);
    auto raw = legacy_root->lookup_child("seed"); CHECK(raw);
    auto *raw_slot = static_cast<Mount::MountedNode *>(raw.value());
    CHECK(raw_slot->borrowed_identity_exposed_ && !raw_slot->references_);
    CHECK(legacy->mount.lifetime_.mark_orphan(raw_slot->lifetime_key_));
    legacy->mount.report_.phase = Ext4MountPhase::updating;
    CHECK(legacy->mount.lifetime_.schedule_exact(raw_slot->lifetime_key_, [&](const auto &issued) -> Status {
        ticket = issued; return {};
    }));
    const auto blocked = legacy->mount.validate_node_retirement(ticket);
    CHECK(!blocked && blocked.code() == StatusCode::busy); ++raw_blocks;
    CHECK(raw_slot->inode_ == 12 && legacy->disk.writes == 0 && legacy->disk.flushes == 0);

    auto reserved = std::make_unique<Mounted>(image);
    auto *reserved_root = static_cast<Mount::MountedNode *>(reserved->mount.root()); CHECK(reserved_root);
    CHECK(reserved_root->inode_ == 2);
    CHECK(reserved->mount.lifetime_.mark_orphan(reserved_root->lifetime_key_));
    reserved->mount.report_.phase = Ext4MountPhase::updating;
    CHECK(reserved->mount.lifetime_.schedule_exact(reserved_root->lifetime_key_, [&](const auto &issued) -> Status {
        ticket = issued; return {};
    }));
    const auto denied = reserved->mount.validate_node_retirement(ticket);
    CHECK(!denied && denied.code() == StatusCode::unsupported);
    CHECK(reserved_root->inode_ == 2 && reserved->disk.writes == 0 && reserved->disk.flushes == 0);
}
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes image((std::istreambuf_iterator<char>(input)), {}); CHECK(image.size() == 32 * 1024 * 1024);
    vfs_handoff_and_raw_model(); ram_raw_slot_reuse_model(); ram_second_lock_retry_model();
    mounted_retirement_admission_model(image);
    CHECK(races == blocked_reuse && raw_blocks == 5 && metadata_errors == 1 && ram_retry_checks == 2 && reconfigure_checks == 5);
    std::printf("PASS retained VFS identity: thread_handoffs=%zu blocked_handoffs=%zu raw_reuse_blocks=%zu metadata_failure_drained=%zu ram_lock_retries=%zu reconfigure_refusals=%zu mounted_bookkeeping_only=1 write_flush=0\n",
                races, blocked_reuse, raw_blocks, metadata_errors, ram_retry_checks, reconfigure_checks);
}
