// Model test of the ext4 operation gate across CPUs: host threads stand in
// for CPUs through ScopedHostedCpu over a file-backed image; no guest CPU,
// interrupt or device transport executes. Covers concurrent read, write and
// fsync from two CPUs, a waiter marked SPINNING, busy only on same-CPU
// reentry, a detached node released while another CPU owns the gate, and
// (with OK_LOCK_DEBUG) the hold recorded at lock rank fs_provider.
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

// Gate state is private; this hosted mechanism test inspects it directly.
#define private public
#include "ok/fs/ext4/layout.hpp"
#undef private

using namespace ok;
using namespace ok::fs;
using Bytes = std::vector<std::byte>;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "ext4 gate concurrency:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

namespace
{
// Device I/O happens only inside the gate, so the disk itself never sees two
// callers; it records any overlap instead of serializing it. A designated CPU
// can be held inside a callback, owning the gate, until the test lets it go.
class Disk final : public driver::BlockDevice
{
  public:
    explicit Disk(const Bytes &input) : bytes(input) {}
    driver::BlockGeometry geometry() const override { enter(); leave(); return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> output) override
    {
        enter();
        CHECK(first * 512 + output.size() <= bytes.size());
        std::memcpy(output.data(), bytes.data() + first * 512, output.size());
        leave();
        return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> input) override
    {
        enter();
        CHECK(first * 512 + input.size() <= bytes.size());
        std::memcpy(bytes.data() + first * 512, input.data(), input.size());
        writes.fetch_add(1);
        leave();
        return {};
    }
    Status flush() override { enter(); flushes.fetch_add(1); leave(); return {}; }

    Bytes bytes;
    std::atomic<int> inside{0};
    std::atomic<bool> overlap{false};
    std::atomic<usize> writes{0}, flushes{0};
    // Hold: the CPU (+1) to stop in its next callback, and the release.
    std::atomic<u32> hold_cpu{0};
    std::atomic<bool> held{false}, go{false};
    // Runs once on the holding CPU while it owns the gate.
    void (*inside_hook)(Disk &){};
    Ext4FileSystem *fs{};
    Node *probe{};

  private:
    void enter() const
    {
        auto &self = const_cast<Disk &>(*this);
        if (self.inside.fetch_add(1) != 0) self.overlap = true;
        if (self.hold_cpu.load() == static_cast<u32>(smp::current_cpu()) + 1)
        {
            self.hold_cpu = 0;
            if (self.inside_hook) self.inside_hook(self);
            self.held = true;
            while (!self.go.load()) std::this_thread::yield();
        }
    }
    void leave() const { const_cast<Disk &>(*this).inside.fetch_sub(1); }
};

Bytes load(const char *path)
{
    auto *file = std::fopen(path, "rb"); CHECK(file);
    CHECK(!std::fseek(file, 0, SEEK_END));
    Bytes input(static_cast<usize>(std::ftell(file)));
    CHECK(!std::fseek(file, 0, SEEK_SET));
    CHECK(std::fread(input.data(), 1, input.size(), file) == input.size());
    std::fclose(file);
    return input;
}

Node *child(Node &root, std::string_view name)
{
    auto found = root.lookup_child_retained(name);
    CHECK(found && found.value());
    return found.value();
}

void wait_for(const std::atomic<bool> &flag)
{
    while (!flag.load()) std::this_thread::yield();
}

bool gate_open(const Ext4FileSystem &fs)
{
    return !fs.operation_active_ && fs.operation_owner_.load() == 0;
}

// Two CPUs write, read back and fsync their own files and a shared one; every
// call is served (none is refused as busy) and no two reach the disk at once.
void concurrent_io(const Bytes &image)
{
    Disk disk(image);
    auto fs = std::make_unique<Ext4FileSystem>();
    CHECK(fs->mount(disk)); auto *root = fs->root(); CHECK(root);
    for (auto name : {"one", "two", "shared"}) CHECK(root->create(name, NodeType::regular));
    std::array<Node *, 2> own{child(*root, "one"), child(*root, "two")};
    auto *shared = child(*root, "shared");
    CHECK(shared->write(0, std::span<const std::byte>(Bytes(512, std::byte{0}))));
    constexpr int rounds = 200;
    std::atomic<int> ready{0};
    std::vector<std::thread> cpus;
    for (int index = 0; index < 2; ++index)
        cpus.emplace_back([&, index] {
            const smp::ScopedHostedCpu cpu(static_cast<smp::CpuId>(1 + index));
            ready.fetch_add(1);
            while (ready.load() != 2) std::this_thread::yield();
            for (int round = 0; round < rounds; ++round)
            {
                Bytes data(1500 + round % 7, static_cast<std::byte>(index * 64 + round % 64));
                CHECK(own[index]->write(static_cast<usize>(round % 5) * 700, data));
                auto read = own[index]->read(static_cast<usize>(round % 5) * 700, data.size());
                CHECK(read && read.value().size == data.size());
                CHECK(!std::memcmp(read.value().data.data(), data.data(), data.size()));
                // Each CPU owns half of the shared file's first 512 bytes.
                const Bytes half(256, static_cast<std::byte>(0x10 + index));
                CHECK(shared->write(static_cast<usize>(index) * 256, half));
                auto mine = shared->read(static_cast<usize>(index) * 256, 256);
                CHECK(mine && mine.value().size == 256 && !std::memcmp(mine.value().data.data(), half.data(), 256));
                if (round % 10 == 0) CHECK(own[index]->sync());
                CHECK(own[index]->query_metadata());
            }
            CHECK(smp::cpu_mode(smp::current_cpu()) == 0 && smp::lock_debug_depth(smp::current_cpu()) == 0);
        });
    for (auto &cpu : cpus) cpu.join();
    CHECK(!disk.overlap.load() && disk.inside.load() == 0);
    CHECK(disk.flushes.load() >= 2 * rounds / 10);
    auto whole = shared->read(0, 512);
    CHECK(whole && whole.value().size == 512);
    for (usize i = 0; i < 512; ++i) CHECK(whole.value().data[i] == static_cast<std::byte>(0x10 + i / 256));
    CHECK(gate_open(*fs));
    for (auto *node : {own[0], own[1], shared}) CHECK(node->release());
}

// CPU 3 owns the gate inside a device callback. A reentrant call there gets
// busy; CPU 4's call waits marked SPINNING and is served once CPU 3 finishes.
void owner_and_waiter(const Bytes &image)
{
    Disk disk(image);
    auto fs = std::make_unique<Ext4FileSystem>();
    CHECK(fs->mount(disk)); auto *root = fs->root(); CHECK(root);
    CHECK(root->create("file", NodeType::regular));
    auto *file = child(*root, "file");
    disk.fs = fs.get(); disk.probe = root;
    static std::atomic<bool> reentry_busy{false};
    static std::atomic<usize> owner_depth{~usize{0}};
    disk.inside_hook = [](Disk &self) {
        CHECK(self.fs->operation_active_ && self.fs->operation_owner_.load() == 3 + 1);
        const auto nested = self.probe->query_metadata();
        reentry_busy = !nested && nested.status().code() == StatusCode::busy;
        owner_depth = smp::lock_debug_depth(smp::current_cpu());
    };
    disk.hold_cpu = 3 + 1;
    std::thread owner([&] {
        const smp::ScopedHostedCpu cpu(3);
        CHECK(file->write(0, std::span<const std::byte>(Bytes(100, std::byte{7}))));
    });
    wait_for(disk.held);
    CHECK(reentry_busy.load());
    CHECK(owner_depth.load() == (smp::lock_debug_enabled ? 1u : 0u));
    std::atomic<bool> served{false};
    std::thread waiter([&] {
        const smp::ScopedHostedCpu cpu(4);
        auto metadata = file->query_metadata();
        CHECK(metadata && metadata.value().size == 100);
        served = true;
    });
    while ((smp::cpu_mode(4) & smp::cpu_mode_spinning) == 0) std::this_thread::yield();
    CHECK(!served.load() && fs->operation_owner_.load() == 3 + 1);
    disk.go = true;
    owner.join();
    waiter.join();
    CHECK(served.load() && smp::cpu_mode(3) == 0 && smp::cpu_mode(4) == 0);
    CHECK(smp::lock_debug_depth(3) == 0 && smp::lock_debug_depth(4) == 0);
    CHECK(gate_open(*fs) && !disk.overlap.load());
    CHECK(file->release());
}

// A detached node's last release on CPU 6 while CPU 5 owns the gate only
// drops the reference; CPU 5's finish destroys the inode, with no busy result.
void deferred_release(const Bytes &image)
{
    Disk disk(image);
    auto fs = std::make_unique<Ext4FileSystem>();
    CHECK(fs->mount(disk)); auto *root = fs->root(); CHECK(root);
    CHECK(root->create("victim", NodeType::regular));
    CHECK(root->create("other", NodeType::regular));
    auto *victim = static_cast<Ext4Node *>(child(*root, "victim"));
    auto *other = child(*root, "other");
    CHECK(root->remove("victim", false));
    CHECK(victim->phase_ == Ext4Node::Phase::detached && victim->references_ == 1);
    disk.inside_hook = nullptr;
    disk.hold_cpu = 5 + 1;
    std::thread owner([&] {
        const smp::ScopedHostedCpu cpu(5);
        CHECK(other->write(0, std::span<const std::byte>(Bytes(64, std::byte{1}))));
    });
    wait_for(disk.held);
    const auto writes = disk.writes.load();
    {
        const smp::ScopedHostedCpu cpu(6);
        CHECK(victim->release());
        CHECK(smp::lock_debug_depth(6) == 0);
    }
    // Deferred: no I/O and no retirement while CPU 5 still owns the gate.
    CHECK(disk.writes.load() == writes && victim->references_ == 0 &&
          victim->phase_ == Ext4Node::Phase::detached && fs->operation_owner_.load() == 5 + 1);
    disk.go = true;
    owner.join();
    CHECK(victim->phase_ == Ext4Node::Phase::vacant || victim->phase_ == Ext4Node::Phase::tombstone);
    CHECK(disk.writes.load() > writes && gate_open(*fs) && !fs->failed_);
    CHECK(other->release());
}

// The inline cleanup of a release with the gate free is an operation of its
// own: owner word set during it, hold recorded, and both cleared after.
void inline_release(const Bytes &image)
{
    Disk disk(image);
    auto fs = std::make_unique<Ext4FileSystem>();
    CHECK(fs->mount(disk)); auto *root = fs->root(); CHECK(root);
    CHECK(root->create("victim", NodeType::regular));
    auto *victim = static_cast<Ext4Node *>(child(*root, "victim"));
    CHECK(root->remove("victim", false));
    disk.fs = fs.get(); disk.probe = root;
    static std::atomic<bool> owned{false};
    disk.inside_hook = [](Disk &self) {
        owned = self.fs->operation_owner_.load() == 2 + 1 &&
                smp::lock_debug_depth(2) == (smp::lock_debug_enabled ? 1u : 0u);
        self.go = true;
    };
    disk.go = false;
    disk.hold_cpu = 2 + 1;
    {
        const smp::ScopedHostedCpu cpu(2);
        CHECK(victim->release());
        CHECK(smp::lock_debug_depth(2) == 0);
    }
    CHECK(owned.load() && gate_open(*fs));
    CHECK(victim->phase_ == Ext4Node::Phase::vacant || victim->phase_ == Ext4Node::Phase::tombstone);
}

// The hold is recorded at fs_provider: below the description position lock
// it is in order, above the scheduler rank it is an order violation.
void recorded_rank(const Bytes &image)
{
    if constexpr (!smp::lock_debug_enabled) return;
    Disk disk(image);
    auto fs = std::make_unique<Ext4FileSystem>();
    CHECK(fs->mount(disk)); auto *root = fs->root(); CHECK(root);
    const smp::ScopedHostedCpu cpu(7);
    const auto before = smp::lock_violation_count(7);
    {
        smp::RankedSpinLock<smp::LockRank::ofd_position> position;
        position.lock();
        CHECK(root->query_metadata());
        position.unlock();
    }
    CHECK(smp::lock_violation_count(7) == before);
    {
        smp::RankedSpinLock<smp::LockRank::scheduler> higher;
        higher.lock();
        CHECK(root->query_metadata());
        higher.unlock();
    }
    CHECK(smp::lock_violation_count(7) == before + 1);
    CHECK(smp::lock_debug_depth(7) == 0 && gate_open(*fs));
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    testing::ext4_clock::reset();
    const auto image = load(argv[1]);
    concurrent_io(image);
    owner_and_waiter(image);
    deferred_release(image);
    inline_release(image);
    recorded_rank(image);
    CHECK(testing::ext4_clock::calls() == 0);
    std::printf("PASS ext4 gate concurrency (model, hosted CPUs, lock debug %s): concurrent read/write/fsync, "
                "waiter SPINNING, same-CPU busy, deferred and inline release, fs_provider rank\n",
                smp::lock_debug_enabled ? "on" : "off");
}
