// An actual ext4 reclaim failure must survive the VFS PathReference boundary,
// and a last release while another CPU owns the ext4 gate is deferred to that
// owner instead of returning busy.
#include "ext4_realtime_model.hpp"
#include "ok/smp/lock.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Node phases and gate state are private; this hosted test inspects them.
#define private public
#include "ok/fs/ext4/layout.hpp"
#undef private

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
    std::atomic<usize> writes{}, flushes{};
    usize failures{};
    bool fail_write{}, fail_when_retiring{};
    Ext4FileSystem *owner{};
    Ext4Node *watched{};
    // The CPU (+1) to stop in its next callback, owning the gate, until `go`.
    std::atomic<u32> hold_cpu{0};
    std::atomic<bool> held{false}, go{false};
  private:
    void unlocked() const
    {
        auto &self = const_cast<Disk &>(*this);
        if (self.hold_cpu.load() == static_cast<u32>(smp::current_cpu()) + 1)
        {
            self.hold_cpu = 0;
            self.held = true;
            while (!self.go.load()) std::this_thread::yield();
        }
        if (owner) { CHECK(owner->identity_lock_.try_lock()); owner->identity_lock_.unlock(); }
    }
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
    const auto writes = disk.writes.load(), flushes = disk.flushes.load();
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
    const auto writes = disk.writes.load(), flushes = disk.flushes.load();
    CHECK(!root->create("after-failure", NodeType::regular));
    CHECK(!storage->sync());
    CHECK(storage->mount(disk).code() == StatusCode::busy);
    CHECK(disk.writes == writes && disk.flushes == flushes);
}

// CPU 1 owns the gate inside a device callback while CPU 0 drops the last
// path reference to an unlinked node: the release succeeds at once with no
// I/O, and CPU 1's operation reclaims the inode when it finishes.
void release_while_other_cpu_owns(const Bytes &input)
{
    Disk disk(input);
    auto storage = std::make_unique<Ext4FileSystem>();
    CHECK(storage->mount(disk));
    auto *root = storage->root(); CHECK(root);
    auto vfs = std::make_unique<VirtualFileSystem>();
    CHECK(vfs->mount("/", *root));
    CHECK(root->create("victim", NodeType::regular));
    CHECK(root->create("other", NodeType::regular));
    auto path = vfs->resolve({}, "/victim"); CHECK(path);
    auto *node = static_cast<Ext4Node *>(path.value().node());
    CHECK(root->remove("victim", false));
    CHECK(node->phase_ == Ext4Node::Phase::detached && node->references_ == 1);
    disk.owner = storage.get();
    disk.hold_cpu = 1 + 1;
    std::thread owner([&] {
        const smp::ScopedHostedCpu cpu(1);
        CHECK(root->sync());
    });
    while (!disk.held.load()) std::this_thread::yield();
    const auto writes = disk.writes.load();
    CHECK(storage->operation_owner_.load() == 1 + 1);
    CHECK(path.value().release());
    CHECK(!node->references_ && node->phase_ == Ext4Node::Phase::detached && disk.writes.load() == writes);
    disk.go = true;
    owner.join();
    CHECK(node->phase_ == Ext4Node::Phase::vacant || node->phase_ == Ext4Node::Phase::tombstone);
    CHECK(disk.writes.load() > writes && !storage->failed_);
    CHECK(!storage->operation_active_ && storage->operation_owner_.load() == 0);
    CHECK(path.value().release());
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
    release_while_other_cpu_owns(input);
    CHECK(testing::ext4_clock::calls() == 0);
    std::puts("PASS actual VFS/ext4 release errors: path release and unlink cleanup propagated, retiring quarantine, "
              "no repeated writes; a release while another CPU owns the gate is deferred to it");
}
