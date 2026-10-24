// Hosted actual native Session/child bridge execution. Mounted role and full
// suffix filesystem semantics remain separate integration gates.
#include "ext4_zero_truncate_preview_test_support.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <ok/fs/ext4_mounted_typed_cursor.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace zero_preview_fixture;
namespace {
using Mount = Ext4MountedMetadata<2>;
using Owner = Mount::Owner;
using Workspace = Owner::TypedOwnerWorkspace;
using Phase = Ext4MountedTypedCursorPhase;
using Stage = Ext4MountedTypedSuffixStage;
using Origin = Ext4MountedTypedOrigin;
using Route = Ext4MountedTypedRoute;
using Opening = Ext4MountedTypedOpening;
usize checks{}, fixtures{}, comparisons{}, suffix_reads{}, in_flight_guards{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "native bridge:%d: %s\n", __LINE__, #x); std::abort(); } } while (false)
void be32(Block &bytes, usize at, u32 value) {
    for (usize byte = 0; byte < 4; ++byte) bytes[at + byte] = u8(value >> (24 - byte * 8));
}
Block data{};
Ext4ZeroTruncatePlan zero_plan{};
Ext4ZeroTruncatePlan zero_scratch{};
Ext4ZeroTruncateJournalFormat zero_journal{};
Ext4ZeroTruncateJournalFormatScratch zero_format{};
Ext4AllocatedEmptyDataScratch allocated_scratch{};
Ext4AllocatedEmptyDataPlan allocated_plan{};
Ext4AllocatedEmptyDataJournalFormat allocated_journal{};
Ext4AllocatedEmptyDataJournalFormatScratch allocated_format{};
void media(Image &image, bool allocated, bool recovery, u8 mask, u32 count) {
    auto &head = image.blocks[1000];
    be32(head, 0, jbd2::magic); be32(head, 4, u32(jbd2::RecordType::superblock_v2));
    be32(head, 12, 1024); be32(head, 16, 1024); be32(head, 20, 1); be32(head, 24, 70);
    be32(head, 40, jbd2::feature_checksum_v3 | (allocated ? jbd2::feature_revoke : 0));
    be32(head, 64, 1); head[80] = 4;
    for (usize at = 0; at < 16; ++at) head[48 + at] = image.f.binding.uuid[at];
    be32(head, 252, jbd2::detail::crc32c(~u32{0}, head));
    image.f.binding.empty_log = true; image.f.binding.journal_sequence = 70;
    data = {};
    for (usize at = 7; at < 73; ++at) data[at] = u8(1 + at % 251);
    if (allocated) {
        const Ext4AllocatedEmptyDataSource source{image.f.group, image.f.cross ? image.f.physical : 70, 8,
            image.blocks[1], image.blocks[2], image.blocks[image.f.cross ? 8195 : 3], image.blocks[4], image.blocks[8]};
        CHECK(Ext4AllocatedEmptyDataPlanner::plan_off_stack(image.f.binding, {13, 9, 73, 1700000222, 123456789},
            source, allocated_scratch, allocated_plan));
        CHECK(Ext4AllocatedEmptyDataJournalFormatter::format(image.f.binding, allocated_plan, head,
            image.blocks[4], allocated_format, allocated_journal));
    } else if (count) {
        const Ext4ZeroTruncateSource source{image.f.group, 8, image.blocks[1], image.blocks[2],
            image.blocks[image.f.cross ? 8195 : 3], image.blocks[4], image.blocks[8]};
        CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(image.f.binding, image.update(), source, zero_scratch, zero_plan));
        CHECK(Ext4ZeroTruncateJournalFormatter::format(image.f.binding, zero_plan, head, image.blocks[4], zero_format, zero_journal));
    }
    if (recovery) {
        image.blocks[1000] = allocated ? allocated_journal.head : zero_journal.head;
        image.blocks[1001] = allocated ? allocated_journal.descriptor : zero_journal.descriptor;
        for (u32 at = 0; at < 4; ++at) image.blocks[1002 + at] = allocated ? allocated_journal.payloads[at] : zero_journal.payloads[at];
        image.blocks[1006] = allocated ? allocated_journal.commit : zero_journal.commit;
        image.blocks[1007] = allocated ? allocated_journal.end : zero_journal.end;
        for (u32 home = 0; home < 4; ++home) {
            const u32 target = allocated ? allocated_plan.targets[home] : zero_plan.targets[home];
            image.blocks[target] = allocated ? (mask & (1U << home) ? allocated_plan.after[home] : allocated_plan.before[home]) :
                (mask & (1U << home) ? zero_plan.after[home] : zero_plan.before[home]);
        }
        if (allocated) image.blocks[allocated_plan.physical] = data;
        image.f.binding.empty_log = false;
        image.f.binding.journal_sequence = allocated ? allocated_journal.active_sequence : zero_journal.active_sequence;
    }
}
using Session = Owner::Registry::Session;
struct Authority final {
    Session *observed{};
    Status refusal{};
    usize validations{}, completions{};
    Status validate(Phase, Stage) { ++validations; if (!refusal) return refusal;
        if (observed) { const auto current = observed->report(); CHECK(current); if (current.value().in_flight) ++in_flight_guards; }
        return {}; }
    Status complete_stage(Stage) { ++completions; return refusal; }
};
using Cursor = Ext4MountedTypedCursor<Session, Authority>;
using Attachment = Owner::TypedMountedCursorAttachment<Cursor>;
static_assert(!std::is_copy_constructible_v<Attachment> && !std::is_move_constructible_v<Attachment>);
struct Device final : driver::BlockDevice {
    std::array<Block, 8705> &blocks;
    usize reads{}, writes{}, flushes{};
    bool read_failure{};
    explicit Device(std::array<Block, 8705> &value) : blocks(value) {}
    driver::BlockGeometry geometry() const override { return {blocks.size() * 2, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> output) override {
        ++reads;
        if (read_failure) return Status::io_error("model selected native suffix read refused");
        if (sector % 2 || output.size() != 1024 || sector / 2 >= blocks.size()) return Status::invalid_argument("model read");
        for (usize at = 0; at < 1024; ++at) output[at] = std::byte(blocks[sector / 2][at]);
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> input) override {
        ++writes;
        if (sector % 2 || input.size() != 1024 || sector / 2 >= blocks.size()) return Status::invalid_argument("model write");
        for (usize at = 0; at < 1024; ++at) blocks[sector / 2][at] = std::to_integer<u8>(input[at]);
        return {};
    }
    Status flush() override { ++flushes; return {}; }
};
struct OuterFixture final {
    Image image;
    Device device;
    Owner::Registry registry{};
    std::unique_ptr<Mount> mount{new Mount(Ext4MountedPolicy::ordered_data_dirty_noatime)};
    std::unique_ptr<Workspace> workspace{new Workspace};
    Authority authority{};
    std::unique_ptr<Cursor> cursor;
    std::unique_ptr<Cursor::Context> context;
    std::unique_ptr<Attachment> attachment;
    std::array<std::byte, 1024> bytes{};
    Route route;
    bool allocated, recovery;
    explicit OuterFixture(bool allocated_case, bool recovering, bool cross, u32 count, u8 mask = 0, bool maximum_sequence = false, bool cancellation = false, bool refuse_child = false)
        : image(allocated_case ? 0 : count, cross), device(image.blocks),
          route(allocated_case ? recovering ? Route::allocated_recovery : Route::allocated_fresh :
                recovering ? Route::zero_recovery : count ? Route::zero_fresh : Route::zero_noop),
          allocated(allocated_case), recovery(recovering) {
        media(image, allocated, recovery, mask, count);
        if (maximum_sequence) {
            CHECK(route == Route::zero_noop);
            image.f.binding.journal_sequence = 0xffffffffU;
            auto &head = image.blocks[1000];
            be32(head, 24, 0xffffffffU); be32(head, 252, 0);
            be32(head, 252, jbd2::detail::crc32c(~u32{0}, head));
        }
        const auto key = registry.register_device(device, 500 + fixtures); CHECK(key);
        CHECK(mount->owner_.open(registry, key.value(), {0, device.geometry().block_count}));
        std::array<Owner::Pin, 4> pins{};
        const std::array<u64, 4> blocks{1, 8, 71, allocated || !count ? 900U : cross ? image.f.physical + 2U : 72U};
        for (usize at = 0; at < pins.size(); ++at) CHECK(mount->owner_.pin(blocks[at], pins[at]));
        for (auto &pin : pins) CHECK(pin.release());
        auto &session = mount->owner_.cache_.session_;
        cursor.reset(new Cursor(session));
        context.reset(new Cursor::Context(session, authority, mount->owner_, *mount, 37,
            workspace.get(), &Owner::typed_mounted_bridge_thunk<Cursor>));
        attachment.reset(new Attachment(mount->owner_, *workspace, *cursor, *context));
        authority.observed = &session;
        CHECK(mount->owner_.typed_admit_mounted_cursor(*attachment));
        Owner::Pin denied;
        const auto before = session.report(); CHECK(before);
        CHECK(!mount->owner_.pin(8, denied));
        CHECK(!mount->owner_.rearm());
        const auto after = session.report(); CHECK(after);
        CHECK(reclaim_history::same_session(before.value(), after.value()));
        CHECK(cursor->reserve(*context, u32(image.f.binding.metadata_count / 4),
            image.f.binding.layout.filesystem_blocks, Origin::mutation,
            allocated ? Opening::allocated_family : route == Route::zero_noop ? Opening::noop_only : Opening::zero_family));
        CHECK(cursor->read(*context, 16, bytes));
        if (cancellation) {
            const auto terminal = session.report(); CHECK(terminal);
            CHECK(mount->owner_.typed_cancel_mounted_cursor_before_child(*attachment,
                Status::unsupported("legal classifier refusal")).code() == StatusCode::unsupported);
            CHECK(!mount->owner_.typed_mounted_pending_ && !attachment->admitted_);
            CHECK(cursor->report().phase == Phase::refused && !workspace->attempted_);
            CHECK(reclaim_history::same_session(session.report().value(), terminal.value()));
            CHECK(mount->owner_.pin(8, denied)); CHECK(denied.release());
            return;
        }
        CHECK(cursor->refine(*context, route, image.f.binding.journal_sequence));
        CHECK(cursor->pause_child(*context));
        if (refuse_child) {
            const auto before = session.report(); CHECK(before);
            authority.refusal = Status::permission("legal original outer authority refusal");
            const auto rejected = mount->owner_.run_typed_zero_with_authority(*workspace,
                image.f.binding, image.update(), authority, recovery);
            CHECK(rejected.code() == StatusCode::permission && !workspace->attempted_);
            CHECK(reclaim_history::same_session(session.report().value(), before.value()));
            CHECK(mount->owner_.typed_mounted_cursor_stop(*attachment, rejected).code() == StatusCode::permission);
            CHECK(session.report().value().state == block::DeviceState::quarantined);
            return;
        }
        CHECK(allocated ? mount->owner_.run_typed_allocated_with_authority(*workspace, image.f.binding,
            {13, 9, 73, 1700000222, 123456789}, 7, data, authority, recovery) :
            mount->owner_.run_typed_zero_with_authority(*workspace, image.f.binding, image.update(), authority, recovery));
        CHECK(mount->owner_.typed_mounted_pending_);
        CHECK(!mount->owner_.pin(8, denied));
        CHECK(mount->owner_.typed_resume_mounted_cursor(*attachment));
        CHECK(cursor->report().child_bridge_available && cursor->report().phase == Phase::suffix);
        CHECK(!mount->owner_.typed_resume_mounted_cursor(*attachment));
        CHECK(mount->owner_.typed_mounted_cursor_exact(*attachment));
        ++fixtures;
    }
    Session &session() { return mount->owner_.cache_.session_; }
    auto actual() { const auto result = session().report(); CHECK(result); return result.value(); }
    auto child_terminal() { return allocated ? workspace->allocated_.progress_ : workspace->zero_.progress_; }
};
void complete(OuterFixture &f) {
    auto &cursor = *f.cursor;
    const auto child = f.child_terminal();
    const auto writes = f.device.writes, flushes = f.device.flushes;
    Cursor::Receipt receipt;
    CHECK(!cursor.close_suffix(*f.context, receipt));
    for (u32 at = 0;; ++at) {
        const auto stage = Cursor::selected_stage(Origin::mutation, f.route, at);
        if (stage == Stage::none) break;
        CHECK(cursor.begin_stage(*f.context, stage));
        CHECK(cursor.read(*f.context, 16, f.bytes));
        CHECK(f.mount->owner_.typed_mounted_cursor_exact(*f.attachment));
        CHECK(reclaim_history::same_session(child, f.child_terminal()));
        CHECK(cursor.end_stage(*f.context)); ++suffix_reads;
    }
    CHECK(f.device.writes == writes && f.device.flushes == flushes);
    CHECK(cursor.close_suffix(*f.context, receipt));
    CHECK(f.mount->owner_.typed_accept_mounted_cursor_receipt(*f.attachment, receipt));
    CHECK(f.attachment->receipt_consumed_ && cursor.report().receipt_consumed);
    CHECK(f.mount->owner_.typed_mounted_consumed_receipt_exact(*f.attachment, receipt));
    CHECK(f.mount->owner_.typed_mounted_cursor_exact(*f.attachment));
    CHECK(!f.mount->owner_.typed_accept_mounted_cursor_receipt(*f.attachment, receipt));
    CHECK(!f.mount->owner_.rearm());
    Owner::Pin denied; CHECK(!f.mount->owner_.pin(8, denied));
    CHECK(f.mount->owner_.typed_mounted_pending_);
    CHECK(reclaim_history::same_session(child, f.child_terminal()));
    auto expected = child;
    expected.reads += usize(cursor.report().suffix_reads);
    expected.completed_reads += usize(cursor.report().suffix_reads);
    CHECK(reclaim_history::same_session(expected, f.actual()));
    CHECK(reclaim_history::same_session(expected, f.attachment->suffix_terminal_));
}
void positive_routes() {
    { OuterFixture f(false, false, false, 0, 0, true); complete(f); CHECK(f.cursor->report().final_sequence == 0xffffffffU); }
    for (bool cross : {false, true}) {
        { OuterFixture f(false, false, cross, 0); complete(f); }
        { OuterFixture f(false, false, cross, 3); complete(f); }
        { OuterFixture f(true, false, cross, 0); complete(f); }
        for (u8 mask = 0; mask < 16; ++mask) {
            { OuterFixture f(false, true, cross, 3, mask); complete(f); }
            { OuterFixture f(true, true, cross, 0, mask); complete(f); }
        }
    }
}
void saved_comparison_controls() {
    OuterFixture f(false, false, false, 0);
    auto &a = *f.attachment;
    const auto compare = [&](auto &field) {
        const auto saved = field;
        field ^= 1;
        // Callback-free comparison only. No transport occurs under changed
        // state and the saved record is restored before execution continues.
        CHECK(!f.mount->owner_.typed_mounted_cursor_exact(a));
        field = saved;
        CHECK(f.mount->owner_.typed_mounted_cursor_exact(a)); ++comparisons;
    };
    const auto counters = [&](auto &r) {
        compare(r.reads); compare(r.completed_reads); compare(r.writes); compare(r.completed_writes);
        compare(r.flushes); compare(r.completed_flushes);
        compare(r.identity); compare(r.registration_generation); compare(r.session_serial);
    };
    counters(a.child_terminal_); counters(a.child_terminal_seal_);
    counters(f.cursor->bridge_terminal_); counters(f.cursor->bridge_terminal_seal_);
    complete(f);
}
void legitimate_refusal_and_lifetime() {
    {
        OuterFixture f(false, false, false, 0); const auto child = f.child_terminal();
        CHECK(f.cursor->begin_stage(*f.context, Stage::noop_terminal));
        f.device.read_failure = true;
        CHECK(!f.cursor->read(*f.context, 16, f.bytes));
        const auto actual = f.actual();
        CHECK(actual.state == block::DeviceState::quarantined && actual.first_error == StatusCode::io_error);
        CHECK(actual.reads == child.reads + 1 && actual.completed_reads == child.completed_reads);
        CHECK(!actual.in_flight && !actual.pending_writes);
        CHECK(reclaim_history::same_session(child, f.child_terminal()));
    }
    {
        OuterFixture f(false, false, false, 0); const auto child = f.child_terminal();
        CHECK(f.cursor->begin_stage(*f.context, Stage::noop_terminal));
        f.authority.refusal = Status::permission("model concrete authority refused");
        CHECK(!f.cursor->read(*f.context, 16, f.bytes));
        const auto actual = f.actual();
        CHECK(actual.state == block::DeviceState::quarantined && actual.reads == child.reads);
    }
    {
        OuterFixture f(false, false, false, 0); const auto child = f.child_terminal();
        f.attachment.reset();
        const auto actual = f.actual();
        CHECK(actual.state == block::DeviceState::quarantined && actual.reads == child.reads);
        CHECK(f.mount->owner_.typed_mounted_pending_);
    }
}
} // namespace
int main() {
    positive_routes(); saved_comparison_controls(); legitimate_refusal_and_lifetime();
    { OuterFixture cancelled(false, false, false, 0, 0, false, true); }
    { OuterFixture refused(false, false, false, 0, 0, false, false, true); }
    CHECK(in_flight_guards && suffix_reads && comparisons == 36);
    std::printf("PASS guarded child outer authority: fixtures=%zu checks=%zu comparisons=%zu suffix_reads=%zu in_flight_guards=%zu; mounted semantics and renewal uncomposed\n",
        fixtures, checks, comparisons, suffix_reads, in_flight_guards);
}
