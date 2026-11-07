// Kernel sections: commit, decline accounting, the A5 decline digest and the
// A8 shadow comparison (src/user/runtime/section.hpp).
#include "section.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
namespace
{
// A8, per CPU: the admitted identity calls counted towards the next shadow,
// and the shadowed call's expected result. Only the owning CPU touches its
// entry, under the kernel lock.
struct ShadowCpu
{
    u64 admitted{};
    bool armed{};
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    i64 expected{};
};
constinit std::array<ShadowCpu, smp::max_cpus> shadows{};

u64 frame_hash(const hw::UserFrame &frame)
{
    // FNV-1a over the saved user frame.
    u64 hash = 1469598103934665603ULL;
    const auto *bytes = reinterpret_cast<const unsigned char *>(&frame);
    for (usize index = 0; index < sizeof(frame); ++index)
        hash = (hash ^ bytes[index]) * 1099511628211ULL;
    return hash;
}
} // namespace

bool AdmissionDigest::equal(const AdmissionDigest &other, bool pages) const
{
    const bool same = frame == other.frame && syscall_pending == other.syscall_pending && state == other.state &&
        slot_used == other.slot_used && close_on_exec == other.close_on_exec && description == other.description &&
        (!offset_stable || !other.offset_stable || offset == other.offset) &&
        references == other.references && flags == other.flags &&
        entries == other.entries && origins_live == other.origins_live && origins_bound == other.origins_bound &&
        continuations_pending == other.continuations_pending && futex_live == other.futex_live &&
        processes == other.processes;
    return same && (!pages || (pool_user == other.pool_user && pool_objects == other.pool_objects &&
                               pool_free == other.pool_free));
}

bool section_working_elsewhere()
{
    const auto self = smp::current_cpu();
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        if (cpu != self && smp::section_records[cpu].load() == smp::SectionState::working)
            return true;
    return false;
}

AdmissionDigest admission_digest(Kernel &kernel, const sched::ProcessControlBlock &process,
                                 const sched::ThreadControlBlock &thread)
{
    AdmissionDigest digest{};
    digest.frame = frame_hash(thread.native_frame);
    digest.syscall_pending = thread.syscall_pending;
    digest.state = thread.state;
    // The descriptor a file call would name (the first argument register).
    const auto fd = static_cast<i32>(static_cast<u32>(hw::syscall(thread.native_frame).arguments[0]));
    if (const auto *slot = process.fd_table().entry(fd))
    {
        digest.slot_used = slot->used;
        digest.close_on_exec = slot->close_on_exec;
        digest.description = slot->description;
        if (const auto *description = slot->used ? kernel.scheduler().open_description(slot->description) : nullptr)
        {
            // Read only when no section can be moving it (no unlocked read).
            digest.offset_stable = !section_working_elsewhere();
            if (digest.offset_stable)
                digest.offset = description->offset;
            digest.references = description->references;
            digest.flags = description->flags;
        }
    }
    digest.entries = entry_syscalls + entry_timers + entry_interrupts + entry_faults + entry_rejections;
    // A drain during admission (the copy-on-write resolver's, for an output
    // range) commits the process's pinned sections, and each commit revokes
    // its thread's origin record. A pinned section's thread record changes
    // only at that commit (sections never touch origins), so it is left out
    // here and in the digest compared with this one.
    auto origins = syscall_origins.ownership();
    for (const auto &record : smp::section_records)
    {
        const auto pinned = record.load();
        if (pinned != smp::SectionState::working && pinned != smp::SectionState::returning) continue;
        if (const auto *owner = kernel.scheduler().find_thread(record.pid, record.tid))
        {
            const auto share = syscall_origins.thread_ownership(*owner);
            origins.live -= share.live;
            origins.bound -= share.bound;
        }
    }
    digest.origins_live = origins.live;
    digest.origins_bound = origins.bound;
    digest.continuations_pending = vfs_continuations.ownership().pending;
    digest.futex_live = futex_objects.accounting().live_objects;
    digest.processes = kernel.scheduler().processes().size();
    digest.pool_user = page_pool.user_count();
    digest.pool_objects = page_pool.object_count();
    digest.pool_free = page_pool.free_count();
    return digest;
}

Admission decline_section(smp::SectionDecline reason, const std::optional<AdmissionDigest> &before, Kernel &kernel,
                          const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread)
{
    smp::count_section_decline(reason);
    // A user-range check may resolve copy-on-write before it fails: a
    // fault decline may change page counts, and nothing else.
    if (before && !admission_digest(kernel, process, thread).equal(*before, reason != smp::SectionDecline::fault))
        smp::detail::section_assert(&smp::SectionCpu::purity_faults, "section decline changed state");
    return Admission::decline;
}

bool shadow_turn(smp::CpuId cpu)
{
    if (cpu >= shadows.size()) return false;
    // The first of every period, so a CPU's first identity call is checked.
    return ++shadows[cpu].admitted % section_shadow_period == 1;
}

void shadow_expect(smp::CpuId cpu, sched::ProcessId pid, sched::ThreadId tid, i64 value)
{
    if (cpu < shadows.size()) shadows[cpu] = {shadows[cpu].admitted, true, pid, tid, value};
}

void shadow_compare(smp::CpuId cpu, sched::ProcessId pid, sched::ThreadId tid, i64 result)
{
    if (cpu >= shadows.size() || !shadows[cpu].armed) return;
    auto &shadow = shadows[cpu];
    shadow.armed = false;
    if (shadow.pid != pid || shadow.tid != tid) return;
    smp::detail::section_count(&smp::SectionCpu::shadow_checked);
    if (result != shadow.expected)
        smp::detail::section_assert(&smp::SectionCpu::shadow_mismatched, "section shadow result differs");
}

void commit_section(smp::SectionRecord &record) noexcept
{
    // Under the kernel lock, on the section's CPU or on a drainer. The record
    // is already committed; its process and thread are found again by id.
    if (runtime_kernel == nullptr) return;
    auto &kernel = *runtime_kernel;
    const auto self = smp::current_cpu();
    // A7: the owner's thread is still in its CPU's slot (any mutator that
    // could have vacated it drained, and so committed, first).
    if (self < smp::max_cpus && &record == &smp::section_records[self] &&
        (kernel.scheduler().current_pid(self) != record.pid || kernel.scheduler().current_tid(self) != record.tid))
        smp::detail::section_assert(&smp::SectionCpu::section_asserts, "section owner lost its CPU slot");
    auto *process = kernel.scheduler().find(record.pid);
    auto *thread = kernel.scheduler().find_thread(record.pid, record.tid);
    if (!process || !thread || !thread->native_active || !thread->syscall_pending) return;
    thread->syscall_pending = false;
    thread->console_wait_epoch = 0;
    if (auto status = set_result(*process, thread->native_frame, record.result); !status.ok())
    {
        runtime_failure = status;
        finish(kernel, record.pid, 127);
    }
}

void start_sections()
{
    smp::section_commit_handler = [](smp::SectionRecord &record) noexcept { commit_section(record); };
    if constexpr (smp_sections)
    {
        // A section that waits for the ext4 gate or polls a device serves its
        // CPU's lines: a kernel-lock holder may wait for one routed here.
        smp::spin_service = interrupt::serve_local_interrupts;
        smp::open_section_admission();
    }
}

void drain_if_pinned(sched::ProcessId pid)
{
    for (const auto &record : smp::section_records)
    {
        const auto state = record.load();
        if ((state == smp::SectionState::working || state == smp::SectionState::returning) && record.pid == pid)
        {
            smp::drain(pid);
            return;
        }
    }
}
} // namespace runtime_detail
} // namespace ok::user::native
