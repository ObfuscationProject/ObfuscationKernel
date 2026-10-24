#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>

using namespace owned_fixture;
namespace clock_model = ok::testing::ext4_clock;
using Mount = Ext4MountedMetadata<1>;

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL mounted next data:%d: %s\n", __LINE__, #expression); std::exit(1); \
} } while (false)

namespace
{
struct Mounted
{
    Disk disk;
    Mount::Registry registry;
    Mount mount{Ext4MountedPolicy::ordered_data_dirty_noatime};
    Mount::Registry::Registration key{};
    explicit Mounted(const Bytes &image) : disk(image)
    {
        auto registered = registry.register_device(disk, 17); CHECK(registered);
        key = registered.value();
        const auto status = mount.mount(registry, key, {0, image.size() / 512});
        if (!status) std::fprintf(stderr, "mount: %.*s\n", int(status.message().size()), status.message().data());
        CHECK(status);
    }
    Node *file()
    {
        auto *root = mount.root(); CHECK(root);
        const auto node = root->lookup_child("empty-note"); CHECK(node);
        return node.value();
    }
};
[[nodiscard]] time::RealtimeSample sample(u32 seconds)
{ return {{seconds, 123456789}, {200, 1}, 1, time::RealtimeOrigin::administrator_software}; }
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    auto mounted = std::make_unique<Mounted>(original);
    auto *file = mounted->file();
    const auto before = file->query_metadata(); CHECK(before);
    CHECK(before.value().size == 0 && before.value().blocks == 0);
    std::array<std::byte, 1024> first{};
    for (usize at = 0; at < first.size(); ++at) first[at] = std::byte(u8(1 + at % 251));
    clock_model::set(sample(1700000200));
    auto status = file->write(0, first);
    if (!status) std::fprintf(stderr, "first write: %.*s\n", int(status.message().size()), status.message().data());
    CHECK(status);
    const auto filled = file->query_metadata(); CHECK(filled);
    CHECK(filled.value().size == 1024 && filled.value().blocks == 2);
    const u32 free_after_first = r32(mounted->disk.live, 1036);
    const usize inode_at = inode_offset(mounted->disk.live, 14);
    const u32 first_physical = r32(mounted->disk.live, inode_at + 60);
    CHECK(first_physical && first_physical * 1024 + 1024 <= mounted->disk.live.size());
    const Bytes old_block(mounted->disk.live.begin() + first_physical * 1024,
                          mounted->disk.live.begin() + first_physical * 1024 + 1024);
    CHECK(std::equal(old_block.begin(), old_block.end(), reinterpret_cast<const u8 *>(first.data())));
    const Bytes first_only = mounted->disk.durable;
    save(argv[2], mounted->disk.durable);

    constexpr std::string_view text = "next-block payload";
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    const usize trace_start = mounted->disk.trace.size();
    const usize writes_before = mounted->disk.writes, flushes_before = mounted->disk.flushes;
    clock_model::set(sample(1700000300));
    CHECK(file->write(1023, bytes.first(2)).code() == StatusCode::unsupported);
    CHECK(file->write(2048, bytes).code() == StatusCode::unsupported);
    CHECK(mounted->disk.writes == writes_before && mounted->disk.flushes == flushes_before);
    status = file->write(1031, bytes);
    if (!status) std::fprintf(stderr, "next write: %.*s\n", int(status.message().size()), status.message().data());
    CHECK(status);
    const auto after = file->query_metadata(); CHECK(after);
    CHECK(after.value().size == 1031 + bytes.size() && after.value().blocks == 4);
    CHECK(after.value().attributes.inode == before.value().attributes.inode);
    CHECK(after.value().uid == before.value().uid && after.value().gid == before.value().gid);
    CHECK(r32(mounted->disk.live, 1036) + 1 == free_after_first);
    CHECK(std::equal(old_block.begin(), old_block.end(), mounted->disk.live.begin() + first_physical * 1024));
    usize first_write = mounted->disk.trace.size(), second_write = mounted->disk.trace.size();
    for (usize index = trace_start; index < mounted->disk.trace.size(); ++index)
        if (mounted->disk.trace[index].operation == 'w')
        {
            if (first_write == mounted->disk.trace.size()) first_write = index;
            else { second_write = index; break; }
        }
    CHECK(first_write < second_write && mounted->disk.trace[first_write].size == 1024);
    bool ordered_flush = false;
    for (usize index = first_write + 1; index < second_write; ++index)
        ordered_flush |= mounted->disk.trace[index].operation == 'f';
    CHECK(ordered_flush && mounted->disk.live == mounted->disk.durable);
    const u32 next_physical = u32(mounted->disk.trace[first_write].sector / 2);
    CHECK(next_physical != first_physical);
    const auto &new_block = mounted->disk.submitted[writes_before].bytes;
    CHECK(new_block.size() == 1024);
    for (usize at = 0; at < 7; ++at) CHECK(new_block[at] == 0);
    for (usize at = 0; at < text.size(); ++at) CHECK(new_block[7 + at] == u8(text[at]));
    for (usize at = 7 + text.size(); at < new_block.size(); ++at) CHECK(new_block[at] == 0);
    auto reread = file->read(0, 2048); CHECK(reread && reread.value().size == 1031 + text.size());
    for (usize at = 0; at < 1024; ++at) CHECK(reread.value().data[at] == first[at]);
    for (usize at = 1024; at < 1031; ++at) CHECK(reread.value().data[at] == std::byte{});
    for (usize at = 0; at < text.size(); ++at)
        CHECK(reread.value().data[1031 + at] == bytes[at]);
    save(std::filesystem::path(argv[2]).replace_extension(".completed.img"), mounted->disk.durable);

    auto restarted = std::make_unique<Mounted>(mounted->disk.durable);
    auto *reopened = restarted->file();
    const auto metadata = reopened->query_metadata(); CHECK(metadata);
    CHECK(metadata.value().size == 1031 + text.size() && metadata.value().blocks == 4);
    reread = reopened->read(0, 2048); CHECK(reread && reread.value().size == 1031 + text.size());
    for (usize at = 0; at < 1024; ++at) CHECK(reread.value().data[at] == first[at]);
    for (usize at = 1024; at < 1031; ++at) CHECK(reread.value().data[at] == std::byte{});
    for (usize at = 0; at < text.size(); ++at)
        CHECK(reread.value().data[1031 + at] == bytes[at]);
    CHECK(reopened->write(2048, bytes).code() == StatusCode::unsupported);
    CHECK(restarted->mount.report().phase == Ext4MountPhase::readable);
    auto boundary = std::make_unique<Mounted>(first_only);
    auto *bounded = boundary->file();
    std::array<std::byte, 1024> full{};
    for (usize block = 1; block <= 15; ++block)
    {
        full.fill(std::byte(u8(block)));
        status = bounded->write(block * 1024, full);
        if (!status) std::fprintf(stderr, "block %zu: %.*s\n", block,
                                  int(status.message().size()), status.message().data());
        CHECK(status);
        const auto grown = bounded->query_metadata(); CHECK(grown);
        CHECK(grown.value().size == (block + 1) * 1024 && grown.value().blocks == (block + 1) * 2);
    }
    const auto last = bounded->read(15 * 1024, 1024); CHECK(last && last.value().size == 1024);
    for (usize at = 0; at < 1024; ++at) CHECK(last.value().data[at] == std::byte{15});
    const auto bounded_writes = boundary->disk.writes, bounded_flushes = boundary->disk.flushes;
    CHECK(bounded->write(16 * 1024, bytes.first(1)).code() == StatusCode::unsupported);
    CHECK(boundary->disk.writes == bounded_writes && boundary->disk.flushes == bounded_flushes);
    CHECK(boundary->disk.live == boundary->disk.durable);
    save(std::filesystem::path(argv[2]).replace_extension(".capacity.img"), boundary->disk.durable);
    auto with_sibling = std::make_unique<Mounted>(first_only);
    auto *sibling_root = with_sibling->mount.root(); CHECK(sibling_root);
    auto sibling = sibling_root->create_child("sibling", {NodeType::regular, 0600, 1000, 1001});
    if (!sibling) std::fprintf(stderr, "sibling create: %.*s\n", int(sibling.status().message().size()),
                               sibling.status().message().data());
    CHECK(sibling);
    const auto sibling_metadata = sibling.value()->query_metadata(); CHECK(sibling_metadata);
    CHECK(sibling_metadata.value().attributes.inode == 15);
    CHECK(inode_offset(with_sibling->disk.live, 14) / 1024 ==
          inode_offset(with_sibling->disk.live, 15) / 1024);
    CHECK(sibling.value()->write(0, first));
    CHECK(sibling.value()->write(1024, bytes.first(1)));
    const auto sibling_final = sibling.value()->query_metadata(); CHECK(sibling_final);
    CHECK(sibling_final.value().size == 1025 && sibling_final.value().blocks == 4);
    const auto target_unchanged = with_sibling->file()->query_metadata(); CHECK(target_unchanged);
    CHECK(target_unchanged.value().size == 1024 && target_unchanged.value().blocks == 2);
    save(std::filesystem::path(argv[2]).replace_extension(".sibling.img"), with_sibling->disk.durable);
    std::puts("PASS hosted mounted next data: append, zero fill, barriers, restart, 16-block boundary and same-table sibling");
}
