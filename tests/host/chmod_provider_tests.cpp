// Hosted provider model: no syscall, user process or hardware is executed.
#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4/layout.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ok;
using Bytes = std::vector<std::byte>;
namespace clock_model = testing::ext4_clock;
namespace
{
usize checks{};
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "provider check failed at %d\n", __LINE__); std::exit(1); } } while (false)
u16 u16at(const Bytes &b, usize at) { return std::to_integer<u16>(b.at(at)) | (std::to_integer<u16>(b.at(at + 1)) << 8); }
u32 u32at(const Bytes &b, usize at) { return u16at(b, at) | (u32{u16at(b, at + 2)} << 16); }
usize inode_at(const Bytes &b, u32 ino)
{
    const usize block = usize{1024} << u32at(b, 1048), per = u32at(b, 1064);
    const usize group = (ino - 1) / per, index = (ino - 1) % per;
    return u32at(b, (u32at(b, 1044) + 1) * block + group * 32 + 8) * block + index * u16at(b, 1112);
}
class Disk final : public driver::BlockDevice
{
  public:
    Bytes bytes;
    usize writes{};
    bool fail_write{}, partial{};
    usize failure_at{1};
    explicit Disk(const Bytes &data) : bytes(data) {}
    driver::BlockGeometry geometry() const override { return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        CHECK(sector <= bytes.size() / 512 && out.size() <= bytes.size() - sector * 512);
        std::memcpy(out.data(), bytes.data() + sector * 512, out.size());
        return Status::success();
    }
    Status write_blocks(u64 sector, std::span<const std::byte> input) override
    {
        CHECK(sector <= bytes.size() / 512 && input.size() <= bytes.size() - sector * 512);
        ++writes;
        const bool failure = fail_write && writes == failure_at;
        if (!failure || partial)
            std::memcpy(bytes.data() + sector * 512, input.data(), failure ? input.size() / 2 : input.size());
        return failure ? Status::io_error("injected mode-write failure") : Status::success();
    }
    Status flush() override { return Status::success(); }
};
time::RealtimeSample sample(i64 seconds)
{
    return {{seconds, 123456789}, {200, 1}, 1, time::RealtimeOrigin::administrator_software};
}
constexpr fs::Ext4MountOptions maintained{true, fs::Ext4TimestampPolicy::maintained};
user::Credentials owner(bool same_group = true)
{
    return {.uid = 71, .gid = 2000, .euid = 1000, .egid = same_group ? 2000u : 72u};
}
void unchanged_except_mode_ctime(const Bytes &before, const Bytes &after, u32 ino)
{
    const usize at = inode_at(before, ino), stride = u16at(before, 1112);
    auto expected = before;
    std::copy_n(after.begin() + at, 2, expected.begin() + at);
    std::copy_n(after.begin() + at + 12, 4, expected.begin() + at + 12);
    if (stride == 256)
        std::copy_n(after.begin() + at + 132, 4, expected.begin() + at + 132);
    CHECK(expected == after);
}
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    auto *input = std::fopen(argv[1], "rb"); CHECK(input);
    CHECK(std::fseek(input, 0, SEEK_END) == 0); const auto size = std::ftell(input); CHECK(size > 0);
    std::rewind(input); Bytes original(static_cast<usize>(size));
    CHECK(std::fread(original.data(), 1, original.size(), input) == original.size()); CHECK(std::fclose(input) == 0);
    Disk seed(original); fs::Ext4FileSystem initial;
    clock_model::reset(); clock_model::set(sample(1700000000)); CHECK(initial.mount(seed, maintained));
    auto file = initial.root()->create_child("mode-file", {fs::NodeType::regular, 0644, 1000, 2000}); CHECK(file);
    auto directory = initial.root()->create_child("mode-dir", {fs::NodeType::directory, 0700, 1000, 2000}); CHECK(directory);
    CHECK(initial.root()->create_symlink("mode-link", "mode-file", 1000, 2000)); CHECK(initial.sync());
    const Bytes prepared = seed.bytes;
    for (unsigned which = 0; which != 5; ++which)
    {
        Disk disk(prepared); fs::Ext4FileSystem filesystem; clock_model::set(sample(1700000077)); CHECK(filesystem.mount(disk, maintained));
        auto *node = filesystem.root()->lookup(which == 4 ? "mode-dir" : "mode-file"); CHECK(node);
        const auto before = node->query_metadata(); CHECK(before); const auto samples = clock_model::calls();
        auto caller = which == 2 ? user::root_credentials() : owner(which != 1);
        const u32 mode = which == 3 ? 0644 : (0100000u | 07777u);
        const auto result = node->change_mode(mode, caller); CHECK(result.ok()); CHECK(clock_model::calls() == samples + 1);
        auto after = node->query_metadata(); CHECK(after);
        const u32 expected = which == 1 ? 05777 : (which == 3 ? 0644 : 07777);
        CHECK((after.value().mode & 07777) == expected && after.value().type == before.value().type);
        CHECK(after.value().uid == 1000 && after.value().gid == 2000);
        CHECK(after.value().attributes.change.seconds == 1700000077);
        CHECK(after.value().attributes.access.seconds == before.value().attributes.access.seconds);
        CHECK(after.value().attributes.modification.seconds == before.value().attributes.modification.seconds);
        CHECK(u16at(disk.bytes, 1082) == 0);
        CHECK(filesystem.sync());
        CHECK(u16at(disk.bytes, 1082) == 1);
        unchanged_except_mode_ctime(prepared, disk.bytes, static_cast<u32>(after.value().attributes.inode));
    }
    for (unsigned which = 0; which != 6; ++which)
    {
        Disk disk(prepared); fs::Ext4FileSystem filesystem;
        const fs::Ext4MountOptions options{which != 1 && which != 2, (which >= 1 && which <= 3) ? fs::Ext4TimestampPolicy::unqualified : fs::Ext4TimestampPolicy::maintained};
        clock_model::set(sample(1700000088)); CHECK(filesystem.mount(disk, options));
        auto *node = filesystem.root()->lookup(which == 2 || which == 4 ? "mode-link" : "mode-file"); CHECK(node);
        auto caller = owner(); if (which == 0 || which == 1) caller.euid = 71;
        if (which == 5) clock_model::fail(Status::unsupported("no qualified clock"));
        const auto samples = clock_model::calls();
        auto result = node->change_mode(0600, caller);
        if (which == 0) CHECK(result.kind() == fs::ModeChangeResult::Kind::failed && result.status().code() == StatusCode::permission);
        else if (which == 1 || which == 2) CHECK(result.kind() == fs::ModeChangeResult::Kind::read_only);
        else if (which == 5) CHECK(result.kind() == fs::ModeChangeResult::Kind::failed && result.status().code() == StatusCode::io_error);
        else CHECK(result.kind() == fs::ModeChangeResult::Kind::unavailable);
        CHECK(disk.bytes == prepared && disk.writes == 0);
        CHECK(clock_model::calls() == samples + (which == 5 ? 1 : 0));
    }
    for (bool partial : {false, true})
    for (usize fail_at : {1u, 2u})
    {
        Disk disk(prepared); fs::Ext4FileSystem filesystem; clock_model::set(sample(1700000099)); CHECK(filesystem.mount(disk, maintained));
        auto *node = filesystem.root()->lookup("mode-file"); CHECK(node);
        disk.fail_write = true; disk.partial = partial; disk.failure_at = fail_at;
        auto result = node->change_mode(0700, owner());
        CHECK(result.kind() == fs::ModeChangeResult::Kind::failed && result.status().code() == StatusCode::io_error);
        CHECK(disk.writes > 0); const auto writes = disk.writes;
        disk.fail_write = false;
        result = node->change_mode(0755, owner());
        CHECK(!result.ok() && result.status().code() == StatusCode::io_error && disk.writes == writes);
    }
    std::printf("CHMOD_PROVIDER_MODEL checks=%zu success=5 refusal=6 write-faults=4 user-execution=0\n", checks);
}
