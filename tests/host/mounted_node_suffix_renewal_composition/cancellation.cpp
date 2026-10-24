// Authentic cancellation retires admission only; no child/receipt is forged.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include "renewal_fixture.cpp"
#pragma GCC diagnostic pop
namespace {
usize cancellations{}, reentries{};
void cancel(Owner &owner, Workspace &workspace, Mount &mount, Authority &authority,
    const Ext4RecoveryInfo &binding, u64 serial, unsigned reads, bool refine) {
    auto &session = owner.cache_.session_;
    auto cursor = std::make_unique<Cursor>(session);
    auto context = std::make_unique<Cursor::Context>(session, authority, owner, mount, serial,
        &workspace, &Owner::typed_mounted_bridge_thunk<Cursor>);
    auto attachment = std::make_unique<Attachment>(owner, workspace, *cursor, *context);
    const auto operation = owner.typed_operation_serial_;
    const auto handoff = owner.typed_handoff_serial_, renewed = owner.typed_renewed_serial_;
    CHECK(owner.typed_admit_mounted_cursor(*attachment));
    CHECK(owner.typed_operation_serial_ == operation + 1);
    const auto *permanent = &*owner.typed_attachment_;
    const auto start = session.report(); CHECK(start);
    const auto cache_report = owner.cache_.report_;
    const auto entries = owner.cache_.entries_;
    CHECK(cursor->reserve(*context, u32(binding.metadata_count / 4),
        binding.layout.filesystem_blocks, Origin::mutation, Opening::noop_only));
    std::array<std::byte, 1024> bytes{};
    for (unsigned at = 0; at < reads; ++at) CHECK(cursor->read(*context, 16, bytes));
    if (refine) CHECK(cursor->refine(*context, Route::zero_noop, binding.journal_sequence));
    CHECK(owner.typed_cancel_mounted_cursor_before_child(*attachment,
        Status::unsupported("declared classifier request not selected")).code() == StatusCode::unsupported);
    CHECK(cursor->report().phase == Phase::refused && !cursor->report().pending);
    CHECK(!attachment->admitted_ && !attachment->renewed_ && !attachment->resumed_ && !attachment->child_);
    CHECK(!attachment->receipt_ && !attachment->receipt_consumed_ && !cursor->bridge_minted_);
    CHECK(!owner.typed_mounted_pending_ && !owner.typed_mounted_attachment_ && !owner.typed_mounted_workspace_);
    CHECK(&*owner.typed_attachment_ == permanent && &permanent->workspace_ == &workspace);
    CHECK(&permanent->owner_ == &owner && &permanent->session_ == &session);
    CHECK(owner.typed_workspace_ == &workspace && owner.typed_workspace_seal_ == &workspace);
    CHECK(owner.typed_operation_serial_ == operation + 1 && owner.typed_operation_serial_seal_ == operation + 1);
    CHECK(workspace.operation_serial_ == operation + 1 && workspace.operation_serial_seal_ == operation + 1);
    CHECK(owner.typed_handoff_serial_ == handoff && owner.typed_renewed_serial_ == renewed);
    CHECK(workspace.handoff_serial_ == handoff && workspace.handoff_serial_seal_ == handoff);
    CHECK(!workspace.attempted_ && !workspace.consumed_ && !workspace.zero_.attempted_ && !workspace.allocated_.attempted_);
    CHECK(!workspace.published_cache_.minted_ && !owner.typed_renewal_quarantined_);
    CHECK(Owner::Cache::same_report(cache_report, owner.cache_.report_));
    for (usize at = 0; at < entries.size(); ++at) {
        CHECK(Owner::Cache::same_report(entries[at].report, owner.cache_.entries_[at].report));
        CHECK(entries[at].canonical == owner.cache_.entries_[at].canonical);
        CHECK(entries[at].staged == owner.cache_.entries_[at].staged);
    }
    auto endpoint = start.value(); endpoint.reads += reads; endpoint.completed_reads += reads;
    const auto after = session.report(); CHECK(after);
    CHECK(reclaim_history::same_session(endpoint, after.value()));
    attachment.reset(); context.reset(); cursor.reset();
    const auto destroyed = session.report(); CHECK(destroyed);
    CHECK(reclaim_history::same_session(endpoint, destroyed.value()));
    CHECK(owner.report().phase == Ext4OwnedPhase::open && owner.report().status);
    ++cancellations;
}
void fresh_cancellations(bool cross) {
    Image image(0, cross); media(image, false, false, 0, 0);
    Device device(image.blocks); Owner::Registry registry;
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    auto workspace = std::make_unique<Workspace>(); Authority authority;
    const auto key = registry.register_device(device, 47000 + cross); CHECK(key);
    CHECK(mount->owner_.open(registry, key.value(), {0, device.geometry().block_count}));
    authority.observed = &mount->owner_.cache_.session_;
    const auto writes = device.writes, flushes = device.flushes;
    for (u64 at = 0; at < 6; ++at)
        cancel(mount->owner_, *workspace, *mount, authority, image.f.binding, 100 + at,
            unsigned(at % 3), bool(at % 2));
    CHECK(device.writes == writes && device.flushes == flushes);
}
void after_real_renewal(bool cross) {
    Fixture f(false, false, cross, 0);
    const auto endpoint = f.actual();
    CHECK(f.mount->owner_.typed_cancel_mounted_cursor_before_child(*f.attachment,
        Status::unsupported("child already selected")).code() == StatusCode::bad_descriptor);
    CHECK(reclaim_history::same_session(endpoint, f.actual()));
    Cursor::Receipt receipt; finish_receipt(f, receipt); renew(f, receipt);
    f.attachment.reset(); f.context.reset(); f.cursor.reset();
    for (u64 at = 0; at < 6; ++at)
        cancel(f.mount->owner_, *f.workspace, *f.mount, f.authority, f.image.f.binding,
            200 + at, unsigned(at % 3), bool(at % 2));
    repeat_noop(f, 300);
    Cursor::Receipt final_receipt; finish_receipt(f, final_receipt); renew(f, final_receipt);
    CHECK(f.mount->owner_.typed_operation_serial_ == 8);
    CHECK(f.mount->owner_.typed_handoff_serial_ == 8 && f.mount->owner_.typed_renewed_serial_ == 8);
    ++reentries;
}
}
int main() {
    for (bool cross : {false, true}) { fresh_cancellations(cross); after_real_renewal(cross); }
    std::printf("PASS permanent-anchor classifier cancellation: checks=%zu cancellations=%zu real_child_reentries=%zu; no reconstruction or fake Receipt\n",
        checks, cancellations, reentries);
}
