// This hosted adversarial model alone uses -fno-access-control. Production
// private owner, child and Session headers are compiled unchanged with access
// control enabled by the separate freestanding instantiation target.
#include "ext4_zero_truncate_preview_test_support.hpp"
#include <ok/fs/ext4_owned_transaction.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace zero_preview_fixture;
namespace
{
using Owner = Ext4OwnedTransaction<2, 32>;
using Workspace = Owner::TypedOwnerWorkspace;
using Registry = Owner::Registry;
using Session = Registry::Session;
using Action = Owner::TypedAction;
using Cache = Owner::Cache;
usize checks{}, positives{}, refusals{}, faults{}, drifts{}, callback_exclusions{}, foreign_controls{}, aliases{}, terminal_controls{}, delegated_failed_reads{}, recovery_drift_controls{};
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "typed owner:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)
std::array<Block, 8705> before{}, foreign_media{};
Ext4ZeroTruncatePlan zero_scratch{}, zero_plan{};
Ext4ZeroTruncateJournalFormatScratch zero_formatting{};
Ext4ZeroTruncateJournalFormat zero_journal{};
Ext4AllocatedEmptyDataScratch allocated_scratch{};
Ext4AllocatedEmptyDataPlan allocated_plan{};
Ext4AllocatedEmptyDataJournalFormatScratch allocated_formatting{};
Ext4AllocatedEmptyDataJournalFormat allocated_journal{};
Block data{};
static_assert(!std::is_copy_constructible_v<Owner> && !std::is_move_constructible_v<Owner>);
static_assert(!std::is_copy_constructible_v<Workspace> && !std::is_move_constructible_v<Workspace>);
void be32(Block &image, usize at, u32 value)
{ for (usize byte = 0; byte < 4; ++byte) image[at + byte] = u8(value >> (24 - 8 * byte)); }
u32 population(u8 mask)
{ u32 count{}; for (u32 bit = 0; bit < 4; ++bit) if (mask & (1U << bit)) ++count; return count; }
void media(Image &image, bool allocated, bool recovery, u8 mask, u32 count)
{
    auto &head = image.blocks[1000]; head = {};
    be32(head, 0, jbd2::magic); be32(head, 4, u32(jbd2::RecordType::superblock_v2));
    be32(head, 12, 1024); be32(head, 16, 1024); be32(head, 20, 1); be32(head, 24, 70);
    be32(head, 40, jbd2::feature_checksum_v3 | (allocated ? jbd2::feature_revoke : 0));
    be32(head, 64, 1); head[80] = 4;
    for (usize at = 0; at < 16; ++at) head[48 + at] = image.f.binding.uuid[at];
    be32(head, 252, jbd2::detail::crc32c(~u32{0}, head));
    image.f.binding.empty_log = true; image.f.binding.journal_sequence = 70;
    for (u32 record = 1; record < 8; ++record)
        for (usize at = 0; at < 1024; ++at) image.blocks[1000 + record][at] = u8(record * 23 + at * 7);
    for (u32 physical = 70; physical < 90; ++physical)
        if (physical != 71) for (usize at = 0; at < 1024; ++at) image.blocks[physical][at] = u8(physical + at * 19);
    data = {}; for (usize at = 7; at < 73; ++at) data[at] = u8(1 + (at * 17) % 251);
    if (allocated)
    {
        const Ext4AllocatedEmptyDataSource source{image.f.group, image.f.cross ? image.f.physical : 70, 8,
            image.blocks[1], image.blocks[2], image.blocks[image.f.cross ? 8195 : 3], image.blocks[4], image.blocks[8]};
        CHECK(Ext4AllocatedEmptyDataPlanner::plan_off_stack(image.f.binding, {13, 9, 73, 1700000222, 123456789},
            source, allocated_scratch, allocated_plan));
        CHECK(Ext4AllocatedEmptyDataJournalFormatter::format(image.f.binding, allocated_plan, head,
            image.blocks[4], allocated_formatting, allocated_journal));
    }
    else if (count)
    {
        const Ext4ZeroTruncateSource source{image.f.group, 8, image.blocks[1], image.blocks[2],
            image.blocks[image.f.cross ? 8195 : 3], image.blocks[4], image.blocks[8]};
        CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(image.f.binding, image.update(), source, zero_scratch, zero_plan));
        CHECK(Ext4ZeroTruncateJournalFormatter::format(image.f.binding, zero_plan, head, image.blocks[4], zero_formatting, zero_journal));
    }
    if (recovery)
    {
        CHECK(allocated || count);
        const auto &journal_head = allocated ? allocated_journal.head : zero_journal.head;
        image.blocks[1000] = journal_head;
        image.blocks[1001] = allocated ? allocated_journal.descriptor : zero_journal.descriptor;
        for (u32 at = 0; at < 4; ++at) image.blocks[1002 + at] = allocated ? allocated_journal.payloads[at] : zero_journal.payloads[at];
        image.blocks[1006] = allocated ? allocated_journal.commit : zero_journal.commit;
        image.blocks[1007] = allocated ? allocated_journal.end : zero_journal.end;
        for (u32 home = 0; home < 4; ++home)
        {
            const u32 target = allocated ? allocated_plan.targets[home] : zero_plan.targets[home];
            image.blocks[target] = allocated ? (mask & (1U << home) ? allocated_plan.after[home] : allocated_plan.before[home]) :
                (mask & (1U << home) ? zero_plan.after[home] : zero_plan.before[home]);
        }
        if (allocated) image.blocks[allocated_plan.physical] = data;
        image.f.binding.empty_log = false;
        image.f.binding.journal_sequence = allocated ? allocated_journal.active_sequence : zero_journal.active_sequence;
    }
    image.f.binding.layout.first_sector = 4;
}
struct Device final : driver::BlockDevice
{
    std::array<Block, 8705> &blocks;
    static constexpr u64 origin = 11, offset = 4, sectors = origin + offset + 8705 * 2 + 8;
    Owner *owner{}; Workspace *workspace{}; Session *foreign{};
    usize reads{}, writes{}, flushes{}, fail_number{}, hook_number{1};
    u32 fail_kind{}, hook_kind{}, mutation{};
    bool hooked{};
    Device(std::array<Block, 8705> &media) : blocks(media) {}
    driver::BlockGeometry geometry() const override { return {sectors, 512, true}; }
    void hook(u32 kind, usize number)
    {
        if (hooked || !mutation || hook_kind != kind || hook_number != number) return;
        hooked = true; CHECK(owner && workspace);
        if (mutation == 1)
        {
            const auto phase = owner->report_.phase;
            owner->report_.phase = Ext4OwnedPhase::open;
            Owner::Pin pin;
            CHECK(owner->pin(73 + offset / 2, pin).code() == StatusCode::busy);
            CHECK(owner->prepare({}).code() == StatusCode::busy);
            CHECK(owner->prepare_rename({}).code() == StatusCode::busy);
            CHECK(owner->prepare_create({}).code() == StatusCode::busy);
            CHECK(owner->prepare_first_data({}, 0, data).code() == StatusCode::busy);
            CHECK(owner->prepare_next_data({}, 0, data).code() == StatusCode::busy);
            CHECK(owner->prepare_namespace({}).code() == StatusCode::busy);
            CHECK(owner->discard().code() == StatusCode::busy);
            CHECK(owner->close().code() == StatusCode::busy);
            CHECK(owner->rearm().code() == StatusCode::busy);
            CHECK(owner->mounted_teardown_preflight().code() == StatusCode::busy);
            owner->report_.phase = Ext4OwnedPhase::prepared;
            CHECK(owner->commit().code() == StatusCode::busy);
            owner->report_.phase = Ext4OwnedPhase::durable;
            CHECK(owner->checkpoint().code() == StatusCode::busy);
            owner->report_.phase = phase;
            ++callback_exclusions;
        }
        if (mutation == 2) ++workspace->binding_.metadata[127].count;
        if (mutation == 3) ++owner->cache_.entries_[0].report.generation;
        if (mutation == 4) owner->cache_.entries_[0].canonical[37] ^= std::byte{1};
        if (mutation == 5) ++owner->cache_.session_.first_;
        if (mutation == 6) workspace->session_ = foreign;
        if (mutation == 7) { workspace->owner_ = nullptr; workspace->cache_ = nullptr; workspace->session_ = nullptr; workspace->original_ = nullptr; }
        if (mutation == 8) { workspace->zero_.control_.phase = Ext4ZeroTruncateTransactionPhase::complete; }
        if (mutation == 9) { workspace->allocated_.control_.phase = Ext4AllocatedEmptyDataTransactionPhase::complete; }
        if (mutation == 10) ++workspace->prefix_progress_.completed_reads;
        if (mutation == 11) ++workspace->child_initial_.reads;
        if (mutation == 12) { CHECK(foreign); foreign->geometry_.block_count -= 2; ++foreign_controls; }
        if (mutation == 13) ++workspace->prefix_.entries_[0].report.generation;
        if (mutation == 14) ++workspace->reconciliation_.final_reports_[0].generation;
        if (mutation == 15)
        {
            auto copied = workspace->allocated_.report();
            copied.phase = Ext4AllocatedEmptyDataTransactionPhase::complete;
            copied.final_sequence = 1234;
            CHECK(workspace->allocated_.report().final_sequence != copied.final_sequence);
        }
        if (mutation == 16) owner->report_.completion.journal_sequence = 4321;
        if (mutation == 17)
        {
            auto &session = owner->cache_.session_;
            session.service_->entries_[session.registration_.slot_].maximum_transfer += 512;
        }
        if (mutation == 18)
        {
            auto &session = owner->cache_.session_;
            CHECK(foreign);
            session.service_->entries_[session.registration_.slot_].device =
                foreign->service_->entries_[foreign->registration_.slot_].device;
        }
        if (mutation == 19)
        {
            owner->typed_attempted_ = false; owner->typed_completed_ = true;
            owner->report_.phase = Ext4OwnedPhase::open;
            Owner::Pin pin;
            CHECK(owner->pin(71, pin).code() == StatusCode::busy);
            CHECK(owner->close().code() == StatusCode::busy);
        }
        if (mutation == 20) owner->typed_context_ = nullptr;
        if (mutation == 21) { owner->report_.phase = Ext4OwnedPhase::checkpointed; CHECK(!owner->rearm()); }
        if (mutation == 22) data[9] ^= 1;
        if (mutation == 23) workspace->data_request_[9] ^= 1;
        if (mutation == 24) workspace->data_request_seal_[9] ^= 1;
        if (mutation == 25) workspace->prefix_snapshot_.entries_[0].canonical[53] ^= std::byte{1};
    }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        ++reads; hook(1, reads);
        if (fail_kind == 1 && fail_number == reads) return Status::io_error("typed owner selected READ failure");
        if (sector < origin + offset || (sector - origin - offset) % 2 || out.size() != 1024 ||
            (sector - origin - offset) / 2 >= blocks.size()) return Status::invalid_argument("typed owner model READ bounds");
        const auto &image = blocks[(sector - origin - offset) / 2];
        for (usize at = 0; at < 1024; ++at) out[at] = std::byte(image[at]);
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> in) override
    {
        ++writes; hook(2, writes);
        if (fail_kind == 2 && fail_number == writes) return Status::io_error("typed owner selected WRITE failure");
        if (sector < origin + offset || (sector - origin - offset) % 2 || in.size() != 1024 ||
            (sector - origin - offset) / 2 >= blocks.size()) return Status::invalid_argument("typed owner model WRITE bounds");
        auto &image = blocks[(sector - origin - offset) / 2];
        for (usize at = 0; at < 1024; ++at) image[at] = std::to_integer<u8>(in[at]);
        return {};
    }
    Status flush() override
    {
        ++flushes; hook(3, flushes);
        return fail_kind == 3 && fail_number == flushes ? Status::io_error("typed owner selected FLUSH failure") : Status{};
    }
};
struct Input { bool allocated{}, recovery{}, cross{}, resident{true}; u8 mask{}; u32 count{3}, fail_kind{}, mutation{}, hook_kind{}; usize fail_number{}, hook_number{1}; };
struct Counts { usize reads{}, writes{}, flushes{}, prefix{}, preview{}; };
Counts run(Input input)
{
    Image image(input.allocated ? 0 : input.count, input.cross);
    media(image, input.allocated, input.recovery, input.mask, input.count);
    before = foreign_media = image.blocks;
    Device device(image.blocks), other(foreign_media);
    Registry registry; Session foreign;
    auto storage = std::unique_ptr<Owner>(new Owner(true)); auto &owner = *storage;
    auto retained = std::unique_ptr<Workspace>(new Workspace); auto &workspace = *retained;
    const auto key = registry.register_device(device, 31), decoy = registry.register_device(other, 33);
    CHECK(key && decoy);
    CHECK(owner.open(registry, key.value(), {Device::origin, Device::sectors - Device::origin}));
    CHECK(registry.acquire(decoy.value(), {Device::origin, Device::sectors - Device::origin}, block::SessionAccess::writable, foreign));
    std::array<Owner::Pin, 32> pins{};
    usize residents{};
    auto retain = [&](u32 physical) {
        CHECK(residents < pins.size());
        CHECK(owner.pin(physical + Device::offset / 2, pins[residents])); ++residents;
    };
    if (input.resident)
    {
        for (u32 record = 0; record < 8; ++record) retain(1000 + record);
        for (u32 physical : {1U, 2U, image.f.cross ? 8195U : 3U, 8U}) retain(physical);
        if (input.allocated) retain(allocated_plan.physical);
        else if (input.count)
            for (u32 at = 0; at < zero_plan.released_count; ++at) retain(zero_plan.released_mapping[at]);
        retain(69);
        for (usize at = 0; at < residents; ++at) CHECK(pins[at].release());
    }
    const auto cache_before = owner.cache_.entries_;
    const auto cache_report_before = owner.buffers();
    const auto original = owner.session_report().value();
    const Block request_before = data;
    device.reads = device.writes = device.flushes = 0;
    device.owner = &owner; device.workspace = &workspace; device.foreign = &foreign;
    device.fail_kind = input.fail_kind; device.fail_number = input.fail_number;
    device.mutation = input.mutation; device.hook_kind = input.hook_kind; device.hook_number = input.hook_number;
    auto status = input.allocated ? owner.run_typed_allocated(workspace, image.f.binding,
        {13, 9, 73, 1700000222, 123456789}, 7, data, input.recovery) :
        owner.run_typed_zero(workspace, image.f.binding, image.update(), input.recovery);
    const auto actual = owner.session_report().value();
    const auto detached = [](const auto &child) { return child.owner_guard_retired_ && !child.owner_context_ &&
        !child.owner_context_seal_ && !child.owner_check_ && !child.owner_check_seal_; };
    CHECK(input.allocated ? detached(workspace.allocated_) : detached(workspace.zero_));
    CHECK(detached(workspace.allocated_.recovery_preview_) && detached(workspace.zero_.recovery_preview_));
    const auto delegated_failure = [&](const auto &child) {
        const auto &preview = child.recovery_preview_;
        using PreviewPhase = decltype(preview.report().phase);
        if (preview.report().phase != PreviewPhase::failed) return;
        CHECK(!child.report().reads && !child.report().completed_reads);
        if (preview.terminal_binding())
        {
            const auto report = preview.report();
            CHECK(report.attempts == actual.reads - preview.initial_.reads);
            CHECK(report.completions == actual.completed_reads - preview.initial_.completed_reads);
            CHECK(!report.dispatch_pending);
            CHECK(reclaim_history::same_session(report.observed, actual));
            ++delegated_failed_reads;
        }
    };
    if (input.recovery)
    {
        if (input.allocated) delegated_failure(workspace.allocated_);
        else delegated_failure(workspace.zero_);
    }
    CHECK(!actual.in_flight);
    CHECK(!other.reads && !other.writes && !other.flushes && foreign_media == before);
    CHECK(actual.reads - original.reads == device.reads);
    CHECK(actual.writes - original.writes == device.writes);
    CHECK(actual.flushes - original.flushes == device.flushes);
    if (input.fail_kind || (input.mutation && input.mutation != 1 && input.mutation != 12 &&
                           input.mutation != 15 && input.mutation != 16 && input.mutation != 21 && input.mutation != 22))
    {
        if (status || actual.state != block::DeviceState::quarantined)
            std::fprintf(stderr, "unexpected owner acceptance allocated=%d recovery=%d mutation=%u kind=%u hook=%zu fail=%u/%zu status=%u hooked=%d\n",
                input.allocated, input.recovery, input.mutation, input.hook_kind, input.hook_number,
                input.fail_kind, input.fail_number, u32(status.code()), device.hooked);
        CHECK(!status && actual.state == block::DeviceState::quarantined);
        if (input.mutation) CHECK(input.hook_kind == 1 ? device.reads == input.hook_number :
                                 input.hook_kind == 2 ? device.writes == input.hook_number :
                                                       device.flushes == input.hook_number);
        CHECK(owner.report().phase == Ext4OwnedPhase::failed && workspace.phase_ == Owner::TypedOwnerPhase::failed && workspace.consumed_);
        const auto io = device.reads + device.writes + device.flushes;
        CHECK(!owner.run_typed_zero(workspace, image.f.binding, image.update(), input.recovery));
        CHECK(!owner.close() && !owner.rearm());
        CHECK(io == device.reads + device.writes + device.flushes);
        if (input.fail_kind) ++faults;
        else { CHECK(device.hooked); ++drifts; }
    }
    else
    {
        CHECK(status && actual.state == block::DeviceState::owned && !actual.pending_writes);
        CHECK(workspace.phase_ == Owner::TypedOwnerPhase::published && workspace.consumed_ && owner.typed_completed_);
        CHECK(workspace.prefix_reads_ == residents && workspace.prefix_reservation_ == 32);
        CHECK(owner.report().phase == Ext4OwnedPhase::checkpointed);
        const auto idle_io = device.reads + device.writes + device.flushes;
        CHECK(input.allocated ? workspace.allocated_.idle() : workspace.zero_.idle());
        CHECK(idle_io == device.reads + device.writes + device.flushes);
        CHECK(owner.report().completion.journal_sequence == (input.allocated || input.count ? 72 : 70));
        CHECK(device.writes == (input.allocated ? input.recovery ? 5 - population(input.mask) : 14 :
            !input.count ? 0 : input.recovery ? 5 - population(input.mask) : 13));
        CHECK(device.flushes == (input.allocated ? input.recovery ? 3 : 7 : !input.count ? 0 : input.recovery ? 3 : 6));
        for (usize slot = 0; slot < 32; ++slot)
        {
            const auto &old = cache_before[slot]; const auto &now = owner.cache_.entries_[slot];
            if (old.report.state != MetadataBufferState::ready)
            { CHECK(Cache::same_report(old.report, now.report) && old.canonical == now.canonical && old.staged == now.staged); continue; }
            const u64 physical = old.report.block - Device::offset / 2;
            bool retired = false;
            if (!input.allocated && input.count)
                for (u32 at = 0; at < zero_plan.released_count; ++at) retired |= zero_plan.released_mapping[at] == physical;
            const bool replaced = (input.allocated || input.count) &&
                ((physical >= 1000 && physical < 1008) || physical == 1 || physical == 2 || physical == 8 ||
                 physical == (image.f.cross ? 8195U : 3U) || (input.allocated && physical == allocated_plan.physical));
            if (retired)
            { CHECK(now.report.state == MetadataBufferState::unused && now.report.generation == old.report.generation + 1); CHECK((now.canonical == std::array<std::byte, 1024>{})); }
            else if (replaced)
            {
                CHECK(now.report.state == MetadataBufferState::ready && now.report.block == old.report.block && now.report.generation == old.report.generation + 1);
                for (usize at = 0; at < 1024; ++at) CHECK(now.canonical[at] == std::byte(image.blocks[physical][at]));
                CHECK(now.staged == now.canonical);
            }
            else CHECK(Cache::same_report(old.report, now.report) && old.canonical == now.canonical && old.staged == now.staged);
        }
        for (u32 physical = 0; physical < image.blocks.size(); ++physical)
        {
            bool affected = (input.allocated || input.count) && (physical >= 1000 && physical < 1008);
            if (input.allocated)
            { affected |= physical == allocated_plan.physical; for (u32 target : allocated_plan.targets) affected |= physical == target; }
            else if (input.count) for (u32 target : zero_plan.targets) affected |= physical == target;
            if (!affected) CHECK(image.blocks[physical] == before[physical]);
        }
        if (input.allocated) CHECK(image.blocks[allocated_plan.physical] == request_before);
        if (input.allocated || input.count) CHECK(jbd2::detail::be32(image.blocks[1000], 28) == 0);
        CHECK(owner.buffers().resident_blocks == cache_report_before.resident_blocks -
            (input.resident && !input.allocated && input.count ? zero_plan.released_count : 0));
        const auto io = device.reads + device.writes + device.flushes;
        CHECK(!owner.run_typed_zero(workspace, image.f.binding, image.update(), input.recovery));
        CHECK(io == device.reads + device.writes + device.flushes);
        CHECK(owner.rearm() && owner.report().phase == Ext4OwnedPhase::open);
        CHECK(owner.close()); CHECK(registry.detach(key.value())); ++positives;
    }
    CHECK(foreign.close()); CHECK(registry.detach(decoy.value()));
    return {device.reads, device.writes, device.flushes, workspace.prefix_reads_, input.recovery ?
        input.allocated ? workspace.allocated_.recovery_preview_.report().attempts :
                          workspace.zero_.recovery_preview_.report().attempts : 0};
}
void admission_controls()
{
    for (bool allocated : {false, true}) for (u32 field = 0; field < 10; ++field)
    {
        Image image(allocated ? 0 : 3); media(image, allocated, false, 0, 3);
        Device device(image.blocks); Registry registry;
        auto storage = std::unique_ptr<Owner>(new Owner(true)); auto &owner = *storage;
        auto retained = std::unique_ptr<Workspace>(new Workspace); auto &workspace = *retained;
        const auto key = registry.register_device(device, 31); CHECK(key);
        CHECK(owner.open(registry, key.value(), {Device::origin, Device::sectors - Device::origin}));
        Owner::Pin pin; CHECK(owner.pin(69 + Device::offset / 2, pin)); CHECK(pin.release());
        auto &entry = registry.entries_[owner.cache_.session_.registration_.slot_];
        const auto initial = entry.report;
        const auto groups = u32(image.f.binding.metadata_count / 4);
        const usize child_reads = allocated ? 5884 + 2125 * usize(groups) : 5849 + 2124 * usize(groups);
        const usize child_writes = allocated ? 14 : 13, child_flushes = allocated ? 7 : 6;
        if (field == 0) entry.report.reads = entry.report.completed_reads = ~usize{0} - child_reads - 31;
        if (field == 1) entry.report.writes = entry.report.completed_writes = ~usize{0} - child_writes + 1;
        if (field == 2) entry.report.flushes = entry.report.completed_flushes = ~usize{0} - child_flushes + 1;
        if (field == 3) ++entry.report.completed_reads;
        if (field == 4) ++entry.report.completed_writes;
        if (field == 5) ++entry.report.completed_flushes;
        if (field == 6) image.f.binding.layout.first_sector = 5;
        if (field == 7) image.f.binding.metadata_count = 129;
        if (field == 8) workspace.binding_ = image.f.binding;
        if (!allocated && field == 9) image.f.binding.metadata_count = 0;
        const auto before_reads = device.reads;
        const auto &binding = field == 8 ? workspace.binding_ : image.f.binding;
        const auto &image_request = field == 9 ? workspace.checking_ : data;
        const auto status = allocated ? owner.run_typed_allocated(workspace, binding,
            {13, 9, 73, 1700000222, 123456789}, 7, image_request) :
            owner.run_typed_zero(workspace, binding, image.update());
        {
            CHECK(!status && !workspace.attempted_ && !owner.typed_attempted_);
            CHECK(device.reads == before_reads && !device.writes && !device.flushes);
            CHECK(owner.report().phase == Ext4OwnedPhase::open && owner.buffers().state == MetadataOwnerState::ready);
            entry.report = initial; CHECK(owner.close()); ++refusals;
        }
        CHECK(registry.detach(key.value()));
    }
}
// Snapshot complete actual object representations off stack. Refused aliases
// must not change any caller, owner, workspace, Registry or media byte.
template <typename T> std::unique_ptr<std::byte[]> snapshot_bytes(const T &value)
{
    auto result = std::unique_ptr<std::byte[]>(new std::byte[sizeof(value)]);
    std::memcpy(result.get(), &value, sizeof(value));
    return result;
}
void alias_controls()
{
    for (bool recovery : {false, true}) for (u32 field = 0; field < 9; ++field)
    {
        Image image(0); media(image, true, recovery, 0, 0);
        Device device(image.blocks); Registry registry;
        auto storage = std::unique_ptr<Owner>(new Owner(true)); auto &owner = *storage;
        auto retained = std::unique_ptr<Workspace>(new Workspace); auto &workspace = *retained;
        const auto key = registry.register_device(device, 31); CHECK(key);
        CHECK(owner.open(registry, key.value(), {Device::origin, Device::sectors - Device::origin}));
        workspace.binding_ = workspace.zero_.binding_ = image.f.binding;
        owner.reclaim_capture_.binding_ = image.f.binding;
        workspace.checking_ = workspace.data_request_ = owner.data_image_ = data;
        const Ext4RecoveryInfo *binding = &image.f.binding;
        const Block *request = &data;
        Workspace *submitted = &workspace;
        if (field == 0) binding = &workspace.binding_;
        if (field == 1) binding = &owner.reclaim_capture_.binding_;
        if (field == 2) request = &workspace.checking_;
        if (field == 3) request = &owner.data_image_;
        // These fabricated typed references are only addresses: production
        // must reject overlap before reading or writing the alleged objects.
        if (field == 4) request = reinterpret_cast<const Block *>(binding);
        if (field == 5) submitted = reinterpret_cast<Workspace *>(&owner);
        if (field == 6) submitted = reinterpret_cast<Workspace *>(
            reinterpret_cast<std::byte *>(&owner) + alignof(Workspace));
        if (field == 7) binding = &workspace.zero_.binding_;
        if (field == 8) request = &workspace.data_request_;
        const auto owner_before = snapshot_bytes(owner), workspace_before = snapshot_bytes(workspace);
        const auto registry_before = snapshot_bytes(registry), image_before = snapshot_bytes(image);
        const auto data_before = snapshot_bytes(data);
        const auto reads = device.reads;
        const auto status = owner.run_typed_allocated(*submitted, *binding,
            {13, 9, 73, 1700000222, 123456789}, 7, *request, recovery);
        CHECK(!status && status.code() == StatusCode::invalid_argument);
        CHECK(std::memcmp(owner_before.get(), &owner, sizeof(owner)) == 0);
        CHECK(std::memcmp(workspace_before.get(), &workspace, sizeof(workspace)) == 0);
        CHECK(std::memcmp(registry_before.get(), &registry, sizeof(registry)) == 0);
        CHECK(std::memcmp(image_before.get(), &image, sizeof(image)) == 0);
        CHECK(std::memcmp(data_before.get(), &data, sizeof(data)) == 0);
        CHECK(device.reads == reads && !device.writes && !device.flushes);
        CHECK(owner.close()); CHECK(registry.detach(key.value())); ++aliases;
    }
}
void terminal_observation_controls()
{
    for (u32 field = 0; field < 10; ++field)
    {
        Image image(0); media(image, true, false, 0, 0);
        Device original(image.blocks), foreign_device(image.blocks);
        Registry registry, foreign_registry; Session session, foreign;
        const auto key = registry.register_device(original, 31), other = foreign_registry.register_device(foreign_device, 31);
        CHECK(key && other);
        const block::SessionBounds bounds{Device::origin, Device::sectors - Device::origin};
        CHECK(registry.acquire(key.value(), bounds, block::SessionAccess::writable, session));
        CHECK(foreign_registry.acquire(other.value(), bounds, block::SessionAccess::writable, foreign));
        const auto opening = session.observe_binding(); CHECK(opening && !session.observe_terminal_binding());
        original.fail_kind = 1; original.fail_number = 1;
        Block scratch{};
        CHECK(!session.read_blocks(Device::offset, std::as_writable_bytes(std::span{scratch})));
        const auto failed = session.report(); CHECK(failed && failed.value().state == block::DeviceState::quarantined);
        CHECK(!session.observe_binding());
        const auto terminal = session.observe_terminal_binding(); CHECK(terminal && terminal.value() == opening.value());
        auto &entry = registry.entries_[session.registration_.slot_];
        if (field == 1) entry.device = &foreign_device;
        if (field == 2) ++entry.maximum_transfer;
        if (field == 3) --entry.geometry.block_count;
        if (field == 4) ++session.first_;
        if (field == 5) --session.geometry_.block_count;
        if (field == 6) session.geometry_.writable = false;
        if (field == 7) ++entry.report.identity;
        if (field == 8) ++entry.report.registration_generation;
        if (field == 9)
        {
            CHECK(foreign.abort(Status::io_error("author same-number foreign quarantine")));
            auto &foreign_entry = foreign_registry.entries_[foreign.registration_.slot_];
            foreign_entry.report = failed.value();
            session.service_ = foreign.service_; session.registration_ = foreign.registration_; session.serial_ = foreign.serial_;
            CHECK(reclaim_history::same_session(session.report().value(), failed.value()));
        }
        const auto actual = session.observe_terminal_binding();
        CHECK(field == 0 ? actual && actual.value() == opening.value() : !actual || actual.value() != opening.value());
        const auto original_reads = original.reads, foreign_reads = foreign_device.reads;
        CHECK(!session.read_blocks(Device::offset, std::as_writable_bytes(std::span{scratch})));
        CHECK(!session.write_blocks(Device::offset, std::as_bytes(std::span{scratch})));
        CHECK(!session.flush() && !session.close());
        CHECK(original.reads == original_reads && foreign_device.reads == foreign_reads);
        CHECK(!original.writes && !original.flushes && !foreign_device.writes && !foreign_device.flushes);
        if (field != 9) CHECK(foreign.close());
        ++terminal_controls;
    }
}
}
int main()
{
    const auto zero = run({}); const auto allocated = run({.allocated=true}); run({.count=0});
    run({.count=16}); run({.cross=true}); run({.allocated=true, .cross=true});
    run({.resident=false}); run({.allocated=true, .resident=false}); run({.resident=false, .count=0});
    for (u8 mask = 0; mask < 16; ++mask)
    {
        run({.recovery=true, .mask=mask});
        run({.allocated=true, .recovery=true, .mask=mask});
        run({.recovery=true, .resident=false, .mask=mask, .count=16});
        run({.allocated=true, .recovery=true, .resident=false, .mask=mask});
    }
    for (bool is_allocated : {false, true})
    {
        const auto recovery = run({.allocated=is_allocated, .recovery=true});
        CHECK(recovery.preview);
        for (usize at = 1; at <= recovery.preview; ++at)
            run({.allocated=is_allocated, .recovery=true, .fail_kind=1, .fail_number=recovery.prefix + at});
        // First and last delegated READs must retain every original outer
        // context, cache prefix and parent seal; refusal stops at that callback.
        for (usize at : {usize{1}, recovery.preview})
        {
            for (u32 mutation : {2U, 3U, 4U, 5U, 6U, 7U, 10U, 11U, 13U, 17U, 18U, 19U, 20U, 25U})
            {
                run({.allocated=is_allocated, .recovery=true, .mutation=mutation, .hook_kind=1,
                    .hook_number=recovery.prefix + at}); ++recovery_drift_controls;
            }
            run({.allocated=is_allocated, .recovery=true, .mutation=is_allocated ? 9U : 8U,
                .hook_kind=1, .hook_number=recovery.prefix + at}); ++recovery_drift_controls;
        }
        for (u32 harmless : {1U, 12U, 15U, 16U, 21U})
            run({.allocated=is_allocated, .recovery=true, .mutation=harmless, .hook_kind=1,
                .hook_number=recovery.prefix + 1});
    }
    for (bool is_allocated : {false, true})
    {
        const auto counts = is_allocated ? allocated : zero;
        for (usize read : {usize{1}, counts.prefix, counts.prefix + 1, counts.reads})
            run({.allocated=is_allocated, .fail_kind=1, .fail_number=read});
        for (usize at = 1; at <= counts.writes; ++at)
            run({.allocated=is_allocated, .fail_kind=2, .fail_number=at});
        for (usize at = 1; at <= counts.flushes; ++at)
            run({.allocated=is_allocated, .fail_kind=3, .fail_number=at});
        for (u32 kind : {1U, 2U, 3U})
        {
            run({.allocated=is_allocated, .mutation=1, .hook_kind=kind});
            for (u32 harmless : {12U, 15U, 16U, 21U}) run({.allocated=is_allocated, .mutation=harmless, .hook_kind=kind});
            for (u32 mutation : {2U, 3U, 4U, 5U, 6U, 7U, 10U, 11U, 13U, 17U, 18U, 19U, 20U})
                run({.allocated=is_allocated, .mutation=mutation, .hook_kind=kind});
            run({.allocated=is_allocated, .mutation=is_allocated ? 9U : 8U, .hook_kind=kind,
                .hook_number=kind == 1 ? counts.prefix + 1 : 1});
        }
        run({.allocated=is_allocated, .mutation=14, .hook_kind=2});
        run({.allocated=is_allocated, .mutation=14, .hook_kind=3});
        // Child-owned original READs must retain the complete prefix, without
        // introducing cache misses or old-image retirement READs in the child.
        for (u32 mutation : {2U, 3U, 4U, 5U, 6U, 7U, 10U, 11U, 13U, 17U, 18U})
            run({.allocated=is_allocated, .mutation=mutation, .hook_kind=1, .hook_number=counts.prefix + 1});
        if (is_allocated)
            for (u32 mutation : {22U, 23U, 24U}) run({.allocated=true, .mutation=mutation, .hook_kind=1});
    }
    admission_controls();
    alias_controls();
    terminal_observation_controls();
    std::printf("CANONICAL_TYPED_OWNER checks=%zu positives=%zu refusals=%zu faults=%zu drifts=%zu callback_exclusions=%zu foreign_controls=%zu aliases_without_mutation=%zu terminal_observation_controls=%zu delegated_failed_reads=%zu recovery_drift_controls=%zu source_only=yes\n",
        checks, positives, refusals, faults, drifts, callback_exclusions, foreign_controls, aliases, terminal_controls,
        delegated_failed_reads, recovery_drift_controls);
    std::printf("CANONICAL_OWNER_STORAGE buffer_capacity=32 owner_bytes=%zu workspace_bytes=%zu production_workspace=retained_off_stack\n",
        sizeof(Owner), sizeof(Workspace));
}
