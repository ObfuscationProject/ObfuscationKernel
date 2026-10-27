#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>

using namespace owned_fixture;
namespace clock_model = ok::testing::ext4_clock;
using Mount = ok::fs::Ext4MountedMetadata<1>;
static ok::usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL ordered data %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

struct Mounted
{
    Disk disk;
    Mount::Registry registry;
    Mount mount{ok::fs::Ext4MountedPolicy::ordered_data_noatime};
    Mount::Registry::Registration key{};
    explicit Mounted(const Bytes &bytes) : disk(bytes)
    {
        auto registered = registry.register_device(disk, 17);
        CHECK(registered);
        key = registered.value();
        CHECK(mount.mount(registry, key, {0, bytes.size() / 512}));
    }
    ok::fs::Node *seed()
    {
        auto *root = mount.root(); CHECK(root);
        auto found = root->lookup_child("seed"); CHECK(found);
        return found.value();
    }
};

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    CHECK(input);
    Bytes original((std::istreambuf_iterator<char>(input)), {});
    if (argc == 4 && std::string_view(argv[3]) == "revoke") optional_revoke_fixture(original);
    const auto base_inode = inode_offset(original, 12);
    const auto target_inode_block = base_inode / 1024;
    const auto journal_first = journal(original) / 1024;
    clock_model::reset();
    clock_model::set({{1700000077, 123456789}, {200, 1}, 1,
                      ok::time::RealtimeOrigin::administrator_software});
    auto mounted = std::make_unique<Mounted>(original);
    auto *seed = mounted->seed();
    const auto old = seed->query_metadata(); CHECK(old && old.value().size == 4096);
    const auto writes_before = mounted->disk.writes;
    CHECK(seed->truncate(0).code() == ok::StatusCode::unsupported);
    CHECK(mounted->disk.writes == writes_before);
    const std::array<std::byte, 4> replacement{std::byte{'C'}, std::byte{'O'}, std::byte{'D'}, std::byte{'E'}};
    CHECK(seed->write(100, replacement));
    CHECK(mounted->mount.report().epoch == 2);
    auto observed = seed->read(96, 12); CHECK(observed && observed.value().size == 12);
    for (ok::usize index = 0; index < replacement.size(); ++index)
        CHECK(observed.value().data[4 + index] == replacement[index]);
    CHECK(seed->truncate(4000));
    auto shortened = seed->query_metadata(); CHECK(shortened && shortened.value().size == 4000);
    const std::array<std::byte, 5> suffix{std::byte{'A'}, std::byte{'P'}, std::byte{'P'}, std::byte{'E'}, std::byte{'N'}};
    CHECK(seed->write(4000, suffix));
    CHECK(seed->sync());
    auto grown = seed->query_metadata(); CHECK(grown && grown.value().size == 4005);
    observed = seed->read(3996, 9); CHECK(observed && observed.value().size == 9);
    for (ok::usize index = 0; index < suffix.size(); ++index)
        CHECK(observed.value().data[4 + index] == suffix[index]);
    CHECK(mounted->mount.report().epoch == 4 && mounted->disk.durable == mounted->disk.live);
    const auto completed = mounted->disk.durable;
    const auto first_data_block = 530U, data_block = 533U;
    for (ok::usize offset = 0; offset < original.size(); ++offset)
    {
        const auto block = offset / 1024;
        const bool allowed = block == 1 || block == target_inode_block || block == first_data_block ||
            block == data_block ||
            (block >= journal_first && block < journal_first + 6);
        if (!allowed && original[offset] != completed[offset])
        {
            std::fprintf(stderr, "unexpected changed block=%zu offset=%zu data=%u journal=%zu inode=%zu\n",
                         offset / 1024, offset, data_block, journal_first, target_inode_block);
            CHECK(false);
        }
    }
    {
        auto rebooted = std::make_unique<Mounted>(completed);
        auto *again = rebooted->seed();
        auto metadata = again->query_metadata(); CHECK(metadata && metadata.value().size == 4005);
        auto bytes = again->read(3996, 9); CHECK(bytes && bytes.value().size == 9);
        for (ok::usize index = 0; index < suffix.size(); ++index)
            CHECK(bytes.value().data[4 + index] == suffix[index]);
        CHECK(rebooted->disk.writes == 0);
    }
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char *>(completed.data()), static_cast<std::streamsize>(completed.size()));
    CHECK(output.good());
    std::printf("PASS ordered data hosted checks=%zu writes=%zu flushes=%zu epochs=%llu\n",
                checks, mounted->disk.writes, mounted->disk.flushes,
                static_cast<unsigned long long>(mounted->mount.report().epoch));
}
