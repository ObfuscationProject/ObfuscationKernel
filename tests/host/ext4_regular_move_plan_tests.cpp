#include "ok/fs/ext4_regular_move_plan.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "regular move plan model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Block = Ext4RegularMoveBlock;
using namespace ext4_checked;
void put16(std::span<u8> bytes, usize at, u16 value)
{ bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
void put32(std::span<u8> bytes, usize at, u32 value)
{ for (usize byte = 0; byte < 4; ++byte) bytes[at + byte] = u8(value >> (8 * byte)); }
usize slot(u32 number) { return ((number - 1) % 4) * 256; }

// Synthetic checksummed images exercise planning only. There is no device,
// fixture command, mounted VFS, journal emission, replay or publication here.
struct Fixture
{
    Ext4RecoveryInfo binding{};
    Block super{}, descriptors{}, from_directory{}, to_directory{};
    std::array<Block, 8> tables{};
    std::array<Block, 4> inode_bitmaps{}, block_bitmaps{};
    Ext4RegularMoveEmptyOrphanAuthorization orphan{};
    u32 from{}, to{}, source{}, seed{};
    static constexpr u32 from_block = 600, to_block = 601, file_block = 602;

    explicit Fixture(unsigned sharing = 0)
    {
        from = sharing == 0 ? 2 : sharing == 1 ? 11 : 13;
        to = sharing == 0 ? 11 : sharing == 1 ? 12 : 14;
        source = sharing == 2 ? 15 : 13;
        binding.layout.block_size = 1024; binding.layout.filesystem_blocks = 32768;
        binding.layout.journal_first_block = 1000; binding.layout.journal_blocks = 1024;
        binding.journal_inode = 8; binding.metadata_count = 16;
        binding.empty_log = true;
        for (usize index = 0; index < binding.uuid.size(); ++index) binding.uuid[index] = u8(index + 7);
        seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        orphan = {binding.uuid, 0, true};
        put32(super, 0, 8192); put32(super, 4, 32768); put32(super, 20, 1);
        put32(super, 32, 8192); put32(super, 40, 2048); put16(super, 56, 0xef53);
        put32(super, 84, 11); put16(super, 88, 256); put32(super, 92, 4);
        put32(super, 96, 0x42); put32(super, 100, 0x400); put32(super, 224, 8); super[373] = 1;
        for (usize index = 0; index < binding.uuid.size(); ++index) super[104 + index] = binding.uuid[index];
        for (u32 group = 0; group < 4; ++group)
        {
            const u32 begin = 1 + group * 8192;
            binding.metadata[group * 4] = {begin, 2};
            binding.metadata[group * 4 + 1] = {begin + 2, 1};
            binding.metadata[group * 4 + 2] = {begin + 3, 1};
            binding.metadata[group * 4 + 3] = {begin + 4, 512};
            put32(descriptors, group * 32, begin + 2);
            put32(descriptors, group * 32 + 4, begin + 3);
            put32(descriptors, group * 32 + 8, begin + 4);
            inode_bitmaps[group].fill(0xff); block_bitmaps[group].fill(0xff);
            refresh_bitmaps(group);
        }
        put32(super, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super}.first(1020)));
        initialize_inode(from, true, from_block);
        initialize_inode(to, true, to_block);
        initialize_inode(source, false, file_block);
        initialize_directory(from_directory, from, 2);
        initialize_directory(to_directory, to, 2);
        usize source_offset = 24;
        if (from == 2)
        {
            put16(node(from), 26, 3);
            entry(from_directory, 24, to, 16, "inbox", 2);
            source_offset = 40;
        }
        entry(from_directory, source_offset, source, u16(1012 - source_offset), "seed", 1);
        refresh_inode(from); refresh_inode(to); refresh_inode(source);
        refresh_directory(true); refresh_directory(false);
    }
    static u32 table_block(u32 number) { return 5 + (number - 1) / 4; }
    Block &table(u32 number) { return tables[(number - 1) / 4]; }
    const Block &table(u32 number) const { return tables[(number - 1) / 4]; }
    std::span<u8> node(u32 number) { return std::span<u8>{table(number)}.subspan(slot(number), 256); }
    void initialize_inode(u32 number, bool directory, u32 physical)
    {
        auto inode = node(number);
        put16(inode, 0, directory ? 0040755 : 0100640); put16(inode, 2, 1000); put16(inode, 24, 1001);
        put32(inode, 4, 1024); put32(inode, 8, 1700000000); put32(inode, 12, 1700000001);
        put32(inode, 16, 1700000002); put16(inode, 26, directory ? 2 : 1);
        put32(inode, 28, 2); put32(inode, 32, 0x80000); put32(inode, 36, 5);
        put16(inode, 40, 0xf30a); put16(inode, 42, 1); put16(inode, 44, 4);
        put16(inode, 56, 1); put32(inode, 60, physical); put32(inode, 100, number + 100);
        put16(inode, 128, 32); put32(inode, 132, 4); put32(inode, 136, 8); put32(inode, 140, 12);
    }
    static void entry(Block &block, usize offset, u32 inode, u16 record, std::string_view name, u8 type)
    {
        for (usize at = offset; at < offset + record; ++at) block[at] = 0;
        put32(block, offset, inode); put16(block, offset + 4, record);
        block[offset + 6] = u8(name.size()); block[offset + 7] = type;
        for (usize at = 0; at < name.size(); ++at) block[offset + 8 + at] = u8(name[at]);
    }
    static void initialize_directory(Block &block, u32 number, u32 parent)
    {
        entry(block, 0, number, 12, ".", 2); entry(block, 12, parent, 12, "..", 2);
        entry(block, 24, 0, 988, {}, 0); put16(block, 1016, 12); block[1019] = 0xde;
    }
    void refresh_inode(u32 number)
    {
        auto inode = node(number); inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(inode_seed(seed, number, inode), inode);
        put16(inode, 124, u16(crc)); put16(inode, 130, u16(crc >> 16));
    }
    void refresh_directory(bool from_side)
    {
        const u32 number = from_side ? from : to;
        auto &directory = from_side ? from_directory : to_directory;
        put32(directory, 1020, jbd2::detail::crc32c(inode_seed(seed, number, node(number)),
            std::span<const u8>{directory}.first(1012)));
    }
    void refresh_bitmaps(u32 group)
    {
        const usize at = group * 32;
        put16(descriptors, at + 24, u16(jbd2::detail::crc32c(seed, block_bitmaps[group])));
        put16(descriptors, at + 26, u16(jbd2::detail::crc32c(seed, std::span<const u8>{inode_bitmaps[group]}.first(256))));
        put16(descriptors, at + 30, 0);
        put16(descriptors, at + 30, u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed, encoded(group)),
            std::span<const u8>{descriptors}.subspan(at, 32))));
    }
    Ext4RegularMoveSource images() const
    {
        return {table_block(from), table_block(to), table_block(source), from_block, to_block,
            super, descriptors, table(from), table(to), table(source), from_directory, to_directory,
            inode_bitmaps, block_bitmaps, orphan};
    }
    Ext4RegularMoveUpdate update(std::string_view new_name = "note") const
    { return {from, to, source, from + 100, to + 100, source + 100, "seed", new_name, 1700000999, 123456789}; }
    Status plan(Ext4RegularMovePlan &output) const
    { return Ext4RegularMovePlanner::plan(binding, update(), images(), output); }
};

usize home(const Ext4RegularMovePlan &plan, u32 target)
{
    for (usize index = 0; index < plan.count; ++index) if (plan.targets[index] == target) return index;
    CHECK(false); return 0;
}
usize find(const Block &directory, std::string_view name)
{
    for (usize at = 0; at < 1012; at += le16(directory, at + 4))
        if (le32(directory, at) && name == std::string_view{
            reinterpret_cast<const char *>(directory.data() + at + 8), directory[at + 6]}) return at;
    return 1012;
}
bool changed_inode_byte(usize byte, bool parent)
{
    return (byte >= 12 && byte < (parent ? 20U : 16U)) || (byte >= 36 && byte < 40) ||
        byte == 124 || byte == 125 || byte == 130 || byte == 131 ||
        (byte >= 132 && byte < (parent ? 140U : 136U)) || (byte >= 152 && byte < 156);
}
void complete(unsigned sharing)
{
    Fixture f(sharing); const auto old_tables = f.tables;
    const auto old_from = f.from_directory, old_to = f.to_directory;
    const auto old_super = f.super, old_descriptors = f.descriptors;
    const auto old_inodes = f.inode_bitmaps, old_blocks = f.block_bitmaps;
    Ext4RegularMovePlan plan{}; CHECK(f.plan(plan));
    CHECK(plan.count == 5 - sharing && plan.targets[0] == f.from_block && plan.targets[1] == f.to_block);
    CHECK(plan.allocation_unchanged && plan.file_identity_retained && plan.source_link_count_unchanged);
    CHECK(plan.source == f.source && plan.source_generation == f.source + 100);
    CHECK(plan.retained_data_count == 1 && plan.retained_data_blocks[0] == f.file_block);
    CHECK(plan.before[0] == old_from && plan.before[1] == old_to);
    CHECK(find(plan.after[0], "seed") == 1012 && find(plan.after[1], "note") == plan.destination_entry_offset);
    CHECK(le32(plan.after[1], plan.destination_entry_offset) == f.source);
    CHECK(std::string_view(plan.old_name.data(), plan.old_name_length) == "seed");
    CHECK(std::string_view(plan.new_name.data(), plan.new_name_length) == "note");
    for (u32 number : {f.from, f.to, f.source})
    {
        const auto index = home(plan, Fixture::table_block(number));
        CHECK(plan.before[index] == f.table(number));
        const auto before = std::span<const u8>{plan.before[index]}.subspan(slot(number), 256);
        const auto after = std::span<const u8>{plan.after[index]}.subspan(slot(number), 256);
        CHECK(inode_checksum(f.seed, number, after));
        CHECK(le32(after, 100) == le32(before, 100) && le16(after, 26) == le16(before, 26));
        CHECK(le32(after, 12) == f.update().seconds && le32(after, 132) == (f.update().nanoseconds << 2));
        CHECK(le32(after, 36) == 6 && le32(after, 152) == 0);
        for (usize byte = 0; byte < 256; ++byte)
            if (!changed_inode_byte(byte, number != f.source)) CHECK(before[byte] == after[byte]);
    }
    for (usize index = 2; index < plan.count; ++index)
        for (usize byte = 0; byte < 1024; ++byte)
        {
            const u32 number = (plan.targets[index] - 5) * 4 + u32(byte / 256) + 1;
            if (number != f.from && number != f.to && number != f.source)
                CHECK(plan.before[index][byte] == plan.after[index][byte]);
        }
    for (unsigned side = 0; side < 2; ++side)
    {
        const u32 number = side ? f.to : f.from;
        const auto inode = std::span<const u8>{plan.after[home(plan, Fixture::table_block(number))]}.subspan(slot(number), 256);
        CHECK(le32(plan.after[side], 1020) == jbd2::detail::crc32c(inode_seed(f.seed, number, inode),
            std::span<const u8>{plan.after[side]}.first(1012)));
    }
    CHECK(f.tables == old_tables && f.from_directory == old_from && f.to_directory == old_to);
    CHECK(f.super == old_super && f.descriptors == old_descriptors && f.inode_bitmaps == old_inodes && f.block_bitmaps == old_blocks);
}
void reject(Fixture &fixture, StatusCode expected)
{
    Ext4RegularMovePlan output{}; output.count = 5;
    const auto status = fixture.plan(output);
    CHECK(!status && status.code() == expected && output.count == 0);
}
void refusals()
{
    Fixture bad_source; bad_source.table(bad_source.source)[slot(bad_source.source) + 124] ^= 1;
    reject(bad_source, StatusCode::io_error);
    Fixture bad_from; bad_from.from_directory[1020] ^= 1; reject(bad_from, StatusCode::io_error);
    Fixture bad_to; bad_to.to_directory[1020] ^= 1; reject(bad_to, StatusCode::io_error);
    Fixture links; put16(links.node(links.source), 26, 2); links.refresh_inode(links.source); reject(links, StatusCode::unsupported);
    Fixture alias; put32(alias.node(alias.source), 60, alias.to_block); alias.refresh_inode(alias.source); reject(alias, StatusCode::unsupported);
    Fixture unallocated_inode;
    unallocated_inode.inode_bitmaps[0][(unallocated_inode.source - 1) / 8] &= u8(~(1U << ((unallocated_inode.source - 1) % 8)));
    unallocated_inode.refresh_bitmaps(0); reject(unallocated_inode, StatusCode::io_error);
    Fixture unallocated_directory;
    unallocated_directory.block_bitmaps[0][(Fixture::to_block - 1) / 8] &= u8(~(1U << ((Fixture::to_block - 1) % 8)));
    unallocated_directory.refresh_bitmaps(0); reject(unallocated_directory, StatusCode::io_error);
    Fixture occupied; Fixture::entry(occupied.to_directory, 24, 99, 988, "note", 1);
    occupied.refresh_directory(false); reject(occupied, StatusCode::already_exists);
    Fixture malformed; put16(malformed.to_directory, 28, 7); malformed.refresh_directory(false); reject(malformed, StatusCode::io_error);
    Fixture duplicate;
    Fixture::entry(duplicate.to_directory, 24, 99, 12, "same", 1);
    Fixture::entry(duplicate.to_directory, 36, 100, 976, "same", 1);
    duplicate.refresh_directory(false); reject(duplicate, StatusCode::io_error);
    Fixture stale;
    auto changed = stale.update(); ++changed.to_generation; Ext4RegularMovePlan out{};
    CHECK(Ext4RegularMovePlanner::plan(stale.binding, changed, stale.images(), out).code() == StatusCode::io_error && !out.count);
    for (unsigned selected = 0; selected < 3; ++selected)
    {
        Fixture overflow;
        const u32 number = selected == 0 ? overflow.from : selected == 1 ? overflow.to : overflow.source;
        put32(overflow.node(number), 36, ~u32{0}); put32(overflow.node(number), 152, ~u32{0});
        overflow.refresh_inode(number); reject(overflow, StatusCode::overflow);
    }
    Fixture invalid;
    for (std::string_view name : {std::string_view{"."}, std::string_view{".."}, std::string_view{"bad/name"}, std::string_view{}})
    {
        changed = invalid.update(name); out.count = 4;
        CHECK(Ext4RegularMovePlanner::plan(invalid.binding, changed, invalid.images(), out).code() == StatusCode::invalid_argument && !out.count);
    }
    changed = invalid.update(); changed.to_parent = changed.from_parent;
    CHECK(Ext4RegularMovePlanner::plan(invalid.binding, changed, invalid.images(), out).code() == StatusCode::invalid_argument);
    Fixture shared(2); const auto images = shared.images(); Block inconsistent = shared.table(shared.to); inconsistent[900] ^= 1;
    const Ext4RegularMoveSource inconsistent_view{
        images.from_parent_table_block, images.to_parent_table_block, images.source_table_block,
        images.from_directory_block, images.to_directory_block, images.superblock, images.descriptors,
        images.from_parent_table, inconsistent, images.source_table, images.from_directory, images.to_directory,
        images.inode_bitmaps, images.block_bitmaps, images.orphan};
    CHECK(Ext4RegularMovePlanner::plan(shared.binding, shared.update(), inconsistent_view, out).code() == StatusCode::io_error && !out.count);

    Fixture full;
    for (usize index = 0; index < 81; ++index)
    {
        const std::array<char, 3> name{{'n', char('0' + index / 10), char('0' + index % 10)}};
        Fixture::entry(full.to_directory, 24 + index * 12, u32(100 + index), 12,
            std::string_view{name.data(), name.size()}, 1);
    }
    Fixture::entry(full.to_directory, 996, 200, 16, "lastseed", 1); full.refresh_directory(false);
    const auto long_update = full.update("this-name-needs-a-larger-record");
    CHECK(Ext4RegularMovePlanner::plan(full.binding, long_update, full.images(), out).code() == StatusCode::no_space && !out.count);

    Fixture output_alias;
    out.count = 5; out.before[0] = output_alias.from_directory;
    const auto preserved_alias = out.before[0];
    const auto normal = output_alias.images();
    const Ext4RegularMoveSource alias_view{
        normal.from_parent_table_block, normal.to_parent_table_block, normal.source_table_block,
        normal.from_directory_block, normal.to_directory_block, normal.superblock, normal.descriptors,
        normal.from_parent_table, normal.to_parent_table, normal.source_table, out.before[0], normal.to_directory,
        normal.inode_bitmaps, normal.block_bitmaps, normal.orphan};
    CHECK(Ext4RegularMovePlanner::plan(output_alias.binding, output_alias.update(), alias_view, out).code() == StatusCode::invalid_argument);
    CHECK(out.count == 5 && out.before[0] == preserved_alias);
}

void destination_slack_and_owned_names()
{
    Fixture fixture;
    Fixture::entry(fixture.to_directory, 12, 2, 1000, "..", 2);
    fixture.refresh_directory(false);
    std::array<char, 4> old_name{{'s', 'e', 'e', 'd'}}, new_name{{'n', 'o', 't', 'e'}};
    auto update = fixture.update(); update.old_name = std::string_view{old_name.data(), old_name.size()};
    update.new_name = std::string_view{new_name.data(), new_name.size()};
    Ext4RegularMovePlan output{};
    CHECK(Ext4RegularMovePlanner::plan(fixture.binding, update, fixture.images(), output));
    CHECK(output.destination_entry_offset == 24 && le16(output.after[1], 16) == 12);
    old_name.fill('x'); new_name.fill('x');
    CHECK((std::string_view{output.old_name.data(), output.old_name_length} == "seed"));
    CHECK((std::string_view{output.new_name.data(), output.new_name_length} == "note"));

    Fixture same_name;
    CHECK(Ext4RegularMovePlanner::plan(same_name.binding, same_name.update("seed"), same_name.images(), output));
    CHECK(find(output.after[0], "seed") == 1012 && find(output.after[1], "seed") != 1012);

    Fixture empty;
    auto inode = empty.node(empty.source);
    put32(inode, 4, 0); put32(inode, 28, 0); put16(inode, 42, 0);
    for (usize at = 52; at < 100; ++at) inode[at] = 0;
    empty.refresh_inode(empty.source);
    CHECK(empty.plan(output) && output.retained_data_count == 0 && output.file_identity_retained);
}
} // namespace

int main()
{
    complete(0); complete(1); complete(2); refusals(); destination_slack_and_owned_names();
    std::puts("ext4 cross-parent regular move pure-plan model: pass");
}
