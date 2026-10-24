// An actual ext4 reclaim failure must survive the VFS PathReference boundary.
#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4_fs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace ok;
using namespace ok::fs;
using Bytes = std::vector<std::byte>;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "vfs/ext4 release:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

namespace
{
class Disk final : public driver::BlockDevice
{
  public:
    explicit Disk(const Bytes &input) : bytes(input) {}
    driver::BlockGeometry geometry() const override
    { unlocked(); return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> output) override
    {
        unlocked();
        CHECK(first * 512 + output.size() <= bytes.size());
        std::memcpy(output.data(), bytes.data() + first * 512, output.size());
        return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> input) override
    {
        unlocked();
        ++writes;
        if (fail_write || (fail_when_retiring && watched && watched->phase_ == Ext4Node::Phase::retiring))
        {
            fail_write = false;
            fail_when_retiring = false;
            ++failures;
            return Status::io_error("injected ext4 reclaim failure");
        }
        CHECK(first * 512 + input.size() <= bytes.size());
        std::memcpy(bytes.data() + first * 512, input.data(), input.size());
        return {};
    }
    Status flush() override { unlocked(); ++flushes; return {}; }
    Bytes bytes;
    usize writes{}, flushes{}, failures{};
    bool fail_write{}, fail_when_retiring{};
    Ext4FileSystem *owner{};
    Ext4Node *watched{};
  private:
    void unlocked() const
    { if (owner) { CHECK(owner->identity_lock_.try_lock()); owner->identity_lock_.unlock(); } }
};

void release_error(const Bytes &input)
{
    Disk disk(input);
    auto storage = std::make_unique<Ext4FileSystem>();
    CHECK(storage->mount(disk));
    disk.owner = storage.get();
    auto *root = storage->root(); CHECK(root);
    auto vfs = std::make_unique<VirtualFileSystem>();
    CHECK(vfs->mount("/", *root));
    CHECK(root->create("victim", NodeType::regular));
    auto path = vfs->resolve({}, "/victim"); CHECK(path);
    auto *node = static_cast<Ext4Node *>(path.value().node());
    CHECK(node && node->references_ == 1 && node->phase_ == Ext4Node::Phase::linked);
    CHECK(root->remove("victim", false));
    CHECK(node->phase_ == Ext4Node::Phase::detached && node->references_ == 1);
    disk.fail_write = true;
    const auto status = path.value().release();
    CHECK(status.code() == StatusCode::io_error);
    CHECK(status.message() == "injected ext4 reclaim failure");
    CHECK(!node->references_ && node->phase_ == Ext4Node::Phase::retiring);
    CHECK(storage->failed_ && !storage->operation_active_);
    const auto writes = disk.writes, flushes = disk.flushes;
    CHECK(path.value().release());
    CHECK(!node->query_metadata());
    CHECK(!root->create("after-failure", NodeType::regular));
    CHECK(!storage->sync());
    CHECK(storage->mount(disk).code() == StatusCode::busy);
    CHECK(disk.writes == writes && disk.flushes == flushes);
}

void unlink_cleanup_error(const Bytes &input)
{
    Disk disk(input);
    auto storage = std::make_unique<Ext4FileSystem>();
    CHECK(storage->mount(disk));
    disk.owner = storage.get();
    auto *root = storage->root(); CHECK(root);
    auto vfs = std::make_unique<VirtualFileSystem>();
    CHECK(vfs->mount("/", *root));
    CHECK(root->create("victim", NodeType::regular));
    auto found = root->lookup_child_retained("victim"); CHECK(found);
    auto *node = static_cast<Ext4Node *>(found.value());
    CHECK(node->release());
    CHECK(node->phase_ == Ext4Node::Phase::linked && !node->references_);
    disk.watched = node;
    disk.fail_when_retiring = true;
    const auto status = vfs->unlink("/victim");
    CHECK(status.code() == StatusCode::io_error);
    CHECK(status.message() == "injected ext4 reclaim failure");
    CHECK(disk.failures == 1 && !disk.fail_when_retiring);
    CHECK(node->phase_ == Ext4Node::Phase::retiring && !node->references_);
    CHECK(storage->failed_ && !storage->operation_active_);
    const auto writes = disk.writes, flushes = disk.flushes;
    CHECK(!root->create("after-failure", NodeType::regular));
    CHECK(!storage->sync());
    CHECK(storage->mount(disk).code() == StatusCode::busy);
    CHECK(disk.writes == writes && disk.flushes == flushes);
}
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    testing::ext4_clock::reset();
    auto *file = std::fopen(argv[1], "rb"); CHECK(file);
    CHECK(!std::fseek(file, 0, SEEK_END));
    Bytes input(static_cast<usize>(std::ftell(file)));
    CHECK(!std::fseek(file, 0, SEEK_SET));
    CHECK(std::fread(input.data(), 1, input.size(), file) == input.size());
    std::fclose(file);
    release_error(input);
    unlink_cleanup_error(input);
    CHECK(testing::ext4_clock::calls() == 0);
    std::puts("PASS actual VFS/ext4 release errors: path release and unlink cleanup propagated, retiring quarantine, no repeated writes");
}
