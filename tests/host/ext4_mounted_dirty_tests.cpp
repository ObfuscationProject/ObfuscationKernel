#include "ext4_mounted_support.hpp"
#include "ext4_mounted_retention_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

using namespace owned_fixture;
namespace clock_model = ok::testing::ext4_clock;
using Mount = ok::fs::Ext4MountedMetadata<1>;

static void require(bool condition, const char *what)
{
    if (!condition) { std::fprintf(stderr, "FAIL mounted dirty: %s\n", what); std::exit(1); }
}
static void require_status(ok::Status status, const char *what)
{
    if (!status)
    {
        std::fprintf(stderr, "FAIL mounted dirty: %s code=%u message=%.*s\n", what,
                     unsigned(status.code()), int(status.message().size()), status.message().data());
        std::exit(1);
    }
}
struct Mounted
{
    Disk disk;
    Mount::Registry registry;
    Mount mount{ok::fs::Ext4MountedPolicy::ordered_data_dirty_noatime};
    Mount::Registry::Registration key{};
    ok::fs::VirtualFileSystem vfs{};
    ok::fs::VfsMountHandle attachment{};
    std::optional<ok::fs::PathReference> seed_reference{};
    explicit Mounted(const Bytes &bytes) : disk(bytes)
    {
        auto registered = registry.register_device(disk, 17);
        require(bool(registered), "register device");
        key = registered.value();
        require_status(mount.mount(registry, key, {0, bytes.size() / 512}), "mount");
        auto published = publish_owned_root(mount, vfs);
        require(bool(published), "owned root publication"); attachment = published.value();
    }
    ok::fs::Node *seed()
    {
        if (!seed_reference)
        {
            auto found = vfs.resolve({}, "/seed"); require(bool(found), "seed lookup");
            seed_reference.emplace(std::move(found).value());
        }
        return seed_reference->node();
    }
    void drain()
    {
        if (seed_reference)
        { require_status(seed_reference->release(), "release seed path"); seed_reference.reset(); }
        require_status(detach_and_drain(mount, vfs, attachment), "detach and drain");
    }
    ok::Status finish()
    {
        drain(); return mount.freeze_clean();
    }
};
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    require(bool(input), "input");
    const Bytes original((std::istreambuf_iterator<char>(input)), {});
    clock_model::reset();
    clock_model::set({{1700000077, 123456789}, {200, 1}, 1,
                      ok::time::RealtimeOrigin::administrator_software});
    auto first = std::make_unique<Mounted>(original);
    const auto journal_first = journal(original) / 1024;
    require(r32(first->disk.durable, 1024 + 96) == 0x46, "mount recovery bit durable");
    require(be32(first->disk.durable, journal_first * 1024 + 28) == 0, "mount empty journal");
    auto *seed = first->seed();
    const std::array<std::byte, 4> replacement{std::byte{'C'}, std::byte{'O'}, std::byte{'D'}, std::byte{'E'}};
    const auto trace_begin = first->disk.trace.size();
    require_status(seed->write(100, replacement), "single-action write");
    const auto trace_end = first->disk.trace.size();
    std::vector<char> calls;
    for (ok::usize i = trace_begin; i < trace_end; ++i)
        if (first->disk.trace[i].operation == 'w' || first->disk.trace[i].operation == 'f')
            calls.push_back(first->disk.trace[i].operation);
    require(calls.size() == 16, "one-action write and FLUSH call count");
    require_status(seed->sync(), "file sync");
    require(first->mount.report().epoch == 2, "write epoch");
    require(r32(first->disk.durable, 1024 + 96) == 0x46, "write keeps dirty bit");
    require(be32(first->disk.durable, journal_first * 1024 + 28) == 0, "checkpoint empties journal");
    auto changed = seed->read(100, 4);
    require(bool(changed) && changed.value().size == 4, "read changed data");
    for (ok::usize i = 0; i < 4; ++i) require(changed.value().data[i] == replacement[i], "changed bytes");
    const auto inode_at = inode_offset(original, 12);
    const auto original_version = r32(original, inode_at + 36);
    const auto completed_version = r32(first->disk.durable, inode_at + 36);
    require(completed_version == original_version + 1, "one inode version increment");
    for (ok::usize prefix = 0; prefix < calls.size(); ++prefix)
    {
        auto interrupted = std::make_unique<Mounted>(original);
        auto *target = interrupted->seed();
        ok::usize writes = 0, flushes = 0;
        for (ok::usize i = 0; i < prefix; ++i)
            calls[i] == 'w' ? ++writes : ++flushes;
        if (calls[prefix] == 'w') interrupted->disk.fail_write = interrupted->disk.writes + writes + 1;
        else interrupted->disk.fail_flush = interrupted->disk.flushes + flushes + 1;
        require(!target->write(100, replacement), "interrupted transaction fails");
        require(interrupted->mount.report().phase == ok::fs::Ext4MountPhase::failed,
                "uncertain transaction quarantines mount");
        interrupted->disk.crash();
        const auto after_crash = interrupted->disk.live;
        const bool committed = be32(after_crash, journal_first * 1024 + 28) != 0 ||
                               r32(after_crash, inode_at + 36) == completed_version;
        auto resumed = std::make_unique<Mounted>(after_crash);
        auto *resumed_seed = resumed->seed();
        auto metadata = resumed_seed->query_metadata();
        require(bool(metadata) && metadata.value().size == 4096, "recovered inode size");
        const auto recovered_version = r32(resumed->disk.durable, inode_at + 36);
        require(recovered_version == (committed ? completed_version : original_version),
                "completed-call cut recovers selected inode version");
        require_status(resumed->finish(), "recovered clean freeze");
        require(r32(resumed->disk.durable, 1024 + 96) == 0x42, "recovered clean superblock");
    }
    {
        std::ofstream output(argv[2], std::ios::binary);
        output.write(reinterpret_cast<const char *>(first->disk.durable.data()),
                     static_cast<std::streamsize>(first->disk.durable.size()));
        require(bool(output), "dirty output");
    }
    auto second = std::make_unique<Mounted>(first->disk.durable);
    require(second->disk.writes == 0, "dirty-empty remount has no writes");
    seed = second->seed();
    changed = seed->read(100, 4);
    require(bool(changed) && changed.value().size == 4, "remount data");
    for (ok::usize i = 0; i < 4; ++i) require(changed.value().data[i] == replacement[i], "remount bytes");
    require_status(seed->retain(), "retain node before freeze");
    second->drain();
    require(second->mount.freeze_clean().code() == ok::StatusCode::busy, "retained node blocks freeze");
    require_status(seed->release(), "release node before freeze");
    require_status(second->mount.freeze_clean(), "clean freeze");
    require(second->mount.report().phase == ok::fs::Ext4MountPhase::clean_sealed, "sealed phase");
    require(r32(second->disk.durable, 1024 + 96) == 0x42, "freeze clears recovery bit");
    require(be32(second->disk.durable, journal_first * 1024 + 28) == 0, "freeze empty journal");
    require(!seed->read(0, 1), "sealed node rejects read");
    {
        auto failure = std::make_unique<Mounted>(first->disk.durable);
        const auto flushes = failure->disk.flushes;
        failure->disk.fail_flush = flushes + 3;
        require(!failure->finish(), "final freeze FLUSH fails");
        require(failure->mount.report().phase == ok::fs::Ext4MountPhase::failed,
                "failed freeze quarantines mount");
        failure->disk.crash();
        require(r32(failure->disk.durable, 1024 + 96) == 0x46,
                "failed final FLUSH leaves durable dirty image");
        auto recovery = std::make_unique<Mounted>(failure->disk.durable);
        require_status(recovery->finish(), "freeze retry through new lease");
        require(r32(recovery->disk.durable, 1024 + 96) == 0x42,
                "retry seals clean image");
    }
    {
        auto corrupted = first->disk.durable;
        corrupted[journal_first * 1024 + 252] ^= 1;
        Disk disk(corrupted);
        Mount::Registry registry;
        auto key = registry.register_device(disk, 19);
        require(bool(key), "register corrupted disk");
        auto mount = std::make_unique<Mount>(ok::fs::Ext4MountedPolicy::ordered_data_dirty_noatime);
        require(!mount->mount(registry, key.value(), {0, corrupted.size() / 512}),
                "corrupted journal control rejected");
        require(disk.writes == 0, "corrupted journal control has no writes");
    }
    {
        std::ofstream output(argv[3], std::ios::binary);
        output.write(reinterpret_cast<const char *>(second->disk.durable.data()),
                     static_cast<std::streamsize>(second->disk.durable.size()));
        require(bool(output), "clean output");
    }
    std::printf("PASS mounted dirty writes=%zu flushes=%zu cuts=%zu freeze_writes=%zu freeze_flushes=%zu\n",
                first->disk.writes, first->disk.flushes, calls.size(), second->disk.writes, second->disk.flushes);
}
