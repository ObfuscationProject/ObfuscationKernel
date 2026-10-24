// Hosted VFS inventory model. Private access only issues fixture provider tokens
// and checks shallow ownership adversaries; this is not a drain or guest test.
#include "ok/fs/vfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace ok;
using namespace ok::fs;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "VFS inventory:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
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
    usize count{}, pins{}, queries{}, retains{}, releases{};
    void (*on_query)(ModelNode &){};
    void *context{};

    void add(ModelNode &child)
    { CHECK(count < children.size()); children[count++] = &child; child.parent = this; child.identity = identity; }
    std::string_view name() const override { return edge; }
    Metadata metadata() const override
    { return {.type = NodeType::directory, .mode = mode_for(NodeType::directory, 0755)}; }
    Result<Metadata> query_metadata() const override
    {
        auto &self = *const_cast<ModelNode *>(this); ++self.queries;
        if (self.on_query) self.on_query(self);
        return metadata();
    }
    Result<FileBuffer> read(usize, usize) const override { return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { return Status::unsupported(); }
    Status retain() override { ++pins; ++retains; return {}; }
    Status release() override { CHECK(pins); --pins; ++releases; return {}; }
    const void *filesystem_identity() const override { return identity; }
    Node *lookup(std::string_view child) override
    {
        if (child == ".") return this;
        if (child == "..") return parent ? parent : this;
        for (usize index = 0; index < count; ++index)
            if (children[index]->edge == child) return children[index];
        return nullptr;
    }
    Status create(std::string_view, NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32, u32) override { return Status::unsupported(); }
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
    auto result = vfs.mount_owned(path, std::move(token).value()); CHECK(result);
    return result.value();
}

struct Fixture
{
    Provider provider;
    ModelNode run{"run"}, extra{"extra"};
    std::unique_ptr<VirtualFileSystem> vfs{std::make_unique<VirtualFileSystem>()};
    VfsMountHandle root{}, runtime{};
    std::array<OwnedMountExpectation, 2> expected{};
    Fixture()
    {
        provider.root.add(run); provider.root.add(extra);
        root = attach(*vfs, "/", provider);
        auto result = vfs->mount_ram_owned("/run"); CHECK(result); runtime = result.value();
        expected = {{{root, "/", OwnedMountKind::provider, {}},
                     {runtime, "/run", OwnedMountKind::ram, root}}};
    }
};

void exact_and_read_only()
{
    Fixture f;
    const auto serial = f.vfs->operation_serial_;
    const auto root_queries = f.provider.root.queries;
    const auto run_queries = f.run.queries;
    const auto retains = f.run.retains, releases = f.run.releases;
    CHECK(f.vfs->preflight_owned_mounts(f.expected));
    std::swap(f.expected[0], f.expected[1]);
    CHECK(f.vfs->preflight_owned_mounts(f.expected));
    CHECK(f.vfs->operation_serial_ == serial);
    CHECK(f.provider.root.queries == root_queries && f.run.queries == run_queries);
    CHECK(f.run.retains == retains && f.run.releases == releases);
    CHECK(f.vfs->ram_mount_count() == 1 && f.vfs->ram_detach_count() == 0);
    // An inventory snapshot cannot certify release of a service's socket owner.
    auto socket = f.vfs->create_socket({}, "/run/service", 0600); CHECK(socket);
    CHECK(f.vfs->preflight_owned_mounts(f.expected));
    CHECK(f.vfs->socket_path_reference_count() == 1);
    CHECK(f.vfs->detach(f.runtime).code() == StatusCode::busy);
    CHECK(socket.value().release());
    CHECK(f.vfs->detach(f.runtime));
    CHECK(f.vfs->preflight_owned_mounts(f.expected).code() == StatusCode::bad_descriptor);
    const OwnedMountExpectation root_only{f.root, "/", OwnedMountKind::provider, {}};
    CHECK(f.vfs->preflight_owned_mounts({&root_only, 1}));
}

void handle_and_topology_refusals()
{
    Fixture f, foreign;
    CHECK(f.vfs->preflight_owned_mounts({}).code() == StatusCode::unsupported);
    CHECK(f.vfs->preflight_owned_mounts({f.expected.data(), 1}).code() == StatusCode::unsupported);
    auto changed = f.expected;
    changed[1].attachment = foreign.runtime;
    CHECK(f.vfs->preflight_owned_mounts(changed).code() == StatusCode::bad_descriptor);
    changed = f.expected; changed[1].attachment = {};
    CHECK(f.vfs->preflight_owned_mounts(changed).code() == StatusCode::bad_descriptor);
    changed = f.expected; changed[1] = changed[0];
    CHECK(f.vfs->preflight_owned_mounts(changed).code() == StatusCode::invalid_argument);
    changed = f.expected; changed[1].covered_mount = foreign.root;
    CHECK(f.vfs->preflight_owned_mounts(changed).code() == StatusCode::bad_descriptor);
    changed = f.expected; changed[1].covered_mount = {};
    CHECK(f.vfs->preflight_owned_mounts(changed).code() == StatusCode::invalid_argument);
    changed = f.expected; changed[0].covered_mount = f.runtime;
    CHECK(f.vfs->preflight_owned_mounts(changed).code() == StatusCode::invalid_argument);
    changed = f.expected; changed[1].kind = OwnedMountKind::provider;
    CHECK(f.vfs->preflight_owned_mounts(changed).code() == StatusCode::invalid_argument);
    changed = f.expected; changed[1].kind = static_cast<OwnedMountKind>(255);
    CHECK(f.vfs->preflight_owned_mounts(changed).code() == StatusCode::invalid_argument);
    changed = f.expected; changed[1].path = "/run/";
    CHECK(f.vfs->preflight_owned_mounts(changed).code() == StatusCode::invalid_argument);
    std::array<OwnedMountExpectation, 9> oversized{};
    CHECK(f.vfs->preflight_owned_mounts(oversized).code() == StatusCode::invalid_argument);
}

void undeclared_owners()
{
    {
        Provider extra;
        Fixture f;
        const auto attachment = attach(*f.vfs, "/extra", extra);
        CHECK(f.vfs->preflight_owned_mounts(f.expected).code() == StatusCode::unsupported);
        const std::array<OwnedMountExpectation, 3> all{{f.expected[0], f.expected[1],
            {attachment, "/extra", OwnedMountKind::provider, f.root}}};
        CHECK(f.vfs->preflight_owned_mounts(all));
        CHECK(f.vfs->detach(attachment));
        CHECK(f.vfs->preflight_owned_mounts(f.expected));
    }
    {
        ModelNode borrowed;
        Fixture f;
        CHECK(f.vfs->mount_borrowed("/extra", borrowed));
        CHECK(f.vfs->preflight_owned_mounts(f.expected).code() == StatusCode::unsupported);
        const std::array<OwnedMountExpectation, 3> all{{f.expected[0], f.expected[1],
            {VfsMountHandle(*f.vfs, 3), "/extra", OwnedMountKind::provider, f.root}}};
        CHECK(f.vfs->preflight_owned_mounts(all).code() == StatusCode::unsupported);
    }
    {
        Provider child;
        Fixture f;
        CHECK(f.vfs->create("/run/child", NodeType::directory));
        const auto attachment = attach(*f.vfs, "/run/child", child);
        CHECK(f.vfs->preflight_owned_mounts(f.expected).code() == StatusCode::unsupported);
        const std::array<OwnedMountExpectation, 3> all{{f.expected[0], f.expected[1],
            {attachment, "/run/child", OwnedMountKind::provider, f.runtime}}};
        CHECK(f.vfs->preflight_owned_mounts(all));
    }
}

void active_operation_and_consistency()
{
    Fixture f;
    f.provider.root.context = &f;
    f.provider.root.on_query = [](ModelNode &node) {
        auto &fixture = *static_cast<Fixture *>(node.context);
        CHECK(fixture.vfs->preflight_owned_mounts(fixture.expected).code() == StatusCode::busy);
    };
    CHECK(f.vfs->stat("/"));
    f.provider.root.on_query = nullptr;
    CHECK(f.vfs->preflight_owned_mounts(f.expected));
    auto &entry = f.vfs->mounts_[f.root.id() - 1];
    auto *original = entry.root;
    entry.root = nullptr;
    CHECK(f.vfs->preflight_owned_mounts(f.expected).code() == StatusCode::fault);
    entry.root = &f.extra;
    CHECK(f.vfs->preflight_owned_mounts(f.expected).code() == StatusCode::fault);
    entry.root = original;
    auto &ram = f.vfs->mounts_[f.runtime.id() - 1];
    original = ram.root; ram.root = &f.run;
    CHECK(f.vfs->preflight_owned_mounts(f.expected).code() == StatusCode::fault);
    ram.root = original;
    CHECK(f.vfs->preflight_owned_mounts(f.expected));
    auto empty = std::make_unique<VirtualFileSystem>();
    CHECK(empty->preflight_owned_mounts({}));
}
}

int main()
{
    exact_and_read_only(); handle_and_topology_refusals();
    undeclared_owners(); active_operation_and_consistency();
    std::puts("VFS_MOUNT_PREFLIGHT_PASS groups=4 guest=0 physical=0 drain=0 seal=0");
}
