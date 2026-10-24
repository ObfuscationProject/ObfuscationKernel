#include "ext4_completion_support.hpp"
#include <ok/fs/ext4_completion.hpp>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace fixture;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL create replay:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

namespace
{
struct Disk final : driver::BlockDevice
{
    explicit Disk(Bytes source) : image(std::move(source)) {}
    struct Operation { char kind; u64 sector; };
    Bytes image;
    std::vector<Operation> operations;
    usize writes{}, flushes{};
    driver::BlockGeometry geometry() const override { return {image.size() / 512, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if (sector >= image.size() / 512 || out.size() % 512 ||
            out.size() / 512 > image.size() / 512 - sector)
            return Status::io_error("create replay test read bounds");
        std::copy_n(image.begin() + sector * 512, out.size(),
                    reinterpret_cast<u8 *>(out.data()));
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> bytes) override
    {
        ++writes;
        operations.push_back({'w', sector});
        if (sector >= image.size() / 512 || bytes.size() != 1024 ||
            bytes.size() / 512 > image.size() / 512 - sector)
            return Status::io_error("create replay test write bounds");
        std::copy_n(reinterpret_cast<const u8 *>(bytes.data()), bytes.size(),
                    image.begin() + sector * 512);
        return {};
    }
    Status flush() override { ++flushes; operations.push_back({'f', 0}); return {}; }
};

struct Published
{
    usize journal{};
    usize count{};
    std::array<u32, 6> target{};
};
Published inspect(const Bytes &image)
{
    Published result{};
    result.journal = journal(image);
    CHECK(be32(image, result.journal + 28) != 0);
    for (usize index = 0; index < result.target.size(); ++index)
    {
        const usize tag = result.journal + 1024 + (index ? 44 + (index - 1) * 16 : 12);
        result.target[index] = be32(image, tag);
        CHECK(result.target[index] < image.size() / 1024);
        if (be32(image, tag + 4) & jbd2::flag_last_tag)
        {
            result.count = index + 1;
            break;
        }
    }
    CHECK(result.count == 5 || result.count == 6);
    CHECK(result.target[result.count - 3] == 2 && result.target[result.count - 2] == 1);
    return result;
}
void home_after(Bytes &image, const Published &published, usize index)
{
    std::copy_n(image.begin() + published.journal + (index + 2) * 1024, 1024,
                image.begin() + usize(published.target[index]) * 1024);
}
void accepted(Bytes image, const Published &published)
{
    Disk device(std::move(image));
    auto completion = std::make_unique<Ext4RecoveryCompletion<6, 2>>(
        false, true, Ext4ReplayPolicy::directory_create);
    const auto status = completion->run(device, {0, device.image.size() / 512});
    if (!status)
        std::fprintf(stderr, "create replay rejected code=%u message=%.*s\n",
                     u32(status.code()), int(status.message().size()), status.message().data());
    CHECK(status);
    CHECK(completion->report().complete());
    CHECK(completion->report().validated_actions == published.count);
    CHECK(device.writes == published.count + 1);
    CHECK(device.flushes == 5);
    usize cursor{};
    const auto expect_flush = [&]()
    {
        CHECK(cursor < device.operations.size() && device.operations[cursor].kind == 'f');
        ++cursor;
    };
    const auto expect_write = [&](u32 block)
    {
        CHECK(cursor < device.operations.size() && device.operations[cursor].kind == 'w' &&
              device.operations[cursor].sector == u64(block) * 2);
        ++cursor;
    };
    const usize table_count = published.count - 4;
    expect_flush();
    for (usize index = 0; index < table_count; ++index) expect_write(published.target[index]);
    expect_flush();
    for (usize index = table_count; index < table_count + 3; ++index)
        expect_write(published.target[index]);
    expect_flush();
    expect_write(published.target[published.count - 1]);
    expect_flush();
    expect_write(u32(published.journal / 1024));
    expect_flush();
    CHECK(cursor == device.operations.size());
    for (usize index = 0; index < published.count; ++index)
        CHECK(std::equal(device.image.begin() + usize(published.target[index]) * 1024,
                         device.image.begin() + (usize(published.target[index]) + 1) * 1024,
                         device.image.begin() + published.journal + (index + 2) * 1024));
    CHECK(be32(device.image, published.journal + 28) == 0);
}
void rejected(Bytes image)
{
    Disk device(std::move(image));
    auto completion = std::make_unique<Ext4RecoveryCompletion<6, 2>>(
        false, true, Ext4ReplayPolicy::directory_create);
    CHECK(!completion->run(device, {0, device.image.size() / 512}));
    CHECK(device.writes == 0 && device.flushes == 0);
}
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes published((std::istreambuf_iterator<char>(input)), {});
    CHECK(published.size() == 32 * 1024 * 1024);
    const auto profile = inspect(published);
    const usize table_count = profile.count - 4;

    // Table writes precede their FLUSH; either table may have reached media.
    for (usize mask = 0; mask < (usize{1} << table_count); ++mask)
    {
        Bytes image = published;
        for (usize index = 0; index < table_count; ++index)
            if (mask & (usize{1} << index)) home_after(image, profile, index);
        accepted(std::move(image), profile);
    }
    // The three allocation homes may persist in any combination after the
    // table FLUSH, before the directory write is admitted.
    for (usize mask = 0; mask < 8; ++mask)
    {
        Bytes image = published;
        for (usize index = 0; index < table_count; ++index) home_after(image, profile, index);
        for (usize bit = 0; bit < 3; ++bit)
            if (mask & (usize{1} << bit)) home_after(image, profile, table_count + bit);
        accepted(std::move(image), profile);
    }
    {
        Bytes image = published;
        for (usize index = 0; index < profile.count; ++index) home_after(image, profile, index);
        accepted(std::move(image), profile);
    }

    // A directory home cannot precede the durable table/allocation barriers.
    {
        Bytes image = published;
        home_after(image, profile, profile.count - 1);
        rejected(std::move(image));
    }
    {
        Bytes image = published;
        home_after(image, profile, table_count);
        rejected(std::move(image));
    }
    // Third images and torn journal payloads must never reach a home write.
    {
        Bytes image = published;
        image[usize(profile.target[table_count]) * 1024 + 17] ^= 0x80;
        rejected(std::move(image));
    }
    {
        Bytes image = published;
        image[usize(profile.target[table_count + 1]) * 1024 + 14] ^= 1;
        rejected(std::move(image));
    }
    {
        Bytes image = published;
        image[profile.journal + (table_count + 2) * 1024 + 19] ^= 1;
        rejected(std::move(image));
    }
    std::puts("PASS private create replay: allocation persistence combinations and malformed logs");
}
