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
    CHECK(file->sync());
    CHECK(filesystem.sync());
    std::puts("PASS ext4 write: overwrite, holes, zero write, truncate, open unlink, mkdir/rmdir, inode/block reuse");
}
