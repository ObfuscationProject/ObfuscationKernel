// Included only by the private serialized n64 runtime after its pool declarations.
using PrivateN64Readiness = Mips64N64SignalReadiness;
namespace private_n64_backend = hw::mips64_n64_private;
struct PrivateN64State
{
    PrivateN64Readiness readiness{};
    u64 image_generation{};
};
std::array<PrivateN64State, memory::KernelEntryStackPool::capacity> private_n64_states{};
PrivateN64Readiness::Identity private_n64_identity(const sched::ProcessControlBlock &process,
                                                 const sched::ThreadControlBlock &thread)
{
    if (!thread.entry_stack.pool || thread.entry_stack.slot >= private_n64_states.size() ||
        thread.kernel_stack_top < memory::KernelEntryStackPool::usable_bytes) return {};
    return {reinterpret_cast<uptr>(thread.entry_stack.pool),
        thread.kernel_stack_top - memory::KernelEntryStackPool::usable_bytes, thread.kernel_stack_top,
        thread.entry_stack.slot, thread.owner, thread.tid, thread.entry_stack.generation,
        private_n64_states[thread.entry_stack.slot].image_generation, process.resources().native_space};
}
PrivateN64Readiness *private_n64_state(const sched::ProcessControlBlock &process,
                                     const sched::ThreadControlBlock &thread)
{
    const auto id = private_n64_identity(process, thread);
    if (!id.valid()) return nullptr;
    auto &state = private_n64_states[id.slot].readiness;
    return state.identity() == id ? &state : nullptr;
}
void private_n64_revoke_slot(u32 slot)
{
    if (slot < private_n64_states.size())
    {
        auto &state = private_n64_states[slot].readiness;
        state.revoke(state.identity());
    }
}
void private_n64_revoke(const sched::ThreadControlBlock &thread)
{
    if (thread.entry_stack.pool) private_n64_revoke_slot(thread.entry_stack.slot);
}
bool private_n64_reset(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread)
{
    if (!thread.entry_stack.pool || thread.entry_stack.slot >= private_n64_states.size() ||
        !validate_entry_stack(entry_stacks, thread)) return false;
    auto &entry = private_n64_states[thread.entry_stack.slot];
    entry.readiness.revoke(entry.readiness.identity());
    if (entry.image_generation == ~u64{}) return false;
    ++entry.image_generation;
    const auto id = private_n64_identity(process, thread);
    if (!id.valid()) return false;
    entry.readiness.reset(id, private_n64_backend::capability());
    return true;
}
bool private_n64_initial(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread)
{
    return private_n64_reset(process, thread) &&
        private_n64_state(process, thread)->publish_initial(private_n64_identity(process, thread),
            thread.native_frame, private_n64_backend::capability());
}
bool private_n64_inherit(const sched::ProcessControlBlock &parent_process,
                         const sched::ThreadControlBlock &parent,
                         const sched::ProcessControlBlock &child_process,
                         const sched::ThreadControlBlock &child)
{
    auto *from = private_n64_state(parent_process, parent);
    if (!from || !private_n64_reset(child_process, child)) return false;
    return private_n64_state(child_process, child)->inherit_syscall(
        private_n64_identity(child_process, child), *from, private_n64_identity(parent_process, parent),
        parent.native_frame, child.native_frame, private_n64_backend::capability());
}
bool private_n64_begin(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                       const hw::KernelEntryStackView &view)
{
    auto *state = private_n64_state(process, thread);
    return state && state->begin_entry(private_n64_identity(process, thread), thread.native_frame,
        process.resources().native_space, view, private_n64_backend::capability());
}
bool private_n64_capture(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                         const hw::KernelEntryStackView &view)
{
    auto *state = private_n64_state(process, thread);
    return state && state->capture(private_n64_identity(process, thread), thread.native_frame,
        process.resources().native_space, view, private_n64_backend::observation(thread.native_frame),
        private_n64_backend::capability());
}
Status private_n64_completion(const sched::ProcessControlBlock &process, const hw::UserFrame &frame, i64 result)
{
    for (const auto &thread : process.threads())
        if (&thread.native_frame == &frame)
        {
            auto *state = private_n64_state(process, thread);
            if (!state) return Status::fault("private n64 completion owner is unpublished");
            const auto id = private_n64_identity(process, thread);
            const auto cap = private_n64_backend::capability();
            const bool updated = process.resources().personality == Personality::linux
                ? state->publish_linux_completion(id, frame, cap, result)
                : state->publish_syscall_completion(id, frame, cap);
            return updated ? Status::success() : Status::fault("private n64 completion lost captured context");
        }
    return Status::fault("private n64 completion frame has no thread owner");
}

// These transactions run on the serialized service stack. A failed preflight
// does not mutate scheduler storage or publish replacement page tables.
bool private_n64_stage_relocation(Kernel &kernel)
{
    for (auto &entry : private_n64_states) entry.readiness.cancel_relocation();
    for (const auto &process : kernel.scheduler().processes())
        for (const auto &thread : process.threads())
            if (thread.native_active)
            {
                auto *state = private_n64_state(process, thread);
                if (!validate_entry_stack(entry_stacks, thread) || !state ||
                    !state->stage_relocation(private_n64_identity(process, thread), thread.native_frame,
                                             private_n64_backend::capability()))
                {
                    for (auto &entry : private_n64_states) entry.readiness.cancel_relocation();
                    return false;
                }
            }
    return true;
}
bool private_n64_commit_relocation(Kernel &kernel)
{
    bool valid = true;
    for (const auto &process : kernel.scheduler().processes())
        for (const auto &thread : process.threads())
            if (thread.native_active)
            {
                auto *state = private_n64_state(process, thread);
                valid &= validate_entry_stack(entry_stacks, thread).ok() && state &&
                    state->commit_relocation(private_n64_identity(process, thread), thread.native_frame,
                                              private_n64_backend::capability());
            }
    for (auto &entry : private_n64_states) entry.readiness.cancel_relocation();
    return valid;
}
bool private_n64_space_update(sched::ProcessResources &resources, hw::AddressSpace previous,
                              hw::AddressSpace next, bool commit)
{
    if (!runtime_kernel) return false;
    for (const auto &process : runtime_kernel->scheduler().processes())
        if (&process.resources() == &resources)
        {
            for (const auto &thread : process.threads())
                if (thread.native_active)
                {
                    if (!validate_entry_stack(entry_stacks, thread)) return false;
                    auto before = private_n64_identity(process, thread);
                    before.space = previous;
                    if (!before.valid()) return false;
                    auto &state = private_n64_states[before.slot].readiness;
                    auto after = before; after.space = next;
                    if (commit ? !state.publish_space(before, after, thread.native_frame, private_n64_backend::capability())
                               : !state.can_publish_space(before, thread.native_frame, next, private_n64_backend::capability()))
                        return false;
                }
            return true;
        }
    return false;
}
