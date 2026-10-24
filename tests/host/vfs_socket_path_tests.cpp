// Hosted RAM namespace and socket path ownership tests; no guest execution.
#include "ok/fs/vfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>

using namespace ok;
using namespace ok::fs;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "socket VFS:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

static_assert(!std::is_copy_constructible_v<SocketPathReference>);
static_assert(std::is_move_constructible_v<SocketPathReference>);
static_assert(!std::is_move_assignable_v<SocketPathReference>);
static_assert(mode_for(NodeType::socket, 0620) == (mode_socket | 0620));

namespace
{
ResolveContext ids(u32 uid, u32 gid)
{
    return {.credentials = {uid, gid}, .check_search = true};
}

void metadata_permissions_and_rebind()
{
    auto storage = std::make_unique<VirtualFileSystem>();
    auto &vfs = *storage;
    CHECK(vfs.create("/run", NodeType::directory));
    auto mount = vfs.mount_ram_owned("/run", 02777, 0, 42);
    CHECK(mount && vfs.ram_mount_count() == 1);
    const auto owner = ids(100, 200);
    auto bound = vfs.create_socket(owner, "/run/service", 0620);
    CHECK(bound && bound.value().valid() && vfs.socket_path_reference_count() == 1);
    auto metadata = vfs.query_metadata(*bound.value().get());
    CHECK(metadata && metadata.value().type == NodeType::socket);
    CHECK(metadata.value().mode == (mode_socket | 0620));
    CHECK(metadata.value().uid == 100 && metadata.value().gid == 42);
    CHECK(metadata.value().size == 0 && metadata.value().blocks == 0 && metadata.value().link_count == 1);
    CHECK(metadata.value().attributes.has(Attribute::inode) && metadata.value().attributes.has(Attribute::device));
    const auto original_inode = metadata.value().attributes.inode;
    CHECK(!bound.value().get()->read(0, 1));
    CHECK(!bound.value().get()->write(0, {}));
    CHECK(!bound.value().get()->truncate(0));
    auto same = vfs.resolve_socket(owner, "/run/service");
    CHECK(same && same.value().same_identity(bound.value()));
    CHECK(vfs.socket_path_reference_count() == 2);
    CHECK(same.value().release() && vfs.socket_path_reference_count() == 1);
    auto group = vfs.resolve_socket(ids(101, 42), "/run/service");
    CHECK(group && group.value().release());
    auto denied = vfs.resolve_socket(ids(101, 43), "/run/service");
    CHECK(!denied && denied.status().code() == StatusCode::denied);
    CHECK(vfs.chmod("/run", 0700));
    auto search_denied = vfs.resolve_socket(owner, "/run/service");
    CHECK(!search_denied && search_denied.status().code() == StatusCode::denied);
    CHECK(!vfs.create_socket(owner, "/run/hidden", 0600));
    CHECK(vfs.chmod("/run", 0755));
    auto parent_write_denied = vfs.create_socket(owner, "/run/no-parent-write", 0600);
    CHECK(!parent_write_denied && parent_write_denied.status().code() == StatusCode::denied);
    auto search_only = vfs.resolve_socket(owner, "/run/service");
    CHECK(search_only && search_only.value().release());
    CHECK(vfs.chmod("/run", 01777));
    CHECK(vfs.create_socket(owner, "/run/service", 0600).status().code() == StatusCode::already_exists);
    CHECK(vfs.create_socket(owner, "/run/trailing/", 0600).status().code() == StatusCode::not_directory);
    CHECK(!vfs.resolve({}, "/run/trailing"));
    CHECK(vfs.create_socket(ids(101, 43), "/run/no-write", 0600));
    CHECK(vfs.remove(ids(102, 43), "/run/no-write", false).code() == StatusCode::permission);
    CHECK(vfs.remove(ids(101, 43), "/run/no-write", false));

    const user::Credentials stranger{.uid = 101, .gid = 43, .euid = 101, .egid = 43};
    const user::Credentials caller{.uid = 100, .gid = 200, .euid = 100, .egid = 200};
    CHECK(!bound.value().get()->change_mode(0666, stranger).ok());
    CHECK(bound.value().get()->change_mode(02777, caller).ok());
    CHECK(bound.value().get()->metadata().mode == (mode_socket | 0777));
    CHECK(bound.value().get()->change_mode(0600, user::root_credentials()).ok());
    CHECK(vfs.detach(mount.value()).code() == StatusCode::busy);
    CHECK(vfs.remove(owner, "/run/service", false));
    CHECK(bound.value().get()->metadata().link_count == 0);
    CHECK(!vfs.resolve_socket(owner, "/run/service"));
    auto replacement = vfs.create_socket(owner, "/run/service", 0600);
    CHECK(replacement && !replacement.value().same_identity(bound.value()));
    CHECK(replacement.value().get() != bound.value().get());
    CHECK(replacement.value().get()->metadata().attributes.inode != original_inode);
    CHECK(bound.value().release() && bound.value().release());
    CHECK(vfs.socket_path_reference_count() == 1);
    SocketPathReference moved(std::move(replacement).value());
    CHECK(moved.valid() && !replacement.value().valid());
    const auto replacement_inode = moved.get()->metadata().attributes.inode;
    CHECK(moved.release() && vfs.socket_path_reference_count() == 0);
    auto stale_node = vfs.resolve_socket(owner, "/run/service");
    CHECK(stale_node && stale_node.value().get()->metadata().attributes.inode == replacement_inode);
    CHECK(stale_node.value().release());
    CHECK(vfs.create_socket(owner, "/run/service", 0600).status().code() == StatusCode::already_exists);
    CHECK(vfs.remove(owner, "/run/service", false));
    CHECK(vfs.detach(mount.value()));
    CHECK(vfs.ram_mount_count() == 0 && vfs.ram_detach_count() == 1);
}

class ForeignNode final : public Node
{
  public:
    explicit ForeignNode(NodeType type = NodeType::directory) : type(type) {}
    NodeType type;
    ForeignNode *child{};
    usize pins{}, creates{}, writes{};
    mutable usize queries{};
    usize fail_query{};
    std::string_view name() const override { return type == NodeType::directory ? "/" : "socket"; }
    Metadata metadata() const override { return {.type = type, .mode = mode_for(type, 0777)}; }
    Result<Metadata> query_metadata() const override
    { return ++queries == fail_query ? Result<Metadata>{Status::io_error("foreign metadata failure")}
                                    : Result<Metadata>{metadata()}; }
    const void *filesystem_identity() const override { return this; }
    Status retain() override { ++pins; return {}; }
    Status release() override { CHECK(pins); --pins; return {}; }
    Status expose_borrowed_identity() override { return {}; }
    Node *lookup(std::string_view name) override
    { return name == "." || name == ".." ? this : name == "socket" ? child : nullptr; }
    Result<FileBuffer> read(usize, usize) const override { return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { ++writes; return {}; }
    Status create(std::string_view, NodeType) override { ++creates; return {}; }
    Result<Node *> create_child_retained(std::string_view, const InitialNodeAttributes &) override
    { ++creates; return Status::fault("foreign socket creation reached"); }
    Status chmod(u32) override { return {}; }
    Status chown(u32, u32) override { return {}; }
};

void unsupported_provider_has_no_creation_effects()
{
    ForeignNode provider, socket(NodeType::socket);
    provider.child = &socket;
    auto storage = std::make_unique<VirtualFileSystem>();
    auto &vfs = *storage;
    CHECK(vfs.create("/foreign", NodeType::directory));
    CHECK(vfs.mount_borrowed("/foreign", provider));
    auto result = vfs.create_socket(ids(100, 100), "/foreign/new", 0600);
    CHECK(!result && result.status().code() == StatusCode::unsupported);
    CHECK(!provider.creates && !provider.writes && !provider.pins);
    auto resolved = vfs.resolve_socket(ids(100, 100), "/foreign/socket");
    CHECK(!resolved && resolved.status().code() == StatusCode::unsupported);
    CHECK(!socket.pins && !vfs.socket_path_reference_count());
    provider.queries = 0;
    provider.fail_query = 1;
    auto ram = vfs.mount_ram_owned("/foreign");
    CHECK(!ram && ram.status().code() == StatusCode::io_error);
    CHECK(!provider.pins && !vfs.ram_mount_count() && !vfs.ram_detach_count());
    CHECK(!provider.creates && !provider.writes);
}

void publication_cleanup_and_stale_mounts()
{
    auto storage = std::make_unique<VirtualFileSystem>();
    auto &vfs = *storage;
    CHECK(vfs.create("/run", NodeType::directory));
    CHECK(vfs.create("/run/covered", NodeType::regular));
    auto underlying = vfs.stat("/run/covered"); CHECK(underlying);
    CHECK(!vfs.mount_ram_owned("/missing"));
    CHECK(!vfs.mount_ram_owned("/run/covered"));
    CHECK(!vfs.ram_mount_count() && !vfs.ram_detach_count());
    auto mount = vfs.mount_ram_owned("/run", 0777); CHECK(mount);
    CHECK(!vfs.resolve({}, "/run/covered"));
    CHECK(vfs.create("/run/sub", NodeType::directory));
    auto file = vfs.create_file({}, "/run/sub/held", 0600, 0, 0, false); CHECK(file);
    const std::array<std::byte, 1> data{std::byte{'x'}};
    CHECK(file.value().node()->write(0, data));
    auto directory = vfs.resolve({}, "/run/sub"); CHECK(directory);
    const auto stale = directory.value().handle();
    auto socket = vfs.create_socket(ids(100, 100), "/run/service", 0600); CHECK(socket);
    CHECK(vfs.detach(mount.value()).code() == StatusCode::busy);
    CHECK(socket.value().release());
    CHECK(vfs.detach(mount.value()));
    CHECK(vfs.ram_mount_count() == 0 && vfs.ram_detach_count() == 1);
    CHECK(file.value().node()->metadata().link_count == 0);
    CHECK(file.value().node()->read(0, 1).value().data[0] == std::byte{'x'});
    CHECK(!vfs.resolve({.start = stale}, "held"));
    CHECK(!vfs.resolve({.start = stale}, "/run/covered"));
    CHECK(file.value().release() && directory.value().release());
    CHECK(vfs.stat("/run/covered").value().attributes.inode == underlying.value().attributes.inode);
    CHECK(vfs.detach(mount.value()).code() == StatusCode::bad_descriptor);
    for (usize id = 2; id <= 8; ++id)
    {
        auto next = vfs.mount_ram_owned("/run"); CHECK(next && next.value().id() == id);
        CHECK(vfs.detach(next.value()));
    }
    CHECK(vfs.mount_ram_owned("/run").status().code() == StatusCode::no_memory);
    CHECK(vfs.ram_mount_count() == 0 && vfs.ram_detach_count() == 8);
    CHECK(vfs.stat("/run/covered"));
}

void sockets_do_not_use_file_buffers_and_failed_creation_reclaims()
{
    auto storage = std::make_unique<VirtualFileSystem>();
    auto &vfs = *storage;
    CHECK(vfs.create("/run", NodeType::directory));
    auto mount = vfs.mount_ram_owned("/run"); CHECK(mount);
    for (usize i = 0; i < max_ram_file_buffers; ++i)
    {
        char path[32]; std::snprintf(path, sizeof(path), "/tmp/f%zu", i);
        CHECK(vfs.create(path, NodeType::regular));
    }
    CHECK(!vfs.create("/tmp/no-buffer", NodeType::regular));
    CHECK(vfs.create("/run/sub", NodeType::directory));
    for (usize i = 0; i < max_child_nodes - 1; ++i)
    {
        char path[32]; std::snprintf(path, sizeof(path), "/run/s%zu", i);
        auto socket = vfs.create_socket({}, path, 0600); CHECK(socket && socket.value().release());
    }
    CHECK(vfs.create_socket({}, "/run/overflow", 0600).status().code() == StatusCode::no_memory);
    CHECK(!vfs.socket_path_reference_count());
    for (usize i = 0; i < 16; ++i)
    {
        char path[32]; std::snprintf(path, sizeof(path), "/run/sub/s%zu", i);
        auto socket = vfs.create_socket({}, path, 0600); CHECK(socket && socket.value().release());
    }
    CHECK(vfs.detach(mount.value()));
    CHECK(!vfs.socket_path_reference_count() && !vfs.ram_mount_count());
    CHECK(vfs.unlink("/tmp/f0"));
    CHECK(vfs.create("/tmp/reclaimed", NodeType::regular));
}

void exhausted_node_publication_has_no_namespace_effect()
{
    auto storage = std::make_unique<VirtualFileSystem>();
    auto &vfs = *storage;
    CHECK(vfs.create("/run", NodeType::directory));
    const auto inode = vfs.stat("/run").value().attributes.inode;
    for (usize i = 0; i < max_ram_nodes - 4; ++i)
    {
        char path[32]; std::snprintf(path, sizeof(path), i < max_child_nodes ? "/tmp/d%zu" : "/dev/d%zu", i);
        CHECK(vfs.create(path, NodeType::directory));
    }
    CHECK(vfs.mount_ram_owned("/run").status().code() == StatusCode::no_memory);
    CHECK(!vfs.ram_mount_count() && !vfs.ram_detach_count() && !vfs.socket_path_reference_count());
    CHECK(vfs.stat("/run").value().attributes.inode == inode);
    CHECK(vfs.rmdir("/tmp/d0"));
    auto mounted = vfs.mount_ram_owned("/run"); CHECK(mounted && mounted.value().id() == 1);
    CHECK(vfs.detach(mounted.value()));
}
} // namespace

int main()
{
    metadata_permissions_and_rebind();
    unsupported_provider_has_no_creation_effects();
    publication_cleanup_and_stale_mounts();
    sockets_do_not_use_file_buffers_and_failed_creation_reclaims();
    exhausted_node_publication_has_no_namespace_effect();
    std::puts("VFS_SOCKET_PATH_PASS metadata=1 permissions=1 rebind=1 ownership=1 cleanup=1 hosted=1 guest=0");
}
