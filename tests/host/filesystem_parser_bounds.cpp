#include "ok/fs/ext4/layout.hpp"
#include "ext4_realtime_model.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace ok;
using Bytes = std::vector<std::byte>;
static usize checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line=%d expression=%s\n", __LINE__, #x); std::exit(1); } } while (0)
static u16 get16(std::span<const std::byte> b, usize p)
{ return std::to_integer<u16>(b[p]) | (std::to_integer<u16>(b[p + 1]) << 8); }
static u32 get32(std::span<const std::byte> b, usize p)
{ return get16(b, p) | (u32{get16(b, p + 2)} << 16); }
static void put16(std::span<std::byte> b, usize p, u16 x)
{ b[p] = std::byte(x); b[p + 1] = std::byte(x >> 8); }
static void put32(std::span<std::byte> b, usize p, u32 x)
{ put16(b, p, static_cast<u16>(x)); put16(b, p + 2, static_cast<u16>(x >> 16)); }

class Device final : public driver::BlockDevice
{
  public:
    explicit Device(usize size) : bytes(size), sectors(size / 512) {}
    driver::BlockGeometry geometry() const override { return {sectors, sector_size, true}; }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads;
        if (fail_read) return Status::io_error("injected superblock read failure");
        if (first >= sectors || out.size() % 512 || out.size() / 512 > sectors - first)
            return Status::invalid_argument();
        std::fill(out.begin(), out.end(), std::byte{});
        if (first < bytes.size() / 512)
        {
            const usize at = static_cast<usize>(first) * 512;
            std::memcpy(out.data(), bytes.data() + at, std::min(out.size(), bytes.size() - at));
        }
        if (!replacement.empty() && first == replacement_sector && out.size() == replacement.size() &&
            matching_reads++ >= skip_reads)
        {
            std::memcpy(out.data(), replacement.data(), out.size());
            ++injections;
        }
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("regression device forbids writes"); }
    Status flush() override { return {}; }
    Bytes bytes, replacement;
    u64 sectors{}, replacement_sector{};
    u32 sector_size{512};
    usize reads{}, writes{}, matching_reads{}, skip_reads{}, injections{};
    bool fail_read{};
};

static Bytes image()
{
    Bytes b(4096);
    put32(b, 1024, 128); put32(b, 1028, 4); put32(b, 1036, 2);
    put16(b, 1080, 0xef53); put16(b, 1112, 256);
    b[3072] = std::byte{0x7b};
    return b;
}

static void parsers()
{
    std::array<std::byte, 1024> block{};
    auto admitted = image();
    fs::Ext4Volume volume;
    CHECK(volume.mount(admitted));
    CHECK(volume.read_block(3, block) && block[0] == std::byte{0x7b});
    for (const u64 index : {u64{4}, u64{1} << 32, u64{1} << 63, ~u64{0}})
    {
        block.fill(std::byte{0x51});
        CHECK(!volume.read_block(index, block));
        CHECK(std::all_of(block.begin(), block.end(), [](auto b) { return b == std::byte{0x51}; }));
    }
    Device device(8192);
    std::copy(admitted.begin(), admitted.end(), device.bytes.begin());
    fs::Ext4Volume device_volume;
    CHECK(device_volume.mount(device));
    auto reads = device.reads;
    CHECK(!device_volume.read_block(4, block) && device.reads == reads);
    CHECK(!device_volume.read_block(~u64{0}, block) && device.reads == reads);
    device.sectors = 7;
    CHECK(!device_volume.read_block(3, block) && device.reads == reads);
    device.sectors = 16;
    device.sector_size = 4096;
    CHECK(!device_volume.read_block(0, block) && device.reads == reads);
    device.sector_size = 512;
    CHECK(device_volume.read_block(3, block) && block[0] == std::byte{0x7b});

    for (unsigned corruption = 0; corruption != 9; ++corruption)
    {
        auto bad = admitted;
        switch (corruption)
        {
        case 0: put32(bad, 1028, 9); break;
        case 1: put32(bad, 1048, 7); break;
        case 2: put16(bad, 1080, 0); break;
        case 3: put32(bad, 1028, 0); break;
        case 4: put32(bad, 1028, 1); break;
        case 5: put32(bad, 1036, 5); break;
        case 6: put32(bad, 1024 + 0x150, 1); break;
        case 7: put32(bad, 1024 + 0x60, 0x80); break;
        case 8: put32(bad, 1048, 1); put32(bad, 1028, 3); break;
        }
        fs::Ext4Volume fresh;
        CHECK(!fresh.mount(bad) && !fresh.mounted() && !fresh.info());
        CHECK(!volume.mount(bad));
        CHECK(volume.mounted() && volume.info().value().block_count_low == 4 &&
              volume.info().value().block_size == 1024);
        CHECK(volume.read_block(3, block) && block[0] == std::byte{0x7b});
        Device rejected(4096);
        rejected.bytes = bad;
        CHECK(!device_volume.mount(rejected));
        CHECK(device_volume.info().value().block_count_low == 4 && device_volume.info().value().block_size == 1024);
        CHECK(device_volume.read_block(3, block) && block[0] == std::byte{0x7b});
        CHECK(!fresh.mount(rejected) && !fresh.mounted());
    }
    Device failed(4096);
    failed.fail_read = true;
    CHECK(!device_volume.mount(failed));
    CHECK(device_volume.read_block(3, block) && block[0] == std::byte{0x7b});
    failed.fail_read = false;
    failed.sectors = 3;
    CHECK(!device_volume.mount(failed));
    CHECK(device_volume.read_block(3, block) && block[0] == std::byte{0x7b});
    // The maximum admitted filesystem index still uses u64 sector arithmetic.
    Device large(4096);
    large.bytes = admitted;
    put32(large.bytes, 1028, 0xffffffffu);
    large.sectors = u64{0xffffffffu} * 2;
    fs::Ext4Volume large_volume;
    CHECK(large_volume.mount(large));
    CHECK(large_volume.read_block(0xfffffffeu, block));
    reads = large.reads;
    CHECK(!large_volume.read_block(0xffffffffu, block) && large.reads == reads);
    std::printf("EXT4_PARSER_BOUNDS_PASS checks=%zu hosted=1 guest=0\n", checks);
}

static usize inode_offset(const Bytes &bytes, u32 number)
{
    const u32 block_size = 1024u << get32(bytes, 1048);
    const u32 per_group = get32(bytes, 1064);
    const u32 group = (number - 1) / per_group;
    const usize descriptors = usize{get32(bytes, 1044) + 1} * block_size;
    const u32 table = get32(bytes, descriptors + group * 32 + 8);
    return usize{table} * block_size + ((number - 1) % per_group) * get16(bytes, 1112);
}
static u32 directory_block(const Bytes &bytes, u32 number)
{
    const auto inode = inode_offset(bytes, number);
    CHECK(get16(bytes, inode + 40) == 0xf30a && get16(bytes, inode + 46) == 0);
    CHECK(get16(bytes, inode + 42) != 0);
    return get32(bytes, inode + 60);
}
static Bytes malformed_directory(u32 size, unsigned corruption)
{
    Bytes block(size);
    // Deleted records with no insertion slot large enough for the probe name.
    put16(block, 4, 12);
    for (u32 offset = 12; offset < size - 4; offset += 8)
        put16(block, offset + 4, 8);
    switch (corruption)
    {
    case 0: break; // A valid chain ends with only four bytes of header remaining.
    case 1: put16(block, 4, 0); break;
    case 2: put16(block, 4, 4); break;
    case 3: put16(block, 4, 10); break;
    case 4: put16(block, 4, static_cast<u16>(size + 4)); break;
    case 5: put16(block, 4, 8); block[6] = std::byte{1}; break;
    }
    return block;
}
static void directory(const char *path)
{
    auto *file = std::fopen(path, "rb");
    CHECK(file && !std::fseek(file, 0, SEEK_END));
    const auto size = std::ftell(file);
    CHECK(size > 0);
    Bytes baseline(static_cast<usize>(size));
    std::rewind(file);
    CHECK(std::fread(baseline.data(), 1, baseline.size(), file) == baseline.size());
    std::fclose(file);
    const u32 block_size = 1024u << get32(baseline, 1048);
    testing::ext4_clock::set({{1800000100, 123456789}, {0, 1}, 1,
                             time::RealtimeOrigin::administrator_software});
    constexpr std::string_view missing = "missing-directory-probe";
    constexpr const char *operations[]{"list", "find", "create-plan", "insert", "preflight-insert", "empty-directory", "child-name"};
    for (unsigned operation = 0; operation != 7; ++operation)
        for (unsigned corruption = 0; corruption != 6; ++corruption)
        {
            Device device(baseline.size());
            device.bytes = baseline;
            auto filesystem = std::make_unique<fs::Ext4FileSystem>();
            const bool maintained = operation == 4;
            CHECK(filesystem->mount(device, fs::Ext4MountOptions{
                .writable = true, .timestamps = maintained ? fs::Ext4TimestampPolicy::maintained : fs::Ext4TimestampPolicy::unqualified}));
            auto *root = filesystem->root();
            auto *source = root->lookup("source-file");
            auto *source_directory = root->lookup("source-dir");
            auto *target_directory = root->lookup("target-dir");
            CHECK(source && source_directory && target_directory);
            const u32 number = operation == 5 ? static_cast<u32>(target_directory->metadata().attributes.inode) : 2;
            device.replacement_sector = u64{directory_block(baseline, number)} * (block_size / 512);
            device.replacement = malformed_directory(block_size, corruption);
            device.skip_reads = operation >= 2 && operation <= 4 ? 1 : 0;
            Status result;
            switch (operation)
            {
            case 0: result = root->list(fs::DirectoryReadIntent::inspection).status(); break;
            case 1: result = root->lookup_child(missing).status(); break;
            case 2: result = root->create(missing, fs::NodeType::regular); break;
            case 3:
            case 4: result = root->link(missing, *source); break;
            case 5: result = root->rename("source-dir", *root, "target-dir"); break;
            case 6: result = source_directory->directory_path(*root).status(); break;
            }
            if (result.code() != StatusCode::io_error)
                std::fprintf(stderr, "operation=%s corruption=%u status=%u injected=%zu\n", operations[operation], corruption, unsigned(result.code()), device.injections);
            CHECK(result.code() == StatusCode::io_error);
            CHECK(device.injections != 0 && device.writes == 0 && device.bytes == baseline);
            CHECK(device.reads < 100);
            CHECK(!filesystem->sync() && device.writes == 0);
            if (corruption == 5)
                std::printf("EXT4_DIRECTORY_OPERATION_PASS operation=%s malformed_shapes=6 block_size=%u\n",
                            operations[operation], block_size);
        }
    std::printf("EXT4_DIRECTORY_BOUNDS_PASS block_size=%u operations=7 malformed_shapes=6 checks=%zu writes=0 hosted=1 guest=0\n", block_size, checks);
}
int main(int argc, char **argv)
{
    testing::ext4_clock::reset();
    if (argc == 2) directory(argv[1]); else parsers();
}
