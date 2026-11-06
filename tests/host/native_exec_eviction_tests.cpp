// Exec with a sibling thread that another CPU runs, through the actual runtime
// dispatcher, loader, entry stack pool and scheduler. The other CPU is a model:
// its runtime entry record, active lease and scheduler slot are set the way
// enter_unlocked and the scheduler leave them; no second CPU executes.
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_clock_model.hpp"
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
    CHECK(native::prepare_native_thread(p.threads()[0])); CHECK(native::install(p, image.value()));
    auto sibling = kernel.scheduler().create_thread_prepared(p.pid(), p.threads()[0].context, [](auto &created)
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
    native::cpu_entries[other_cpu] = {pid, tid, running.entry_stack, false};
}

// The other CPU's view after it returned from user mode.
void return_elsewhere(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid, bool revoked)
{
    EXPECT(native::cpu_entries[other_cpu].revoked == revoked);
    native::cpu_entries[other_cpu] = {};
    if (revoked) return; // Exec deactivated the lease and vacated the slot.
    auto &running = thread(kernel, pid, tid);
    EXPECT(native::entry_stacks.deactivate_entry(running.entry_stack, {pid, tid}, other_cpu));
    EXPECT(kernel.scheduler().clear_on_cpu(pid, tid, other_cpu));
}

i64 exec(Kernel &kernel, sched::ProcessId pid, sched::ThreadId caller, bool &complete)
{
    EXPECT(kernel.scheduler().activate_thread(pid, caller)); EXPECT(kernel.posix().activate(pid));
    complete = true;
    return native::dispatch(kernel, pid, {OK_SYS_EXECVE, {path_address, 0, 0, 0, 0, 0}}, complete);
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
    return {process(kernel, pid).resources().native_space, process(kernel, pid).threads().size(), running.entry_stack,
            running.native_frame.pc, running.kernel_stack_top, running.on_cpu, native::entry_stacks.accounting()};
}
// Nothing of the thread another CPU runs, or of the group, changed.
void untouched(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid, const Snapshot &before)
{
    const auto after = snapshot(kernel, pid, tid);
    EXPECT(native::same_space(after.space, before.space) && after.threads == before.threads);
    EXPECT(after.lease == before.lease && after.pc == before.pc && after.top == before.top);
    EXPECT(after.on_cpu == other_cpu && before.on_cpu == other_cpu);
    EXPECT(native::cpu_entries[other_cpu].pid == pid && native::cpu_entries[other_cpu].tid == tid);
    EXPECT(!native::cpu_entries[other_cpu].revoked && native::cpu_entries[other_cpu].lease == before.lease);
    EXPECT(native::entry_stacks.active_on(other_cpu) && after.stacks.active_slots == 1);
    EXPECT(after.stacks.leased_slots == before.stacks.leased_slots);
    EXPECT(after.stacks.reservations == before.stacks.reservations && after.stacks.quarantined_slots == 0);
    EXPECT(native::runtime_status().ok() && native::started);
}

void retire(Kernel &kernel, sched::ProcessId pid)
{
    native::release_process(kernel, pid);
    EXPECT(kernel.scheduler().exit_process(pid, 0)); EXPECT(kernel.scheduler().reap_orphans());
}

// Every refusal that used to follow the eviction now precedes it.
void refused_exec_keeps_sibling(Kernel &kernel)
{
    const auto g = create(kernel);
    thread(kernel, g.pid, g.sibling).native_frame.pc = hw::user_begin + 0x123;
    run_elsewhere(kernel, g.pid, g.sibling);
    const auto before = snapshot(kernel, g.pid, g.sibling);
    bool complete{};
    // Scheduler preflight: a prepared UNIX receive refuses exec.
    process(kernel, g.pid).resources().unix_receive_operations = 1;
    EXPECT(exec(kernel, g.pid, g.pid, complete) < 0 && complete);
    process(kernel, g.pid).resources().unix_receive_operations = 0;
    untouched(kernel, g.pid, g.sibling, before);
    // Entry stack preparation: the caller's stack top disagrees with its lease.
    thread(kernel, g.pid, g.pid).kernel_stack_top ^= 16;
    EXPECT(exec(kernel, g.pid, g.pid, complete) < 0 && complete);
    thread(kernel, g.pid, g.pid).kernel_stack_top ^= 16;
    untouched(kernel, g.pid, g.sibling, before);
    // Eviction preparation: a CPU entry that does not name the thread's lease.
    native::cpu_entries[other_cpu].lease.generation ^= 1;
    EXPECT(exec(kernel, g.pid, g.pid, complete) < 0 && complete);
    native::cpu_entries[other_cpu].lease.generation ^= 1;
    untouched(kernel, g.pid, g.sibling, before);
    // The sibling's entry completes normally and the group exits as usual.
    return_elsewhere(kernel, g.pid, g.sibling, false);
    EXPECT(thread(kernel, g.pid, g.sibling).native_frame.pc == hw::user_begin + 0x123);
    retire(kernel, g.pid);
}

// The caller is the leader; the sibling another CPU runs is evicted and erased.
void exec_evicts_sibling(Kernel &kernel)
{
    const auto g = create(kernel);
    const auto old_space = process(kernel, g.pid).resources().native_space;
    const auto leader_lease = thread(kernel, g.pid, g.pid).entry_stack;
    const auto sibling_lease = thread(kernel, g.pid, g.sibling).entry_stack;
    run_elsewhere(kernel, g.pid, g.sibling);
    const auto stacks = native::entry_stacks.accounting();
    bool complete{};
    EXPECT(exec(kernel, g.pid, g.pid, complete) == 0 && !complete);
    EXPECT(native::runtime_status().ok() && native::started);
    auto &p = process(kernel, g.pid);
    EXPECT(p.threads().size() == 1 && p.threads()[0].tid == g.pid && p.threads()[0].entry_stack == leader_lease);
    EXPECT(!native::same_space(old_space, p.resources().native_space));
    EXPECT(!native::entry_stacks.active_on(other_cpu) && native::entry_stacks.accounting().active_slots == 0);
    EXPECT(native::entry_stacks.accounting().leased_slots == stacks.leased_slots - 1);
    EXPECT(!native::entry_stacks.validate_inactive(sibling_lease, {g.pid, g.sibling}));
    EXPECT(!kernel.scheduler().find_thread(g.pid, g.sibling));
    return_elsewhere(kernel, g.pid, g.sibling, true);
    retire(kernel, g.pid);
}

// A sibling calls exec while the leader runs on another CPU: the leader's
// evicted lease becomes the new image's, and the caller's is released.
void exec_evicts_leader(Kernel &kernel)
{
    const auto g = create(kernel);
    const auto leader_lease = thread(kernel, g.pid, g.pid).entry_stack;
    const auto caller_lease = thread(kernel, g.pid, g.sibling).entry_stack;
    run_elsewhere(kernel, g.pid, g.pid);
    bool complete{};
    EXPECT(exec(kernel, g.pid, g.sibling, complete) == 0 && !complete);
    EXPECT(native::runtime_status().ok() && native::started);
    auto &p = process(kernel, g.pid);
    EXPECT(p.threads().size() == 1 && p.threads()[0].tid == g.pid && p.threads()[0].entry_stack == leader_lease);
    EXPECT(p.threads()[0].on_cpu == sched::no_cpu);
    EXPECT(native::entry_stacks.validate_inactive(leader_lease, {g.pid, g.pid}));
    EXPECT(!native::entry_stacks.validate_inactive(caller_lease, {g.pid, g.sibling}));
    EXPECT(native::entry_stacks.accounting().active_slots == 0);
    return_elsewhere(kernel, g.pid, g.pid, true);
    retire(kernel, g.pid);
}

// Without a thread on another CPU nothing is evicted, as on one CPU.
void exec_without_other_cpu(Kernel &kernel)
{
    const auto g = create(kernel);
    bool complete{};
    EXPECT(exec(kernel, g.pid, g.sibling, complete) == 0 && !complete);
    EXPECT(process(kernel, g.pid).threads().size() == 1 && native::cpu_entries[other_cpu].pid == 0);
    retire(kernel, g.pid);
}
} // namespace

int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(kernel.posix().activate(0));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(kernel.scheduler().configure_cpus(2) && kernel.scheduler().set_online_cpus(0x3));
    native::runtime_kernel = &kernel; native::started = true; mock::spaces_enabled = true;
    make_elf(); CHECK(kernel.vfs().create_readonly_file("/image", elf, 0755));
    std::memcpy(mock::memory.data() + (path_address - hw::user_begin), "/image", 7);
    refused_exec_keeps_sibling(kernel);
    exec_evicts_sibling(kernel);
    exec_evicts_leader(kernel);
    exec_without_other_cpu(kernel);
    const auto stacks = native::entry_stacks.accounting();
    EXPECT(!stacks.leased_slots && !stacks.active_slots && !stacks.quarantined_slots);
    EXPECT(stacks.reservations == stacks.releases && native::runtime_status().ok());
    std::printf("PASS native exec eviction checks=%zu scope=hosted-model guest=0 smp=0\n", checks);
}
