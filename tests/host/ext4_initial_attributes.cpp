#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4/layout.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace ok;
using Bytes = std::vector<std::byte>;
static usize checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); std::exit(1); } } while (0)

u16 u16_at(const Bytes &bytes, usize offset)
{
    return std::to_integer<u16>(bytes[offset]) | (std::to_integer<u16>(bytes[offset + 1]) << 8);
}
u32 u32_at(const Bytes &bytes, usize offset)
{
    return u16_at(bytes, offset) | (u32{u16_at(bytes, offset + 2)} << 16);
}
void put16(Bytes &bytes, usize offset, u16 value)
{
    bytes[offset] = std::byte(value); bytes[offset + 1] = std::byte(value >> 8);
}
void put32(Bytes &bytes, usize offset, u32 value)
{
    put16(bytes, offset, static_cast<u16>(value)); put16(bytes, offset + 2, static_cast<u16>(value >> 16));
}
u32 block_size(const Bytes &bytes) { return 1024u << u32_at(bytes, 1048); }
usize inode_at(const Bytes &bytes, u32 number)
{
    const auto per = u32_at(bytes, 1064), size = block_size(bytes);
    const auto group = (number - 1) / per, index = (number - 1) % per;
    return usize{u32_at(bytes, (u32_at(bytes, 1044) + 1) * size + group * 32 + 8)} * size +
           index * u16_at(bytes, 1112);
}
std::vector<u32> inode_blocks(const Bytes &bytes, u32 number)
{
    const auto at = inode_at(bytes, number);
    CHECK(u16_at(bytes, at + 40) == 0xf30a && u16_at(bytes, at + 46) == 0);
    std::vector<u32> blocks;
    for (u16 i = 0; i < u16_at(bytes, at + 42); ++i)
        for (u16 j = 0; j < u16_at(bytes, at + 56 + i * 12); ++j)
            blocks.push_back(u32_at(bytes, at + 60 + i * 12) + j);
    return blocks;
}
u32 named_inode(const Bytes &bytes, u32 parent, std::string_view name)
{
    const auto size = block_size(bytes);
    for (auto block : inode_blocks(bytes, parent))
        for (u32 offset = 0; offset < size;)
        {
            const auto at = usize{block} * size + offset;
            const auto length = u16_at(bytes, at + 4);
            CHECK(length >= 8 && length % 4 == 0 && length <= size - offset);
            const auto namesize = std::to_integer<u8>(bytes[at + 6]);
            CHECK(namesize <= length - 8);
            if (u32_at(bytes, at) && std::string_view(reinterpret_cast<const char *>(bytes.data() + at + 8), namesize) == name)
                return u32_at(bytes, at);
            offset += length;
        }
    return 0;
}
bool has_room(const Bytes &bytes, u32 parent)
{
    const auto size = block_size(bytes);
    for (auto block : inode_blocks(bytes, parent))
        for (u32 offset = 0; offset < size;)
        {
            const auto at = usize{block} * size + offset;
            const auto length = u16_at(bytes, at + 4);
            CHECK(length >= 8 && length % 4 == 0 && length <= size - offset);
            const auto used = u32_at(bytes, at) ? (8 + std::to_integer<u8>(bytes[at + 6]) + 3) & ~u32{3} : 0;
            if (length - used >= 40) return true;
            offset += length;
        }
    return false;
}
void attributes(const Bytes &bytes, u32 number, const fs::InitialNodeAttributes &expected)
{
    const auto at = inode_at(bytes, number);
    CHECK(u16_at(bytes, at) == fs::mode_for(expected.type, expected.mode));
    CHECK((u32{u16_at(bytes, at + 2)} | (u32{u16_at(bytes, at + 120)} << 16)) == expected.uid);
    CHECK((u32{u16_at(bytes, at + 24)} | (u32{u16_at(bytes, at + 122)} << 16)) == expected.gid);
}
class Device final : public driver::BlockDevice
{
  public:
    explicit Device(const Bytes &input) : bytes(input), durable(input) {}
    driver::BlockGeometry geometry() const override { return {bytes.size() / 512, 512, true}; }
    bool fail(char kind)
    {
        if (armed) { trace.push_back(kind); if (++calls == failure) { triggered = true; return true; } }
        return false;
    }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        CHECK(first * 512 + out.size() <= bytes.size());
        if (fail('r')) return Status::not_found("injected transport read status");
        std::memcpy(out.data(), bytes.data() + first * 512, out.size()); return Status::success();
    }
    Status write_blocks(u64 first, std::span<const std::byte> input) override
    {
        CHECK(first * 512 + input.size() <= bytes.size()); ++writes;
        const bool error = fail('w');
        const usize count = error ? (effect == 0 ? 0 : effect == 1 ? input.size() / 2 : input.size()) : input.size();
        std::memcpy(bytes.data() + first * 512, input.data(), count);
        if (watch)
            if (const auto number = named_inode(bytes, 2, "initial-dir"))
            {
                attributes(bytes, number, watched); observed = true;
            }
        return error ? Status::unsupported("injected uncertain write") : Status::success();
    }
    Status flush() override
    {
        const bool error = fail('f'); if (!error || effect) durable = bytes;
        return error ? Status::no_memory("injected uncertain flush") : Status::success();
    }
    void arm(usize point, unsigned outcome)
    { armed = true; calls = 0; failure = point; effect = outcome; triggered = false; trace.clear(); }
    Bytes bytes, durable;
    std::vector<char> trace;
    usize calls{}, writes{}, failure{};
    unsigned effect{};
    bool armed{}, triggered{}, watch{}, observed{};
    fs::InitialNodeAttributes watched{fs::NodeType::directory, 03750, 0x12345678, 0x87654321};
};
void save(const Bytes &bytes, const std::string &path)
{
    std::ofstream stream(path, std::ios::binary); stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    CHECK(stream.good());
}
void initial_attributes(const Bytes &baseline, const std::string &output)
{
    Device disk(baseline); fs::Ext4FileSystem volume; CHECK(volume.mount(disk));
    disk.watch = true;
    auto made = volume.root()->create_child("initial-dir", disk.watched);
    CHECK(made && made.value() && disk.observed);
    disk.watch = false;
    CHECK(volume.root()->lookup_child("initial-dir").value() == made.value());
    const auto ino = named_inode(disk.bytes, 2, "initial-dir");
    attributes(disk.bytes, ino, disk.watched);
    CHECK(named_inode(disk.bytes, ino, ".") == ino && named_inode(disk.bytes, ino, "..") == 2);
    const fs::InitialNodeAttributes file{fs::NodeType::regular, 07777, 0xffffffff, 0xfedcba98};
    auto child = made.value()->create_child("full-owner", file); CHECK(child && child.value());
    attributes(disk.bytes, named_inode(disk.bytes, ino, "full-owner"), file);
    CHECK(u16_at(disk.bytes, inode_at(disk.bytes, ino) + 26) == 2);
    CHECK(u16_at(disk.bytes, inode_at(disk.bytes, 2) + 26) == u16_at(baseline, inode_at(baseline, 2) + 26) + 1);
    auto writes = disk.writes;
    CHECK(volume.root()->create_child("initial-dir", disk.watched).status().code() == StatusCode::already_exists);
    CHECK(child.value()->create_child("bad-parent", file).status().code() == StatusCode::not_directory);
    CHECK(!volume.root()->create_child("bad-mode", {fs::NodeType::regular, 010644, 0, 0}));
    CHECK(!volume.root()->create_child("bad-type", {fs::NodeType::symlink, 0777, 0, 0}));
    CHECK(!volume.root()->create_child("bad/name", file));
    CHECK(disk.writes == writes && volume.sync());
    save(disk.durable, output + "/created.img");
    Device readonly(disk.durable); fs::Ext4FileSystem ro; CHECK(ro.mount(readonly, false));
    CHECK(!ro.root()->create_child("readonly", file)); CHECK(readonly.writes == 0 && readonly.bytes == disk.durable);
    for (u16 links : {u16{0}, u16{65535}})
    {
        auto input = baseline; put16(input, inode_at(input, 2) + 26, links);
        Device limited(input); fs::Ext4FileSystem fs; CHECK(fs.mount(limited));
        auto result = fs.root()->create_child("limit", disk.watched);
        CHECK(result.status().code() == (links ? StatusCode::link_limit : StatusCode::not_found));
        CHECK(limited.writes == 0 && limited.bytes == input);
    }
}
Bytes free_block_budget(const Bytes &input, unsigned budget)
{
    auto bytes = input; const auto size = block_size(input), first = u32_at(input, 1044);
    const auto per = u32_at(input, 1056), blocks = u32_at(input, 1028), groups = (blocks - first - 1) / per + 1;
    const usize descriptors = (first + 1) * size;
    unsigned remaining = budget;
    for (u32 group = 0; group < groups; ++group)
    {
        const auto bitmap = usize{u32_at(input, descriptors + group * 32)} * size;
        for (u32 i = 0; i < size; ++i) bytes[bitmap + i] = std::byte{0xff};
        unsigned kept = 0;
        for (u32 bit = 0; bit < per && first + group * per + bit < blocks && remaining; ++bit)
            if (!(std::to_integer<u8>(input[bitmap + bit / 8]) & (1u << (bit % 8))))
            {
                bytes[bitmap + bit / 8] &= std::byte(~(1u << (bit % 8)));
                --remaining; ++kept;
            }
        put16(bytes, descriptors + group * 32 + 12, static_cast<u16>(kept));
    }
    CHECK(remaining == 0); put32(bytes, 1036, budget); return bytes;
}
Bytes full_parent(const Bytes &input, bool four_extents)
{
    Device disk(input); fs::Ext4FileSystem volume; CHECK(volume.mount(disk));
    auto parent = volume.root()->create_child("capacity", {fs::NodeType::directory, 0755, 0, 0});
    auto file = volume.root()->create_child("source", {fs::NodeType::regular, 0644, 0, 0});
    CHECK(parent && file);
    const auto ino = named_inode(disk.bytes, 2, "capacity");
    unsigned links = 0, blockers = 0; u64 old_blocks = 0;
    while (true)
    {
        const auto blocks = parent.value()->query_metadata().value().blocks;
        if (four_extents && blocks != old_blocks)
        {
            const auto name = "blocker-" + std::to_string(blockers++);
            auto blocker = volume.root()->create_child(name, {fs::NodeType::regular, 0644, 0, 0}); CHECK(blocker);
            const std::byte value{0x41}; CHECK(blocker.value()->write(0, {&value, 1})); old_blocks = blocks;
        }
        if (!has_room(disk.bytes, ino) && (!four_extents || u16_at(disk.bytes, inode_at(disk.bytes, ino) + 42) == 4)) break;
        char name[32]; std::snprintf(name, sizeof(name), "%031u", links++);
        CHECK(parent.value()->link(name, *file.value())); CHECK(links < 1000);
    }
    CHECK(volume.sync()); return disk.durable;
}
void capacity(const Bytes &baseline, const std::string &output)
{
    auto no_inodes = baseline;
    const auto size = block_size(baseline), first = u32_at(baseline, 1044), per = u32_at(baseline, 1056);
    const auto groups = (u32_at(baseline, 1028) - first - 1) / per + 1;
    for (u32 group = 0; group < groups; ++group)
    {
        const auto descriptor = usize{first + 1} * size + group * 32;
        const auto bitmap = usize{u32_at(baseline, descriptor + 4)} * size;
        for (u32 i = 0; i < size; ++i) no_inodes[bitmap + i] = std::byte{0xff};
        put16(no_inodes, descriptor + 14, 0);
    }
    put32(no_inodes, 1040, 0);
    Device exhausted(no_inodes); fs::Ext4FileSystem no_inode_fs; CHECK(no_inode_fs.mount(exhausted));
    CHECK(no_inode_fs.root()->create_child("no-inodes", {fs::NodeType::directory, 0700, 1, 2}).status().code() == StatusCode::no_space);
    CHECK(exhausted.writes == 0 && exhausted.bytes == no_inodes);
    const auto no_blocks = free_block_budget(baseline, 0);
    Device block_limited(no_blocks); fs::Ext4FileSystem no_block_fs; CHECK(no_block_fs.mount(block_limited));
    CHECK(no_block_fs.root()->create_child("no-blocks", {fs::NodeType::directory, 0700, 1, 2}).status().code() == StatusCode::no_space);
    CHECK(no_block_fs.sync() && block_limited.durable == no_blocks);
    CHECK(no_block_fs.root()->create_child("still-writable", {fs::NodeType::regular, 0600, 1, 2}));
    CHECK(no_block_fs.sync());

    Device disk(baseline); fs::Ext4FileSystem volume; CHECK(volume.mount(disk));
    for (unsigned i = 0; i < 127; ++i)
        CHECK(volume.root()->create_child("cache-" + std::to_string(i), {fs::NodeType::regular, 0600, 1, 2}));
    const auto writes = disk.writes;
    for (unsigned i = 0; i < 2; ++i)
        CHECK(volume.root()->create_child("cache-full", {fs::NodeType::directory, 0700, 1, 2}).status().code() == StatusCode::no_memory);
    CHECK(disk.writes == writes && volume.sync()); save(disk.durable, output + "/node-cache.img");

    const auto fragmented = full_parent(baseline, true);
    for (auto type : {fs::NodeType::regular, fs::NodeType::directory})
    {
        Device target(fragmented); fs::Ext4FileSystem fs; CHECK(fs.mount(target));
        auto *parent = fs.root()->lookup("capacity"); CHECK(parent);
        CHECK(parent->create_child("extent-capacity-name-0000000000", {type, 0700, 11, 12}).status().code() == StatusCode::no_space);
        CHECK(!parent->lookup_child("extent-capacity-name-0000000000"));
        CHECK(fs.sync() && target.durable == fragmented);
        CHECK(parent->remove("0000000000000000000000000000000", false));
        CHECK(parent->create_child("fits", {type, 0700, 11, 12}));
        CHECK(fs.sync());
    }
    save(fragmented, output + "/extent-rollback.img");
}
usize fault_matrix(const Bytes &baseline, bool rollback, bool parent_growth, fs::NodeType type)
{
    const fs::InitialNodeAttributes initial{type, 03751, 0xffffffff, 0x87654321};
    const auto invoke = [&](fs::Node *parent) { return parent->create_child("new-child-000000000000000000000", initial); };
    Device good(baseline); fs::Ext4FileSystem successful; CHECK(successful.mount(good));
    auto *parent = parent_growth ? successful.root()->lookup("capacity") : successful.root(); CHECK(parent);
    good.arm(~usize{0}, 0);
    auto result = invoke(parent);
    CHECK(rollback ? result.status().code() == StatusCode::no_space : result.ok());
    const auto operations = good.trace.size();
    if (rollback) CHECK(successful.sync() && good.durable == baseline);
    usize cases = 0;
    for (usize point = 1; point <= operations; ++point)
        for (unsigned effect = 0; effect < 3; ++effect)
        {
            Device disk(baseline); fs::Ext4FileSystem volume; CHECK(volume.mount(disk));
            auto *directory = parent_growth ? volume.root()->lookup("capacity") : volume.root(); CHECK(directory);
            disk.arm(point, effect);
            auto failed = invoke(directory); CHECK(!failed && disk.triggered);
            CHECK(failed.status().code() == StatusCode::io_error);
            const auto writes = disk.writes;
            CHECK(!volume.sync()); CHECK(!directory->create_child("after-failure", initial));
            CHECK(disk.writes == writes);
            CHECK(volume.mount(disk).code() == StatusCode::busy);
            ++cases;
        }
    return cases;
}
int main(int argc, char **argv)
{
    testing::ext4_clock::reset();
    struct LegacyClockGuard { ~LegacyClockGuard() { CHECK(testing::ext4_clock::calls() == 0); } } legacy_clock_guard;
    CHECK(argc == 3);
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate); CHECK(file.good());
    Bytes baseline(static_cast<usize>(file.tellg())); file.seekg(0);
    file.read(reinterpret_cast<char *>(baseline.data()), baseline.size()); CHECK(file.good());
    initial_attributes(baseline, argv[2]); capacity(baseline, argv[2]);
    const auto full = full_parent(baseline, false);
    usize failures = 0;
    for (auto type : {fs::NodeType::regular, fs::NodeType::directory})
    {
        failures += fault_matrix(baseline, false, false, type);
        failures += fault_matrix(full, false, true, type);
    }
    failures += fault_matrix(free_block_budget(baseline, 0), true, false, fs::NodeType::directory);
    failures += fault_matrix(free_block_budget(full, 1), true, true, fs::NodeType::directory);
    std::printf("PASS ext4 initial attributes checks=%zu transport-fault-cases=%zu atomic-observation=1 cache-admission=1 logical-rollback=1\n", checks, failures);
}
