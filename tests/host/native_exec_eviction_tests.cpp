// Exec and exit_group with a sibling thread that another CPU runs, through
// the actual runtime dispatcher, loader, entry stack pool and scheduler
// (spec 4.8). The other CPU is a host thread: it holds the sibling in user
// mode (its scheduler slot and active lease, as the step leaves them), and
// once the group is frozen it returns the way the step does: the post-user
// relock takes the kernel lock that the quiescing caller released, the
// lease is deactivated, the frozen check skips dispatch and put_prev takes
// the thread off its CPU. The caller holds the kernel lock as the step does.
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_NATIVE_VFS_CONTINUATION_MODEL 1
#include <atomic>
#include <thread>
#include "native_clock_model.hpp"
namespace ok::user::native
{
struct SyscallOriginTestAccess
{
    static u16 &borrows(SyscallOrigins &table, memory::KernelEntryStackHandle lease)
    { return table.records_[lease.slot].linux_udp_borrows; }
    static smp::PerCpuSequence &images(SyscallOrigins &table) { return table.image_sequence_; }
};
struct VfsStatContinuationTestAccess
{
    template <typename Table> static smp::PerCpuSequence &images(Table &table) { return table.images_; }
};
} // namespace ok::user::native
namespace console
{
std::array<char, 4096> text{};
usize size{};
std::string_view view() { return {text.data(), size}; }
} // namespace console
extern "C" void ok_platform_console_write_char(char value)
{
    if (console::size < console::text.size()) console::text[console::size++] = value;
}
namespace
{
usize checks{};
#define EXPECT(x) do { ++checks; CHECK(x); } while (false)
constexpr smp::CpuId other_cpu = 1;
constexpr uptr path_address = hw::user_begin + 512;

// A two-segment static native ELF for the configured architecture.
std::array<std::byte, 4112> elf{};
void put(usize offset, u64 value, usize bytes)
{
    for (usize i = 0; i < bytes; ++i) elf[offset + i] = static_cast<std::byte>(value >> (8 * i));
}
void make_elf()
{
    const u16 machine = []
    {
        switch (arch::configured_arch_operations().architecture())
        {
        case arch::Architecture::x86_64: return u16{0x3e};
        case arch::Architecture::i386: return u16{0x03};
        case arch::Architecture::aarch64: return u16{0xb7};
        case arch::Architecture::arm32: return u16{0x28};
        case arch::Architecture::rv64: case arch::Architecture::rv32: return u16{0xf3};
        case arch::Architecture::mips: case arch::Architecture::mips64: return u16{0x08};
        case arch::Architecture::ppc: return u16{0x14};
        case arch::Architecture::loongarch64: return u16{0x102};
        }
        return u16{0};
    }();
    const uptr base = hw::user_begin;
    for (usize i = 0; i < 4; ++i) elf[i] = static_cast<std::byte>("\x7f" "ELF"[i]);
    put(4, 2, 1); put(5, 1, 1); put(6, 1, 1);
    put(16, 2, 2); put(18, machine, 2); put(20, 1, 4); put(24, base + 0x100, 8); put(32, 64, 8);
    put(52, 64, 2); put(54, 56, 2); put(56, 2, 2);
    const std::array<std::array<u64, 7>, 2> segments{{{5, 0, base, 512, 4096, 4096, 0},
                                                       {6, 4096, base + 0x1000, 16, 4096, 4096, 0}}};
    for (usize i = 0; i < segments.size(); ++i)
    {
        const usize at = 64 + i * 56;
        put(at, 1, 4); put(at + 4, segments[i][0], 4); put(at + 8, segments[i][1], 8);
        put(at + 16, segments[i][2], 8); put(at + 24, segments[i][2], 8); put(at + 32, segments[i][3], 8);
        put(at + 40, segments[i][4], 8); put(at + 48, segments[i][5], 8);
    }
    elf[256] = std::byte{0xc3};
}

struct Group
{
    sched::ProcessId pid{};
    sched::ThreadId sibling{};
};
sched::ProcessControlBlock &process(Kernel &kernel, sched::ProcessId pid) { return *kernel.scheduler().find(pid); }
sched::ThreadControlBlock &thread(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid)
{ return *kernel.scheduler().find_thread(pid, tid); }

Group create(Kernel &kernel)
{
    native::Arguments arguments; CHECK(arguments.push_argv("image"));
    auto image = native::load(kernel, "/image", arguments); CHECK(image);
    auto made = kernel.scheduler().create_user_process("exec-eviction",
        kernel.arch().make_user_context({image.value().frame.pc, image.value().frame.sp})); CHECK(made);
    auto &p = process(kernel, made.value());
    CHECK(native::prepare_native_thread(p.leader())); CHECK(native::install(p, image.value()));
    auto sibling = kernel.scheduler().create_thread_prepared(p.pid(), p.leader().context, [](auto &created)
        { created.native_active = true; return native::prepare_native_thread(created); });
    CHECK(sibling);
    CHECK(kernel.scheduler().set_runnable(p.pid()));
    return {p.pid(), sibling.value()};
}

// What CPU `other_cpu` holds while it runs `tid` in user mode.
void run_elsewhere(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid)
{
    auto &running = thread(kernel, pid, tid);
    EXPECT(kernel.scheduler().mark_on_cpu(pid, tid, other_cpu));
    EXPECT(native::entry_stacks.activate_entry(running.entry_stack, {pid, tid}, other_cpu));
}

// CPU `other_cpu` on its own host thread: it stays in user mode until a
// group transition froze the group (the claimer's kick) or the test lets
// it return, then runs the step's return path under the kernel lock. A
// `syscall` trap stays pending when the group is frozen; `before_return`
// runs first under the kernel lock (another CPU's kill).
class OtherCpu
{
  public:
    OtherCpu(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid, bool syscall = false,
             void (*before_return)(Kernel &, sched::ProcessId) = nullptr, bool keep_lease = false)
        : kernel_(kernel), pid_(pid), tid_(tid), syscall_(syscall), before_(before_return), keep_lease_(keep_lease),
          cpu_([this] { run(); })
    {
    }
    ~OtherCpu() { join(); }
    void release() { release_.store(true, std::memory_order_release); }
    void join()
    {
        release();
        if (cpu_.joinable()) cpu_.join();
    }
    [[nodiscard]] bool frozen_seen() const { return frozen_.load(std::memory_order_acquire); }

  private:
    void run()
    {
        const smp::ScopedHostedCpu hosted{other_cpu};
        while (!release_.load(std::memory_order_acquire) && !kernel_.scheduler().frozen(pid_))
            std::this_thread::yield();
        smp::bkl_lock(); // The post-user relock (enter_unlocked).
        if (before_) before_(kernel_, pid_);
        auto &running = thread(kernel_, pid_, tid_);
        if (!keep_lease_) CHECK(native::entry_stacks.deactivate_entry(running.entry_stack, {pid_, tid_}, other_cpu));
        const bool frozen = smp::bkl_thread_frozen(pid_);
        if (frozen && syscall_) running.syscall_pending = true;
        frozen_.store(frozen, std::memory_order_release);
        kernel_.scheduler().put_prev(other_cpu);
        smp::bkl_unlock();
    }
    Kernel &kernel_;
    sched::ProcessId pid_;
    sched::ThreadId tid_;
    bool syscall_;
    void (*before_)(Kernel &, sched::ProcessId);
    bool keep_lease_;
    std::atomic<bool> release_{false};
    std::atomic<bool> frozen_{false};
    std::thread cpu_;
};

// The caller's step: under the kernel lock, as the step dispatches.
i64 call(Kernel &kernel, sched::ProcessId pid, sched::ThreadId caller, u64 number, uptr argument, bool &complete)
{
    EXPECT(kernel.scheduler().activate_thread(pid, caller)); EXPECT(kernel.posix().activate(pid));
    complete = true;
    console::size = 0;
    smp::bkl_lock();
    const auto result = native::dispatch(kernel, pid, {number, {argument, 0, 0, 0, 0, 0}}, complete);
    EXPECT(smp::kernel_lock.owned_by_current_cpu());
    smp::bkl_unlock();
    return result;
}
i64 exec(Kernel &kernel, sched::ProcessId pid, sched::ThreadId caller, bool &complete)
{
    return call(kernel, pid, caller, OK_SYS_EXECVE, path_address, complete);
}
// The old eviction report is gone: a quiesced exec takes no entry over.
bool nothing_reported() { return console::view().find("OK_NATIVE_EXEC_EVICT") == std::string_view::npos; }
// A quiesced sibling is off its CPU with its frame, lease and pending call,
// and is queued again once the group thawed.
void thawed(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid, uptr pc, bool pending)
{
    const auto &sibling = thread(kernel, pid, tid);
    EXPECT(!kernel.scheduler().frozen(pid) && kernel.scheduler().transition(pid) == sched::GroupTransition::none);
    EXPECT(sibling.on_cpu == sched::no_cpu && sibling.queued_cpu != sched::no_cpu);
    EXPECT(sibling.native_frame.pc == pc && sibling.syscall_pending == pending);
    EXPECT(native::entry_stacks.validate_inactive(sibling.entry_stack, {pid, tid}));
    EXPECT(!native::entry_stacks.active_on(other_cpu) && kernel.scheduler().current_tid(other_cpu) == 0);
}

struct Snapshot
{
    hw::AddressSpace space{};
    usize threads{};
    memory::KernelEntryStackHandle lease{};
    uptr pc{}, top{};
    smp::CpuId on_cpu{};
    memory::KernelEntryStackAccounting stacks{};
};
Snapshot snapshot(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid)
{
    const auto &running = thread(kernel, pid, tid);
    return {process(kernel, pid).resources().native_space, process(kernel, pid).record_count(), running.entry_stack,
            running.native_frame.pc, running.kernel_stack_top, running.on_cpu, native::entry_stacks.accounting()};
}
// Nothing of the sibling or of the group changed but its placement: the
// exec quiesced it and thawed the group again.
void untouched(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid, const Snapshot &before, bool running = true)
{
    const auto after = snapshot(kernel, pid, tid);
    EXPECT(native::same_space(after.space, before.space) && after.threads == before.threads);
    EXPECT(after.lease == before.lease && after.pc == before.pc && after.top == before.top);
    EXPECT(before.on_cpu == other_cpu && after.on_cpu == sched::no_cpu);
    EXPECT(!native::entry_stacks.active_on(other_cpu) && after.stacks.active_slots == 0);
    EXPECT(after.stacks.leased_slots == before.stacks.leased_slots);
    EXPECT(after.stacks.reservations == before.stacks.reservations && after.stacks.quarantined_slots == 0);
    EXPECT(!kernel.scheduler().frozen(pid) && thread(kernel, pid, tid).queued_cpu != sched::no_cpu);
    if (running) EXPECT(native::runtime_status().ok() && native::started);
}

void retire(Kernel &kernel, sched::ProcessId pid)
{
    native::release_process(kernel, pid);
    EXPECT(kernel.scheduler().exit_process(pid, 0)); EXPECT(kernel.scheduler().reap_orphans());
}

// exec quiesces the group first (spec 4.8): every refusal comes after the
// sibling left its CPU, and thaws the group. The sibling keeps its frame
// and its trapped syscall, and is queued again; no user progress is lost.
void refused_exec_thaws_sibling(Kernel &kernel)
{
    const auto g = create(kernel);
    thread(kernel, g.pid, g.sibling).native_frame.pc = hw::user_begin + 0x123;
    bool complete{};
    const auto refuse = [&](auto &&arm, auto &&disarm, bool keep_lease = false) {
        run_elsewhere(kernel, g.pid, g.sibling);
        const auto before = snapshot(kernel, g.pid, g.sibling);
        arm();
        {
            OtherCpu cpu{kernel, g.pid, g.sibling, true, nullptr, keep_lease};
            EXPECT(exec(kernel, g.pid, g.pid, complete) < 0 && complete && nothing_reported());
            cpu.join();
            EXPECT(cpu.frozen_seen());
        }
        disarm();
        if (keep_lease)
            EXPECT(native::entry_stacks.deactivate_entry(thread(kernel, g.pid, g.sibling).entry_stack,
                                                         {g.pid, g.sibling}, other_cpu));
        untouched(kernel, g.pid, g.sibling, before);
        thawed(kernel, g.pid, g.sibling, hw::user_begin + 0x123, true);
        thread(kernel, g.pid, g.sibling).syscall_pending = false;
    };
    // Scheduler preflight: a prepared UNIX receive refuses exec.
    refuse([&] { process(kernel, g.pid).resources().unix_receive_operations = 1; },
           [&] { process(kernel, g.pid).resources().unix_receive_operations = 0; });
    // Entry stack preparation: the caller's stack top disagrees with its lease.
    refuse([&] { thread(kernel, g.pid, g.pid).kernel_stack_top ^= 16; },
           [&] { thread(kernel, g.pid, g.pid).kernel_stack_top ^= 16; });
    // A sibling whose lease is still active after the quiesce (a step that
    // did not deactivate it) refuses the commit's escrow.
    refuse([] {}, [] {}, true);
    retire(kernel, g.pid);
}

// The caller is the leader; the sibling in user mode on CPU 1 leaves it at
// the claimer's kick, while the caller holds the kernel lock (it is released
// for the wait), and is erased by the commit. Its trapped execve, a second
// exec of the group, stays pending and never runs (Linux de_thread).
void exec_quiesces_sibling(Kernel &kernel)
{
    const auto g = create(kernel);
    const auto old_space = process(kernel, g.pid).resources().native_space;
    const auto leader_lease = thread(kernel, g.pid, g.pid).entry_stack;
    const auto sibling_lease = thread(kernel, g.pid, g.sibling).entry_stack;
    run_elsewhere(kernel, g.pid, g.sibling);
    const auto stacks = native::entry_stacks.accounting();
    bool complete{};
    {
        OtherCpu cpu{kernel, g.pid, g.sibling, true};
        EXPECT(exec(kernel, g.pid, g.pid, complete) == 0 && !complete && nothing_reported());
        cpu.join();
        EXPECT(cpu.frozen_seen());
    }
    EXPECT(native::runtime_status().ok() && native::started);
    auto &p = process(kernel, g.pid);
    EXPECT(p.record_count() == 1 && p.leader().tid == g.pid && p.leader().entry_stack == leader_lease);
    EXPECT(!native::same_space(old_space, p.resources().native_space));
    EXPECT(!kernel.scheduler().frozen(g.pid) && kernel.scheduler().transition(g.pid) == sched::GroupTransition::none);
    EXPECT(!native::entry_stacks.active_on(other_cpu) && native::entry_stacks.accounting().active_slots == 0);
    EXPECT(native::entry_stacks.accounting().leased_slots == stacks.leased_slots - 1);
    EXPECT(!native::entry_stacks.validate_inactive(sibling_lease, {g.pid, g.sibling}));
    EXPECT(!kernel.scheduler().find_thread(g.pid, g.sibling) && kernel.scheduler().current_tid(other_cpu) == 0);
    retire(kernel, g.pid);
}

// A sibling calls exec while the leader runs on CPU 1: the leader's lease
// becomes the new image's, and the caller's is released.
void exec_quiesces_leader(Kernel &kernel)
{
    const auto g = create(kernel);
    const auto leader_lease = thread(kernel, g.pid, g.pid).entry_stack;
    const auto caller_lease = thread(kernel, g.pid, g.sibling).entry_stack;
    run_elsewhere(kernel, g.pid, g.pid);
    bool complete{};
    {
        OtherCpu cpu{kernel, g.pid, g.pid};
        EXPECT(exec(kernel, g.pid, g.sibling, complete) == 0 && !complete && nothing_reported());
        cpu.join();
        EXPECT(cpu.frozen_seen());
    }
    EXPECT(native::runtime_status().ok() && native::started);
    auto &p = process(kernel, g.pid);
    EXPECT(p.record_count() == 1 && p.leader().tid == g.pid && p.leader().entry_stack == leader_lease);
    EXPECT(p.leader().on_cpu == sched::no_cpu && p.leader().queued_cpu != sched::no_cpu);
    EXPECT(native::entry_stacks.validate_inactive(leader_lease, {g.pid, g.pid}));
    EXPECT(!native::entry_stacks.validate_inactive(caller_lease, {g.pid, g.sibling}));
    EXPECT(native::entry_stacks.accounting().active_slots == 0);
    retire(kernel, g.pid);
}

// Without a thread on another CPU nothing waits, as on one CPU.
void exec_without_other_cpu(Kernel &kernel)
{
    const auto g = create(kernel);
    bool complete{};
    EXPECT(exec(kernel, g.pid, g.sibling, complete) == 0 && !complete && nothing_reported());
    EXPECT(process(kernel, g.pid).record_count() == 1 && !kernel.scheduler().frozen(g.pid));
    retire(kernel, g.pid);
}

// Exit beats exec (spec 4.8): a kill from another CPU lands while the exec
// claimer waits with the kernel lock released. The exec's commit check finds
// the pending exit, and the caller exits the group in place of the exec.
void kill_from_other_cpu(Kernel &kernel, sched::ProcessId pid)
{
    native::request_group_exit(kernel, pid, {0, 9});
}
void exit_beats_exec(Kernel &kernel)
{
    const auto g = create(kernel);
    const auto old_space = process(kernel, g.pid).resources().native_space;
    run_elsewhere(kernel, g.pid, g.sibling);
    bool complete{};
    {
        OtherCpu cpu{kernel, g.pid, g.sibling, false, kill_from_other_cpu};
        EXPECT(exec(kernel, g.pid, g.pid, complete) == 0 && !complete && nothing_reported());
        cpu.join();
        EXPECT(cpu.frozen_seen());
    }
    EXPECT(native::runtime_status().ok() && native::started);
    auto &p = process(kernel, g.pid);
    EXPECT(p.state() == sched::ProcessState::zombie && p.wait_status() == 9);
    EXPECT(!p.resources().native_space.root && !native::same_space(old_space, p.resources().native_space));
    for (const auto &member : p.thread_records()) EXPECT(!member.entry_stack.pool && !member.native_active);
    EXPECT(native::entry_stacks.accounting().active_slots == 0 && kernel.scheduler().current_tid(other_cpu) == 0);
    EXPECT(kernel.scheduler().reap_orphans());
}

// exit_group with a sibling in user mode on CPU 1 (spec 4.8): the caller
// claims exit and waits with the kernel lock released; the sibling's frozen
// check takes it off its CPU, and the group retires every lease.
void exit_group_quiesces_sibling(Kernel &kernel)
{
    const auto g = create(kernel);
    run_elsewhere(kernel, g.pid, g.sibling);
    bool complete{};
    {
        OtherCpu cpu{kernel, g.pid, g.sibling, true};
        EXPECT(call(kernel, g.pid, g.pid, OK_SYS_EXIT_GROUP, 5, complete) == 0 && !complete);
        cpu.join();
        EXPECT(cpu.frozen_seen());
    }
    EXPECT(native::runtime_status().ok() && native::started);
    auto &p = process(kernel, g.pid);
    EXPECT(p.state() == sched::ProcessState::zombie && p.exit_code() == 5);
    for (const auto &member : p.thread_records()) EXPECT(!member.entry_stack.pool && !member.native_active);
    EXPECT(native::entry_stacks.accounting().active_slots == 0 && kernel.scheduler().current_tid(other_cpu) == 0);
    EXPECT(kernel.scheduler().reap_orphans());
}

// The checks that predict a retirement or install step fail only on an
// invariant breach. They stop the runtime as that step's failure did after the
// scheduler commit: exec returns without writing the caller's frame. Nothing
// was committed: the quiesced sibling is thawed with its frame, the group is
// as it was.
void expect_stopped(Kernel &kernel, sched::ProcessId pid, sched::ThreadId caller, bool elsewhere,
                    sched::ThreadId sibling)
{
    const auto space = process(kernel, pid).resources().native_space;
    const auto threads = process(kernel, pid).record_count();
    const auto pc = thread(kernel, pid, caller).native_frame.pc;
    bool complete{};
    if (elsewhere)
    {
        run_elsewhere(kernel, pid, sibling);
        OtherCpu cpu{kernel, pid, sibling};
        EXPECT(exec(kernel, pid, caller, complete) == 0 && !complete && nothing_reported());
        cpu.join();
        EXPECT(cpu.frozen_seen());
    }
    else
        EXPECT(exec(kernel, pid, caller, complete) == 0 && !complete && nothing_reported());
    EXPECT(!native::runtime_status().ok() && !native::started);
    EXPECT(native::same_space(process(kernel, pid).resources().native_space, space));
    EXPECT(process(kernel, pid).record_count() == threads && thread(kernel, pid, caller).native_frame.pc == pc);
    EXPECT(!kernel.scheduler().frozen(pid) && kernel.scheduler().current_tid(other_cpu) == 0);
    native::runtime_failure = Status::success();
    native::started = true;
}
void breach_after_quiesce_stops(Kernel &kernel)
{
    const auto g = create(kernel);
    thread(kernel, g.pid, g.sibling).native_frame.pc = hw::user_begin + 0x123;
    const auto leader_lease = thread(kernel, g.pid, g.pid).entry_stack;
    // A futex wait whose object no longer resolves (cancel_futex).
    auto &futex = native::futex_waits.back();
    futex = {g.pid, g.sibling, {&native::futex_objects, 0, 0, ~u64{0}}};
    expect_stopped(kernel, g.pid, g.pid, true, g.sibling);
    thawed(kernel, g.pid, g.sibling, hw::user_begin + 0x123, false);
    futex = {};
    // A native UDP receive whose object no longer resolves.
    auto &udp = native::native_udp_receive_waits.back();
    udp = {g.pid, g.sibling, {&native::native_udp_receive_objects, 0, 0, ~u64{0}}};
    expect_stopped(kernel, g.pid, g.pid, true, g.sibling);
    thawed(kernel, g.pid, g.sibling, hw::user_begin + 0x123, false);
    udp = {};
    // A live Linux UDP borrow on the new leader's syscall origin record.
    auto &borrows = native::SyscallOriginTestAccess::borrows(native::syscall_origins, leader_lease);
    borrows = 1;
    expect_stopped(kernel, g.pid, g.pid, true, g.sibling);
    thawed(kernel, g.pid, g.sibling, hw::user_begin + 0x123, false);
    borrows = 0;
    // Exhausted syscall origin and VFS continuation image sequences.
    // (The sequences are per CPU: the calling CPU's count is exhausted.)
    auto &origin_images = native::SyscallOriginTestAccess::images(native::syscall_origins);
    const auto origin_saved = origin_images.exchange_count(smp::PerCpuSequence::limit);
    expect_stopped(kernel, g.pid, g.pid, true, g.sibling);
    thawed(kernel, g.pid, g.sibling, hw::user_begin + 0x123, false);
    origin_images.exchange_count(origin_saved);
    auto &vfs_images = native::VfsStatContinuationTestAccess::images(native::vfs_continuations);
    const auto vfs_saved = vfs_images.exchange_count(smp::PerCpuSequence::limit);
    expect_stopped(kernel, g.pid, g.pid, true, g.sibling);
    thawed(kernel, g.pid, g.sibling, hw::user_begin + 0x123, false);
    vfs_images.exchange_count(vfs_saved);
    // On one CPU the same breach stops the runtime the same way.
    futex = {g.pid, g.sibling, {&native::futex_objects, 0, 0, ~u64{0}}};
    expect_stopped(kernel, g.pid, g.sibling, false, g.pid);
    futex = {};
    retire(kernel, g.pid);
}

// The one refusal a program can reach after its leader exited: no entry stack
// lease is left for the new leader. The sibling on the other CPU was
// quiesced and is thawed with its frame; once a lease is free the same exec
// quiesces it again and succeeds.
void leaseless_leader_exec(Kernel &kernel)
{
    const auto g = create(kernel);
    auto caller = kernel.scheduler().create_thread_prepared(g.pid, thread(kernel, g.pid, g.pid).context,
        [](auto &created) { created.native_active = true; return native::prepare_native_thread(created); });
    EXPECT(caller);
    EXPECT(native::retire_native_thread(thread(kernel, g.pid, g.pid)));
    EXPECT(kernel.scheduler().exit_thread(g.pid, g.pid));
    const auto sibling_lease = thread(kernel, g.pid, g.sibling).entry_stack;
    const auto caller_lease = thread(kernel, g.pid, caller.value()).entry_stack;
    run_elsewhere(kernel, g.pid, g.sibling);
    const auto before = snapshot(kernel, g.pid, g.sibling);
    const auto leased = native::entry_stacks.accounting().leased_slots;
    std::array<memory::KernelEntryStackHandle, memory::KernelEntryStackPool::capacity> holds{};
    usize held{};
    for (; held < holds.size(); ++held)
    {
        auto reserved = native::entry_stacks.reserve({1000 + held, 1000 + held});
        if (!reserved) break;
        holds[held] = reserved.value();
    }
    EXPECT(held && native::entry_stacks.accounting().leased_slots == memory::KernelEntryStackPool::capacity);
    const auto full = snapshot(kernel, g.pid, g.sibling);
    bool complete{};
    {
        OtherCpu cpu{kernel, g.pid, g.sibling};
        const auto refused = exec(kernel, g.pid, caller.value(), complete);
        EXPECT(refused < 0 && complete && nothing_reported());
        cpu.join();
        EXPECT(cpu.frozen_seen());
    }
    untouched(kernel, g.pid, g.sibling, full);
    EXPECT(native::entry_stacks.accounting().releases == full.stacks.releases);
    for (usize i = 0; i < held; ++i) EXPECT(native::entry_stacks.release(holds[i], {1000 + i, 1000 + i}));
    EXPECT(native::entry_stacks.accounting().leased_slots == leased);
    run_elsewhere(kernel, g.pid, g.sibling);
    {
        OtherCpu cpu{kernel, g.pid, g.sibling};
        EXPECT(exec(kernel, g.pid, caller.value(), complete) == 0 && !complete && nothing_reported());
        cpu.join();
        EXPECT(cpu.frozen_seen());
    }
    EXPECT(native::runtime_status().ok() && native::started);
    auto &p = process(kernel, g.pid);
    EXPECT(p.record_count() == 1 && p.leader().tid == g.pid && p.leader().entry_stack.pool);
    EXPECT(p.leader().entry_stack != sibling_lease && p.leader().entry_stack != caller_lease);
    // The revived leader is queued: nothing else would select the new image.
    EXPECT(p.leader().state == sched::ProcessState::runnable && p.leader().on_cpu == sched::no_cpu &&
           p.leader().queued_cpu != sched::no_cpu);
    EXPECT(!native::entry_stacks.validate_inactive(sibling_lease, {g.pid, g.sibling}));
    EXPECT(!native::entry_stacks.validate_inactive(caller_lease, {g.pid, caller.value()}));
    EXPECT(native::entry_stacks.accounting().active_slots == 0 &&
           native::entry_stacks.accounting().leased_slots == leased - 1);
    static_cast<void>(before);
    retire(kernel, g.pid);
}
// The leader waits in a CPU hotplug write when a sibling calls exec. The
// leader's TCB, lease and wait ticket carry over to the new image, so its
// request goes with the old image: the new image never sees its result. The
// new image's only thread keeps the caller's affinity, not the leader's.
void exec_drops_leader_hotplug_request(Kernel &kernel)
{
    // CPU 1 is online for the service's checks.
    smp::cpu_record(1).state.store(static_cast<u8>(smp::CpuState::online));
    smp::detail::online.store(0x3);
    smp::detail::started.store(0x3);
    const auto g = create(kernel);
    constexpr uptr text = hw::user_begin + 640;
    std::memcpy(mock::memory.data() + (text - hw::user_begin), "0\n", 2);
    const auto before = smp::hotplug::statistics();
    // The process's activation picks a thread; then the leader is made current.
    EXPECT(kernel.posix().activate(g.pid)); EXPECT(kernel.scheduler().activate_thread(g.pid, g.pid));
    EXPECT(kernel.scheduler().current_tid(0) == g.pid);
    bool complete = true;
    EXPECT(native::cpu_control_write(kernel, g.pid, 1, text, 2, complete) == 0 && !complete);
    const auto &leader = thread(kernel, g.pid, g.pid);
    const auto ticket = leader.wait_registration.ticket();
    EXPECT(leader.state == sched::ProcessState::blocked && leader.wait_reason == sched::ThreadWaitReason::cpu_hotplug);
    EXPECT(leader.wait_registration.pending(ticket) && smp::hotplug::pending() == 1);
    // The blocked leader is confined to CPU 1; the caller runs on CPU 0.
    kernel.scheduler().store_thread_affinity(process(kernel, g.pid), thread(kernel, g.pid, g.pid), 0x2);
    kernel.scheduler().store_thread_affinity(process(kernel, g.pid), thread(kernel, g.pid, g.sibling), 0x1);
    EXPECT(kernel.posix().activate(0));
    EXPECT(exec(kernel, g.pid, g.sibling, complete) == 0 && !complete && nothing_reported());
    EXPECT(native::runtime_status().ok() && native::started);
    auto &p = process(kernel, g.pid);
    EXPECT(p.record_count() == 1 && p.leader().tid == g.pid);
    EXPECT(p.leader().wait_registration.ticket() == ticket && !p.leader().wait_registration.active());
    // The parked leader carries the new image: nothing but its queue entry
    // would select it, and its placement mirrors match.
    EXPECT(p.leader().state == sched::ProcessState::runnable && p.leader().on_cpu == sched::no_cpu &&
           p.leader().queued_cpu != sched::no_cpu);
    EXPECT(kernel.scheduler().audit_queues() == 0);
    EXPECT(kernel.scheduler().thread_affinity(p, p.leader()) == 0x1);
    EXPECT(smp::hotplug::pending() == 0 && !smp::hotplug::finished(p.leader().entry_stack.slot));
    EXPECT(!smp::hotplug::consume(p.leader().entry_stack.slot, ticket));
    EXPECT(smp::hotplug::statistics().dropped_waiting == before.dropped_waiting + 1);
    EXPECT(native::cpu_control_resume(p.leader()) == std::nullopt);
    retire(kernel, g.pid);
    smp::detail::started.store(0x1);
    smp::detail::online.store(0x1);
    smp::cpu_record(1).state.store(static_cast<u8>(smp::CpuState::offline));
}
} // namespace

int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(kernel.posix().activate(0));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(kernel.scheduler().configure_cpus(2) && kernel.scheduler().set_online_cpus(0x3));
    native::runtime_kernel = &kernel; native::started = true; mock::spaces_enabled = true;
    // The runtime's frozen check (installed when it starts serving threads).
    smp::bkl_frozen_check = native::group_frozen;
    make_elf(); CHECK(kernel.vfs().create_readonly_file("/image", elf, 0755));
    std::memcpy(mock::memory.data() + (path_address - hw::user_begin), "/image", 7);
    refused_exec_thaws_sibling(kernel);
    exec_quiesces_sibling(kernel);
    exec_quiesces_leader(kernel);
    exec_without_other_cpu(kernel);
    exit_beats_exec(kernel);
    exit_group_quiesces_sibling(kernel);
    breach_after_quiesce_stops(kernel);
    leaseless_leader_exec(kernel);
    exec_drops_leader_hotplug_request(kernel);
    const auto stacks = native::entry_stacks.accounting();
    EXPECT(!stacks.leased_slots && !stacks.active_slots && !stacks.quarantined_slots);
    EXPECT(stacks.reservations == stacks.releases && native::runtime_status().ok());
    EXPECT(smp::check_total(smp::KernelCheck::quiesce_timeouts) == 0);
    std::printf("PASS native exec eviction checks=%zu scope=hosted-model model_cpus=2 guest=0 smp=0\n", checks);
}
