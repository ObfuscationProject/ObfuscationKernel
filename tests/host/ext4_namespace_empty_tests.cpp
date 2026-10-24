#include "ok/fs/ext4_namespace_plan.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "namespace empty model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Block = Ext4NamespaceBlock;
using Image = std::vector<u8>;

class MemoryDisk final : public driver::BlockDevice
{
  public:
    explicit MemoryDisk(const Image &image) : image_(image) {}
    driver::BlockGeometry geometry() const override { return {image_.size() / 512, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if (sector > image_.size() / 512 || out.size() % 512 ||
            out.size() / 512 > image_.size() / 512 - sector)
            return Status::invalid_argument("namespace empty read bounds");
        for (usize index = 0; index < out.size(); ++index)
            out[index] = std::byte(image_[sector * 512 + index]);
        ++reads; return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("namespace empty model must not write"); }
    Status flush() override
    { ++flushes; return Status::denied("namespace empty model must not flush"); }
    usize reads{}, writes{}, flushes{};
  private:
    const Image &image_;
};

Block load(const Image &image, u32 number)
{
    CHECK(number < image.size() / 1024);
    Block result{};
    for (usize index = 0; index < result.size(); ++index)
        result[index] = image[usize{number} * 1024 + index];
    return result;
}
u32 value(const Block &image, usize at) { return ext4_checked::le32(image, at); }
u16 short_value(const Block &image, usize at) { return ext4_checked::le16(image, at); }
void store16(std::span<u8> image, usize at, u16 value)
{ image[at] = u8(value); image[at + 1] = u8(value >> 8); }
void store32(std::span<u8> image, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) image[at + index] = u8(value >> (index * 8)); }
usize slot(u32 number) { return ((number - 1) % 4) * 256; }

struct Fixture
{
    const Ext4RecoveryInfo &binding;
    u32 source_number{}, destination_number{};
    std::string_view source_name{}, destination_name{};
    u32 parent_block{}, source_block{}, destination_block{}, directory_block{};
    Block super{}, descriptors{}, parent{}, source{}, destination{}, directory{};
    std::array<Block, 4> inode_bitmaps{}, block_bitmaps{};
    Ext4NamespaceEmptyOrphanAuthorization orphan{};

    Fixture(const Image &image, const Ext4RecoveryInfo &info, u32 from, u32 to,
            std::string_view old_name, std::string_view new_name)
        : binding(info), source_number(from), destination_number(to),
          source_name(old_name), destination_name(new_name)
    {
        super = load(image, 1); descriptors = load(image, 2);
        const auto table_for = [&](u32 number) {
            const u32 group = (number - 1) / 2048;
            return value(descriptors, group * 32 + 8) + ((number - 1) % 2048) / 4;
        };
        parent_block = table_for(2);
        source_block = table_for(from);
        destination_block = table_for(to);
        parent = load(image, parent_block);
        source = load(image, source_block);
        destination = load(image, destination_block);
        directory_block = value(parent, slot(2) + 60);
        directory = load(image, directory_block);
        for (u32 group = 0; group < 4; ++group)
        {
            block_bitmaps[group] = load(image, value(descriptors, group * 32));
            inode_bitmaps[group] = load(image, value(descriptors, group * 32 + 4));
        }
        orphan = {binding.uuid, 0, true};
    }
    Ext4NamespaceSource images() const
    {
        return {parent_block, source_block, destination_block, directory_block,
                super, descriptors, parent, source, destination, directory,
                inode_bitmaps, block_bitmaps, orphan};
    }
    Ext4NamespaceUpdate replacement() const
    {
        return {Ext4NamespaceOperation::replace, 2, source_number, destination_number,
                value(parent, slot(2) + 100), value(source, slot(source_number) + 100),
                value(destination, slot(destination_number) + 100),
                source_name, destination_name, 1700000333, 123456789};
    }
    Ext4NamespaceUpdate unlink() const
    {
        return {Ext4NamespaceOperation::unlink, 2, source_number, 0,
                value(parent, slot(2) + 100), value(source, slot(source_number) + 100),
                0, source_name, {}, 1700000333, 123456789};
    }
    usize entry(std::string_view name) const
    {
        for (usize offset = 0; offset < 1012; offset += short_value(directory, offset + 4))
            if (value(directory, offset) && directory[offset + 6] == name.size() &&
                std::string_view{reinterpret_cast<const char *>(directory.data() + offset + 8),
                                 name.size()} == name)
                return offset;
        return 1012;
    }
    void stamp_source()
    {
        auto inode = std::span<u8>{source}.subspan(slot(source_number), 256);
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const u32 checksum = jbd2::detail::crc32c(
            ext4_checked::inode_seed(seed, source_number, inode), inode);
        store16(inode, 124, u16(checksum)); store16(inode, 130, u16(checksum >> 16));
        if (source_block == destination_block) destination = source;
    }
};

const Block &after_for(const Ext4NamespacePlan &plan, u32 number)
{
    for (usize index = 0; index < plan.count; ++index)
        if (plan.targets[index] == number) return plan.after[index];
    std::abort();
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    CHECK(input);
    const Image image((std::istreambuf_iterator<char>(input)), {});
    CHECK(image.size() == 32 * 1024 * 1024);
    MemoryDisk disk(image);
    Ext4RecoveryBinding admission;
    CHECK(admission.inspect(disk, {0, image.size() / 512}));
    CHECK(admission.info());
    const auto &binding = *admission.info();
    const usize admitted_reads = disk.reads;

    Fixture empty_source(image, binding, 14, 15, "frontier-one", "frontier-two");
    for (u32 number : {14U, 15U})
    {
        const auto inode = std::span<const u8>{empty_source.source}.subspan(slot(number), 256);
        CHECK(ext4_checked::le32(inode, 4) == 0 && ext4_checked::le32(inode, 28) == 0 &&
              ext4_checked::le16(inode, 40) == 0xf30a && ext4_checked::le16(inode, 42) == 0 &&
              ext4_checked::le16(inode, 44) == 4 && ext4_checked::le16(inode, 46) == 0);
        for (usize at = 52; at < 100; ++at) CHECK(inode[at] == 0);
    }
    Ext4NamespacePlan replacement{};
    CHECK(Ext4NamespacePlanner::plan(binding, empty_source.replacement(),
                                     empty_source.images(), replacement));
    CHECK(replacement.count == 4 && replacement.orphan_published &&
          value(after_for(replacement, empty_source.directory_block), 0) == 2);
    CHECK(value(after_for(replacement, empty_source.source_block), slot(14) + 4) == 0 &&
          value(after_for(replacement, empty_source.source_block), slot(14) + 28) == 0 &&
          short_value(after_for(replacement, empty_source.source_block), slot(14) + 42) == 0);

    Fixture empty_victim(image, binding, 12, 14, "seed", "frontier-one");
    Ext4NamespacePlan replaced_empty{};
    CHECK(Ext4NamespacePlanner::plan(binding, empty_victim.replacement(),
                                     empty_victim.images(), replaced_empty));
    CHECK(replaced_empty.count == 5 && replaced_empty.orphan_published &&
          short_value(after_for(replaced_empty, empty_victim.destination_block), slot(14) + 26) == 0 &&
          value(after_for(replaced_empty, empty_victim.destination_block), slot(14) + 28) == 0 &&
          value(after_for(replaced_empty, empty_victim.directory_block),
                empty_victim.entry("frontier-one")) == 12);

    Ext4NamespacePlan unlinked{};
    CHECK(Ext4NamespacePlanner::plan(binding, empty_source.unlink(),
                                     empty_source.images(), unlinked));
    CHECK(unlinked.count == 4 && unlinked.orphan_published &&
          value(after_for(unlinked, 1), 232) == 14 &&
          value(after_for(unlinked, empty_source.directory_block),
                empty_source.entry("frontier-one")) == 0);

    usize negatives = 0;
    auto reject = [&](const Fixture &fixture) {
        Ext4NamespacePlan result{};
        const auto status = Ext4NamespacePlanner::plan(binding, fixture.unlink(), fixture.images(), result);
        CHECK(!status && status.code() == StatusCode::unsupported && result.count == 0);
        ++negatives;
    };
    auto sectors = empty_source;
    store32(std::span<u8>{sectors.source}, slot(14) + 28, 2);
    sectors.stamp_source();
    reject(sectors);
    auto extent = empty_source;
    store16(std::span<u8>{extent.source}, slot(14) + 42, 1);
    extent.stamp_source();
    reject(extent);
    auto stale_pointer = empty_source;
    store32(std::span<u8>{stale_pointer.source}, slot(14) + 52, 1);
    stale_pointer.stamp_source();
    reject(stale_pointer);
    CHECK(negatives == 3 && disk.reads == admitted_reads && disk.writes == 0 && disk.flushes == 0);
    std::printf("PASS pure namespace empty unlink/replacement negatives=%zu\n", negatives);
}
