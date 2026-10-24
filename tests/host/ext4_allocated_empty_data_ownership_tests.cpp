#include "ext4_zero_truncate_test_support.hpp"
#include <ok/fs/ext4_allocated_empty_data_ownership.hpp>
#include <cstdio>
#include <cstdlib>
using namespace zero_fixture;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "allocated empty ownership:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)
namespace
{
std::array<Block, 8193> disk{}, saved{};
struct Image
{
    Fixture f;
    std::array<Block, 8193> &blocks{disk};
    explicit Image(u32 count = 1)
    {
        blocks.fill(Block{});
        blocks[1] = f.superblock; blocks[2] = f.descriptors;
        blocks[3] = f.bitmap; blocks[4] = f.inode_bitmap; blocks[8] = f.inode_table;
        for (u32 number : {2U, 8U}) blocks[4][(number - 1) / 8] |= u8(1U << ((number - 1) % 8));
        const auto make = [&](u32 number, u16 mode, u32 size, u32 length, u32 physical, u16 links)
        {
            auto raw = inode(number);
            for (auto &byte : raw) byte = 0;
            put16(raw, 0, mode); put16(raw, 26, links);
            put32(raw, 4, size); put32(raw, 28, length * 2); put32(raw, 32, 0x80000);
            put16(raw, 40, 0xf30a); put16(raw, 42, length ? 1 : 0); put16(raw, 44, 4);
            put16(raw, 56, u16(length)); put32(raw, 60, physical); put16(raw, 128, 32);
            stamp_inode(raw, number, f.seed);
        };
        make(2, 0040755, 1024, 1, 71, 2);
        make(8, 0100600, 1048576, 1024, 1000, 1);
        make(13, 0100640, count ? (count - 1) * 1024 + 1 : 0, count, count ? 72 : 0, 1);
        put32(inode(13), 100, 9); put32(inode(13), 36, 42);
        stamp_inode(inode(13), 13, f.seed);
        auto &directory = blocks[71];
        put32(directory, 0, 2); put16(directory, 4, 12); directory[6] = 1; directory[7] = 2; directory[8] = '.';
        put32(directory, 12, 2); put16(directory, 16, 1000); directory[18] = 2; directory[19] = 2;
        directory[20] = directory[21] = '.';
        put16(directory, 1016, 12); directory[1019] = 0xde;
        put32(directory, 1020, jbd2::detail::crc32c(
            ext4_checked::inode_seed(f.seed, 2, inode(2)), std::span<const u8>{directory}.first(1012)));
        refresh();
    }
    std::span<u8, 256> inode(u32 number)
    { return std::span<u8, 256>{blocks[5 + (number - 1) / 4].data() + ((number - 1) % 4) * 256, 256}; }
    void refresh()
    {
        u32 free = 0;
        for (u32 bit = 0; bit < 256; ++bit)
            if (!(blocks[4][bit / 8] & (1U << (bit % 8)))) ++free;
        put16(blocks[2], 14, u16(free)); put16(blocks[2], 16, 1);
        put32(blocks[1], 16, free);
        put16(blocks[2], 26, u16(jbd2::detail::crc32c(f.seed, std::span<const u8>{blocks[4]}.first(32))));
        free = 0;
        for (u32 bit = 0; bit < 8192; ++bit)
            if (!(blocks[3][bit / 8] & (1U << (bit % 8)))) ++free;
        put16(blocks[2], 12, u16(free)); put32(blocks[1], 12, free);
        put16(blocks[2], 24, u16(jbd2::detail::crc32c(f.seed, blocks[3])));
        stamp_descriptor(std::span<u8>{blocks[2]}.first(32), 0, f.seed);
        put32(blocks[1], 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{blocks[1]}.first(1020)));
    }
    void duplicate(u32 number, bool direct = false)
    {
        const auto source = inode(13); auto target = inode(number);
        for (usize at = 0; at < 256; ++at) target[at] = source[at];
        if (direct)
        {
            put16(target, 0, 0120777); put32(target, 4, 60); put32(target, 32, 0);
            for (usize at = 40; at < 100; ++at) target[at] = 0;
            put32(target, 40, 72);
        }
        stamp_inode(target, number, f.seed);
        blocks[4][(number - 1) / 8] |= u8(1U << ((number - 1) % 8)); refresh();
    }
    Ext4ZeroTruncateUpdate update()
    { return {13, 9, ext4_checked::le32(inode(13), 4), 1, 0}; }
};
struct Reader
{
    Image &image;
    usize reads{}, fail_at{}, target_reads{}, change_at{}, drift_at{};
    Status read_block(u32 number, std::span<u8> out)
    {
        if (++reads == fail_at) return Status::io_error("injected ownership read failure");
        if (number >= image.blocks.size() || out.size() < 1024) return Status::io_error("ownership model bounds");
        if (number == 8 && ++target_reads == change_at)
        {
            put32(image.inode(13), 100, 10); stamp_inode(image.inode(13), 13, image.f.seed);
        }
        for (usize at = 0; at < 1024; ++at) out[at] = image.blocks[number][at];
        if (reads == drift_at) out[900] ^= 1;
        return {};
    }
};

using View = Ext4ReadOnlyView<Reader>;
Ext4AllocatedEmptyDataScratch planning{};
Ext4AllocatedEmptyDataPlan plan{}, changed{};
Ext4AllocatedEmptyDataOwnershipScratch scratch{};
Ext4AllocatedEmptyDataOwnership proof{};
Ext4ReadOnlyOpenScratch opening{};
usize positive{}, refusal{}, read_faults{}, bookend_drifts{}, baseline_reads{};
void prepare_plan(Image &image, u32 size = 73)
{
    const Ext4AllocatedEmptyDataUpdate update{13, 9, size, 1700000222, 123456789};
    const Ext4AllocatedEmptyDataSource source{0, 70, 8, image.blocks[1], image.blocks[2],
        image.blocks[3], image.blocks[4], image.blocks[8]};
    CHECK(Ext4AllocatedEmptyDataPlanner::plan_off_stack(image.f.binding, update, source, planning, plan));
}
void verify(Image &image, const Ext4AllocatedEmptyDataPlan &candidate, bool accepted)
{
    Reader reader{image}; View view; saved = image.blocks;
    const auto opened = view.open(reader, image.f.binding, opening);
    if (!opened)
    {
        CHECK(!accepted && image.blocks == saved); ++refusal; return;
    }
    const usize before = reader.reads;
    proof.complete = true; proof.inode = 99;
    const auto status = Ext4AllocatedEmptyDataOwnershipValidator::prove(image.f.binding, view,
        candidate, scratch, proof);
    CHECK(bool(status) == accepted && image.blocks == saved);
    if (accepted)
    {
        CHECK(proof.complete && proof.inode == 13 && proof.generation == 9 &&
            proof.physical == 70 && proof.group == 0 && proof.old_version == candidate.old_version);
        baseline_reads = reader.reads - before; ++positive;
    }
    else { CHECK(!proof.complete && !proof.inode && !proof.physical); ++refusal; }
}
void positives()
{
    for (u32 size : {1U, 73U, 1023U, 1024U})
        for (u64 version : {u64{0}, u64{0xffffffff}, u64{0x100000000}, ~u64{0} - 1})
        {
            Image image(0); put32(image.inode(13), 36, u32(version)); put32(image.inode(13), 152, u32(version >> 32));
            stamp_inode(image.inode(13), 13, image.f.seed); prepare_plan(image, size); verify(image, plan, true);
        }
}
void mutations()
{
    for (usize home = 0; home < 4; ++home) for (usize at : {usize{0}, usize{17}, usize{1023}})
    {
        Image image(0); prepare_plan(image); changed = plan; changed.before[home][at] ^= 1; verify(image, changed, false);
        changed = plan; changed.after[home][at] ^= 1; verify(image, changed, false);
    }
    for (u32 field = 0; field < 9; ++field)
    {
        Image image(0); prepare_plan(image); changed = plan;
        if (field == 0) ++changed.update.generation;
        if (field == 1) changed.update.inode = 14;
        if (field == 2) ++changed.old_version;
        if (field == 3) ++changed.new_version;
        if (field == 4) ++changed.physical;
        if (field == 5) ++changed.group;
        if (field == 6) ++changed.targets[0];
        if (field == 7) ++changed.targets[3];
        if (field == 8) changed.count = 3;
        verify(image, changed, false);
    }
    {
        Image image(0); prepare_plan(image);
        // A directory pointing at the nominally free candidate is refused
        // even with valid inode, allocation-bitmap and metadata CRCs.
        put32(image.inode(2), 60, 70); stamp_inode(image.inode(2), 2, image.f.seed);
        image.blocks[70] = image.blocks[71]; prepare_plan(image); verify(image, plan, false);
    }
    {
        Image image(0); prepare_plan(image);
        put32(image.inode(8), 60, 70); stamp_inode(image.inode(8), 8, image.f.seed);
        Reader reader{image}; View view;
        const auto opened = view.open(reader, image.f.binding, opening);
        if (opened)
        {
            CHECK(!Ext4AllocatedEmptyDataOwnershipValidator::prove(image.f.binding, view, plan, scratch, proof));
            CHECK(!proof.complete); ++refusal;
        }
        else ++refusal;
    }
    for (u32 kind = 0; kind < 3; ++kind)
    {
        Image image(0); prepare_plan(image);
        if (kind == 0) image.blocks[3][2 / 8] &= u8(~(1U << (2 % 8)));
        if (kind == 1) image.blocks[3][71 / 8] &= u8(~(1U << (71 % 8)));
        if (kind == 2)
        {
            auto inode = image.inode(14);
            for (auto &byte : inode) byte = 0;
            put16(inode, 0, 0100644); put16(inode, 26, 1); put32(inode, 4, 1); put32(inode, 28, 2);
            put32(inode, 32, 0x80000); put16(inode, 40, 0xf30a); put16(inode, 42, 1); put16(inode, 44, 4);
            put16(inode, 56, 1); put32(inode, 60, 70); put16(inode, 128, 32);
            stamp_inode(inode, 14, image.f.seed); image.blocks[4][1] |= 1U << 5;
        }
        image.refresh();
        if (kind == 2) prepare_plan(image);
        verify(image, plan, false);
    }
    for (usize fault = 1; fault <= baseline_reads; ++fault)
    {
        Image image(0); prepare_plan(image); Reader reader{image}; View view;
        CHECK(view.open(reader, image.f.binding, opening)); reader.fail_at = reader.reads + fault;
        saved = image.blocks;
        CHECK(!Ext4AllocatedEmptyDataOwnershipValidator::prove(image.f.binding, view, plan, scratch, proof));
        CHECK(!proof.complete && !proof.inode && !view.valid() && image.blocks == saved); ++read_faults;
    }
    for (usize when : {usize{1}, usize{2}, usize{3}, usize{4}, usize{5}, usize{6}})
    {
        Image image(0); prepare_plan(image); Reader reader{image}; View view;
        CHECK(view.open(reader, image.f.binding, opening)); reader.target_reads = 0; reader.change_at = when;
        const auto status = Ext4AllocatedEmptyDataOwnershipValidator::prove(image.f.binding, view, plan, scratch, proof);
        if (reader.target_reads >= when) { CHECK(!status && !proof.complete); ++refusal; }
        else { CHECK(status); ++positive; }
    }
    for (usize position = 1; position <= 5; ++position)
    {
        Image image(0); prepare_plan(image); Reader reader{image}; View view;
        CHECK(view.open(reader, image.f.binding, opening));
        reader.drift_at = reader.reads + baseline_reads - 5 + position; saved = image.blocks;
        CHECK(!Ext4AllocatedEmptyDataOwnershipValidator::prove(image.f.binding, view, plan, scratch, proof));
        CHECK(!proof.complete && !proof.inode && !view.valid() && image.blocks == saved); ++bookend_drifts;
    }
    {
        Image image(0); prepare_plan(image); Reader reader{image}; View view;
        CHECK(view.open(reader, image.f.binding, opening)); const auto before = reader.reads;
        scratch.proposed.inode = 99;
        CHECK(!Ext4AllocatedEmptyDataOwnershipValidator::prove(image.f.binding, view, plan, scratch, scratch.proposed));
        CHECK(scratch.proposed.inode == 99 && reader.reads == before); ++refusal;
    }
}
} // namespace
int main()
{
    positives(); mutations();
    std::printf("PASS hosted allocated-empty ownership positive=%zu refusal=%zu read_faults=%zu bookend_drifts=%zu baseline_reads=%zu\n",
        positive, refusal, read_faults, bookend_drifts, baseline_reads);
    return 0;
}
