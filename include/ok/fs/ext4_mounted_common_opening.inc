// Retained original operation behind the public caller-owned storage boundary.
[[nodiscard]] Status typed_run_common_operation(TypedColdWorkspace &work, TypedColdPublication &publication)
{
    const auto *selection = work.original_.common_;
    if (!selection || &publication.suffix_ != &work || typed_cold_opening_ != &work.original_ ||
        !owner_.common_opening_ || owner_.common_opening_->selection_ != selection || work.attempted_ ||
        !typed_cold_workspace_unused(work.owner_workspace_))
        return Status::invalid_argument("common typed operation lacks its original selected storage");
    const auto overlap = [](const void *a, usize na, const void *b, usize nb) {
        const auto x = reinterpret_cast<usize>(a), y = reinterpret_cast<usize>(b);
        return x <= y ? y - x < na : x - y < nb;
    };
    if (overlap(&work, sizeof(work), this, sizeof(*this)) ||
        overlap(&work, sizeof(work), startup_workspace_, sizeof(*startup_workspace_)) ||
        overlap(&work, sizeof(work), &work.owner_workspace_, sizeof(work.owner_workspace_)) ||
        overlap(&work, sizeof(work), owner_.common_opening_, sizeof(*owner_.common_opening_)) ||
        overlap(&work, sizeof(work), selection, sizeof(*selection)))
        return Status::invalid_argument("common typed operation overlaps a permanent or original opening domain");
    if (auto status = startup_opening(&owner_, 1); !status) return status;
    if (auto status = owner_.adopt_common_typed_cursor(*owner_.common_opening_->selection_, work.attachment_); !status)
        return typed_cold_stop(work, status);
    if (auto status = typed_classify_common_sources(work); !status) return typed_cold_stop(work, status);
    if (auto status = typed_run_cold_child(work); !status) return typed_cold_stop(work, status);
    if (auto status = typed_run_cold_suffix(work); !status) return status;
    return typed_publish_cold_root(publication);
}
template <typename Operation>
[[nodiscard]] Status common_mount_scope(typename Owner::Registry &registry,
    typename Owner::Registry::Registration key, block::SessionBounds bounds,
    typename Owner::TypedOwnerWorkspace &workspace, Operation operation)
{
    const auto overlap = [](const void *a, usize na, const void *b, usize nb) {
        const auto x = reinterpret_cast<usize>(a), y = reinterpret_cast<usize>(b);
        return x <= y ? y - x < na : x - y < nb;
    };
    {
        smp::ScopedSpinLock guard(lock_);
        if (report_.phase != Ext4MountPhase::unused) return Status::already_exists("common mount is one-shot");
        if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime || !startup_workspace_ ||
            !typed_cold_workspace_unused(workspace) || overlap(startup_workspace_, sizeof(*startup_workspace_), this, sizeof(*this)) ||
            overlap(&workspace, sizeof(workspace), this, sizeof(*this)) ||
            overlap(&workspace, sizeof(workspace), startup_workspace_, sizeof(*startup_workspace_)) ||
            (reclaim_workspace_ && (overlap(reclaim_workspace_, sizeof(*reclaim_workspace_), this, sizeof(*this)) ||
                overlap(reclaim_workspace_, sizeof(*reclaim_workspace_), startup_workspace_, sizeof(*startup_workspace_)) ||
                overlap(reclaim_workspace_, sizeof(*reclaim_workspace_), &workspace, sizeof(workspace)))))
            return Status::invalid_argument("common original permanent storage or policy");
        report_.phase = Ext4MountPhase::opening;
    }
    // Claim locks protect only permanent bindings and are released before Owner
    // opening, transport, nested operation scopes or Mount/Owner/cache locks.
    if (auto status = startup_workspace_->claim(this, &owner_); !status) return fail(status);
    if (auto status = owner_.claim_common_workspace(workspace); !status) return fail(status);
    if (reclaim_workspace_)
        if (auto status = reclaim_workspace_->claim(this); !status) return fail(status);
    if (auto status = owner_.open(registry, key, bounds); !status) return fail(status);
    identity_ = owner_.buffers(); owner_.publication_domain_ = 1;
    typename Owner::CommonOpening common(owner_, *this, *startup_workspace_, workspace);
    if (auto status = owner_.construct_common_selection(common); !status) return fail(status);
    auto &selection = *common.selection_;
    if (auto status = owner_.reserve_common_opening(common); !status)
    {
        owner_.destroy_common_selection(common);
        if (auto closed = owner_.close(); !closed) return fail(closed);
        smp::ScopedSpinLock guard(lock_); report_.phase = Ext4MountPhase::closed; report_.status = status; return status;
    }
    auto status = owner_.recover_startup(*startup_workspace_, this, 1, startup_reader_, startup_ordinary_reader_);
    if (status && owner_.common_typed_selected())
    {
        const TypedColdOpening original(*this, *startup_workspace_, workspace, &selection);
        typed_cold_opening_ = &original;
        // Actual operation storage is distinct; it ends before this original
        // context, Selection, Mount/Owner/Session and permanent child storage.
        status = operation(original);
        typed_cold_opening_ = nullptr;
    }
    else if (status) status = mount_after_startup();
    if (status)
    {
        smp::ScopedSpinLock guard(lock_);
        if (report_.phase != Ext4MountPhase::readable || report_.epoch != 1 || report_.reading ||
            !nodes_[0].inode_ || nodes_[0].lifetime_key_.mount != this ||
            nodes_[0].lifetime_key_.session != identity_.session_serial)
            status = Status::fault("common scope has no authentic terminal root");
    }
    status = owner_.finish_common_opening(common, status, binding_.journal_sequence);
    return status ? Status{} : fail(status);
}
