// Private signal delivery, frames, waits and per-architecture signal state.
#include "private.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
#ifdef OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE
static_assert(PrivateSignalCode::code_bytes <= hw::page_size);
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
namespace private_mips_backend = hw::mips_o32_private;
std::array<PrivateMipsReadiness, memory::KernelEntryStackPool::capacity> private_mips_states{};
namespace
{

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
} // namespace
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
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
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
namespace
{
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
} // namespace
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
namespace
{

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
} // namespace
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
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
std::array<PrivatePpcState, memory::KernelEntryStackPool::capacity> private_ppc_states{};

PrivatePpcReadiness::Identity private_ppc_identity(const sched::ProcessControlBlock &process,
                                                 const sched::ThreadControlBlock &thread)
{
    if (thread.entry_stack.pool != &entry_stacks || thread.entry_stack.slot >= private_ppc_states.size()) return {};
    return {process.resources().native_space, thread.owner, thread.tid,
            private_ppc_states[thread.entry_stack.slot].image_generation};
}
namespace
{
PrivatePpcState *private_ppc_record(const sched::ProcessControlBlock &process,
                                  const sched::ThreadControlBlock &thread)
{
    const auto id = private_ppc_identity(process, thread);
    if (!id.valid()) return nullptr;
    auto &record = private_ppc_states[thread.entry_stack.slot];
    return record.readiness.identity() == id ? &record : nullptr;
}
} // namespace
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
namespace
{
bool private_ppc_initial_valid(const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread)
{
    const auto *record = private_ppc_record(process, thread);
    return record && record->initial && record->readiness.state() == PrivatePpcReadiness::State::unverified &&
        record->lease.inactive() && record->image.valid(private_ppc_backend::capability()) &&
        private_ppc_backend::address_space_live(process.resources().native_space) &&
        private_ppc_backend::same_frame(record->initial_frame, thread.native_frame);
}
} // namespace
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
namespace
{
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
} // namespace

void private_ppc_cancel_transactions()
{
    for (auto &record : private_ppc_states)
    {
        if (record.readiness.transaction() != PrivatePpcReadiness::Transaction::none) record.readiness.cancel();
        record.initial_relocation = record.initial_space = false;
        record.staged_space = {};
    }
}
namespace
{
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
} // namespace
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
#endif
#ifdef OK_PRIVATE_POLL_MASK
PollSignalMasks poll_signal_masks;
SignalWaitState<lu::configured_native_profile()> signal_waits;
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
namespace
{
constexpr bool native_signal_diagnostics = true;
} // namespace
#else
namespace
{
constexpr bool native_signal_diagnostics = false;
} // namespace
#endif
namespace
{

usize private_signal_wait_live()
{
    usize count{};
    for (const auto &r : signal_waits.records())
        if (r.phase != PrivateSignalWaitState::Phase::none) ++count;
    return count;
}
void private_signal_wait_identity(const PrivateSignalWaitState::Record &r)
{
    if constexpr (!native_signal_diagnostics) return;
    emit(" pid="); emit_number(r.pid); emit(" tid="); emit_number(r.tid);
    emit(" epoch="); emit_number(r.epoch); emit(" wait="); emit_number(r.ticket.generation);
    emit(r.request.kind == SignalWaitKind::suspend ? " kind=suspend" : " kind=timed");
}
void private_signal_wait_inputs(const PrivateSignalWaitState::Record &r)
{
    if constexpr (!native_signal_diagnostics) return;
    emit(" syscall="); emit_number(r.syscall); emit(" pc="); emit_number(r.resume);
    emit(" set="); emit_number(r.set_address); emit(" info="); emit_number(r.request.info);
    emit(" timeout="); emit_number(r.timeout_address); emit(" sigsetsize="); emit_number(r.sigsetsize);
    emit(" wanted0="); emit_number(r.request.wanted.words[0]);
    emit(" wanted1="); emit_number(r.request.wanted.words[1]);
    emit(" original0="); emit_number(r.original.words[0]);
    emit(" original1="); emit_number(r.original.words[1]);
    emit(" deadline="); emit_number(r.timeout.deadline());
}
void private_signal_wait_active_mask(const sched::ThreadControlBlock &thread)
{
    if constexpr (!native_signal_diagnostics) return;
    emit(" active0="); emit_number(thread.native_signal_mask.words[0]);
    emit(" active1="); emit_number(thread.native_signal_mask.words[1]);
}
void private_signal_wait_signed(i64 value)
{
    if constexpr (!native_signal_diagnostics) return;
    if (value < 0) { emit("-"); emit_number(u64{0} - static_cast<u64>(value)); }
    else emit_number(static_cast<u64>(value));
}
} // namespace
void private_signal_wait_trace_retire(const PrivateSignalWaitState::Record &r, std::string_view cause)
{
    if constexpr (!native_signal_diagnostics) return;
    emit("OK_NATIVE_SIGNAL_WAIT_RETIRE"); private_signal_wait_identity(r);
    emit(" cause="); emit(cause); emit(" published=0 staged="); emit_number(r.result_staged);
    emit(" live_before="); emit_number(private_signal_wait_live()); emit("\n");
}
namespace
{
void private_signal_wait_trace_rearm(const PrivateSignalWaitState::Record &r,
                                      sched::WaitTicket old, const sched::ThreadControlBlock &thread,
                                      std::string_view cause)
{
    if constexpr (!native_signal_diagnostics) return;
    emit("OK_NATIVE_SIGNAL_WAIT_REARM"); private_signal_wait_identity(r);
    emit(" old_wait="); emit_number(old.generation); emit(" cause="); emit(cause);
    private_signal_wait_inputs(r); private_signal_wait_active_mask(thread);
    emit(" published=0 staged="); emit_number(r.result_staged);
    emit(" live="); emit_number(private_signal_wait_live()); emit("\n");
}
void private_signal_wait_trace_commit(const sched::ThreadControlBlock &thread, u32 signal)
{
    if constexpr (!native_signal_diagnostics) return;
    if (const auto *r = signal_waits.find(thread); r && signal_waits.saved(thread))
    {
        emit("OK_NATIVE_SIGNAL_WAIT_COMMIT"); private_signal_wait_identity(*r);
        emit(" signal="); emit_number(signal); emit(" published=1 result=-4");
        emit(" live_before="); emit_number(private_signal_wait_live()); emit("\n");
    }
}
} // namespace
#endif
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
namespace
{
bool private_linux_udp_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                                    const sched::ThreadControlBlock &thread);
} // namespace
#if defined(__i386__)
using PrivateSignalDelivery = I386SignalDelivery;
using PrivateSignalFrame = I386SignalReturn;
#elif defined(__arm__)
using PrivateSignalDelivery = Arm32SignalDelivery;
using PrivateSignalFrame = Arm32SignalReturn;
#elif defined(__aarch64__)
using PrivateSignalDelivery = Aarch64SignalDelivery;
using PrivateSignalFrame = Aarch64SignalFrame;
#elif defined(OK_PRIVATE_PPC32_SIGNAL)
using PrivateSignalDelivery = Ppc32SignalDelivery;
using PrivateSignalFrame = Ppc32SignalReturn;
#elif defined(OK_PRIVATE_MIPS64_N64_SIGNAL)
using PrivateSignalDelivery = Mips64N64SignalDelivery;
using PrivateSignalFrame = Mips64N64SignalReturn;
#elif defined(OK_PRIVATE_MIPS_O32_SIGNAL)
using PrivateSignalDelivery = MipsO32SignalDelivery;
using PrivateSignalFrame = MipsO32SignalReturn;
#elif defined(__loongarch64)
using PrivateSignalDelivery = Loongarch64SignalDelivery;
using PrivateSignalFrame = Loongarch64SignalFrame;
#elif defined(__riscv)
using PrivateSignalDelivery = RiscvSignalDelivery<private_signal_profile>;
using PrivateSignalFrame = RiscvSignalFrame<private_signal_profile>;
#else
using PrivateSignalDelivery = X86SignalDelivery;
using PrivateSignalFrame = X86SignalFrame;
#endif
#if !defined(__i386__) && !defined(__arm__) && !defined(__loongarch64) && !defined(OK_PRIVATE_MIPS_O32_SIGNAL) && !defined(OK_PRIVATE_MIPS64_N64_SIGNAL) && !defined(OK_PRIVATE_PPC32_SIGNAL)

template <typename Read>
bool private_signal_decode(const hw::UserFrame &current, Read &&read, PrivateSignalFrame::Restored &out)
{
    auto raw_stack = [](PrivateSignalFrame::Stack raw, PrivateSignalFrame::Stack &normalized) noexcept {
        normalized = raw; return true;
    };
#if defined(__aarch64__)
    return PrivateSignalFrame::decode(current.sp, current, hw::user_begin, hw::user_end, read, out, raw_stack);
#elif defined(__riscv)
    static_cast<void>(raw_stack);
    return PrivateSignalFrame::decode(current.sp, current, hw::user_begin, hw::user_end, read, out);
#else
    return PrivateSignalFrame::decode(current.sp, current, hw::user_begin, hw::user_end, 0xffbf, read, out, raw_stack);
#endif
}
#endif
using PrivateSignalView = SignalPendingView<PrivateSignalQueue, PrivateProcessQueue>;
std::array<PrivateSignalActions, sched::max_processes> private_signal_actions{};
std::array<PrivateSignalQueue, memory::KernelEntryStackPool::capacity> private_signal_queues{};
std::array<sched::SignalAltstack, memory::KernelEntryStackPool::capacity> private_signal_altstacks{};
PrivateSignalActions *private_signal_find(sched::ProcessId pid)
{
    for (auto &entry : private_signal_actions) if (entry.pid == pid) return &entry;
    return nullptr;
}

PrivateSignalActions *private_signal_process(Kernel &kernel, sched::ProcessId pid)
{
    const auto *process = kernel.scheduler().find(pid);
    if (!pid || !process || process->state() == sched::ProcessState::zombie ||
        process->state() == sched::ProcessState::exited) return nullptr;
    for (auto &entry : private_signal_actions)
        if (entry.pid == pid) return &entry;
    for (auto &entry : private_signal_actions)
        if ((!entry.pid || !kernel.scheduler().find(entry.pid)) && entry.epoch != ~u64{0})
        {
            if (!entry.shared.reset({pid, pid, ++entry.epoch})) return nullptr;
            entry.pid = pid;
            entry.table = {};
            entry.group_kill = false;
            entry.native_termination = 0;
            entry.lineage = {};
            return &entry;
        }
    return nullptr;
}
namespace
{
sched::SignalActionTable *private_signal_table(Kernel &kernel, sched::ProcessId pid)
{
    auto *entry = private_signal_process(kernel, pid);
    return entry ? &entry->table : nullptr;
}
PrivateProcessQueue *private_signal_shared(Kernel &kernel, sched::ProcessId pid)
{
    auto *entry = private_signal_process(kernel, pid);
    return entry ? &entry->shared : nullptr;
}
} // namespace
void private_signal_retire(sched::ProcessId pid)
{
    for (auto &entry : private_signal_actions)
        if (entry.pid == pid)
        {
            entry.shared.retire();
            entry.pid = 0;
            entry.table = {};
            entry.group_kill = false;
            entry.native_termination = 0;
            entry.lineage = {};
            return;
        }
}
PrivateSignalQueue *private_signal_queue(const sched::ThreadControlBlock &thread)
{
    if (!thread.entry_stack.pool || thread.entry_stack.slot >= private_signal_queues.size()) return nullptr;
    auto &queue = private_signal_queues[thread.entry_stack.slot];
    const PrivateSignalQueue::Identity identity{thread.owner, thread.tid, thread.entry_stack.generation};
    if (queue.identity() != identity)
    {
        if (!queue.reset(identity)) return nullptr;
        private_signal_altstacks[thread.entry_stack.slot].reset();
    }
    return &queue;
}
void private_signal_thread_retire(sched::ProcessId pid, sched::ThreadId tid, memory::KernelEntryStackHandle lease)
{
    if (!lease.pool || lease.slot >= private_signal_queues.size()) return;
    auto &queue = private_signal_queues[lease.slot];
    if (queue.identity() != PrivateSignalQueue::Identity{pid, tid, lease.generation}) return;
    // Revoke borrowed views while preserving the non-repeating sequence counter.
    queue.retire();
    private_signal_altstacks[lease.slot].reset();
}
namespace
{
PrivateSignalView private_signal_pending(Kernel &kernel, const sched::ProcessControlBlock &process,
                                         const sched::ThreadControlBlock &thread)
{
    if (thread.owner != process.pid() || kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread || !thread.native_active)
        return {nullptr, nullptr};
    return {private_signal_queue(thread), private_signal_shared(kernel, process.pid())};
}
} // namespace
sched::SignalAltstack *private_signal_altstack(const sched::ThreadControlBlock &thread)
{
    return private_signal_queue(thread) ? &private_signal_altstacks[thread.entry_stack.slot] : nullptr;
}
namespace
{
// Fixed FXSAVE/FPSIMD/ARM32 D32/RISC-V D/LoongArch scalar FPU state: no dynamic extension-state permissions.
constexpr auto private_signal_stack_size = [](u64) noexcept { return true; };

bool private_signal_can_enter(const sched::ProcessControlBlock &process, const hw::UserFrame &frame)
{
    // Match the backend's user PC and SP entry checks before publishing signal
    // state. A rejected destination is a bad user frame, not an exit(127).
    u64 stack_address = frame.sp;
#if defined(__i386__)
    if (!I386SignalEntry::effective_sp(frame, hw::user_begin, hw::user_end, stack_address)) return false;
    for (usize i = 8; i < 12; ++i)
        if (!I386RtSignalFrame::data_selector(frame.registers[i] & 0xffff)) return false;
#endif
    if (frame.pc < hw::user_begin || frame.pc >= hw::user_end ||
        stack_address <= hw::user_begin || stack_address > hw::user_end ||
        !hw::user_range(process.resources().native_space, stack_address - 1, 1, true)) return false;
#if defined(__arm__)
    if (!Arm32RtSignalFrame::program(frame.pc, frame.flags, hw::user_begin, hw::user_end) ||
        !hw::Arm32FpState::admitted(frame)) return false;
#elif defined(OK_PRIVATE_PPC32_SIGNAL)
    if ((frame.pc & 3) || !private_ppc_backend::frame_mode(frame)) return false;
#elif defined(OK_PRIVATE_MIPS64_N64_SIGNAL)
    if ((frame.pc & 3) || (frame.sp & 7) || !private_n64_backend::frame_mode(frame)) return false;
#elif defined(OK_PRIVATE_MIPS_O32_SIGNAL)
    if ((frame.pc & 3) || (frame.sp & 7) || !private_mips_backend::frame_mode(frame)) return false;
#elif defined(__aarch64__) || defined(__loongarch64)
    if (frame.pc & 3) return false;
#elif defined(__riscv)
    if (frame.pc & 1) return false;
#endif
    for (const auto &page : process.resources().native_pages)
        if (frame.pc >= page.virtual_address && frame.pc - page.virtual_address < hw::page_size)
            return (page.flags & hw::page_execute) != 0;
    return false;
}

bool private_signal_restorer(const sched::ProcessControlBlock &process, u64 &address)
{
    address = 0;
#ifdef OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE
    // Validate the complete code extent and its retained ownership. User VM
    // mutation cannot replace/unmap/protect this reserved page in the fixture.
    const hw::PageMapping *mapping = nullptr;
    for (const auto &page : process.resources().native_pages)
        if (page.virtual_address == private_signal_code_address) { mapping = &page; break; }
    if (!mapping || mapping->flags != (hw::page_read | hw::page_execute) ||
        !page_pool.owns(mapping->physical_address)) return false;
    constexpr auto code = PrivateSignalCode::image();
    const auto *bytes = reinterpret_cast<const std::byte *>(mapping->physical_address);
    for (usize i = 0; i < code.size(); ++i) if (bytes[i] != code[i]) return false;
    return PrivateSignalCode::entry_address(private_signal_code_address, SignalTrampolineEntry::realtime,
                                            hw::user_begin, hw::user_end, address);
#else
    static_cast<void>(process);
    return true;
#endif
}
} // namespace
Status private_signal_fork(Kernel &kernel, sched::ProcessId parent, sched::ThreadId caller, sched::ProcessId child)
{
    auto *parent_thread = kernel.scheduler().find_thread(parent, caller);
    auto *child_process = kernel.scheduler().find(child);
    if (!parent_thread || !child_process || child_process->threads().empty())
        return Status::fault("private signal fork thread owner");
    auto &child_thread = child_process->threads()[0];
    auto *from = private_signal_table(kernel, parent);
    auto *to = private_signal_table(kernel, child);
    if (!from || !to)
        return Status::fault("private signal action owner capacity");
    auto *parent_stack = private_signal_altstack(*parent_thread);
    auto *child_stack = private_signal_altstack(child_thread);
    if (!parent_stack || !child_stack)
        return Status::fault("private signal fork altstack owner");
#ifdef OK_PRIVATE_PPC32_SIGNAL
    auto *parent_process = kernel.scheduler().find(parent);
    if (!parent_process || (parent_process->resources().personality == Personality::linux &&
        !private_ppc_inherit(*parent_process, *parent_thread, *child_process, child_thread, false)))
        return Status::fault("private PPC fork signal context inheritance failed");
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    auto *parent_process = kernel.scheduler().find(parent);
    if (!parent_process || !private_n64_inherit(*parent_process, *parent_thread, *child_process, child_thread))
        return Status::fault("private n64 fork signal context inheritance failed");
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    // The caller has finalized the child return registers, SP and TLS.
    if (!private_mips_inherit(*parent_thread, child_thread))
        return Status::fault("private MIPS fork signal context inheritance failed");
#endif
    *to = from->fork_copy();
    *child_stack = *parent_stack;
    return PrivateChildExit::inherit(kernel.scheduler(), parent, caller, child, private_signal_find);
}
void private_signal_exec(Kernel &kernel, sched::ProcessControlBlock &process, PrivateSignalQueue *previous,
                         sched::SignalAltstack old_stack)
{
    auto *owner = private_signal_find(process.pid());
    auto *actions = owner ? &owner->table : nullptr;
    auto *next = private_signal_queue(process.threads()[0]);
    if (!PrivateChildExit::owns(owner, process.pid()) || !actions || !next || !previous)
    {
        runtime_failure = Status::fault("private signal exec lost owner");
        started = false;
        return;
    }
    actions->reset_for_exec();
    auto *stack = private_signal_altstack(process.threads()[0]);
    if (!stack) { runtime_failure = Status::fault("private signal exec altstack owner"); started = false; return; }
    old_stack.clear_for_exec();
    *stack = old_stack;
    if (previous == next)
    {
        if (auto status = PrivateChildExit::exec_committed(kernel.scheduler(), process.pid(), private_signal_find); !status)
        { runtime_failure = status; started = false; }
        return;
    }
    // Only the actual calling thread survives exec. Old tickets never escape
    // the serialized service stack; recreate them for the retained leader.
    if (!next->reset(next->identity()))
    {
        runtime_failure = Status::fault("private signal exec queue reset");
        started = false;
        return;
    }
    PrivateSignalQueue::Delivery ticket;
    while (previous->peek({}, ticket))
    {
        const auto &info = ticket.info;
        const auto moved = info.signal == 8 && info.code == 128 && !info.sender_pid && !info.sender_uid
            ? next->enqueue_kernel(8) : next->enqueue(info);
        if (moved != PrivateSignalQueue::Enqueue::queued || !previous->consume(ticket))
        {
            runtime_failure = Status::fault("private signal exec queue transfer");
            started = false;
            return;
        }
    }
    if (auto status = PrivateChildExit::exec_committed(kernel.scheduler(), process.pid(), private_signal_find); !status)
    { runtime_failure = status; started = false; }
}
namespace
{
bool private_signal_wait_rearm_delivery(Kernel &kernel, sched::ProcessControlBlock &process,
                                        sched::ThreadControlBlock &thread);
} // namespace
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_MIPS_O32_SIGNAL)
namespace
{
// A handler is due but its resume state cannot be verified. Like Linux
// force_sigsegv for an unusable frame, only the process terminates.
bool private_signal_unusable_frame(Kernel &kernel, sched::ProcessControlBlock &process)
{
    emit("OK_NATIVE_SIGNAL_FRAME_REFUSED pid="); emit_number(process.pid()); emit(" signal=11\n");
    finish(kernel, process.pid(), 0, 11);
    return false;
}
} // namespace
#endif
bool private_signal_deliver(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread)
{
    if (process.resources().personality != Personality::linux) return true;
    auto queue = private_signal_pending(kernel, process, thread);
    auto *actions = private_signal_table(kernel, process.pid());
    if (!queue || !actions)
    {
        runtime_failure = Status::fault("private signal owner missing");
        started = false;
        return false;
    }
    for (;;)
    {
        u32 signal{};
        u64 restorer{};
        if (!private_signal_restorer(process, restorer))
        { runtime_failure = Status::fault("private signal return page lost ownership"); started = false; return false; }
#ifdef OK_PRIVATE_PPC32_SIGNAL
        auto *readiness = private_ppc_state(process, thread);
        const auto identity = private_ppc_identity(process, thread);
        const auto capability = private_ppc_backend::capability();
        if (!readiness)
        { runtime_failure = Status::fault("private PPC signal owner missing"); started = false; return false; }
        PrivateSignalView::Delivery pending;
        if (queue.peek(thread.native_signal_mask, pending))
        {
            const auto *action = actions->find(pending.info.signal);
            if (action && !PrivateSignalDelivery::Actions::ignored(pending.info.signal, *action) &&
                action->handler != sched::SignalAction::default_handler && PrivateSignalDelivery::supported(*action) &&
                !readiness->ready(identity, thread.native_frame, capability))
            {
                if (private_ppc_initial_valid(process, thread)) return !signal_waits.saved(thread);
                runtime_failure = Status::fault("private PPC handler origin unverified");
                started = false; return false;
            }
        }
        const auto cpu_context = readiness->context(identity, thread.native_frame, capability);
#elif defined(OK_PRIVATE_MIPS64_N64_SIGNAL)
        auto *readiness = private_n64_state(process, thread);
        if (!readiness)
        { runtime_failure = Status::fault("private n64 signal lease readiness missing"); started = false; return false; }
        const auto identity = private_n64_identity(process, thread);
        const auto capability = private_n64_backend::capability();
        const auto gate = mips64_n64_handler_gate(queue, *actions, thread.native_signal_mask,
                                                *readiness, identity, thread.native_frame, capability);
        if (gate == Mips64N64HandlerGate::defer_handler) return !signal_waits.saved(thread);
        if (gate != Mips64N64HandlerGate::proceed) return private_signal_unusable_frame(kernel, process);
        const auto cpu_context = readiness->context(identity, thread.native_frame, capability);
#elif defined(OK_PRIVATE_MIPS_O32_SIGNAL)
        auto *readiness = private_mips_state(thread);
        if (!readiness)
        { runtime_failure = Status::fault("private MIPS signal lease readiness missing"); started = false; return false; }
        const auto identity = private_mips_identity(thread);
        const auto capability = private_mips_backend::capability();
        const auto gate = mips_o32_handler_gate(queue, *actions, thread.native_signal_mask,
                                               *readiness, identity, capability);
        if (gate == MipsO32HandlerGate::defer_handler) return !signal_waits.saved(thread);
        if (gate != MipsO32HandlerGate::proceed) return private_signal_unusable_frame(kernel, process);
        const auto cpu_context = readiness->context(identity, capability);
#else
        constexpr u32 cpu_context = 0xffbf;
#endif
        const auto result = PrivateSignalDelivery::deliver(queue, *actions, thread.native_signal_mask,
            thread.native_frame, *private_signal_altstack(thread), hw::user_begin, hw::user_end, cpu_context,
            [&](u64 address, std::span<const std::byte> bytes) {
                return hw::copy_to_user(process.resources().native_space, address, bytes.data(), bytes.size()).ok();
            }, signal, restorer, [&](const hw::UserFrame &staged) {
                #if defined(__arm__)
                const u64 restorer_pc = staged.registers[14] & ~u64{1};
                bool executable = false;
                for (const auto &page : process.resources().native_pages)
                    if (restorer_pc >= page.virtual_address && restorer_pc - page.virtual_address < hw::page_size)
                        executable = (page.flags & hw::page_execute) != 0;
                if (!executable) return false;
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
                return private_signal_can_enter(process, staged) &&
                    readiness->stage_handler(identity, thread.native_frame, staged, capability);
#else
                return private_signal_can_enter(process, staged);
#endif
            }, signal_waits.saved(thread) ? signal_waits.saved(thread) : poll_signal_masks.saved(thread));
        using R = PrivateSignalDelivery::Result;
        if (result == R::handler)
        {
#if defined(OK_PRIVATE_MIPS_O32_SIGNAL) || defined(OK_PRIVATE_MIPS64_N64_SIGNAL)
            if (!readiness->publish_handler(identity, thread.native_frame, capability))
            { runtime_failure = Status::fault("private MIPS handler publication lost readiness"); started = false; return false; }
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
            if (!readiness->publish_handler(identity, thread.native_frame, capability))
            { runtime_failure = Status::fault("private PPC handler publication failed"); started = false; return false; }
#endif
            poll_signal_masks.handler_committed(thread);
            private_signal_wait_trace_commit(thread, signal);
            signal_waits.handler_committed(thread);
            return true;
        }
#ifdef OK_PRIVATE_PPC32_SIGNAL
        if (readiness->transaction() != PrivatePpcReadiness::Transaction::none) readiness->cancel();
#endif
        if (result == R::none)
        {
            // An architecture deferral permits a peer to commit SIG_IGN before
            // handler publication. The logical suspend must remain blocked.
            if (signal_waits.saved(thread))
                return private_signal_wait_rearm_delivery(kernel, process, thread);
            // Ignored work may drain the queue without constructing a frame.
            if (poll_signal_masks.saved(thread)) poll_signal_masks.restore(thread);
            return true;
        }
        if (result == R::ignored) continue;
        if (result == R::terminate) finish(kernel, process.pid(), 0, static_cast<u8>(signal));
        else
        {
            runtime_failure = Status::fault("private signal unsupported/corrupt delivery");
            started = false;
        }
        return false;
    }
}
using PrivateSleepInterruption = LinuxSleepInterruption<private_signal_profile>;
bool private_sleep_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread)
{
    if (!PrivateSleepInterruption::owns(process, thread, thread.wait_registration.ticket())) return false;
    const auto queue = private_signal_pending(kernel, process, thread);
    const auto *actions = private_signal_table(kernel, process.pid());
    return queue && actions && sleep_interrupting_signal<private_signal_profile>(queue, *actions,
        thread.native_signal_mask, [](const sched::SignalAction &action) { return PrivateSignalDelivery::supported(action); });
}
bool private_sleep_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                   sched::ThreadControlBlock &thread)
{
    // No peer execution is permitted between this recipient-side action check
    // and private_signal_deliver. Enqueue never commits EINTR or consumes work.
    if (!private_sleep_signal_ready(kernel, process, thread)) return false;
    const auto now = arch::monotonic_nanoseconds();
    if (!now) { runtime_failure = now.status(); started = false; return false; }
    const auto ticket = thread.wait_registration.ticket();
    const auto completion = PrivateSleepInterruption::complete(kernel.scheduler(), process, thread, ticket, now.value(),
        [&](uptr destination, std::span<const u8> bytes) {
            return hw::copy_to_user(process.resources().native_space, destination, bytes.data(), bytes.size()).ok();
        }, [&](i64 result) { return set_result(process, thread.native_frame, result).ok(); });
    using Outcome = PrivateSleepInterruption::Outcome;
    if (completion.outcome == Outcome::lost) return false;
    if (completion.outcome == Outcome::failed)
    { runtime_failure = Status::fault("sleep interruption completion failed"); started = false; return false; }
    if (native_signal_diagnostics && completion.outcome == Outcome::interrupted)
    {
        emit("OK_NATIVE_SLEEP_INTERRUPT pid="); emit_number(process.pid());
        emit(" tid="); emit_number(thread.tid); emit(" wait="); emit_number(ticket.generation);
        emit(completion.request.absolute ? " kind=absolute" : " kind=relative");
        emit(PrivateSleepInterruption::Codec::timespec_size(completion.request.format) == 8 ? " format=time32" : " format=time64");
        emit(completion.result == -14 ? " result=efault" : " result=eintr");
        emit(" deadline="); emit_number(completion.deadline); emit(" now="); emit_number(now.value()); emit("\n");
    }
    return true;
}
struct PrivatePipeSignal
{
    memory::ObjectHandle object{};
    PipeReadWait wait{};
    SyscallOrigins::Identity origin{};
    PrivateSignalView::Delivery pending{};
    sched::SignalAction action{};
};
namespace
{

bool private_pipe_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread, PrivatePipeSignal &out)
{
    if (kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
        process.resources().personality != Personality::linux || thread.owner != process.pid() ||
        !thread.native_active || !thread.syscall_pending ||
        process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited ||
        (thread.state != sched::ProcessState::blocked && thread.state != sched::ProcessState::runnable &&
         thread.state != sched::ProcessState::running)) return false;
    const auto object = pipe_read_object(process.pid(), thread.tid);
    if (!object.pool) return false;
    auto resolved = pipe_read_objects.resolve(object);
    if (!resolved) return false;
    const auto &wait = *resolved.value();
    if (wait.pid != process.pid() || wait.tid != thread.tid) return false;
    const auto origin = syscall_origins.identity(process, thread);
    if (!syscall_origins.prepare_restart(origin, thread.native_frame, pipe_origin_binding(object, wait))) return false;
    out.object = object; out.wait = wait; out.origin = origin;
    return true;
}

bool private_pipe_signal_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread, PrivatePipeSignal &out)
{
    if (!private_pipe_signal_owned(kernel, process, thread, out)) return false;
    const auto ready = kernel.scheduler().pipe_readiness(out.wait.pipe);
    // Real data or EOF completes the read normally before pending delivery.
    if (!ready || ready.value().buffered || !ready.value().writers) return false;
    auto queue = private_signal_pending(kernel, process, thread);
    const auto *actions = private_signal_table(kernel, process.pid());
    if (!queue || !actions) return false;
    auto blocked = thread.native_signal_mask;
    while (queue.peek(blocked, out.pending))
    {
        const auto *action = actions->find(out.pending.info.signal);
        if (!action) return false;
        if (PrivateSignalDelivery::Actions::ignored(out.pending.info.signal, *action))
        { blocked.set(out.pending.info.signal); continue; }
        if (!PrivateSignalDelivery::supported(*action)) return false;
        out.action = *action;
        return true;
    }
    return false;
}
} // namespace

bool private_pipe_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread)
{
    PrivatePipeSignal selected;
    return private_pipe_signal_select(kernel, process, thread, selected);
}

bool private_pipe_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                  sched::ThreadControlBlock &thread)
{
    PrivatePipeSignal selected;
    if (!private_pipe_signal_select(kernel, process, thread, selected)) return false;
    const auto &wait = selected.wait;
    const auto binding = pipe_origin_binding(selected.object, wait);
    if (selected.action.handler == sched::SignalAction::default_handler)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    constexpr u64 sa_restart = 0x10000000;
    const bool restart = (selected.action.flags & sa_restart) != 0;
    bool published = false;
    if (restart)
    {
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
        using Publication = SyscallRestartPublication<private_signal_profile>;
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        auto *state = private_n64_state(process, thread);
        const auto identity = private_n64_identity(process, thread);
        const auto capability = private_n64_backend::capability();
#else
        auto *state = private_ppc_state(process, thread);
        const auto identity = private_ppc_identity(process, thread);
        const auto capability = private_ppc_backend::capability();
#endif
        if (state)
        {
            auto staged = Publication::stage(syscall_origins, selected.origin, binding, *state, identity,
                                             thread.native_frame, capability);
            if (staged) published = Publication::commit(staged.value(), selected.origin, binding, identity,
                                                        thread.native_frame, capability);
        }
#else
        const auto permit = syscall_origins.prepare_restart(selected.origin, thread.native_frame, binding);
        if (permit && syscall_origins.consume_restart(permit.value(), selected.origin, thread.native_frame, binding))
        { thread.native_frame = permit.value().plan().frame; published = true; }
#endif
    }
    else published = set_result(process, thread.native_frame, -4).ok();
    if (!published)
    { runtime_failure = Status::fault("pipe signal publication lost owned origin"); started = false; return false; }
    // No scheduler or user boundary occurs from selection through handler setup.
    // An already-ready ticket needs no second completion, but its pin still does.
    if (thread.wait_registration.active() &&
        !kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::interrupted))
    { runtime_failure = Status::fault("pipe signal lost current wait"); started = false; return false; }
    cancel_pipe_read(kernel, selected.object, "interrupted", restart ? 0 : -4);
    thread.syscall_pending = false;
    if (!runtime_failure) { started = false; return false; }
    if constexpr (native_signal_diagnostics)
    {
        emit(wait.vectors.count ? "OK_NATIVE_PIPE_READV_SIGNAL pid=" : "OK_NATIVE_PIPE_SIGNAL pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" request="); emit_number(wait.request); emit(" wait="); emit_number(wait.ticket.generation);
        emit(" origin="); emit_number(selected.origin.capture); emit(" signal="); emit_number(selected.pending.info.signal);
        emit(restart ? " outcome=restart" : " outcome=eintr"); emit(" pc="); emit_number(thread.native_frame.pc);
        emit(" syscall="); emit_number(*lu::Codec<private_signal_profile>::syscall_number(wait.vectors.count ? lu::Operation::readv : lu::Operation::read));
        emit(" fd="); emit_number(static_cast<u32>(wait.fd)); emit(wait.vectors.count ? " array=" : " output="); emit_number(wait.output);
        emit(" bytes="); emit_number(wait.count); emit_pipe_vectors(wait.vectors); emit(" pinned=0 live=");
        emit_number(pipe_read_objects.accounting().live_objects); emit("\n");
    }
    return true;
}
struct PrivatePipeWriteSignal
{
    memory::ObjectHandle object{};
    PipeWriteWait wait{};
    SyscallOrigins::Identity origin{};
    PrivateSignalView::Delivery pending{};
    sched::SignalAction action{};
};
namespace
{

bool private_pipe_write_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread, PrivatePipeWriteSignal &out)
{
    if (kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
        process.resources().personality != Personality::linux || thread.owner != process.pid() ||
        !thread.native_active || !thread.syscall_pending ||
        process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited ||
        (thread.state != sched::ProcessState::blocked && thread.state != sched::ProcessState::runnable &&
         thread.state != sched::ProcessState::running)) return false;
    const auto object = pipe_write_object(process.pid(), thread.tid);
    if (!object.pool) return false;
    auto resolved = pipe_write_objects.resolve(object);
    if (!resolved) return false;
    const auto &wait = *resolved.value();
    if (wait.pid != process.pid() || wait.tid != thread.tid) return false;
    const auto origin = syscall_origins.identity(process, thread);
    if (!wait.origin_capture || origin.capture != wait.origin_capture ||
        !syscall_origins.validate_bound(origin, thread.native_frame, pipe_write_origin_binding(object, wait))) return false;
    out.object = object; out.wait = wait; out.origin = origin;
    return true;
}

bool private_pipe_write_signal_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread, PrivatePipeWriteSignal &out)
{
    if (!private_pipe_write_signal_owned(kernel, process, thread, out)) return false;
    const auto ready = kernel.scheduler().pipe_readiness(out.wait.pipe);
    // Sufficient space or a readerless pipe completes normally before delivery.
    if (!ready || !ready.value().readers ||
        (ready.value().available && (out.wait.count > sched::pipe_capacity ||
                                     ready.value().available >= out.wait.count))) return false;
    auto queue = private_signal_pending(kernel, process, thread);
    const auto *actions = private_signal_table(kernel, process.pid());
    if (!queue || !actions) return false;
    auto blocked = thread.native_signal_mask;
    while (queue.peek(blocked, out.pending))
    {
        const auto *action = actions->find(out.pending.info.signal);
        if (!action) return false;
        if (PrivateSignalDelivery::Actions::ignored(out.pending.info.signal, *action))
        { blocked.set(out.pending.info.signal); continue; }
        if (!PrivateSignalDelivery::supported(*action)) return false;
        out.action = *action;
        return true;
    }
    return false;
}
} // namespace

bool private_pipe_write_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread)
{
    PrivatePipeWriteSignal selected;
    return private_pipe_write_signal_select(kernel, process, thread, selected);
}

bool private_pipe_write_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                  sched::ThreadControlBlock &thread)
{
    PrivatePipeWriteSignal selected;
    if (!private_pipe_write_signal_select(kernel, process, thread, selected)) return false;
    const auto &wait = selected.wait;
    const auto binding = pipe_write_origin_binding(selected.object, wait);
    if (selected.action.handler == sched::SignalAction::default_handler)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    constexpr u64 sa_restart = 0x10000000;
    const bool restart = !wait.committed && (selected.action.flags & sa_restart) != 0;
    bool published = false;
    if (restart)
    {
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
        using Publication = SyscallRestartPublication<private_signal_profile>;
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        auto *state = private_n64_state(process, thread);
        const auto identity = private_n64_identity(process, thread);
        const auto capability = private_n64_backend::capability();
#else
        auto *state = private_ppc_state(process, thread);
        const auto identity = private_ppc_identity(process, thread);
        const auto capability = private_ppc_backend::capability();
#endif
        if (state)
        {
            auto staged = Publication::stage(syscall_origins, selected.origin, binding, *state, identity,
                                             thread.native_frame, capability);
            if (staged) published = Publication::commit(staged.value(), selected.origin, binding, identity,
                                                        thread.native_frame, capability);
        }
#else
        const auto permit = syscall_origins.prepare_restart(selected.origin, thread.native_frame, binding);
        if (permit && syscall_origins.consume_restart(permit.value(), selected.origin, thread.native_frame, binding))
        { thread.native_frame = permit.value().plan().frame; published = true; }
#endif
    }
    else published = set_result(process, thread.native_frame, pipe_write_terminal_result(wait, -4)).ok();
    if (!published)
    { runtime_failure = Status::fault("pipe write signal publication lost owned origin"); started = false; return false; }
    // No scheduler or user boundary occurs from selection through handler setup.
    // An already-ready ticket needs no second completion, but its pin still does.
    if (thread.wait_registration.active() &&
        !kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::interrupted))
    { runtime_failure = Status::fault("pipe write signal lost current wait"); started = false; return false; }
    cancel_pipe_write(kernel, selected.object, "interrupted", restart ? 0 : pipe_write_terminal_result(wait, -4));
    thread.syscall_pending = false;
    if (!runtime_failure) { started = false; return false; }
    if constexpr (native_signal_diagnostics)
    {
        if (wait.count > sched::pipe_capacity)
            emit(wait.vectors.count ? "OK_NATIVE_PIPE_WRITEV_LARGE_SIGNAL pid=" : "OK_NATIVE_PIPE_WRITE_LARGE_SIGNAL pid=");
        else emit(wait.vectors.count ? "OK_NATIVE_PIPE_WRITEV_SIGNAL pid=" : "OK_NATIVE_PIPE_WRITE_SIGNAL pid=");
        emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" request="); emit_number(wait.request); emit(" wait="); emit_number(wait.ticket.generation);
        emit(" origin="); emit_number(selected.origin.capture); emit(" signal="); emit_number(selected.pending.info.signal);
        emit(wait.committed ? " outcome=partial" : restart ? " outcome=restart" : " outcome=eintr"); emit(" pc="); emit_number(thread.native_frame.pc);
        emit(" syscall="); emit_number(*lu::Codec<private_signal_profile>::syscall_number(wait.vectors.count ? lu::Operation::writev : lu::Operation::write));
        emit(" fd="); emit_number(static_cast<u32>(wait.fd)); emit(wait.vectors.count ? " array=" : " input="); emit_number(wait.input);
        emit(" bytes="); emit_number(wait.count); emit_pipe_vectors(wait.vectors);
        if (wait.count > sched::pipe_capacity) { emit(" committed="); emit_number(wait.committed); }
        emit(" pinned=0 live=");
        emit_number(pipe_write_objects.accounting().live_objects); emit("\n");
    }
    return true;
}
struct PrivateChildSignal
{
    memory::ObjectHandle object{};
    ChildWait wait{};
    SyscallOrigins::Identity origin{};
    PrivateSignalView::Delivery pending{};
    sched::SignalAction action{};
};
namespace
{

bool private_child_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread, PrivateChildSignal &out)
{
    if (kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
        process.resources().personality != Personality::linux || thread.owner != process.pid() ||
        !thread.native_active || !thread.syscall_pending ||
        process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited ||
        (thread.state != sched::ProcessState::blocked && thread.state != sched::ProcessState::runnable &&
         thread.state != sched::ProcessState::running)) return false;
    const auto object = child_wait_object(process.pid(), thread.tid);
    if (!object.pool) return false;
    auto resolved = child_wait_objects.resolve(object);
    if (!resolved) return false;
    const auto &wait = *resolved.value();
    if (wait.pid != process.pid() || wait.tid != thread.tid) return false;
    const auto origin = syscall_origins.identity(process, thread);
    if (!syscall_origins.prepare_restart(origin, thread.native_frame, child_origin_binding(object, wait))) return false;
    out.object = object; out.wait = wait; out.origin = origin;
    return true;
}

bool private_child_signal_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread, PrivateChildSignal &out)
{
    if (!private_child_signal_owned(kernel, process, thread, out)) return false;
    // Reaping, ECHILD and output admission errors win before caught work.
    if (child_wait_probe(kernel, process.pid(), out.wait.operation) != -11) return false;
    auto queue = private_signal_pending(kernel, process, thread);
    const auto *actions = private_signal_table(kernel, process.pid());
    if (!queue || !actions) return false;
    auto blocked = thread.native_signal_mask;
    while (queue.peek(blocked, out.pending))
    {
        const auto *action = actions->find(out.pending.info.signal);
        if (!action) return false;
        if (PrivateSignalDelivery::Actions::ignored(out.pending.info.signal, *action))
        { blocked.set(out.pending.info.signal); continue; }
        if (!PrivateSignalDelivery::supported(*action)) return false;
        out.action = *action;
        return true;
    }
    return false;
}
} // namespace

bool private_child_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread)
{
    PrivateChildSignal selected;
    return private_child_signal_select(kernel, process, thread, selected);
}

bool private_child_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                  sched::ThreadControlBlock &thread)
{
    PrivateChildSignal selected;
    if (!private_child_signal_select(kernel, process, thread, selected)) return false;
    const auto &wait = selected.wait;
    const auto binding = child_origin_binding(selected.object, wait);
    if (selected.action.handler == sched::SignalAction::default_handler)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    constexpr u64 sa_restart = 0x10000000;
    const bool restart = (selected.action.flags & sa_restart) != 0;
    bool published = false;
    if (restart)
    {
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
        using Publication = SyscallRestartPublication<private_signal_profile>;
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        auto *state = private_n64_state(process, thread);
        const auto identity = private_n64_identity(process, thread);
        const auto capability = private_n64_backend::capability();
#else
        auto *state = private_ppc_state(process, thread);
        const auto identity = private_ppc_identity(process, thread);
        const auto capability = private_ppc_backend::capability();
#endif
        if (state)
        {
            auto staged = Publication::stage(syscall_origins, selected.origin, binding, *state, identity,
                                             thread.native_frame, capability);
            if (staged) published = Publication::commit(staged.value(), selected.origin, binding, identity,
                                                        thread.native_frame, capability);
        }
#else
        const auto permit = syscall_origins.prepare_restart(selected.origin, thread.native_frame, binding);
        if (permit && syscall_origins.consume_restart(permit.value(), selected.origin, thread.native_frame, binding))
        { thread.native_frame = permit.value().plan().frame; published = true; }
#endif
    }
    else published = set_result(process, thread.native_frame, -4).ok();
    if (!published)
    { runtime_failure = Status::fault("child wait signal publication lost owned origin"); started = false; return false; }
    // No scheduler or user boundary occurs from selection through handler setup.
    // An already-ready ticket needs no second completion, but its owned request still does.
    if (thread.wait_registration.active() &&
        !kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::interrupted))
    { runtime_failure = Status::fault("child wait signal lost current wait"); started = false; return false; }
    cancel_child_wait(kernel, selected.object, "interrupted", restart ? 0 : -4);
    thread.syscall_pending = false;
    if (!runtime_failure) { started = false; return false; }
    if constexpr (native_signal_diagnostics)
    {
        emit("OK_NATIVE_CHILD_SIGNAL pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" request="); emit_number(wait.request); emit(" wait="); emit_number(wait.ticket.generation);
        emit(" origin="); emit_number(selected.origin.capture); emit(" signal="); emit_number(selected.pending.info.signal);
        emit(restart ? " outcome=restart" : " outcome=eintr"); emit(" pc="); emit_number(thread.native_frame.pc);
        emit(" selector="); emit_number(static_cast<u32>(wait.operation.selector));
        emit(" output="); emit_number(wait.operation.output); emit(" live=");
        emit_number(child_wait_objects.accounting().live_objects); emit("\n");
    }
    return true;
}
struct PrivateFutexSignal
{
    memory::ObjectHandle object{};
    FutexWait wait{};
    SyscallOrigins::Identity origin{};
    PrivateSignalView::Delivery pending{};
    sched::SignalAction action{};
};
namespace
{

bool private_futex_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread, PrivateFutexSignal &out)
{
    if (kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
        process.resources().personality != Personality::linux || thread.owner != process.pid() ||
        !thread.native_active || !thread.syscall_pending ||
        process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited ||
        thread.state != sched::ProcessState::blocked) return false;
    const auto object = futex_object(process.pid(), thread.tid);
    if (!object.pool) return false;
    const auto resolved = futex_objects.resolve(object);
    if (!resolved) return false;
    const auto &wait = *resolved.value();
    if (wait.pid != process.pid() || wait.tid != thread.tid) return false;
    const auto origin = syscall_origins.identity(process, thread);
    // Timed waits own an interruptible continuation, but no restart permit.
    if (!syscall_origins.validate_bound(origin, thread.native_frame, futex_origin_binding(object, wait)))
        return false;
    out.object = object; out.wait = wait; out.origin = origin;
    return true;
}

bool private_futex_signal_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                 const sched::ThreadControlBlock &thread, PrivateFutexSignal &out)
{
    if (!private_futex_signal_owned(kernel, process, thread, out)) return false;
    if (out.wait.timeout_address)
    {
        const auto now = arch::monotonic_nanoseconds();
        // Expiry and a previously claimed wake complete through the normal path.
        // Never reread or rebase the original relative timeout in user memory.
        if (!now || !out.wait.timeout.armed() || now.value() >= out.wait.timeout.deadline()) return false;
    }
    auto queue = private_signal_pending(kernel, process, thread);
    const auto *actions = private_signal_table(kernel, process.pid());
    if (!queue || !actions) return false;
    auto blocked = thread.native_signal_mask;
    while (queue.peek(blocked, out.pending))
    {
        const auto *action = actions->find(out.pending.info.signal);
        if (!action) return false;
        if (PrivateSignalDelivery::Actions::ignored(out.pending.info.signal, *action))
        { blocked.set(out.pending.info.signal); continue; }
        if (!PrivateSignalDelivery::supported(*action)) return false;
        out.action = *action;
        return true;
    }
    return false;
}
} // namespace

bool private_futex_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread)
{
    PrivateFutexSignal selected;
    return private_futex_signal_select(kernel, process, thread, selected);
}

bool private_futex_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                   sched::ThreadControlBlock &thread)
{
    PrivateFutexSignal selected;
    if (!private_futex_signal_select(kernel, process, thread, selected)) return false;
    const auto &wait = selected.wait;
    const auto binding = futex_origin_binding(selected.object, wait);
    if (selected.action.handler == sched::SignalAction::default_handler)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    constexpr u64 sa_restart = 0x10000000;
    // Linux ERESTART_RESTARTBLOCK becomes EINTR for a caught timed WAIT,
    // independently of SA_RESTART. Only the original null timeout may restart.
    const bool restart = !wait.timeout_address && (selected.action.flags & sa_restart) != 0;
    bool published = false;
    if (restart)
    {
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
        using Publication = SyscallRestartPublication<private_signal_profile>;
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        auto *state = private_n64_state(process, thread);
        const auto identity = private_n64_identity(process, thread);
        const auto capability = private_n64_backend::capability();
#else
        auto *state = private_ppc_state(process, thread);
        const auto identity = private_ppc_identity(process, thread);
        const auto capability = private_ppc_backend::capability();
#endif
        if (state)
        {
            auto staged = Publication::stage(syscall_origins, selected.origin, binding, *state, identity,
                                             thread.native_frame, capability);
            if (staged) published = Publication::commit(staged.value(), selected.origin, binding, identity,
                                                        thread.native_frame, capability);
        }
#else
        const auto permit = syscall_origins.prepare_restart(selected.origin, thread.native_frame, binding);
        if (permit && syscall_origins.consume_restart(permit.value(), selected.origin, thread.native_frame, binding))
        { thread.native_frame = permit.value().plan().frame; published = true; }
#endif
    }
    else published = set_result(process, thread.native_frame, -4).ok();
    if (!published)
    { runtime_failure = Status::fault("futex signal publication lost owned origin"); started = false; return false; }
    // Exact ticket retirement and object release precede handler construction.
    // Sigreturn restores only the user frame; a restart must execute anew.
    if (!kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::interrupted))
    { runtime_failure = Status::fault("futex signal lost current wait"); started = false; return false; }
    cancel_futex(kernel, selected.object, "interrupted", restart ? 0 : -4);
    thread.syscall_pending = false;
    if (!runtime_failure) { started = false; return false; }
    if constexpr (native_signal_diagnostics)
    {
        emit("OK_NATIVE_FUTEX_SIGNAL pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" wait="); emit_number(wait.ticket.generation); emit(" origin="); emit_number(selected.origin.capture);
        emit(" signal="); emit_number(selected.pending.info.signal); emit(restart ? " outcome=restart" : " outcome=eintr");
        emit(" pc="); emit_number(thread.native_frame.pc); emit(" syscall="); emit_number(wait.syscall);
        emit(" address="); emit_number(wait.address); emit(" operation="); emit_number(binding.operation);
        emit(" expected="); emit_number(wait.expected); emit(" timeout="); emit_number(wait.timeout_address);
        emit(" deadline="); emit_number(wait.timeout.deadline()); emit(" live=");
        emit_number(futex_objects.accounting().live_objects); emit("\n");
    }
    return true;
}
struct PrivatePollSignal
{
    memory::ObjectHandle object{};
    PrivateSignalView::Delivery pending{};
    sched::SignalAction action{};
    u64 now{};
};
namespace
{

bool private_poll_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread, PrivatePollSignal &out)
{
    if (kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
        process.resources().personality != Personality::linux || thread.owner != process.pid() ||
        !thread.native_active || !thread.syscall_pending ||
        process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited ||
        thread.state != sched::ProcessState::blocked || thread.wait_reason != sched::ThreadWaitReason::poll ||
        thread.wait_key != 0) return false;
    out.object = poll_object(process.pid(), thread.tid);
    if (!out.object.pool) return false;
    auto found = poll_objects.resolve(out.object);
    if (!found) return false;
    auto &wait = *found.value();
    if (wait.pid != process.pid() || wait.tid != thread.tid ||
        wait.ticket.owner != process.pid() || wait.ticket.thread != thread.tid ||
        !thread.wait_registration.pending(wait.ticket) || !wait.syscall ||
        thread.native_frame.pc != wait.resume ||
        poll_syscall_number(thread.native_frame, wait.select_sets) != wait.syscall) return false;
    return true;
}

bool private_poll_signal_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread, PrivatePollSignal &out)
{
    if (!private_poll_signal_owned(kernel, process, thread, out)) return false;
    auto found = poll_objects.resolve(out.object);
    if (!found) return false;
    auto &wait = *found.value();
    const auto now = arch::monotonic_nanoseconds();
    if (!now) return false;
    out.now = now.value();
    if (wait.timeout.armed() && out.now >= wait.timeout.deadline()) return false;
    // Provider readiness, EOF and provider errors complete on the normal path.
    const auto ready = poll_ready(kernel, wait);
    if (!ready || ready.value()) return false;
    auto queue = private_signal_pending(kernel, process, thread);
    const auto *actions = private_signal_table(kernel, process.pid());
    if (!queue || !actions) return false;
    auto blocked = thread.native_signal_mask;
    while (queue.peek(blocked, out.pending))
    {
        const auto *action = actions->find(out.pending.info.signal);
        if (!action) return false;
        if (PrivateSignalDelivery::Actions::ignored(out.pending.info.signal, *action))
        { blocked.set(out.pending.info.signal); continue; }
        if (!PrivateSignalDelivery::supported(*action)) return false;
        out.action = *action;
        return true;
    }
    return false;
}
} // namespace

bool private_poll_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread)
{
    PrivatePollSignal selected;
    return private_poll_signal_select(kernel, process, thread, selected);
}

bool private_poll_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                  sched::ThreadControlBlock &thread)
{
    PrivatePollSignal selected;
    if (!private_poll_signal_select(kernel, process, thread, selected)) return false;
    if (selected.action.handler == sched::SignalAction::default_handler)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    auto found = poll_objects.resolve(selected.object);
    if (!found) return false;
    const auto &wait = *found.value();
    const auto ticket = wait.ticket;
    const auto deadline = wait.timeout.deadline();
    const bool select_sets = wait.select_sets;
    if (!kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::interrupted)) return false;
    // Poll writes only revents on EINTR; select leaves the supplied sets intact.
    // Neither family restarts for a caught handler, even with SA_RESTART.
    const i64 result = !select_sets && !poll_output(process.resources().native_space, wait) ? -14 : -4;
    finish_poll_timeout(process.resources().native_space, wait.timeout_output, selected.now);
    const auto published = set_result(process, thread.native_frame, result);
    thread.syscall_pending = false;
    // EFAULT is an ordinary result and restores before subsequent selection.
    // EINTR leaves the temporary delivery mask live until a frame commits.
    poll_signal_masks.complete(thread, selected.object, ticket, result == -4);
    cancel_poll(kernel, selected.object, "interrupted", result);
    if (!published || !runtime_failure)
    {
        if (!published) runtime_failure = published;
        started = false;
        return false;
    }
    if constexpr (native_signal_diagnostics)
    {
        emit("OK_NATIVE_POLL_SIGNAL pid="); emit_number(process.pid()); emit(" tid="); emit_number(thread.tid);
        emit(" wait="); emit_number(ticket.generation); emit(" signal="); emit_number(selected.pending.info.signal);
        emit(select_sets ? " kind=select" : " kind=poll"); emit(result == -14 ? " result=efault" : " result=eintr");
        emit(" deadline="); emit_number(deadline); emit(" now="); emit_number(selected.now);
        emit(" live="); emit_number(poll_objects.accounting().live_objects); emit("\n");
    }
    return true;
}
using PrivateSignalWaitCodec = SignalWaitCodec<private_signal_profile>;
namespace
{

bool private_signal_wait_frame(const sched::ThreadControlBlock &thread,
                               const PrivateSignalWaitState::Record &record)
{
    if (thread.native_frame.pc != record.resume) return false;
    if (!record.result_staged) return hw::syscall(thread.native_frame).number == record.syscall;
    const auto result = lu::Codec<private_signal_profile>::decode_return(
        lu::NativeBridge<private_signal_profile>::snapshot(thread.native_frame));
    return record.request.kind == SignalWaitKind::suspend && result && result.value().error == 4;
}
bool private_signal_wait_owner(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread)
{
    const auto *r = signal_waits.find(thread);
    return r && r->phase == PrivateSignalWaitState::Phase::waiting &&
        kernel.scheduler().find(process.pid()) == &process &&
        kernel.scheduler().find_thread(process.pid(), thread.tid) == &thread &&
        process.pid() == thread.owner && process.resources().personality == Personality::linux &&
        process.state() != sched::ProcessState::zombie && process.state() != sched::ProcessState::exited &&
        thread.native_active && thread.syscall_pending && private_signal_wait_frame(thread, *r) &&
        thread.wait_registration.ticket() == r->ticket;
}
bool private_signal_wait_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread)
{
    if (!private_signal_wait_owner(kernel, process, thread)) return false;
    const auto &r = *signal_waits.find(thread);
    const auto reason = r.request.kind == SignalWaitKind::timed ? sched::ThreadWaitReason::signal_wait
                                                              : sched::ThreadWaitReason::signal_suspend;
    return thread.state == sched::ProcessState::blocked && thread.wait_reason == reason &&
        thread.wait_key == r.epoch && thread.wait_registration.pending(r.ticket);
}
} // namespace
enum class PrivateSignalWaitOutcome : u8 { none, received, timeout, interrupted, ignored, cancelled, rearm, fatal };
struct PrivateSignalWaitSelection
{
    PrivateSignalWaitOutcome outcome{};
    PrivateSignalView::Delivery pending{};
};
namespace
{
PrivateSignalWaitSelection private_signal_wait_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                                       const sched::ThreadControlBlock &thread, u64 now)
{
    if (!private_signal_wait_owner(kernel, process, thread)) return {};
    const auto &r = *signal_waits.find(thread);
    if (!thread.wait_registration.active())
    {
        // A committed external cancellation owns the outcome. A generic ready
        // wake only rearms the copied logical request; it consumes no signal.
        if (thread.state == sched::ProcessState::runnable || thread.state == sched::ProcessState::running)
            return {thread.wait_registration.completion() == sched::WaitCompletion::ready
                ? PrivateSignalWaitOutcome::rearm : PrivateSignalWaitOutcome::cancelled, {}};
        return {};
    }
    if (!private_signal_wait_owned(kernel, process, thread)) return {};
    auto pending = private_signal_pending(kernel, process, thread);
    if (!pending) return {};
    PrivateSignalWaitSelection selected;
    if (r.request.kind == SignalWaitKind::timed)
    {
        if (pending.peek(PrivateSignalWaitCodec::selection_mask(r.request.wanted), selected.pending))
        { selected.outcome = PrivateSignalWaitOutcome::received; return selected; }
        // Matching work wins at the final dequeue even when the timer elapsed.
        // With no match, the serialized elapsed timer wins unrelated interrupts.
        if (r.timeout.armed() && now >= r.timeout.deadline())
            return {PrivateSignalWaitOutcome::timeout, {}};
    }
    const auto *actions = private_signal_table(kernel, process.pid());
    if (!actions) return {};
    auto blocked = thread.native_signal_mask;
    while (pending.peek(blocked, selected.pending))
    {
        const auto *action = actions->find(selected.pending.info.signal);
        if (!action) return {};
        if (PrivateSignalDelivery::Actions::ignored(selected.pending.info.signal, *action))
        {
            if (r.request.kind == SignalWaitKind::suspend)
            { selected.outcome = PrivateSignalWaitOutcome::ignored; return selected; }
            // Linux's raw timedwait wakes for unrelated unblocked ignored
            // work. Normal return delivery drains it without a handler frame.
            selected.outcome = PrivateSignalWaitOutcome::interrupted; return selected;
        }
        if (action->handler == sched::SignalAction::default_handler)
        { selected.outcome = PrivateSignalWaitOutcome::fatal; return selected; }
        if (!PrivateSignalDelivery::supported(*action)) return {};
        selected.outcome = PrivateSignalWaitOutcome::interrupted; return selected;
    }
    return {};
}
} // namespace
bool private_signal_wait_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread, u64 now)
{
    return private_signal_wait_select(kernel, process, thread, now).outcome != PrivateSignalWaitOutcome::none;
}
namespace
{
i64 private_signal_wait_copy(sched::ProcessControlBlock &process, PrivateSignalView &queue,
                             const PrivateSignalView::Delivery &ticket, u64 output)
{
    std::array<std::byte, 128> info{};
    if (!PrivateSignalWaitCodec::encode_info(ticket.info, info) || !queue.consume(ticket))
    {
        runtime_failure = Status::fault("signal wait lost selected queue authority");
        started = false; return -5;
    }
    // Linux synchronous receive is destructive before its optional copyout.
    // Do not validate the output while empty or undo this consume on EFAULT.
    return output && !hw::copy_to_user(process.resources().native_space, output, info.data(), info.size())
        ? -14 : static_cast<i64>(ticket.info.signal);
}
} // namespace
bool private_signal_wait_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                   sched::ThreadControlBlock &thread, u64 now)
{
    auto selected = private_signal_wait_select(kernel, process, thread, now);
    using Outcome = PrivateSignalWaitOutcome;
    while (selected.outcome == Outcome::ignored)
    {
        // Like Linux get_signal, suspend drains unblocked ignored occurrences
        // without returning to userspace or restoring its temporary mask.
        auto queue = private_signal_pending(kernel, process, thread);
        if (!queue.consume(selected.pending))
        { runtime_failure = Status::fault("suspend ignored dequeue lost owner"); started = false; return false; }
        selected = private_signal_wait_select(kernel, process, thread, now);
    }
    if (selected.outcome == Outcome::none) return false;
    if (selected.outcome == Outcome::rearm)
    {
        auto &r = *signal_waits.find(thread);
        const auto reason = r.request.kind == SignalWaitKind::timed ? sched::ThreadWaitReason::signal_wait
                                                                  : sched::ThreadWaitReason::signal_suspend;
        if (!kernel.scheduler().activate_thread(process.pid(), thread.tid))
        { runtime_failure = Status::fault("signal wait rearm activation failed"); started = false; return false; }
        const auto ticket = kernel.scheduler().register_current_wait(reason, r.epoch);
        if (!ticket)
        { runtime_failure = ticket.status(); started = false; return false; }
        const auto old = r.ticket; r.ticket = ticket.value();
        private_signal_wait_trace_rearm(r, old, thread, "ready");
        return false; // The logical wait still blocks; never enter user mode.
    }
    if (selected.outcome == Outcome::fatal)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    const auto record = *signal_waits.find(thread);
    const auto cause = selected.outcome == Outcome::received ? sched::WaitCompletion::ready :
                       selected.outcome == Outcome::timeout ? sched::WaitCompletion::timeout :
                       sched::WaitCompletion::interrupted;
    if (selected.outcome != Outcome::cancelled && !kernel.scheduler().complete_wait(record.ticket, cause)) return false;
    auto queue = private_signal_pending(kernel, process, thread);
    const bool suspend_delivery = record.request.kind == SignalWaitKind::suspend && selected.outcome == Outcome::interrupted;
    if (suspend_delivery)
    {
        signal_waits.find(thread)->phase = PrivateSignalWaitState::Phase::delivery;
        signal_waits.find(thread)->result_staged = true;
    }
    else signal_waits.restore(thread);
    i64 result = selected.outcome == Outcome::timeout ? -11 : -4;
    if (selected.outcome == Outcome::received)
        result = private_signal_wait_copy(process, queue, selected.pending, record.request.info);
    thread.syscall_pending = false;
    // A suspend rearmed after frame deferral already has a verified EINTR
    // completion. PPC's completion proof is one-shot; do not republish it.
    const auto published = record.result_staged ? Status::success() : set_result(process, thread.native_frame, result);
    if (!published || !runtime_failure)
    {
        if (!published) runtime_failure = published;
        started = false; return false;
    }
    if constexpr (native_signal_diagnostics)
    {
        emit("OK_NATIVE_SIGNAL_WAIT_COMPLETE"); private_signal_wait_identity(record);
        emit(" outcome=");
        emit(selected.outcome == Outcome::received ? (result == -14 ? "efault" : "received") :
             selected.outcome == Outcome::timeout ? "timeout" : selected.outcome == Outcome::cancelled ? "cancelled" : "eintr");
        emit(" signal="); emit_number(selected.pending.info.signal);
        emit(" code="); private_signal_wait_signed(selected.pending.info.code);
        emit(" sender_pid="); emit_number(selected.pending.info.sender_pid);
        emit(" sender_uid="); emit_number(selected.pending.info.sender_uid);
        emit(" info="); emit_number(record.request.info);
        emit(" result="); private_signal_wait_signed(result);
        emit(" published="); emit_number(!suspend_delivery);
        emit(" staged="); emit_number(suspend_delivery);
        emit(" consumed="); emit_number(selected.outcome == Outcome::received);
        emit(" live="); emit_number(private_signal_wait_live());
        emit(" deadline="); emit_number(record.timeout.deadline()); emit(" now="); emit_number(now); emit("\n");
    }
    return true;
}
namespace
{
bool private_signal_wait_call(Kernel &kernel, sched::ProcessControlBlock &process,
                               sched::ThreadControlBlock &thread, bool &complete, i64 &result)
{
    const auto raw = hw::syscall(thread.native_frame);
    if (PrivateSignalWaitCodec::kind(raw.number) == SignalWaitKind::none) return false;
    const auto decoded = lu::Codec<private_signal_profile>::decode_call(
        lu::NativeBridge<private_signal_profile>::snapshot(thread.native_frame), 4);
    if (!decoded) { result = -22; return true; }
    PrivateSignalWaitCodec::Request request;
    result = PrivateSignalWaitCodec::decode(decoded.value(),
        [&](u64 address, std::span<std::byte> bytes) {
            return hw::copy_from_user(process.resources().native_space, bytes.data(), address, bytes.size()).ok();
        }, request);
    if (result) return true;
    if (signal_waits.find(thread) || poll_signal_masks.find(thread)) { result = -22; return true; }
    auto queue = private_signal_pending(kernel, process, thread);
    if (!queue) { result = -12; return true; }
    if (request.kind == SignalWaitKind::timed)
    {
        PrivateSignalView::Delivery ticket;
        if (queue.peek(PrivateSignalWaitCodec::selection_mask(request.wanted), ticket))
        { result = private_signal_wait_copy(process, queue, ticket, request.info); return true; }
        if (request.has_timeout && request.timeout.seconds == 0 && request.timeout.nanoseconds == 0)
        { result = -11; return true; }
    }
    u64 now{};
    if (request.kind == SignalWaitKind::timed && request.has_timeout)
    {
        const auto clock = arch::monotonic_nanoseconds();
        if (!clock) { result = error(clock.status()); return true; }
        now = clock.value();
    }
    const auto &arguments = decoded.value().arguments;
    const std::array<u64, 3> input{arguments[0], request.kind == SignalWaitKind::timed ? arguments[2] : 0,
                                 arguments[request.kind == SignalWaitKind::timed ? 3 : 1]};
    if (!signal_waits.begin(thread, request, raw.number, now, input)) { result = -12; return true; }
    const auto reason = request.kind == SignalWaitKind::timed ? sched::ThreadWaitReason::signal_wait
                                                            : sched::ThreadWaitReason::signal_suspend;
    const auto ticket = kernel.scheduler().register_current_wait(reason, signal_waits.find(thread)->epoch);
    if (!ticket)
    { signal_waits.restore(thread); result = error(ticket.status()); return true; }
    if (!signal_waits.bind(thread, ticket.value()))
    {
        static_cast<void>(kernel.scheduler().complete_wait(ticket.value(), sched::WaitCompletion::cancelled));
        signal_waits.restore(thread); runtime_failure = Status::fault("signal wait bind lost owner");
        started = false; result = -5; return true;
    }
    complete = false; result = 0;
    const auto &record = *signal_waits.find(thread);
    if constexpr (native_signal_diagnostics)
    {
        emit("OK_NATIVE_SIGNAL_WAIT_ARM"); private_signal_wait_identity(record);
        private_signal_wait_inputs(record); private_signal_wait_active_mask(thread);
        emit(" live="); emit_number(private_signal_wait_live()); emit("\n");
    }
    return true;
}
} // namespace
bool private_signal_wait_pending(Kernel &kernel)
{
    for (const auto &record : signal_waits.records())
    {
        if (record.phase == PrivateSignalWaitState::Phase::none) continue;
        const auto *owner = kernel.scheduler().find_thread(record.pid, record.tid);
        if (owner && owner->native_active && signal_waits.find(*owner) == &record) return true;
    }
    return false;
}
namespace
{
bool private_signal_wait_rearm_delivery(Kernel &kernel, sched::ProcessControlBlock &process,
                                        sched::ThreadControlBlock &thread)
{
    auto *r = signal_waits.find(thread);
    if (!r || !signal_waits.saved(thread)) return false;
    // A deferred architecture frame can lose its pending handler when a peer
    // commits SIG_IGN. Keep the original temporary mask and logical suspend;
    // the already staged EINTR becomes observable only in an eventual frame.
    if (!kernel.scheduler().activate_thread(process.pid(), thread.tid))
    { runtime_failure = Status::fault("suspend delivery rearm activation failed"); started = false; return false; }
    const auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::signal_suspend, r->epoch);
    if (!ticket)
    { runtime_failure = ticket.status(); started = false; return false; }
    const auto old = r->ticket;
    r->ticket = ticket.value(); r->phase = PrivateSignalWaitState::Phase::waiting;
    thread.syscall_pending = true;
    private_signal_wait_trace_rearm(*r, old, thread, "delivery");
    return false; // The caller must not enter user mode after rearming.
}
i64 private_signal_wait_enqueue(Kernel &kernel, sched::ProcessControlBlock &recipient, sched::ThreadControlBlock &target,
                                 sched::PendingSignalInfo info)
{
    const auto *real = signal_waits.real_blocked(target);
    if (real && real->contains(info.signal))
    {
        // The existing public helper checks only the live delivery mask.
        // A sigtimedwait original mask retains ignored signals at generation.
        auto *queue = private_signal_queue(target);
        if (!queue) return -12;
        const auto result = queue->enqueue(info);
        return result == PrivateSignalQueue::Enqueue::queued || result == PrivateSignalQueue::Enqueue::coalesced ? 0 : -11;
    }
    return user::enqueue_directed_signal<private_signal_profile>(recipient, target, info,
        [&](sched::ProcessId pid) { return private_signal_table(kernel, pid); },
        [&](sched::ThreadControlBlock &owner) { return private_signal_queue(owner); });
}
bool private_signal_admit_blocked(Kernel &kernel, const sched::ProcessControlBlock &recipient,
                                  const sched::ThreadControlBlock &target)
{
    // Only the captured Linux receive family may admit its pending frame.
    if (linux_udp_receive_object(recipient.pid(), target.tid).pool)
        return private_linux_udp_signal_owned(kernel, recipient, target);
    PrivatePipeSignal pipe;
    PrivatePipeWriteSignal pipe_write;
    PrivateChildSignal child;
    PrivateFutexSignal futex;
    PrivatePollSignal poll;
    return private_signal_wait_owned(kernel, recipient, target) ||
           PrivateSleepInterruption::owns(recipient, target, target.wait_registration.ticket()) ||
           private_pipe_signal_owned(kernel, recipient, target, pipe) ||
           private_pipe_write_signal_owned(kernel, recipient, target, pipe_write) ||
           private_child_signal_owned(kernel, recipient, target, child) ||
           private_futex_signal_owned(kernel, recipient, target, futex) ||
           private_poll_signal_owned(kernel, recipient, target, poll);
}
i64 private_process_kill_one(Kernel &kernel, user::ProcessSignalSender sender, u64 raw_pid, u64 raw_signal)
{
    using Codec = SignalActionCodec<private_signal_profile>;
    const auto pid = static_cast<i32>(raw_pid);
    const auto signal = static_cast<i32>(raw_signal);
    if (pid <= 0) return -95;
    const auto target = user::find_directed_signal_target(kernel.scheduler(), 0, static_cast<u32>(pid));
    if (!target) return -3;
    if (signal < 0 || signal > static_cast<i32>(Codec::max_signal)) return -22;
    if (!user::directed_signal_permitted(sender.credentials, target.process->credentials(),
                                        sender.pid == target.process->pid())) return -1;
    if (!signal) return 0;
    auto &recipient = *target.process;
    if (recipient.state() == sched::ProcessState::zombie || recipient.state() == sched::ProcessState::exited)
        return 0;
    if (recipient.resources().personality != Personality::linux) return -95;
    if (signal == static_cast<i32>(Codec::stop_signal) || signal == (Codec::mips ? 24 : 20) ||
        signal == (Codec::mips ? 26 : 21) || signal == (Codec::mips ? 27 : 22) ||
        signal == static_cast<i32>(Codec::continue_signal)) return -95;
    auto *state = private_signal_process(kernel, recipient.pid());
    if (!state) return -12;
    if (signal == 9)
    {
        // Service on the next serialized step, before any user/wait completion.
        // Self-kill must not retire the caller while its syscall is publishing.
        state->group_kill = true;
        return 0;
    }
    const auto *action = state->table.find(static_cast<u32>(signal));
    if (!action) return -22;
    const bool ignored = Codec::ignored(static_cast<u32>(signal), *action);
    // Linux prepare_signal examines the named task, not a chosen recipient or
    // an intersection across siblings. A poll saved return mask is not real_blocked.
    const auto *real = signal_waits.real_blocked(*target.thread);
    if (ignored && !target.thread->native_signal_mask.contains(static_cast<u32>(signal)) &&
        !(real && real->contains(static_cast<u32>(signal)))) return 0;
    bool live = false, unblocked = false, qualified = false;
    for (const auto &peer : recipient.threads())
    {
        if (peer.state == sched::ProcessState::exited || !peer.native_active) continue;
        live = true;
        if (peer.native_signal_mask.contains(static_cast<u32>(signal))) continue;
        unblocked = true;
        qualified |= peer.state == sched::ProcessState::runnable || peer.state == sched::ProcessState::running ||
                     private_signal_admit_blocked(kernel, recipient, peer);
    }
    if (!live || (!ignored && unblocked && !qualified)) return -95;
    const auto enqueued = state->shared.enqueue({static_cast<u32>(signal), 0,
        static_cast<u32>(sender.pid), sender.credentials.uid});
    if (enqueued == PrivateProcessQueue::Enqueue::exhausted)
    {
        runtime_failure = Status::fault("private process signal sequence exhausted");
        started = false;
        return -12;
    }
    return enqueued == PrivateProcessQueue::Enqueue::queued || enqueued == PrivateProcessQueue::Enqueue::coalesced
        ? 0 : -22;
}
i64 native_process_kill_one(Kernel &kernel, user::ProcessSignalSender sender, u64 raw_pid, u64 raw_signal)
{
    const auto pid = static_cast<i32>(raw_pid);
    const auto signal = static_cast<i32>(raw_signal);
    if (pid <= 0) return -95;
    auto *recipient = kernel.scheduler().find(static_cast<u32>(pid));
    if (!recipient) return -3;
    if (signal < 0 || signal > OK_NATIVE_SIGNAL_MAX) return -22;
    if (!user::directed_signal_permitted(sender.credentials, recipient->credentials(),
                                        sender.pid == recipient->pid())) return -1;
    if (recipient->execution() != sched::ProcessExecution::user_process) return -95;
    if (!signal || recipient->state() == sched::ProcessState::zombie ||
        recipient->state() == sched::ProcessState::exited) return 0;
    if (signal != OK_SIGTERM && signal != OK_SIGKILL) return -95;
    if (recipient->pid() == sched::user_init_pid) return -1;
    // These two numbers have the same meaning on every retained Linux ABI.
    if (recipient->resources().personality == Personality::linux)
        return private_process_kill_one(kernel, sender, raw_pid, raw_signal);
    bool live = false;
    for (const auto &thread : recipient->threads())
        live |= thread.native_active && thread.state != sched::ProcessState::exited;
    if (!live) return -95;
    auto *state = private_signal_process(kernel, recipient->pid());
    if (!state) return -12;
    // Commit on the next service step, after the sender's saved return frame.
    // A later TERM must not replace an already accepted uncatchable KILL.
    if (state->native_termination != OK_SIGKILL)
        state->native_termination = static_cast<u8>(signal);
    return 0;
}
i64 private_process_kill(Kernel &kernel, const sched::ProcessControlBlock &sender, u64 raw_pid, u64 raw_signal)
{
    const auto captured = user::process_signal_sender(sender);
    if (static_cast<i32>(raw_pid) > 0)
        return private_process_kill_one(kernel, captured, raw_pid, raw_signal);
    return user::send_process_group_signal<SignalActionCodec<private_signal_profile>::max_signal>(
        kernel.scheduler(), captured, raw_pid, raw_signal,
        [&](user::ProcessSignalSender origin, sched::ProcessId target, i32 signal) {
            return native_process_kill_one(kernel, origin, target, static_cast<u64>(signal));
        });
}
} // namespace
i64 native_process_kill(Kernel &kernel, const sched::ProcessControlBlock &sender, u64 raw_pid, u64 raw_signal)
{
    const auto captured = user::process_signal_sender(sender);
    if (static_cast<i32>(raw_pid) > 0)
        return native_process_kill_one(kernel, captured, raw_pid, raw_signal);
    return user::send_process_group_signal<OK_NATIVE_SIGNAL_MAX>(kernel.scheduler(), captured, raw_pid, raw_signal,
        [&](user::ProcessSignalSender origin, sched::ProcessId target, i32 signal) {
            return native_process_kill_one(kernel, origin, target, static_cast<u64>(signal));
        });
}
void private_process_signal_fatal(Kernel &kernel)
{
    // Stable side-table iteration survives group teardown and PCB compaction.
    for (auto &state : private_signal_actions)
        if (state.pid && (state.group_kill || state.native_termination))
        {
            const auto pid = state.pid;
            const auto signal = state.group_kill ? u8{9} : state.native_termination;
            state.group_kill = false;
            state.native_termination = 0;
            const auto *process = kernel.scheduler().find(pid);
            if (!process || process->state() == sched::ProcessState::zombie ||
                process->state() == sched::ProcessState::exited)
            {
                private_signal_retire(pid);
                continue;
            }
            finish(kernel, pid, 0, signal);
            if (!runtime_failure) return;
        }
}
bool private_signal_call(Kernel &kernel, sched::ProcessControlBlock &process,
                         sched::ThreadControlBlock &thread, bool &complete, i64 &result)
{
    if (process.resources().personality != Personality::linux) return false;
    if (private_signal_wait_call(kernel, process, thread, complete, result)) return true;
    const auto call = hw::syscall(thread.native_frame);
    using Codec = SignalActionCodec<private_signal_profile>;
    using Operation = lu::Operation;
    auto is = [&](Operation operation) {
        const auto number = lu::Codec<private_signal_profile>::syscall_number(operation);
        return number && call.number == *number;
    };
    const bool returning = is(Operation::rt_sigreturn)
#if defined(__i386__) || defined(__arm__) || defined(OK_PRIVATE_MIPS_O32_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
        || is(Operation::sigreturn)
#endif
        ;
    if (!is(Operation::rt_sigaction) && !returning &&
        !is(Operation::kill) && !is(Operation::tgkill) && !is(Operation::tkill) && !is(Operation::rt_sigpending) &&
        !is(Operation::sigaltstack)) return false;
    if (is(Operation::kill))
    {
        result = private_process_kill(kernel, process, call.arguments[0], call.arguments[1]);
        return true;
    }
    if (is(Operation::rt_sigpending))
    {
        const auto &arguments = call.arguments;
        using MaskCodec = SignalMaskCodec<private_signal_profile>;
        if (arguments[1] > MaskCodec::bytes) { result = -22; return true; }
        if (!arguments[1]) { result = 0; return true; }
        auto pending = private_signal_pending(kernel, process, thread);
        if (!pending) { result = -12; return true; }
        result = user::query_directed_signal_pending<private_signal_profile>(pending.pending_mask(),
            thread.native_signal_mask, arguments[0], arguments[1],
            [&](u64 address, std::span<const std::byte> bytes) {
                return hw::copy_to_user(process.resources().native_space, address,
                                        bytes.data(), bytes.size()).ok();
            });
        return true;
    }
    if (is(Operation::tkill) || is(Operation::tgkill))
    {
        std::array<u64, 6> arguments{};
        for (usize index = 0; index < arguments.size(); ++index)
            arguments[index] = call.arguments[index];
        result = user::send_directed_signal<private_signal_profile>(kernel.scheduler(), process,
            is(Operation::tkill), arguments,
            [&](sched::ProcessControlBlock &recipient, sched::ThreadControlBlock &target,
                sched::PendingSignalInfo info) -> i64 {
                return private_signal_wait_enqueue(kernel, recipient, target, info);
            }, [&](const sched::ProcessControlBlock &recipient, const sched::ThreadControlBlock &target) {
                return private_signal_admit_blocked(kernel, recipient, target);
            });
        return true;
    }
    auto &space = process.resources().native_space;
    auto read = [&](u64 address, std::span<std::byte> bytes) {
        return hw::copy_from_user(space, bytes.data(), address, bytes.size()).ok();
    };
    auto write = [&](u64 address, std::span<const std::byte> bytes) {
        return hw::copy_to_user(space, address, bytes.data(), bytes.size()).ok();
    };
    auto *actions = private_signal_table(kernel, process.pid());
    auto *queue = private_signal_queue(thread);
    auto *shared = private_signal_shared(kernel, process.pid());
    if (!actions || !queue || !shared) { result = -12; return true; }
    const auto &a = call.arguments;
    if (is(Operation::sigaltstack))
    {
        result = update_signal_altstack<private_signal_profile>(*private_signal_altstack(thread),
            a[0], a[1], thread.native_frame.sp, read, write, private_signal_stack_size);
        return true;
    }
    if (is(Operation::rt_sigaction))
    {
        if (a[3] != Codec::Mask::bytes) { result = -22; return true; }
        if (a[1])
        {
            std::array<std::byte, Codec::bytes> bytes{};
            if (!read(a[1], bytes)) { result = -14; return true; }
            if (!PrivateSignalDelivery::supported(Codec::sanitize(Codec::decode(bytes))))
            { result = -95; return true; }
        }
        result = update_signal_action<private_signal_profile>(*actions, static_cast<i32>(a[0]), a[1], a[2], a[3],
            read, write, [&](u32 signal) noexcept {
                shared->discard(signal);
                for (auto &peer : process.threads())
                    if (auto *pending = private_signal_queue(peer)) pending->discard(signal);
            });
        return true;
    }
    if (returning)
    {
#if defined(OK_PRIVATE_PPC32_SIGNAL)
        auto *readiness = private_ppc_state(process, thread);
        const auto identity = private_ppc_identity(process, thread);
        const auto capability = private_ppc_backend::capability();
        if (!readiness)
        { runtime_failure = Status::fault("private PPC return owner missing"); started = false; complete = false; return true; }
        Ppc32SignalReturn::Restored restored;
        const bool valid = Ppc32SignalReturn::decode(
            is(Operation::sigreturn) ? Operation::sigreturn : Operation::rt_sigreturn,
            thread.native_frame, private_ppc_return_context(process, thread), hw::user_begin, hw::user_end,
            read, [&](const hw::UserFrame &staged) { return private_signal_can_enter(process, staged); }, restored) &&
            readiness->stage_sigreturn(identity, thread.native_frame, restored.frame, capability);
        complete = false;
        thread.syscall_pending = false;
        if (!valid) finish(kernel, process.pid(), 0, 11);
        else
        {
            auto stack = *private_signal_altstack(thread);
            if (restored.restore_stack)
                apply_restored_altstack<private_signal_profile>(stack, restored.stack, restored.frame.sp, private_signal_stack_size);
            thread.native_frame = restored.frame;
            thread.native_signal_mask = restored.mask;
            *private_signal_altstack(thread) = stack;
            // User diagnostic metadata never becomes trusted capture state.
            if (!readiness->publish_sigreturn(identity, thread.native_frame, capability))
            { runtime_failure = Status::fault("private PPC return publication failed"); started = false; }
        }
#elif defined(OK_PRIVATE_MIPS64_N64_SIGNAL)
        auto *readiness = private_n64_state(process, thread);
        if (!readiness)
        { runtime_failure = Status::fault("private n64 return readiness missing"); started = false; complete = false; return true; }
        const auto identity = private_n64_identity(process, thread);
        const auto capability = private_n64_backend::capability();
        const bool valid = Mips64N64SignalReturn::restore(Operation::rt_sigreturn,
            *queue, *actions, thread.native_signal_mask, thread.native_frame,
            *private_signal_altstack(thread), readiness->context(identity, thread.native_frame, capability),
            hw::user_begin, hw::user_end, read, write,
            [&](const hw::UserFrame &staged) { return private_signal_can_enter(process, staged); },
            private_signal_stack_size);
        complete = false;
        thread.syscall_pending = false;
        if (!valid) finish(kernel, process.pid(), 0, 11);
        else if (!readiness->publish_sigreturn(identity, thread.native_frame, capability))
        { runtime_failure = Status::fault("private n64 return publication lost readiness"); started = false; }
#elif defined(OK_PRIVATE_MIPS_O32_SIGNAL)
        auto *readiness = private_mips_state(thread);
        if (!readiness)
        { runtime_failure = Status::fault("private MIPS return lease readiness missing"); started = false; complete = false; return true; }
        const auto identity = private_mips_identity(thread);
        const auto capability = private_mips_backend::capability();
        const bool valid = MipsO32SignalReturn::restore(
            is(Operation::sigreturn) ? Operation::sigreturn : Operation::rt_sigreturn,
            *queue, *actions, thread.native_signal_mask, thread.native_frame,
            *private_signal_altstack(thread), readiness->context(identity, capability),
            hw::user_begin, hw::user_end, read, write,
            [&](const hw::UserFrame &staged) { return private_signal_can_enter(process, staged); },
            private_signal_stack_size);
        complete = false;
        thread.syscall_pending = false;
        if (!valid) finish(kernel, process.pid(), 0, 11);
        else if (!readiness->publish_sigreturn(identity, thread.native_frame, capability))
        { runtime_failure = Status::fault("private MIPS return publication lost readiness"); started = false; }
        // No normal completion: edited v0 and a3 belong to the restored frame.
#elif defined(__loongarch64)
        const bool valid = Loongarch64SignalReturn::restore(*queue, *actions,
            thread.native_signal_mask, thread.native_frame, *private_signal_altstack(thread),
            hw::user_begin, hw::user_end, read, write,
            [&](const hw::UserFrame &staged) { return private_signal_can_enter(process, staged); },
            private_signal_stack_size);
        // The adapter commits r4 and edited r2/fs_base together. No TCB TLS
        // cache overrides r2 on entry. Pending SIGFPE is selected before entry.
        complete = false;
        thread.syscall_pending = false;
        if (!valid) finish(kernel, process.pid(), 0, 11);
#else
        PrivateSignalFrame::Restored restored;
#if defined(__arm__)
        const bool valid = Arm32SignalReturn::decode(
            is(Operation::sigreturn) ? Operation::sigreturn : Operation::rt_sigreturn,
            thread.native_frame, hw::user_begin, hw::user_end, read,
            [&](const hw::UserFrame &staged) { return private_signal_can_enter(process, staged); }, restored);
#elif defined(__i386__)
        const bool valid = I386SignalReturn::decode(
            is(Operation::sigreturn) ? Operation::sigreturn : Operation::rt_sigreturn,
            thread.native_frame, hw::user_begin, hw::user_end, read,
            [&](const hw::UserFrame &staged, u64) { return private_signal_can_enter(process, staged); }, restored);
#else
        const bool valid = private_signal_decode(thread.native_frame, read, restored) &&
                           private_signal_can_enter(process, restored.frame);
#endif
        complete = false;
        thread.syscall_pending = false;
        if (!valid) finish(kernel, process.pid(), 0, 11);
        else
        {
            thread.native_frame = restored.frame;
            thread.native_signal_mask = restored.mask;
#if defined(__i386__) || defined(__arm__)
            // Only RT restores alternate-stack metadata. Architectural SP is
            // preserved; any segmented effective address is admission-only.
            if (restored.restore_stack)
                apply_restored_altstack<private_signal_profile>(*private_signal_altstack(thread),
                    restored.stack, restored.frame.sp, private_signal_stack_size);
#else
#if defined(__riscv)
            // RV TP is exclusively saved in native_frame.registers[4]. No
            // FS/GS TCB mirror is consulted by this backend on subsequent entry.
            const sched::SignalAltstack stack{restored.stack.base, restored.stack.size, restored.stack.flags};
#else
            const sched::SignalAltstack stack{restored.stack.pointer, restored.stack.size, restored.stack.flags};
#endif
            apply_restored_altstack<private_signal_profile>(*private_signal_altstack(thread),
                stack, restored.frame.sp, private_signal_stack_size);
#endif
        }
#endif
        result = 0;
        return true;
    }
    result = -38;
    return true;
}
Status private_child_exit_commit(Kernel &kernel, PrivateChildExit::Completion &completion)
{
    PrivateChildExit::Notification notification{};
    const auto status = PrivateChildExit::commit(kernel.scheduler(), completion, private_signal_find, notification);
    if (!status) return status;
    if constexpr (native_signal_diagnostics)
    {
        emit("OK_NATIVE_CHILD_EXIT child="); emit_number(completion.child);
        emit(" parent="); emit_number(completion.parent);
        emit(" parent_tid="); emit_number(completion.parent_tid);
        emit(" parent_epoch="); emit_number(completion.parent_epoch);
        emit(" uid="); emit_number(completion.uid);
        emit(" code="); emit_number(static_cast<u32>(completion.notification.code()));
        emit(" status="); emit_number(static_cast<u32>(completion.notification.status()));
        emit(" wait_code="); emit_number(static_cast<u32>(completion.wait_cause.code()));
        emit(" wait_status="); emit_number(static_cast<u32>(completion.wait_cause.status()));
        emit(" published="); emit_number(completion.published);
        emit(" notify="); emit_number(completion.notify);
        emit(" queued="); emit_number(notification == PrivateChildExit::Notification::queued);
        emit(" coalesced="); emit_number(notification == PrivateChildExit::Notification::coalesced);
        emit(" autoreap="); emit_number(completion.autoreap); emit("\n");
    }
    return Status::success();
}

bool private_child_reap(Kernel &kernel)
{
    sched::OrphanReapProgress progress{};
    const auto status = kernel.scheduler().reap_orphans_checked(progress);
    if (native_signal_diagnostics && (progress.reaped || !status))
    {
        emit("OK_NATIVE_CHILD_REAP count="); emit_number(progress.reaped);
        emit(" failed_pid="); emit_number(progress.failed_pid);
        emit(" failed_after_erase="); emit_number(progress.failed_after_erase);
        emit(" success="); emit_number(status.ok()); emit("\n");
    }
    if (!status) { runtime_failure = status; started = false; }
    return status.ok();
}
#endif
void queue_pipe_sigpipe(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid)
{
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
    if (process.resources().personality != Personality::linux) return;
    auto *thread = kernel.scheduler().find_thread(process.pid(), tid);
    if (kernel.scheduler().find(process.pid()) != &process || !thread ||
        kernel.scheduler().current_thread() != thread || !thread->native_active || !thread->syscall_pending)
    {
        runtime_failure = Status::fault("pipe SIGPIPE lost calling thread ownership");
        started = false;
        return;
    }
    // Linux pipe_write uses send_sig(SIGPIPE, current, 0): SI_USER, group PID,
    // real UID. Existing standard pending work retains its original payload.
    const auto result = user::enqueue_directed_signal<private_signal_profile>(process, *thread,
        {13, 0, static_cast<u32>(process.pid()), process.credentials().uid},
        [&](sched::ProcessId pid) { return private_signal_table(kernel, pid); },
        [&](sched::ThreadControlBlock &owner) { return private_signal_queue(owner); });
    if (result != 0)
    {
        runtime_failure = Status::fault("pipe SIGPIPE could not retain pending signal");
        started = false;
    }
#else
    static_cast<void>(kernel); static_cast<void>(process); static_cast<void>(tid);
#endif
}
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
#if defined(OK_ENABLE_TEST_POINTS)
namespace
{
constexpr bool linux_udp_signal_diagnostics = true;
} // namespace
#else
namespace
{
constexpr bool linux_udp_signal_diagnostics = false;
} // namespace
#endif
struct PrivateLinuxUdpSignal
{
    memory::ObjectHandle object{};
    SyscallOrigins::LinuxUdpReceiveBinding binding{};
    PrivateSignalView::Delivery pending{};
    sched::SignalAction action{};
};
#ifdef OK_LINUX_UDP_SIGNAL_FAILURE_FIXTURE
namespace
{
// A scalar hosted input exercises the real encoder after exact consumption.
// It cannot replace capture, publication authority or a scheduling boundary.
i64 private_linux_udp_signal_model_result{-4};
} // namespace
#endif
namespace
{

bool private_linux_udp_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                                    const sched::ThreadControlBlock &thread, PrivateLinuxUdpSignal &out)
{
    if (!runtime_failure || kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
        process.resources().personality != Personality::linux || thread.owner != process.pid() ||
        !thread.native_active || !thread.syscall_pending ||
        process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited)
        return false;
    const auto object = linux_udp_receive_object(process.pid(), thread.tid);
    if (object.pool != &linux_udp_receive_objects) return false;
    const auto *member = linux_udp_receive_member(object);
    const auto resolved = member ? linux_udp_receive_objects.resolve(object)
        : Result<LinuxUdpReceiveWait *>{Status::denied("Linux UDP signal lost membership")};
    if (!resolved) return false;
    const auto &wait = *resolved.value();
    const auto cause = thread.wait_registration.active() ? sched::WaitCompletion::none
                                                         : thread.wait_registration.completion();
    if (member->pid != process.pid() || member->tid != thread.tid || wait.pid != process.pid() ||
        wait.tid != thread.tid || wait.phase != LinuxUdpReceivePhase::published || !wait.capture || !wait.binding ||
        *wait.binding != linux_udp_receive_binding(object, wait) ||
        syscall_origins.linux_udp_receive_borrowed(*wait.capture) ||
        !syscall_origins.validate_linux_udp_receive(*wait.capture, thread.native_frame, *wait.binding, cause))
        return false;
    out.object = object; out.binding = *wait.binding;
    return true;
}

bool private_linux_udp_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                                    const sched::ThreadControlBlock &thread)
{
    PrivateLinuxUdpSignal owned;
    return private_linux_udp_signal_owned(kernel, process, thread, owned);
}

bool private_linux_udp_signal_select(Kernel &kernel, const sched::ProcessControlBlock &process,
                                     const sched::ThreadControlBlock &thread, PrivateLinuxUdpSignal &out)
{
    if (!private_linux_udp_signal_owned(kernel, process, thread, out)) return false;
    const auto ready = kernel.native_udp_executor().readiness(kernel.native_udp(), out.binding.endpoint);
    const auto *description = kernel.scheduler().open_description(out.binding.description);
    // Actual data and terminal results keep normal receive priority. Sequence
    // changes without remaining data do not make a stolen readiness wake final.
    if (!ready || ready.value().receive.readable() || ready.value().receive.terminal ||
        ready.value().executor.failed || !description || (description->flags & posix::o_NONBLOCK)) return false;
    auto queue = private_signal_pending(kernel, process, thread);
    const auto *actions = private_signal_table(kernel, process.pid());
    if (!queue || !actions) return false;
    auto blocked = thread.native_signal_mask;
    while (queue.peek(blocked, out.pending))
    {
        const auto *action = actions->find(out.pending.info.signal);
        if (!action) return false;
        if (PrivateSignalDelivery::Actions::ignored(out.pending.info.signal, *action))
        { blocked.set(out.pending.info.signal); continue; }
        if (!PrivateSignalDelivery::supported(*action)) return false;
        out.action = *action;
        return true;
    }
    return false;
}
} // namespace

bool private_linux_udp_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                    const sched::ThreadControlBlock &thread)
{
    PrivateLinuxUdpSignal selected;
    return private_linux_udp_signal_select(kernel, process, thread, selected);
}

bool private_linux_udp_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                       sched::ThreadControlBlock &thread)
{
    PrivateLinuxUdpSignal selected;
    if (!private_linux_udp_signal_select(kernel, process, thread, selected)) return false;
    if (selected.action.handler == sched::SignalAction::default_handler)
    {
        finish(kernel, process.pid(), 0, static_cast<u8>(selected.pending.info.signal));
        return false;
    }
    const auto resolved = linux_udp_receive_objects.resolve(selected.object);
    if (!resolved) { static_cast<void>(linux_udp_receive_invariant(resolved.status())); return false; }
    auto &wait = *resolved.value();
    const auto qualified = syscall_origins.linux_udp_receive_restart_qualified(
        *wait.capture, thread.native_frame, selected.binding);
    if (!qualified) { static_cast<void>(linux_udp_receive_invariant(qualified.status())); return false; }
    constexpr u64 sa_restart = 0x10000000;
    // Only a valid exact record with absent opcode qualification can use the
    // compatibility EINTR fallback. Stale authority never selects this branch.
    const bool restart = (selected.action.flags & sa_restart) && qualified.value();
    // Diagnostic scalars are never publication authority. A failed lookup
    // leaves zeros and cannot alter the original completion decision.
    u64 trace_capture{}, trace_epoch{}, trace_image{}, trace_stack_generation{};
    if constexpr (linux_udp_signal_diagnostics)
    {
        const auto identity = syscall_origins.linux_udp_receive_identity(
            *wait.capture, thread.native_frame, selected.binding);
        if (identity)
        {
            trace_capture = identity.value().capture;
            trace_epoch = identity.value().epoch;
            trace_image = identity.value().image;
            trace_stack_generation = identity.value().lease.generation;
        }
    }
    Status published{};
    if (restart)
    {
        const auto origin = syscall_origins.linux_udp_receive_identity(*wait.capture, thread.native_frame, selected.binding);
        if (!origin) { static_cast<void>(linux_udp_receive_invariant(origin.status())); return false; }
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
        using Publication = SyscallRestartPublication<private_signal_profile>;
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        auto *state = private_n64_state(process, thread);
        const auto identity = private_n64_identity(process, thread);
        const auto capability = private_n64_backend::capability();
#else
        auto *state = private_ppc_state(process, thread);
        const auto identity = private_ppc_identity(process, thread);
        const auto capability = private_ppc_backend::capability();
#endif
        published = Status::fault("Linux UDP restart publication lacks readiness");
        if (state)
        {
            auto staged = Publication::stage(syscall_origins, origin.value(), selected.binding, *state, identity,
                                             thread.native_frame, capability);
            if (staged && Publication::commit(staged.value(), origin.value(), selected.binding, identity,
                                              thread.native_frame, capability)) published = {};
        }
#else
        const auto permit = syscall_origins.prepare_restart(origin.value(), thread.native_frame, selected.binding);
        if (!permit) { static_cast<void>(linux_udp_receive_invariant(permit.status())); return false; }
        if (!syscall_origins.consume_restart(permit.value(), origin.value(), thread.native_frame, selected.binding))
            published = Status::fault("Linux UDP restart publication lost exact permit");
        else thread.native_frame = permit.value().plan().frame;
#endif
    }
    else
    {
        published = syscall_origins.consume_linux_udp_receive_interruption(
            *wait.capture, thread.native_frame, selected.binding);
        if (!published) { static_cast<void>(linux_udp_receive_invariant(published)); return false; }
        // EINTR is 4 on all ten target profiles, independent of opcode proof.
        published = set_linux_udp_result(process, thread.native_frame,
#ifdef OK_LINUX_UDP_SIGNAL_FAILURE_FIXTURE
        private_linux_udp_signal_model_result
#else
        -4
#endif
        );
    }
    if (!published) static_cast<void>(linux_udp_receive_invariant(published));
    if (thread.wait_registration.active() &&
        !kernel.scheduler().complete_wait(selected.binding.ticket, sched::WaitCompletion::interrupted))
    {
        const auto failed_ticket = Status::fault("Linux UDP signal lost its exact current ticket");
        if (published) published = failed_ticket;
        static_cast<void>(linux_udp_receive_invariant(failed_ticket));
    }
    const auto retired = retire_linux_udp_receive(kernel, selected.object, LinuxUdpReceiveRetirement::completed);
    thread.syscall_pending = false;
    if constexpr (linux_udp_signal_diagnostics)
    {
        // Emit only after all original success conditions, before user entry.
        // No wait/object/capture reference is read after exact retirement.
        if (published && retired && runtime_failure)
        {
            emit("OK_LINUX_UDP_SIGNAL pid="); emit_number(process.pid());
            emit(" tid="); emit_number(thread.tid);
            emit(" capture="); emit_number(trace_capture);
            emit(" epoch="); emit_number(trace_epoch);
            emit(" image="); emit_number(trace_image);
            emit(" stack_generation="); emit_number(trace_stack_generation);
            emit(" object_page="); emit_number(selected.object.page_slot);
            emit(" object_slot="); emit_number(selected.object.object_slot);
            emit(" object_generation="); emit_number(selected.object.generation);
            emit(" request="); emit_number(selected.binding.request_sequence);
            emit(" ticket="); emit_number(selected.binding.ticket.generation);
            emit(" signal="); emit_number(selected.pending.info.signal);
            emit(" qualified="); emit_number(qualified.value() ? 1 : 0);
            emit(restart ? " outcome=restart" : " outcome=eintr");
            emit(" pc="); emit_number(thread.native_frame.pc);
            emit(" retired=1 pending=0\n");
        }
    }
    return published && retired && runtime_failure;
}
#endif
} // namespace runtime_detail
#if defined(OK_NATIVE_SIGNAL_RUNTIME)
Status private_signal_before_process_compaction(sched::SchedulerState &scheduler)
{
    if (runtime_kernel && &runtime_kernel->scheduler() == &scheduler)
        if (auto status = prune_departed_linux_udp_receives(*runtime_kernel); !status) return status;
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (runtime_kernel && &runtime_kernel->scheduler() == &scheduler &&
        !private_ppc_stage_relocation(*runtime_kernel))
    {
        private_ppc_cancel_transactions();
        runtime_failure = Status::fault("private PPC process compaction stage failed");
        started = false;
        return runtime_failure;
    }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (runtime_kernel && &runtime_kernel->scheduler() == &scheduler &&
        !private_n64_stage_relocation(*runtime_kernel))
    {
        runtime_failure = Status::fault("private n64 process compaction preflight failed");
        return runtime_failure;
    }
#else
    static_cast<void>(scheduler);
#endif
    return Status::success();
}
Status private_signal_after_process_compaction(sched::SchedulerState &scheduler)
{
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (runtime_kernel && &runtime_kernel->scheduler() == &scheduler &&
        !private_ppc_commit_relocation(*runtime_kernel))
    {
        private_ppc_cancel_transactions();
        runtime_failure = Status::fault("private PPC process compaction commit failed");
        started = false;
        return runtime_failure;
    }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (runtime_kernel && &runtime_kernel->scheduler() == &scheduler &&
        !private_n64_commit_relocation(*runtime_kernel))
    {
        runtime_failure = Status::fault("private n64 process compaction commit failed");
        return runtime_failure;
    }
#else
    static_cast<void>(scheduler);
#endif
    return Status::success();
}
#endif
} // namespace ok::user::native
