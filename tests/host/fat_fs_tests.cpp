#include "ok/fs/fat_fs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
using namespace ok;
static_assert(!std::is_copy_constructible_v<fs::FatFileSystem>);
static_assert(!std::is_move_constructible_v<fs::FatFileSystem>);
static_assert(!std::is_copy_assignable_v<fs::FatFileSystem>);
static_assert(!std::is_move_assignable_v<fs::FatFileSystem>);
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #x);                                                   \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)
class Disk final : public driver::BlockDevice
{
    std::FILE *file_;
    u64 size_{};

  public:
    usize writes{}, fail_at{};
    explicit Disk(const char *path)
    {
        file_ = std::fopen(path, "r+b");
        CHECK(file_);
        CHECK(!std::fseek(file_, 0, SEEK_END));
        size_ = static_cast<u64>(std::ftell(file_));
    }
    ~Disk()
    {
        std::fclose(file_);
    }
    driver::BlockGeometry geometry() const override
    {
        return {size_ / 512, 512, true};
    }
    Status read_blocks(u64 sector, std::span<std::byte> bytes) override
    {
        if (sector >= size_ / 512 || bytes.size() % 512 || bytes.size() > size_ - sector * 512)
            return Status::invalid_argument();
        return !std::fseek(file_, static_cast<long>(sector * 512), SEEK_SET) &&
                       std::fread(bytes.data(), 1, bytes.size(), file_) == bytes.size()
                   ? Status{}
                   : Status::io_error();
    }
    Status write_blocks(u64 sector, std::span<const std::byte> bytes) override
    {
        if (sector >= size_ / 512 || bytes.size() % 512 || bytes.size() > size_ - sector * 512)
            return Status::invalid_argument();
        ++writes;
        if (fail_at && writes == fail_at)
            return Status::io_error("injected write failure");
        return !std::fseek(file_, static_cast<long>(sector * 512), SEEK_SET) &&
                       std::fwrite(bytes.data(), 1, bytes.size(), file_) == bytes.size()
                   ? Status{}
                   : Status::io_error();
    }
    Status flush() override
    {
        return !std::fflush(file_) && !::fsync(::fileno(file_)) ? Status{} : Status::io_error();
    }
};
int main(int argc, char **argv)
{
    CHECK(argc == 3 || argc == 4);
    Disk disk(argv[1]);
    static fs::FatFileSystem volume;
    auto mounted = volume.mount(disk);
    if (std::strcmp(argv[2], "reject") == 0)
    {
        CHECK(!mounted && !volume.root() && disk.writes == 0);
        std::puts("FAT_REJECT_PASS writes=0");
        return 0;
    }
    if (!mounted)
    {
        std::fprintf(stderr, "mount code=%u %.*s\n", static_cast<unsigned>(mounted.code()),
                     static_cast<int>(mounted.message().size()), mounted.message().data());
        return 1;
    }
    auto *root = volume.root();
    CHECK(root);
    if (std::strcmp(argv[2], "fault") == 0)
    {
        CHECK(argc == 4);
        disk.fail_at = static_cast<usize>(std::strtoul(argv[3], nullptr, 10));
        auto created = root->create_child("fault file.txt", {fs::NodeType::regular, 0644, 0, 0});
        Status status = created ? Status{} : created.status();
        if (created)
        {
            std::array<std::byte, 4096> bytes{};
            status = created.value()->write(0, bytes);
        }
        if (status)
            status = volume.sync();
        CHECK(!status && disk.writes >= disk.fail_at);
        const auto writes = disk.writes;
        CHECK(!root->create("must-not-write", fs::NodeType::regular));
        CHECK(!volume.sync());
        CHECK(disk.writes == writes);
        std::puts("FAT_WRITE_FAILURE_PASS disabled=1");
        return 0;
    }
    if (std::strcmp(argv[2], "contiguous") == 0)
    {
        auto *dir = root->lookup("Folder");
        CHECK(dir);
        auto *file = dir->lookup("Long file name.txt");
        CHECK(file);
        std::array<std::byte, 4096> bytes{};
        CHECK(file->write(9000, bytes));
        CHECK(file->truncate(7000));
        CHECK(volume.sync());
        std::puts("EXFAT_NOFATCHAIN_EXTEND_PASS");
        return 0;
    }
    if (std::strcmp(argv[2], "reject") == 0)
    {
        CHECK(false);
    }
    if (std::strcmp(argv[2], "write") == 0)
    {
        const auto before = disk.writes;
        CHECK(!root->create_child("wrong-owner", {fs::NodeType::regular, 0644, 1000, 0}));
        CHECK(!root->create_child("wrong-mode", {fs::NodeType::regular, 0600, 0, 0}));
        CHECK(disk.writes == before);
        CHECK(root->create("Folder", fs::NodeType::directory));
        auto *dir = root->lookup("folder");
        CHECK(dir);
        if (volume.info().variant == fs::FatVariant::fat32)
        {
            // Existing 8.3 aliases share the same case-insensitive namespace as LFNs.
            const auto writes = disk.writes;
            CHECK(root->create("f0000001.dat", fs::NodeType::regular).code() == StatusCode::already_exists);
            CHECK(disk.writes == writes);
            CHECK(root->lookup("F0000001.DAT") == dir);
            CHECK(root->create("F0000002.DAT", fs::NodeType::regular));
            CHECK(root->create("alias-safe.txt", fs::NodeType::regular));
            CHECK(root->remove("F0000002.DAT", false));
            CHECK(root->remove("alias-safe.txt", false));
        }
        CHECK(dir->create("Long file name.txt", fs::NodeType::regular));
        auto *file = dir->lookup("long FILE name.TXT");
        CHECK(file);
        std::array<std::byte, 4096> bytes;
        for (usize i = 0; i < bytes.size(); ++i)
            bytes[i] = std::byte(i % 251);
        CHECK(file->write(0, bytes));
        CHECK(file->write(5000, bytes));
        auto gap = file->read(4096, 904);
        CHECK(gap && gap.value().size == 904);
        for (usize i = 0; i < gap.value().size; ++i)
            CHECK(gap.value().data[i] == std::byte{});
        CHECK(file->truncate(6000));
        CHECK(file->truncate(7000));
        CHECK(root->create("delete.txt", fs::NodeType::regular));
        auto *deleted = root->lookup("delete.txt");
        CHECK(deleted && deleted->retain());
        CHECK(root->remove("delete.txt", false));
        CHECK(deleted->read(0, 1));
        CHECK(deleted->retain());
        CHECK(deleted->release());
        CHECK(deleted->release());
        CHECK(!deleted->read(0, 1));
        CHECK(root->create("empty", fs::NodeType::directory));
        CHECK(root->remove("empty", true));
        static fs::VirtualFileSystem vfs;
        CHECK(vfs.create("/media", fs::NodeType::directory));
        CHECK(vfs.mount("/media", *root));
        CHECK(vfs.create({}, "/media/vfs-open.txt", fs::NodeType::regular, 0644, 0, 0));
        auto held = vfs.resolve({}, "/media/vfs-open.txt");
        CHECK(held);
        const std::array<std::byte, 4> content{std::byte{'o'}, std::byte{'p'}, std::byte{'e'}, std::byte{'n'}};
        CHECK(held.value().node()->write(0, content));
        CHECK(vfs.remove({}, "/media/vfs-open.txt", false));
        CHECK(!vfs.resolve({}, "/media/vfs-open.txt"));
        auto retained_data = held.value().node()->read(0, 4);
        CHECK(retained_data && retained_data.value().size == 4 && retained_data.value().data[0] == std::byte{'o'});
        CHECK(held.value().node()->write(1, content));
        const auto busy_writes = disk.writes;
        CHECK(volume.sync().code() == StatusCode::busy);
        CHECK(disk.writes == busy_writes);
        CHECK(held.value().release());
        // Retained VFS operations must reclaim identities beyond the fixed table size.
        for (usize i = 0; i < fs::FatFileSystem::max_nodes * 4; ++i)
        {
            CHECK(vfs.create({}, "/media/recycled.txt", fs::NodeType::regular, 0644, 0, 0));
            CHECK(vfs.remove({}, "/media/recycled.txt", false));
        }
        CHECK(!deleted->read(0, 1)); // The earlier borrowed identity remains a tombstone.
        CHECK(volume.sync());
    }
    else
    {
        auto *dir = root->lookup("Folder");
        CHECK(dir);
        auto *file = dir->lookup("Long file name.txt");
        CHECK(file && file->metadata().size == 7000);
        auto data = file->read(0, 4096);
        CHECK(data && data.value().size == 4096);
        for (usize i = 0; i < 4096; ++i)
            CHECK(data.value().data[i] == std::byte(i % 251));
        data = file->read(5000, 2000);
        CHECK(data && data.value().size == 2000);
        for (usize i = 0; i < 1000; ++i)
            CHECK(data.value().data[i] == std::byte(i % 251));
        for (usize i = 1000; i < 2000; ++i)
            CHECK(data.value().data[i] == std::byte{});
        CHECK(!root->lookup("delete.txt") && !root->lookup("empty"));
        CHECK(disk.writes == 0);
    }
    std::puts("FAT_VFS_PASS hosted=1 guest=0");
}
