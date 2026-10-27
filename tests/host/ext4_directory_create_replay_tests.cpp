#include "ext4_owned_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <memory>

using namespace owned_fixture;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL directory replay:%d: %s\n", __LINE__, #expression); std::exit(1); \
} } while (false)

namespace
{
using Block = Ext4CreateBlock;
using Owner = Ext4OwnedTransaction<1, 9>;
constexpr Ext4CreateUpdate directory_update{2, 1700000111, 123456789, "proof-directory",
    {NodeType::directory, 02750, 0x10001, 0x20002}};
constexpr Ext4CreateUpdate regular_update{2, 1700000111, 123456789, "proof-file",
    {NodeType::regular, 0640, 1000, 1001}};

void store16(Block &block, usize at, u16 value)
{ block[at] = u8(value); block[at + 1] = u8(value >> 8); }
void store32(Block &block, usize at, u32 value)
{ for (usize byte = 0; byte < 4; ++byte) block[at + byte] = u8(value >> (byte * 8)); }
void store_big32(Block &block, usize at, u32 value)
{ for (usize byte = 0; byte < 4; ++byte) block[at + byte] = u8(value >> ((3 - byte) * 8)); }
u32 independent_crc(u32 initial, std::span<const u8> bytes)
{ return ext2fs_crc32c_le(initial, bytes.data(), bytes.size()); }
u32 independent_inode_seed(u32 filesystem_seed, u32 number, std::span<const u8> inode)
{
    const std::array<u8, 4> encoded{u8(number), u8(number >> 8), u8(number >> 16), u8(number >> 24)};
    return independent_crc(independent_crc(filesystem_seed, encoded), inode.subspan(100, 4));
}
void stamp_inode(Block &block, usize offset, u32 number, u32 filesystem_seed)
{
    auto inode = std::span<u8>{block}.subspan(offset, 256);
    inode[124] = inode[125] = inode[130] = inode[131] = 0;
    const u32 checksum = independent_crc(independent_inode_seed(filesystem_seed, number, inode), inode);
    store16(block, offset + 124, u16(checksum)); store16(block, offset + 130, u16(checksum >> 16));
}
void stamp_group(Block &block, u32 group, u32 filesystem_seed)
{
    const std::array<u8, 4> encoded{u8(group), u8(group >> 8), u8(group >> 16), u8(group >> 24)};
    store16(block, group * 32 + 30, 0);
    store16(block, group * 32 + 30, u16(independent_crc(
        independent_crc(filesystem_seed, encoded), std::span<const u8>{block}.subspan(group * 32, 32))));
}
void load_block(const Bytes &image, usize offset, Block &block)
{
    CHECK(offset <= image.size() && block.size() <= image.size() - offset);
    std::copy_n(image.begin() + offset, block.size(), block.begin());
}
void write_block(Bytes &image, usize offset, const Block &block)
{
    CHECK(offset <= image.size() && block.size() <= image.size() - offset);
    std::copy(block.begin(), block.end(), image.begin() + offset);
}

struct Published
{
    usize first{}, count{};
    u32 inode{};
    std::array<u32, 8> targets{};
};
Published inspect(const Bytes &image, bool directory)
{
    Published out{};
    out.first = journal(image);
    CHECK(be32(image, out.first + 28) == 1);
    for (usize index = 0; index < out.targets.size(); ++index)
    {
        const usize tag = out.first + 1024 + (index ? 44 + (index - 1) * 16 : 12);
        out.targets[index] = be32(image, tag);
        CHECK(out.targets[index] < image.size() / 1024);
        if (be32(image, tag + 4) & jbd2::flag_last_tag) { out.count = index + 1; break; }
    }
    CHECK(directory ? (out.count == 7 || out.count == 8) : (out.count == 5 || out.count == 6));
    if (directory) out.inode = r32(image, out.first + 1024 + 180);
    return out;
}
Bytes publish(const Bytes &dirty, Ext4CreateUpdate update)
{
    Disk disk(dirty); Owner::Registry registry;
    const auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, dirty.size() / 512}));
    const auto prepared = owner->prepare_create(update);
    if (!prepared) std::fprintf(stderr, "directory publish prepare: %.*s\n",
        static_cast<int>(prepared.message().size()), prepared.message().data());
    CHECK(prepared);
    const auto committed = owner->commit();
    if (!committed) std::fprintf(stderr, "directory publish commit: %.*s\n",
        static_cast<int>(committed.message().size()), committed.message().data());
    CHECK(committed);
    CHECK(disk.live == disk.durable);
    const auto published = disk.durable;
    // Retire the actual owner through its public API after retaining the image.
    CHECK(owner->checkpoint() && owner->close());
    return published;
}
void accepted(const Bytes &published, const Published &profile, Ext4ReplayPolicy policy,
              std::string_view name, NodeType type)
{
    Disk disk(published);
    auto completion = std::make_unique<Ext4RecoveryCompletion<8, 2>>(false, true, policy);
    CHECK(completion->run(disk, {0, published.size() / 512}));
    CHECK(completion->report().complete() && completion->report().validated_actions == profile.count);
    CHECK(disk.writes == profile.count + 1 && disk.flushes == 5 && disk.live == disk.durable);
    CHECK(be32(disk.live, profile.first + 28) == 0);
    auto view = std::make_unique<Ext4ReplayPreview<8, 2>>(false, true, policy);
    CHECK(view->open(disk, {0, disk.live.size() / 512}));
    const auto created = view->lookup(2, name); CHECK(created);
    const auto node = view->inode(created.value()); CHECK(node);
    CHECK((node.value().mode & 0170000) == (type == NodeType::directory ? 0040000 : 0100000));
    CHECK(view->lookup(2, "seed").value() == 12);
}
void rejected(const Bytes &image, Ext4ReplayPolicy policy, bool canonical)
{
    Disk disk(image);
    auto completion = std::make_unique<Ext4RecoveryCompletion<8, 2>>(false, true, policy);
    const auto status = completion->run(disk, {0, image.size() / 512});
    CHECK(!status);
    if (canonical)
        CHECK(status.message() == "ext4 directory create witnessed image differs from canonical plan");
    CHECK(!disk.writes && !disk.flushes && !completion->report().write_attempts &&
          !completion->report().flush_attempts && disk.live == image && disk.durable == image);
}

enum class Corruption { dot, dotdot, mode, links, parent_links, used_directories, free_blocks };
Bytes semantic_corruption(const Bytes &published, const Published &profile, Corruption corruption)
{
    using namespace ext4_checked;
    Bytes out = published;
    Block descriptor{};
    load_block(published, profile.first + 1024, descriptor);
    Ext4CreatePlan plan{};
    plan.count = profile.count;
    for (usize home = 0; home < profile.count; ++home)
    {
        plan.targets[home] = profile.targets[home];
        load_block(published, profile.first + (home + 2) * 1024, plan.after[home]);
        const usize tag = home ? 44 + (home - 1) * 16 : 12;
        if (jbd2::detail::be32(descriptor, tag + 4) & jbd2::flag_escape)
            store_big32(plan.after[home], 0, jbd2::magic);
    }
    Ext4DirectoryCreateWitness witness{};
    CHECK(Ext4DirectoryCreateWitnessCodec::decode(descriptor,
        std::span<const Block>{plan.after}.first(plan.count),
        std::span<Block>{plan.before}.first(plan.count), witness));
    plan.inode = witness.inode; plan.parent = witness.update.parent;
    plan.directory_block = profile.targets[plan.count - 1];
    plan.allocated_directory_block = witness.physical;
    plan.block_group = (witness.physical - 1) / r32(published, 1056);
    const usize tables = plan.count - 6, inode_bitmap = tables;
    const usize descriptors = inode_bitmap + 1, superblock = inode_bitmap + 2;
    const usize data = plan.count - 2;
    const usize inode_table = tables == 2 ? 1 : 0;
    const usize new_offset = ((plan.inode - 1) % 4) * 256;
    const usize parent_offset = ((plan.parent - 1) % 4) * 256;
    const u32 filesystem_seed = fixture::seed(published);
    switch (corruption)
    {
        case Corruption::dot: store32(plan.after[data], 0, plan.parent); break;
        case Corruption::dotdot: store32(plan.after[data], 12, plan.inode); break;
        case Corruption::mode: store16(plan.after[inode_table], new_offset, 0042770); break;
        case Corruption::links: store16(plan.after[inode_table], new_offset + 26, 3); break;
        case Corruption::parent_links:
            store16(plan.after[0], parent_offset + 26, u16(le16(plan.after[0], parent_offset + 26) + 1)); break;
        case Corruption::used_directories:
        {
            const u32 group = (plan.inode - 1) / r32(published, 1064);
            store16(plan.after[descriptors], group * 32 + 16,
                u16(le16(plan.after[descriptors], group * 32 + 16) + 1));
            stamp_group(plan.after[descriptors], group, filesystem_seed); break;
        }
        case Corruption::free_blocks:
            store32(plan.after[superblock], 12, le32(plan.after[superblock], 12) + 1);
            store32(plan.after[superblock], 1020, independent_crc(~u32{0},
                std::span<const u8>{plan.after[superblock]}.first(1020))); break;
    }
    stamp_inode(plan.after[0], parent_offset, plan.parent, filesystem_seed);
    stamp_inode(plan.after[inode_table], new_offset, plan.inode, filesystem_seed);
    const auto inode = std::span<const u8>{plan.after[inode_table]}.subspan(new_offset, 256);
    store32(plan.after[data], 1020, independent_crc(
        independent_inode_seed(filesystem_seed, plan.inode, inode),
        std::span<const u8>{plan.after[data]}.first(1012)));
    // Keep the complete witness internally valid with exact original preimages.
    // Refusal must therefore reach canonical transition comparison, not merely
    // detect stale payload/descriptor CRCs or stale reversible witness patches.
    CHECK(Ext4DirectoryCreateWitnessCodec::encode(witness.update, plan, descriptor));
    std::array<Block, 8> recovered{};
    Ext4DirectoryCreateWitness decoded{};
    CHECK(Ext4DirectoryCreateWitnessCodec::decode(descriptor,
        std::span<const Block>{plan.after}.first(plan.count),
        std::span<Block>{recovered}.first(plan.count), decoded));
    for (usize home = 0; home < plan.count; ++home) CHECK(recovered[home] == plan.before[home]);
    const auto sequence = std::span<const u8>{descriptor}.subspan(8, 4);
    for (usize home = 0; home < plan.count; ++home)
    {
        const usize tag = home ? 44 + (home - 1) * 16 : 12;
        auto payload = plan.after[home];
        u32 flags = jbd2::detail::be32(descriptor, tag + 4) & ~jbd2::flag_escape;
        if (jbd2::detail::be32(payload, 0) == jbd2::magic)
        { store_big32(payload, 0, 0); flags |= jbd2::flag_escape; }
        store_big32(descriptor, tag + 4, flags);
        store_big32(descriptor, tag + 12,
            independent_crc(independent_crc(filesystem_seed, sequence), payload));
        write_block(out, profile.first + (home + 2) * 1024, payload);
    }
    store_big32(descriptor, 1020, 0);
    store_big32(descriptor, 1020, independent_crc(filesystem_seed, descriptor));
    write_block(out, profile.first + 1024, descriptor);
    journal_record_crc(out, profile.first + (plan.count + 2) * 1024, 16);
    return out;
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 2 || argc == 3);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes dirty((std::istreambuf_iterator<char>(input)), {});
    CHECK(dirty.size() == 32 * 1024 * 1024);
    if (argc == 3) { CHECK(std::string_view(argv[2]) == "revoke"); owned_fixture::optional_revoke_fixture(dirty); }
    const usize bitmap = r32(dirty, descriptor(dirty, 0) + 4) * 1024;
    for (u32 number : {14U, 15U})
    {
        CHECK(!(dirty[bitmap + (number - 1) / 8] & (1U << ((number - 1) % 8))));
        std::fill_n(dirty.begin() + inode_offset(dirty, number), 256, 0);
    }
    w32(dirty, 1120, r32(dirty, 1120) | 4U); super_crc(dirty);
    const Bytes directory = publish(dirty, directory_update);
    const auto directory_profile = inspect(directory, true);
    accepted(directory, directory_profile, Ext4ReplayPolicy::bounded_directory_create,
        directory_update.name, NodeType::directory);
    usize semantic_refusals{};
    for (const auto corruption : {Corruption::dot, Corruption::dotdot, Corruption::mode,
         Corruption::links, Corruption::parent_links, Corruption::used_directories, Corruption::free_blocks})
    {
        rejected(semantic_corruption(directory, directory_profile, corruption),
            Ext4ReplayPolicy::bounded_directory_create, true);
        ++semantic_refusals;
    }
    Bytes wrong_count = directory;
    wrong_count[directory_profile.first + 1024 + 172] = u8(directory_profile.count - 1);
    journal_record_crc(wrong_count, directory_profile.first + 1024, 1020);
    rejected(wrong_count, Ext4ReplayPolicy::bounded_directory_create, false);
    const Bytes regular = publish(dirty, regular_update);
    const auto regular_profile = inspect(regular, false);
    accepted(regular, regular_profile, Ext4ReplayPolicy::directory_create, regular_update.name, NodeType::regular);
    rejected(regular, Ext4ReplayPolicy::bounded_directory_create, false);
    rejected(directory, Ext4ReplayPolicy::directory_create, false);
    std::printf("DIRECTORY_REPLAY_SEMANTICS_PASS public_api=1 canonical_refusals=%zu zero_write_refusals=10 regular_control=1 optional_revoke=%u\n",
        semantic_refusals, argc == 3 ? 1U : 0U);
}
