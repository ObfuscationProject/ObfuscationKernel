#include "ext4_owned_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <memory>
using namespace owned_fixture;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL first data owner:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
namespace {
using Owner = Ext4OwnedTransaction<1, 8>;
using Block = std::array<u8, 1024>;
constexpr u32 target_inode = 14, offset = 7;
constexpr Ext4FirstDataUpdate allocation_update{target_inode, 23, 1700000300, 234567891};
Block data_image() {
    Block image{};
    constexpr std::string_view text = "First note save!";
    static_assert(text.size() == allocation_update.size - offset);
    for (usize index = 0; index < text.size(); ++index) image[offset + index] = u8(text[index]);
    return image;
}
usize restarts{};
void verify(Disk &disk, bool allocated) {
    auto preview = std::make_unique<Ext4ReplayPreview<6, 2>>(false, true, Ext4ReplayPolicy::first_data_allocation);
    const auto opened = preview->open(disk, {disk.origin, disk.live.size() / 512});
    if (!opened) std::fprintf(stderr, "verify preview: %s\n", opened.message().data());
    CHECK(opened);
    CHECK(preview->lookup(2, "empty-note").value() == target_inode);
    const auto inode = preview->inode(target_inode); CHECK(inode);
    CHECK(inode.value().mode == 0100600 && inode.value().uid == 1000 && inode.value().gid == 1001);
    CHECK(inode.value().links == 1 && inode.value().size == (allocated ? allocation_update.size : 0));
    std::array<u8, 1024> read{};
    const auto result = preview->read(target_inode, 0, read); CHECK(result);
    CHECK(result.value() == (allocated ? allocation_update.size : 0));
    if (allocated) CHECK(read == data_image());
}
void restart(const Bytes &durable, bool allocated) {
    Disk disk(durable); Ext4MountedMetadata<1>::Registry registry;
    const auto key = registry.register_device(disk, 29); CHECK(key);
    auto mount = std::make_unique<Ext4MountedMetadata<1>>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    auto status = mount->mount(registry, key.value(), {0, durable.size() / 512});
    if (!status) std::fprintf(stderr, "restart failed: %s\n", status.message().data());
    CHECK(status);
    auto *root = mount->root(); CHECK(root);
    const auto found = root->lookup_child("empty-note"); CHECK(found);
    const auto metadata = found.value()->query_metadata(); CHECK(metadata);
    CHECK(metadata.value().size == (allocated ? allocation_update.size : 0));
    CHECK(metadata.value().blocks == (allocated ? 2 : 0));
    verify(disk, allocated); CHECK(disk.live == disk.durable); ++restarts;
}
void rejected(const Bytes &image, Ext4FirstDataUpdate request, u32 at, const Block &bytes, StatusCode expected) {
    Disk disk(image); Owner::Registry registry;
    const auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true); CHECK(owner->open(registry, key.value(), {0, image.size()/512}));
    const auto status = owner->prepare_first_data(request, at, bytes);
    CHECK(status.code() == expected && owner->report().phase == Ext4OwnedPhase::rejected);
    CHECK(!disk.writes && !disk.flushes && disk.live == image && disk.durable == image);
    CHECK(!owner->buffers().read_pins && !owner->buffers().transaction_active && owner->close());
}
struct Counts { usize writes{}, flushes{}, homes{}, home_flushes{}; };
Counts successful(const Bytes &image, const std::filesystem::path &out) {
    Disk disk(image); disk.origin = 37; Owner::Registry registry;
    const auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true); CHECK(owner->open(registry, key.value(), {37, image.size()/512}));
    auto status = owner->prepare_first_data(allocation_update, offset, data_image());
    if (!status) std::fprintf(stderr, "prepare failed: %s\n", status.message().data());
    CHECK(status && owner->buffers().read_pins == 6 && !disk.writes && !disk.flushes);
    auto other = allocation_update; other.inode = 12;
    Block poison{}; poison.fill(0xa5);
    CHECK(owner->prepare_first_data(other, 0, poison).code() == StatusCode::busy);
    const Ext4InodeMetadataUpdate intrusive{12, 1000, 1001, 1700000300, 234567891,
        0600, true, 4096, 1700000300, 234567891};
    CHECK(owner->prepare_ordered_data(intrusive, 9999, poison).code() == StatusCode::busy);
    CHECK(owner->report().phase == Ext4OwnedPhase::prepared && owner->buffers().read_pins == 6);
    CHECK(owner->commit()); Counts counts{disk.writes, disk.flushes};
    CHECK(disk.live == disk.durable); save(out / "published.img", disk.durable);
    // Full initialized data is the first write and gets its own successful
    // FLUSH before the first journal descriptor or payload can be submitted.
    usize first_write = disk.trace.size(), second_write = disk.trace.size(); bool flushed{};
    for (usize index = 0; index < disk.trace.size(); ++index) if (disk.trace[index].operation == 'w') {
        if (first_write == disk.trace.size()) first_write = index; else { second_write = index; break; }
    }
    CHECK(first_write < second_write && disk.trace[first_write].size == 1024);
    for (usize index = first_write + 1; index < second_write; ++index) flushed |= disk.trace[index].operation == 'f';
    const auto first_image = data_image();
    CHECK(flushed && disk.submitted.front().bytes == Bytes(first_image.begin(), first_image.end()));
    CHECK(owner->checkpoint());
    counts.homes = disk.writes - counts.writes; counts.home_flushes = disk.flushes - counts.flushes;
    CHECK(owner->report().completion.validated_actions == 4 && owner->report().phase == Ext4OwnedPhase::checkpointed);
    CHECK(!owner->buffers().read_pins && !owner->buffers().transaction_active && owner->close());
    CHECK(disk.live == disk.durable); save(out / "completed.img", disk.durable);
    verify(disk, true);
    // Allocation changes one block, no inode. All owner-sealed data bytes are
    // checked independently of the reader's EOF and preserve the zero tail.
    CHECK(r32(image, 1036) == r32(disk.live, 1036) + 1);
    CHECK(r32(image, 1040) == r32(disk.live, 1040));
    const usize physical = r32(disk.live, inode_offset(disk.live, target_inode) + 60);
    const auto initialized = data_image();
    CHECK(std::equal(initialized.begin(), initialized.end(), disk.live.begin() + physical * 1024));
    return counts;
}
void fault(const Bytes &image, Counts counts, bool checkpoint, usize write, usize flush, bool after) {
    Disk disk(image); Owner::Registry registry;
    const auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true); CHECK(owner->open(registry, key.value(), {0, image.size()/512}));
    CHECK(owner->prepare_first_data(allocation_update, offset, data_image()));
    if (checkpoint) CHECK(owner->commit());
    if (write) (after ? disk.fail_write_after : disk.fail_write) = disk.writes + write;
    if (flush) (after ? disk.fail_flush_after : disk.fail_flush) = disk.flushes + flush;
    CHECK(!(checkpoint ? owner->checkpoint() : owner->commit()));
    CHECK(owner->report().phase == Ext4OwnedPhase::failed && owner->buffers().state == MetadataOwnerState::failed);
    CHECK(owner->prepare_first_data(allocation_update, offset, data_image()).code() == StatusCode::denied);
    const bool allocated = checkpoint || (after && ((write && write == counts.writes) || (flush && flush == counts.flushes)));
    disk.crash(); restart(disk.durable, allocated);
}
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4); std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes image((std::istreambuf_iterator<char>(input)), {});
    if (argc == 4 && std::string_view(argv[3]) == "revoke") optional_revoke_fixture(image);
    CHECK(image.size() == 32 * 1024 * 1024);
    std::filesystem::create_directory(argv[2]);
    const auto counts = successful(image, argv[2]);
    CHECK(counts.writes == 9 && counts.flushes == 5 && counts.homes == 5);
    auto request = allocation_update; request.size = 0; rejected(image, request, offset, data_image(), StatusCode::invalid_argument);
    request = allocation_update; request.nanoseconds = 1000000000; rejected(image, request, offset, data_image(), StatusCode::invalid_argument);
    request = allocation_update; request.inode = 12; rejected(image, request, offset, data_image(), StatusCode::unsupported);
    auto poison = data_image(); poison[0] = 1; rejected(image, allocation_update, offset, poison, StatusCode::invalid_argument);
    poison = data_image(); poison[1000] = 1; rejected(image, allocation_update, offset, poison, StatusCode::invalid_argument);
    rejected(image, allocation_update, allocation_update.size, data_image(), StatusCode::invalid_argument);
    for (bool after : {false, true}) {
        for (usize index = 1; index <= counts.writes; ++index) fault(image, counts, false, index, 0, after);
        for (usize index = 1; index <= counts.flushes; ++index) fault(image, counts, false, 0, index, after);
        for (usize index = 1; index <= counts.homes; ++index) fault(image, counts, true, index, 0, after);
        for (usize index = 1; index <= counts.home_flushes; ++index) fault(image, counts, true, 0, index, after);
    }
    std::printf("PASS first data owner restarts=%zu publication_writes=%zu publication_flushes=%zu checkpoint_writes=%zu checkpoint_flushes=%zu\n",
        restarts, counts.writes, counts.flushes, counts.homes, counts.home_flushes);
}
