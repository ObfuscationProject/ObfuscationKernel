// The same real ext4 callback must reject the former VFS lock-across-I/O design.
// The ext4 operation gate refuses reentry only from its owning CPU: inside the
// callback a nested VFS or direct ext4 call returns busy, while another hosted
// CPU's direct ext4 call waits (marked SPINNING) and is served afterwards.
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

// The VFS and gate state are private; this hosted control inspects them.
#define private public
#include "ok/fs/ext4/layout.hpp"
#undef private

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
    Node *root{};
    std::atomic<bool> active{false};
    usize reentrant_busy{};
    usize direct_busy{};
    // Another CPU's direct ext4 call, started from the first callback.
    std::optional<std::thread> other_cpu{};
    std::atomic<bool> other_served{false};
    bool other_was_waiting{};
    Bytes bytes;
  private:
    void probe() const
    {
        if (!active) return;
        auto &self = const_cast<Disk &>(*this);
        // Only the callback's own CPU (0) probes; the other CPU runs after it.
        if (smp::current_cpu() != 0) return;
        CHECK(vfs && vfs->lock_.try_lock());
        vfs->lock_.unlock();
        const auto nested = vfs->stat("/victim");
        CHECK(!nested && nested.status().code() == StatusCode::busy);
        ++self.reentrant_busy;
        const auto direct = root->query_metadata();
        CHECK(!direct && direct.status().code() == StatusCode::busy);
        ++self.direct_busy;
        if (self.other_cpu) return;
        self.other_cpu.emplace([this] {
            const smp::ScopedHostedCpu cpu(1);
            auto &owner = const_cast<Disk &>(*this);
            const auto metadata = owner.root->query_metadata();
            CHECK(metadata && metadata.value().type == NodeType::directory);
            owner.other_served = true;
        });
        while ((smp::cpu_mode(1) & smp::cpu_mode_spinning) == 0 && !self.other_served.load())
            std::this_thread::yield();
        self.other_was_waiting = !self.other_served.load();
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
    disk.vfs = vfs.get(); disk.root = root; disk.active = true;
    CHECK(vfs->stat("/victim"));
    disk.active = false;
    CHECK(disk.reentrant_busy >= 1 && disk.direct_busy == disk.reentrant_busy);
    CHECK(disk.other_cpu); disk.other_cpu->join();
    CHECK(disk.other_was_waiting && disk.other_served.load());
    CHECK(!storage->operation_active_ && storage->operation_owner_.load() == 0);
    CHECK(smp::cpu_mode(1) == 0);
    CHECK(testing::ext4_clock::calls() == 0);
    std::puts("PASS actual ext4 callback lock control: provider I/O unlocked, reentrant VFS and ext4 calls "
              "busy on the owning CPU, another CPU waits and is served");
}
