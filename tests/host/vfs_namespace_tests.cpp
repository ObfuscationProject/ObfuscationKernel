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
        CHECK(vfs.lookup("/tmp/reuse") != source && vfs.lookup("/tmp/reuse") != victim);
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
int main()
{
    independent_buffer_reuse();
    links_and_replacement();
    directory_topology();
    mounts_and_capacity();
    std::puts("PASS RAM namespace: hard links, retained replacement/unlink, inode reuse, directory moves, live paths, "
              "mounts, capacity");
}
