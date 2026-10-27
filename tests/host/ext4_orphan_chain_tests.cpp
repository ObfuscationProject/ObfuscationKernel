#include "ok/fs/ext4/alloc.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "orphan chain model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Block = Ext4OrphanBlock;
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
            return Status::invalid_argument("orphan model read bounds");
        for (usize index = 0; index < out.size(); ++index)
            out[index] = std::byte(image_[sector * 512 + index]);
        ++reads; return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("orphan model must not write"); }
    Status flush() override
    { ++flushes; return Status::denied("orphan model must not flush"); }
    usize reads{}, writes{}, flushes{};
  private:
    const Image &image_;
};

class ImageReader final
{
  public:
    explicit ImageReader(const Image &image) : image_(image) {}
    Status read_block(u32 number, std::span<u8> out)
    {
        if (out.size() < 1024 || number >= image_.size() / 1024)
            return Status::invalid_argument("orphan model block read bounds");
        for (usize index = 0; index < 1024; ++index)
            out[index] = image_[usize{number} * 1024 + index];
        ++reads; return {};
    }
    usize reads{};
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
u32 value(const Block &bytes, usize at) { return ext4_checked::le32(bytes, at); }
void store16(std::span<u8> bytes, usize at, u16 value)
{ bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
void store32(std::span<u8> bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8)); }
usize slot(u32 number) { return ((number - 1) % 4) * 256; }

struct Fixture
{
    const Ext4RecoveryInfo &binding;
    Block super{}, descriptors{};
    std::array<Block, 4> inode_bitmaps{}, block_bitmaps{};
    std::array<Ext4OrphanInodeImage, 8> images{};
    usize count{};

    Fixture(const Image &image, const Ext4RecoveryInfo &info) : binding(info)
    {
        super = load(image, 1); descriptors = load(image, 2);
        for (u32 group = 0; group < 4; ++group)
        {
            block_bitmaps[group] = load(image, value(descriptors, group * 32));
            inode_bitmaps[group] = load(image, value(descriptors, group * 32 + 4));
        }
    }
    Ext4OrphanChainSource source() const
    { return {super, descriptors, inode_bitmaps, block_bitmaps, std::span{images}.first(count)}; }
    u32 table_for(u32 number) const
    {
        const u32 group = (number - 1) / 2048;
        return value(descriptors, group * 32 + 8) + ((number - 1) % 2048) / 4;
    }
    void stamp_super()
    {
        store32(super, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super}.first(1020)));
    }
    void stamp_inode(Block &table, u32 number)
    {
        auto inode = std::span<u8>{table}.subspan(slot(number), 256);
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const u32 checksum = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(checksum)); store16(inode, 130, u16(checksum >> 16));
    }
    void stamp_inode_bitmap(u32 group)
    {
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        store16(descriptors, group * 32 + 26, u16(jbd2::detail::crc32c(
            seed, std::span<const u8>{inode_bitmaps[group]}.first(256))));
        stamp_descriptor(group);
    }
    void stamp_block_bitmap(u32 group)
    {
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        store16(descriptors, group * 32 + 24, u16(jbd2::detail::crc32c(
            seed, std::span<const u8>{block_bitmaps[group]})));
        stamp_descriptor(group);
    }
    void stamp_descriptor(u32 group)
    {
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const usize at = group * 32;
        store16(descriptors, at + 30, 0);
        store16(descriptors, at + 30, u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)),
            std::span<const u8>{descriptors}.subspan(at, 32))));
    }
    void add(const Image &image, u32 number, u32 next)
    {
        CHECK(count < images.size());
        auto &entry = images[count++];
        entry.number = number;
        entry.table_block = table_for(number);
        entry.table = load(image, entry.table_block);
        auto inode = std::span<u8>{entry.table}.subspan(slot(number), 256);
        entry.expected_generation = ext4_checked::le32(inode, 100);
        store16(inode, 26, 0);
        store32(inode, 20, next);
        // The namespace transaction increments i_version before publication.
        store32(inode, 36, ext4_checked::le32(inode, 36) + 1);
        stamp_inode(entry.table, number);
        for (usize prior = 0; prior + 1 < count; ++prior)
        {
            if (images[prior].table_block != entry.table_block) continue;
            auto &earlier = images[prior].table;
            auto copy = std::span<const u8>{earlier}.subspan(slot(images[prior].number), 256);
            for (usize index = 0; index < 256; ++index)
                entry.table[slot(images[prior].number) + index] = copy[index];
            copy = std::span<const u8>{entry.table}.subspan(slot(number), 256);
            for (usize index = 0; index < 256; ++index)
                earlier[slot(number) + index] = copy[index];
        }
    }
    void head(u32 number)
    {
        store32(super, 232, number);
        stamp_super();
    }
};

void negative(const Ext4RecoveryInfo &binding, const Fixture &fixture, StatusCode code, usize &count)
{
    Ext4OrphanChainProof proof{};
    const auto status = Ext4OrphanChainValidator::validate(binding, fixture.source(), proof);
    CHECK(!status && status.code() == code && proof.count() == 0);
    ++count;
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

    Fixture empty(image, binding);
    Ext4OrphanChainProof empty_proof{};
    CHECK(Ext4OrphanChainValidator::validate(binding, empty.source(), empty_proof));
    CHECK(empty_proof.count() == 0 && empty_proof.observed_head() == 0 &&
          !empty_proof.member(14, 0));

    Fixture chain(image, binding);
    chain.add(image, 15, 14); chain.add(image, 14, 0); chain.head(15);
    Ext4OrphanChainProof proof{};
    CHECK(Ext4OrphanChainValidator::validate(binding, chain.source(), proof));
    CHECK(proof.count() == 2 && proof.observed_head() == 15 &&
          proof.member(15, chain.images[0].expected_generation) &&
          proof.member(14, chain.images[1].expected_generation) &&
          !proof.member(14, chain.images[1].expected_generation + 1));
    CHECK(proof.at(0) && proof.at(1) && !proof.at(2) &&
          proof.at(0)->size == 0 && proof.at(0)->data_count == 0 &&
          proof.at(1)->size == 0 && proof.at(1)->data_count == 0);

    Fixture data(image, binding);
    data.add(image, 12, 14); data.add(image, 14, 0); data.head(12);
    Ext4OrphanChainProof data_proof{};
    CHECK(Ext4OrphanChainValidator::validate(binding, data.source(), data_proof));
    CHECK(data_proof.count() == 2 && data_proof.at(0) && data_proof.at(1) &&
          data_proof.at(0)->size == 4096 && data_proof.at(0)->data_count == 4 &&
          data_proof.at(1)->data_count == 0);
    Fixture two_data(image, binding);
    two_data.add(image, 12, 13); two_data.add(image, 13, 0); two_data.head(12);
    Ext4OrphanChainProof two_data_proof{};
    CHECK(Ext4OrphanChainValidator::validate(binding, two_data.source(), two_data_proof));
    CHECK(two_data_proof.count() == 2 && two_data_proof.at(1)->data_count == 1);

    // The unchanged ordinary pathname decoder rejects the very same zero-link
    // inode bytes. The synthetic overlay leaves the superblock's head at zero
    // solely so that its ordinary view can open and exercise inode() itself.
    Image linked_view = image;
    for (usize index = 0; index < 1024; ++index)
        linked_view[usize{chain.images[0].table_block} * 1024 + index] = chain.images[0].table[index];
    ImageReader reader(linked_view);
    Ext4ReadOnlyView<ImageReader> ordinary;
    CHECK(ordinary.open(reader, binding));
    const auto pathname = ordinary.inode(14);
    CHECK(!pathname && pathname.status().code() == StatusCode::unsupported);

    usize rejected = 0;
    auto broken_crc = chain;
    broken_crc.super[1020] ^= 1;
    negative(binding, broken_crc, StatusCode::io_error, rejected);
    auto missing = chain;
    missing.count = 1;
    negative(binding, missing, StatusCode::io_error, rejected);
    auto reordered = chain;
    std::swap(reordered.images[0], reordered.images[1]);
    negative(binding, reordered, StatusCode::io_error, rejected);
    auto cycle = chain;
    for (auto &entry : cycle.images)
        if (entry.number == 14)
        {
            auto inode = std::span<u8>{entry.table}.subspan(slot(14), 256);
            store32(inode, 20, 15); cycle.stamp_inode(entry.table, 14);
        }
    cycle.images[0].table = cycle.images[1].table;
    cycle.images[2] = cycle.images[0];
    cycle.count = 3;
    negative(binding, cycle, StatusCode::io_error, rejected);
    auto bad_generation = chain;
    ++bad_generation.images[0].expected_generation;
    negative(binding, bad_generation, StatusCode::io_error, rejected);
    auto unallocated = chain;
    unallocated.inode_bitmaps[0][(15 - 1) / 8] &= u8(~(1U << ((15 - 1) % 8)));
    unallocated.stamp_inode_bitmap(0);
    negative(binding, unallocated, StatusCode::io_error, rejected);
    auto live_link = chain;
    for (auto &entry : live_link.images)
    {
        store16(std::span<u8>{entry.table}, slot(15) + 26, 1);
        live_link.stamp_inode(entry.table, 15);
    }
    negative(binding, live_link, StatusCode::unsupported, rejected);
    auto stale_extent = chain;
    for (auto &entry : stale_extent.images)
    {
        store32(std::span<u8>{entry.table}, slot(15) + 52, 1);
        stale_extent.stamp_inode(entry.table, 15);
    }
    negative(binding, stale_extent, StatusCode::unsupported, rejected);
    auto bad_table_alias = chain;
    bad_table_alias.images[1].table[0] ^= 1;
    negative(binding, bad_table_alias, StatusCode::io_error, rejected);
    auto too_many = chain;
    too_many.count = 8;
    negative(binding, too_many, StatusCode::io_error, rejected);
    auto wrong_uuid = chain;
    wrong_uuid.super[104] ^= 1; wrong_uuid.stamp_super();
    negative(binding, wrong_uuid, StatusCode::io_error, rejected);
    auto bad_descriptor = chain;
    bad_descriptor.descriptors[30] ^= 1;
    negative(binding, bad_descriptor, StatusCode::io_error, rejected);
    auto missing_data = data;
    const u32 physical = data_proof.at(0)->data_blocks[0];
    const u32 group = (physical - 1) / 8192, local = (physical - 1) % 8192;
    missing_data.block_bitmaps[group][local / 8] &= u8(~(1U << (local % 8)));
    missing_data.stamp_block_bitmap(group);
    negative(binding, missing_data, StatusCode::io_error, rejected);
    auto shared_data = two_data;
    store32(std::span<u8>{shared_data.images[1].table}, slot(13) + 60,
            two_data_proof.at(0)->data_blocks[0]);
    shared_data.stamp_inode(shared_data.images[1].table, 13);
    negative(binding, shared_data, StatusCode::io_error, rejected);
    auto impossible_head = chain;
    impossible_head.head(8193);
    negative(binding, impossible_head, StatusCode::io_error, rejected);
    std::array<Ext4OrphanInodeImage, 9> over_capacity{};
    auto wide_source = chain.source();
    wide_source.chain = over_capacity;
    Ext4OrphanChainProof capacity_proof{};
    const auto capacity_status = Ext4OrphanChainValidator::validate(binding, wide_source, capacity_proof);
    CHECK(!capacity_status && capacity_status.code() == StatusCode::unsupported &&
          capacity_proof.count() == 0);
    ++rejected;
    CHECK(rejected == 16 && disk.reads == admitted_reads && disk.writes == 0 && disk.flushes == 0);
    std::printf("PASS pure orphan chain model: empty, two-inode, data, ordinary rejection, negatives=%zu\n",
                rejected);
}
