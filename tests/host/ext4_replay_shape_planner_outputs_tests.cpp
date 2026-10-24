#include "ok/fs/ext4_create_plan.hpp"
#include "ok/fs/ext4_first_data_plan.hpp"
#include "ok/fs/ext4_next_data_plan.hpp"
#include "ok/fs/ext4_replay_shape.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "replay planner outputs:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Block = std::array<u8, 1024>;
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
            return Status::invalid_argument("replay output fixture read bounds");
        for (usize index = 0; index < out.size(); ++index)
            out[index] = std::byte(image_[sector * 512 + index]);
        ++reads; return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("replay output fixture must not write"); }
    Status flush() override
    { ++flushes; return Status::denied("replay output fixture must not flush"); }
    usize reads{}, writes{}, flushes{};
  private:
    const Image &image_;
};

Image read_image(const char *name)
{
    std::ifstream stream(name, std::ios::binary);
    CHECK(stream);
    Image image((std::istreambuf_iterator<char>(stream)), {});
    CHECK(image.size() == 32 * 1024 * 1024);
    return image;
}
Block load(const Image &image, u32 number)
{
    CHECK(number < image.size() / 1024);
    Block result{};
    for (usize index = 0; index < result.size(); ++index)
        result[index] = image[usize{number} * 1024 + index];
    return result;
}
u32 value(const Block &block, usize at) { return ext4_checked::le32(block, at); }
usize inode_slot(u32 number) { return ((number - 1) % 4) * 256; }
u32 table_for(const Block &descriptors, u32 number)
{
    const u32 group = (number - 1) / 2048;
    return value(descriptors, group * 32 + 8) + ((number - 1) % 2048) / 4;
}

template <usize N>
void check_images(const Image &image, const std::array<u32, N> &targets,
                  const std::array<Block, N> &before,
                  const std::array<Block, N> &after, usize count)
{
    CHECK(count <= N);
    for (usize index = 0; index < count; ++index)
    {
        CHECK(before[index] == load(image, targets[index]) && after[index] != before[index]);
        for (usize previous = 0; previous < index; ++previous)
            CHECK(targets[previous] != targets[index]);
    }
}

Result<Ext4ReplayShape> classify(const Ext4RecoveryInfo &binding,
                                 std::span<const u32> targets, u32 directory = 0)
{
    CHECK(!targets.empty() && targets.size() <= 6);
    std::array<jbd2::ReplayAction, 6> actions{};
    for (usize index = 0; index < targets.size(); ++index)
        actions[index] = {targets[index], u32(index + 2), 41, 0, 0, false};
    const jbd2::ReplayPlan plan{
        std::span<const jbd2::ReplayAction>{actions}.first(targets.size()),
        1, 42, 12, 0, false, false};
    const auto directories = directory ? std::span<const u32>{&directory, 1} : std::span<const u32>{};
    return Ext4ReplayShapePlanner::classify(binding, {directories}, plan);
}
void expect_family(const Ext4RecoveryInfo &binding, std::span<const u32> targets,
                   Ext4ReplayFamily family, u32 directory = 0)
{
    const auto result = classify(binding, targets, directory);
    CHECK(result && result.value().includes(family) &&
          result.value().candidates == (u32{1} << u8(family)));
}
void create_case(const Image &image, const Ext4RecoveryInfo &binding)
{
    const Block super = load(image, 1), descriptors = load(image, 2);
    const Block inode_bitmap = load(image, value(descriptors, 4));
    const auto selected = Ext4CreatePlanner::select_inode(binding, super, descriptors, inode_bitmap, 0);
    CHECK(selected);
    const u32 inode = selected.value(), parent = 2;
    const u32 parent_block = table_for(descriptors, parent), new_block = table_for(descriptors, inode);
    const Block parent_table = load(image, parent_block), new_table = load(image, new_block);
    const u32 directory_block = value(parent_table, inode_slot(parent) + 60);
    const Block directory = load(image, directory_block);
    const Ext4CreateSource source{inode, 0, parent_block, new_block, directory_block,
                                  super, descriptors, inode_bitmap,
                                  parent_table, new_table, directory};
    const Ext4CreateUpdate update{parent, 1700000111, 123456789,
                                   "replay-shape-note", {NodeType::regular, 0640, 1000, 1001}};
    Ext4CreatePlan output{};
    const auto status = Ext4CreatePlanner::plan(binding, update, source, output);
    if (!status) std::fprintf(stderr, "create planner: %.*s\n",
                              int(status.message().size()), status.message().data());
    CHECK(status && output.count == 6 && output.inode == inode);
    check_images(image, output.targets, output.before, output.after, output.count);
    expect_family(binding, std::span{output.targets}.first(output.count),
                  Ext4ReplayFamily::empty_create, directory_block);
    auto old_assumed_order = output.targets;
    std::swap(old_assumed_order[0], old_assumed_order[4]);
    CHECK(!classify(binding, std::span{old_assumed_order}.first(output.count), directory_block));
}
void first_case(const Image &image, const Ext4RecoveryInfo &binding)
{
    const Block super = load(image, 1), descriptors = load(image, 2);
    const Block bitmap = load(image, value(descriptors, 0));
    const auto selected = Ext4FirstDataPlanner::select_block(binding, super, descriptors, bitmap, 0);
    CHECK(selected);
    const u32 inode = 14, table_block = table_for(descriptors, inode);
    const Block table = load(image, table_block);
    CHECK(value(table, inode_slot(inode) + 4) == 0);
    const Ext4FirstDataSource source{0, selected.value(), table_block,
                                     super, descriptors, bitmap, table};
    const Ext4FirstDataUpdate update{inode, 19, 1700000222, 222222222};
    Ext4FirstDataPlan output{};
    const auto status = Ext4FirstDataPlanner::plan(binding, update, source, output);
    if (!status) std::fprintf(stderr, "first-data planner: %.*s\n",
                              int(status.message().size()), status.message().data());
    CHECK(status && output.count == 4 && output.inode == inode);
    check_images(image, output.targets, output.before, output.after, output.count);
    expect_family(binding, std::span{output.targets}.first(output.count),
                  Ext4ReplayFamily::bounded_data_allocation);
    auto old_assumed_order = output.targets;
    std::swap(old_assumed_order[0], old_assumed_order[2]);
    CHECK(!classify(binding, old_assumed_order));
}
void next_case(const Image &image, const Ext4RecoveryInfo &binding)
{
    const Block super = load(image, 1), descriptors = load(image, 2);
    const Block bitmap = load(image, value(descriptors, 0));
    const auto selected = Ext4NextDataPlanner::select_block(binding, super, descriptors, bitmap, 0);
    CHECK(selected);
    const u32 inode = 12, table_block = table_for(descriptors, inode);
    const Block table = load(image, table_block);
    const u32 old_size = value(table, inode_slot(inode) + 4);
    CHECK(old_size == 4096);
    const Ext4NextDataSource source{0, selected.value(), table_block,
                                    super, descriptors, bitmap, table};
    const Ext4NextDataUpdate update{inode, old_size, old_size + 19,
                                     1700000333, 333333333};
    Ext4NextDataPlan output{};
    const auto status = Ext4NextDataPlanner::plan(binding, update, source, output);
    if (!status) std::fprintf(stderr, "next-data planner: %.*s\n",
                              int(status.message().size()), status.message().data());
    CHECK(status && output.count == 4 && output.inode == inode);
    check_images(image, output.targets, output.before, output.after, output.count);
    expect_family(binding, std::span{output.targets}.first(output.count),
                  Ext4ReplayFamily::bounded_data_allocation);
    auto old_assumed_order = output.targets;
    std::swap(old_assumed_order[0], old_assumed_order[2]);
    CHECK(!classify(binding, old_assumed_order));
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    const Image create = read_image(argv[1]);
    const Image first = read_image(argv[2]);
    const Image next = read_image(argv[3]);
    for (usize index = 0; index < 3; ++index)
    {
        const Image &image = index == 0 ? create : index == 1 ? first : next;
        MemoryDisk disk(image);
        Ext4RecoveryBinding admission;
        CHECK(admission.inspect(disk, {0, image.size() / 512}));
        CHECK(admission.info());
        const usize reads = disk.reads;
        if (index == 0) create_case(image, *admission.info());
        if (index == 1) first_case(image, *admission.info());
        if (index == 2) next_case(image, *admission.info());
        CHECK(disk.reads == reads && disk.writes == 0 && disk.flushes == 0);
    }
    std::puts("PASS actual planner output replay shape: create, first-data, next-data, old order rejected");
}
