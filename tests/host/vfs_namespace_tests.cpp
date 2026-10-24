#include "ok/fs/vfs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                                       \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)
using namespace ok;
using namespace ok::fs;
std::span<const std::byte> bytes(const char *s)
{
    return {reinterpret_cast<const std::byte *>(s), std::strlen(s)};
}
void content(Node &node, const char *s)
{
    auto data = node.read(0, max_file_data);
    CHECK(data && data.value().size == std::strlen(s));
    CHECK(std::memcmp(data.value().data.data(), s, data.value().size) == 0);
}
void path(VirtualFileSystem &vfs, Node &node, std::string_view expected)
{
    auto result = vfs.directory_path(node);
    CHECK(result && result.value().view() == expected);
}
void links_and_replacement()
{
    auto storage = std::make_unique<VirtualFileSystem>();
    auto &vfs = *storage;
    CHECK(vfs.create("/tmp/a", NodeType::regular));
    auto *source = vfs.lookup("/tmp/a");
    CHECK(source && source->retain());
    CHECK(source->write(0, bytes("source")));
    CHECK(vfs.link("/tmp/a", "/tmp/b"));
    CHECK(vfs.link("/tmp/a", "/tmp/c"));
    CHECK(vfs.lookup("/tmp/b") == source && source->metadata().link_count == 3);
    CHECK(vfs.rename("/tmp/a", "/tmp/b")); // Same inode keeps both names.
    CHECK(vfs.lookup("/tmp/a") == source && vfs.lookup("/tmp/b") == source);
    CHECK(vfs.create("/tmp/d", NodeType::regular));
    auto *victim = vfs.lookup("/tmp/d");
    CHECK(victim && victim->retain());
    CHECK(victim->write(0, bytes("victim")));
    CHECK(vfs.rename("/tmp/a", "/tmp/d"));
    CHECK(!vfs.lookup("/tmp/a") && vfs.lookup("/tmp/d") == source);
    CHECK(victim->metadata().link_count == 0 && source->metadata().link_count == 3);
    content(*victim, "victim");
    CHECK(victim->write(0, bytes("Victim")));
    content(*source, "source");
    CHECK(vfs.unlink("/tmp/b"));
    CHECK(source->metadata().link_count == 2);
    CHECK(vfs.unlink("/tmp/c"));
    CHECK(source->metadata().link_count == 1);
    CHECK(vfs.unlink("/tmp/d"));
    CHECK(source->metadata().link_count == 0);
    for (int i = 0; i < 400; ++i)
    {
        CHECK(vfs.create("/tmp/reuse", NodeType::regular));
        auto scoped = vfs.resolve({}, "/tmp/reuse");
        CHECK(scoped && scoped.value().node() != source && scoped.value().node() != victim);
        CHECK(scoped.value().release());
        CHECK(vfs.unlink("/tmp/reuse"));
    }
    content(*source, "source");
    content(*victim, "Victim");
    CHECK(source->release() && victim->release());
    for (int i = 0; i < 400; ++i)
    {
        CHECK(vfs.create("/tmp/reuse", NodeType::regular));
        CHECK(vfs.unlink("/tmp/reuse"));
    }
    CHECK(!vfs.link("/tmp", "/alias"));
}
void directory_topology()
{
    auto storage = std::make_unique<VirtualFileSystem>();
    auto &vfs = *storage;
    CHECK(vfs.create("/a", NodeType::directory));
    CHECK(vfs.create("/b", NodeType::directory));
    auto *a = vfs.lookup("/a"), *b = vfs.lookup("/b");
    CHECK(vfs.create("/a/sub", NodeType::directory));
    CHECK(vfs.create("/a/sub/child", NodeType::directory));
    auto *sub = vfs.lookup("/a/sub"), *child = vfs.lookup("/a/sub/child");
    CHECK(sub->retain() && child->retain());
    path(vfs, *child, "/a/sub/child");
    CHECK(a->metadata().link_count == 3 && b->metadata().link_count == 2);
    CHECK(vfs.rename("/a/sub", "/a/longer-name"));
    CHECK(a->metadata().link_count == 3);
    path(vfs, *sub, "/a/longer-name");
    CHECK(vfs.rename("/a/longer-name", "/b/moved"));
    CHECK(a->metadata().link_count == 2 && b->metadata().link_count == 3);
    CHECK(vfs.lookup("/b/moved/..") == b);
    path(vfs, *child, "/b/moved/child");
    CHECK(!vfs.rename("/b", "/b/moved/child/cycle"));
    CHECK(vfs.lookup("/b/moved") == sub);
    CHECK(vfs.create("/a/empty", NodeType::directory));
    auto *empty = vfs.lookup("/a/empty");
    CHECK(empty->retain());
    CHECK(vfs.rename("/b/moved", "/a/empty"));
    CHECK(empty->metadata().link_count == 0 && !vfs.directory_path(*empty));
    CHECK(a->metadata().link_count == 3 && b->metadata().link_count == 2);
    path(vfs, *child, "/a/empty/child");
    CHECK(vfs.lookup("/a/empty/..") == a);
    CHECK(vfs.create("/a/nonempty", NodeType::directory));
    CHECK(vfs.create("/a/nonempty/file", NodeType::regular));
    CHECK(vfs.rename("/a/empty", "/a/nonempty").code() == StatusCode::not_empty);
    CHECK(vfs.rename("/a/empty", "/a/nonempty/file").code() == StatusCode::not_directory);
    CHECK(vfs.rename("/a/nonempty/file", "/a/empty").code() == StatusCode::is_directory);
    CHECK(vfs.lookup("/a/empty") == sub && vfs.lookup("/a/empty/child") == child);
    CHECK(vfs.rmdir("/a/empty/child"));
    CHECK(!vfs.directory_path(*child));
    CHECK(child->release());
    CHECK(vfs.rmdir("/a/empty"));
    CHECK(!vfs.directory_path(*sub));
    CHECK(sub->release());
    CHECK(empty->release());
}
void mounts_and_capacity()
{
    auto storage = std::make_unique<VirtualFileSystem>();
    auto &vfs = *storage;
    auto foreign_storage = std::make_unique<VirtualFileSystem>();
    auto &foreign = *foreign_storage;
    CHECK(vfs.create("/parent", NodeType::directory));
    CHECK(vfs.create("/parent/mount", NodeType::directory));
    CHECK(vfs.create("/other", NodeType::directory));
    auto *covered = vfs.lookup("/parent/mount");
    CHECK(covered->retain());
    CHECK(foreign.create("/file", NodeType::regular));
    CHECK(foreign.create("/dir", NodeType::directory));
    CHECK(vfs.mount("/parent/mount", *foreign.lookup("/")));
    CHECK(vfs.mount("/other", *foreign.lookup("/")));
    CHECK(!vfs.directory_path(*covered));
    path(vfs, *vfs.lookup("/parent/mount/dir"), "/parent/mount/dir");
    CHECK(vfs.link("/parent/mount/file", "/other/alias").code() == StatusCode::cross_device);
    CHECK(vfs.rename("/parent/mount/file", "/other/moved").code() == StatusCode::cross_device);
    CHECK(vfs.link("/parent/mount/file", "/tmp/alias").code() == StatusCode::cross_device);
    CHECK(vfs.rename("/parent/mount", "/moved").code() == StatusCode::busy);
    CHECK(vfs.rename("/parent", "/moved").code() == StatusCode::busy);
    CHECK(vfs.create("/full", NodeType::directory));
    for (unsigned i = 0; i < max_child_nodes; ++i)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "/full/d%u", i);
        CHECK(vfs.create(name, NodeType::directory));
    }
    CHECK(vfs.create("/tmp/source", NodeType::directory));
    CHECK(!vfs.rename("/tmp/source", "/full/overflow"));
    CHECK(vfs.lookup("/tmp/source") && !vfs.lookup("/full/overflow"));
    CHECK(vfs.rename("/full/d0", "/full/renamed"));
    CHECK(!vfs.lookup("/full/d0") && vfs.lookup("/full/renamed"));
    CHECK(covered->release());
}
void independent_buffer_reuse()
{
    auto storage = std::make_unique<VirtualFileSystem>();
    auto &vfs = *storage;
    CHECK(vfs.create("/tmp/early-directory", NodeType::directory));
    for (unsigned i = 0; i < max_ram_file_buffers; ++i)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "/tmp/f%u", i);
        CHECK(vfs.create(name, NodeType::regular));
    }
    CHECK(vfs.rmdir("/tmp/early-directory"));
    CHECK(vfs.unlink("/tmp/f31"));
    CHECK(vfs.create("/tmp/reclaimed", NodeType::regular));
    CHECK(vfs.write_file("/tmp/reclaimed", bytes("new buffer")));
}
void object_starts_and_root_boundaries()
{
    auto storage = std::make_unique<VirtualFileSystem>();
    auto &vfs = *storage;
    CHECK(vfs.create("/jail", NodeType::directory));
    CHECK(vfs.create("/jail/child", NodeType::directory));
    CHECK(vfs.create("/jail/data", NodeType::regular));
    CHECK(vfs.create({}, "/jail/absolute", NodeType::symlink, 0777, 0, 0, "/data"));
    auto root = vfs.resolve({}, "/jail");
    auto start = vfs.resolve({}, "/jail/child");
    CHECK(root && start);
    const ResolveContext context{.root = root.value().handle(), .start = start.value().handle()};
    auto up = vfs.resolve(context, "../../..");
    CHECK(up && up.value().node() == root.value().node());
    auto target = vfs.resolve(context, "/absolute");
    CHECK(target && target.value().node() == vfs.lookup("/jail/data"));
    CHECK(vfs.create("/covered", NodeType::directory));
    CHECK(vfs.create("/covered/private", NodeType::regular));
    auto held = vfs.resolve({}, "/covered");
    auto foreign = std::make_unique<VirtualFileSystem>();
    CHECK(foreign->create("/public", NodeType::regular));
    CHECK(vfs.create({}, "/alias", NodeType::symlink, 0777, 0, 0, "/covered"));
    CHECK(vfs.mount("/alias", *foreign->lookup("/")).code() == StatusCode::too_many_symlinks);
    CHECK(vfs.mount("/alias/child", *foreign->lookup("/")).code() == StatusCode::too_many_symlinks);
    CHECK(vfs.mount("/covered", *foreign->lookup("/")));
    const ResolveContext previous{.start = held.value().handle()};
    CHECK(vfs.resolve(previous, "private"));
    CHECK(!vfs.resolve(previous, "public"));
    CHECK(!vfs.resolve({}, "/covered/private"));
    CHECK(vfs.resolve({}, "/covered/public"));
    CHECK(vfs.create(previous, "new", NodeType::regular, 0644, 0, 0));
    CHECK(vfs.resolve(previous, "new") && !vfs.resolve({}, "/covered/new"));
    CHECK(!vfs.directory_path(*held.value().node(), held.value().handle().mount_id));
    auto opened = vfs.create_file(previous, "opened", 0644, 0, 0, true);
    CHECK(opened && opened.value().node()->write(0, bytes("original")));
    CHECK(vfs.remove(previous, "opened", false));
    CHECK(vfs.create(previous, "opened", NodeType::regular, 0644, 0, 0));
    auto replacement = vfs.resolve(previous, "opened");
    CHECK(replacement && replacement.value().node() != opened.value().node());
    content(*opened.value().node(), "original");
}

// A recycling provider must transfer a retain before returning from lookup.
// Its attempted reuse in that handoff is rejected while the reference lives.
class RetainedLookupNode final : public Node
{
  public:
    explicit RetainedLookupNode(std::string_view name, NodeType type) : name_(name), type_(type) {}
    std::string_view name() const override { return name_; }
    Metadata metadata() const override
    {
        Metadata result{};
        result.type = type_;
        result.mode = type_ == NodeType::directory ? 0755u : 0644u;
        result.link_count = 1;
        return result;
    }
    Result<Metadata> query_metadata() const override
    {
        if (requires_retain && !references) return Status::fault("recycled node was read without a retain");
        return metadata();
    }
    Result<FileBuffer> read(usize, usize) const override { return FileBuffer{}; }
    Status write(usize, std::span<const std::byte>) override { return Status::unsupported("model is read-only"); }
    Status retain() override { ++references; return {}; }
    Status release() override
    {
        if (!references) return Status::bad_descriptor("model node not retained");
        --references; return {};
    }
    Node *lookup(std::string_view child) override
    {
        ++bare_lookups;
        return find(child);
    }
    Result<Node *> lookup_child_retained(std::string_view child) override
    {
        ++retained_lookups;
        auto *node = find(child);
        if (!node) return Status::not_found("model child absent");
        if (auto status = node->retain(); !status) return status;
        auto &target = static_cast<RetainedLookupNode &>(*node);
        ++target.reuse_attempts;
        if (!target.references) ++target.unsafe_reuses;
        return node;
    }
    Status create(std::string_view, NodeType) override { return Status::unsupported("model is read-only"); }
    Status chmod(u32) override { return Status::unsupported("model is read-only"); }
    Status chown(u32, u32) override { return Status::unsupported("model is read-only"); }

    RetainedLookupNode *parent{};
    RetainedLookupNode *sub{};
    RetainedLookupNode *file{};
    bool requires_retain{};
    usize references{}, retained_lookups{}, bare_lookups{}, reuse_attempts{}, unsafe_reuses{};

  private:
    Node *find(std::string_view child)
    {
        if (child == "..") return parent ? parent : this;
        if (child == "sub") return sub;
        if (child == "file") return file;
        return nullptr;
    }
    std::string_view name_;
    NodeType type_;
};

void retained_provider_handoff_model()
{
    auto storage = std::make_unique<VirtualFileSystem>();
    auto &vfs = *storage;
    CHECK(vfs.create("/mount", NodeType::directory));
    RetainedLookupNode root("/", NodeType::directory);
    RetainedLookupNode sub("sub", NodeType::directory);
    RetainedLookupNode file("file", NodeType::regular);
    root.parent = &root; root.sub = &sub; root.file = &file;
    sub.parent = &root; sub.requires_retain = true;
    file.requires_retain = true;
    CHECK(vfs.mount("/mount", root));
    {
        auto reference = vfs.resolve({}, "/mount/sub/../file");
        CHECK(reference && reference.value().node() == &file);
        CHECK(file.references == 1 && sub.references == 0 && root.references == 0);
        CHECK(reference.value().release());
    }
    CHECK(!file.references);
    auto rejected = vfs.resolve({}, "/mount/file/other");
    CHECK(!rejected && rejected.status().code() == StatusCode::not_directory);
    CHECK(!file.references && !sub.references && !root.references);
    CHECK(root.retained_lookups >= 3 && sub.retained_lookups == 1);
    CHECK(!root.bare_lookups && !sub.bare_lookups);
    CHECK(file.reuse_attempts >= 2 && !file.unsafe_reuses && !sub.unsafe_reuses);
}

int main()
{
    object_starts_and_root_boundaries();
    independent_buffer_reuse();
    links_and_replacement();
    directory_topology();
    mounts_and_capacity();
    retained_provider_handoff_model();
    std::puts("PASS RAM namespace: hard links, retained replacement/unlink, inode reuse, directory moves, live paths, "
              "mounts, capacity and retained provider handoff model");
}
