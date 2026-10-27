#include "ok/fs/ext4/dir.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "namespace planner test:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Block = Ext4NamespaceBlock;
using Image = std::vector<u8>;

class MemoryDisk final : public driver::BlockDevice
{
  public:
    explicit MemoryDisk(const Image &bytes) : bytes_(bytes) {}
    driver::BlockGeometry geometry() const override { return {bytes_.size() / 512, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if (sector > bytes_.size() / 512 || out.size() % 512 ||
            out.size() / 512 > bytes_.size() / 512 - sector)
            return Status::invalid_argument("namespace test read bounds");
        for (usize index = 0; index < out.size(); ++index)
            out[index] = std::byte(bytes_[sector * 512 + index]);
        ++reads; return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("pure planner must not write"); }
    Status flush() override
    { ++flushes; return Status::denied("pure planner must not flush"); }
    usize reads{}, writes{}, flushes{};
  private:
    const Image &bytes_;
};

Block load(const Image &image, u32 number)
{
    CHECK(number < image.size() / 1024);
    Block result{};
    for (usize index = 0; index < result.size(); ++index)
        result[index] = image[number * 1024 + index];
    return result;
}
u32 value(const Block &image, usize at) { return ext4_checked::le32(image, at); }
u16 short_value(const Block &image, usize at) { return ext4_checked::le16(image, at); }
void store16(Block &image, usize at, u16 value)
{ image[at] = u8(value); image[at + 1] = u8(value >> 8); }
void store32(Block &image, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) image[at + index] = u8(value >> (index * 8)); }
usize slot(u32 inode) { return ((inode - 1) % 4) * 256; }

struct Fixture
{
    const Image &disk;
    const Ext4RecoveryInfo &binding;
    u32 parent_number{2}, source_number{14}, destination_number{15};
    u32 parent_block{}, source_block{}, destination_block{}, directory_block{};
    Block super{}, descriptors{}, parent{}, source{}, destination{}, directory{};
    std::array<Block, 4> inode_bitmaps{}, block_bitmaps{};
    Ext4NamespaceEmptyOrphanAuthorization orphan{};

    Fixture(const Image &image, const Ext4RecoveryInfo &info, u32 from = 14, u32 to = 15)
        : disk(image), binding(info), source_number(from), destination_number(to)
    {
        super = load(disk, 1); descriptors = load(disk, 2);
        const auto table_for = [&](u32 inode) {
            const u32 group = (inode - 1) / 2048;
            return value(descriptors, group * 32 + 8) + ((inode - 1) % 2048) / 4;
        };
        parent_block = table_for(parent_number);
        source_block = table_for(source_number);
        destination_block = table_for(destination_number);
        parent = load(disk, parent_block);
        source = load(disk, source_block);
        destination = load(disk, destination_block);
        directory_block = value(parent, slot(parent_number) + 60);
        directory = load(disk, directory_block);
        for (u32 group = 0; group < 4; ++group)
        {
            block_bitmaps[group] = load(disk, value(descriptors, group * 32));
            inode_bitmaps[group] = load(disk, value(descriptors, group * 32 + 4));
        }
        orphan = {binding.uuid, 0, true};
    }
    Ext4NamespaceSource images() const
    {
        return {parent_block, source_block, destination_block, directory_block,
                super, descriptors, parent, source, destination, directory,
                inode_bitmaps, block_bitmaps, orphan};
    }
    Ext4NamespaceUpdate replacement(std::string_view old_name, std::string_view new_name) const
    {
        return {Ext4NamespaceOperation::replace, parent_number, source_number, destination_number,
                value(parent, slot(parent_number) + 100), value(source, slot(source_number) + 100),
                value(destination, slot(destination_number) + 100), old_name, new_name,
                1700000333, 123456789};
    }
    Ext4NamespaceUpdate unlink(std::string_view old_name) const
    {
        return {Ext4NamespaceOperation::unlink, parent_number, source_number, 0,
                value(parent, slot(parent_number) + 100), value(source, slot(source_number) + 100),
                0, old_name, {}, 1700000333, 123456789};
    }
    void stamp_inode(Block &table, u32 number)
    {
        auto inode = std::span<u8>{table}.subspan(slot(number), 256);
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const u32 checksum = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        inode[124] = u8(checksum); inode[125] = u8(checksum >> 8);
        inode[130] = u8(checksum >> 16); inode[131] = u8(checksum >> 24);
    }
    void stamp_directory()
    {
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const auto inode = std::span<const u8>{parent}.subspan(slot(parent_number), 256);
        store32(directory, 1020, jbd2::detail::crc32c(
            ext4_checked::inode_seed(seed, parent_number, inode),
            std::span<const u8>{directory}.first(1012)));
    }
    void stamp_descriptor(u32 group)
    {
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const usize offset = group * 32;
        store16(descriptors, offset + 30, 0);
        const u32 checksum = jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)),
            std::span<const u8>{descriptors}.subspan(offset, 32));
        store16(descriptors, offset + 30, u16(checksum));
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
    usize entry(std::string_view name) const
    {
        for (usize offset = 0; offset < 1012; offset += short_value(directory, offset + 4))
            if (value(directory, offset) && directory[offset + 6] == name.size() &&
                std::string_view{reinterpret_cast<const char *>(directory.data() + offset + 8), name.size()} == name)
                return offset;
        return 1012;
    }
    void add_alias(u32 number, std::string_view anchor, std::string_view alias)
    {
        const usize at = entry(anchor);
        CHECK(at < 1012 && alias.size() <= 31);
        const usize minimum = (8 + directory[at + 6] + 3) & ~usize{3};
        const u16 record = short_value(directory, at + 4);
        CHECK(record >= minimum + ((8 + alias.size() + 3) & ~usize{3}));
        store16(directory, at + 4, u16(minimum));
        const usize created = at + minimum;
        store32(directory, created, number);
        store16(directory, created + 4, u16(record - minimum));
        directory[created + 6] = u8(alias.size()); directory[created + 7] = 1;
        for (usize index = created + 8; index < at + record; ++index) directory[index] = 0;
        for (usize index = 0; index < alias.size(); ++index)
            directory[created + 8 + index] = u8(alias[index]);
        stamp_directory();
    }
};

const Block &before_for(const Ext4NamespacePlan &plan, u32 block)
{
    for (usize index = 0; index < plan.count; ++index)
        if (plan.targets[index] == block) return plan.before[index];
    std::abort();
}
const Block &after_for(const Ext4NamespacePlan &plan, u32 block)
{
    for (usize index = 0; index < plan.count; ++index)
        if (plan.targets[index] == block) return plan.after[index];
    std::abort();
}
void pure(const MemoryDisk &disk, usize reads)
{ CHECK(disk.reads == reads && disk.writes == 0 && disk.flushes == 0); }
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    CHECK(input);
    const Image bytes((std::istreambuf_iterator<char>(input)), {});
    CHECK(bytes.size() == 32 * 1024 * 1024);
    MemoryDisk disk(bytes);
    Ext4RecoveryBinding admission;
    CHECK(admission.inspect(disk, {0, bytes.size() / 512}));
    CHECK(admission.info());
    const auto &binding = *admission.info();
    const usize admitted_reads = disk.reads;

    Fixture base(bytes, binding);
    Ext4NamespacePlan replacement{};
    CHECK(Ext4NamespacePlanner::plan(binding, base.replacement("basic-note", "second-note"),
                                     base.images(), replacement));
    CHECK(replacement.count == 4 && !replacement.no_op && replacement.orphan_published &&
          replacement.allocation_unchanged && replacement.removed_inode == 15);
    CHECK(before_for(replacement, base.source_block) == base.source);
    CHECK(value(after_for(replacement, 1), 232) == 15);
    CHECK(short_value(after_for(replacement, base.destination_block), slot(15) + 26) == 0);
    CHECK(short_value(after_for(replacement, base.source_block), slot(14) + 26) == 1);
    CHECK(value(after_for(replacement, base.directory_block), base.entry("basic-note")) == 0);
    CHECK(value(after_for(replacement, base.directory_block), base.entry("second-note")) == 14);
    CHECK(replacement.targets[0] == base.parent_block &&
          replacement.targets[1] == base.source_block &&
          replacement.targets[2] == base.directory_block && replacement.targets[3] == 1);
    const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
    const auto &parent_after = after_for(replacement, base.parent_block);
    const auto &table_after = after_for(replacement, base.source_block);
    CHECK(ext4_checked::inode_checksum(seed, 2,
          std::span<const u8>{parent_after}.subspan(slot(2), 256)));
    CHECK(ext4_checked::inode_checksum(seed, 14,
          std::span<const u8>{table_after}.subspan(slot(14), 256)));
    CHECK(ext4_checked::inode_checksum(seed, 15,
          std::span<const u8>{table_after}.subspan(slot(15), 256)));
    CHECK(jbd2::detail::crc32c(~u32{0},
          std::span<const u8>{after_for(replacement, 1)}.first(1020)) ==
          value(after_for(replacement, 1), 1020));
    CHECK(jbd2::detail::crc32c(ext4_checked::inode_seed(
          seed, 2, std::span<const u8>{base.parent}.subspan(slot(2), 256)),
          std::span<const u8>{after_for(replacement, base.directory_block)}.first(1012)) ==
          value(after_for(replacement, base.directory_block), 1020));
    for (usize index = 0; index < 1024; ++index)
    {
        if (index < slot(2) || index >= slot(2) + 256)
            CHECK(parent_after[index] == base.parent[index]);
        if ((index < slot(14) || index >= slot(14) + 256) &&
            (index < slot(15) || index >= slot(15) + 256))
            CHECK(table_after[index] == base.source[index]);
    }
    const usize old_at = base.entry("basic-note"), new_at = base.entry("second-note");
    for (usize index = 0; index < 1012; ++index)
        if ((index < old_at || index >= old_at + short_value(base.directory, old_at + 4)) &&
            (index < new_at || index >= new_at + short_value(base.directory, new_at + 4)))
            CHECK(after_for(replacement, base.directory_block)[index] == base.directory[index]);
    pure(disk, admitted_reads);

    Fixture remove(bytes, binding);
    remove.destination_number = remove.source_number;
    remove.destination_block = remove.source_block;
    remove.destination = remove.source;
    Ext4NamespacePlan unlinked{};
    CHECK(Ext4NamespacePlanner::plan(binding, remove.unlink("basic-note"), remove.images(), unlinked));
    CHECK(unlinked.count == 4 && unlinked.orphan_published && unlinked.removed_inode == 14 &&
          value(after_for(unlinked, 1), 232) == 14 &&
          value(after_for(unlinked, remove.directory_block), remove.entry("basic-note")) == 0 &&
          value(after_for(unlinked, remove.directory_block), remove.entry("second-note")) == 15);
    pure(disk, admitted_reads);

    Fixture unlink_linked(bytes, binding);
    unlink_linked.destination_number = unlink_linked.source_number;
    unlink_linked.destination_block = unlink_linked.source_block;
    store16(unlink_linked.source, slot(14) + 26, 2);
    unlink_linked.stamp_inode(unlink_linked.source, 14);
    unlink_linked.destination = unlink_linked.source;
    unlink_linked.add_alias(14, "second-note", "basic-link");
    Ext4NamespacePlan unlink_decremented{};
    CHECK(Ext4NamespacePlanner::plan(binding, unlink_linked.unlink("basic-note"),
                                     unlink_linked.images(), unlink_decremented));
    CHECK(unlink_decremented.count == 3 && !unlink_decremented.orphan_published &&
          short_value(after_for(unlink_decremented, unlink_linked.source_block), slot(14) + 26) == 1 &&
          value(after_for(unlink_decremented, unlink_linked.directory_block),
                unlink_linked.entry("basic-link")) == 14);
    pure(disk, admitted_reads);

    Fixture linked(bytes, binding);
    store16(linked.destination, slot(15) + 26, 2);
    linked.stamp_inode(linked.destination, 15);
    linked.source = linked.destination; // Source and victim share one table block.
    linked.add_alias(15, "second-note", "second-link");
    Ext4NamespacePlan decremented{};
    CHECK(Ext4NamespacePlanner::plan(binding, linked.replacement("basic-note", "second-note"),
                                     linked.images(), decremented));
    CHECK(decremented.count == 3 && !decremented.orphan_published &&
          short_value(after_for(decremented, linked.destination_block), slot(15) + 26) == 1 &&
          value(after_for(decremented, linked.directory_block), linked.entry("second-link")) == 15);
    pure(disk, admitted_reads);

    Fixture same(bytes, binding);
    same.destination_number = same.source_number;
    same.destination_block = same.source_block;
    store16(same.source, slot(14) + 26, 2);
    same.stamp_inode(same.source, 14);
    same.destination = same.source;
    same.add_alias(14, "second-note", "same-note");
    Ext4NamespacePlan no_op{};
    CHECK(Ext4NamespacePlanner::plan(binding, same.replacement("basic-note", "same-note"),
                                     same.images(), no_op));
    CHECK(no_op.no_op && no_op.count == 0 && no_op.allocation_unchanged && !no_op.orphan_published);
    pure(disk, admitted_reads);

    Fixture identical(bytes, binding);
    identical.destination_number = identical.source_number;
    identical.destination_block = identical.source_block;
    identical.destination = identical.source;
    Ext4NamespacePlan identical_no_op{};
    CHECK(Ext4NamespacePlanner::plan(binding, identical.replacement("basic-note", "basic-note"),
                                     identical.images(), identical_no_op));
    CHECK(identical_no_op.no_op && identical_no_op.count == 0 && identical_no_op.allocation_unchanged);
    pure(disk, admitted_reads);

    Fixture wide(bytes, binding, 12, 14);
    Ext4NamespacePlan five{};
    CHECK(Ext4NamespacePlanner::plan(binding, wide.replacement("seed", "basic-note"),
                                     wide.images(), five));
    CHECK(five.count == 5 && five.targets[0] == wide.parent_block &&
          five.targets[1] == wide.source_block && five.targets[2] == wide.destination_block &&
          five.targets[3] == wide.directory_block && five.targets[4] == 1);
    pure(disk, admitted_reads);

    usize negative = 0;
    auto reject = [&](const Ext4RecoveryInfo &info, const Ext4NamespaceUpdate &update,
                      const Fixture &fixture, StatusCode code) {
        Ext4NamespacePlan output{};
        const auto status = Ext4NamespacePlanner::plan(info, update, fixture.images(), output);
        CHECK(!status && status.code() == code && output.count == 0 && !output.no_op);
        pure(disk, admitted_reads);
        ++negative;
    };
    auto bad_generation = base.replacement("basic-note", "second-note");
    ++bad_generation.source_generation;
    reject(binding, bad_generation, base, StatusCode::io_error);
    auto bad_name = base.replacement("basic-note", "second-note");
    bad_name.destination_name = "bad/name";
    reject(binding, bad_name, base, StatusCode::invalid_argument);
    auto bad_time = base.unlink("basic-note");
    bad_time.nanoseconds = 1000000000;
    reject(binding, bad_time, remove, StatusCode::invalid_argument);
    auto bad_unlink_destination = base.unlink("basic-note");
    bad_unlink_destination.destination_generation = 1;
    reject(binding, bad_unlink_destination, remove, StatusCode::invalid_argument);
    auto bad_checksum = base;
    bad_checksum.parent[slot(2) + 124] ^= 1;
    reject(binding, base.replacement("basic-note", "second-note"),
           bad_checksum, StatusCode::io_error);
    auto bad_dir = base;
    bad_dir.directory[1020] ^= 1;
    reject(binding, base.replacement("basic-note", "second-note"), bad_dir, StatusCode::io_error);
    auto bad_alias = base;
    bad_alias.source[0] ^= 1;
    reject(binding, base.replacement("basic-note", "second-note"), bad_alias, StatusCode::io_error);
    auto absent_inode = base;
    absent_inode.inode_bitmaps[0][(14 - 1) / 8] &= u8(~(1U << ((14 - 1) % 8)));
    absent_inode.stamp_inode_bitmap(0);
    reject(binding, base.replacement("basic-note", "second-note"),
           absent_inode, StatusCode::io_error);
    auto absent_data = base;
    const u32 physical = value(base.source, slot(14) + 60);
    absent_data.block_bitmaps[0][(physical - 1) / 8] &= u8(~(1U << ((physical - 1) % 8)));
    absent_data.stamp_block_bitmap(0);
    reject(binding, base.replacement("basic-note", "second-note"),
           absent_data, StatusCode::io_error);
    auto absent_directory = base;
    const u32 directory_group = (base.directory_block - 1) / 8192;
    const u32 directory_local = (base.directory_block - 1) % 8192;
    absent_directory.block_bitmaps[directory_group][directory_local / 8] &=
        u8(~(1U << (directory_local % 8)));
    absent_directory.stamp_block_bitmap(directory_group);
    reject(binding, base.replacement("basic-note", "second-note"),
           absent_directory, StatusCode::io_error);
    auto orphan_unchecked = base;
    orphan_unchecked.orphan.independently_checked_empty = false;
    reject(binding, base.replacement("basic-note", "second-note"),
           orphan_unchecked, StatusCode::unsupported);
    auto prior_orphan = base;
    store32(prior_orphan.super, 232, 11);
    store32(prior_orphan.super, 1020, jbd2::detail::crc32c(
        ~u32{0}, std::span<const u8>{prior_orphan.super}.first(1020)));
    reject(binding, base.replacement("basic-note", "second-note"),
           prior_orphan, StatusCode::unsupported);
    auto duplicate = base;
    duplicate.add_alias(15, "second-note", "basic-note");
    reject(binding, base.replacement("basic-note", "second-note"),
           duplicate, StatusCode::io_error);
    auto malformed = base;
    store16(malformed.directory, base.entry("basic-note") + 4, 3);
    malformed.stamp_directory();
    reject(binding, base.replacement("basic-note", "second-note"),
           malformed, StatusCode::io_error);
    auto same_bad_links = same;
    store16(same_bad_links.source, slot(14) + 26, 1);
    same_bad_links.stamp_inode(same_bad_links.source, 14);
    same_bad_links.destination = same_bad_links.source;
    reject(binding, same_bad_links.replacement("basic-note", "same-note"),
           same_bad_links, StatusCode::io_error);
    auto short_ring = binding;
    short_ring.layout.journal_blocks = 6;
    reject(short_ring, base.replacement("basic-note", "second-note"),
           base, StatusCode::unsupported);
    reject(binding, base.replacement("basic-note", "basic-note"), base, StatusCode::invalid_argument);
    CHECK(negative == 17);

    std::printf("PASS pure namespace replacement/unlink/no-op images negative=%zu\n", negative);
}
