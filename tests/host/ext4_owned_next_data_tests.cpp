#include "ext4_owned_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <memory>

using namespace owned_fixture;
using Owner = Ext4OwnedTransaction<1, 8>;
using Mount = Ext4MountedMetadata<1>;
using Block = std::array<u8, 1024>;

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL next data owner:%d: %s\n", __LINE__, #expression); std::exit(1); \
} } while (false)

namespace
{
constexpr u32 inode_number = 14, old_size = 1024, offset = 1031;
constexpr std::string_view payload = "owner next block";
constexpr Ext4NextDataUpdate next_update{inode_number, old_size, offset + u32(payload.size()),
                                    1700000400, 987654321};
usize restarts{}, old_restarts{}, new_restarts{};

Block image()
{
    Block result{};
    for (usize index = 0; index < payload.size(); ++index)
        result[offset - old_size + index] = u8(payload[index]);
    return result;
}
bool restart(const Bytes &durable, const Bytes &baseline)
{
    Disk disk(durable);
    Mount::Registry registry;
    const auto key = registry.register_device(disk, 29); CHECK(key);
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    const auto status = mount->mount(registry, key.value(), {0, durable.size() / 512});
    if (!status) std::fprintf(stderr, "restart mount: %.*s\n", int(status.message().size()), status.message().data());
    CHECK(status);
    auto *root = mount->root(); CHECK(root);
    const auto file = root->lookup_child("empty-note"); CHECK(file);
    const auto metadata = file.value()->query_metadata(); CHECK(metadata);
    CHECK(metadata.value().size == old_size || metadata.value().size == next_update.size);
    CHECK(metadata.value().blocks == (metadata.value().size == old_size ? 2 : 4));
    CHECK(metadata.value().uid == 1000 && metadata.value().gid == 1001);
    const auto read = file.value()->read(0, 2048); CHECK(read);
    CHECK(read.value().size == metadata.value().size);
    const usize old_at = inode_offset(baseline, inode_number);
    const u32 old_physical = r32(baseline, old_at + 60);
    CHECK(old_physical);
    for (usize index = 0; index < old_size; ++index)
        CHECK(read.value().data[index] == std::byte(baseline[old_physical * 1024 + index]));
    const bool new_state = metadata.value().size == next_update.size;
    if (!new_state) ++old_restarts;
    else
    {
        ++new_restarts;
        for (usize index = old_size; index < offset; ++index)
            CHECK(read.value().data[index] == std::byte{});
        for (usize index = 0; index < payload.size(); ++index)
            CHECK(read.value().data[offset + index] == std::byte(u8(payload[index])));
    }
    CHECK(disk.live == disk.durable);
    ++restarts;
    return new_state;
}
void rejected(const Bytes &source, Ext4NextDataUpdate request, u32 at,
              const Block &bytes, StatusCode expected)
{
    Disk disk(source); Owner::Registry registry;
    const auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, source.size() / 512}));
    const auto status = owner->prepare_next_data(request, at, bytes);
    if (status.code() != expected)
        std::fprintf(stderr, "reject expected=%u got=%u %.*s\n", u32(expected), u32(status.code()),
                     int(status.message().size()), status.message().data());
    CHECK(status.code() == expected && owner->report().phase == Ext4OwnedPhase::rejected);
    CHECK(!disk.writes && !disk.flushes && disk.live == source && disk.durable == source);
    CHECK(!owner->buffers().read_pins && !owner->buffers().transaction_active && owner->close());
}
struct Counts { usize writes{}, flushes{}, homes{}, home_flushes{}; };
Counts successful(const Bytes &source, const std::filesystem::path &out)
{
    Disk disk(source); disk.origin = 37; Owner::Registry registry;
    const auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {37, source.size() / 512}));
    auto status = owner->prepare_next_data(next_update, offset, image());
    if (!status) std::fprintf(stderr, "prepare: %.*s\n", int(status.message().size()), status.message().data());
    CHECK(status && owner->buffers().read_pins == 6 && !disk.writes && !disk.flushes);
    auto other = next_update; other.inode = 12;
    CHECK(owner->prepare_next_data(other, offset, image()).code() == StatusCode::busy);
    CHECK(owner->report().phase == Ext4OwnedPhase::prepared);
    CHECK(owner->commit());
    Counts count{disk.writes, disk.flushes};
    CHECK(disk.live == disk.durable); save(out / "published.img", disk.durable);
    usize first = disk.trace.size(), second = disk.trace.size();
    for (usize index = 0; index < disk.trace.size(); ++index)
        if (disk.trace[index].operation == 'w')
        {
            if (first == disk.trace.size()) first = index;
            else { second = index; break; }
        }
    CHECK(first < second && disk.trace[first].size == 1024);
    bool ordered_flush{};
    for (usize index = first + 1; index < second; ++index)
        ordered_flush |= disk.trace[index].operation == 'f';
    const auto proposed_image = image();
    CHECK(ordered_flush && disk.submitted.front().bytes ==
          Bytes(proposed_image.begin(), proposed_image.end()));
    CHECK(owner->checkpoint());
    count.homes = disk.writes - count.writes;
    count.home_flushes = disk.flushes - count.flushes;
    CHECK(owner->report().completion.validated_actions == 4 &&
          owner->report().phase == Ext4OwnedPhase::checkpointed);
    CHECK(!owner->buffers().read_pins && !owner->buffers().transaction_active && owner->close());
    CHECK(disk.live == disk.durable); save(out / "completed.img", disk.durable);
    CHECK(r32(source, 1036) == r32(disk.live, 1036) + 1);
    CHECK(r32(source, 1040) == r32(disk.live, 1040));
    CHECK(restart(disk.durable, source));
    return count;
}
void fault(const Bytes &source, bool checkpoint, usize write, usize flush, bool after)
{
    Disk disk(source); Owner::Registry registry;
    const auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, source.size() / 512}));
    CHECK(owner->prepare_next_data(next_update, offset, image()));
    if (checkpoint) CHECK(owner->commit());
    if (write) (after ? disk.fail_write_after : disk.fail_write) = disk.writes + write;
    if (flush) (after ? disk.fail_flush_after : disk.fail_flush) = disk.flushes + flush;
    CHECK(!(checkpoint ? owner->checkpoint() : owner->commit()));
    CHECK(owner->report().phase == Ext4OwnedPhase::failed &&
          owner->buffers().state == MetadataOwnerState::failed);
    CHECK(owner->prepare_next_data(next_update, offset, image()).code() == StatusCode::denied);
    disk.crash();
    const bool all_after_published = checkpoint && !write && flush == 3 && after;
    if (all_after_published)
    {
        CHECK(r32(disk.durable, inode_offset(disk.durable, inode_number) + 4) == next_update.size);
        CHECK(be32(disk.durable, journal(disk.durable) + 28) == 1);
    }
    const bool new_state = restart(disk.durable, source);
    if (all_after_published) CHECK(new_state);
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3 || argc == 4);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes source((std::istreambuf_iterator<char>(input)), {});
    if (argc == 4 && std::string_view(argv[3]) == "revoke") optional_revoke_fixture(source);
    CHECK(source.size() == 32 * 1024 * 1024);
    std::filesystem::create_directory(argv[2]);
    const auto counts = successful(source, argv[2]);
    CHECK(counts.writes == 9 && counts.flushes == 5 && counts.homes == 5);
    auto request = next_update; request.old_size = 0;
    rejected(source, request, offset, image(), StatusCode::invalid_argument);
    request = next_update; request.size = old_size + 1025;
    rejected(source, request, offset, image(), StatusCode::invalid_argument);
    request = next_update; request.nanoseconds = 1000000000;
    rejected(source, request, offset, image(), StatusCode::invalid_argument);
    request = next_update; request.inode = 12;
    rejected(source, request, offset, image(), StatusCode::unsupported);
    auto poison = image(); poison[0] = 1;
    rejected(source, next_update, offset, poison, StatusCode::invalid_argument);
    poison = image(); poison[1000] = 1;
    rejected(source, next_update, offset, poison, StatusCode::invalid_argument);
    rejected(source, next_update, next_update.size, image(), StatusCode::invalid_argument);
    for (bool after : {false, true})
    {
        for (usize at = 1; at <= counts.writes; ++at) fault(source, false, at, 0, after);
        for (usize at = 1; at <= counts.flushes; ++at) fault(source, false, 0, at, after);
        for (usize at = 1; at <= counts.homes; ++at) fault(source, true, at, 0, after);
        for (usize at = 1; at <= counts.home_flushes; ++at) fault(source, true, 0, at, after);
    }
    std::printf("PASS next data owner restarts=%zu old=%zu new=%zu publication_writes=%zu publication_flushes=%zu checkpoint_writes=%zu checkpoint_flushes=%zu\n",
                restarts, old_restarts, new_restarts, counts.writes, counts.flushes,
                counts.homes, counts.home_flushes);
}
