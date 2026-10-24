#include "ext4_orphan_access_fixture.hpp"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    CHECK(input);
    const Image image((std::istreambuf_iterator<char>(input)), {});
    CHECK(image.size() == 32 * 1024 * 1024);
    MemoryDisk disk(image);
    Ext4RecoveryBinding admission;
    CHECK(admission.inspect(disk, {0, image.size() / 512}));
    CHECK(admission.info());
    const auto &binding = *admission.info();
    const usize admitted_reads = disk.reads;

    // The ordinary pathname view still rejects a zero-link inode.
    Fixture ordinary_fixture(image, binding);
    ordinary_fixture.add(image, 15, 14);
    ordinary_fixture.add(image, 14, 0);
    Image ordinary_image = image;
    for (usize index = 0; index < 1024; ++index)
        ordinary_image[usize{ordinary_fixture.images[0].table_block} * 1024 + index] =
            ordinary_fixture.images[0].table[index];
    ImageReader reader(ordinary_image);
    Ext4ReadOnlyView<ImageReader> ordinary;
    CHECK(ordinary.open(reader, binding));
    const auto pathname = ordinary.inode(14);
    CHECK(!pathname && pathname.status().code() == StatusCode::unsupported);

    Fixture data(image, binding);
    data.add(image, 12, 14);
    data.add(image, 14, 0);
    data.head(12);
    const Ext4OrphanAccessKey key{binding.uuid, 12, 12, data.images[0].expected_generation};
    Ext4OrphanReadOnlySnapshot snapshot{};
    CHECK(!snapshot.valid());
    CHECK(!snapshot.physical_block(0) &&
          snapshot.physical_block(0).status().code() == StatusCode::not_initialized);
    CHECK(Ext4OrphanReadOnlyDecoder::capture(binding, data.source(), key, snapshot));
    CHECK(snapshot.valid() && snapshot.filesystem_uuid() == binding.uuid &&
          snapshot.observed_head() == 12 && snapshot.number() == 12 &&
          snapshot.generation() == key.generation && snapshot.size() == 4096 &&
          snapshot.block_count() == 4);

    Ext4OrphanChainProof proof{};
    CHECK(Ext4OrphanChainValidator::validate(binding, data.source(), proof));
    const auto *member = proof.member(key.number, key.generation);
    CHECK(member && member->data_count == 4);
    for (u32 logical = 0; logical < snapshot.block_count(); ++logical)
    {
        auto physical = snapshot.physical_block(logical);
        CHECK(physical && physical.value() == member->data_blocks[logical] &&
              physical.value() > 0 && physical.value() < binding.layout.filesystem_blocks);
    }
    CHECK(!snapshot.physical_block(4) &&
          snapshot.physical_block(4).status().code() == StatusCode::not_found);

    const Ext4OrphanAccessKey empty_key{binding.uuid, 12, 14,
                                        data.images[1].expected_generation};
    CHECK(Ext4OrphanReadOnlyDecoder::capture(binding, data.source(), empty_key, snapshot));
    CHECK(snapshot.valid() && snapshot.number() == 14 && snapshot.size() == 0 &&
          snapshot.block_count() == 0 && !snapshot.physical_block(0));

    usize rejected = 0;
    auto reject = [&](const Fixture &fixture, Ext4OrphanAccessKey requested, StatusCode code) {
        CHECK(Ext4OrphanReadOnlyDecoder::capture(binding, data.source(), key, snapshot));
        const auto status = Ext4OrphanReadOnlyDecoder::capture(
            binding, fixture.source(), requested, snapshot);
        CHECK(!status && status.code() == code && !snapshot.valid() &&
              snapshot.number() == 0 && snapshot.block_count() == 0 &&
              !snapshot.physical_block(0));
        ++rejected;
    };
    auto bad = key;
    bad.filesystem_uuid[0] ^= 1;
    reject(data, bad, StatusCode::io_error);
    bad = key;
    bad.observed_head = 14;
    reject(data, bad, StatusCode::io_error);
    bad = key;
    bad.observed_head = 0;
    reject(data, bad, StatusCode::invalid_argument);
    bad = key;
    bad.number = 10;
    reject(data, bad, StatusCode::invalid_argument);
    bad = key;
    bad.number = 8193;
    reject(data, bad, StatusCode::invalid_argument);
    bad = key;
    bad.number = 13;
    reject(data, bad, StatusCode::not_found);
    bad = key;
    ++bad.generation;
    reject(data, bad, StatusCode::not_found);
    auto missing = data;
    missing.count = 1;
    reject(missing, key, StatusCode::io_error);
    auto changed_head = data;
    changed_head.head(14);
    reject(changed_head, key, StatusCode::io_error);
    auto corrupt = data;
    corrupt.images[0].table[slot(12) + 4] ^= 1;
    reject(corrupt, key, StatusCode::io_error);
    Fixture no_chain(image, binding);
    reject(no_chain, key, StatusCode::io_error);
    CHECK(rejected == 11 && disk.reads == admitted_reads && disk.writes == 0 &&
          disk.flushes == 0);

    std::ifstream filled_input(argv[2], std::ios::binary);
    CHECK(filled_input);
    const Image filled_image((std::istreambuf_iterator<char>(filled_input)), {});
    CHECK(filled_image.size() == image.size());
    MemoryDisk filled_disk(filled_image);
    Ext4RecoveryBinding filled_admission;
    CHECK(filled_admission.inspect(filled_disk, {0, filled_image.size() / 512}));
    CHECK(filled_admission.info());
    const auto &filled_binding = *filled_admission.info();
    const usize filled_reads = filled_disk.reads;
    Fixture filled(filled_image, filled_binding);
    filled.add(filled_image, 14, 0);
    filled.head(14);
    const Ext4OrphanAccessKey filled_key{filled_binding.uuid, 14, 14,
                                         filled.images[0].expected_generation};
    CHECK(Ext4OrphanReadOnlyDecoder::capture(filled_binding, filled.source(),
                                             filled_key, snapshot));
    CHECK(snapshot.valid() && snapshot.size() == 45 && snapshot.block_count() == 1 &&
          snapshot.physical_block(0) && filled_disk.reads == filled_reads &&
          filled_disk.writes == 0 && filled_disk.flushes == 0);
    std::printf("PASS orphan read-only snapshot: populated, empty, rejections=%zu no_device_io=1\n",
                rejected);
}
