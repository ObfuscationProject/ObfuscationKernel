// Actual canonical owner/cache/session composition over an existing fixture.
// Hosted synchronous device models do not qualify hardware or an OS workflow.
#include "ext4_mounted_support.hpp"
#include <ok/fs/ext4_regular_move_transaction.hpp>
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>
#include <type_traits>
using namespace owned_fixture;
using Block = Ext4RegularMoveBlock;
using Owner = Ext4OwnedTransaction<1, 9>;
using Mount = Ext4MountedMetadata<1>;
namespace
{
usize checks{}, mixtures{}, refusals{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "owned regular move:%d: %s\n", __LINE__, #expression); std::abort(); \
} } while (false)
template <typename T> concept PublicPrivateCheckpoint = requires(T &owner) { owner.checkpoint_original(true); };
static_assert(!std::is_copy_constructible_v<Owner> && !std::is_move_constructible_v<Owner>);
static_assert(!std::is_copy_constructible_v<Ext4RegularMoveCompletion> &&
              !std::is_move_constructible_v<Ext4RegularMoveCompletion>);
static_assert(!PublicPrivateCheckpoint<Owner>);
Ext4RegularMoveUpdate request(const Bytes &bytes)
{
    return {2, 14, 12, r32(bytes, inode_offset(bytes, 2) + 100),
        r32(bytes, inode_offset(bytes, 14) + 100), r32(bytes, inode_offset(bytes, 12) + 100),
        "seed", "moved.txt", 1700000555, 123456789};
}
Block image(const Bytes &bytes, usize block)
{ Block out{}; std::copy_n(bytes.begin() + block * 1024, 1024, out.begin()); return out; }
void pin_image(Owner &owner, u64 block, const Block &expected)
{
    Owner::Pin pin; CHECK(owner.pin(block, pin));
    std::array<std::byte, 1024> observed{}; CHECK(pin.read(0, observed));
    for (usize byte = 0; byte < 1024; ++byte) CHECK(observed[byte] == std::byte(expected[byte]));
    CHECK(pin.release());
}
void normal_owner(const Bytes &original)
{
    Disk disk(original); disk.origin = 37; Owner::Registry registry;
    const auto key = registry.register_device(disk, 611); CHECK(key);
    auto owner = std::make_unique<Owner>(true); CHECK(owner->open(registry, key.value(), {37, original.size() / 512}));
    Owner::Pin reader; CHECK(owner->pin(journal(original) / 1024, reader));
    CHECK(owner->prepare_regular_move(request(original)).code() == StatusCode::busy);
    CHECK(owner->report().phase == Ext4OwnedPhase::open && disk.writes == 0 && disk.flushes == 0);
    CHECK(reader.release()); CHECK(owner->prepare_regular_move(request(original)));
    CHECK(owner->report().regular_move && !owner->report().cold_regular_move &&
        owner->report().phase == Ext4OwnedPhase::prepared && disk.writes == 0 && disk.flushes == 0);
    Owner::Pin commit_reader; CHECK(owner->pin(journal(original) / 1024, commit_reader));
    CHECK(owner->commit().code() == StatusCode::busy && owner->report().phase == Ext4OwnedPhase::prepared);
    CHECK(commit_reader.release()); CHECK(owner->commit()); CHECK(owner->checkpoint());
    const auto report = owner->report();
    CHECK(report.phase == Ext4OwnedPhase::checkpointed && report.regular_move_completion.complete());
    CHECK(report.regular_move_completion.physical_verified && report.regular_move_completion.filesystem_dirty_retained);
    CHECK(report.completion.phase == Ext4CompletionPhase::idle &&
        report.completion.journal_sequence == report.regular_move_completion.empty_sequence);
    CHECK(!owner->buffers().read_pins && !owner->buffers().transaction_active);
    pin_image(*owner, journal(original) / 1024, image(disk.durable, journal(original) / 1024));
    CHECK(disk.live == disk.durable && (r32(disk.durable, 1120) & 4));
    CHECK(!owner->checkpoint() && owner->close());
}
void shared_table_owner(const Bytes &original)
{
    // The populated directory and its existing note share one full inode-table
    // home. The canonical owner must retain both slot edits in that one image.
    CHECK(inode_offset(original, 14) / 1024 == inode_offset(original, 15) / 1024);
    Disk disk(original); Owner::Registry registry; const auto key = registry.register_device(disk, 616); CHECK(key);
    auto owner = std::make_unique<Owner>(true); CHECK(owner->open(registry, key.value(), {0, original.size() / 512}));
    const Ext4RegularMoveUpdate update{14, 2, 15, r32(original, inode_offset(original, 14) + 100),
        r32(original, inode_offset(original, 2) + 100), r32(original, inode_offset(original, 15) + 100),
        "note.txt", "root-note.txt", 1700000555, 123456789};
    CHECK(owner->prepare_regular_move(update) && owner->commit() && owner->checkpoint());
    CHECK(owner->report().regular_move_completion.complete() && owner->report().regular_move_completion.validated_actions == 4);
    struct Reader { Disk &disk; Status read_block(u32 number, std::span<u8> bytes)
        { return disk.read_blocks(u64{number} * 2, std::as_writable_bytes(bytes)); } } reader{disk};
    Ext4RecoveryBinding binding; CHECK(binding.inspect(disk, {0, original.size() / 512}));
    Ext4ReadOnlyView<Reader> view; CHECK(view.open(reader, *binding.info()));
    CHECK(view.lookup(14, "note.txt").status().code() == StatusCode::not_found);
    CHECK(view.lookup(2, "root-note.txt").value() == 15);
    CHECK(view.inode(15).value().generation == update.source_generation && view.inode(15).value().links == 1);
    CHECK(view.inode(14).value().generation == update.from_generation && view.inode(14).value().links >= 2);
    CHECK(owner->close());
}
void resident_preimage_refusal(const Bytes &original, bool ancillary)
{
    Disk disk(original); Owner::Registry registry; const auto key = registry.register_device(disk, 612); CHECK(key);
    auto owner = std::make_unique<Owner>(true); CHECK(owner->open(registry, key.value(), {0, original.size() / 512}));
    const usize selected = ancillary ? 1 : journal(original) / 1024;
    pin_image(*owner, selected, image(original, selected));
    if (ancillary) { disk.live[1144] ^= 1; super_crc(disk.live); }
    else
    {
        const usize at = journal(original);
        big32(disk.live, at + 24, be32(original, at + 24) + 2);
        big32(disk.live, at + 252, 0); big32(disk.live, at + 252, crc(~u32{0}, disk.live, at, 1024));
    }
    disk.durable = disk.live;
    CHECK(!owner->prepare_regular_move(request(disk.live)));
    CHECK(owner->report().phase == Ext4OwnedPhase::failed && owner->buffers().state == MetadataOwnerState::failed);
    CHECK(disk.writes == 0 && disk.flushes == 0);
    ++refusals;
}
void post_publication_ancillary_refusal(const Bytes &original)
{
    Disk disk(original); Owner::Registry registry; const auto key = registry.register_device(disk, 613); CHECK(key);
    auto owner = std::make_unique<Owner>(true); CHECK(owner->open(registry, key.value(), {0, original.size() / 512}));
    CHECK(owner->prepare_regular_move(request(original)) && owner->commit());
    disk.live[1144] ^= 1; super_crc(disk.live); disk.durable = disk.live;
    const auto writes = disk.writes, flushes = disk.flushes;
    CHECK(!owner->checkpoint()); CHECK(owner->report().phase == Ext4OwnedPhase::failed);
    CHECK(disk.writes == writes && disk.flushes == flushes); ++refusals;
}
struct Published { Bytes bytes; std::array<u64, 6> blocks{}; std::array<Block, 6> after{}; usize count{}; u32 sequence{}; };
Published publish(const Bytes &original)
{
    Disk disk(original); Ext4RegularMoveTransaction writer; Published out;
    CHECK(writer.prepare(disk, {0, original.size() / 512}, request(original)));
    CHECK(writer.logical_images(out.blocks, out.after, out.count)); out.sequence = writer.report().sequence;
    CHECK(writer.publish_prepared()); out.bytes = disk.durable; return out;
}
void check_mounted(Mount &mount, Disk &disk, const Published &published)
{
    const auto recovery = mount.startup_report();
    CHECK(recovery.initial_family == Ext4StartupFamily::regular_move_active && recovery.strict_ordinary_admission);
    CHECK(recovery.regular_move_checkpoints == 1 && recovery.regular_move_result_validated);
    CHECK(recovery.final_sequence == published.sequence + 2 && !recovery.final_head);
    auto *root = mount.root(); CHECK(root);
    CHECK(root->lookup_child("seed").status().code() == StatusCode::not_found);
    auto directory = root->lookup_child_retained("archived-documents"); CHECK(directory);
    auto file = directory.value()->lookup_child_retained("moved.txt"); CHECK(file);
    CHECK(file.value()->query_metadata().value().attributes.inode == 12);
    CHECK(file.value()->query_metadata().value().link_count == 1);
    CHECK(file.value()->release() && directory.value()->release());
    CHECK(root->sync()); CHECK(disk.live == disk.durable && (r32(disk.durable, 1120) & 4));
}
void cold_mixtures(const Bytes &original, const Published &published)
{
    for (u32 mask = 0; mask < (1U << (published.count - 1)); ++mask)
    {
        auto mixed = published.bytes;
        for (usize index = 1; index < published.count; ++index)
            if (mask & (1U << (index - 1)))
                std::copy(published.after[index].begin(), published.after[index].end(), mixed.begin() + published.blocks[index] * 1024);
        Disk disk(mixed); Mount::Registry registry; const auto key = registry.register_device(disk, 614); CHECK(key);
        auto storage = std::make_unique<Mount::CommonStorage>(); auto mount = std::make_unique<Mount>(*storage);
        CHECK(mount->mount(registry, key.value(), {0, original.size() / 512})); check_mounted(*mount, disk, published); ++mixtures;
    }
}
void cold_transport_refusal(const Bytes &original, const Published &published)
{
    Disk disk(published.bytes); disk.fail_flush = 1; Mount::Registry registry;
    const auto key = registry.register_device(disk, 615); CHECK(key);
    auto storage = std::make_unique<Mount::CommonStorage>(); auto mount = std::make_unique<Mount>(*storage);
    CHECK(!mount->mount(registry, key.value(), {0, original.size() / 512}));
    CHECK(!mount->root() && !mount->report().published && disk.writes == 0 && disk.flushes == 1); ++refusals;
}
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes original((std::istreambuf_iterator<char>(input)), {}); CHECK(original.size() == 32 * 1024 * 1024);
    normal_owner(original); shared_table_owner(original); resident_preimage_refusal(original, false); resident_preimage_refusal(original, true);
    post_publication_ancillary_refusal(original); const auto published = publish(original);
    cold_mixtures(original, published); cold_transport_refusal(original, published);
    std::printf("EXT4_OWNED_REGULAR_MOVE_MODEL_PASS checks=%zu mixtures=%zu refusals=%zu\n", checks, mixtures, refusals);
}
