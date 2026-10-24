#include "ok/fs/ext4_orphan_snapshot.hpp"
#include "snapshot_fixture.hpp"

#include <memory>

namespace
{
void write_block(Image &image, u32 physical, const Block &block)
{
    CHECK(physical < image.size() / 1024);
    for (usize index = 0; index < block.size(); ++index)
        image[usize{physical} * 1024 + index] = block[index];
}

Image prepared_image(const char *path)
{
    Image image = read_image(path);
    MemoryDisk disk(image);
    Ext4RecoveryBinding bootstrap{};
    CHECK(bootstrap.inspect(disk, {0, image.size() / 512}));
    CHECK(bootstrap.info());
    Fixture fixture(image, *bootstrap.info());
    fixture.add(image, 12, 14);
    fixture.add(image, 14, 0);
    fixture.head(12);
    Tables tables(image, fixture);
    DirectoryImages directories(image, fixture, tables);
    directories.remove_target(fixture, tables, 12);
    directories.remove_target(fixture, tables, 14);

    write_block(image, 1, fixture.super);
    for (usize index = 0; index < fixture.count; ++index)
        write_block(image, fixture.images[index].table_block, fixture.images[index].table);
    for (const auto &directory : directories.blocks)
        write_block(image, directory.physical, directory.bytes);
    return image;
}

class TriggerDisk final : public driver::BlockDevice
{
  public:
    TriggerDisk(Image image, u32 trigger, Block replacement, bool mutate)
        : image_(std::move(image)), trigger_(trigger), replacement_(replacement), mutate_(mutate) {}

    driver::BlockGeometry geometry() const override
    { return {image_.size() / 512, 512, true}; }

    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if (sector > image_.size() / 512 || out.empty() || out.size() % 512 ||
            out.size() / 512 > image_.size() / 512 - sector)
            return Status::invalid_argument("independent disk read bounds");
        for (usize index = 0; index < out.size(); ++index)
            out[index] = std::byte(image_[sector * 512 + index]);
        ++reads;
        if (mutate_ && !triggered && sector == u64{trigger_} * 2 && out.size() == 1024)
        {
            write_block(image_, trigger_, replacement_);
            triggered = true;
        }
        return {};
    }

    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("independent disk refuses write"); }
    Status flush() override
    { ++flushes; return Status::denied("independent disk refuses flush"); }

    void replace(u32 physical, const Block &block) { write_block(image_, physical, block); }

    bool triggered{};
    usize reads{}, writes{}, flushes{};

  private:
    Image image_;
    u32 trigger_{};
    Block replacement_{};
    bool mutate_{};
};

void run_case(const char *name, const Image &image, u32 trigger,
              const Block &replacement, bool mutate, bool expect_success)
{
    TriggerDisk disk(image, trigger, replacement, mutate);
    block::BlockSessions<1> registry{};
    const auto key = registry.register_device(disk, mutate ? 0x1202 : 0x1201);
    CHECK(key);
    auto storage = std::make_unique<Ext4OrphanSnapshotStorage>();
    auto scratch = std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
    Ext4OrphanSnapshotAdmission<1> admission{};
    const auto status = admission.open(registry, key.value(), {0, image.size() / 512},
                                       *storage, *scratch);
    const auto &report = admission.report();
    std::printf("%s: success=%d phase=%u code=%u message=%.*s triggered=%d reads=%zu writes=%zu flushes=%zu\n",
                name, bool(status), unsigned(report.phase), unsigned(status.code()),
                int(status.message().size()), status.message().data(), disk.triggered,
                disk.reads, disk.writes, disk.flushes);
    CHECK(bool(status) == expect_success);
    CHECK(!disk.writes && !disk.flushes);
    if (mutate) CHECK(disk.triggered);
    if (!status)
    {
        CHECK(!report.valid && report.phase == Ext4OrphanSnapshotPhase::rejected);
        const auto registration = registry.inspect(key.value());
        CHECK(registration && registration.value().state == block::DeviceState::quarantined);
        return;
    }
    CHECK(report.valid && report.head == 12);
    Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
    const auto planned = admission.plan_head(report.head, report.generation, plan);
    std::printf("%s plan: success=%d code=%u valid=%d\n",
                name, bool(planned), unsigned(planned.code()), plan.valid);
    CHECK(planned && plan.valid);
    CHECK(admission.close());
}

void run_late_bitmap_tail_drift(const Image &image)
{
    const Block descriptors = load(image, 2);
    const u32 bitmap_physical = value(descriptors, 4);
    TriggerDisk disk(image, 0, {}, false);
    block::BlockSessions<1> registry{};
    const auto key = registry.register_device(disk, 0x1203);
    CHECK(key);
    auto storage = std::make_unique<Ext4OrphanSnapshotStorage>();
    auto scratch = std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
    Ext4OrphanSnapshotAdmission<1> admission{};
    CHECK(admission.open(registry, key.value(), {0, image.size() / 512}, *storage, *scratch));
    Block changed = load(image, bitmap_physical);
    changed[300] ^= 1;
    disk.replace(bitmap_physical, changed);
    Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
    plan.valid = true;
    const auto status = admission.plan_head(admission.report().head,
                                            admission.report().generation, plan);
    const auto &report = admission.report();
    const auto registration = registry.inspect(key.value());
    std::printf("late-inode-bitmap-tail-drift: success=%d phase=%u code=%u message=%.*s plan_valid=%d writes=%zu flushes=%zu\n",
                bool(status), unsigned(report.phase), unsigned(status.code()),
                int(status.message().size()), status.message().data(), plan.valid,
                disk.writes, disk.flushes);
    CHECK(!status && !plan.valid && !report.valid &&
          report.phase == Ext4OrphanSnapshotPhase::rejected &&
          registration && registration.value().state == block::DeviceState::quarantined &&
          !disk.writes && !disk.flushes);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const Image image = prepared_image(argv[1]);
    const Block descriptors = load(image, 2);
    const u32 journal_table = value(descriptors, 8) + 1;
    Block changed = load(image, journal_table);
    auto inode = std::span<u8>{changed}.subspan(slot(8), 256);
    CHECK(ext4_checked::le16(inode, 0) == 0100600);
    store16(inode, 0, 0100644);
    const u32 seed = jbd2::detail::crc32c(~u32{0},
        std::span<const u8>{image}.subspan(1024 + 104, 16));
    inode[124] = inode[125] = inode[130] = inode[131] = 0;
    const u32 checksum = jbd2::detail::crc32c(
        ext4_checked::inode_seed(seed, 8, inode), inode);
    store16(inode, 124, u16(checksum));
    store16(inode, 130, u16(checksum >> 16));

    Image final_journal_image = image;
    write_block(final_journal_image, journal_table, changed);
    run_case("stable", image, journal_table, changed, false, true);
    run_case("steady-invalid-journal-mode", final_journal_image,
             journal_table, changed, false, false);
    run_case("journal-mode-changes-after-bootstrap", image, journal_table, changed, true, false);

    Block changed_descriptor = descriptors;
    const u16 old_unused = ext4_checked::le16(changed_descriptor, 28);
    CHECK(old_unused > 0 && old_unused <= 2048);
    store16(changed_descriptor, 28, u16(old_unused - 1));
    store16(changed_descriptor, 30, 0);
    store16(changed_descriptor, 30, u16(jbd2::detail::crc32c(
        jbd2::detail::crc32c(seed, ext4_checked::encoded(0)),
        std::span<const u8>{changed_descriptor}.first(32))));
    run_case("descriptor-changes-after-bootstrap", image, 2, changed_descriptor, true, false);
    run_late_bitmap_tail_drift(image);
    std::puts("PASS bootstrap structural drift: accepted=1 rejected=4 external_write_flush=0");
    return 0;
}
