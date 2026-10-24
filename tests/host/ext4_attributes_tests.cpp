#include "ext4_realtime_model.hpp"
// File-backed integration with raw-inode oracle supplied by the Python runner.
#include "ok/fs/ext4_fs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
using namespace ok;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::abort(); } } while (false)
class Disk final : public driver::BlockDevice
{
    std::FILE *file_; u64 bytes_;
  public:
    usize writes{}, flushes{};
    explicit Disk(const char *path) : file_(std::fopen(path, "r+b"))
    { CHECK(file_); CHECK(std::fseek(file_, 0, SEEK_END) == 0); bytes_ = std::ftell(file_); }
    ~Disk() override { std::fclose(file_); }
    driver::BlockGeometry geometry() const override { return {bytes_ / 512, 512, true}; }
    Status read_blocks(u64 start, std::span<std::byte> out) override
    {
        if (start > bytes_ / 512 || out.size() % 512 || out.size() > bytes_ - start * 512) return Status::fault();
        if (std::fseek(file_, static_cast<long>(start * 512), SEEK_SET) || std::fread(out.data(), 1, out.size(), file_) != out.size()) return Status::io_error();
        return {};
    }
    Status write_blocks(u64 start, std::span<const std::byte> in) override
    {
        ++writes;
        if (start > bytes_ / 512 || in.size() % 512 || in.size() > bytes_ - start * 512) return Status::fault();
        if (std::fseek(file_, static_cast<long>(start * 512), SEEK_SET) || std::fwrite(in.data(), 1, in.size(), file_) != in.size()) return Status::io_error();
        return {};
    }
    Status flush() override { ++flushes; return std::fflush(file_) == 0 ? Status{} : Status::io_error(); }
};
void absent(fs::Node &node)
{
    auto value = node.query_metadata(); CHECK(value);
    auto &attributes = value.value().attributes;
    CHECK(attributes.has(fs::Attribute::inode) && attributes.inode != 0 && !attributes.has(fs::Attribute::device));
    CHECK(!attributes.has(fs::Attribute::access_time) && !attributes.has(fs::Attribute::modification_time) && !attributes.has(fs::Attribute::change_time));
    CHECK(attributes.access.source == fs::TimestampSource::unavailable && attributes.modification.source == fs::TimestampSource::unavailable && attributes.change.source == fs::TimestampSource::unavailable);
}
int main(int argc, char **argv)
{
    testing::ext4_clock::reset();
    struct LegacyClockGuard { ~LegacyClockGuard() { CHECK(testing::ext4_clock::calls() == 0); } } legacy_clock_guard;
    CHECK(argc == 11);
    Disk disk(argv[1]); auto filesystem = std::make_unique<fs::Ext4FileSystem>();
    const bool writable = std::strcmp(argv[2], "write") == 0;
    CHECK(filesystem->mount(disk, writable)); auto *root = filesystem->root(); CHECK(root);
    auto *node = root->lookup("sample"); CHECK(node);
    if (std::strcmp(argv[2], "invalid") == 0)
    {
        CHECK(!node->query_metadata()); CHECK(disk.writes == 0 && disk.flushes == 0);
        std::puts("PASS malformed read-only timestamp rejected without disk mutation"); return 0;
    }
    if (!writable)
    {
        auto metadata = node->query_metadata(); CHECK(metadata); const auto &value = metadata.value();
        CHECK(value.size == 7 && (value.mode & 0777) == 0640 && value.link_count == 1);
        CHECK(value.attributes.inode == std::strtoull(argv[3], nullptr, 10) && value.attributes.has(fs::Attribute::inode));
        CHECK(!value.attributes.has(fs::Attribute::device));
        const fs::EpochTimestamp values[] = {value.attributes.access, value.attributes.modification, value.attributes.change};
        const fs::Attribute flags[] = {fs::Attribute::access_time, fs::Attribute::modification_time, fs::Attribute::change_time};
        for (usize index = 0; index < 3; ++index)
        {
            CHECK(value.attributes.has(flags[index]));
            CHECK(values[index].source == fs::TimestampSource::readonly_ext4);
            CHECK(values[index].seconds == std::strtoll(argv[4 + index * 2], nullptr, 10));
            CHECK(values[index].nanoseconds == std::strtoul(argv[5 + index * 2], nullptr, 10));
        }
        CHECK(root->query_metadata().value().attributes.inode == 2);
        CHECK(node->metadata().attributes.inode == value.attributes.inode);
        CHECK(disk.writes == 0 && disk.flushes == 0);
    }
    else
    {
        absent(*node); const std::array<std::byte, 1> byte{std::byte{'x'}};
        CHECK(node->write(0, byte)); absent(*node);
        CHECK(node->truncate(3)); absent(*node);
        CHECK(node->chmod(0600)); absent(*node);
        CHECK(node->chown(1000, 1000)); absent(*node);
        CHECK(root->link("alias", *node)); absent(*node);
        CHECK(root->rename("sample", *root, "renamed")); absent(*node);
        CHECK(node->retain()); CHECK(root->remove("renamed", false) && root->remove("alias", false)); absent(*node); CHECK(node->release());
        CHECK(root->create("created", fs::NodeType::regular)); absent(*root->lookup("created"));
        CHECK(root->create("directory", fs::NodeType::directory)); absent(*root->lookup("directory"));
        CHECK(root->create_symlink("symlink", "created", 0, 0)); absent(*root->lookup("symlink"));
        CHECK(filesystem->sync());
    }
    std::puts("PASS ext4 inode identity and truthful timestamp provider");
}
