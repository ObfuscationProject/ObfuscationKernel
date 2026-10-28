// Reuse the actual-runtime hosted fixture; do not execute its policy matrix.
#define main inherited_child_exit_runtime_main
#include "native_child_exit_runtime_tests.cpp"
#undef main

namespace {
constexpr uptr path_address = hw::user_begin + 128;
void install_exec_image(Kernel &kernel)
{
    static std::array<std::byte, 4112> elf{};
    const auto put = [&](usize offset, u64 value, usize size) {
        for (usize i = 0; i < size; ++i) elf[offset + i] = static_cast<std::byte>(value >> (8 * i));
    };
    elf[0]=std::byte{0x7f}; elf[1]=std::byte{'E'}; elf[2]=std::byte{'L'}; elf[3]=std::byte{'F'};
    elf[4]=std::byte{2}; elf[5]=std::byte{1}; elf[6]=std::byte{1}; elf[7]=std::byte{3};
    put(16,2,2); put(18,62,2); put(20,1,4); put(24,hw::user_begin+256,8); put(32,64,8);
    put(52,64,2); put(54,56,2); put(56,2,2);
    put(64,1,4); put(68,5,4); put(80,hw::user_begin,8); put(88,hw::user_begin,8);
    put(96,512,8); put(104,4096,8); put(112,4096,8);
    put(120,1,4); put(124,6,4); put(128,4096,8); put(136,hw::user_begin+4096,8);
    put(144,hw::user_begin+4096,8); put(152,16,8); put(160,4096,8); put(168,4096,8);
    elf[256]=std::byte{0xc3};
    CHECK(kernel.vfs().create_readonly_file("/child-exec-audit", elf));
}
i64 exec(Kernel &kernel, u64 pid, u64 tid, bool &complete)
{
    constexpr char path[] = "/child-exec-audit";
    std::memcpy(mock::memory.data() + path_address - hw::user_begin, path, sizeof(path));
    activate(kernel, pid, tid); mock::spaces_enabled = true; complete = true;
    return native::dispatch(kernel, pid, {OK_SYS_EXECVE, {path_address, 0, 0}}, complete);
}
struct StackPressure
{
    std::array<memory::KernelEntryStackHandle, memory::KernelEntryStackPool::capacity> leases{};
    usize count{};
    void fill()
    {
        for (;;)
        {
            const auto made = native::entry_stacks.reserve({900000 + count, 900000 + count});
            if (!made) { expect(made.status().code() == StatusCode::no_memory); break; }
            leases[count++] = made.value();
        }
        expect(native::entry_stacks.accounting().leased_slots == memory::KernelEntryStackPool::capacity);
    }
    void drain()
    {
        for (usize i = 0; i < count; ++i)
            CHECK(native::entry_stacks.release(leases[i], {900000 + i, 900000 + i}));
        count = 0;
    }
};
void canonical_stack_exec(Kernel &kernel, bool nonleader, bool retired_leader, bool reused_slot)
{
    const auto parent = create(kernel), observer = create(kernel);
    action(kernel, parent, handler);
    const auto caller = nonleader ? peer(kernel, parent) : parent;
    const auto child = fork(kernel, parent, caller);
    auto &calling_thread = *kernel.scheduler().find_thread(parent, caller);
    calling_thread.native_signal_mask.set(sigchld); calling_thread.native_signal_mask.set(12);
    auto *directed = native::private_signal_queue(calling_thread);
    CHECK(directed->enqueue({12, -6, static_cast<u32>(observer), 0}) == native::PrivateSignalQueue::Enqueue::queued);
    auto *shared = &native::private_signal_find(parent)->shared;
    CHECK(shared->enqueue_child_exit(sched::ChildExitStatus::normal(31), 777, 888) == native::PrivateProcessQueue::Enqueue::queued);
    const auto shared_ticket = pending(parent);
    const auto owner_epoch = native::private_signal_find(parent)->epoch;
    const auto leader_lease = kernel.scheduler().find_thread(parent, parent)->entry_stack;
    const auto caller_lease = calling_thread.entry_stack;
    if (retired_leader) raw_exit(kernel, parent, parent, 17);
    u64 occupying{};
    if (reused_slot)
    {
        occupying = create(kernel);
        expect(kernel.scheduler().find_thread(occupying, occupying)->entry_stack.slot == leader_lease.slot);
    }
    const auto before = native::entry_stacks.accounting();
    bool complete{}; expect(exec(kernel, parent, caller, complete) == 0 && !complete);
    expect(native::runtime_status().ok());
    auto &process = *kernel.scheduler().find(parent);
    expect(process.threads().size() == 1 && process.threads()[0].tid == parent);
    const auto canonical = process.threads()[0].entry_stack;
    const auto after = native::entry_stacks.accounting();
    if (!retired_leader)
    {
        expect(canonical == leader_lease);
        expect(after.reservations == before.reservations);
        expect(after.releases == before.releases + static_cast<usize>(nonleader));
    }
    else
    {
        expect(canonical != leader_lease && canonical != caller_lease);
        expect(canonical.generation > leader_lease.generation);
        expect(after.reservations == before.reservations + 1 && after.releases == before.releases + 1);
        expect(reused_slot ? canonical.slot != leader_lease.slot : canonical.slot == leader_lease.slot);
    }
    expect(native::private_signal_find(parent)->epoch == owner_epoch);
    expect(&native::private_signal_find(parent)->shared == shared && shared->identity() == shared_ticket.owner);
    expect(shared->consume(shared_ticket));
    expect(native::private_signal_queue(process.threads()[0])->pending_mask().contains(12));
    expect(native::private_signal_find(child)->lineage.parent_tid == parent);
    expect(native::private_signal_find(parent)->lineage.leader_cause == sched::ChildExitStatus{});
    if (nonleader) expect(!kernel.scheduler().find_thread(parent, caller));
    raw_exit(kernel, child, child, 23);
    expect(pending(parent).info.sender_pid == child); CHECK(kernel.scheduler().wait_process(parent, child));
    release(kernel, parent); release(kernel, observer); if (occupying) release(kernel, occupying);
}
void canonical_stack_pressure(Kernel &kernel)
{
    for (bool retired_leader : {false, true})
    {
        const auto parent = create(kernel), caller = peer(kernel, parent), child = fork(kernel, parent, caller);
        const auto original = kernel.scheduler().find_thread(parent, parent)->entry_stack;
        const auto caller_lease = kernel.scheduler().find_thread(parent, caller)->entry_stack;
        if (retired_leader) raw_exit(kernel, parent, parent, 17);
        const auto epoch = native::private_signal_find(parent)->epoch;
        const auto lineage = native::private_signal_find(child)->lineage;
        auto *shared = &native::private_signal_find(parent)->shared;
        CHECK(shared->enqueue_child_exit(sched::ChildExitStatus::normal(31), 777, 888) == native::PrivateProcessQueue::Enqueue::queued);
        const auto ticket = pending(parent);
        StackPressure pressure; pressure.fill();
        const auto before = native::entry_stacks.accounting();
        const auto pages_before = native::page_pool.used_count(); bool complete{};
        const auto result = exec(kernel, parent, caller, complete);
        const auto after = native::entry_stacks.accounting();
        if (retired_leader)
        {
            expect(result == -12 && complete);
            expect(native::page_pool.used_count() == pages_before);
            expect(after.leased_slots == before.leased_slots && after.reservations == before.reservations && after.releases == before.releases);
            expect(kernel.scheduler().find_thread(parent, caller)->entry_stack == caller_lease);
            expect(!kernel.scheduler().find_thread(parent, parent)->entry_stack.pool);
            expect(native::private_signal_find(parent)->lineage.leader_cause == sched::ChildExitStatus::normal(17));
            expect(native::private_signal_find(child)->lineage.parent_tid == lineage.parent_tid);
            expect(kernel.scheduler().find(parent)->threads().size() == 2);
        }
        else
        {
            expect(result == 0 && !complete);
            expect(kernel.scheduler().find_thread(parent, parent)->entry_stack == original);
            expect(after.reservations == before.reservations && after.releases == before.releases + 1);
            expect(native::private_signal_find(child)->lineage.parent_tid == parent);
        }
        expect(native::private_signal_find(parent)->epoch == epoch && shared->consume(ticket));
        expect(native::runtime_status().ok()); pressure.drain();
        release(kernel, child); release(kernel, parent);
    }
}
void replacement_chain(Kernel &kernel)
{
    const auto parent = create(kernel), creator = peer(kernel, parent), replacement = peer(kernel, parent);
    auto &creator_thread = *kernel.scheduler().find_thread(parent, creator);
    auto &replacement_thread = *kernel.scheduler().find_thread(parent, replacement);
    creator_thread.native_signal_mask.set(sigchld); replacement_thread.native_signal_mask.set(sigchld);
    const auto first = fork(kernel, parent, creator), second = fork(kernel, parent, creator);
    raw_exit(kernel, parent, creator, 19);
    expect(native::private_signal_find(first)->lineage.parent_tid == parent);
    raw_exit(kernel, first, first, 21);
    expect(!native::private_signal_find(parent)->shared.pending_mask().contains(sigchld));
    CHECK(kernel.scheduler().wait_process(parent, first));
    raw_exit(kernel, parent, parent, 17);
    expect(native::private_signal_find(second)->lineage.parent_tid == replacement);
    raw_exit(kernel, second, second, 22);
    expect(pending(parent).info.child_status == 22);
    CHECK(kernel.scheduler().wait_process(parent, second));
    release(kernel, parent);
}
void leader_first_fatal(Kernel &kernel, u32 signal)
{
    const auto parent = create(kernel); action(kernel, parent, handler);
    const auto child = fork(kernel, parent), survivor = peer(kernel, child);
    raw_exit(kernel, child, child, 17);
    const auto *borrowed = kernel.scheduler().find_thread(child, survivor);
    expect(native::private_process_kill(kernel, *kernel.scheduler().find(parent), child, signal) == 0);
    if (signal == 9) native::private_process_signal_fatal(kernel);
    else expect(!native::private_signal_deliver(kernel, *kernel.scheduler().find(child),
                                              *kernel.scheduler().find_thread(child, survivor)));
    expect(native::runtime_status().ok());
    expect(kernel.scheduler().find_thread(child, survivor) == borrowed);
    const auto info = pending(parent).info;
    expect(info.code == 1 && info.child_status == 17);
    expect(kernel.scheduler().find(child)->wait_status() == static_cast<i32>(signal));
    CHECK(kernel.scheduler().wait_process(parent, child)); release(kernel, parent);
}
void post_commit_space_failure(Kernel &kernel)
{
    const auto pid = create(kernel);
    auto &process = *kernel.scheduler().find(pid);
    auto old_space = process.resources().native_space;
    const auto old_page_count = process.resources().native_pages.size();
    const auto lease = process.threads()[0].entry_stack;
    const auto page_count = native::page_pool.user_count();
    mock::fail_destroy_root = old_space.root;
    bool complete{}; expect(exec(kernel, pid, pid, complete) == 0 && !complete);
    expect(!native::runtime_status());
    expect(kernel.scheduler().find_thread(pid, pid)->entry_stack == lease);
    expect(kernel.scheduler().find(pid)->resources().native_space.root != old_space.root);
    expect(native::page_pool.pinned_count() == old_page_count);
    const auto entries = mock::entries;
    expect(!native::step(kernel) && !native::started && mock::entries == entries);
    // Controlled harness recovery of retained pre-commit VM ownership.
    native::runtime_failure = Status::success();
    for (auto &held : native::quarantined_spaces)
        if (held.space.root && native::same_space(held.space, old_space))
            expect(native::retire_space(old_space, held.pages).ok());
    expect(native::runtime_status().ok() && !native::page_pool.pinned_count());
    release(kernel, pid);
    expect(native::page_pool.user_count() == page_count - 1);
}
void leader_cause_reset_after_exec(Kernel &kernel)
{
    const auto parent = create(kernel); action(kernel, parent, handler);
    const auto child = fork(kernel, parent), survivor = peer(kernel, child);
    raw_exit(kernel, child, child, 17);
    bool complete{}; expect(exec(kernel, child, survivor, complete) == 0 && !complete);
    expect(native::private_signal_find(child)->lineage.leader_cause == sched::ChildExitStatus{});
    raw_exit(kernel, child, child, 23);
    expect(pending(parent).info.child_status == 23 && kernel.scheduler().find(child)->wait_status() == 23 * 256);
    CHECK(kernel.scheduler().wait_process(parent, child)); release(kernel, parent);
}
}
// Linux exec stores AT_RANDOM bytes; hosted kernels have no entropy device.
struct HostedExecEntropy final : driver::EntropySource
{
    Status fill(std::span<std::byte> output) override
    {
        for (usize index = 0; index < output.size(); ++index) output[index] = static_cast<std::byte>(index + 1);
        return Status::success();
    }
};
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    static HostedExecEntropy entropy; kernel.posix().set_entropy_source(&entropy);
    install_exec_image(kernel);
    canonical_stack_exec(kernel, false, false, false);
    canonical_stack_exec(kernel, true, false, false);
    canonical_stack_exec(kernel, true, true, false);
    canonical_stack_exec(kernel, true, true, true);
    canonical_stack_pressure(kernel); replacement_chain(kernel);
    leader_first_fatal(kernel, 15); leader_first_fatal(kernel, 9); leader_cause_reset_after_exec(kernel);
    post_commit_space_failure(kernel);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    std::printf("CHILD_EXIT_EXEC_AUDIT_PASS checks=%zu hosted=1 native=0 baseline_matrix_rerun=0\n", checks);
}
