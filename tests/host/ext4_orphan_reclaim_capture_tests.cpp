#include "ok/fs/ext4_orphan_reclaim_capture.hpp"
#include "semantic_fixture.hpp"
#include <limits>

namespace
{
using Sessions = block::BlockSessions<4>;
using Capture = Ext4OrphanReclaimCapture<4>;
constexpr u64 origin = 37;

Image committed(const Scenario &scenario, unsigned mask)
{
    auto image = scenario.mixture(mask);
    const u32 first = u32(scenario.old_binding.layout.journal_first_block);
    const auto &journal = scenario.journal;
    apply(image, first, journal.journal_head); apply(image, first + 1, journal.descriptor);
    for (usize index = 0; index < journal.count; ++index) apply(image, first + 2 + u32(index), journal.payloads[index]);
    apply(image, first + 2 + u32(journal.count), journal.commit);
    apply(image, first + 3 + u32(journal.count), journal.end);
    return image;
}

class Disk final : public driver::BlockDevice
{
  public:
    explicit Disk(const Image &source) : bytes(origin * 512 + source.size(), 0x55)
    { std::copy(source.begin(), source.end(), bytes.begin() + origin * 512); }
    driver::BlockGeometry geometry() const override { return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads;
        if (reads == fail_at) return Status::io_error("capture injected read fault");
        if (first < origin || first >= bytes.size() / 512 || out.size() > bytes.size() - first * 512)
            return Status::io_error("capture physical partition bounds");
        const u32 block = u32((first - origin) / 2);
        if (first == origin + u64{drift_block} * 2 && ++drift_reads == 2)
            bytes[origin * 512 + usize{block} * 1024 + 900] ^= 1;
        for (usize at = 0; at < out.size(); ++at) out[at] = std::byte(bytes[first * 512 + at]);
        return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> in) override
    {
        ++writes;
        if (first < origin || first >= bytes.size() / 512 || in.size() > bytes.size() - first * 512)
            return Status::io_error("capture physical write bounds");
        for (usize at = 0; at < in.size(); ++at) bytes[first * 512 + at] = u8(in[at]);
        return {};
    }
    Status flush() override { ++flushes; return {}; }
    void corrupt(u32 physical) { bytes[origin * 512 + usize{physical} * 1024 + 900] ^= 1; }
    void prefix() const { for (usize at = 0; at < origin * 512; ++at) CHECK(bytes[at] == 0x55); }
    Image bytes;
    usize reads{}, writes{}, flushes{}, fail_at{~usize{0}}, drift_reads{};
    u32 drift_block{~u32{0}};
};

struct Harness
{
    Disk disk;
    Sessions registry;
    Sessions::Session session;
    std::unique_ptr<Capture> capture = std::make_unique<Capture>();
    std::unique_ptr<Ext4OrphanReclaimCaptureStorage> storage = std::make_unique<Ext4OrphanReclaimCaptureStorage>();
    std::unique_ptr<Ext4OrphanReclaimColdScratch> scratch = std::make_unique<Ext4OrphanReclaimColdScratch>();
    Sessions::Registration registration{};
    Harness(const Image &image, bool writable = true) : disk(image)
    {
        const auto key = registry.register_device(disk, 91); CHECK(key); registration = key.value();
        CHECK(registry.acquire(registration, {origin, 65536},
            writable ? block::SessionAccess::writable : block::SessionAccess::readonly, session));
    }
    Status run(const Scenario &scenario)
    { return capture->capture(session, scenario.active(scenario.journal), *storage, *scratch); }
    void rejected()
    {
        CHECK(!capture->report().valid && !capture->report().semantic.valid);
        CHECK(capture->report().phase == Ext4OrphanReclaimCapturePhase::rejected);
        const auto report = registry.inspect(registration); CHECK(report);
        CHECK(report.value().state == block::DeviceState::quarantined);
        CHECK(!session.close()); disk.prefix();
    }
};

usize accepted_capture{}, barrier_capture{}, faults{}, raw_drift{}, counter_rejects{};
bool allowed(usize count, unsigned mask)
{ return mask == 0 || ((mask & 1) && (!(mask & (1U << (count - 1))) || mask == ((1U << count) - 1))); }

void matrix(Scenario &scenario)
{
    for (unsigned mask = 0; mask < (1U << scenario.journal.count); ++mask)
    {
        Harness test(committed(scenario, mask));
        const auto status = test.run(scenario);
        if (bool(status) != allowed(scenario.journal.count, mask))
        {
            std::fprintf(stderr, "capture mask=%u count=%zu status=%u %.*s\n", mask, scenario.journal.count,
                unsigned(status.code()), int(status.message().size()), status.message().data()); std::abort();
        }
        CHECK(test.disk.writes == 0 && test.disk.flushes == 0);
        if (!status) { test.rejected(); ++barrier_capture; continue; }
        CHECK(test.capture->report().valid && test.capture->report().semantic.valid);
        CHECK(test.capture->report().table_blocks == 512 && test.capture->report().directory_blocks > 0);
        CHECK(test.capture->report().original.session_serial == test.capture->report().observed.session_serial);
        CHECK(test.capture->revalidate());
        CHECK(!test.run(scenario)); // one-shot refusal does not discard admission
        CHECK(test.capture->report().valid); CHECK(test.session.close()); test.disk.prefix(); ++accepted_capture;
    }
}

void hostile(Scenario &scenario)
{
    const auto image = committed(scenario, 0);
    // An already-flushed writable owner may have nonzero transport counters.
    {
        Harness test(image); const auto bytes = load(image, 1);
        CHECK(test.session.write_blocks(2, std::as_bytes(std::span{bytes}))); CHECK(test.session.flush());
        CHECK(test.run(scenario)); CHECK(test.capture->report().original.writes == 1);
        CHECK(test.capture->report().original.flushes == 1); CHECK(test.capture->revalidate());
        CHECK(test.disk.writes == 1 && test.disk.flushes == 1); CHECK(test.session.close()); ++accepted_capture;
    }
    {
        Harness test(image, false); CHECK(test.run(scenario)); CHECK(test.capture->revalidate());
        CHECK(test.disk.writes == 0 && test.disk.flushes == 0); CHECK(test.session.close()); ++accepted_capture;
    }
    for (usize read : {usize{1}, usize{4}, usize{8}, usize{11}, usize{20}, usize{500}, usize{530}})
    {
        Harness test(image); test.disk.fail_at = read; CHECK(!test.run(scenario)); test.rejected();
        CHECK(test.disk.writes == 0 && test.disk.flushes == 0); ++faults;
    }
    // Semantically ignored INODE_UNINIT bitmap padding is still byte-sealed.
    const u32 raw = u32(scenario.old_binding.metadata[6].first);
    {
        Harness test(image); test.disk.drift_block = raw; CHECK(!test.run(scenario)); test.rejected(); ++raw_drift;
    }
    for (u32 physical : {u32{1}, u32{2}, raw, u32(scenario.old_binding.layout.journal_first_block),
        u32(scenario.old_binding.metadata[3].first), u32{512}})
    {
        Harness test(image); CHECK(test.run(scenario));
        // Select the actual captured root directory instead of an arbitrary data block.
        if (physical == 512) physical = test.storage->directories[0].physical;
        test.disk.corrupt(physical); CHECK(!test.capture->revalidate()); test.rejected(); ++raw_drift;
    }
    {
        Harness test(image); CHECK(test.run(scenario)); Block other{};
        CHECK(test.session.read_blocks(2, std::as_writable_bytes(std::span{other})));
        CHECK(!test.capture->revalidate()); test.rejected(); ++counter_rejects;
    }
    {
        Harness test(image); CHECK(test.run(scenario)); const auto bytes = load(image, 1);
        CHECK(test.session.write_blocks(2, std::as_bytes(std::span{bytes}))); CHECK(test.session.flush());
        CHECK(!test.capture->revalidate()); test.rejected(); ++counter_rejects;
    }
    {
        Harness test(image); const auto bytes = load(image, 1);
        CHECK(test.session.write_blocks(2, std::as_bytes(std::span{bytes})));
        CHECK(!test.run(scenario)); test.rejected(); CHECK(test.disk.flushes == 0); ++counter_rejects;
    }
    {
        Harness test(image); auto binding = scenario.active(scenario.journal); binding.layout.first_sector = 17;
        CHECK(!test.capture->capture(test.session, binding, *test.storage, *test.scratch)); test.rejected(); ++counter_rejects;
    }
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    Scenario filled(read_image(argv[1]), true); matrix(filled); hostile(filled);
    Scenario empty(read_image(argv[2]), false); matrix(empty); hostile(empty);
    std::printf("PASS existing-session reclaim capture: accepted=%zu barrier_rejects=%zu read_faults=%zu raw_drift_rejects=%zu counter_rejects=%zu partition_origin=37 capture_write_flush=0\n",
        accepted_capture, barrier_capture, faults, raw_drift, counter_rejects);
}
