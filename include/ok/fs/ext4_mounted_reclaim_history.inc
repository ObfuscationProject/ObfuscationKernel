// Read-only active terminal projection; the physical writer gate stays closed.
static Status startup_reclaim_history_domain(const void *context)
{ return static_cast<const Ext4OwnedTransaction *>(context)->startup_domain_guard(); }
[[nodiscard]] Status startup_reclaim_result_history()
{
    if (auto status = startup_domain_guard(); !status) return status;
    auto &w = *startup_workspace_; auto &history = startup_reclaim_history_;
    if (startup_report_.initial_family != Ext4StartupFamily::reclaim_active ||
        startup_report_.phase != Ext4StartupPhase::classifying ||
        startup_reclaim_binding_.session_ != &cache_.session_ ||
        startup_reclaim_binding_.storage_ != &w.active_reclaim_ ||
        startup_reclaim_binding_.scratch_ != &w.verification_)
        return Status::fault("startup active result original binding domain differs");
    if (auto status = history.prepare(startup_reclaim_binding_,
        {this, &cache_, &w, startup_mount_, startup_epoch_, startup_generation_,
         &startup_reclaim_history_domain, this}); !status) return status;
    const auto before = reclaim_io_;
    Status result;
    if (!history.member_count_)
        result = history.prove_ordinary(w.reclaim_ordinary_, startup_route_budget_);
    else
    {
        result = startup_reclaim_result_capture_.capture_reclaim_result(cache_.session_, w.reclaim_result_,
            w.reclaim_preparation_, history, w.reclaim_result_seal_, startup_route_budget_);
        if (result)
            result = startup_reclaim_result_capture_.namespace_storage_exact();
        if (result)
            result = history.validate_remaining(startup_reclaim_result_capture_.binding_, startup_reclaim_result_capture_.source(), startup_route_budget_);
        if (result)
            result = startup_reclaim_result_capture_.namespace_storage_exact();
    }
    if (!result) return result;
    if (!exact_session(before, history.seal_.initial) || before.reads > ~block::SessionCounter{0} - history.reads_)
        return Status::fault("startup active prospective independent counter shadow differs");
    if (auto status = history.stable(); !status) return status;
    reclaim_io_.reads += history.reads_; reclaim_io_.completed_reads += history.reads_;
    if (auto status = reclaim_session_exact(); !status) return status;
    if (auto status = startup_route_budget_.refine(cache_.session_, history.member_count_,
                                                  startup_report_.initial_sequence); !status)
        return status;
    auto &composed = startup_reclaim_composed_;
    if (auto status = composed.prepare(history, w.reclaim_composed_, startup_route_budget_); !status) return status;
    result = composed.run(startup_route_budget_);
    if (!result) return result;
    if (!exact_session(reclaim_io_, composed.initial_) || reclaim_io_.reads > ~block::SessionCounter{0} - composed.reads_)
        return Status::fault("startup composed independent counter shadow differs");
    if (auto status = composed.stable(); !status) return status;
    reclaim_io_.reads += composed.reads_; reclaim_io_.completed_reads += composed.reads_;
    if (auto status = reclaim_session_exact(); !status) return status;
    startup_report_.reclaim_dry_route_validated = true;
    // This is only a prospective observation; no checkpoint or physical admission occurred.
    startup_report_.prospective_members = history.member_count_; startup_report_.reclaim_result_validated = true;
    return startup_domain_guard();
}
