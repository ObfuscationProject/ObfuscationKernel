// Actual MountedNode/OwnedTransaction composition on a retained ext4 fixture.
// Transport faults remain hosted models; this is not native Files execution.
#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <memory>

using namespace owned_fixture;
namespace
{
usize checks{}, cold_cases{}, refusal_cases{}, in_flight_checks{}, terminal_lease_checks{}, terminal_refusals{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "mounted regular move:%d: %s\n", __LINE__, #expression); std::abort(); \
} } while (false)
using Mount = Ext4MountedMetadata<1>;
struct Mounted final
{
    Disk &disk;
    Mount::Registry registry{};
    std::unique_ptr<Mount::CommonStorage> storage{std::make_unique<Mount::CommonStorage>()};
    std::unique_ptr<Mount> mount{std::make_unique<Mount>(*storage)};
    explicit Mounted(Disk &value) : disk(value) {}
    Status open()
    {
        auto registered = registry.register_device(disk, 277);
        if (!registered) return registered.status();
        return mount->mount(registry, registered.value(), {0, disk.live.size() / 512});
    }
};
void clock()
{
    ok::testing::ext4_clock::set({{1700000777, 246813579}, {777, 1}, 1,
        time::RealtimeOrigin::administrator_software});
}
void payload(Node &file, const Bytes &original)
{
    auto metadata = file.query_metadata(); CHECK(metadata);
    CHECK(metadata.value().attributes.inode == 12 && metadata.value().size == 4096 &&
        metadata.value().blocks == 8 && metadata.value().link_count == 1);
    const usize first = r32(original, inode_offset(original, 12) + 60) * 1024;
    for (usize at = 0; at < 4096; at += 256)
    {
        auto bytes = file.read(at, 256); CHECK(bytes && bytes.value().size == 256);
        CHECK(std::equal(bytes.value().data.begin(), bytes.value().data.begin() + 256,
            reinterpret_cast<const std::byte *>(original.data() + first + at)));
    }
}
void no_io_holds(Mount &mount)
{
    for (u32 inode : {u32{2}, u32{12}, u32{14}})
    {
        auto state = mount.lifetime_snapshot(inode); CHECK(state);
        CHECK(state.value().holds[static_cast<usize>(Ext4InodeHold::io)] == 0 &&
            state.value().holds[static_cast<usize>(Ext4InodeHold::pin)] == 0);
    }
}
usize healthy(const Bytes &original, const char *completed_output, const char *renamed_output)
{
    Disk disk(original); Mounted mounted(disk); CHECK(mounted.open()); clock();
    auto *root = mounted.mount->root(); CHECK(root);
    auto directory = root->lookup_child_retained("archived-documents"); CHECK(directory);
    auto file = root->lookup_child_retained("seed"); CHECK(file);
    auto old_file_name = file.value()->name(); auto old_directory_name = directory.value()->name();
    const auto before = file.value()->query_metadata(); CHECK(before);
    const auto epoch = mounted.mount->report().epoch;
    no_io_holds(*mounted.mount);
    const auto writes = disk.writes, flushes = disk.flushes;
    CHECK(!root->rename("seed", *directory.value(), "note.txt"));
    CHECK(!root->rename("seed", *directory.value(), "../invalid"));
    CHECK(disk.writes == writes && disk.flushes == flushes && mounted.mount->report().epoch == epoch);
    ++refusal_cases;
    bool observed{}, released{}, last_read_old{};
    std::array<usize, 3> last_read_holds{};
    usize last_read{};
    disk.hook = [&](char operation) {
        if (operation == 'r')
        {
            last_read = disk.reads + 1;
            last_read_old = old_file_name == "seed" && file.value()->name() == "seed" &&
                mounted.mount->report().epoch == epoch;
            usize index{};
            for (u32 inode : {u32{2}, u32{12}, u32{14}})
            {
                auto state = mounted.mount->lifetime_snapshot(inode); CHECK(state);
                last_read_holds[index++] = state.value().holds[static_cast<usize>(Ext4InodeHold::io)];
            }
        }
        if (observed || operation != 'w') return;
        observed = true;
        for (u32 inode : {u32{2}, u32{12}, u32{14}})
        {
            auto state = mounted.mount->lifetime_snapshot(inode); CHECK(state);
            CHECK(state.value().holds[static_cast<usize>(Ext4InodeHold::io)] == 1);
            ++in_flight_checks;
        }
        CHECK(!mounted.mount->begin_drain());
        CHECK(!root->lookup_child_retained("seed"));
        CHECK(!root->rename("seed", *directory.value(), "second.txt"));
        CHECK(file.value()->release()); released = true;
    };
    CHECK(root->rename("seed", *directory.value(), "moved.txt")); disk.hook = {};
    CHECK(observed && released && mounted.mount->report().epoch == epoch + 1);
    CHECK(last_read == disk.reads && last_read_old);
    for (auto held : last_read_holds) { CHECK(held == 1); ++terminal_lease_checks; }
    const auto terminal_read = disk.reads;
    CHECK(old_file_name == "seed" && file.value()->name() == "moved.txt");
    CHECK(root->lookup_child_retained("seed").status().code() == StatusCode::not_found);
    auto moved = directory.value()->lookup_child_retained("moved.txt"); CHECK(moved);
    CHECK(moved.value() == file.value());
    payload(*moved.value(), original); no_io_holds(*mounted.mount);
    CHECK(root->sync()); save(completed_output, disk.durable);
    CHECK(root->rename("archived-documents", *root, "files-archive"));
    CHECK(old_directory_name == "archived-documents" && directory.value()->name() == "files-archive");
    CHECK(directory.value()->directory_path(*root).value().view() == "/files-archive");
    CHECK(moved.value()->name() == "moved.txt" && old_file_name == "seed");
    payload(*moved.value(), original);
    CHECK(moved.value()->release() && directory.value()->release());
    no_io_holds(*mounted.mount); CHECK(root->sync()); save(renamed_output, disk.durable);
    return terminal_read;
}
void terminal_verification_refusal(const Bytes &original, usize terminal_read, bool generation)
{
    Disk disk(original); Mounted mounted(disk); CHECK(mounted.open()); clock();
    auto *root = mounted.mount->root(); CHECK(root);
    auto directory = root->lookup_child_retained("archived-documents"); CHECK(directory);
    auto file = root->lookup_child_retained("seed"); CHECK(file);
    const auto old_file_name = file.value()->name(); const auto old_directory_name = directory.value()->name();
    CHECK(file.value()->query_metadata());
    const auto epoch = mounted.mount->report().epoch;
    no_io_holds(*mounted.mount);
    CHECK(!root->rename("seed", *directory.value(), "note.txt"));
    CHECK(!root->rename("seed", *directory.value(), "../invalid"));
    bool injected{};
    disk.hook = [&](char operation) {
        if (operation != 'r' || disk.reads + 1 != terminal_read) return;
        injected = true;
        for (u32 inode : {u32{2}, u32{12}, u32{14}})
        {
            auto state = mounted.mount->lifetime_snapshot(inode); CHECK(state);
            CHECK(state.value().holds[static_cast<usize>(Ext4InodeHold::io)] == 1);
        }
        CHECK(file.value()->name() == "seed" && mounted.mount->report().epoch == epoch);
        if (!generation) disk.fail_read = terminal_read;
        else
        {
            const auto at = inode_offset(disk.live, 12);
            w32(disk.live, at + 100, r32(disk.live, at + 100) + 1);
            inode_crc(disk.live, 12, at);
        }
    };
    CHECK(!root->rename("seed", *directory.value(), "moved.txt")); disk.hook = {};
    CHECK(injected && disk.reads == terminal_read);
    CHECK(mounted.mount->report().phase == Ext4MountPhase::failed && !mounted.mount->root());
    CHECK(mounted.mount->report().epoch == epoch && old_file_name == "seed" && file.value()->name() == "seed");
    CHECK(old_directory_name == "archived-documents" && directory.value()->name() == "archived-documents");
    CHECK(!root->sync()); no_io_holds(*mounted.mount);
    CHECK(file.value()->release() && directory.value()->release());
    ++terminal_refusals;
}
Bytes mounted_publication_loss(const Bytes &original)
{
    Disk disk(original);
    {
        Mounted mounted(disk); CHECK(mounted.open()); clock();
        auto *root = mounted.mount->root(); CHECK(root);
        auto directory = root->lookup_child_retained("archived-documents"); CHECK(directory);
        // Publication FLUSH persisted the active head but reports failure.
        disk.fail_flush_after = disk.flushes + 4;
        CHECK(!root->rename("seed", *directory.value(), "moved.txt"));
        CHECK(mounted.mount->report().phase == Ext4MountPhase::failed && !mounted.mount->root());
        CHECK(!root->sync()); CHECK(directory.value()->release());
        CHECK(be32(disk.durable, journal(disk.durable) + 28) == 1);
        no_io_holds(*mounted.mount);
    }
    disk.crash(); return disk.durable;
}
void cold_completion(const Bytes &original, const Bytes &active, const char *output)
{
    Disk disk(active); Mounted mounted(disk); CHECK(mounted.open());
    CHECK(disk.writes > 0 && disk.flushes >= 3);
    const auto report = mounted.mount->startup_report();
    CHECK(report.phase == Ext4StartupPhase::complete && report.strict_ordinary_admission);
    CHECK(be32(disk.durable, journal(disk.durable) + 28) == 0 && (r32(disk.durable, 1120) & 4U));
    auto *root = mounted.mount->root(); CHECK(root);
    auto directory = root->lookup_child_retained("archived-documents"); CHECK(directory);
    CHECK(root->lookup_child_retained("seed").status().code() == StatusCode::not_found);
    auto moved = directory.value()->lookup_child_retained("moved.txt"); CHECK(moved);
    payload(*moved.value(), original);
    CHECK(moved.value()->release() && directory.value()->release());
    CHECK(root->sync()); save(output, disk.durable); ++cold_cases;
}
void cold_foreign_refusal(const Bytes &active)
{
    auto bytes = active; const usize first = journal(bytes);
    bytes[first + 1024 + 128] ^= 1;
    std::array<u8, 16> uuid{}; std::copy_n(bytes.begin() + 1128, 16, uuid.begin());
    const u32 seed = jbd2::detail::crc32c(~u32{0}, uuid);
    big32(bytes, first + 1024 + 1020, 0);
    big32(bytes, first + 1024 + 1020, crc(seed, bytes, first + 1024, 1024));
    Disk disk(bytes); Mounted mounted(disk); CHECK(!mounted.open());
    CHECK(disk.writes == 0 && disk.flushes == 0 && disk.live == bytes && !mounted.mount->root());
    ++refusal_cases;
}
}
int main(int argc, char **argv)
{
    CHECK(argc == 5);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    const auto terminal_read = healthy(original, argv[2], argv[3]);
    terminal_verification_refusal(original, terminal_read, false);
    terminal_verification_refusal(original, terminal_read, true);
    const auto active = mounted_publication_loss(original);
    cold_completion(original, active, argv[4]); cold_foreign_refusal(active);
    std::printf("EXT4_REGULAR_MOVE_MOUNTED_MODEL_PASS checks=%zu cold_cases=%zu refusal_cases=%zu in_flight_checks=%zu terminal_lease_checks=%zu terminal_refusals=%zu\n",
        checks, cold_cases, refusal_cases, in_flight_checks, terminal_lease_checks, terminal_refusals);
}
