#include "ext4_owned_support.hpp"
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL create frontier:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

namespace
{
using Owner = Ext4OwnedTransaction<1, 8>;

Bytes create(Bytes image, std::string_view name, u32 expected_inode, bool checkpoint)
{
    Disk disk(image);
    Owner::Registry registry;
    const auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, image.size() / 512}));
    const Ext4CreateUpdate update{2, 1700000111, 123456789, name,
                                  {NodeType::regular, 0600, 1000, 1001}};
    const auto status = owner->prepare_create(update);
    if (!status)
        std::fprintf(stderr, "create frontier prepare: %.*s\n",
                     int(status.message().size()), status.message().data());
    CHECK(status);
    CHECK(owner->created_inode() == expected_inode);
    CHECK(owner->commit());
    if (checkpoint) CHECK(owner->checkpoint());
    CHECK(disk.live == disk.durable);
    if (checkpoint) CHECK(owner->close());
    return disk.durable;
}
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes image((std::istreambuf_iterator<char>(input)), {});
    CHECK(image.size() == 32 * 1024 * 1024);
    const usize group = descriptor(image, 0);
    const u32 per_group = r32(image, 1064);
    CHECK(per_group == 2048 && r16(image, group + 28) == 2032);
    w32(image, 1120, r32(image, 1120) | 4U);
    super_crc(image);
    image = create(std::move(image), "frontier-one", 14, true);
    image = create(std::move(image), "frontier-two", 15, true);
    CHECK(r16(image, group + 28) == 2032);
    image = create(std::move(image), "frontier-three", 17, false);
    CHECK(r16(image, group + 28) == 2032);
    const usize journal_offset = journal(image);
    CHECK(be32(image, journal_offset + 28) != 0);
    const usize descriptor_payload = journal_offset + 5 * 1024;
    CHECK(r16(image, descriptor_payload + 28) == 2031);
    save(argv[2], image);
    std::puts("PASS private create frontier: inode 17 advances unused-inode high-water mark");
}
