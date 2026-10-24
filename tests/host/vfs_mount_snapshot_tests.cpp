// Actual-source hosted VFS observations. Fixture tokens do not prove media,
// hardware execution, namespace isolation, reference drain or clean shutdown.
#include "ok/fs/vfs.hpp"
#include "../kernel/points/mount_snapshot_checks.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace ok;
using namespace ok::fs;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "VFS snapshot:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

namespace
{
struct ModelNode final : Node
{
    explicit ModelNode(std::string_view edge = "/") : edge(edge) {}
    std::string_view edge;
    const void *identity{};
    ModelNode *parent{};
    std::array<ModelNode *, 8> children{};
    usize count{}, pins{};
    mutable usize calls{};
    bool release_failure{};
    void (*on_query)(ModelNode &){};
    void *context{};

    void add(ModelNode &child)
    { CHECK(count < children.size()); children[count++] = &child; child.parent = this; child.identity = identity; }
    std::string_view name() const override { ++calls; return edge; }
    Metadata metadata() const override
    { ++calls; return {.type = NodeType::directory, .mode = mode_for(NodeType::directory, 0755)}; }
    Result<Metadata> query_metadata() const override
    {
        auto &self = *const_cast<ModelNode *>(this); ++calls;
        if (self.on_query) self.on_query(self);
        return metadata();
    }
    Result<FileBuffer> read(usize, usize) const override { ++calls; return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { ++calls; return Status::unsupported(); }
    Status retain() override { ++calls; ++pins; return {}; }
    Status release() override
    { ++calls; CHECK(pins); --pins; return release_failure ? Status::io_error("fixture release failure") : Status{}; }
    const void *filesystem_identity() const override { ++calls; return identity; }
    Status expose_borrowed_identity() override { ++calls; return Status::unsupported(); }
    Node *lookup(std::string_view child) override
    {
        ++calls;
        if (child == ".") return this;
        if (child == "..") return parent ? parent : this;
        for (usize index = 0; index < count; ++index)
            if (children[index]->edge == child) return children[index];
        return nullptr;
    }
    Status create(std::string_view, NodeType) override { ++calls; return Status::unsupported(); }
    Status chmod(u32) override { ++calls; return Status::unsupported(); }
    Status chown(u32, u32) override { ++calls; return Status::unsupported(); }
};

struct Provider
{
    ModelNode root;
    MountedRootRetentionDomain domain;
    Provider() { root.identity = this; CHECK(domain.bind(&root, this, 1, 1)); }
};

VfsMountHandle attach(VirtualFileSystem &vfs, std::string_view path, Provider &provider)
{
    auto token = provider.domain.issue(); CHECK(token);
    auto attached = vfs.mount_owned(path, std::move(token).value()); CHECK(attached);
    return attached.value();
}

struct Fixture
{
    Provider provider;
    ModelNode run{"run"}, extra{"extra"};
    std::unique_ptr<VirtualFileSystem> vfs{std::make_unique<VirtualFileSystem>()};
    VfsMountHandle root;
    Fixture()
    {
        provider.root.add(run); provider.root.add(extra);
        root = attach(*vfs, "/", provider);
    }
};

bool same(const MountSnapshot &left, const MountSnapshot &right)
{
    return left.generation == right.generation && left.slot == right.slot && left.state == right.state &&
           left.parent_id == right.parent_id && left.path_length == right.path_length && left.path == right.path;
}

MountSnapshot sentinel()
{
    MountSnapshot row;
    row.generation = 0xa1a2a3a4; row.slot = 0xb1b2b3b4;
    row.state = static_cast<MountSnapshotState>(0xc1c2c3c4);
    row.parent_id = 0xd1d2d3d4; row.path_length = 0xe1e2e3e4;
    row.path.fill('!');
    return row;
}

MountSnapshot row(const VirtualFileSystem &vfs, u32 slot, u32 generation, MountSnapshotState state,
                  u32 parent, std::string_view path)
{
    auto output = sentinel();
    CHECK(vfs.mount_snapshot(slot, output));
    CHECK(output.generation == generation && output.slot == slot && output.state == state);
    CHECK(output.parent_id == parent && output.path_length == path.size());
    CHECK(std::string_view(output.path.data(), output.path_length) == path);
    for (usize index = path.size(); index < output.path.size(); ++index) CHECK(output.path[index] == '\0');
    return output;
}

void empty_and_rejection()
{
    auto vfs = std::make_unique<VirtualFileSystem>();
    row(*vfs, 0, 1, MountSnapshotState::underlying_ram, 0, "/");
    // Failed borrowed admission can leave private draft bytes in an unused slot.
    CHECK(vfs->mounts_[0].path.assign("/private-draft"));
    vfs->mounts_[0].covered_mount_id = 7;
    for (u32 slot = 1; slot < mount_snapshot_capacity; ++slot)
        row(*vfs, slot, 1, MountSnapshotState::unused, 0, "");
    for (u32 slot : {mount_snapshot_capacity, ~u32{0}})
    {
        auto output = sentinel(); const auto before = output;
        CHECK(vfs->mount_snapshot(slot, output).code() == StatusCode::invalid_argument);
        CHECK(same(output, before));
    }
    CHECK(vfs->create("/ordinary", NodeType::directory));
    vfs->set_mode(FileSystemMode::ext4_read_only);
    row(*vfs, 0, 1, MountSnapshotState::underlying_ram, 0, "/");
}

void all_states_and_no_callbacks()
{
    ModelNode borrowed;
    Fixture f;
    auto runtime = f.vfs->mount_ram_owned("/run"); CHECK(runtime);
    CHECK(f.vfs->mount_borrowed("/extra", borrowed));
    const auto serial = f.vfs->operation_serial_;
    const auto root_calls = f.provider.root.calls, run_calls = f.run.calls;
    const auto extra_calls = f.extra.calls, borrowed_calls = borrowed.calls;
    row(*f.vfs, 0, 4, MountSnapshotState::underlying_ram, 0, "/");
    row(*f.vfs, 1, 4, MountSnapshotState::owned_provider, 0, "/");
    row(*f.vfs, 2, 4, MountSnapshotState::owned_ram, 1, "/run");
    row(*f.vfs, 3, 4, MountSnapshotState::borrowed_provider, 1, "/extra");
    for (u32 slot = 4; slot < mount_snapshot_capacity; ++slot)
        row(*f.vfs, slot, 4, MountSnapshotState::unused, 0, "");
    CHECK(f.vfs->operation_serial_ == serial);
    CHECK(f.provider.root.calls == root_calls && f.run.calls == run_calls);
    CHECK(f.extra.calls == extra_calls && borrowed.calls == borrowed_calls);
    auto socket = f.vfs->create_socket({}, "/run/socket", 0600); CHECK(socket);
    row(*f.vfs, 2, 4, MountSnapshotState::owned_ram, 1, "/run");
    CHECK(f.vfs->detach(runtime.value()).code() == StatusCode::busy);
    row(*f.vfs, 0, 4, MountSnapshotState::underlying_ram, 0, "/");
    CHECK(socket.value().release());
    CHECK(f.vfs->detach(runtime.value()));
    row(*f.vfs, 2, 5, MountSnapshotState::detached, 1, "/run");
    CHECK(f.vfs->detach(runtime.value()).code() == StatusCode::bad_descriptor);
    row(*f.vfs, 2, 5, MountSnapshotState::detached, 1, "/run");
}

void busy_and_corruption_leave_output()
{
    Fixture f;
    f.provider.root.context = &f;
    f.provider.root.on_query = [](ModelNode &node) {
        auto &fixture = *static_cast<Fixture *>(node.context);
        auto output = sentinel(); const auto before = output;
        CHECK(fixture.vfs->mount_snapshot(0, output).code() == StatusCode::busy);
        CHECK(same(output, before));
    };
    CHECK(f.vfs->stat("/"));
    f.provider.root.on_query = nullptr;
    auto output = sentinel(); const auto before = output;
    auto &entry = f.vfs->mounts_[0];
    const auto phase = entry.phase;
    entry.phase = static_cast<VirtualFileSystem::MountPhase>(255);
    CHECK(f.vfs->mount_snapshot(0, output).code() == StatusCode::fault && same(output, before));
    entry.phase = phase;
    const auto parent = entry.covered_mount_id;
    entry.covered_mount_id = mount_snapshot_capacity;
    CHECK(f.vfs->mount_snapshot(1, output).code() == StatusCode::fault && same(output, before));
    entry.covered_mount_id = parent;
    const auto path = entry.path;
    entry.path.clear();
    CHECK(f.vfs->mount_snapshot(1, output).code() == StatusCode::fault && same(output, before));
    entry.path = path;
    row(*f.vfs, 1, 2, MountSnapshotState::owned_provider, 0, "/");
}

void long_path_and_owned_copy()
{
    auto vfs = std::make_unique<VirtualFileSystem>();
    std::string path;
    for (usize component = 0; component < 8; ++component)
    {
        path += '/'; path += std::string(component == 7 ? 30 : 31, static_cast<char>('a' + component));
        CHECK(vfs->create(path, NodeType::directory));
    }
    CHECK(path.size() == 255);
    auto mounted = vfs->mount_ram_owned(path); CHECK(mounted);
    const auto copy = row(*vfs, 1, 2, MountSnapshotState::owned_ram, 0, path);
    CHECK(vfs->detach(mounted.value()));
    row(*vfs, 1, 3, MountSnapshotState::detached, 0, path);
    CHECK(copy.state == MountSnapshotState::owned_ram && copy.generation == 2);
    CHECK(std::string_view(copy.path.data(), copy.path_length) == path && copy.path[255] == '\0');
    CHECK(vfs->mount_ram_owned(path + "x").status().code() == StatusCode::name_too_long);
    row(*vfs, 0, 3, MountSnapshotState::underlying_ram, 0, "/");
}

void lifetime_capacity_and_generation()
{
    auto vfs = std::make_unique<VirtualFileSystem>();
    CHECK(vfs->create("/run", NodeType::directory));
    for (u32 slot = 1; slot < mount_snapshot_capacity; ++slot)
    {
        auto mounted = vfs->mount_ram_owned("/run"); CHECK(mounted && mounted.value().id() == slot);
        row(*vfs, slot, 2 * slot, MountSnapshotState::owned_ram, 0, "/run");
        CHECK(vfs->detach(mounted.value()));
        row(*vfs, slot, 2 * slot + 1, MountSnapshotState::detached, 0, "/run");
    }
    CHECK(vfs->mount_ram_owned("/run").status().code() == StatusCode::no_memory);
    row(*vfs, 0, 17, MountSnapshotState::underlying_ram, 0, "/");
    for (u32 slot = 1; slot < mount_snapshot_capacity; ++slot)
        row(*vfs, slot, 17, MountSnapshotState::detached, 0, "/run");
}

void failed_cleanup_retains_history()
{
    Provider extra;
    Fixture f;
    const auto child = attach(*f.vfs, "/extra", extra);
    f.extra.release_failure = true;
    CHECK(f.vfs->detach(child).code() == StatusCode::io_error);
    row(*f.vfs, 2, 4, MountSnapshotState::detached, 1, "/extra");
    CHECK(f.vfs->shutdown_reference_status().code() == StatusCode::io_error);
    row(*f.vfs, 0, 4, MountSnapshotState::underlying_ram, 0, "/");
}
} // namespace

int main()
{
    empty_and_rejection(); all_states_and_no_callbacks(); busy_and_corruption_leave_output();
    long_path_and_owned_copy(); lifetime_capacity_and_generation(); failed_cleanup_retains_history();
    const auto debug_model = ok::tests::check_mount_snapshot_model();
    if (!debug_model)
        std::fprintf(stderr, "mount snapshot debug model: %.*s\n", static_cast<int>(debug_model.message().size()),
                     debug_model.message().data());
    CHECK(debug_model);
    std::puts("VFS_MOUNT_SNAPSHOT_PASS groups=6 debug_models=1 guest=0 physical=0 drain=0 seal=0");
}
