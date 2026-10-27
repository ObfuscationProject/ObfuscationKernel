// The same real ext4 callback must reject the former VFS lock-across-I/O design.
#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4/layout.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace ok;
using namespace ok::fs;
using Bytes = std::vector<std::byte>;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "actual callback lock:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

namespace
{
class Disk final : public driver::BlockDevice
{
  public:
    explicit Disk(const Bytes &input) : bytes(input) {}
    driver::BlockGeometry geometry() const override { probe(); return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> output) override
    {
        probe(); CHECK(first * 512 + output.size() <= bytes.size());
        std::memcpy(output.data(), bytes.data() + first * 512, output.size()); return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> input) override
    {
        probe(); CHECK(first * 512 + input.size() <= bytes.size());
        std::memcpy(bytes.data() + first * 512, input.data(), input.size()); return {};
    }
    Status flush() override { probe(); return {}; }
    VirtualFileSystem *vfs{};
    bool active{};
    usize reentrant_busy{};
    Bytes bytes;
  private:
    void probe() const
    {
        if (!active) return;
        CHECK(vfs && vfs->lock_.try_lock());
        vfs->lock_.unlock();
        const auto nested = vfs->stat("/victim");
        CHECK(!nested && nested.status().code() == StatusCode::busy);
        ++const_cast<Disk *>(this)->reentrant_busy;
    }
};
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); testing::ext4_clock::reset();
    auto *file = std::fopen(argv[1], "rb"); CHECK(file);
    CHECK(!std::fseek(file, 0, SEEK_END));
    Bytes input(static_cast<usize>(std::ftell(file)));
    CHECK(!std::fseek(file, 0, SEEK_SET));
    CHECK(std::fread(input.data(), 1, input.size(), file) == input.size());
    std::fclose(file);
    Disk disk(input); auto storage = std::make_unique<Ext4FileSystem>();
    CHECK(storage->mount(disk)); auto *root = storage->root(); CHECK(root);
    auto vfs = std::make_unique<VirtualFileSystem>(); CHECK(vfs->mount("/", *root));
    CHECK(root->create("victim", NodeType::regular));
    disk.vfs = vfs.get(); disk.active = true;
    CHECK(vfs->stat("/victim"));
    CHECK(disk.reentrant_busy >= 1);
    CHECK(testing::ext4_clock::calls() == 0);
    std::puts("PASS actual ext4 callback lock control: provider I/O unlocked and reentrant VFS returns busy");
}
