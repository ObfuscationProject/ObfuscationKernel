// Hosted unsupported-inode admission regression; no user instructions or syscall dispatcher.
#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4_fs.hpp"
#include "ok/abi/linux_chmod_call.hpp"
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
    const auto metadata = file.value()->query_metadata(); CHECK(metadata);
    const usize at = inode_at(prepared, static_cast<u32>(metadata.value().attributes.inode));
    Disk disk(prepared);
    disk.bytes.at(at + 108) = std::byte{1};
    const Bytes before = disk.bytes;
    fs::Ext4FileSystem filesystem; clock_model::set(sample(1700000077));
    CHECK(filesystem.mount(disk, maintained));
    auto *node = filesystem.root()->lookup("mode-file"); CHECK(node);
    const auto samples = clock_model::calls();
    const auto result = node->change_mode(0600, owner());
    using namespace abi::linux_user;
    const u64 arguments[]{3, 0600};
    const auto canonical = ChmodCallAdapter<Profile::x86_64>::execute(ChmodCall::fchmod, arguments,
        [](u64, FixedString<256> &) { return Status::fault("unexpected pathname read"); },
        [&](ChmodRequest request, std::string_view) { return node->change_mode(request.permissions, owner()); });
    CHECK(disk.bytes == before && disk.writes == 0 && clock_model::calls() == samples);
    std::printf("CHMOD_HIGH_SIZE_REPRO kind=%u status=%u canonical=%lld writes=%zu clock_samples=%zu\n",
                unsigned(result.kind()), unsigned(result.status().code()), static_cast<long long>(canonical),
                disk.writes, clock_model::calls() - samples);
    CHECK(result.kind() == fs::ModeChangeResult::Kind::unavailable);
    CHECK(canonical == -95);
    return 0;
}
