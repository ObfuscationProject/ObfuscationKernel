#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4/layout.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
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
class FileDevice final : public driver::BlockDevice
{
  public:
    explicit FileDevice(const char *path)
    {
        file = std::fopen(path, "r+b");
        CHECK(file);
        std::fseek(file, 0, SEEK_END);
        size = std::ftell(file);
    }
    ~FileDevice() override
    {
        std::fclose(file);
    }
    driver::BlockGeometry geometry() const override
    {
        return {size / 512, 512, true};
    }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        if (first > size / 512 || out.size() % 512 || out.size() > size - first * 512)
            return Status::fault();
        if (std::fseek(file, static_cast<long>(first * 512), SEEK_SET) ||
            std::fread(out.data(), 1, out.size(), file) != out.size())
            return Status::fault();
        return Status::success();
    }
    Status write_blocks(u64 first, std::span<const std::byte> in) override
    {
        if (first > size / 512 || in.size() % 512 || in.size() > size - first * 512)
            return Status::fault();
        if (std::fseek(file, static_cast<long>(first * 512), SEEK_SET) ||
            std::fwrite(in.data(), 1, in.size(), file) != in.size())
            return Status::fault();
        return Status::success();
    }
    Status flush() override
    {
        return !std::fflush(file) && !::fsync(::fileno(file)) ? Status::success() : Status::fault();
    }

  private:
    std::FILE *file;
    u64 size;
};
std::span<const std::byte> bytes(const char *s)
{
    return {reinterpret_cast<const std::byte *>(s), std::strlen(s)};
}
void check_content(fs::Node &node, const char *expected)
{
    auto value = node.read(0, fs::max_file_data);
    CHECK(value && value.value().size == std::strlen(expected));
    CHECK(std::memcmp(value.value().data.data(), expected, value.value().size) == 0);
}
void symlink_content(fs::Node &parent, const char *name, usize length, char fill, bool owned = false)
{
    auto *link = parent.lookup(name);
    CHECK(link && link->metadata().type == fs::NodeType::symlink);
    const auto metadata = link->metadata();
    CHECK(metadata.size == length && metadata.mode == (fs::mode_symlink | 0777));
    CHECK(metadata.blocks == (length < 60 ? 0 : metadata.block_size / 512));
    if (owned)
        CHECK(metadata.uid == 0x12345678 && metadata.gid == 0x87654321);
    const std::string target(length, fill);
    check_content(*link, target.c_str());
    auto part = link->read(length - 1, 16);
    CHECK(part && part.value().size == 1 && part.value().data[0] == std::byte(fill));
    part = link->read(0, 1);
    CHECK(part && part.value().size == 1 && part.value().data[0] == std::byte(fill));
    part = link->read(length, 1);
    CHECK(part && part.value().size == 0);
    CHECK(!link->write(0, bytes("overwrite")) && !link->truncate(0));
}
void symlinks(fs::Node &root, bool write)
{
    const auto block_size = root.metadata().block_size;
    for (const auto length : {usize{1}, usize{59}, usize{60}, usize{255}, usize{block_size - 1}})
    {
        char name[24];
        std::snprintf(name, sizeof(name), "s%zu", length);
        const std::string target(length, 's');
        if (write)
            CHECK(root.create_symlink(name, target, 0x12345678, 0x87654321));
        symlink_content(root, name, length, 's', true);
    }
    symlink_content(root, "upstream59", 59, 'u');
    symlink_content(root, "upstream60", 60, 'u');
    symlink_content(root, "upstreammax", block_size - 1, 'u');
    symlink_content(root, "legacy60", 60, 'l');
    if (!write)
    {
        CHECK(!root.lookup("legacy-drop"));
        auto *persistent = root.lookup("symlink-renamed");
        CHECK(persistent && root.lookup("symlink-alias") == persistent && persistent->metadata().link_count == 2);
        check_content(*persistent, "../persist/notes");
        return;
    }
    CHECK(!root.create_symlink("s1", "exists", 0, 0));
    CHECK(!root.create_symlink("empty", "", 0, 0));
    CHECK(!root.create_symlink("nul", std::string_view("a\0b", 3), 0, 0));
    CHECK(!root.create_symlink("oversized", std::string(block_size, 'x'), 0, 0));
    CHECK(root.create_symlink("symlink-original", "../persist/notes", 0, 0));
    auto *source = root.lookup("symlink-original");
    CHECK(source && root.link("symlink-alias", *source));
    CHECK(root.rename("symlink-original", root, "symlink-renamed"));
    CHECK(!root.lookup("symlink-original") && source->metadata().link_count == 2);
    for (const auto length : {usize{59}, usize{60}})
    {
        const std::string target(length, 't');
        CHECK(root.create_symlink("temporary-link", target, 0, 0));
        auto *victim = root.lookup("temporary-link");
        CHECK(victim && victim->retain());
        CHECK(root.create_symlink("replacement-link", "replacement", 0, 0));
        CHECK(root.rename("replacement-link", root, "temporary-link"));
        CHECK(victim->metadata().link_count == 0);
        check_content(*victim, target.c_str());
        check_content(*root.lookup("temporary-link"), "replacement");
        CHECK(victim->release() && root.remove("temporary-link", false));
        // Symlink replacement of a regular file and the reverse release the
        // correct allocation representation without following either target.
        CHECK(root.create_symlink("temporary-link", target, 0, 0));
        CHECK(root.create("replacement-file", fs::NodeType::regular));
        CHECK(root.rename("replacement-file", root, "temporary-link"));
        CHECK(root.lookup("temporary-link")->metadata().type == fs::NodeType::regular);
        CHECK(root.create_symlink("replacement-link", target, 0, 0));
        CHECK(root.rename("replacement-link", root, "temporary-link"));
        CHECK(root.remove("temporary-link", false));
    }
    auto *legacy = root.lookup("legacy60");
    CHECK(legacy && root.link("legacy-alias", *legacy));
    CHECK(root.rename("legacy-alias", root, "legacy-renamed"));
    CHECK(root.remove("legacy-renamed", false) && legacy->metadata().link_count == 1);
    symlink_content(root, "legacy-drop", 60, 'l');
    auto *legacy_drop = root.lookup("legacy-drop");
    CHECK(legacy_drop && legacy_drop->retain() && root.remove("legacy-drop", false));
    check_content(*legacy_drop, std::string(60, 'l').c_str());
    CHECK(legacy_drop->release());
}
void namespace_mutations(fs::Node &root)
{
    CHECK(root.create("namespace", fs::NodeType::directory));
    auto *parent = root.lookup("namespace");
    CHECK(parent);
    CHECK(parent->create("left", fs::NodeType::directory));
    CHECK(parent->create("right", fs::NodeType::directory));
    auto *left = parent->lookup("left"), *right = parent->lookup("right");
    CHECK(left && right);
    CHECK(left->create("source", fs::NodeType::regular));
    auto *source = left->lookup("source");
    CHECK(source && source->retain());
    CHECK(source->write(0, bytes("source")));
    CHECK(left->link("alias", *source));
    CHECK(right->link("remote", *source));
    CHECK(source->metadata().link_count == 3 && left->lookup("alias") == source);
    CHECK(left->rename("source", *left, "alias"));
    CHECK(left->lookup("source") == source && left->lookup("alias") == source);
    CHECK(right->create("victim", fs::NodeType::regular));
    auto *victim = right->lookup("victim");
    CHECK(victim && victim->retain());
    CHECK(victim->write(0, bytes("victim")));
    CHECK(left->rename("source", *right, "victim"));
    CHECK(!left->lookup("source") && right->lookup("victim") == source);
    CHECK(victim->metadata().link_count == 0 && source->metadata().link_count == 3);
    check_content(*victim, "victim");
    CHECK(victim->write(0, bytes("Victim")));
    check_content(*source, "source");
    CHECK(left->remove("alias", false));
    CHECK(source->metadata().link_count == 2);
    CHECK(right->remove("remote", false));
    CHECK(source->metadata().link_count == 1);
    CHECK(right->remove("victim", false));
    CHECK(source->metadata().link_count == 0);
    check_content(*source, "source");
    check_content(*victim, "Victim");
    CHECK(source->release() && victim->release());
    CHECK(left->create("durable", fs::NodeType::regular));
    source = left->lookup("durable");
    CHECK(source && source->write(0, bytes("persistent aliases")));
    CHECK(right->link("alias", *source));
    CHECK(left->rename("durable", *right, "durable"));
    CHECK(source->metadata().link_count == 2);

    CHECK(left->create("sub", fs::NodeType::directory));
    auto *sub = left->lookup("sub");
    CHECK(sub && sub->retain());
    CHECK(sub->create("child", fs::NodeType::directory));
    auto *child = sub->lookup("child");
    CHECK(child && child->retain());
    CHECK(left->metadata().link_count == 3 && right->metadata().link_count == 2);
    CHECK(left->rename("sub", *left, "longer-directory-name"));
    CHECK(left->metadata().link_count == 3);
    CHECK(left->rename("longer-directory-name", *right, "moved"));
    CHECK(left->metadata().link_count == 2 && right->metadata().link_count == 3);
    CHECK(sub->lookup("..") == right);
    auto path = child->directory_path(root);
    CHECK(path && path.value().view() == "/namespace/right/moved/child");
    CHECK(!parent->rename("right", *child, "cycle"));
    CHECK(parent->lookup("right") == right && right->lookup("moved") == sub);
    CHECK(left->create("empty", fs::NodeType::directory));
    auto *empty = left->lookup("empty");
    CHECK(empty && empty->retain());
    CHECK(right->rename("moved", *left, "empty"));
    CHECK(empty->metadata().link_count == 0 && !empty->directory_path(root));
    CHECK(left->metadata().link_count == 3 && right->metadata().link_count == 2);
    CHECK(sub->lookup("..") == left);
    path = child->directory_path(root);
    CHECK(path && path.value().view() == "/namespace/left/empty/child");
    CHECK(right->create("nonempty", fs::NodeType::directory));
    auto *nonempty = right->lookup("nonempty");
    CHECK(nonempty);
    CHECK(nonempty->create("file", fs::NodeType::regular));
    CHECK(left->rename("empty", *right, "nonempty").code() == StatusCode::not_empty);
    CHECK(left->rename("empty", *right, "durable").code() == StatusCode::not_directory);
    CHECK(right->rename("durable", *left, "empty").code() == StatusCode::is_directory);
    CHECK(!right->link("directory-link", *sub));
    CHECK(!right->link("alias", *source));
    CHECK(left->lookup("empty") == sub && sub->lookup("child") == child);
    CHECK(nonempty->remove("file", false));
    CHECK(right->remove("nonempty", true));
    CHECK(empty->release());
    CHECK(child->release());
    CHECK(sub->release());

    // Replacement within one directory must decrement the parent's link count once.
    CHECK(right->create("one", fs::NodeType::directory));
    CHECK(right->create("two", fs::NodeType::directory));
    CHECK(right->metadata().link_count == 4);
    CHECK(right->rename("one", *right, "two"));
    CHECK(right->metadata().link_count == 3);
    CHECK(right->remove("two", true));
    CHECK(right->metadata().link_count == 2);
}
void namespace_reopened(fs::Node &root)
{
    auto *parent = root.lookup("namespace");
    CHECK(parent);
    auto *left = parent->lookup("left"), *right = parent->lookup("right");
    CHECK(left && right);
    auto *durable = right->lookup("durable");
    CHECK(durable && right->lookup("alias") == durable);
    CHECK(durable->metadata().link_count == 2);
    check_content(*durable, "persistent aliases");
    CHECK(!left->lookup("durable") && !right->lookup("remote") && !right->lookup("victim"));
    auto *sub = left->lookup("empty");
    CHECK(sub && sub->lookup("child"));
    CHECK(sub->lookup("..") == left && sub->metadata().link_count == 3);
    CHECK(left->metadata().link_count == 3 && right->metadata().link_count == 2);
    auto path = sub->lookup("child")->directory_path(root);
    CHECK(path && path.value().view() == "/namespace/left/empty/child");
}
int main(int argc, char **argv)
{
    testing::ext4_clock::reset();
    struct LegacyClockGuard { ~LegacyClockGuard() { CHECK(testing::ext4_clock::calls() == 0); } } legacy_clock_guard;
    CHECK(argc == 3);
    FileDevice disk(argv[1]);
    fs::Ext4FileSystem filesystem;
    auto mounted = filesystem.mount(disk, std::strcmp(argv[2], "readonly") != 0);
    if (std::strcmp(argv[2], "reject") == 0)
    {
        CHECK(!mounted);
        std::puts("PASS unsupported/unclean ext4 is rejected without modification");
        return 0;
    }
    if (!mounted)
        std::fprintf(stderr, "%.*s\n", int(mounted.message().size()), mounted.message().data());
    CHECK(mounted);
    auto *root = filesystem.root();
    CHECK(root);
    if (std::strcmp(argv[2], "readonly") == 0)
    {
        CHECK(root->lookup("persist"));
        CHECK(!root->create("forbidden", fs::NodeType::regular));
        CHECK(!root->create_symlink("forbidden", "target", 0, 0));
        symlinks(*root, false);
        CHECK(!root->chmod(0700));
        auto *directory = root->lookup("persist");
        CHECK(directory && !root->link("forbidden", *directory->lookup("notes")));
        CHECK(!directory->rename("notes", *directory, "renamed"));
        std::puts("PASS read-only ext4 mount rejects mutation");
        return 0;
    }
    if (std::strcmp(argv[2], "verify") == 0)
    {
        auto *dir = root->lookup("persist");
        CHECK(dir);
        auto *file = dir->lookup("notes");
        CHECK(file);
        auto result = file->read(0, 4096);
        CHECK(result && result.value().size == 4096);
        CHECK(result.value().data[0] == std::byte{'X'});
        CHECK(file->metadata().size == 9000);
        result = file->read(8999, 5);
        CHECK(result && result.value().size == 1 && result.value().data[0] == std::byte{'!'});
        CHECK(!dir->lookup("gone"));
        CHECK(!dir->lookup("sub"));
        namespace_reopened(*root);
        symlinks(*root, false);
        std::puts("PASS ext4 reopen: persisted data, size, directory entries");
        return 0;
    }
    CHECK(root->create("persist", fs::NodeType::directory));
    auto *dir = root->lookup("persist");
    CHECK(dir);
    CHECK(dir->create("notes", fs::NodeType::regular));
    auto *file = dir->lookup("notes");
    CHECK(file);
    CHECK(file->write(0, bytes("abcdef")));
    CHECK(file->write(0, bytes("X")));
    CHECK(file->write(0, {}));
    CHECK(file->metadata().size == 6);
    auto read = file->read(0, 10);
    CHECK(read && read.value().size == 6 && read.value().data[5] == std::byte{'f'});
    CHECK(file->write(8999, bytes("!")));
    CHECK(file->metadata().size == 9000);
    read = file->read(4000, 4096);
    CHECK(read && read.value().size == 4096);
    for (usize i = 0; i < read.value().size; ++i)
        CHECK(read.value().data[i] == std::byte{0});
    CHECK(dir->create("gone", fs::NodeType::regular));
    auto *gone = dir->lookup("gone");
    CHECK(gone);
    CHECK(gone->retain());
    CHECK(gone->write(0, bytes("open unlink")));
    CHECK(dir->remove("gone", false));
    CHECK(!dir->lookup("gone"));
    read = gone->read(0, 100);
    CHECK(read && read.value().size == 11);
    CHECK(gone->release());
    CHECK(dir->create("sub", fs::NodeType::directory));
    auto *sub = dir->lookup("sub");
    CHECK(sub);
    CHECK(sub->create("child", fs::NodeType::regular));
    CHECK(!dir->remove("sub", true));
    CHECK(sub->remove("child", false));
    CHECK(dir->remove("sub", true));
    for (int i = 0; i < 150; ++i)
    {
        CHECK(dir->create("reuse", fs::NodeType::regular));
        auto retained = dir->lookup_child_retained("reuse");
        CHECK(retained);
        auto *n = retained.value();
        CHECK(n->write(0, bytes("old")));
        CHECK(n->truncate(1));
        CHECK(n->truncate(64));
        read = n->read(1, 63);
        CHECK(read && read.value().size == 63);
        for (usize j = 0; j < 63; ++j)
            CHECK(read.value().data[j] == std::byte{0});
        CHECK(dir->remove("reuse", false));
        CHECK(n->release());
    }
    // The node cache holds 128 identities. Unreferenced identities that were
    // never handed out raw are evicted, so one mount reaches more inodes; a
    // retained node stays resident throughout.
    {
        auto many = root->create_child_retained("many", {fs::NodeType::directory, 0755, 0, 0});
        CHECK(many);
        auto *held = many.value();
        constexpr int directories = 8, per_directory = 40;
        std::array<u64, directories * per_directory> inodes{};
        for (int d = 0; d < directories; ++d)
        {
            char name[8]; std::snprintf(name, sizeof(name), "d%d", d);
            auto sub = held->create_child_retained(name, {fs::NodeType::directory, 0755, 0, 0}); CHECK(sub);
            for (int f = 0; f < per_directory; ++f)
            {
                char leaf[8]; std::snprintf(leaf, sizeof(leaf), "f%d", f);
                auto created = sub.value()->create_child_retained(leaf, {fs::NodeType::regular, 0644, 0, 0});
                CHECK(created);
                inodes[d * per_directory + f] = created.value()->query_metadata().value().attributes.inode;
                CHECK(created.value()->release());
            }
            CHECK(sub.value()->release());
        }
        for (int d = 0; d < directories; ++d)
        {
            char name[8]; std::snprintf(name, sizeof(name), "d%d", d);
            auto sub = held->lookup_child_retained(name); CHECK(sub);
            for (int f = 0; f < per_directory; ++f)
            {
                char leaf[8]; std::snprintf(leaf, sizeof(leaf), "f%d", f);
                auto found = sub.value()->lookup_child_retained(leaf); CHECK(found);
                CHECK(found.value()->query_metadata().value().attributes.inode == inodes[d * per_directory + f]);
                CHECK(found.value()->release());
            }
            CHECK(sub.value()->release());
        }
        CHECK(held->query_metadata().value().type == fs::NodeType::directory);
        CHECK(held->release());
    }
    namespace_mutations(*root);
    symlinks(*root, true);
    CHECK(file->sync());
    CHECK(filesystem.sync());
    std::puts("PASS ext4 write: overwrite, holes, zero write, truncate, open unlink, mkdir/rmdir, inode/block reuse, "
              "links, replacement, directory topology, inline/extent/direct-block symlinks");
}
