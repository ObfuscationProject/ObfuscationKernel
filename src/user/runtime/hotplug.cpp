// User CPU hotplug and affinity (docs/modules/sysfs.md): the write route of
// /sys/devices/system/cpu/cpuN/online, sched_getaffinity, sched_setaffinity
// and getcpu, and the OK_SMP_HOTPLUG_USER, OK_SMP_HOTPLUG_RECORDS and
// OK_SMP_AFFINITY_BROKEN reports.
// Everything here runs under the big kernel lock (smp-scaling WP1).
#include "private.hpp"
#include "ok/smp/hotplug.hpp"
#include "ok/user/cpu_control.hpp"
#include "ok/user/native_pipe_vectors.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
namespace
{
namespace hotplug = smp::hotplug;
using Codec = lu::Codec<lu::configured_native_profile()>;
constexpr usize word_bytes = Codec::profile.word_bytes;
constexpr bool big_endian = Codec::profile.big_endian;
constexpr i64 errno_fault = 14, errno_search = 3, errno_permission = 1;

// The writing thread: running here, with the entry-stack lease that names its record.
sched::ThreadControlBlock *writer(Kernel &kernel, sched::ProcessId pid)
{
    auto *caller = kernel.scheduler().current_thread();
    if (!caller || caller->owner != pid || caller->state != sched::ProcessState::running || !caller->entry_stack.pool ||
        caller->entry_stack.slot >= hotplug::record_capacity)
        return nullptr;
    return caller;
}

// After the bytes were gathered: parse, register the wait first, then file
// the request. A no-op, a refusal or a parse error completes the call.
i64 file_request(Kernel &kernel, sched::ProcessId pid, sched::ThreadControlBlock &caller, u16 cpu,
                 std::span<const char> text, usize count, bool &complete)
{
    if (count == 0) return 0; // Linux sysfs_kf_write: nothing to store.
    const auto want = hotplug::parse_bool(text);
    if (!want) return -hotplug::errno_invalid;
    if (cpu == 0 || cpu >= smp::max_cpus) return -hotplug::errno_invalid;
    auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::cpu_hotplug, 0);
    if (!ticket) return error(ticket.status());
    const auto submitted = hotplug::submit(caller.entry_stack.slot,
                                           {.cpu = cpu, .want = *want, .pid = pid, .tid = caller.tid,
                                            .ticket = ticket.value(), .count = static_cast<i64>(count)});
    if (!submitted.armed)
    {
        // The owner completes its own ticket; the call returns now.
        static_cast<void>(kernel.scheduler().complete_wait(ticket.value(), sched::WaitCompletion::cancelled));
        return submitted.result;
    }
    // CPU 0 serves the request at its next loop pass.
    if (smp::current_cpu() != 0) smp::send_ipi(0, smp::ipi_kick | smp::ipi_reschedule);
    complete = false;
    return 0;
}

// The CPUs present (bring-up started them): the highest plus one is nr_cpu_ids.
u32 cpu_ids(u32 present)
{
    u32 count = 1;
    for (u32 cpu = 0; cpu < smp::max_cpus; ++cpu)
        if ((present & (u32{1} << cpu)) != 0) count = cpu + 1;
    return count;
}

// One native word of a cpu_set_t, in memory order.
std::array<u8, word_bytes> encode_mask(u32 mask)
{
    std::array<u8, word_bytes> bytes{};
    for (usize i = 0; i < 4 && i < word_bytes; ++i)
        bytes[big_endian ? word_bytes - 1 - i : i] = static_cast<u8>(mask >> (8 * i));
    return bytes;
}
u32 decode_mask(const std::array<u8, word_bytes> &bytes)
{
    u32 mask = 0;
    for (usize i = 0; i < 4 && i < word_bytes; ++i)
        mask |= static_cast<u32>(bytes[big_endian ? word_bytes - 1 - i : i]) << (8 * i);
    return mask;
}

// sched_*affinity name a thread by tid; 0 is the caller.
sched::ThreadControlBlock *target_thread(Kernel &kernel, sched::ProcessId pid, u64 target)
{
    if (target == 0) return kernel.scheduler().find_thread(pid, kernel.scheduler().current_tid(smp::current_cpu()));
    if (target > sched::max_task_id) return nullptr;
    return kernel.scheduler().find_thread_by_tid(static_cast<sched::ThreadId>(target));
}
} // namespace

std::optional<i64> cpu_control_resume(const sched::ThreadControlBlock &thread)
{
    if (!thread.entry_stack.pool || !hotplug::finished(thread.entry_stack.slot)) return std::nullopt;
    return hotplug::consume(thread.entry_stack.slot, thread.wait_registration.ticket());
}

void cpu_control_retire(const sched::ThreadControlBlock &thread)
{
    if (thread.entry_stack.pool) cpu_control_retire(thread.entry_stack, thread.owner, thread.tid);
}

void cpu_control_retire(memory::KernelEntryStackHandle lease, sched::ProcessId pid, sched::ThreadId tid)
{
    hotplug::abandon(lease.slot, pid, tid);
}

i64 sched_affinity_get(Kernel &kernel, sched::ProcessId pid, u64 target, u64 length, uptr output)
{
    const auto view = smp::cpu_view();
    if (length * 8 < cpu_ids(view.present) || length % word_bytes != 0) return -hotplug::errno_invalid;
    auto *thread = target_thread(kernel, pid, target);
    const auto *process = thread ? kernel.scheduler().find(thread->owner) : nullptr;
    if (!process) return -errno_search;
    const auto bytes = encode_mask(sched::SchedulerState::effective_affinity(*process, *thread) & view.online);
    auto &space = kernel.scheduler().find(pid)->resources().native_space;
    if (!hw::copy_to_user(space, output, bytes.data(), bytes.size())) return -errno_fault;
    return static_cast<i64>(word_bytes);
}

i64 sched_affinity_set(Kernel &kernel, sched::ProcessId pid, u64 target, u64 length, uptr input)
{
    auto *caller = kernel.scheduler().find(pid);
    if (!caller) return -errno_search;
    // A shorter mask is zero-filled, a longer one read up to the CPUs this kernel has.
    std::array<u8, word_bytes> bytes{};
    const usize copied = length < word_bytes ? static_cast<usize>(length) : word_bytes;
    if (copied && !hw::copy_from_user(caller->resources().native_space, bytes.data(), input, copied))
        return -errno_fault;
    const auto mask = static_cast<u16>(decode_mask(bytes) & smp::cpu_view().present);
    auto *thread = target_thread(kernel, pid, target);
    auto *process = thread ? kernel.scheduler().find(thread->owner) : nullptr;
    if (!process) return -errno_search;
    // Linux check_same_owner: root, or the caller's euid is the target's euid or uid.
    const auto mine = caller->credentials(), theirs = process->credentials();
    if (mine.euid != 0 && mine.euid != theirs.euid && mine.euid != theirs.uid) return -errno_permission;
    if (auto status = kernel.scheduler().set_thread_affinity(*process, *thread, mask); !status) return error(status);
    // A target running outside its new mask leaves its CPU now.
    const auto on = thread->on_cpu;
    if (on < smp::max_cpus && on != smp::current_cpu() &&
        (sched::SchedulerState::effective_affinity(*process, *thread) & (1u << on)) == 0)
        smp::send_ipi(on, smp::ipi_kick);
    return 0;
}

i64 cpu_number(Kernel &kernel, sched::ProcessId pid, uptr cpu, uptr node)
{
    auto &space = kernel.scheduler().find(pid)->resources().native_space;
    auto put = [&](uptr address, u32 value) {
        std::array<u8, 4> bytes{};
        for (usize i = 0; i < bytes.size(); ++i) bytes[big_endian ? 3 - i : i] = static_cast<u8>(value >> (8 * i));
        return !address || hw::copy_to_user(space, address, bytes.data(), bytes.size());
    };
    if (!put(cpu, smp::current_cpu()) || !put(node, 0)) return -errno_fault;
    return 0;
}

// CPU 0 prints the broken-affinity records the scheduler kept, never under a
// scheduler lock.
void report_affinity_broken(Kernel &kernel)
{
    if (smp::current_cpu() != 0) return;
    while (const auto broken = kernel.scheduler().take_affinity_broken())
    {
        emit_line("OK_SMP_AFFINITY_BROKEN pid="); emit_number(broken->pid);
        emit(" tid="); emit_number(broken->tid);
        emit(" cpu="); emit_number(broken->cpu);
        emit("\n");
    }
}

// With secondary CPUs started: the user hotplug requests and their outcomes.
void report_cpu_hotplug()
{
    if (smp::started_cpus() < 2) return;
    const auto statistics = hotplug::statistics();
    auto field = [](std::string_view name, u64 value) { emit(" "); emit(name); emit("="); emit_number(value); };
    emit_line("OK_SMP_HOTPLUG_USER scope=hardware");
    field("requests", statistics.requests);
    field("offline", statistics.offline);
    field("online", statistics.online);
    field("noop", statistics.noop);
    field("failures", statistics.failures);
    field("busy", statistics.busy);
    field("orphaned", statistics.orphaned);
    field("wedged", statistics.wedged);
    field("affinity_broken", runtime_kernel ? runtime_kernel->scheduler().affinity_broken() : 0);
    field("max_us", statistics.max_us);
    emit("\n");
    // What became of requests whose writer left: abandoned while the service
    // ran them, or dropped at teardown or exec while still waiting for the
    // service or holding an unconsumed result.
    emit_line("OK_SMP_HOTPLUG_RECORDS scope=hardware");
    field("abandoned", statistics.abandoned);
    field("dropped_waiting", statistics.dropped_waiting);
    field("dropped_done", statistics.dropped_done);
    emit("\n");
}
} // namespace runtime_detail

i64 cpu_control_write(Kernel &kernel, sched::ProcessId pid, u16 cpu, uptr buffer, usize count, bool &complete)
{
    auto *caller = writer(kernel, pid);
    if (!caller) return -hotplug::errno_invalid;
    if (count > hotplug::max_write) return -hotplug::errno_too_big;
    std::array<char, hotplug::parsed_bytes> text{};
    const usize copied = count < text.size() ? count : text.size();
    auto *process = kernel.scheduler().find(pid);
    if (copied && !hw::copy_from_user(process->resources().native_space, text.data(), buffer, copied))
        return -errno_fault;
    return file_request(kernel, pid, *caller, cpu, {text.data(), copied}, count, complete);
}

i64 cpu_control_writev(Kernel &kernel, sched::ProcessId pid, u16 cpu, uptr vectors, usize vector_count,
                       bool &complete)
{
    auto *caller = writer(kernel, pid);
    if (!caller) return -hotplug::errno_invalid;
    std::array<PipeVector, PipeVectors::capacity> vector{};
    if (vector_count > vector.size()) return -hotplug::errno_invalid;
    auto &space = kernel.scheduler().find(pid)->resources().native_space;
    if (vector_count && !hw::copy_from_user(space, vector.data(), vectors, vector_count * sizeof(PipeVector)))
        return -errno_fault;
    usize count = 0;
    for (usize i = 0; i < vector_count; ++i)
    {
        if (vector[i].length > static_cast<usize>(__INTPTR_MAX__) - count) return -hotplug::errno_invalid;
        count += vector[i].length;
    }
    if (count > hotplug::max_write) return -hotplug::errno_too_big;
    std::array<char, hotplug::parsed_bytes> text{};
    usize copied = 0;
    for (usize i = 0; i < vector_count && copied < text.size(); ++i)
    {
        const usize part = vector[i].length < text.size() - copied ? vector[i].length : text.size() - copied;
        if (part && !hw::copy_from_user(space, text.data() + copied, vector[i].base, part)) return -errno_fault;
        copied += part;
    }
    return file_request(kernel, pid, *caller, cpu, {text.data(), copied}, count, complete);
}
} // namespace ok::user::native
