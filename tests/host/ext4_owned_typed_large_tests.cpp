#include <ok/fs/ext4_owned_transaction.hpp>
#include "ext4_owned_typed_allocated_large_fixture.hpp"
#include "ext4_owned_typed_zero_large_fixture.hpp"
#include <memory>

using namespace ok;
using namespace ok::fs;
namespace
{
using Owner = Ext4OwnedTransaction<2, 32>;
using Block = Ext4FirstDataBlock;
using Workspace = Owner::TypedOwnerWorkspace;
usize checks{}, positives{};
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "large typed owner:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)
template <typename Image> struct Device final : driver::BlockDevice
{
    Image &source;
    const bool recovery;
    struct Record { u32 block{}; Block image{}; };
    std::array<Record, 32> written{};
    usize changed{}, reads{}, writes{}, flushes{}, directories{};
    Block scratch{};
    explicit Device(Image &image, bool recover) : source(image), recovery(recover) {}
    driver::BlockGeometry geometry() const override
    { return {11 + source.binding.layout.first_sector + source.binding.layout.filesystem_blocks * 2 + 8, 512, true}; }
    void render(u32 block, Block &out)
    {
        for (usize at = 0; at < changed; ++at) if (written[at].block == block) { out = written[at].image; return; }
        source.render(block, out);
        if constexpr (std::same_as<Image, typed_owner_zero_large_fixture::Image>)
            if (!recovery && block >= 1000 && block < 1008) out = block == 1000 ? source.old_head : Block{};
    }
    Result<u32> number(u64 sector, usize bytes)
    {
        const auto origin = 11 + source.binding.layout.first_sector;
        if (sector < origin || (sector - origin) % 2 || bytes != 1024 ||
            (sector - origin) / 2 >= source.binding.layout.filesystem_blocks)
            return Status::invalid_argument("large owner synthetic media bounds");
        return u32((sector - origin) / 2);
    }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        ++reads; const auto block = number(sector, out.size()); if (!block) return block.status();
        if (block.value() >= 3000 && block.value() < 4024) ++directories;
        render(block.value(), scratch);
        for (usize at = 0; at < 1024; ++at) out[at] = std::byte(scratch[at]);
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> in) override
    {
        ++writes; const auto block = number(sector, in.size()); if (!block) return block.status();
        usize slot = 0; while (slot < changed && written[slot].block != block.value()) ++slot;
        if (slot == changed) { CHECK(changed < written.size()); ++changed; written[slot].block = block.value(); }
        for (usize at = 0; at < 1024; ++at) written[slot].image[at] = std::to_integer<u8>(in[at]);
        return {};
    }
    Status flush() override { ++flushes; return {}; }
};
usize pop(u8 mask) { usize count{}; for (u32 at = 0; at < 4; ++at) count += (mask >> at) & 1; return count; }
template <typename Image>
void run(Image &image, bool recovery, u8 mask)
{
    constexpr bool allocated = std::same_as<Image, typed_owner_allocated_large_fixture::Image>;
    if constexpr (allocated) image.initialize(32, 1024, 128, recovery, mask, 42, 70, 73, 7, 4);
    else
    {
        image.initialize(false); image.mask = mask;
        if (!recovery) { image.binding.empty_log = true; image.binding.journal_sequence = 70; }
    }
    CHECK(image.binding.metadata_count == 128 && image.allocated_records == 128);
    auto owner_storage = std::unique_ptr<Owner>(new Owner(true)); auto &owner = *owner_storage;
    auto workspace_storage = std::unique_ptr<Workspace>(new Workspace); auto &workspace = *workspace_storage;
    auto device_storage = std::unique_ptr<Device<Image>>(new Device<Image>(image, recovery)); auto &device = *device_storage;
    Owner::Registry registry;
    const auto key = registry.register_device(device, 31); CHECK(key);
    CHECK(owner.open(registry, key.value(), {11, device.geometry().block_count - 11}));
    std::array<Owner::Pin, 32> pins{};
    usize count{};
    auto retain = [&](u32 physical) {
        CHECK(count < pins.size()); CHECK(owner.pin(physical + image.binding.layout.first_sector / 2, pins[count])); ++count;
    };
    for (u32 record = 0; record < 8; ++record) retain(1000 + record);
    for (u32 physical : image.plan.targets) retain(physical);
    if constexpr (allocated) retain(image.candidate);
    else for (u32 at = 0; at < image.plan.released_count; ++at) retain(image.plan.released_mapping[at]);
    retain(69);
    for (usize at = 0; at < count; ++at) CHECK(pins[at].release());
    const auto initial = owner.session_report().value();
    const auto cache_before = owner.cache_.entries_;
    device.reads = device.writes = device.flushes = device.directories = 0;
    Status status;
    if constexpr (allocated)
        status = owner.run_typed_allocated(workspace, image.binding,
            {image.target, 9, 73, 1700000999, 999999999}, 7, image.data, recovery);
    else status = owner.run_typed_zero(workspace, image.binding,
        {13, 9, 15361, 1700000111, 765432100}, recovery);
    CHECK(status && workspace.consumed_ && workspace.phase_ == Owner::TypedOwnerPhase::published);
    const auto actual = owner.session_report().value();
    CHECK(actual.state == block::DeviceState::owned && !actual.pending_writes && !actual.in_flight);
    CHECK(actual.reads - initial.reads == device.reads && actual.completed_reads - initial.completed_reads == device.reads);
    CHECK(device.writes == (recovery ? 5 - pop(mask) : allocated ? 14 : 13));
    CHECK(device.flushes == (recovery ? 3 : allocated ? 7 : 6));
    CHECK(workspace.prefix_reads_ == count && count == (allocated ? 14U : 29U));
    CHECK(owner.report().completion.journal_sequence == 72);
    for (usize at = 0; at < 32; ++at)
    {
        const auto &old = cache_before[at]; const auto &now = owner.cache_.entries_[at];
        bool retired{};
        if constexpr (!allocated)
            for (u32 physical : image.plan.released_mapping)
                retired |= old.report.state == MetadataBufferState::ready &&
                    old.report.block == physical + image.binding.layout.first_sector / 2;
        if (retired) CHECK(now.report.state == MetadataBufferState::unused && now.report.generation == old.report.generation + 1);
        else if (old.report.state == MetadataBufferState::ready)
        {
            Block physical{}; device.render(u32(old.report.block - image.binding.layout.first_sector / 2), physical);
            for (usize byte = 0; byte < 1024; ++byte) CHECK(now.canonical[byte] == std::byte(physical[byte]));
            if (old.report.block == 69 + image.binding.layout.first_sector / 2)
                CHECK(Owner::Cache::same_report(old.report, now.report) && old.staged == now.staged);
        }
        else CHECK(Owner::Cache::same_report(old.report, now.report) && old.canonical == now.canonical && old.staged == now.staged);
    }
    for (usize at = 0; at < device.changed; ++at)
    {
        const auto physical = device.written[at].block;
        bool allowed = physical >= 1000 && physical < 1008;
        for (u32 home : image.plan.targets) allowed |= physical == home;
        if constexpr (allocated) allowed |= physical == image.candidate;
        CHECK(allowed);
    }
    CHECK(owner.close() && registry.detach(key.value())); ++positives;
    std::printf("CANONICAL_OWNER_MAX allocated=%d recovery=%d mask=%u groups=32 records=128 roots=1024 prefix_reads=%zu reads=%zu writes=%zu flushes=%zu\n",
        allocated, recovery, u32(mask), count, device.reads, device.writes, device.flushes);
}
}
int main()
{
    auto allocated = std::unique_ptr<typed_owner_allocated_large_fixture::Image>(new typed_owner_allocated_large_fixture::Image);
    auto zero = std::unique_ptr<typed_owner_zero_large_fixture::Image>(new typed_owner_zero_large_fixture::Image);
    run(*allocated, false, 0); run(*zero, false, 0);
    for (u8 mask = 0; mask < 16; ++mask) { run(*allocated, true, mask); run(*zero, true, mask); }
    std::printf("CANONICAL_OWNER_LARGE checks=%zu positives=%zu actual_ownership_bounds=yes source_only=yes\n", checks, positives);
}
