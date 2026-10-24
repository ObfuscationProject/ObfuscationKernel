// Reuse the retained bridge fixtures, executing only successful lifecycle and
// ordinary refusal paths. Changed-field controls below use detached scalars.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main retained_bridge_main
#include "model.cpp"
#undef main
#pragma GCC diagnostic pop
namespace {
usize renewals{}, quiet_refusals{}, detached_controls{}, exhaustion_refusals{};
void finish_receipt(Fixture &f, Cursor::Receipt &receipt) {
    const auto before = f.actual();
    const auto validations = f.authority.validations, completions = f.authority.completions;
    CHECK(!f.mount->owner_.typed_renew_mounted_workspace(*f.attachment, receipt)); ++quiet_refusals;
    CHECK(reclaim_history::same_session(before, f.actual()));
    CHECK(f.authority.validations == validations && f.authority.completions == completions);
    for (u32 at = 0;; ++at) {
        const auto stage = Cursor::selected_stage(Origin::mutation, f.route, at);
        if (stage == Stage::none) break;
        CHECK(f.cursor->begin_stage(*f.context, stage));
        CHECK(f.cursor->read(*f.context, 16, f.bytes));
        CHECK(f.cursor->end_stage(*f.context));
    }
    CHECK(f.cursor->close_suffix(*f.context, receipt));
    const auto closed = f.actual();
    CHECK(!f.mount->owner_.typed_renew_mounted_workspace(*f.attachment, receipt)); ++quiet_refusals;
    CHECK(!f.mount->owner_.rearm()); ++quiet_refusals;
    CHECK(reclaim_history::same_session(closed, f.actual()));
    CHECK(f.mount->owner_.typed_accept_mounted_cursor_receipt(*f.attachment, receipt));
}
void renew(Fixture &f, Cursor::Receipt &receipt) {
    auto &owner = f.mount->owner_;
    const auto before = f.actual();
    const auto entries = owner.cache_.entries_;
    const auto report = owner.cache_.report_;
    const auto operation = owner.typed_operation_serial_;
    const auto workspace = &owner.typed_attachment_->workspace_;
    const auto owner_anchor = &owner.typed_attachment_->owner_;
    const auto session_anchor = &owner.typed_attachment_->session_;
    const auto child = f.attachment->child_terminal_, suffix = f.attachment->suffix_terminal_;
    const auto validations = f.authority.validations, completions = f.authority.completions;
    CHECK(owner.typed_renew_mounted_workspace(*f.attachment, receipt)); ++renewals;
    CHECK(reclaim_history::same_session(before, f.actual()));
    CHECK(Owner::Cache::same_report(report, owner.cache_.report_));
    for (usize at = 0; at < entries.size(); ++at) {
        CHECK(Owner::Cache::same_report(entries[at].report, owner.cache_.entries_[at].report));
        CHECK(entries[at].canonical == owner.cache_.entries_[at].canonical);
        CHECK(entries[at].staged == owner.cache_.entries_[at].staged);
    }
    CHECK(f.authority.validations == validations && f.authority.completions == completions);
    CHECK(owner.typed_operation_serial_ == operation && owner.typed_operation_serial_seal_ == operation);
    CHECK(owner.typed_handoff_serial_ == operation && owner.typed_renewed_serial_ == operation);
    CHECK(&owner.typed_attachment_->workspace_ == workspace && &owner.typed_attachment_->owner_ == owner_anchor);
    CHECK(&owner.typed_attachment_->session_ == session_anchor);
    CHECK(owner.typed_workspace_ == workspace && owner.typed_workspace_seal_ == workspace);
    CHECK(!owner.typed_mounted_pending_ && !owner.typed_mounted_attachment_ && !owner.typed_mounted_workspace_);
    CHECK(!f.workspace->attempted_ && !f.workspace->published_cache_.minted_);
    CHECK(!f.workspace->zero_.attempted_ && !f.workspace->allocated_.attempted_);
    CHECK(f.workspace->phase_ == Owner::TypedOwnerPhase::ready && owner.report().phase == Ext4OwnedPhase::open);
    CHECK(f.attachment->renewed_ && !f.attachment->admitted_ && receipt.consumed_);
    CHECK(reclaim_history::same_session(child, f.attachment->child_terminal_));
    CHECK(reclaim_history::same_session(suffix, f.attachment->suffix_terminal_));
    CHECK(!owner.typed_renew_mounted_workspace(*f.attachment, receipt)); ++quiet_refusals;
    CHECK(reclaim_history::same_session(before, f.actual()));
}
void repeat_noop(Fixture &f, u64 context_serial) {
    f.attachment.reset(); f.context.reset(); f.cursor.reset();
    auto &owner = f.mount->owner_;
    f.cursor.reset(new Cursor(f.session()));
    f.context.reset(new Cursor::Context(f.session(), f.authority, owner, *f.mount, context_serial,
        f.workspace.get(), &Owner::typed_mounted_bridge_thunk<Cursor>));
    f.attachment.reset(new Attachment(owner, *f.workspace, *f.cursor, *f.context));
    CHECK(owner.typed_admit_mounted_cursor(*f.attachment));
    CHECK(f.cursor->reserve(*f.context, u32(f.image.f.binding.metadata_count / 4),
        f.image.f.binding.layout.filesystem_blocks, Origin::mutation, Opening::noop_only));
    CHECK(f.cursor->read(*f.context, 16, f.bytes));
    CHECK(f.cursor->refine(*f.context, Route::zero_noop, f.image.f.binding.journal_sequence));
    CHECK(f.cursor->pause_child(*f.context));
    CHECK(owner.run_typed_zero(*f.workspace, f.image.f.binding, f.image.update()));
    CHECK(owner.typed_resume_mounted_cursor(*f.attachment));
}
void lifecycle() {
    for (bool allocated : {false, true}) for (bool recovery : {false, true}) for (bool cross : {false, true}) {
        Fixture f(allocated, recovery, cross, allocated ? 0 : 3, recovery ? 5 : 0);
        Cursor::Receipt receipt; finish_receipt(f, receipt); renew(f, receipt);
    }
    for (bool cross : {false, true}) {
        Fixture f(false, false, cross, 0);
        for (u64 at = 0; at < 5; ++at) {
            if (at) repeat_noop(f, 37 + at);
            Cursor::Receipt receipt; finish_receipt(f, receipt);
            if (!at) {
                Owner::Pin pin; CHECK(f.mount->owner_.cache_.pin(8, pin));
                const auto before = f.actual();
                CHECK(!f.mount->owner_.typed_renew_mounted_workspace(*f.attachment, receipt)); ++quiet_refusals;
                CHECK(reclaim_history::same_session(before, f.actual())); CHECK(pin.release());
            }
            renew(f, receipt);
            CHECK(f.mount->owner_.typed_operation_serial_ == at + 1);
        }
    }
}
void detached_serial_controls() {
    std::array<u64, 9> serials{7, 7, 7, 6, 7, 7, 7, 7, 7};
    const auto exact = [](const auto &v) {
        return Owner::typed_renewal_serials_exact(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
    };
    CHECK(exact(serials));
    for (usize at = 0; at < serials.size(); ++at) {
        auto changed = serials; changed[at] = at == 3 ? 7 : 8;
        CHECK(!exact(changed)); ++detached_controls;
    }
    auto zero = serials; zero.fill(0); CHECK(!exact(zero)); ++detached_controls;
}
void coherent_capacity() {
    Fixture f(false, false, false, 0);
    { Cursor::Receipt receipt; finish_receipt(f, receipt); renew(f, receipt); }
    // Narrow capacity setup after a real completed renewal. It does not claim
    // execution of 2^64 operations and changes no child/callback/Session record.
    auto &owner = f.mount->owner_;
    owner.typed_operation_serial_ = owner.typed_operation_serial_seal_ = owner.typed_handoff_serial_ =
        owner.typed_renewed_serial_ = ~u64{0} - 1;
    f.workspace->operation_serial_ = f.workspace->operation_serial_seal_ =
        f.workspace->handoff_serial_ = f.workspace->handoff_serial_seal_ = ~u64{0} - 1;
    repeat_noop(f, 90);
    Cursor::Receipt receipt; finish_receipt(f, receipt);
    const auto before = f.actual(), child = f.child_terminal();
    for (usize at = 0; at < 2; ++at) {
        const auto result = owner.typed_renew_mounted_workspace(*f.attachment, receipt);
        CHECK(!result && result.code() == StatusCode::overflow); ++exhaustion_refusals;
        CHECK(reclaim_history::same_session(before, f.actual()));
        CHECK(reclaim_history::same_session(child, f.child_terminal()));
        CHECK(owner.typed_mounted_pending_ && !f.attachment->renewed_ && f.attachment->admitted_);
        CHECK(owner.typed_operation_serial_ == ~u64{0} && f.workspace->attempted_);
    }
}
}
int preserved_renewal_main() {
    positive_routes(); legitimate_refusal_and_lifetime();
    lifecycle(); detached_serial_controls(); coherent_capacity();
    std::printf("PASS mounted Receipt Owner workspace renewal: checks=%zu renewals=%zu quiet_refusals=%zu detached_controls=%zu exhaustion_refusals=%zu; source model only\n",
        checks, renewals, quiet_refusals, detached_controls, exhaustion_refusals);
}
