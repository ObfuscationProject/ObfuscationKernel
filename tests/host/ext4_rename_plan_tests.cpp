#include "../../src/fs/ext4_rename_plan.hpp"
#include "ok/fs/ext4_completion.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "rename planner test:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Image = std::vector<u8>;
using Inode = std::array<u8, 256>;
using Block = std::array<u8, 1024>;

u16 le16(const Image &image, usize at) { return u16{image[at]} | (u16{image[at + 1]} << 8); }
u32 le32(const Image &image, usize at) { return u32{le16(image, at)} | (u32{le16(image, at + 2)} << 16); }
u32 be32(const Image &image, usize at)
{ return (u32{image[at]} << 24) | (u32{image[at + 1]} << 16) | (u32{image[at + 2]} << 8) | image[at + 3]; }
void be32(Image &image, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) image[at + index] = u8(value >> (24 - index * 8)); }

class MemoryDisk final : public driver::BlockDevice
{
  public:
    explicit MemoryDisk(const Image &image) : image_(image) {}
    driver::BlockGeometry geometry() const override { return {image_.size() / 512, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if (sector > image_.size() / 512 || out.size() % 512 ||
            out.size() / 512 > image_.size() / 512 - sector)
            return Status::invalid_argument("rename fixture read bounds");
        for (usize index = 0; index < out.size(); ++index)
            out[index] = std::byte(image_[sector * 512 + index]);
        ++reads; return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("planner must not write"); }
    Status flush() override { ++flushes; return Status::denied("planner must not flush"); }
    usize reads{}, writes{}, flushes{};
  private:
    const Image &image_;
};

void inode_checksum(Inode &inode, u32 number, u32 seed)
{
    inode[124] = inode[125] = inode[130] = inode[131] = 0;
    auto crc = ext4_checked::inode_seed(seed, number, inode);
    for (const auto byte : inode) crc = jbd2::detail::crc_byte(crc, byte);
    inode[124] = u8(crc); inode[125] = u8(crc >> 8);
    inode[130] = u8(crc >> 16); inode[131] = u8(crc >> 24);
}

void directory_checksum(Block &block, const Inode &inode, u32 number, u32 seed)
{
    const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode),
                                          std::span<const u8>{block}.first(1012));
    for (usize index = 0; index < 4; ++index) block[1020 + index] = u8(crc >> (8 * index));
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    CHECK(input);
    const Image original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    MemoryDisk disk(original);
    Ext4RecoveryBinding admission;
    CHECK(admission.inspect(disk, {0, original.size() / 512}));
    CHECK(admission.info() && admission.info()->layout.block_size == 1024);
    const auto &binding = *admission.info();
    const usize table = le32(original, 2048 + 8) * 1024;
    Inode parent{}, source{};
    for (usize index = 0; index < 256; ++index)
    {
        parent[index] = original[table + 256 + index];
        source[index] = original[table + 11 * 256 + index];
    }
    const u32 data_block = ext4_checked::le32(parent, 60);
    CHECK(data_block < original.size() / 1024);
    Block image{};
    for (usize index = 0; index < image.size(); ++index)
        image[index] = original[data_block * 1024 + index];
    const auto planned = Ext4DirectoryRenamePlanner::plan(binding, 2, parent, 12, source,
                                                            data_block, image, "seed", "note");
    CHECK(planned);
    CHECK(planned.value().before == image && planned.value().after != image);
    CHECK(planned.value().directory_inode == 2 && planned.value().source_inode == 12);
    CHECK(planned.value().entry_offset < 1012 && disk.writes == 0 && disk.flushes == 0);
    CHECK(ext4_checked::le32(planned.value().after, 1020) ==
          jbd2::detail::crc32c(ext4_checked::inode_seed(
              jbd2::detail::crc32c(~u32{0}, binding.uuid), 2, parent),
              std::span<const u8>{planned.value().after}.first(1012)));

    auto check_reject = [&](const Inode &directory, const Inode &file, const Block &block,
                            std::string_view old_name, std::string_view new_name, StatusCode code) {
        const auto result = Ext4DirectoryRenamePlanner::plan(binding, 2, directory, 12, file,
                                                               data_block, block, old_name, new_name);
        CHECK(!result && result.status().code() == code);
    };
    check_reject(parent, source, image, "seed", "init", StatusCode::already_exists);
    const auto relocated = Ext4DirectoryRenamePlanner::plan(binding, 2, parent, 12, source,
        data_block, image, "seed", "notes");
    CHECK(relocated && relocated.value().before == image && relocated.value().source_inode == 12);
    check_reject(parent, source, image, "lost", "note", StatusCode::not_found);
    check_reject(parent, source, image, ".", "note", StatusCode::invalid_argument);
    check_reject(parent, source, image, "seed", "bad/name", StatusCode::invalid_argument);
    auto bad_tail = image; bad_tail[1020] ^= 1;
    check_reject(parent, source, bad_tail, "seed", "note", StatusCode::io_error);
    auto bad_source = source; bad_source[124] ^= 1;
    check_reject(parent, bad_source, image, "seed", "note", StatusCode::io_error);
    auto linked = source; linked[26] = 2; linked[27] = 0;
    const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
    inode_checksum(linked, 12, seed);
    check_reject(parent, linked, image, "seed", "note", StatusCode::unsupported);
    auto overlap = source;
    const u32 physical = ext4_checked::le32(overlap, 60);
    overlap[42] = 2; // Two initialized logical runs alias one physical block.
    overlap[56] = 2;
    overlap[64] = 2; overlap[65] = overlap[66] = overlap[67] = 0;
    overlap[68] = 2; overlap[69] = overlap[70] = overlap[71] = 0;
    for (usize index = 0; index < 4; ++index) overlap[72 + index] = u8((physical + 1) >> (8 * index));
    inode_checksum(overlap, 12, seed);
    check_reject(parent, overlap, image, "seed", "note", StatusCode::unsupported);
    auto oversized = source;
    const u32 size = 17 * 1024;
    for (usize index = 0; index < 4; ++index)
    {
        oversized[4 + index] = u8(size >> (8 * index));
        oversized[28 + index] = u8(34U >> (8 * index));
    }
    inode_checksum(oversized, 12, seed);
    check_reject(parent, oversized, image, "seed", "note", StatusCode::unsupported);
    auto malformed = image; malformed[planned.value().entry_offset + 4] = 3;
    directory_checksum(malformed, parent, 2, seed);
    check_reject(parent, source, malformed, "seed", "note", StatusCode::io_error);
    CHECK(disk.writes == 0 && disk.flushes == 0);

    // A published SDK40 inode-only log is accepted. Replacing its first tag
    // with a directory data target must fail before replay writes or flushes.
    std::ifstream published_input(argv[2], std::ios::binary);
    CHECK(published_input);
    const Image published((std::istreambuf_iterator<char>(published_input)), {});
    CHECK(published.size() == original.size());
    MemoryDisk accepted_disk(published);
    Ext4ReplayPreview<4, 2> accepted(true, false);
    CHECK(accepted.open(accepted_disk, {0, published.size() / 512}));
    Image unaccepted = published;
    const usize published_table = le32(published, 2048 + 8) * 1024;
    const u32 journal = le32(published, published_table + 7 * 256 + 60);
    const usize descriptor = u64{journal} * 1024 + 1024;
    const u32 first_tag = be32(published, descriptor + 12);
    const u32 published_directory = le32(published, published_table + 256 + 60);
    CHECK(first_tag != published_directory);
    be32(unaccepted, descriptor + 12, published_directory);
    std::array<u8, 16> uuid{};
    for (usize index = 0; index < uuid.size(); ++index) uuid[index] = published[1024 + 104 + index];
    const u32 journal_seed = jbd2::detail::crc32c(~u32{0}, uuid);
    be32(unaccepted, descriptor + 1020, 0);
    const u32 descriptor_crc = jbd2::detail::crc32c(
        journal_seed, std::span<const u8>{unaccepted}.subspan(descriptor, 1024));
    be32(unaccepted, descriptor + 1020, descriptor_crc);
    MemoryDisk rejected_disk(unaccepted);
    Ext4ReplayPreview<4, 2> rejected(true, false);
    CHECK(!rejected.open(rejected_disk, {0, unaccepted.size() / 512}));
    Ext4RecoveryCompletion<4, 2> completion(true, first_tag != 1);
    CHECK(!completion.run(rejected_disk, {0, unaccepted.size() / 512}));
    CHECK(rejected_disk.writes == 0 && rejected_disk.flushes == 0);

    Image proposed = original;
    for (usize index = 0; index < image.size(); ++index)
        proposed[data_block * 1024 + index] = planned.value().after[index];
    std::ofstream output(argv[3], std::ios::binary);
    output.write(reinterpret_cast<const char *>(proposed.data()),
                 static_cast<std::streamsize>(proposed.size()));
    CHECK(output.good());
    std::printf("PASS bounded rename plan: fit and relocation, ten rejects, current recovery rejects directory tag before I/O; synthetic home image only\n");
}
