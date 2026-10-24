// Included only by the private serialized PPC32 runtime after pool declarations.
using PrivatePpcReadiness = Ppc32PersistentReadiness;
namespace private_ppc_backend = hw::ppc32_private;
struct PrivatePpcState
{
    PrivatePpcReadiness readiness{};
    Ppc32ImageAdmission image{};
    Ppc32OwnedLease lease{};
    hw::UserFrame initial_frame{};
    hw::AddressSpace staged_space{};
    u64 image_generation{};
    bool initial{}, initial_relocation{}, initial_space{};
};
std::array<PrivatePpcState, memory::KernelEntryStackPool::capacity> private_ppc_states{};

PrivatePpcReadiness::Identity private_ppc_identity(const sched::ProcessControlBlock &process,
                                                 const sched::ThreadControlBlock &thread)
{
    if (thread.entry_stack.pool != &entry_stacks || thread.entry_stack.slot >= private_ppc_states.size()) return {};
    return {process.resources().native_space, thread.owner, thread.tid,
            private_ppc_states[thread.entry_stack.slot].image_generation};
}
PrivatePpcState *private_ppc_record(const sched::ProcessControlBlock &process,
                                  const sched::ThreadControlBlock &thread)
{
    const auto id = private_ppc_identity(process, thread);
    if (!id.valid()) return nullptr;
    auto &record = private_ppc_states[thread.entry_stack.slot];
    return record.readiness.identity() == id ? &record : nullptr;
}
PrivatePpcReadiness *private_ppc_state(const sched::ProcessControlBlock &process,
                                     const sched::ThreadControlBlock &thread)
{
    auto *record = private_ppc_record(process, thread);
    return record ? &record->readiness : nullptr;
}
void private_ppc_revoke_slot(u32 slot)
{
    if (slot >= private_ppc_states.size()) return;
    auto &record = private_ppc_states[slot];
    record.readiness.revoke();
    record.initial = record.initial_relocation = record.initial_space = false;
    record.image = {};
    record.lease = {};
    record.staged_space = {};
}
void private_ppc_revoke(const sched::ThreadControlBlock &thread)
{
    if (thread.entry_stack.pool) private_ppc_revoke_slot(thread.entry_stack.slot);
}
bool private_ppc_initial_valid(const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread)
{
    const auto *record = private_ppc_record(process, thread);
    return record && record->initial && record->readiness.state() == PrivatePpcReadiness::State::unverified &&
        record->lease.inactive() && record->image.valid(private_ppc_backend::capability()) &&
        private_ppc_backend::address_space_live(process.resources().native_space) &&
        private_ppc_backend::same_frame(record->initial_frame, thread.native_frame);
}
bool private_ppc_initial(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                         Ppc32ImageAdmission image)
{
    if (thread.entry_stack.pool != &entry_stacks || thread.entry_stack.slot >= private_ppc_states.size() ||
        !validate_entry_stack(entry_stacks, thread)) return false;
    auto &record = private_ppc_states[thread.entry_stack.slot];
    private_ppc_revoke(thread);
    if (record.image_generation == ~u64{}) return false;
    ++record.image_generation;
    record.image = image;
    record.lease = Ppc32OwnedLease::inspect(entry_stacks, thread.entry_stack, {thread.owner, thread.tid});
    if (!record.readiness.reset(private_ppc_identity(process, thread), image, record.lease,
                                private_ppc_backend::capability())) return false;
    record.initial_frame = thread.native_frame;
    record.initial = true;
    return true;
}
bool private_ppc_inherit(const sched::ProcessControlBlock &parent_process, const sched::ThreadControlBlock &parent,
                         const sched::ProcessControlBlock &child_process, const sched::ThreadControlBlock &child,
                         bool clone)
{
    auto *from = private_ppc_record(parent_process, parent);
    if (!from || from->initial) return false;
    PrivatePpcReadiness::ChildEdits edits{};
    if (clone)
    {
        auto call = lu::NativeBridge<lu::Profile::ppc>::decode_call(parent.native_frame, 5);
        if (!call || call.value().number != *lu::Codec<lu::Profile::ppc>::syscall_number(lu::Operation::clone)) return false;
        const auto &arguments = call.value().arguments;
        if (arguments[1]) edits.stack = static_cast<uptr>(arguments[1]);
        if (arguments[0] & linux_bootstrap::clone_flags::settls) edits.tls = static_cast<uptr>(arguments[3]);
    }
    if (!private_ppc_initial(child_process, child, from->image)) return false;
    auto *to = private_ppc_record(child_process, child);
    if (!to || !to->readiness.inherit_syscall(private_ppc_identity(child_process, child), from->readiness,
            private_ppc_identity(parent_process, parent), parent.native_frame, child.native_frame, edits,
            private_ppc_backend::capability())) return false;
    to->initial = false;
    return true;
}
bool private_ppc_begin(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                       const hw::KernelEntryStackView &view)
{
    auto *record = private_ppc_record(process, thread);
    return record && (!record->initial || private_ppc_backend::same_frame(record->initial_frame, thread.native_frame)) &&
        record->readiness.begin_entry(private_ppc_identity(process, thread), thread.native_frame,
            process.resources().native_space, view, private_ppc_backend::capability());
}
bool private_ppc_capture(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                         const hw::KernelEntryStackView &view)
{
    auto *record = private_ppc_record(process, thread);
    if (!record) return false;
    record->initial = false;
    return record->readiness.capture(private_ppc_identity(process, thread), thread.native_frame,
        process.resources().native_space, view, private_ppc_backend::observation(thread.native_frame),
        private_ppc_backend::capability());
}
Status private_ppc_completion(const sched::ProcessControlBlock &process, const hw::UserFrame &frame, i64 result)
{
    if (process.resources().personality != Personality::linux) return Status::success();
    // Normalize the supplied semantic result through the ordinary adapter. The
    // separate proof then compares every actual byte against the captured frame.
    auto expected_result = frame;
    if (auto status = linux_bootstrap::complete<lu::Profile::ppc>(expected_result, result); !status) return status;
    const auto normalized = lu::NativeBridge<lu::Profile::ppc>::decode_return(expected_result);
    if (!normalized) return normalized.status();
    for (const auto &thread : process.threads())
        if (&thread.native_frame == &frame)
        {
            auto *state = private_ppc_state(process, thread);
            return state && state->publish_linux_completion(private_ppc_identity(process, thread), frame,
                private_ppc_backend::capability(), normalized.value()) ? Status::success() :
                Status::fault("private PPC completion lost captured context");
        }
    return Status::fault("private PPC completion has no canonical owner");
}
Ppc32SignalContext private_ppc_return_context(const sched::ProcessControlBlock &process,
                                             const sched::ThreadControlBlock &thread)
{
    Ppc32SignalContext context{};
    auto *state = private_ppc_state(process, thread);
    const auto id = private_ppc_identity(process, thread);
    const auto cap = private_ppc_backend::capability();
    PrivatePpcReadiness::Diagnostics diagnostics{};
    if (!state || !state->pending(id, thread.native_frame, cap) ||
        !state->diagnostics(id, thread.native_frame, cap, diagnostics)) return context;
    context.hardware = Ppc32SignalContext::Hardware::native32_be_e500v2_eager_spe_only;
    context.resume = Ppc32SignalContext::Resume::normalized_no_restart;
    context.metadata = {diagnostics.original_r3, diagnostics.trap, diagnostics.dar, diagnostics.dsisr, diagnostics.result};
    context.metadata_supplied = true;
    return context;
}

void private_ppc_cancel_transactions()
{
    for (auto &record : private_ppc_states)
    {
        if (record.readiness.transaction() != PrivatePpcReadiness::Transaction::none) record.readiness.cancel();
        record.initial_relocation = record.initial_space = false;
        record.staged_space = {};
    }
}
bool private_ppc_stage_relocation(Kernel &kernel)
{
    for (const auto &process : kernel.scheduler().processes())
        if (process.resources().personality == Personality::linux)
            for (const auto &thread : process.threads())
                if (thread.native_active)
                {
                    auto *record = private_ppc_record(process, thread);
                    if (!record || !validate_entry_stack(entry_stacks, thread)) return false;
                    if (record->initial)
                    {
                        if (!private_ppc_initial_valid(process, thread)) return false;
                        record->initial_relocation = true;
                    }
                    else if (!record->readiness.stage_relocation(private_ppc_identity(process, thread),
                        thread.native_frame, private_ppc_backend::capability())) return false;
                }
    return true;
}
bool private_ppc_commit_relocation(Kernel &kernel)
{
    for (const auto &process : kernel.scheduler().processes())
        if (process.resources().personality == Personality::linux)
            for (const auto &thread : process.threads())
                if (thread.native_active)
                {
                    auto *record = private_ppc_record(process, thread);
                    if (!record || !validate_entry_stack(entry_stacks, thread)) return false;
                    if (record->initial)
                    {
                        if (!record->initial_relocation || !private_ppc_initial_valid(process, thread)) return false;
                        record->initial_relocation = false;
                    }
                    else if (!record->readiness.commit_relocation(private_ppc_identity(process, thread),
                        thread.native_frame, private_ppc_backend::capability())) return false;
                }
    return true;
}
bool private_ppc_space_update(sched::ProcessResources &resources, hw::AddressSpace previous,
                              hw::AddressSpace next, bool commit)
{
    if (resources.personality != Personality::linux) return true;
    if (!runtime_kernel) return false;
    for (const auto &process : runtime_kernel->scheduler().processes())
        if (&process.resources() == &resources)
        {
            for (const auto &thread : process.threads())
                if (thread.native_active)
                {
                    if (!validate_entry_stack(entry_stacks, thread) || thread.entry_stack.slot >= private_ppc_states.size()) return false;
                    auto before = private_ppc_identity(process, thread); before.space = previous;
                    auto after = before; after.space = next;
                    auto &record = private_ppc_states[thread.entry_stack.slot];
                    if (!(record.readiness.identity() == before)) return false;
                    if (record.initial)
                    {
                        if (commit)
                        {
                            if (!record.initial_space || !private_ppc_backend::same_space(record.staged_space, next) ||
                                !private_ppc_backend::same_frame(record.initial_frame, thread.native_frame) ||
                                !record.readiness.reset(after, record.image, record.lease, private_ppc_backend::capability())) return false;
                            record.initial_space = false; record.staged_space = {};
                        }
                        else
                        {
                            if (!private_ppc_initial_valid(process, thread)) return false;
                            record.initial_space = true; record.staged_space = next;
                        }
                    }
                    else if (commit ? !record.readiness.commit_space(before, after, thread.native_frame, private_ppc_backend::capability()) :
                        !record.readiness.stage_space(before, thread.native_frame, next, private_ppc_backend::capability())) return false;
                }
            return true;
        }
    return false;
}
