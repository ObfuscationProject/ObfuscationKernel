#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4_fs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                                       \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)
using namespace ok;
using Bytes = std::vector<std::byte>;
u16 read16(const Bytes &data, usize at)
{
    return std::to_integer<u16>(data[at]) | (std::to_integer<u16>(data[at + 1]) << 8);
}
u32 read32(const Bytes &data, usize at)
{
    return read16(data, at) | (u32{read16(data, at + 2)} << 16);
}
void write16(Bytes &data, usize at, u16 value)
{
    data[at] = std::byte(value);
    data[at + 1] = std::byte(value >> 8);
}
void write32(Bytes &data, usize at, u32 value)
{
    write16(data, at, static_cast<u16>(value));
    write16(data, at + 2, static_cast<u16>(value >> 16));
}
std::span<const std::byte> text(const char *value)
{
    return {reinterpret_cast<const std::byte *>(value), std::strlen(value)};
}
class FaultDevice final : public driver::BlockDevice
{
  public:
    explicit FaultDevice(const Bytes &bytes) : cache(bytes), durable(bytes)
    {
    }
    driver::BlockGeometry geometry() const override
    {
        return {cache.size() / 512, 512, true};
    }
    bool fails(char operation)
    {
        trace.push_back(operation);
        if (armed && ++calls == fail_at)
        {
            triggered = true;
            return true;
        }
        return false;
    }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        CHECK(first * 512 + out.size() <= cache.size());
        if (fails('r'))
            return Status::fault("injected read failure");
        std::memcpy(out.data(), cache.data() + first * 512, out.size());
        return Status::success();
    }
    Status write_blocks(u64 first, std::span<const std::byte> in) override
    {
        CHECK(first * 512 + in.size() <= cache.size());
        ++writes;
        if (fails('w'))
        {
            if (partial)
                std::memcpy(cache.data() + first * 512, in.data(), in.size() / 2);
            return Status::fault("injected partial write failure");
        }
        std::memcpy(cache.data() + first * 512, in.data(), in.size());
        return Status::success();
    }
    Status flush() override
    {
        const bool error = fails('f');
        if (!error || partial)
            durable = cache; // A failed flush may have persisted everything.
        return error ? Status::fault("injected flush failure") : Status::success();
    }
    void arm(usize index, bool partial_io)
    {
        armed = true;
        calls = 0;
        fail_at = index;
        partial = partial_io;
        triggered = false;
        trace.clear();
    }
    Bytes cache, durable;
    std::vector<char> trace;
    usize calls{}, fail_at{}, writes{};
    bool armed{}, triggered{}, partial{};
};
fs::Node *notes(fs::Ext4FileSystem &filesystem)
{
    auto *directory = filesystem.root()->lookup("persist");
    CHECK(directory);
    auto *file = directory->lookup("notes");
    CHECK(file);
    return file;
}
Status mutate(fs::Ext4FileSystem &filesystem, fs::Node &file, int operation)
{
    switch (operation)
    {
    case 0:
        return file.write(16000, text("allocation failure"));
    case 1:
        return file.truncate(1);
    case 2:
        return filesystem.root()->create("failure-dir", fs::NodeType::directory);
    case 3: {
        auto *directory = filesystem.root()->lookup("persist");
        return directory ? directory->remove("notes", false) : Status::fault("directory read failed");
    }
    case 4:
        return filesystem.sync();
    case 5:
        return filesystem.root()->link("failure-link", file);
    case 6: {
        auto *directory = filesystem.root()->lookup("persist");
        return directory ? directory->rename("notes", *directory, "longer-new-name")
                         : Status::fault("directory read failed");
    }
    case 7: {
        auto *directory = filesystem.root()->lookup("persist");
        return directory ? directory->rename("notes", *filesystem.root(), "replace-file")
                         : Status::fault("directory read failed");
    }
    case 8: {
        auto *directory = filesystem.root()->lookup("persist");
        return directory ? filesystem.root()->rename("move-dir", *directory, "new-directory")
                         : Status::fault("directory read failed");
    }
    case 9: {
        auto *directory = filesystem.root()->lookup("persist");
        return directory ? filesystem.root()->rename("move-dir", *directory, "replace-dir")
                         : Status::fault("directory read failed");
    }
    case 10:
        return filesystem.root()->create_symlink("failure-symlink", "../persist/notes", 0x12345678, 0x87654321);
    case 11:
        return filesystem.root()->create_symlink("failure-symlink", std::string(60, 'x'), 0, 0);
    case 12: {
        auto *source = filesystem.root()->lookup("s59");
        return source ? filesystem.root()->link("failure-symlink", *source) : Status::fault("link read failed");
    }
    case 13:
        return filesystem.root()->rename("s59", *filesystem.root(), "replace-file");
    case 14:
        return filesystem.root()->remove("s59", false);
    case 15:
        return filesystem.root()->remove("s60", false);
    case 16:
        return filesystem.root()->remove("legacy60", false);
    default:
        return filesystem.root()->rename("s60", *filesystem.root(), "s59");
    }
}
void injected_failures(const Bytes &baseline)
{
    FaultDevice prepared(baseline);
    fs::Ext4FileSystem setup;
    CHECK(setup.mount(prepared));
    CHECK(setup.root()->create("replace-file", fs::NodeType::regular));
    CHECK(setup.root()->lookup("replace-file")->write(0, text("replacement data")));
    CHECK(setup.root()->create("move-dir", fs::NodeType::directory));
    CHECK(setup.root()->lookup("move-dir")->create("child", fs::NodeType::regular));
    CHECK(setup.root()->lookup("persist")->create("replace-dir", fs::NodeType::directory));
    CHECK(setup.sync());
    const auto &namespace_baseline = prepared.durable;
    usize checked = 0;
    for (int operation = 0; operation < 18; ++operation)
    {
        FaultDevice good(namespace_baseline);
        fs::Ext4FileSystem good_fs;
        CHECK(good_fs.mount(good));
        auto *good_file = notes(good_fs);
        CHECK(good_file->write(0, text("S"))); // Begin with a durable dirty marker.
        CHECK(read16(good.durable, 1082) == 0);
        good.arm(~usize{0}, false);
        CHECK(mutate(good_fs, *good_file, operation));
        const auto operations = good.trace.size();
        for (usize failure = 1; failure <= operations; ++failure)
            for (bool partial : {false, true})
            {
                FaultDevice disk(namespace_baseline);
                fs::Ext4FileSystem filesystem;
                CHECK(filesystem.mount(disk));
                auto *file = notes(filesystem);
                CHECK(file->write(0, text("S")));
                disk.arm(failure, partial);
                CHECK(!mutate(filesystem, *file, operation));
                CHECK(disk.triggered);
                CHECK(read16(disk.cache, 1082) == 0);
                CHECK(read16(disk.durable, 1082) == 0);
                const auto writes = disk.writes;
                CHECK(!filesystem.sync());
                CHECK(!filesystem.root()->create("after-failure", fs::NodeType::regular));
                CHECK(!filesystem.root()->chmod(0700));
                CHECK(disk.writes == writes);
                CHECK(read16(disk.cache, 1082) == 0 && read16(disk.durable, 1082) == 0);
                ++checked;
            }
    }
    std::printf("PASS %zu injected ext4 read/write/flush failures: dirty marker and sticky quarantine\n", checked);
}
void namespace_extent_limit(const Bytes &baseline)
{
    FaultDevice disk(baseline);
    fs::Ext4FileSystem filesystem;
    CHECK(filesystem.mount(disk));
    auto *root = filesystem.root();
    CHECK(root->create("capacity", fs::NodeType::directory));
    auto *directory = root->lookup("capacity");
    auto *source = notes(filesystem);
    CHECK(directory && source);
    u64 previous_blocks = 0;
    unsigned blockers = 0, links = 0;
    bool exhausted = false;
    for (unsigned i = 0; i < 1024; ++i)
    {
        const auto blocks = directory->metadata().blocks;
        if (blocks != previous_blocks)
        {
            // Interleave real file blocks so directory growth needs separate extents.
            char blocker[32];
            std::snprintf(blocker, sizeof(blocker), "blocker-%u", blockers++);
            CHECK(root->create(blocker, fs::NodeType::regular));
            CHECK(root->lookup(blocker)->write(0, text("occupied")));
            previous_blocks = blocks;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "%031u", i);
        auto status = directory->link(name, *source);
        if (!status.ok())
        {
            CHECK(status.code() == StatusCode::overflow);
            CHECK(!directory->lookup(name));
            exhausted = true;
            break;
        }
        ++links;
    }
    CHECK(exhausted && links > 0 && source->metadata().link_count == links + 1);
    // Allocation and rollback wrote metadata before hitting the inline-extent
    // limit. Never mark such an interrupted multi-step namespace change clean.
    CHECK(!filesystem.sync());
    CHECK(read16(disk.cache, 1082) == 0 && read16(disk.durable, 1082) == 0);
    CHECK(!root->create("after-capacity", fs::NodeType::regular));
    std::puts("PASS ext4 namespace extent-capacity failure preserves source links and quarantines partial mutation");
}
void malformed_metadata(const Bytes &baseline)
{
    const u32 block_size = 1024u << read32(baseline, 1048);
    const u32 first_data = read32(baseline, 1044);
    const usize descriptors = (first_data + 1) * block_size;
    const u32 bitmap = read32(baseline, descriptors);
    for (int corruption = 0; corruption < 5; ++corruption)
    {
        auto image = baseline;
        if (corruption == 0)
            write32(image, descriptors + 4, bitmap); // Overlapping allocation bitmaps.
        if (corruption == 1)
            write32(image, descriptors + 8, bitmap); // Inode table aliases bitmap.
        if (corruption == 2)
            write32(image, descriptors, first_data); // Bitmap overwrites superblock.
        if (corruption == 3)
            write16(image, descriptors + 14, 0xffff); // Impossible free-inode count.
        if (corruption == 4)
            write32(image, 1024 + 72, 1); // Non-Linux creator uses a different inode ownership layout.
        FaultDevice disk(image);
        fs::Ext4FileSystem filesystem;
        CHECK(!filesystem.mount(disk));
        CHECK(disk.writes == 0 && disk.cache == image);
        disk.cache = disk.durable = baseline;
        CHECK(filesystem.mount(disk)); // A failed read-only mount attempt can be retried.
    }
    const auto inode_size = read16(baseline, 1112);
    const auto table = read32(baseline, descriptors + 8);
    const usize root_inode = table * block_size + inode_size; // inode 2
    const u32 root_block = read32(baseline, root_inode + 60);
    for (int corruption = 0; corruption < 4; ++corruption)
    {
        auto image = baseline;
        if (corruption == 0)
            write32(image, root_inode + 60, bitmap); // Freeing/truncating metadata as file data.
        if (corruption == 1)
            write16(image, root_inode + 56, 0); // Zero-length extent.
        if (corruption == 2)
            write16(image, root_inode + 44, 1); // Wrong inline extent capacity.
        if (corruption == 3)
            write32(image, root_inode + 28, 0); // i_blocks underflows during truncate.
        FaultDevice disk(image);
        fs::Ext4FileSystem filesystem;
        CHECK(filesystem.mount(disk));
        CHECK(!filesystem.root()->create("must-not-write", fs::NodeType::regular));
        CHECK(disk.writes == 0 && disk.cache == image);
    }
    // A non-multiple directory size must not silently hide a partial last block.
    auto image = baseline;
    write32(image, root_inode + 4, block_size - 1);
    FaultDevice odd_directory(image);
    fs::Ext4FileSystem odd_fs;
    CHECK(odd_fs.mount(odd_directory));
    CHECK(!odd_fs.root()->list());
    CHECK(!odd_fs.sync());
    CHECK(odd_directory.writes == 0);
    // Lookup must not construct a cache node for an out-of-range inode number.
    image = baseline;
    usize entry = root_block * block_size;
    while (std::to_integer<u8>(image[entry + 6]) <= 2)
        entry += read16(image, entry + 4);
    const auto name_length = std::to_integer<u8>(image[entry + 6]);
    std::string_view name(reinterpret_cast<const char *>(image.data() + entry + 8), name_length);
    write32(image, entry, read32(image, 1024) + 1);
    FaultDevice invalid_inode(image);
    fs::Ext4FileSystem inode_fs;
    CHECK(inode_fs.mount(invalid_inode));
    CHECK(!inode_fs.root()->lookup(name));
    CHECK(!inode_fs.sync());
    CHECK(invalid_inode.writes == 0);
    std::puts("PASS malformed ext4 group, extent, directory and inode metadata rejected before mutation");
}
usize named_inode(const Bytes &image, std::string_view name)
{
    const u32 block_size = 1024u << read32(image, 1048);
    const usize descriptors = (read32(image, 1044) + 1) * block_size;
    const auto inode_size = read16(image, 1112);
    const usize root_inode = read32(image, descriptors + 8) * block_size + inode_size;
    for (u16 extent = 0; extent < read16(image, root_inode + 42); ++extent)
        for (u16 block = 0; block < read16(image, root_inode + 56 + extent * 12); ++block)
        {
            const usize begin = (read32(image, root_inode + 60 + extent * 12) + block) * block_size;
            for (usize entry = begin; entry < begin + block_size; entry += read16(image, entry + 4))
            {
                const auto size = std::to_integer<u8>(image[entry + 6]);
                if (read32(image, entry) && std::string_view(reinterpret_cast<const char *>(image.data() + entry + 8), size) == name)
                {
                    const auto index = read32(image, entry) - 1;
                    return read32(image, descriptors + (index / read32(image, 1064)) * 32 + 8) * block_size +
                           (index % read32(image, 1064)) * inode_size;
                }
            }
        }
    CHECK(false);
    return 0;
}
void malformed_symlinks(const Bytes &baseline)
{
    const u32 block_size = 1024u << read32(baseline, 1048);
    const usize descriptors = (read32(baseline, 1044) + 1) * block_size;
    const auto short_inode = named_inode(baseline, "s59"), long_inode = named_inode(baseline, "s60");
    const auto legacy_inode = named_inode(baseline, "legacy60");
    for (int corruption = 0; corruption < 17; ++corruption)
    {
        auto image = baseline;
        const char *name = corruption < 4 ? "s59" : corruption < 15 ? "s60" : "legacy60";
        switch (corruption)
        {
        case 0: write32(image, short_inode + 4, 60); break;
        case 1: write32(image, short_inode + 32, 0x80000); break;
        case 2: image[short_inode + 99] = std::byte{'x'}; break; // Missing trailing NUL.
        case 3: image[short_inode + 43] = std::byte{0}; break; // Embedded NUL.
        case 4: write32(image, long_inode + 32, 0x10000000); break; // Inline-data flag.
        case 5: write32(image, long_inode + 104, 1); break; // External attributes.
        case 6: write16(image, long_inode + 116, 1); break; // High block accounting.
        case 7: write32(image, long_inode + 4, block_size); break;
        case 8: write32(image, long_inode + 28, block_size / 512 + 1); break;
        case 9: write16(image, long_inode + 42, 0); break; // Hole.
        case 10: write32(image, long_inode + 52, 1); break;
        case 11: write16(image, long_inode + 56, 0x8001); break; // Unwritten extent.
        case 12: write16(image, long_inode + 58, 1); break;
        case 13: write32(image, long_inode + 60, read32(image, descriptors)); break; // Allocation bitmap.
        case 14: image[read32(image, long_inode + 60) * block_size + 60] = std::byte{'x'}; break;
        case 15: write32(image, legacy_inode + 44, 1); break; // Second direct block.
        case 16: write32(image, legacy_inode + 40, read32(image, descriptors + 8)); break; // Inode table.
        }
        FaultDevice disk(image);
        fs::Ext4FileSystem filesystem;
        CHECK(filesystem.mount(disk));
        auto *node = filesystem.root()->lookup(name);
        CHECK(node && !node->read(0, 4096));
        CHECK(!node->write(0, text("unsafe")) && !node->truncate(0));
        CHECK(!filesystem.root()->remove(name, false));
        CHECK(disk.writes == 0 && disk.cache == image);
    }
    std::puts("PASS malformed inline, extent and direct-block symlinks reject unsafe layouts without writes");
}
void symlink_capacity(const Bytes &baseline)
{
    auto full = baseline;
    const u32 block_size = 1024u << read32(full, 1048), first = read32(full, 1044);
    const auto per_group = read32(full, 1056), blocks = read32(full, 1028);
    const auto groups = (blocks - first - 1) / per_group + 1;
    const usize descriptors = (first + 1) * block_size;
    // Resource-exhaustion model: reserve every data block in allocation
    // metadata. This synthetic image is not an offline-fsck fixture.
    write32(full, 1036, 0);
    for (u32 group = 0; group < groups; ++group)
    {
        const auto bitmap = read32(full, descriptors + group * 32) * block_size;
        for (u32 i = 0; i < block_size; ++i)
            full[bitmap + i] = std::byte{0xff};
        write16(full, descriptors + group * 32 + 12, 0);
    }
    FaultDevice disk(full);
    fs::Ext4FileSystem filesystem;
    CHECK(filesystem.mount(disk));
    disk.arm(~usize{0}, false);
    CHECK(filesystem.root()->create_symlink("no-blocks", std::string(60, 'x'), 0, 0).code() == StatusCode::no_memory);
    const auto calls = disk.trace.size();
    CHECK(!filesystem.root()->lookup("no-blocks"));
    CHECK(read32(disk.cache, 1040) == read32(full, 1040));
    for (u32 group = 0; group < groups; ++group)
    {
        const auto descriptor = descriptors + group * 32;
        CHECK(read16(disk.cache, descriptor + 14) == read16(full, descriptor + 14));
        const auto bitmap = read32(full, descriptor + 4) * block_size;
        CHECK(std::memcmp(disk.cache.data() + bitmap, full.data() + bitmap, block_size) == 0);
    }
    CHECK(filesystem.sync() && read16(disk.durable, 1082) == 1);
    CHECK(filesystem.root()->create_symlink("fits-inline", "target", 0, 0));
    CHECK(filesystem.root()->remove("fits-inline", false) && filesystem.sync());
    for (usize failure = 1; failure <= calls; ++failure)
        for (bool partial : {false, true})
        {
            FaultDevice faulty(full);
            fs::Ext4FileSystem instance;
            CHECK(instance.mount(faulty));
            faulty.arm(failure, partial);
            CHECK(!instance.root()->create_symlink("no-blocks", std::string(60, 'x'), 0, 0));
            CHECK(faulty.triggered);
            const auto writes = faulty.writes;
            CHECK(!instance.sync());
            CHECK(!instance.root()->create_symlink("after-failure", "target", 0, 0));
            CHECK(writes == faulty.writes);
        }
    std::printf("PASS ext4 symlink allocation rollback restores inode counts/bitmap; %zu rollback I/O faults quarantine\n", calls * 2);
}
void ownership(const Bytes &baseline)
{
    auto image = baseline;
    const u32 block_size = 1024u << read32(image, 1048);
    const usize descriptors = (read32(image, 1044) + 1) * block_size;
    const usize root_inode = read32(image, descriptors + 8) * block_size + read16(image, 1112);
    write16(image, root_inode + 2, 0);
    write16(image, root_inode + 24, 1);
    write16(image, root_inode + 120, 1);
    write16(image, root_inode + 122, 2);
    FaultDevice disk(image);
    fs::Ext4FileSystem filesystem;
    CHECK(filesystem.mount(disk));
    CHECK(filesystem.root()->metadata().uid == 65536);
    CHECK(filesystem.root()->metadata().gid == 131073);
    CHECK(filesystem.root()->chown(0xfedcba98, 0x89abcdef));
    CHECK(filesystem.root()->metadata().uid == 0xfedcba98);
    CHECK(filesystem.root()->metadata().gid == 0x89abcdef);
    CHECK(filesystem.root()->chown(0, 0));
    CHECK(filesystem.root()->metadata().uid == 0 && filesystem.root()->metadata().gid == 0);
    CHECK(read16(disk.cache, root_inode + 120) == 0 && read16(disk.cache, root_inode + 122) == 0);
    CHECK(filesystem.sync());
    std::puts("PASS ext4 32-bit UID/GID ownership round trips without truncation");
}
void cached_lifetime(const Bytes &baseline)
{
    FaultDevice disk(baseline);
    fs::Ext4FileSystem filesystem;
    CHECK(filesystem.mount(disk));
    auto *root = filesystem.root();
    CHECK(root->create("pinned", fs::NodeType::regular));
    auto *pinned = root->lookup("pinned");
    CHECK(pinned);
    CHECK(pinned->retain());
    CHECK(pinned->write(0, text("retained content")));
    CHECK(root->remove("pinned", false));
    CHECK(filesystem.sync());
    CHECK(read16(disk.cache, 1082) == 0); // An open unlinked inode prevents a clean marker.
    for (unsigned i = 0; i < 150; ++i)
    {
        CHECK(root->create("other", fs::NodeType::regular));
        auto retained = root->lookup_child_retained("other");
        CHECK(retained);
        auto *other = retained.value();
        CHECK(other != pinned);
        CHECK(other->write(0, text("unrelated")));
        CHECK(root->remove("other", false));
        CHECK(other->release());
    }
    auto read = pinned->read(0, 32);
    CHECK(read && read.value().size == 16);
    CHECK(pinned->release());
    CHECK(!pinned->read(0, 1));
    CHECK(!pinned->retain());
    CHECK(!pinned->release());
    CHECK(!pinned->write(0, text("stale")));
    CHECK(!pinned->truncate(0));
    CHECK(!pinned->sync());
    CHECK(!pinned->lookup("anything"));
    CHECK(!pinned->create("anything", fs::NodeType::regular));
    CHECK(!pinned->remove("anything", false));
    CHECK(!pinned->chmod(0600));
    CHECK(!pinned->chown(0, 0));
    CHECK(!pinned->list());
    static_cast<void>(pinned->metadata());
    CHECK(filesystem.sync());
    CHECK(read16(disk.durable, 1082) == 1);
    std::puts("PASS retained unlinked ext4 nodes survive cache reuse; released nodes fail safely");
}
int main(int argc, char **argv)
{
    testing::ext4_clock::reset();
    struct LegacyClockGuard { ~LegacyClockGuard() { CHECK(testing::ext4_clock::calls() == 0); } } legacy_clock_guard;
    CHECK(argc == 2);
    auto *file = std::fopen(argv[1], "rb");
    CHECK(file);
    CHECK(!std::fseek(file, 0, SEEK_END));
    Bytes baseline(static_cast<usize>(std::ftell(file)));
    CHECK(!std::fseek(file, 0, SEEK_SET));
    CHECK(std::fread(baseline.data(), 1, baseline.size(), file) == baseline.size());
    std::fclose(file);
    injected_failures(baseline);
    namespace_extent_limit(baseline);
    malformed_metadata(baseline);
    malformed_symlinks(baseline);
    symlink_capacity(baseline);
    ownership(baseline);
    cached_lifetime(baseline);
}
