#include "ok/fs/ext4_orphan_chain_capture.hpp"
#include "snapshot_fixture.hpp"
#include <memory>

namespace
{
using Registry = block::BlockSessions<2>;
using Capture = Ext4OrphanChainCapture<2>;
constexpr u64 origin = 37;

void save_block(Image &image, u32 physical, const Block &bytes)
{
    CHECK(physical < image.size() / 1024);
    for (usize index = 0; index < 1024; ++index)
        image[usize{physical} * 1024 + index] = bytes[index];
}

Image orphan_image(const Image &original, bool populated)
{
    MemoryDisk disk(original);
    Ext4RecoveryBinding admission;
    CHECK(admission.inspect(disk, {0, original.size() / 512}));
    CHECK(admission.info());
    Fixture fixture(original, *admission.info());
    if (populated) fixture.add(original, 12, 14);
    fixture.add(original, 14, 0);
    fixture.head(populated ? 12 : 14);
    store32(fixture.super, 96, 0x46);
    fixture.stamp_super();
    const Tables tables(original, fixture);
    DirectoryImages directories(original, fixture, tables);
    if (populated) directories.remove_target(fixture, tables, 12);
    directories.remove_target(fixture, tables, 14);
    Image result = original;
    save_block(result, 1, fixture.super);
    for (usize index = 0; index < fixture.count; ++index)
        save_block(result, fixture.images[index].table_block, fixture.images[index].table);
    for (const auto &entry : directories.blocks)
        save_block(result, entry.physical, entry.bytes);
    return result;
}

class Disk final : public driver::BlockDevice
{
  public:
    explicit Disk(const Image &image) : bytes(origin * 512, 0xa5)
    { bytes.insert(bytes.end(), image.begin(), image.end()); }
    driver::BlockGeometry geometry() const override { return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads;
        if (reads == fail_at) return Status::io_error("borrowed capture injected read fault");
        if (first < origin || first >= bytes.size() / 512 || out.size() > bytes.size() - first * 512)
            return Status::io_error("borrowed capture physical read bounds");
        for (usize at = 0; at < out.size(); ++at) out[at] = std::byte(bytes[first * 512 + at]);
        return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> input) override
    {
        ++writes;
        if (first < origin || first >= bytes.size() / 512 || input.size() > bytes.size() - first * 512)
            return Status::io_error("borrowed capture physical write bounds");
        for (usize at = 0; at < input.size(); ++at) bytes[first * 512 + at] = u8(input[at]);
        return {};
    }
    Status flush() override { ++flushes; return {}; }
    void corrupt(u32 physical)
    { bytes[origin * 512 + usize{physical} * 1024 + 900] ^= 1; }
    Image bytes;
    usize reads{}, writes{}, flushes{}, fail_at{~usize{0}};
};

struct Harness
{
    Disk disk;
    Registry registry;
    Registry::Session session;
    Capture capture;
    Registry::Registration key{};
    std::unique_ptr<Ext4OrphanChainCaptureStorage> storage =
        std::make_unique<Ext4OrphanChainCaptureStorage>();
    std::unique_ptr<Ext4OrphanDirectoryQualifiedReclaimScratch> scratch =
        std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
    Harness(const Image &image, block::SessionAccess access = block::SessionAccess::writable)
        : disk(image)
    {
        const auto registered = registry.register_device(disk, 919);
        CHECK(registered); key = registered.value();
        CHECK(registry.acquire(key, {origin, 65536}, access, session));
    }
    Status run() { return capture.capture(session, *storage, *scratch); }
    void rejected() const
    {
        CHECK(!capture.report().valid && capture.report().phase == Ext4OrphanSnapshotPhase::rejected);
        const auto report = registry.inspect(key); CHECK(report);
        CHECK(report.value().state == block::DeviceState::quarantined);
    }
};

usize admitted{}, rejected{};

void admitted_cases(const Image &image, bool populated)
{
    {
        Harness owner(image);
        Block super{};
        CHECK(owner.session.read_blocks(2, std::as_writable_bytes(std::span{super})));
        CHECK(owner.session.write_blocks(2, std::as_bytes(std::span{super})));
        CHECK(owner.session.flush());
        CHECK(owner.run());
        const auto report = owner.capture.report();
        CHECK(report.valid && report.head == (populated ? 12U : 14U));
        CHECK(report.original_io.reads == 1 && report.original_io.completed_reads == 1);
        CHECK(report.original_io.writes == 1 && report.original_io.completed_writes == 1 &&
              report.original_io.flushes == 1 && report.original_io.completed_flushes == 1);
        CHECK(owner.registry.inspect(owner.key).value().state == block::DeviceState::owned);
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
        CHECK(owner.capture.plan_head(report.head, report.generation, plan));
        CHECK(plan.valid && plan.reclaim.count == (populated ? 5U : 4U));
        CHECK(owner.disk.writes == 1 && owner.disk.flushes == 1);
        CHECK(owner.session.close());
        ++admitted;
    }
    {
        Harness owner(image, block::SessionAccess::readonly);
        CHECK(owner.run());
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
        const auto report = owner.capture.report();
        CHECK(owner.capture.plan_head(report.head, report.generation, plan) && plan.valid);
        CHECK(owner.disk.writes == 0 && owner.disk.flushes == 0 && owner.session.close());
        ++admitted;
    }
}

void hostile_cases(const Image &image)
{
    {
        Harness owner(image);
        Block super{};
        CHECK(owner.session.read_blocks(2, std::as_writable_bytes(std::span{super})));
        CHECK(owner.run());
        CHECK(owner.session.read_blocks(2, std::as_writable_bytes(std::span{super})));
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
        const auto report = owner.capture.report();
        CHECK(!owner.capture.plan_head(report.head, report.generation, plan) && !plan.valid);
        owner.rejected(); ++rejected;
    }
    {
        Harness owner(image);
        CHECK(owner.run());
        Block super{};
        CHECK(owner.session.read_blocks(2, std::as_writable_bytes(std::span{super})));
        CHECK(owner.session.write_blocks(2, std::as_bytes(std::span{super})));
        CHECK(owner.session.flush());
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
        const auto report = owner.capture.report();
        CHECK(!owner.capture.plan_head(report.head, report.generation, plan) && !plan.valid);
        owner.rejected(); ++rejected;
    }
    {
        Harness owner(image);
        CHECK(owner.run());
        const u32 block = owner.storage->directories[0].physical;
        owner.disk.corrupt(block);
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
        const auto report = owner.capture.report();
        CHECK(!owner.capture.plan_head(report.head, report.generation, plan) && !plan.valid);
        owner.rejected(); ++rejected;
    }
    {
        Harness owner(image);
        owner.disk.fail_at = 20;
        CHECK(!owner.run()); owner.rejected(); ++rejected;
    }
    {
        Harness owner(image);
        Block super{};
        CHECK(owner.session.read_blocks(2, std::as_writable_bytes(std::span{super})));
        CHECK(owner.session.write_blocks(2, std::as_bytes(std::span{super})));
        CHECK(!owner.run()); owner.rejected(); ++rejected;
    }
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    const auto filled = orphan_image(read_image(argv[1]), true);
    const auto empty = orphan_image(read_image(argv[2]), false);
    admitted_cases(filled, true);
    admitted_cases(empty, false);
    hostile_cases(filled);
    hostile_cases(empty);
    std::printf("PASS borrowed orphan chain capture: admitted=%zu rejected=%zu existing_session=1 partition_origin=37\n",
                admitted, rejected);
}
