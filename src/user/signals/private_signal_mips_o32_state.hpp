// Included in the serialized private runtime after its resource declarations.
// The backend owns capability/capture tokens; this table owns per-lease state.
using PrivateMipsReadiness = MipsO32SignalReadiness;
namespace private_mips_backend = hw::mips_o32_private;
std::array<PrivateMipsReadiness, memory::KernelEntryStackPool::capacity> private_mips_states{};

PrivateMipsReadiness::Identity private_mips_identity(const sched::ThreadControlBlock &thread)
{
    return {reinterpret_cast<uptr>(thread.entry_stack.pool), thread.entry_stack.slot,
            thread.owner, thread.tid, thread.entry_stack.generation};
}
PrivateMipsReadiness *private_mips_state(const sched::ThreadControlBlock &thread)
{
    const auto identity = private_mips_identity(thread);
    if (!identity.valid() || identity.slot >= private_mips_states.size()) return nullptr;
    auto &state = private_mips_states[identity.slot];
    return state.identity() == identity ? &state : nullptr;
}
bool private_mips_reset(const sched::ThreadControlBlock &thread)
{
    const auto identity = private_mips_identity(thread);
    if (!identity.valid() || identity.slot >= private_mips_states.size()) return false;
    private_mips_states[identity.slot].reset(identity, private_mips_backend::capability());
    return true;
}
bool private_mips_initial(const sched::ThreadControlBlock &thread)
{
    if (!private_mips_reset(thread)) return false;
    return private_mips_state(thread)->publish_initial(private_mips_identity(thread), thread.native_frame,
                                                       private_mips_backend::capability());
}
bool private_mips_inherit(const sched::ThreadControlBlock &parent, const sched::ThreadControlBlock &child)
{
    auto *from = private_mips_state(parent);
    auto *to = private_mips_state(child);
    return from && to && to->inherit_syscall(private_mips_identity(child), *from, private_mips_identity(parent),
                                            child.native_frame, private_mips_backend::capability());
}
bool private_mips_begin(const sched::ThreadControlBlock &thread)
{
    auto *state = private_mips_state(thread);
    return state && state->begin_entry(private_mips_identity(thread), private_mips_backend::capability());
}
bool private_mips_capture(const sched::ThreadControlBlock &thread)
{
    auto *state = private_mips_state(thread);
    return state && state->capture(private_mips_identity(thread), thread.native_frame,
                                   private_mips_backend::observation(thread.native_frame));
}
void private_mips_revoke(const sched::ThreadControlBlock &thread)
{
    if (auto *state = private_mips_state(thread)) state->revoke(private_mips_identity(thread));
}
