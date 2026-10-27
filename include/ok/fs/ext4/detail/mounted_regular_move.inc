// Private cross-parent absent-destination regular-file operation. The source
// keeps the existing original Node mutation role; each parent has a separately
// address-bound I/O Lease for the entire writer/checkpoint/publication suffix.
struct RegularMoveParents final
{
    RegularMoveParents(const NodeMutationContext &source, MountedNode &from, MountedNode &to,
        Lease &from_hold, Lease &to_hold)
        : original(this), source(source), from(from), to(to), from_key(from.lifetime_key_),
          to_key(to.lifetime_key_), from_hold(from_hold), to_hold(to_hold) {}
    RegularMoveParents(const RegularMoveParents &) = delete;
    RegularMoveParents &operator=(const RegularMoveParents &) = delete;
    RegularMoveParents(RegularMoveParents &&) = delete;
    RegularMoveParents &operator=(RegularMoveParents &&) = delete;
    const RegularMoveParents *const original;
    const NodeMutationContext &source;
    MountedNode &from, &to;
    const Ext4InodeLifetimeKey from_key, to_key;
    Lease &from_hold, &to_hold;
};
[[nodiscard]] Status regular_move_parents_exact_locked(const RegularMoveParents &work) const
{
    if (work.original != &work || &work.source != mutation_context_ ||
        &work.from == &work.to || &work.from == &work.source.node || &work.to == &work.source.node ||
        work.from.lifetime_key_ != work.from_key || work.to.lifetime_key_ != work.to_key)
        return Status::fault("mounted regular move original participants changed");
    if (auto status = mutation_integrity_locked(&work.source); !status) return status;
    if (auto status = authentic_node_locked(work.from); !status) return status;
    if (auto status = authentic_node_locked(work.to); !status) return status;
    if (auto status = lifetime_.validate_lease(work.from_hold, work.from_key, Ext4InodeHold::io); !status)
        return status;
    return lifetime_.validate_lease(work.to_hold, work.to_key, Ext4InodeHold::io);
}
[[nodiscard]] Status regular_move_parents_exact(const RegularMoveParents &work) const
{
    if (auto status = identity(work.source.epoch, &work.source); !status) return status;
    smp::ScopedSpinLock guard(lock_);
    return regular_move_parents_exact_locked(work);
}
[[nodiscard]] Status regular_move_suffix(const RegularMoveParents &work,
    std::string_view old_name, const FixedString<max_path_segment> &replacement)
{
    const auto &source = work.source;
    const auto epoch = source.epoch;
    const auto reject = [&](Status reason) { return reject_unprepared(reason, epoch, &source); };
    if (auto status = regular_move_parents_exact(work); !status) return fail(status);
    auto original = view_.lookup(work.from_key.inode, old_name);
    if (!original) return reject(original.status());
    if (original.value() != source.key.inode)
        return reject(Status::busy("mounted regular move source namespace changed before admission"));
    auto target = view_.lookup(work.to_key.inode, replacement.view());
    if (target) return reject(Status::already_exists("mounted regular move destination exists"));
    if (target.status().code() != StatusCode::not_found) return reject(target.status());
    auto before = metadata(source.key.inode);
    if (!before) return reject(before.status());
    if (before.value().type != NodeType::regular || source.key.inode < view_.first_ordinary_inode())
        return reject(Status::unsupported("mounted regular move requires ordinary regular source"));
    for (auto key : {work.from_key, work.to_key})
    {
        auto inode = view_.inode(key.inode);
        if (!inode) return reject(inode.status());
        if (inode.value().generation != key.generation)
            return fail(Status::io_error("mounted regular move parent generation changed"));
        auto directory = metadata(key.inode);
        if (!directory) return reject(directory.status());
        if (directory.value().type != NodeType::directory || !directory.value().link_count ||
            (directory.value().mode & 0300) != 0300)
            return reject(Status::unsupported("mounted regular move parent lacks owner write/search bits"));
        if (auto status = physical_node_generation(key); !status) return fail(status);
    }
    if (auto status = reserve_name_publication(source.key.inode); !status) return reject(status);
    u32 seconds{}, nanoseconds{};
    if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
    if (auto status = regular_move_parents_exact(work); !status) return fail(status);
    const Ext4RegularMoveUpdate update{work.from_key.inode, work.to_key.inode, source.key.inode,
        work.from_key.generation, work.to_key.generation, source.key.generation,
        old_name, replacement.view(), seconds, nanoseconds};
    auto status = owner_.prepare_regular_move(update);
    if (!status)
    {
        if (owner_.report().phase != Ext4OwnedPhase::rejected) return fail(status);
        if (auto drained = owner_.rearm(); !drained) return fail(drained);
        return reject(status);
    }
    if (status = regular_move_parents_exact(work); !status) return fail(status);
    reset(view_);
    if (status = owner_.commit(); !status) return fail(status);
    if (status = regular_move_parents_exact(work); !status) return fail(status);
    if (status = owner_.checkpoint(); !status) return fail(status);
    if (status = regular_move_parents_exact(work); !status) return fail(status);
    for (usize index = 0; index < owner_.participant_count_; ++index)
    {
        std::array<std::byte, 1024> disk{}, cached{};
        if (status = owner_.bound_device().read_blocks(owner_.blocks_[index] * 2, disk); !status) return fail(status);
        typename Owner::Pin pin;
        if (status = owner_.pin(owner_.blocks_[index], pin); !status) return fail(status);
        status = pin.read(0, cached); const auto released = pin.release();
        if (!status || !released) return fail(status ? released : status);
        for (usize byte = 0; byte < disk.size(); ++byte)
            if (disk[byte] != cached[byte] || disk[byte] != std::byte(owner_.sealed_[index][byte]))
                return fail(Status::fault("mounted regular move canonical checkpoint differs"));
    }
    if (status = inspect(true); !status) return fail(status);
    if (status = view_.open(reader_, binding_); !status) return fail(status);
    auto found = view_.lookup(work.to_key.inode, replacement.view());
    if (!found || found.value() != source.key.inode ||
        view_.lookup(work.from_key.inode, old_name).status().code() != StatusCode::not_found)
        return fail(Status::fault("mounted regular move completed namespace differs"));
    auto after = metadata(source.key.inode);
    auto inode = view_.inode(source.key.inode);
    if (!after || !inode || inode.value().generation != source.key.generation ||
        after.value().type != before.value().type || after.value().mode != before.value().mode ||
        after.value().uid != before.value().uid || after.value().gid != before.value().gid ||
        after.value().size != before.value().size || after.value().blocks != before.value().blocks ||
        after.value().link_count != before.value().link_count ||
        after.value().attributes.change.seconds != seconds || after.value().attributes.change.nanoseconds != nanoseconds)
        return fail(Status::fault("mounted regular move completed file identity differs"));
    for (auto key : {work.from_key, work.to_key, source.key})
        if (status = physical_node_generation(key); !status) return fail(status);
    if (status = regular_move_parents_exact(work); !status) return fail(status);
    if (status = owner_.rearm(); !status) return fail(status);
    {
        smp::ScopedSpinLock guard(lock_);
        if (report_.epoch != epoch || report_.phase != Ext4MountPhase::updating)
            status = Status::fault("mounted regular move publication epoch changed");
        else if (status = regular_move_parents_exact_locked(work); status)
        {
            status = publish_name(source.key.inode, replacement);
            if (status) ++report_.epoch;
        }
    }
    return status ? status : fail(status);
}
[[nodiscard]] Status regular_move_admitted(const NodeMutationContext &source,
    MountedNode &from, MountedNode &to, std::string_view old_name,
    const FixedString<max_path_segment> &replacement)
{
    std::optional<Lease> from_hold, to_hold;
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = mutation_integrity_locked(&source); !status) return fail_locked(status);
        if (auto status = authentic_node_locked(from); !status) return status;
        if (auto status = authentic_node_locked(to); !status) return status;
        auto first = retain_existing(from.lifetime_key_, Ext4InodeHold::io);
        if (!first) return first.status();
        from_hold.emplace(std::move(first).value());
        auto second = retain_existing(to.lifetime_key_, Ext4InodeHold::io);
        if (!second)
        {
            const auto released = lifetime_.release(*from_hold);
            return released ? second.status() : fail_locked(released);
        }
        to_hold.emplace(std::move(second).value());
    }
    RegularMoveParents work{source, from, to, *from_hold, *to_hold};
    auto status = regular_move_suffix(work, old_name, replacement);
    {
        smp::ScopedSpinLock guard(lock_);
        const auto integrity = regular_move_parents_exact_locked(work);
        // Consume the actual two local attachments even after quarantine.
        const auto second = lifetime_.release(*to_hold);
        const auto first = lifetime_.release(*from_hold);
        if (!integrity || !second || !first)
            status = fail_locked(!integrity ? integrity : !second ? second : first);
    }
    return status;
}
[[nodiscard]] Status rename_dispatch(MountedNode &parent, std::string_view old_name,
                                    Node &destination, std::string_view new_name)
{
    if (&parent == &destination) return rename_same_parent(parent, old_name, destination, new_name);
    FixedString<max_path_segment> original_name, replacement;
    if (old_name.empty() || new_name.empty() || old_name.size() > 31 || new_name.size() > 31 ||
        old_name == "." || old_name == ".." || new_name == "." || new_name == "..")
        return Status::invalid_argument("mounted regular move component names");
    for (auto name : {old_name, new_name})
        for (char byte : name) if (!byte || byte == '/')
            return Status::invalid_argument("mounted regular move component names");
    if (auto status = original_name.assign(old_name); !status) return status;
    if (auto status = replacement.assign(new_name); !status) return status;
    MountedNode *target{};
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = admission(); !status) return status;
        if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime)
            return Status::unsupported("mounted regular move requires dirty ordered-data policy");
        if (auto status = authentic_node_locked(parent); !status) return status;
        for (auto &node : nodes_) if (&node == &destination) target = &node;
        if (!target) return Status::unsupported("mounted regular move destination is a foreign Node");
        if (auto status = authentic_node_locked(*target); !status) return status;
    }
    auto retained = parent.lookup_child_retained(original_name.view());
    if (!retained) return retained.status();
    MountedNode *file{};
    {
        smp::ScopedSpinLock guard(lock_);
        for (auto &node : nodes_) if (&node == retained.value()) file = &node;
    }
    auto status = file ? node_mutation_operation<MutationTerminalVerification::operation>(*file, [&](const NodeMutationContext &source) {
        return regular_move_admitted(source, parent, *target, original_name.view(), replacement);
    }) : Status::fault("mounted regular move retained source is not an original Node");
    const auto released = retained.value()->release();
    return released ? status : fail(released);
}
