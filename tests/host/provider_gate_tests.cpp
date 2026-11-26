// Model test of the simplefs provider gate across CPUs (smp-scaling WP5,
// spec 2 rank fs_provider): host threads stand in for CPUs through
// ScopedHostedCpu over an in-memory disk; no guest CPU, interrupt or device
// transport executes. Covers two CPUs writing and reading their own files at
// once (served, never refused; the disk never sees two callers), a re-entry
// on the owning CPU from inside a device callback (busy), a waiter on another
// CPU marked SPINNING until the owner finishes, and (with OK_LOCK_DEBUG) the
// hold recorded at rank fs_provider. The image argument is unused (the
// runner passes its ext4 images to every test).
#include "ok/fs/vfs.hpp"
#include "ok/smp/lock.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// The gate is private; this hosted mechanism test inspects it directly.
#define private public
#include "ok/fs/simplefs.hpp"
#undef private

using namespace ok;
using namespace ok::fs;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "provider gate:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

namespace
{
// Records overlapping callers instead of serializing them; can hold a chosen
// CPU inside its next callback (while it owns the gate) until released.
class Disk final : public driver::BlockDevice
{
  public:
    Disk() : bytes(512 * 256) {}
    driver::BlockGeometry geometry() const override { return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> output) override
    {
        enter();
        std::memcpy(output.data(), bytes.data() + first * 512, output.size());
        leave();
        return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> input) override
    {
        enter();
        std::memcpy(bytes.data() + first * 512, input.data(), input.size());
        writes.fetch_add(1);
        leave();
        return {};
    }
    Status flush() override { return {}; }

    std::vector<std::byte> bytes;
    std::atomic<int> inside{0};
    std::atomic<bool> overlap{false};
    std::atomic<usize> writes{0};
    std::atomic<u32> hold_cpu{0};
    std::atomic<bool> held{false}, go{false};
    SimpleDiskFileSystem *fs{};
    std::atomic<bool> reentry_busy{false};
    std::atomic<usize> owner_depth{~usize{0}};

  private:
    void enter()
    {
        if (inside.fetch_add(1) != 0) overlap = true;
        if (hold_cpu.load() == static_cast<u32>(smp::current_cpu()) + 1)
        {
            hold_cpu = 0;
            const auto nested = fs->info();
            reentry_busy = !nested && nested.status().code() == StatusCode::busy;
            owner_depth = smp::lock_debug_depth(smp::current_cpu());
            held = true;
            while (!go.load()) std::this_thread::yield();
        }
    }
    void leave() { inside.fetch_sub(1); }
};

std::vector<std::byte> pattern(u8 seed, usize size)
{
    std::vector<std::byte> out(size);
    for (usize index = 0; index < size; ++index) out[index] = static_cast<std::byte>(seed + index);
    return out;
}

void concurrent_io()
{
    Disk disk;
    auto fs = std::make_unique<SimpleDiskFileSystem>();
    disk.fs = fs.get();
    CHECK(fs->format(disk, "gate"));
    CHECK(fs->mount(disk));
    CHECK(fs->create("one", NodeType::regular) && fs->create("two", NodeType::regular));
    std::atomic<usize> served{0};
    const auto worker = [&](smp::CpuId cpu, const char *name, u8 seed) {
        const smp::ScopedHostedCpu hosted(cpu);
        for (usize round = 0; round < 400; ++round)
        {
            const auto data = pattern(static_cast<u8>(seed + round), 700 + round % 300);
            CHECK(fs->write_file(name, data));
            auto read = fs->read_file(name);
            CHECK(read && read.value().size == data.size());
            CHECK(std::memcmp(read.value().data.data(), data.data(), data.size()) == 0);
            served.fetch_add(1);
        }
        CHECK(smp::cpu_mode(cpu) == 0 && smp::lock_debug_depth(cpu) == 0);
    };
    std::thread first(worker, 1, "one", 3), second(worker, 2, "two", 101);
    first.join();
    second.join();
    CHECK(served.load() == 800 && !disk.overlap.load() && !fs->gate_.held());
}

// CPU 3 owns the gate inside a device callback: its own nested call is busy;
// CPU 4's call waits SPINNING and is served once CPU 3 lets go.
void owner_and_waiter()
{
    Disk disk;
    auto fs = std::make_unique<SimpleDiskFileSystem>();
    disk.fs = fs.get();
    CHECK(fs->format(disk, "gate"));
    CHECK(fs->mount(disk));
    CHECK(fs->create("file", NodeType::regular));
    disk.hold_cpu = 3 + 1;
    std::thread owner([&] {
        const smp::ScopedHostedCpu cpu(3);
        CHECK(fs->write_file("file", pattern(9, 100)));
    });
    while (!disk.held.load()) std::this_thread::yield();
    CHECK(disk.reentry_busy.load() && fs->gate_.owner() == 3 + 1);
    CHECK(disk.owner_depth.load() == (smp::lock_debug_enabled ? 1u : 0u));
    std::atomic<bool> served{false};
    std::thread waiter([&] {
        const smp::ScopedHostedCpu cpu(4);
        auto metadata = fs->stat("file");
        CHECK(metadata && metadata.value().size == 100);
        served = true;
    });
    while ((smp::cpu_mode(4) & smp::cpu_mode_spinning) == 0) std::this_thread::yield();
    CHECK(!served.load() && fs->gate_.owner() == 3 + 1);
    disk.go = true;
    owner.join();
    waiter.join();
    CHECK(served.load() && smp::cpu_mode(3) == 0 && smp::cpu_mode(4) == 0);
    CHECK(smp::lock_debug_depth(3) == 0 && smp::lock_debug_depth(4) == 0);
    CHECK(!fs->gate_.held() && !disk.overlap.load());
}

// Recorded at fs_provider: in order after the description position lock,
// an order violation after the mm gate's rank.
void recorded_rank()
{
    if constexpr (!smp::lock_debug_enabled) return;
    Disk disk;
    auto fs = std::make_unique<SimpleDiskFileSystem>();
    disk.fs = fs.get();
    CHECK(fs->format(disk, "gate"));
    CHECK(fs->mount(disk));
    const smp::ScopedHostedCpu cpu(7);
    const auto before = smp::lock_violation_count(7);
    {
        smp::RankedSpinLock<smp::LockRank::ofd_position> position;
        position.lock();
        CHECK(fs->info());
        position.unlock();
    }
    CHECK(smp::lock_violation_count(7) == before);
    {
        smp::RankedSpinLock<smp::LockRank::mm> higher;
        higher.lock();
        CHECK(fs->info());
        higher.unlock();
    }
    CHECK(smp::lock_violation_count(7) == before + 1);
    CHECK(smp::lock_debug_depth(7) == 0 && !fs->gate_.held());
}
} // namespace

int main(int argc, char **)
{
    CHECK(argc == 2);
    concurrent_io();
    owner_and_waiter();
    recorded_rank();
    std::printf("OK_PROVIDER_GATE scope=model provider=simplefs lock_debug=%s: concurrent I/O, re-entry busy, "
                "waiter SPINNING, fs_provider rank\n", smp::lock_debug_enabled ? "on" : "off");
}
