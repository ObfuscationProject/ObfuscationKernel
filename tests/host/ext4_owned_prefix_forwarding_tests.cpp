// Privileged hosted observations only (-fno-access-control). The actual frozen
// production headers must also be compiled separately with access control.
#include "ext4_zero_truncate_preview_test_support.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

using namespace zero_preview_fixture;
using Owner = Ext4OwnedTransaction<2, 32>;
using Workspace = Owner::TypedOwnerWorkspace;
using Registry = Owner::Registry;
using Cache = Owner::Cache;
using Session = Registry::Session;
enum class Io { geometry, read, write, flush };
usize checks{}, stable{}, refused{}, drift_controls{}, counter_controls{}, exclusion_controls{}, payload_controls{}, fault_controls{}, foreign_controls{}, alias_controls{}, terminal_drift_controls{}, retired_callback_calls{}, retirement_controls{}, recovery_boundary_controls{}, delegated_fault_controls{};
Status retired_probe(void *) { ++retired_callback_calls; return Status::fault("retired owner callback must not execute"); }
void require(bool value, const char *why) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL authored canonical typed owner prefix: %s\n", why); std::abort(); }
}
void be(Block &b, usize at, u32 n) { for (usize k = 0; k < 4; ++k) b[at + k] = u8(n >> (24 - k * 8)); }
// The inherited Image intentionally borrows one global single-fixture disk.
// Independent original/foreign controls must own separate physical media.
struct ReviewImage {
    zero_fixture::Fixture f{};
    std::array<Block,8705> blocks{};
    Ext4ZeroTruncateUpdate request{};
    explicit ReviewImage(u32 count) {
        Image seed(count);
        f = seed.f; blocks = seed.blocks; request = seed.update();
    }
    Ext4ZeroTruncateUpdate update() const { return request; }
};
void head(ReviewImage &image, bool allocated) {
    auto &b = image.blocks[1000]; b = {};
    be(b, 0, jbd2::magic); be(b, 4, u32(jbd2::RecordType::superblock_v2));
    be(b, 12, 1024); be(b, 16, 1024); be(b, 20, 1); be(b, 24, 90);
    be(b, 40, jbd2::feature_checksum_v3 | (allocated ? jbd2::feature_revoke : 0));
    be(b, 64, 1); b[80] = 4;
    for (usize k = 0; k < 16; ++k) b[48 + k] = image.f.binding.uuid[k];
    be(b, 252, jbd2::detail::crc32c(~u32{0}, b));
    image.f.binding.empty_log = true; image.f.binding.journal_sequence = 90;
    for (usize record = 1; record < 8; ++record)
        for (usize byte = 0; byte < 1024; ++byte) image.blocks[1000 + record][byte] = u8(101 + record * 17 + byte * 5);
}
struct Device final : driver::BlockDevice {
    static constexpr u64 origin = 19, volume_offset = 6, sectors = origin + volume_offset + 8705 * 2 + 8;
    std::array<Block, 8705> &blocks;
    mutable usize geometries{};
    usize reads{}, writes{}, flushes{};
    Io failure{Io::geometry};
    usize fail_number{};
    std::function<void(Io, usize)> callback{};
    explicit Device(std::array<Block, 8705> &b) : blocks(b) {}
    driver::BlockGeometry geometry() const override {
        ++geometries;
        if (callback) callback(Io::geometry, geometries);
        return {sectors, 512, true};
    }
    Status read_blocks(u64 first, std::span<std::byte> out) override {
        ++reads; if (callback) callback(Io::read, reads);
        if (failure == Io::read && reads == fail_number) return Status::io_error("independent original READ fault");
        if (first < origin + volume_offset || (first - origin - volume_offset) % 2 || out.size() != 1024 ||
            (first - origin - volume_offset) / 2 >= blocks.size()) return Status::invalid_argument("independent read");
        const auto &image = blocks[(first - origin - volume_offset) / 2];
        for (usize n = 0; n < out.size(); ++n) out[n] = std::byte(image[n]);
        return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> in) override {
        ++writes; if (callback) callback(Io::write, writes);
        if (failure == Io::write && writes == fail_number) return Status::io_error("independent original WRITE fault");
        if (first < origin + volume_offset || (first - origin - volume_offset) % 2 || in.size() != 1024 ||
            (first - origin - volume_offset) / 2 >= blocks.size()) return Status::invalid_argument("independent write");
        auto &image = blocks[(first - origin - volume_offset) / 2];
        for (usize n = 0; n < in.size(); ++n) image[n] = std::to_integer<u8>(in[n]);
        return {};
    }
    Status flush() override {
        ++flushes; if (callback) callback(Io::flush, flushes);
        return failure == Io::flush && flushes == fail_number ? Status::io_error("independent original FLUSH fault") : Status{};
    }
    std::array<usize, 4> calls() const { return {geometries, reads, writes, flushes}; }
};
struct ReviewFixture {
    bool allocated{}, recovery{}, noop{};
    ReviewImage media;
    std::unique_ptr<std::array<Block, 8705>> initial{};
    Device device;
    Registry registry{};
    std::unique_ptr<Owner> owner{new Owner(true)};
    std::unique_ptr<Workspace> workspace{new Workspace};
    Ext4ZeroTruncatePlan zero{};
    Ext4ZeroTruncatePlan zero_scratch{};
    Ext4ZeroTruncateJournalFormat zero_journal{};
    Ext4ZeroTruncateJournalFormatScratch zero_format{};
    Ext4AllocatedEmptyDataPlan allocation{};
    Ext4AllocatedEmptyDataScratch allocation_scratch{};
    Ext4AllocatedEmptyDataJournalFormat allocation_journal{};
    Ext4AllocatedEmptyDataJournalFormatScratch allocation_format{};
    Block data{}, original_data{};
    usize residents{};
    block::SessionReport original_session{};
    Registry::Registration key{};
    ReviewFixture(bool a = false, bool r = false, u8 mask = 0, bool n = false, bool resident = true)
        : allocated(a), recovery(r), noop(n), media(a || n ? 0 : 3), device(media.blocks) {
        head(media, allocated);
        for (usize k = 7; k < 73; ++k) data[k] = u8(3 + (k * 13) % 249);
        original_data = data;
        if (allocated) {
            const Ext4AllocatedEmptyDataSource source{media.f.group, 70, 8, media.blocks[1], media.blocks[2], media.blocks[3], media.blocks[4], media.blocks[8]};
            require(Ext4AllocatedEmptyDataPlanner::plan_off_stack(media.f.binding, {13,9,73,1700000011,22}, source, allocation_scratch, allocation).ok(), "independent allocated plan");
            require(Ext4AllocatedEmptyDataJournalFormatter::format(media.f.binding, allocation, media.blocks[1000], media.blocks[4], allocation_format, allocation_journal).ok(), "independent allocated journal");
        } else if (!noop) {
            const Ext4ZeroTruncateSource source{media.f.group,8,media.blocks[1],media.blocks[2],media.blocks[3],media.blocks[4],media.blocks[8]};
            require(Ext4ZeroTruncatePlanner::plan_off_stack(media.f.binding, media.update(), source, zero_scratch, zero).ok(), "independent truncate plan");
            require(Ext4ZeroTruncateJournalFormatter::format(media.f.binding, zero, media.blocks[1000], media.blocks[4], zero_format, zero_journal).ok(), "independent truncate journal");
        }
        if (recovery) {
            const auto &jhead = allocated ? allocation_journal.head : zero_journal.head;
            media.blocks[1000] = jhead;
            media.blocks[1001] = allocated ? allocation_journal.descriptor : zero_journal.descriptor;
            media.blocks[1006] = allocated ? allocation_journal.commit : zero_journal.commit;
            media.blocks[1007] = allocated ? allocation_journal.end : zero_journal.end;
            for (usize h = 0; h < 4; ++h) {
                media.blocks[1002 + h] = allocated ? allocation_journal.payloads[h] : zero_journal.payloads[h];
                const u32 physical = allocated ? allocation.targets[h] : zero.targets[h];
                media.blocks[physical] = allocated ? ((mask & (1U << h)) ? allocation.after[h] : allocation.before[h]) :
                    ((mask & (1U << h)) ? zero.after[h] : zero.before[h]);
            }
            if (allocated) media.blocks[allocation.physical] = data;
            media.f.binding.empty_log = false;
            media.f.binding.journal_sequence = allocated ? allocation_journal.active_sequence : zero_journal.active_sequence;
        }
        media.f.binding.layout.first_sector = Device::volume_offset;
        initial = std::make_unique<std::array<Block,8705>>(media.blocks);
        const auto registered = registry.register_device(device, 815, 4096); require(registered.ok(), "register actual owner backing"); key = registered.value();
        require(owner->open(registry,key,{Device::origin,Device::sectors-Device::origin}).ok(), "open nonzero actual partition origin");
        if (resident) {
            std::array<Owner::Pin,32> pins{};
            auto retain = [&](u32 block) { require(owner->pin(block + Device::volume_offset/2,pins[residents]).ok(), "retain actual canonical physical block"); ++residents; };
            for (u32 j = 0; j < 8; ++j) retain(1000+j);
            for (u32 b : {1U,2U,3U,8U}) retain(b);
            if (allocated) retain(allocation.physical);
            else if (!noop) for (usize at = 0; at < zero.released_count; ++at) retain(zero.released_mapping[at]);
            retain(69);
            for (usize at = 0; at < residents; ++at) require(pins[at].release().ok(), "drain original resident pins");
        }
        original_session = owner->session_report().value();
        device.reads = device.writes = device.flushes = device.geometries = 0;
    }
    ~ReviewFixture() {
        device.callback = {};
        if (owner->cache_.report_.state == MetadataOwnerState::ready) require(owner->close().ok(), "close actual successful owner");
    }
    Status run() { return allocated ? owner->run_typed_allocated(*workspace,media.f.binding,{13,9,73,1700000011,22},7,data,recovery) : owner->run_typed_zero(*workspace,media.f.binding,media.update(),recovery); }
    void retired_guards() {
        const auto check = [&](auto &guard) {
            require(guard.owner_guard_retired_ && !guard.owner_context_ && !guard.owner_context_seal_ &&
                    !guard.owner_check_ && !guard.owner_check_seal_ && guard.continuing_owner(),
                    "all selected/unused guard pointers and seals are retired before synchronous return");
            require(guard.bind_owner_guard(workspace.get(),retired_probe).code() == StatusCode::already_exists &&
                    !retired_callback_calls,"a retired selected/unused guard cannot rebind or invoke a callback");
            ++retirement_controls;
        };
        check(workspace->zero_); check(workspace->allocated_);
        check(workspace->zero_.recovery_preview_); check(workspace->allocated_.recovery_preview_);
    }
    void success() {
        require(run().ok(), "actual original synchronous typed owner succeeds");
        require(owner->report().phase == Ext4OwnedPhase::checkpointed && workspace->consumed_ && owner->typed_completed_, "complete private terminal handoff");
        require(!owner->typed_context_ && !workspace->zero_.owner_context_ && !workspace->allocated_.owner_context_ &&
                !workspace->zero_.owner_check_ && !workspace->allocated_.owner_check_, "no callback retains local context after return");
        retired_guards();
        require(!retired_callback_calls,"post-return owner inspection executes no retained local callback");
        require(workspace->prefix_reads_ == residents && workspace->prefix_reservation_ == 32, "whole prefix reserved and exactly resident physical images captured");
        if (allocated) require(media.blocks[allocation.physical] == original_data, "write the original snapshotted payload");
        const auto calls = device.calls(); require(!run(), "workspace remains one-shot after publication"); require(device.calls() == calls, "one-shot refusal performs no callback");
        ++stable;
    }
    void failure() {
        require(!run(), "retained original drift refuses");
        const auto report = owner->session_report();
        require(report.ok() && report.value().state == block::DeviceState::quarantined && !report.value().in_flight, "quarantine actual continuing Session");
        require(workspace->phase_ == Owner::TypedOwnerPhase::failed && workspace->consumed_, "original failure is terminal");
        require(!owner->typed_context_ && !workspace->zero_.owner_context_ && !workspace->allocated_.owner_context_ &&
                !workspace->zero_.owner_check_ && !workspace->allocated_.owner_check_, "failure retires call-only local callbacks");
        require(workspace->zero_.owner_guard_retired_ && workspace->allocated_.owner_guard_retired_ &&
                workspace->zero_.continuing_owner() && workspace->allocated_.continuing_owner() &&
                workspace->zero_.bind_owner_guard(workspace.get(),retired_probe).code() == StatusCode::already_exists &&
                workspace->allocated_.bind_owner_guard(workspace.get(),retired_probe).code() == StatusCode::already_exists &&
                !retired_callback_calls,"terminal failure cannot rebind or execute either local callback");
        retired_guards();
        const auto calls = device.calls();
        owner->report_.phase = Ext4OwnedPhase::open;
        require(!run() && !owner->rearm() && !owner->close(), "restoring diagnostic phase cannot revive actual failed admission");
        require(device.calls() == calls, "terminal retry/rearm/close invokes no callback"); ++refused;
    }
};
void exclusions(ReviewFixture &f) {
    auto &o = *f.owner; const auto saved = o.report_;
    o.report_.phase = Ext4OwnedPhase::open;
    Owner::Pin pin{};
    require(o.pin(69+Device::volume_offset/2,pin).code() == StatusCode::busy, "public pins excluded despite authentic report corruption");
    require(o.prepare({}).code() == StatusCode::busy && o.prepare_rename({}).code() == StatusCode::busy && o.prepare_create({}).code() == StatusCode::busy, "old mutation families excluded");
    Ext4InodeMetadataUpdate ordered{}; ordered.ordered_data = true;
    require(o.prepare_ordered_data(ordered,70,f.data).code() == StatusCode::busy,"existing ordered data owner mutation excluded");
    require(o.prepare_first_data({},0,f.data).code() == StatusCode::busy && o.prepare_next_data({},0,f.data).code() == StatusCode::busy && o.prepare_namespace({}).code() == StatusCode::busy, "data/namespace mutation families excluded");
    require(o.prepare_namespace_recovery().code() == StatusCode::busy && o.recover_namespace().code() == StatusCode::busy,
            "existing public namespace recovery family excluded");
    require(o.discard().code() == StatusCode::busy && o.rearm().code() == StatusCode::busy && o.close().code() == StatusCode::busy && o.mounted_teardown_preflight().code() == StatusCode::busy, "discard/rearm/close/teardown excluded");
    o.report_.phase = Ext4OwnedPhase::prepared; require(o.commit().code() == StatusCode::busy, "commit excluded");
    o.report_.phase = Ext4OwnedPhase::durable; require(o.checkpoint().code() == StatusCode::busy, "checkpoint excluded");
    o.report_ = saved; ++exclusion_controls;
}
void drift(ReviewFixture &f, usize field) {
    auto &w = *f.workspace; auto &o = *f.owner;
    switch (field) {
        case 0: w.data_request_[31] ^= u8{1}; break;
        case 1: w.data_request_seal_[32] ^= u8{1}; break;
        case 2: w.owner_ = nullptr; break;
        case 3: w.cache_ = nullptr; break;
        case 4: w.session_ = nullptr; break;
        case 5: ++w.child_initial_.reads; break;
        case 6: ++w.binding_.metadata[127].count; break;
        case 7: o.cache_.entries_[0].canonical[13] ^= std::byte{1}; break;
        case 8: o.cache_.entries_[0].staged[14] ^= std::byte{1}; break;
        case 9: ++o.cache_.entries_[0].report.generation; break;
        case 10: ++w.prefix_.entries_[0].report.generation; break;
        case 11: ++o.cache_.session_.first_; break;
        case 12: ++o.cache_.session_.geometry_.block_count; break;
        case 13: ++f.registry.entries_[0].maximum_transfer; break;
    }
}
void capacity_controls() {
    for (bool allocated : {false,true})
        for (bool recovery : {false,true})
            for (usize field = 0; field < 4; ++field) {
                auto f = std::make_unique<ReviewFixture>(allocated,recovery);
                auto &actual = f->registry.entries_[0].report;
                usize reads{}, writes{}, flushes{};
                if (allocated) {
                    const auto room = Ext4AllocatedEmptyDataTransactionBudget::capacity(actual,1,
                        recovery ? Ext4AllocatedEmptyDataRoute::recovery : Ext4AllocatedEmptyDataRoute::fresh,
                        f->media.f.binding.journal_sequence);
                    require(room.ok(),"compute full original allocated reservation"); reads = room.value().reads; writes = room.value().writes; flushes = room.value().flushes;
                } else {
                    const auto room = Ext4ZeroTruncateTransactionBudget::capacity(actual,1,
                        recovery ? Ext4ZeroTruncateRoute::recovery : Ext4ZeroTruncateRoute::fresh,
                        f->media.f.binding.journal_sequence);
                    require(room.ok(),"compute full original truncate reservation"); reads = room.value().reads; writes = room.value().writes; flushes = room.value().flushes;
                }
                if (field == 0) actual.reads = actual.completed_reads = ~usize{0} - reads - 31;
                if (field == 1) actual.writes = actual.completed_writes = ~usize{0} - writes + 1;
                if (field == 2) actual.flushes = actual.completed_flushes = ~usize{0} - flushes + 1;
                if (field == 3) --actual.completed_reads;
                const auto before = actual; const auto slots = f->owner->cache_.entries_; const auto calls = f->device.calls();
                require(!f->run(),"entire prefix plus child capacity refuses before first dispatch");
                require(reclaim_history::same_session(before,actual) && f->device.calls() == calls,
                        "capacity refusal changes no actual Session counter or callback");
                require(!f->workspace->attempted_ && !f->owner->typed_attempted_ && !f->owner->typed_context_,
                        "capacity refusal does not mint exclusion, child or workspace");
                for (usize n = 0; n < slots.size(); ++n)
                    require(Cache::same_report(slots[n].report,f->owner->cache_.entries_[n].report) &&
                        slots[n].canonical == f->owner->cache_.entries_[n].canonical && slots[n].staged == f->owner->cache_.entries_[n].staged,
                        "whole counter capacity refusal preserves every canonical slot byte");
                require(f->media.blocks == *f->initial,"capacity refusal preserves complete physical media"); ++counter_controls;
            }
}
template <typename T> std::vector<unsigned char> representation(const T &value) {
    std::vector<unsigned char> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}
void alias_refusals() {
    for (usize field = 0; field < 7; ++field) {
        auto f = std::make_unique<ReviewFixture>(true);
        auto *workspace = f->workspace.get();
        const auto *binding = &f->media.f.binding;
        const auto *image = &f->data;
        if (field == 0) workspace = reinterpret_cast<Workspace *>(f->owner.get());
        if (field == 1) { f->workspace->binding_ = *binding; binding = &f->workspace->binding_; }
        if (field == 2) image = &f->workspace->data_request_;
        if (field == 3) image = &f->workspace->data_request_seal_;
        if (field == 4) image = &f->workspace->checking_;
        if (field == 5) image = &f->owner->data_image_;
        // Deliberately overlapping references must be rejected by address and
        // size before any attempt to evaluate a foreign object representation.
        if (field == 6) image = reinterpret_cast<const Block *>(binding);
        const auto owner_bytes = representation(*f->owner);
        const auto workspace_bytes = representation(*f->workspace);
        const auto registry_bytes = representation(f->registry);
        const auto binding_bytes = representation(f->media.f.binding);
        const auto calls = f->device.calls();
        require(f->owner->run_typed_allocated(*workspace,*binding,{13,9,73,1700000011,22},7,*image).code() == StatusCode::invalid_argument,
                "all retained input overlaps refuse before first copy");
        require(owner_bytes == representation(*f->owner) && workspace_bytes == representation(*f->workspace) &&
                registry_bytes == representation(f->registry) && binding_bytes == representation(f->media.f.binding),
                "alias refusal preserves complete owner workspace Registry and caller binding representations");
        require(f->device.calls() == calls && f->media.blocks == *f->initial && f->data == f->original_data,
                "alias refusal preserves all original callbacks media and caller payload");
        ++alias_controls;
    }
}
void original_fault_accounting() {
    for (bool allocated : {false,true}) for (bool recovery : {false,true})
        for (Io operation : {Io::read,Io::write,Io::flush}) {
            auto f = std::make_unique<ReviewFixture>(allocated,recovery);
            f->device.failure = operation;
            f->device.fail_number = operation == Io::read ? f->residents + 1 : 1;
            f->failure();
            const auto &actual = f->registry.entries_[0].report;
            const auto check_child = [&](const auto &child) {
                const auto report = child.report();
                require(report.status.code() == StatusCode::io_error && !report.dispatch_pending,
                        "authentic failed original transport has exact terminal fault bookkeeping");
                if (recovery && operation == Io::read) {
                    const auto preview = child.recovery_preview_.report();
                    require(!report.reads && !report.completed_reads && !report.writes && !report.flushes &&
                            reclaim_history::same_session(child.progress_,report.initial),
                            "failed delegated recovery preview grants no parent child counter credit");
                    require(preview.status.code() == StatusCode::io_error && !preview.dispatch_pending &&
                            preview.attempts == actual.reads - preview.initial.reads &&
                            preview.completions == actual.completed_reads - preview.initial.completed_reads &&
                            reclaim_history::same_session(preview.observed,actual) &&
                            reclaim_history::same_session(child.recovery_preview_.progress_,actual),
                            "delegated recovery preview accounts its exact authentic failed READ prefix");
                    require(reclaim_history::same_session(report.observed,actual),
                            "parent diagnostics refresh the original failed delegated preview report");
                    return;
                }
                require(report.reads == actual.reads - report.initial.reads &&
                        report.writes == actual.writes - report.initial.writes &&
                        report.flushes == actual.flushes - report.initial.flushes &&
                        report.completed_reads == actual.completed_reads - report.initial.completed_reads &&
                        report.completed_writes == actual.completed_writes - report.initial.completed_writes &&
                        report.completed_flushes == actual.completed_flushes - report.initial.completed_flushes,
                        "authentic quarantine accounts every attempted and completed original child counter");
                require(reclaim_history::same_session(child.progress_,actual),
                        "authentic terminal bookkeeping records the exact original quarantined report");
            };
            if (allocated) check_child(f->workspace->allocated_); else check_child(f->workspace->zero_);
            require((operation != Io::read || (f->device.reads == f->residents+1 && !f->device.writes && !f->device.flushes)) &&
                    (operation != Io::write || (f->device.writes == 1 && !f->device.flushes)) &&
                    (operation != Io::flush || f->device.flushes == 1),
                    "no original transport follows the selected fault");
            ++fault_controls;
        }
}
void foreign_quarantine_accounting() {
    for (bool allocated : {false,true}) for (bool recovery : {false,true}) for (Io operation : {Io::read,Io::write,Io::flush}) {
        auto f = std::make_unique<ReviewFixture>(allocated,recovery);
        auto foreign = std::make_unique<ReviewFixture>(allocated);
        auto &session = f->owner->cache_.session_;
        const auto original_key = session.registration_;
        const auto foreign_saved_report = foreign->registry.entries_[0].report;
        auto *const foreign_saved_owner = foreign->registry.entries_[0].owner;
        const auto foreign_calls = foreign->device.calls();
        usize before_reads{}, before_writes{}, before_flushes{};
        usize preview_attempts{}, preview_completions{};
        bool observed{};
        f->device.failure = operation;
        f->device.fail_number = operation == Io::read ? f->residents + 1 : 1;
        f->device.callback = [&](Io io,usize count) {
            if (observed || io != operation || count != f->device.fail_number) return;
            observed = true;
            const auto capture = [&](const auto &child) {
                const auto report = child.report();
                before_reads = report.reads; before_writes = report.writes; before_flushes = report.flushes;
                preview_attempts = child.recovery_preview_.report().attempts;
                preview_completions = child.recovery_preview_.report().completions;
            };
            if (allocated) capture(f->workspace->allocated_); else capture(f->workspace->zero_);
            auto counterfeit = f->registry.entries_[0].report;
            counterfeit.in_flight = false; counterfeit.state = block::DeviceState::quarantined;
            counterfeit.first_error = StatusCode::io_error;
            foreign->registry.entries_[0].report = counterfeit;
            foreign->registry.entries_[0].owner = nullptr;
            session.service_ = &foreign->registry;
            session.registration_ = foreign->owner->cache_.session_.registration_;
        };
        require(!f->run() && observed,"same-number foreign quarantine presented at actual failed original transport");
        const auto check_child = [&](const auto &child) {
            const auto report = child.report();
            require(report.reads == before_reads && report.writes == before_writes && report.flushes == before_flushes,
                    "a foreign quarantined report cannot supply original attempted counter credit");
            if (recovery && operation == Io::read)
                require(child.recovery_preview_.report().attempts == preview_attempts &&
                        child.recovery_preview_.report().completions == preview_completions,
                        "a foreign terminal Session cannot supply delegated preview counter credit");
        };
        if (allocated) check_child(f->workspace->allocated_); else check_child(f->workspace->zero_);
        require(f->registry.entries_[0].report.state == block::DeviceState::quarantined &&
                !f->registry.entries_[0].report.in_flight && f->workspace->consumed_,
                "the original pinned request is quarantined and its workspace terminal");
        require(!f->owner->typed_context_ && !f->workspace->zero_.owner_context_ && !f->workspace->allocated_.owner_context_,
                "foreign fault rejection retires every call-only callback context");
        f->retired_guards();
        require(foreign->device.calls() == foreign_calls && foreign->media.blocks == *foreign->initial,
                "foreign evidence supplies no callback or physical side effect");
        session.service_ = &f->registry; session.registration_ = original_key;
        foreign->registry.entries_[0].report = foreign_saved_report;
        foreign->registry.entries_[0].owner = foreign_saved_owner;
        const auto calls = f->device.calls();
        require(!f->run() && f->device.calls() == calls,"restoring authentic Session backing cannot revive terminal attempt");
        ++foreign_controls;
    }
}
void terminal_actual_binding_drifts() {
    for (bool allocated : {false,true}) for (bool recovery : {false,true}) for (usize field = 0; field < 11; ++field) {
        auto f = std::make_unique<ReviewFixture>(allocated,recovery);
        auto foreign = std::make_unique<ReviewFixture>(allocated);
        auto &session = f->owner->cache_.session_;
        auto &entry = f->registry.entries_[0];
        const auto old_first = session.first_;
        const auto old_geometry = session.geometry_;
        const auto old_device = entry.device;
        const auto old_registered_geometry = entry.geometry;
        const auto old_transfer = entry.maximum_transfer;
        const auto old_identity = entry.report.identity;
        const auto old_generation = entry.report.registration_generation;
        const auto old_serial = entry.report.session_serial;
        const auto foreign_report = foreign->registry.entries_[0].report;
        const auto foreign_calls = foreign->device.calls();
        bool observed{};
        f->device.failure = Io::read; f->device.fail_number = f->residents+1;
        f->device.callback = [&](Io io,usize count) {
            if (observed || io != Io::read || count != f->device.fail_number) return;
            observed = true;
            switch (field) {
                case 0: ++session.first_; break;
                case 1: ++session.geometry_.block_count; break;
                case 2: session.geometry_.block_size = 1024; break;
                case 3: session.geometry_.writable = false; break;
                case 4: ++entry.geometry.block_count; break;
                case 5: entry.geometry.block_size = 1024; break;
                case 6: entry.geometry.writable = false; break;
                case 7: ++entry.maximum_transfer; break;
                case 8: entry.device = &foreign->device; break;
                case 9: ++entry.report.identity; break;
                case 10: ++entry.report.registration_generation; break;
            }
        };
        require(!f->run() && observed,"failed original READ presents drifted actual terminal backing");
        const auto check_child = [&](const auto &child) {
            const auto report = child.report();
            require(!report.reads && !report.writes && !report.flushes && !report.completed_reads,
                    "changed original terminal binding supplies no child counter credit");
            if (recovery)
                require(!child.recovery_preview_.report().attempts && !child.recovery_preview_.report().completions,
                        "changed original terminal binding supplies no delegated preview counter credit");
        };
        if (allocated) check_child(f->workspace->allocated_); else check_child(f->workspace->zero_);
        require(entry.report.state == block::DeviceState::quarantined && !entry.report.in_flight && f->workspace->consumed_,
                "actual original pinned entry quarantines even when key or geometry drifted");
        require(f->device.reads == f->residents+1 && !f->device.writes && !f->device.flushes,
                "terminal original binding refusal excludes every subsequent transport");
        require(reclaim_history::same_session(foreign_report,foreign->registry.entries_[0].report) && foreign->device.calls() == foreign_calls &&
                foreign->media.blocks == *foreign->initial,"terminal backing refusal preserves complete foreign backing");
        require(!f->owner->typed_context_ && !f->workspace->zero_.owner_context_ && !f->workspace->allocated_.owner_context_,
                "terminal drift rejection retires the local owner thunk");
        f->retired_guards();
        session.first_ = old_first; session.geometry_ = old_geometry; entry.device = old_device;
        entry.geometry = old_registered_geometry; entry.maximum_transfer = old_transfer;
        entry.report.identity = old_identity; entry.report.registration_generation = old_generation; entry.report.session_serial = old_serial;
        const auto calls = f->device.calls();
        require(!f->run() && f->device.calls() == calls,"restoring actual terminal binding cannot revive consumed workspace");
        ++terminal_drift_controls;
    }
}
template <typename Fn> void selected_child(ReviewFixture &f, Fn &&fn) {
    if (f.allocated) fn(f.workspace->allocated_); else fn(f.workspace->zero_);
}
struct RecoveryBoundary { Io io{}; usize count{}, device_reads{}; };
void delegated_drift(ReviewFixture &f, usize field) {
    if (field < 14) { drift(f,field); return; }
    selected_child(f,[&](auto &child) {
        auto &preview = child.recovery_preview_;
        switch (field) {
            case 14: child.original_ = nullptr; break;
            case 15: ++child.initial_.reads; break;
            case 16: ++child.binding_.metadata[127].count; break;
            case 17: child.owner_context_ = nullptr; break;
            case 18: child.owner_check_ = nullptr; break;
            case 19: preview.owner_context_ = nullptr; break;
            case 20: preview.owner_check_ = nullptr; break;
            case 21: ++preview.original_binding_.first_sector; break;
            case 22: ++preview.initial_.reads; break;
            case 23: ++preview.binding_.metadata[127].count; break;
            case 24: ++preview.control_.sector; break;
            case 25: ++child.authoritative_binding_.first_sector; break;
        }
    });
}
void every_delegated_recovery_boundary() {
    for (bool allocated : {false,true}) {
        std::vector<RecoveryBoundary> boundaries;
        {
            auto f = std::make_unique<ReviewFixture>(allocated,true);
            f->device.callback = [&](Io io,usize count) {
                selected_child(*f,[&](auto &child) {
                    const auto &preview = child.recovery_preview_;
                    if (preview.control_.pending && !preview.owner_guard_retired_) {
                        require(preview.owner_context_ == &child && preview.owner_context_seal_ == &child &&
                                preview.owner_check_ && preview.owner_check_ == preview.owner_check_seal_,
                                "delegated recovery runtime boundary retains the actual parent with fixed sealed thunk");
                        require(io == Io::geometry || io == Io::read,"delegated preview has only readonly callbacks");
                        boundaries.push_back({io,count,f->device.reads});
                    }
                });
            };
            f->success();
        }
        require(!boundaries.empty() && boundaries.size() % 3 == 0,"trace complete delegated pre-geometry READ post-geometry boundaries");
        const usize reads = boundaries.size()/3;
        require(reads == (allocated ? 58U : 54U),"independent delegated recovery trace matches exact original recovery profile");
        for (usize index = 0; index < boundaries.size(); ++index) {
            const auto boundary = boundaries[index];
            std::vector<usize> fields{2,7,11};
            // Probe every original delegated callback for Owner, cache and
            // actual Session identity; expand first/middle/last of each kind
            // to all retained outer/parent/preview seal dimensions.
            if (index < 3 || (index >= (reads/2)*3 && index < (reads/2)*3+3) || index >= boundaries.size()-3)
                for (usize field = allocated ? 0 : 2; field < 26; ++field)
                    if (std::find(fields.begin(),fields.end(),field) == fields.end()) fields.push_back(field);
            for (usize field : fields) {
                auto f = std::make_unique<ReviewFixture>(allocated,true); bool observed{};
                f->device.callback = [&](Io io,usize count) {
                    if (!observed && io == boundary.io && count == boundary.count) {
                        observed = true; delegated_drift(*f,field);
                    }
                };
                f->failure();
                if (!observed || f->device.reads != boundary.device_reads || f->device.writes || f->device.flushes)
                    std::fprintf(stderr,"DELEGATED_BOUNDARY allocated=%d index=%zu field=%zu expected_reads=%zu actual_reads=%zu observed=%d\n",
                        int(allocated),index,field,boundary.device_reads,f->device.reads,int(observed));
                require(observed && f->device.reads == boundary.device_reads && !f->device.writes && !f->device.flushes,
                        "every delegated callback rejects retained drift before any later readonly transport");
                require(f->media.blocks == *f->initial,"delegated drift preserves complete original physical media");
                ++recovery_boundary_controls;
            }
        }
        for (const auto boundary : boundaries) if (boundary.io == Io::read) {
            auto f = std::make_unique<ReviewFixture>(allocated,true);
            f->device.failure = Io::read; f->device.fail_number = boundary.count;
            f->failure();
            const auto actual = f->registry.entries_[0].report;
            selected_child(*f,[&](auto &child) {
                const auto preview = child.recovery_preview_.report();
                require(preview.status.code() == StatusCode::io_error && !preview.dispatch_pending &&
                        preview.attempts == actual.reads-preview.initial.reads &&
                        preview.completions == actual.completed_reads-preview.initial.completed_reads &&
                        preview.attempts == preview.completions+1 &&
                        reclaim_history::same_session(child.recovery_preview_.progress_,actual) &&
                        reclaim_history::same_session(preview.observed,actual),
                        "every legitimate delegated recovery READ fault retains exact independent preview accounting");
                require(!child.report().reads && !child.report().completed_reads &&
                        reclaim_history::same_session(child.progress_,child.report().initial),
                        "an incomplete delegated preview never grants parent private counter credit");
            });
            require(f->device.reads == boundary.count && !f->device.writes && !f->device.flushes && f->media.blocks == *f->initial,
                    "later delegated READ faults stop all transport and preserve complete original physical media");
            ++delegated_fault_controls;
        }
    }
}
// Authored successor expectations adapt the independently frozen V5 rejection.
int main() {
    usize controls{};
    for (bool allocated : {false,true}) for (bool recovery : {false,true}) {
        for (usize position : {usize{1},usize{7},usize{14}}) {
            for (unsigned stage = 0; stage < 3; ++stage) {
                for (usize field : {usize{2},usize{3},usize{4},usize{7},usize{8},usize{9},usize{11},usize{12},usize{13}}) {
                    auto f = std::make_unique<ReviewFixture>(allocated,recovery); bool observed{};
                    require(position <= f->residents,"selected first/middle/last physical prefix exists");
                    f->device.callback = [&](Io io,usize count) {
                        const bool selected = stage == 1 ? io == Io::read && count == position :
                            io == Io::geometry && count == position*2-(stage == 0 ? 1 : 0);
                        if (!observed && selected) { observed = true; drift(*f,field); }
                    };
                    f->failure();
                    require(observed && f->device.reads == position-(stage == 0 ? 1 : 0) &&
                            !f->device.writes && !f->device.flushes,
                            "prefix boundary refuses with no later physical I/O");
                    require(f->workspace->prefix_reads_ == position-1,
                            "drifted prefix receives no private adopted credit");
                    require(f->media.blocks == *f->initial,"prefix drift preserves complete original physical media");
                    const auto actual = f->registry.entries_[0].report;
                    require(actual.reads == f->original_session.reads+position &&
                            actual.completed_reads == f->original_session.completed_reads+position-1,
                            "prefix boundary retains attempted READ without completion credit");
                    require(!f->workspace->zero_.attempted_ && !f->workspace->allocated_.attempted_,
                            "prefix refusal precedes typed child admission");
                    ++controls;
                }
            }
        }
    }
    std::printf("PASS authored Owner prefix forwarding checks=%zu controls=%zu retirement_controls=%zu source_only=1 independent_acceptance=0\n",checks,controls,retirement_controls);
}
