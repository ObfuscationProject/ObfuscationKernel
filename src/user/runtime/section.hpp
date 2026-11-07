#pragma once
// Kernel sections of the Linux personality (ok/smp/kernel_section.hpp): a
// converted syscall is admitted under the kernel lock, runs its body with
// the lock released and has its result committed under the lock again, by
// its own CPU or by a CPU that drains the process first.
//
// Admission (admit_section) declines with no effect unless the call is in
// a converted family, nothing else claims it and its descriptor and user
// range qualify; the body then needs only the caller's footprint and fine
// locks. Converted families: getpid, gettid, clock_gettime, clock_getres
// (C1) and read, write, pread64, pwrite64, lseek, fsync, fdatasync on ext4
// regular-file nodes (C2). The bodies are the kernel-lock path's own
// helpers (ok/user/linux_file_io.hpp).
#include "private.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/user/linux_file_io.hpp"

#include <optional>

namespace ok::user::native
{
inline namespace runtime_detail
{
enum class Admission : u8
{
    run,
    decline,
};

// The bytes a converted body may read or write per call.
inline constexpr usize section_io_limit = 64 * 1024;
// Every this many admitted getpid/gettid calls one runs under the kernel lock
// instead, with the section body's result kept for comparison (A8).
inline constexpr u64 section_shadow_period = 8;

// One admitted call: captured under the kernel lock before the pin.
template <lu::Profile Profile> struct SectionCall
{
    smp::SectionFamily family{};
    sched::ProcessControlBlock *process{};
    sched::ThreadControlBlock *thread{};
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    lu::Call call{};
    bool time64{};
};

// What a section body reaches: the bound caller's PosixService, its
// descriptors and a by-value snapshot of its address space.
class SectionAccess final
{
  public:
    SectionAccess(posix::PosixService &posix, const sched::SchedulerState &scheduler,
                  const sched::ProcessResources &resources)
        : posix_(posix), scheduler_(scheduler), files_(resources.files), space_(resources.native_space)
    {
    }
    [[nodiscard]] posix::PosixService &posix() { return posix_; }
    [[nodiscard]] const hw::AddressSpace &space() const { return space_; }
    [[nodiscard]] const sched::OpenFileDescription *description(i32 fd) const
    {
        const auto *slot = files_.entry(fd);
        return slot && slot->used ? scheduler_.open_description(slot->description) : nullptr;
    }

  private:
    posix::PosixService &posix_;
    const sched::SchedulerState &scheduler_;
    const sched::FileDescriptorTable &files_;
    hw::AddressSpace space_;
};
static_assert(linux_file_io::Access<SectionAccess>);

struct SectionSelection
{
    smp::SectionFamily family{};
    usize arguments{};
    bool time64{};
};

// The profile's converted Linux numbers. lseek has no _llseek form here:
// sections run on 64-bit profiles, and _llseek stays on the slow path.
template <lu::Profile Profile> std::optional<SectionSelection> section_family(u64 number)
{
    using Codec = lu::Codec<Profile>;
    using O = lu::Operation;
    using F = smp::SectionFamily;
    const auto is = [&](O operation) {
        const auto expected = Codec::syscall_number(operation);
        return expected && number == *expected;
    };
    constexpr usize positioned = Codec::profile.word_bytes == 8 ? 4 : (Codec::profile.aligned_64bit_pairs ? 6 : 5);
    if (is(O::getpid)) return SectionSelection{F::getpid, 0};
    if (is(O::gettid)) return SectionSelection{F::gettid, 0};
    if (is(O::clock_gettime) || is(O::clock_gettime32)) return SectionSelection{F::clock_gettime, 2};
    if (is(O::clock_gettime64)) return SectionSelection{F::clock_gettime, 2, true};
    if (is(O::clock_getres) || is(O::clock_getres_time32)) return SectionSelection{F::clock_getres, 2};
    if (is(O::clock_getres_time64)) return SectionSelection{F::clock_getres, 2, true};
    if (is(O::read)) return SectionSelection{F::read, 3};
    if (is(O::write)) return SectionSelection{F::write, 3};
    if (is(O::pread64)) return SectionSelection{F::pread64, positioned};
    if (is(O::pwrite64)) return SectionSelection{F::pwrite64, positioned};
    if (is(O::lseek)) return SectionSelection{F::lseek, 3};
    if (is(O::fsync)) return SectionSelection{F::fsync, 1};
    if (is(O::fdatasync)) return SectionSelection{F::fdatasync, 1};
    return std::nullopt;
}

// A5: what a decline must leave as it found it. A description's offset is
// compared only when no other CPU's section was working at the first digest:
// such a section may share the description (fork) and move it under its
// position lock, and no section can start while admission holds the kernel
// lock.
struct AdmissionDigest
{
    u64 frame{};
    bool syscall_pending{};
    sched::ProcessState state{};
    bool slot_used{}, close_on_exec{};
    u16 description{};
    bool offset_stable{};
    usize offset{}, references{};
    u32 flags{};
    u64 entries{};
    usize origins_live{}, origins_bound{}, continuations_pending{};
    usize futex_live{}, processes{};
    usize pool_user{}, pool_objects{}, pool_free{};
    [[nodiscard]] bool equal(const AdmissionDigest &other, bool pages) const;
};
AdmissionDigest admission_digest(Kernel &kernel, const sched::ProcessControlBlock &process,
                                 const sched::ThreadControlBlock &thread);
// Whether a section on a CPU other than this one is working.
[[nodiscard]] bool section_working_elsewhere();

// Counts one decline (and, with OK_LOCK_DEBUG, any effect it had).
Admission decline_section(smp::SectionDecline reason, const std::optional<AdmissionDigest> &before, Kernel &kernel,
                          const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread);
// A8: this CPU computed `value` for (pid, tid) and let the kernel-lock path
// run the call; shadow_compare checks that path's result.
void shadow_expect(smp::CpuId cpu, sched::ProcessId pid, sched::ThreadId tid, i64 value);
void shadow_compare(smp::CpuId cpu, sched::ProcessId pid, sched::ThreadId tid, i64 result);
// Whether this admitted identity call is the period's shadow.
bool shadow_turn(smp::CpuId cpu);
// The commit handler (installed by start_sections) and admission start.
void commit_section(smp::SectionRecord &record) noexcept;
void start_sections();

// The body: the kernel-lock path's helpers, with the caller bound.
template <lu::Profile Profile> i64 section_work(SectionAccess &access, const SectionCall<Profile> &call)
{
    using F = smp::SectionFamily;
    const auto &a = call.call.arguments;
    const auto fd = static_cast<posix::Fd>(static_cast<u32>(a[0]));
    switch (call.family)
    {
    case F::getpid: return static_cast<i64>(access.posix().getpid());
    case F::gettid: return static_cast<i64>(access.posix().gettid());
    case F::clock_gettime:
    case F::clock_getres:
        return linux_file_io::linux_clock_record<Profile>(access.space(), call.call, call.family == F::clock_getres,
                                                          call.time64);
    case F::read:
    case F::write: {
        bool would_block{};
        return linux_file_io::regular_rw(access, call.family == F::write, static_cast<posix::Fd>(a[0]),
                                         static_cast<uptr>(a[1]), static_cast<usize>(a[2]), would_block);
    }
    case F::pread64:
    case F::pwrite64: return linux_file_io::linux_positioned_io<Profile>(access, call.family == F::pwrite64, call.call);
    case F::lseek: return linux_file_io::linux_lseek<Profile>(access, false, call.call);
    case F::fsync:
    case F::fdatasync: return linux_file_io::file_sync(access, fd);
    case F::count: break;
    }
    return -38;
}

// Holding the kernel lock (and nothing else): decides whether `thread`'s
// pending Linux syscall runs as a section. A decline changes nothing; the
// kernel-lock path then runs the call.
template <lu::Profile Profile>
Admission admit_section(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                        smp::CpuId cpu, SectionCall<Profile> &out)
{
    using Bridge = lu::NativeBridge<Profile>;
    using Codec = lu::Codec<Profile>;
    using F = smp::SectionFamily;
    using D = smp::SectionDecline;
    std::optional<AdmissionDigest> before;
    if constexpr (smp::lock_debug_enabled)
        before = admission_digest(kernel, process, thread);
    const auto decline = [&](D reason) { return decline_section(reason, before, kernel, process, thread); };
    // 1. Admission is open and the runtime healthy.
    if (!smp::section_admission_open() || shutdown_admission_closed || native_shutdown_pending())
        return decline(D::shutdown);
    if (!started || !runtime_failure.ok()) return decline(D::runtime);
    // 2. A Linux caller and a converted number.
    if (process.resources().personality != Personality::linux) return decline(D::personality);
    const u64 number = Bridge::snapshot(thread.native_frame).integer[Codec::profile.number_register];
    const auto selected = section_family<Profile>(number);
    if (!selected) return decline(D::number);
    // 3. Nothing claims the call ahead of the Linux dispatcher, and the
    // caller is the running thread the dispatcher would serve.
    const auto pid = process.pid();
    const auto tid = thread.tid;
    if (thread.owner != pid || thread.state != sched::ProcessState::running || !thread.native_active ||
        process.threads().empty() || pty_wait_object(pid, tid).pool || linux_udp_receive_object(pid, tid).pool ||
        native_udp_receive_object(pid, tid).pool || child_wait_object(pid, tid).pool ||
        pipe_read_object(pid, tid).pool || pipe_write_object(pid, tid).pool ||
        vfs_continuations.pending(pid, tid) != decltype(vfs_continuations.pending(pid, tid)){} ||
        syscall_origins.has_tcp_immediate(thread) ||
        linux_unix_path_call<Profile>(thread.native_frame, kernel.scheduler(), process) != lu::UnixPathCall::none)
        return decline(D::pending);
    if (thread.console_wait_epoch) return decline(D::console);
    const auto &space = process.resources().native_space;
    auto decoded = Bridge::decode_call_copying_stack(thread.native_frame, selected->arguments,
        [&](uptr address, std::span<u8> destination) {
            return hw::copy_from_user(space, destination.data(), address, destination.size());
        });
    if (!decoded) return decline(D::fault);
    const auto &a = decoded.value().arguments;
    const auto family = selected->family;
    // 4. A regular ext4 file the call may use, with every pre-work errno case
    // (descriptor, seek origin, offset, size) left to the kernel-lock path.
    const bool file = family != F::getpid && family != F::gettid && family != F::clock_gettime &&
                      family != F::clock_getres;
    if (file)
    {
        const auto fd = static_cast<i32>(static_cast<u32>(a[0]));
        const auto *slot = process.fd_table().entry(fd);
        const auto *description = slot && slot->used ? kernel.scheduler().open_description(slot->description) : nullptr;
        if (!description || !description->used) return decline(D::descriptor);
        if (!description->node || !description->node->concurrent_io() ||
            description->pipe != sched::invalid_description || description->unix_write_pipe != sched::invalid_description ||
            description->pty_endpoint || description->console || description->input_console ||
            description->native_shared_file || description->native_udp_handle || description->native_tcp_handle ||
            description->unix_endpoint)
            return decline(D::kind);
        const bool reads = family == F::read || family == F::pread64;
        const bool writes = family == F::write || family == F::pwrite64;
        if ((reads && !description->readable) || (writes && !description->writable)) return decline(D::descriptor);
        if (writes && (description->flags & posix::o_APPEND)) return decline(D::append);
        if ((reads || writes) && a[2] > section_io_limit) return decline(D::size);
        if (family == F::lseek && static_cast<u32>(a[2]) > 2) return decline(D::size);
        if (family == F::pread64 || family == F::pwrite64)
        {
            const auto offset = Codec::decode_argument64(decoded.value(), Codec::profile.aligned_64bit_pairs ? 4 : 3);
            if (!offset || static_cast<i64>(offset.value()) < 0 || a[2] > (Codec::word_mask >> 1) ||
                offset.value() > (Codec::word_mask >> 1) - section_io_limit)
                return decline(D::size);
        }
    }
    // 5. The user range, last: resolving copy-on-write for an output here,
    // under the kernel lock, keeps the resolver out of the body.
    if (family == F::read || family == F::pread64 || family == F::write || family == F::pwrite64 ||
        family == F::clock_gettime || family == F::clock_getres)
    {
        const bool output = family != F::write && family != F::pwrite64;
        usize length = family == F::clock_gettime || family == F::clock_getres
            ? Codec::timespec_size(selected->time64 ? lu::TimeFormat::kernel_timespec : lu::TimeFormat::legacy_timespec)
            : static_cast<usize>(a[2]);
        if (family != F::read && length > sched::pipe_capacity) length = sched::pipe_capacity;
        if (!hw::user_range(space, static_cast<uptr>(a[1]), length, output)) return decline(D::fault);
    }
    if (family == F::getpid || family == F::gettid)
        if (shadow_turn(cpu))
        {
            // A8: compute the body's result here and let the kernel-lock path
            // answer; step() compares the two.
            SectionAccess access{kernel.posix(), kernel.scheduler(), process.resources()};
            kernel.posix().bind_section_caller(cpu, process, thread);
            SectionCall<Profile> shadow{family, &process, &thread, pid, tid, decoded.value(), false};
            const auto value = section_work<Profile>(access, shadow);
            kernel.posix().unbind_section_caller(cpu);
            shadow_expect(cpu, pid, tid, value);
            return decline(D::shadow);
        }
    out = {family, &process, &thread, pid, tid, decoded.value(), selected->time64};
    return Admission::run;
}

// Runs an admitted call: pins it, releases the kernel lock, runs the body and
// returns holding the lock again with the result committed (here, or by a
// CPU that drained this process meanwhile). Nothing after it may use the
// call's process or thread pointers.
template <lu::Profile Profile> void run_section(Kernel &kernel, SectionCall<Profile> &call, smp::CpuId cpu)
{
    auto &posix = kernel.posix();
    SectionAccess access{posix, kernel.scheduler(), call.process->resources()};
    posix.bind_section_caller(cpu, *call.process, *call.thread);
    smp::count_section_family(call.family);
    smp::enter_section(call.pid, call.tid);
    const auto result = section_work<Profile>(access, call);
    posix.unbind_section_caller(cpu);
    // Waiting for the lock, serve this CPU's device lines: a lock holder may
    // wait for one routed here.
    smp::leave_section(result, [] { interrupt::lock_kernel_serving_interrupts(); });
}
} // namespace runtime_detail
} // namespace ok::user::native
