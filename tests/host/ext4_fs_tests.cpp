#include "ok/fs/ext4_fs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        auto *n = dir->lookup("reuse");
        CHECK(n);
        CHECK(n->write(0, bytes("old")));
        CHECK(n->truncate(1));
        CHECK(n->truncate(64));
        read = n->read(1, 63);
        CHECK(read && read.value().size == 63);
        for (usize j = 0; j < 63; ++j)
            CHECK(read.value().data[j] == std::byte{0});
        CHECK(dir->remove("reuse", false));
    }
    namespace_mutations(*root);
    CHECK(file->sync());
    CHECK(filesystem.sync());
    std::puts("PASS ext4 write: overwrite, holes, zero write, truncate, open unlink, mkdir/rmdir, inode/block reuse, "
              "links, replacement, directory topology");
}
