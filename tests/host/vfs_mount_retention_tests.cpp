// Hosted attachment ownership and cleanup model. Private issuer access is only
// for this fixture; production can obtain tokens only from mounted providers.
#include "ok/fs/vfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>

using namespace ok;
using namespace ok::fs;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "owned VFS:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

static_assert(!std::is_copy_constructible_v<VirtualFileSystem>);
static_assert(!std::is_move_constructible_v<VirtualFileSystem>);
static_assert(!std::is_copy_constructible_v<MountedRootReference>);
static_assert(std::is_move_constructible_v<MountedRootReference>);
static_assert(!std::is_move_assignable_v<MountedRootReference>);

namespace
{
struct ModelNode final : Node
{
    explicit ModelNode(std::string_view edge = "/", NodeType type = NodeType::directory)
        : edge(edge), type(type) {}
    std::string_view edge;
    NodeType type;
    const void *identity{};
    ModelNode *parent{};
    std::array<ModelNode *, 8> children{};
    usize count{}, pins{}, retains{}, releases{}, queries{}, fail_query_at{}, fail_release_at{}, exposures{};
    Status exposure_status{};
    bool exposure_recorded{}, refuse_retain{};
    void (*on_query)(ModelNode &){};
    void (*on_release)(ModelNode &){};
    void (*on_exposure)(ModelNode &){};
    void *context{};

    void add(ModelNode &child)
    { CHECK(count < children.size()); children[count++] = &child; child.parent = this; child.identity = identity; }
    std::string_view name() const override { return edge; }
    Metadata metadata() const override { return {.type = type, .mode = mode_for(type, 0777)}; }
    Result<Metadata> query_metadata() const override
    {
        auto &self = *const_cast<ModelNode *>(this);
        ++self.queries;
        if (self.on_query) self.on_query(self);
        return queries == fail_query_at ? Result<Metadata>{Status::io_error("primary metadata failure")}
                                        : Result<Metadata>{metadata()};
    }
    Result<FileBuffer> read(usize, usize) const override { return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { return Status::unsupported(); }
    Status retain() override
    { if (refuse_retain) return Status::busy("borrowed root retain refused"); ++pins; ++retains; return {}; }
    Status release() override
    {
        CHECK(pins); --pins; ++releases;
        if (on_release) on_release(*this);
        return releases == fail_release_at ? Status::io_error("covered cleanup failure") : Status{};
    }
    Status expose_borrowed_identity() override
    {
        ++exposures;
        if (on_exposure) on_exposure(*this);
        if (exposure_status) exposure_recorded = true;
        return exposure_status;
    }
    const void *filesystem_identity() const override { return identity; }
    Node *lookup(std::string_view child) override
    {
        if (child == ".") return this;
        if (child == "..") return parent ? parent : this;
        for (usize i = 0; i < count; ++i) if (children[i]->edge == child) return children[i];
        return nullptr;
    }
    Result<FixedString<96>> directory_path(const Node &root) const override
    {
        FixedString<96> result;
        if (&root == this) return result;
        if (parent != &root) return Status::not_found();
        CHECK(result.assign("/")); CHECK(result.append(edge)); return result;
    }
    Status create(std::string_view, NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32, u32) override { return Status::unsupported(); }
    void drained() const { CHECK(!pins && retains == releases); }
};

struct Provider
{
    ModelNode root;
    MountedRootRetentionDomain domain;
    Provider()
    { root.identity = this; CHECK(domain.bind(&root, this, 1, 1)); }
    Result<MountedRootReference> token() { return domain.issue(); }
};

VfsMountHandle attach(VirtualFileSystem &vfs, std::string_view path, Provider &provider)
{
    auto token = provider.token(); CHECK(token);
    auto mounted = vfs.mount_owned(path, std::move(token).value()); CHECK(mounted);
    CHECK(!token.value().valid()); return mounted.value();
}

void aliases_and_nested_attachments()
{
    Provider parent, child;
    ModelNode sub("sub"); parent.root.add(sub);
    auto vfs = std::make_unique<VirtualFileSystem>();
    CHECK(vfs->create("/a", NodeType::directory));
    CHECK(vfs->create("/b", NodeType::directory));
    auto token = parent.token(); CHECK(token);
    auto alias = token.value().clone(); CHECK(alias);
    auto a = vfs->mount_owned("/a", std::move(token).value()); CHECK(a);
    auto b = vfs->mount_owned("/b", std::move(alias).value()); CHECK(b);
    CHECK(a.value().id() != b.value().id());
    CHECK(!parent.domain.empty());
    CHECK(vfs->mounts_[a.value().id() - 1].covered_reference->node());
    auto c = attach(*vfs, "/a/sub", child);
    CHECK(sub.pins == 1);
    CHECK(vfs->detach(a.value()).code() == StatusCode::busy);
    CHECK(vfs->detach(b.value()));
    CHECK(!parent.domain.empty());
    auto reconstructed = vfs->directory_path(child.root, c.id());
    CHECK(reconstructed && reconstructed.value().view() == "/a/sub");
    CHECK(vfs->detach(c)); CHECK(!sub.pins);
    CHECK(vfs->detach(a.value())); CHECK(parent.domain.empty()); CHECK(child.domain.empty());
    CHECK(vfs->stat("/a"));
    CHECK(vfs->rmdir("/a"));
    parent.root.drained(); child.root.drained(); sub.drained();
}

void borrowed_child_blocks_parent()
{
    Provider parent;
    ModelNode sub("sub"), child;
    parent.root.add(sub);
    auto vfs = std::make_unique<VirtualFileSystem>();
    auto root = attach(*vfs, "/", parent);
    CHECK(vfs->mount_borrowed("/sub", child));
    CHECK(vfs->detach(root).code() == StatusCode::busy);
    VfsMountHandle borrowed(*vfs, 2);
    CHECK(vfs->detach(borrowed).code() == StatusCode::bad_descriptor);
    vfs.reset(); CHECK(parent.domain.empty()); sub.drained(); child.drained();
}

void root_rescue_and_stale_contexts()
{
    Provider provider;
    auto vfs = std::make_unique<VirtualFileSystem>();
    auto other = std::make_unique<VirtualFileSystem>();
    auto mounted = attach(*vfs, "/", provider);
    CHECK(other->detach(mounted).code() == StatusCode::bad_descriptor);
    CHECK(vfs->detach({}).code() == StatusCode::bad_descriptor);
    CHECK(vfs->detach(VfsMountHandle(*vfs, 8)).code() == StatusCode::bad_descriptor);
    CHECK(vfs->detach(mounted));
    CHECK(vfs->detach(mounted).code() == StatusCode::bad_descriptor);
    CHECK(vfs->stat("/dev")); CHECK(vfs->root_handle().mount_id == 0);
    auto released_storage = std::make_unique<ModelNode>();
    auto *dangling = released_storage.get(); released_storage.reset();
    const PathHandle stale{dangling, mounted.id()};
    CHECK(!vfs->resolve({.root = stale, .start = stale}, ".."));
    CHECK(!vfs->resolve({.start = stale}, "/dev"));
    CHECK(!vfs->resolve({.root = stale}, "/dev"));
    CHECK(!vfs->parent_reference(stale, stale));
    CHECK(!vfs->directory_path(*dangling, mounted.id()));
    const PathHandle unused{dangling, 8};
    CHECK(!vfs->resolve({.root = unused}, "/dev"));
    CHECK(!vfs->directory_path(*dangling, 8));
}

void tombstones_and_full_table()
{
    Provider provider;
    auto vfs = std::make_unique<VirtualFileSystem>();
    for (usize i = 1; i <= 8; ++i)
    {
        auto mounted = attach(*vfs, "/tmp", provider);
        CHECK(mounted.id() == i); CHECK(vfs->detach(mounted));
    }
    auto token = provider.token(); CHECK(token);
    auto refused = vfs->mount_owned("/tmp", std::move(token).value());
    CHECK(!refused && refused.status().code() == StatusCode::no_memory);
    CHECK(provider.domain.empty()); CHECK(vfs->stat("/tmp"));
    CHECK(vfs->mount_borrowed("/tmp", provider.root).code() == StatusCode::no_memory);
}

void rollback_and_identity_refusal()
{
    Provider provider;
    auto vfs = std::make_unique<VirtualFileSystem>();
    const std::array<std::string_view, 6> invalid{"relative", "/tmp/", "/tmp/.", "/tmp//x", "/missing", ""};
    for (auto path : invalid)
    {
        auto token = provider.token(); CHECK(token);
        CHECK(!vfs->mount_owned(path, std::move(token).value()));
        CHECK(provider.domain.empty()); CHECK(vfs->mounts_[0].phase == VirtualFileSystem::MountPhase::unused);
    }
    CHECK(vfs->mount_owned("/tmp", {}).status().code() == StatusCode::bad_descriptor);
    provider.root.fail_query_at = provider.root.queries + 1;
    auto token = provider.token(); CHECK(token);
    auto failed = vfs->mount_owned("/tmp", std::move(token).value());
    CHECK(!failed && failed.status().message() == "primary metadata failure");
    CHECK(provider.domain.empty()); provider.root.fail_query_at = 0;
    provider.root.type = NodeType::regular;
    auto regular_token = provider.token(); CHECK(regular_token);
    CHECK(vfs->mount_owned("/tmp", std::move(regular_token).value()).status().code() == StatusCode::invalid_argument);
    CHECK(provider.domain.empty()); provider.root.type = NodeType::directory;
    CHECK(vfs->create("/file", NodeType::regular));
    auto file_token = provider.token(); CHECK(file_token);
    CHECK(!vfs->mount_owned("/file", std::move(file_token).value()));
    CHECK(provider.domain.empty());
    CHECK(vfs->create({}, "/link", NodeType::symlink, 0777, 0, 0, "/tmp"));
    auto symlink_token = provider.token(); CHECK(symlink_token);
    auto symlink = vfs->mount_owned("/link", std::move(symlink_token).value());
    CHECK(!symlink && symlink.status().code() == StatusCode::too_many_symlinks);
    auto mounted = attach(*vfs, "/tmp", provider);
    auto duplicate_token = provider.token(); CHECK(duplicate_token);
    auto duplicate = vfs->mount_owned("/tmp", std::move(duplicate_token).value());
    CHECK(!duplicate && duplicate.status().code() == StatusCode::busy);
    CHECK(vfs->detach(mounted)); CHECK(provider.domain.empty());
}

void registry_refusal()
{
    Provider provider;
    auto vfs = std::make_unique<VirtualFileSystem>();
    std::array<unsigned, 8> identities{};
    for (auto &identity : identities) CHECK(vfs->identities_.admit(&identity));
    auto token = provider.token(); CHECK(token);
    auto refused = vfs->mount_owned("/tmp", std::move(token).value());
    CHECK(!refused && refused.status().code() == StatusCode::no_memory);
    CHECK(provider.domain.empty()); CHECK(vfs->mounts_[0].phase == VirtualFileSystem::MountPhase::unused);
}

struct Reentry
{
    VirtualFileSystem *vfs{};
    usize callbacks{};
    bool require_removed{};
    MountedRootRetentionDomain *parent{};
};
void check_cleanup_admission(ModelNode &node)
{
    auto &state = *static_cast<Reentry *>(node.context);
    ++state.callbacks;
    CHECK(state.vfs->lock_.try_lock()); state.vfs->lock_.unlock();
    CHECK(state.vfs->stat("/").status().code() == StatusCode::busy);
    if (state.require_removed) CHECK(!state.vfs->mounts_[1].attached());
    if (state.parent) CHECK(state.parent->empty().code() == StatusCode::busy);
}

void metadata_callback_admission()
{
    Provider provider;
    auto vfs = std::make_unique<VirtualFileSystem>();
    Reentry state{vfs.get()};
    provider.root.context = &state; provider.root.on_query = check_cleanup_admission;
    auto mounted = attach(*vfs, "/tmp", provider);
    CHECK(state.callbacks == 1); CHECK(vfs->detach(mounted));
}

void check_exposure_admission(ModelNode &node)
{
    CHECK(node.pins);
    check_cleanup_admission(node);
}

void borrowed_publication_compatibility_and_refusal()
{
    ModelNode borrowed, sub("sub"), target;
    borrowed.add(sub);
    auto vfs = std::make_unique<VirtualFileSystem>();
    CHECK(vfs->mount_borrowed("/", borrowed));
    CHECK(borrowed.exposures == 1 && borrowed.exposure_recorded);
    CHECK(!borrowed.pins);
    Reentry state{vfs.get()}; target.context = &state;
    target.on_exposure = check_exposure_admission;
    target.exposure_status = Status::io_error("borrowed exposure failure");
    sub.fail_release_at = sub.releases + 1;
    auto failed = vfs->mount_borrowed("/sub", target);
    CHECK(!failed && failed.message() == "borrowed exposure failure");
    CHECK(target.exposures == 1 && !target.exposure_recorded);
    CHECK(state.callbacks == 1); sub.drained();
    CHECK(vfs->mounts_[1].phase == VirtualFileSystem::MountPhase::unused);
    target.exposure_status = Status::busy("borrowed exposure refused");
    sub.fail_release_at = 0;
    CHECK(vfs->mount_borrowed("/sub", target).code() == StatusCode::busy);
    CHECK(vfs->mounts_[1].phase == VirtualFileSystem::MountPhase::unused);
    target.exposure_status = Status::unsupported("external lifetime only");
    target.fail_release_at = target.releases + 1;
    CHECK(vfs->mount_borrowed("/sub", target).message() == "covered cleanup failure");
    CHECK(vfs->mounts_[1].phase == VirtualFileSystem::MountPhase::unused);
    target.fail_release_at = 0;
    target.refuse_retain = true;
    CHECK(vfs->mount_borrowed("/sub", target).code() == StatusCode::busy);
    CHECK(target.exposures == 3);
    target.refuse_retain = false;
    CHECK(vfs->mount_borrowed("/sub", target));
    CHECK(target.exposures == 4 && !target.exposure_recorded && !target.pins);
    CHECK(state.callbacks == 4);
    CHECK(vfs->mounts_[1].phase == VirtualFileSystem::MountPhase::borrowed);
    vfs.reset(); borrowed.drained(); sub.drained(); target.drained();
}

void invalidate_incoming_serial(ModelNode &node)
{
    auto &provider = *static_cast<Provider *>(node.context);
    for (const auto &record : provider.domain.records_)
        if (record.holder) const_cast<MountedRootReference *>(record.holder)->serial_ = 0;
}

void failed_mount_consumes_corrupted_incoming()
{
    Provider provider;
    auto vfs = std::make_unique<VirtualFileSystem>();
    provider.root.context = &provider; provider.root.on_query = invalidate_incoming_serial;
    provider.root.fail_query_at = 1;
    auto token = provider.token(); CHECK(token);
    auto failed = vfs->mount_owned("/tmp", std::move(token).value());
    CHECK(!failed && failed.status().message() == "primary metadata failure");
    CHECK(provider.domain.failure_.code() == StatusCode::fault);
    CHECK(vfs->mounts_[0].phase == VirtualFileSystem::MountPhase::unused);
    CHECK(vfs->stat("/tmp"));
}

void covered_cleanup_and_error_priority()
{
    Provider provider;
    ModelNode borrowed, sub("sub"); borrowed.add(sub);
    auto vfs = std::make_unique<VirtualFileSystem>();
    CHECK(vfs->mount_borrowed("/", borrowed));
    Reentry state{vfs.get()}; sub.context = &state; sub.on_release = check_cleanup_admission;
    sub.fail_query_at = sub.queries + 2;
    sub.fail_release_at = sub.releases + 1;
    auto token = provider.token(); CHECK(token);
    auto failed = vfs->mount_owned("/sub", std::move(token).value());
    CHECK(!failed && failed.status().message() == "primary metadata failure");
    CHECK(state.callbacks == 1 && provider.domain.empty()); sub.drained();
    CHECK(vfs->mounts_[1].phase == VirtualFileSystem::MountPhase::unused);
    sub.fail_query_at = sub.fail_release_at = 0;
    auto mounted = attach(*vfs, "/sub", provider);
    CHECK(sub.pins == 1); state.require_removed = true;
    sub.fail_release_at = sub.releases + 1;
    auto detached = vfs->detach(mounted);
    CHECK(!detached && detached.message() == "covered cleanup failure");
    CHECK(provider.domain.empty()); CHECK(state.callbacks == 2); sub.drained();
    auto releases = sub.releases;
    CHECK(!vfs->detach(mounted)); CHECK(sub.releases == releases);
    vfs.reset(); CHECK(sub.releases == releases); borrowed.drained();
}

void root_cleanup_failure_consumes_both()
{
    Provider provider;
    ModelNode borrowed, sub("sub"); borrowed.add(sub);
    auto vfs = std::make_unique<VirtualFileSystem>();
    CHECK(vfs->mount("/", borrowed));
    auto mounted = attach(*vfs, "/sub", provider);
    auto &entry = vfs->mounts_[mounted.id() - 1];
    ++entry.root_reference->serial_;
    sub.fail_release_at = sub.releases + 1;
    auto status = vfs->detach(mounted);
    CHECK(status.code() == StatusCode::fault);
    CHECK(sub.releases == sub.fail_release_at); sub.drained();
    CHECK(!entry.attached() && !entry.root_reference && !entry.covered_reference);
    CHECK(!provider.domain.empty());
    const auto releases = sub.releases;
    CHECK(!vfs->detach(mounted)); vfs.reset(); CHECK(sub.releases == releases);
}

void destructor_retires_children_first()
{
    Provider parent, child;
    ModelNode sub("sub"); parent.root.add(sub);
    auto vfs = std::make_unique<VirtualFileSystem>();
    static_cast<void>(attach(*vfs, "/", parent));
    static_cast<void>(attach(*vfs, "/sub", child));
    Reentry state{vfs.get(), 0, true, &parent.domain};
    sub.context = &state; sub.on_release = check_cleanup_admission;
    vfs.reset();
    CHECK(state.callbacks == 1); CHECK(parent.domain.empty()); CHECK(child.domain.empty());
    sub.drained(); parent.root.drained(); child.root.drained();
}
}

int main()
{
    aliases_and_nested_attachments();
    borrowed_child_blocks_parent();
    root_rescue_and_stale_contexts();
    tombstones_and_full_table();
    rollback_and_identity_refusal();
    registry_refusal();
    metadata_callback_admission();
    borrowed_publication_compatibility_and_refusal();
    failed_mount_consumes_corrupted_incoming();
    covered_cleanup_and_error_priority();
    root_cleanup_failure_consumes_both();
    destructor_retires_children_first();
    std::puts("VFS_MOUNT_RETENTION_PASS owned=1 aliases=1 nested=1 tombstones=1 stale_ids=1 cleanup=1 hosted=1 guest=0");
}
