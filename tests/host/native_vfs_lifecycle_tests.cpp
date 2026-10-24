// Actual runtime retirement and image transactions with a hosted VFS provider.
#define OK_ENABLE_TEST_POINTS 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#include <atomic>
#include <thread>
#include <source_location>
#include "native_clock_model.hpp"

namespace
{
usize checks{}, cases{};
void require(bool value, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "VFS_LIFECYCLE_FAIL line=%u checks=%zu\n", where.line(), checks); std::abort(); }
}
struct Provider final : fs::Node
{
    fs::VirtualFileSystem *vfs{};
    mutable std::atomic<bool> pause{}, entered{}, released{};
    std::string_view name() const override { return "lifecycle-provider"; }
    fs::Metadata metadata() const override
    {
        const fs::EpochTimestamp stamp{123456, 7, fs::TimestampSource::readonly_ext4};
        return {.type = fs::NodeType::directory, .size = 321, .mode = fs::mode_for(fs::NodeType::directory, 0755),
            .blocks = 1, .attributes = {.inode = 7, .valid = 63, .access = stamp, .modification = stamp, .change = stamp}};
    }
    Result<fs::Metadata> query_metadata() const override
    {
        static_cast<void>(vfs->mode());
        if (pause.exchange(false)) { entered = true; while (!released.load()) std::this_thread::yield(); }
        return metadata();
    }
    Result<fs::FileBuffer> read(usize, usize) const override { return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { return Status::unsupported(); }
    fs::Node *lookup(std::string_view child) override { return child == "." || child == ".." ? this : nullptr; }
    Status create(std::string_view, fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32, u32) override { return Status::unsupported(); }
    const void *filesystem_identity() const override { return this; }
};
struct Pause
{
    Provider &provider;
    std::thread holder;
    Pause(Provider &node) : provider(node)
    {
        provider.entered = false; provider.released = false; provider.pause = true;
        holder = std::thread([&node] { if (!node.vfs->query_metadata(node)) std::abort(); });
        while (!provider.entered.load()) std::this_thread::yield();
    }
    void release() { provider.released = true; if (holder.joinable()) holder.join(); }
    ~Pause() { release(); }
};
using Continuation = native::VfsStatContinuation<lu::configured_native_profile()>;
struct Fixture
{
    Kernel &kernel;
    Provider &provider;
    sched::ProcessId pid{};
    Fixture(Kernel &owner, Provider &node) : kernel(owner), provider(node)
    {
        auto made = kernel.scheduler().create_user_process("vfs-lifecycle",
            kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 8192})); require(made.ok()); pid = made.value();
        auto *process = kernel.scheduler().find(pid);
        process->resources().personality = sched::UserPersonality::linux;
        process->resources().native_space = {0x100000 + pid * 4096, static_cast<u32>(pid % 16), 1};
        require(native::allocate(process->resources().native_pages, hw::user_begin, hw::page_read | hw::page_execute).ok());
        require(native::prepare_native_thread(thread()).ok()); thread().native_active = true;
        require(kernel.scheduler().set_runnable(pid).ok()); require(kernel.scheduler().activate_thread(pid, pid).ok());
        require(kernel.posix().activate(pid).ok()); native::runtime_kernel = &kernel; native::started = true;
    }
    sched::ProcessControlBlock &process() { return *kernel.scheduler().find(pid); }
    sched::ThreadControlBlock &thread(sched::ThreadId tid = 0) { return *kernel.scheduler().find_thread(pid, tid ? tid : pid); }
    sched::ThreadId peer()
    {
        const auto made = kernel.scheduler().create_thread(pid, thread().context); require(made.ok());
        require(native::prepare_native_thread(thread(made.value())).ok()); thread(made.value()).native_active = true;
        return made.value();
    }
    void capture(sched::ThreadId tid = 0)
    {
        auto &target = thread(tid); require(kernel.scheduler().activate_thread(pid, target.tid).ok());
        target.native_frame = {}; target.native_frame.pc = hw::user_begin + 1026; target.native_frame.sp = hw::user_begin + 8192;
        target.native_frame.registers[14] = *lu::Codec<lu::Profile::x86_64>::syscall_number(lu::Operation::statx);
        target.native_frame.registers[9] = static_cast<uptr>(static_cast<i64>(posix::at_FDCWD));
        target.native_frame.registers[10] = hw::user_begin + 512; target.native_frame.registers[5] = 0x7ff;
        target.native_frame.registers[7] = hw::user_begin + 2048;
        mock::memory[1024] = std::byte{0x0f}; mock::memory[1025] = std::byte{0x05};
        std::strcpy(reinterpret_cast<char *>(mock::memory.data() + 512), "/tmp/lifecycle-provider");
        target.syscall_pending = false; mock::entry_trap = {hw::TrapKind::syscall, 256};
        const auto before = mock::entries;
        auto view = native::entry_stacks.activate_entry(target.entry_stack, {pid, target.tid}, 0); require(view.ok());
        require(native::shared_capture.enter(native::syscall_origins, native::vfs_continuations,
            process(), target, view.value(), kernel.scheduler()).ok());
        require(native::entry_stacks.deactivate_entry(target.entry_stack, {pid, target.tid}, 0).ok());
        require(mock::entries == before + 1); target.syscall_pending = true;
        require(!native::syscall_origins.live(native::syscall_origins.identity(process(), target)));
    }
    Continuation::Handle begin(sched::ThreadId tid = 0)
    {
        auto &target = thread(tid); bool complete = true;
        const auto value = native::vfs_continuations.begin(kernel.scheduler(), kernel.vfs(), pid, target.tid,
            [&](u64 pointer, FixedString<256> &path) { return native::string_from_user(process().resources().native_space, pointer, path); },
            [](uptr, std::span<u8>) { return Status::unsupported(); },
            [&](u64 pointer, usize size) { return hw::user_range(process().resources().native_space, pointer, size, true); },
            [&](u64 pointer, std::span<const u8> bytes) { return hw::copy_to_user(process().resources().native_space, pointer, bytes.data(), bytes.size()); }, complete);
        require(value == -11 && !complete); auto handle = native::vfs_continuations.pending(pid, target.tid);
        require(handle.request && target.wait_registration.active()); return handle;
    }
    ~Fixture()
    {
        native::release_process(kernel, pid); require(native::runtime_status().ok());
        require(kernel.scheduler().exit_process(pid, 0).ok()); require(kernel.scheduler().reap_orphans() == 1);
        require(native::entry_stacks.accounting().leased_slots == 0);
    }
};

#ifdef OK_VFS_LIFECYCLE_PREDECESSOR
void predecessor(Kernel &kernel, Provider &provider)
{
    Fixture f(kernel, provider); f.capture(); Pause held(f.provider); const auto handle = f.begin();
    const auto ticket = native::vfs_continuations.ticket(handle); const auto lease = f.thread().entry_stack;
    const auto before = native::entry_stacks.accounting();
    require(native::retire_native_thread(f.thread()).ok());
    require(!f.thread().entry_stack.pool && native::vfs_continuations.pending(f.pid, f.pid) == handle);
    require(f.thread().wait_registration.pending(ticket));
    require(native::prepare_native_thread(f.thread()).code() == StatusCode::busy);
    require(!f.thread().entry_stack.pool && native::vfs_continuations.pending(f.pid, f.pid) == handle);
    require(native::entry_stacks.accounting().reservations == before.reservations + 1);
    require(native::vfs_continuations.cancel(kernel.scheduler(), handle).ok());
    require(native::prepare_native_thread(f.thread()).ok());
    require(f.thread().entry_stack.slot == lease.slot && f.thread().entry_stack.generation > lease.generation);
    held.release(); require(native::vfs_continuations.drain(kernel.scheduler(), kernel.vfs()).ok()); ++cases;
}
#else
void retirement(Kernel &kernel, Provider &provider, bool ready)
{
    Fixture f(kernel, provider); f.capture(); Pause held(provider); const auto handle = f.begin();
    const auto ticket = native::vfs_continuations.ticket(handle); const auto lease = f.thread().entry_stack;
    if (ready) { held.release(); require(native::vfs_continuations.drain(kernel.scheduler(), kernel.vfs()).ok()); }
    const auto before = native::entry_stacks.accounting();
    require(native::retire_native_thread(f.thread()).ok());
    require(!f.thread().entry_stack.pool && !native::vfs_continuations.pending_count(f.pid));
    require(!f.thread().wait_registration.active());
    require(f.thread().wait_registration.ticket() == ticket);
    require(f.thread().wait_registration.completion() == (ready ? sched::WaitCompletion::ready : sched::WaitCompletion::cancelled));
    require(native::entry_stacks.accounting().releases == before.releases + 1);
    require(native::retire_native_thread(f.thread()).ok());
    require(native::entry_stacks.accounting().releases == before.releases + 1);
    require(native::prepare_native_thread(f.thread()).ok());
    require(f.thread().entry_stack.slot == lease.slot && f.thread().entry_stack.generation > lease.generation);
    require(native::vfs_continuations.cancel(kernel.scheduler(), handle).code() == StatusCode::not_found);
    held.release(); require(native::vfs_continuations.drain(kernel.scheduler(), kernel.vfs()).ok()); ++cases;
}
void owner_and_cleanup_failures(Kernel &kernel, Provider &provider)
{
    Fixture f(kernel, provider); f.capture(); Pause held(provider); const auto handle = f.begin();
    const auto ticket = native::vfs_continuations.ticket(handle); const auto lease = f.thread().entry_stack;
    const auto releases = native::entry_stacks.accounting().releases;
    native::runtime_kernel = nullptr;
    require(native::retire_native_thread(f.thread()).code() == StatusCode::not_initialized);
    require(f.thread().entry_stack == lease && native::vfs_continuations.pending(f.pid, f.pid) == handle);
    native::runtime_kernel = &kernel;
    auto copied_thread = f.thread(); require(native::retire_native_thread(copied_thread).code() == StatusCode::denied);
    require(f.thread().entry_stack == lease && native::vfs_continuations.pending(f.pid, f.pid) == handle);
    f.process().set_state(sched::ProcessState::zombie);
    require(native::retire_native_thread(f.thread()).code() == StatusCode::fault);
    require(f.thread().entry_stack == lease && native::vfs_continuations.pending(f.pid, f.pid) == handle);
    require(f.thread().wait_registration.pending(ticket));
    require(native::entry_stacks.accounting().releases == releases);
    require(kernel.vfs().cancel_certified_operation_wait(ticket, &native::vfs_continuations).code() == StatusCode::not_found);
    f.process().set_state(sched::ProcessState::blocked);
    require(native::retire_native_thread(f.thread()).ok());
    require(!native::vfs_continuations.pending_count(f.pid) && !f.thread().wait_registration.active());
    require(native::entry_stacks.accounting().releases == releases + 1);
    held.release(); require(native::vfs_continuations.drain(kernel.scheduler(), kernel.vfs()).ok()); ++cases;
}
void replacement_wait(Kernel &kernel, Provider &provider)
{
    Fixture f(kernel, provider); f.capture(); Pause held(provider); const auto handle = f.begin();
    const auto old = native::vfs_continuations.ticket(handle);
    require(kernel.scheduler().complete_wait(old, sched::WaitCompletion::interrupted));
    require(kernel.scheduler().activate_thread(f.pid, f.pid).ok());
    const auto replacement = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::futex, 91); require(replacement.ok());
    require(native::retire_native_thread(f.thread()).ok());
    require(!native::vfs_continuations.pending_count(f.pid) && f.thread().wait_registration.pending(replacement.value()));
    require(f.thread().wait_reason == sched::ThreadWaitReason::futex && f.thread().wait_key == 91);
    held.release(); require(native::vfs_continuations.drain(kernel.scheduler(), kernel.vfs()).ok());
    require(!kernel.scheduler().complete_wait(old, sched::WaitCompletion::cancelled));
    require(kernel.scheduler().complete_wait(replacement.value(), sched::WaitCompletion::cancelled)); ++cases;
}
void group_release(Kernel &kernel, Provider &provider)
{
    Fixture f(kernel, provider); const auto peer = f.peer(); const auto other = f.peer();
    Pause held(provider); std::array<Continuation::Handle, 3> handles{};
    f.capture(); handles[0] = f.begin(); f.capture(peer); handles[1] = f.begin(peer);
    f.capture(other); handles[2] = f.begin(other);
    std::array<sched::WaitTicket, 3> tickets{};
    for (usize i = 0; i < handles.size(); ++i) tickets[i] = native::vfs_continuations.ticket(handles[i]);
    const auto before = native::entry_stacks.accounting(); const auto pages = native::page_pool.used_count();
    const auto physical = f.process().resources().native_pages[0].physical_address;
    native::release_process(kernel, f.pid); require(native::runtime_status().ok());
    require(!native::vfs_continuations.pending_count(f.pid) && f.process().resources().native_pages.empty());
    require(native::page_pool.used_count() < pages && !native::page_pool.handle(physical));
    require(native::entry_stacks.accounting().releases == before.releases + 3);
    for (const auto &thread : f.process().threads()) require(!thread.entry_stack.pool && !thread.wait_registration.active());
    for (auto ticket : tickets) require(!kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
    const auto released_pages = native::page_pool.used_count();
    native::release_process(kernel, f.pid); require(native::entry_stacks.accounting().releases == before.releases + 3);
    require(native::page_pool.used_count() == released_pages);
    held.release(); require(native::vfs_continuations.drain(kernel.scheduler(), kernel.vfs()).ok()); ++cases;
}
void stage_guards(Kernel &kernel, Provider &provider, bool missing)
{
    Fixture f(kernel, provider); f.capture(); Pause held(provider); const auto handle = f.begin();
    const auto ticket = native::vfs_continuations.ticket(handle); const auto lease = f.thread().entry_stack;
    const auto old_registration = f.thread().wait_registration;
    if (missing) f.thread().wait_registration = {};
    else
    {
        require(kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
        require(kernel.scheduler().activate_thread(f.pid, f.pid).ok());
        require(kernel.scheduler().register_current_wait(sched::ThreadWaitReason::futex, 92).ok());
    }
    native::VfsImageRetirement plan;
    require(plan.prepare(kernel.scheduler(), f.process()).code() == StatusCode::fault);
    require(native::vfs_continuations.pending(f.pid, f.pid) == handle && f.thread().entry_stack == lease);
    require(plan.consume(kernel.scheduler(), f.process()).code() == StatusCode::fault);
    if (missing) f.thread().wait_registration = old_registration;
    else require(kernel.scheduler().complete_wait(f.thread().wait_registration.ticket(), sched::WaitCompletion::cancelled));
    require(native::retire_native_thread(f.thread()).ok()); held.release();
    require(native::vfs_continuations.drain(kernel.scheduler(), kernel.vfs()).ok()); ++cases;
}
void image_transaction(Kernel &kernel, Provider &provider, unsigned mode)
{
    Fixture f(kernel, provider); const auto peer = f.peer(); const auto caller = f.peer();
    Pause held(provider); f.capture(); const auto leader_handle = f.begin();
    f.capture(peer); const auto peer_handle = f.begin(peer);
    require(kernel.scheduler().activate_thread(f.pid, caller).ok());
    if (mode == 4) { held.release(); require(native::vfs_continuations.drain(kernel.scheduler(), kernel.vfs()).ok()); }
    const auto leader_lease = f.thread().entry_stack; const auto peer_lease = f.thread(peer).entry_stack;
    const auto caller_lease = f.thread(caller).entry_stack;
    const auto leader_ticket = native::vfs_continuations.ticket(leader_handle);
    const auto peer_ticket = native::vfs_continuations.ticket(peer_handle);
    native::VfsImageRetirement plan; require(plan.prepare(kernel.scheduler(), f.process()).ok());
    require(!plan.prepare(kernel.scheduler(), f.process()));
    require(plan.consume(kernel.scheduler(), f.process()).code() == StatusCode::fault);
    const auto before = native::entry_stacks.accounting(); usize callbacks{};
    sched::WaitTicket replacement{};
    const auto context = f.thread(caller).context; const auto memory = f.process().resources().memory;
    auto replaced = native::replace_entry_stacks(native::entry_stacks, f.process(),
        [&] { return mode == 1 ? Status::denied("hosted pre-commit exec failure") : kernel.posix().exec(f.pid, context, memory); },
        [&] {
            ++callbacks;
            require(native::entry_stacks.accounting().releases == before.releases);
            require(native::entry_stacks.validate_inactive(peer_lease, {f.pid, peer}).ok());
            if (mode == 2) return Status::io_error("hosted post-commit cancellation failure");
            if (mode == 3)
            {
                require(kernel.scheduler().activate_thread(f.pid, f.pid).ok());
                auto wait = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::futex, 93); require(wait.ok()); replacement = wait.value();
            }
            return plan.consume(kernel.scheduler(), f.process());
        });
    if (mode == 1)
    {
        require(!replaced && !replaced.committed && callbacks == 0 && replaced.retained_count == 0);
        require(f.process().threads().size() == 3 && f.thread().entry_stack == leader_lease && f.thread(peer).entry_stack == peer_lease);
        require(f.thread(caller).entry_stack == caller_lease && native::entry_stacks.accounting().releases == before.releases);
        require(native::vfs_continuations.pending(f.pid, f.pid) == leader_handle && native::vfs_continuations.pending(f.pid, peer) == peer_handle);
        require(f.thread().wait_registration.pending(leader_ticket) && f.thread(peer).wait_registration.pending(peer_ticket));
    }
    else if (mode == 2 || mode == 3)
    {
        require(!replaced && replaced.committed && callbacks == 1 && replaced.retained_count == 2);
        require(f.process().threads().size() == 1 && f.thread().entry_stack == leader_lease);
        require(native::entry_stacks.accounting().releases == before.releases && native::vfs_continuations.pending_count(f.pid) == 2);
        for (usize i = 0; i < replaced.retained_count; ++i)
            require(native::entry_stacks.validate_inactive(replaced.retained[i].handle, replaced.retained[i].owner).ok());
        if (mode == 2) require(plan.consume(kernel.scheduler(), f.process()).ok());
        else
        {
            require(f.thread().wait_registration.pending(replacement));
            require(native::vfs_continuations.cancel_process(kernel.scheduler(), f.pid).ok());
            require(f.thread().wait_registration.pending(replacement));
            require(kernel.scheduler().complete_wait(replacement, sched::WaitCompletion::cancelled));
            require(plan.consume(kernel.scheduler(), f.process()).code() == StatusCode::fault);
        }
        for (usize i = 0; i < replaced.retained_count; ++i)
            require(native::entry_stacks.release(replaced.retained[i].handle, replaced.retained[i].owner).ok());
    }
    else
    {
        require(replaced && replaced.committed && callbacks == 1 && replaced.retained_count == 0);
        require(f.process().threads().size() == 1 && f.thread().entry_stack == leader_lease);
        require(native::entry_stacks.accounting().releases == before.releases + 2);
        require(!native::vfs_continuations.pending_count(f.pid));
        require(native::vfs_continuations.cancel(kernel.scheduler(), leader_handle).code() == StatusCode::not_found);
        require(native::vfs_continuations.cancel(kernel.scheduler(), peer_handle).code() == StatusCode::not_found);
        require(plan.consume(kernel.scheduler(), f.process()).code() == StatusCode::fault);
        native::ExecRetirement retirement{f.process(), f.pid}; retirement.consume(kernel, f.process());
        native::Image image{}; image.personality = sched::UserPersonality::linux;
        image.space = f.process().resources().native_space; ++image.space.generation;
        image.frame = hw::initial_frame(hw::user_begin + 1024, hw::user_begin + 4096);
        require(native::allocate(image.pages, hw::user_begin, hw::page_read | hw::page_execute).ok());
        native::release(f.process().resources().native_space, f.process().resources().native_pages);
        require(native::install(f.process(), image).ok());
        require(f.thread().entry_stack == leader_lease && !f.thread().syscall_pending);
    }
    held.release(); require(native::vfs_continuations.drain(kernel.scheduler(), kernel.vfs()).ok()); ++cases;
}
void retired_leader_transaction(Kernel &kernel, Provider &provider, bool failed)
{
    Fixture f(kernel, provider); const auto peer = f.peer(); const auto caller = f.peer();
    const auto old_leader = f.thread().entry_stack;
    require(native::retire_native_thread(f.thread()).ok()); require(kernel.scheduler().exit_thread(f.pid, f.pid).ok());
    Pause held(provider); f.capture(peer); const auto handle = f.begin(peer);
    require(kernel.scheduler().activate_thread(f.pid, caller).ok());
    native::VfsImageRetirement plan; require(plan.prepare(kernel.scheduler(), f.process()).ok());
    const auto before = native::entry_stacks.accounting(); usize callbacks{};
    const auto context = f.thread(caller).context; const auto memory = f.process().resources().memory;
    const auto replaced = native::replace_entry_stacks(native::entry_stacks, f.process(),
        [&] { return failed ? Status::denied("hosted retired-leader failed exec") : kernel.posix().exec(f.pid, context, memory); },
        [&] { ++callbacks; return plan.consume(kernel.scheduler(), f.process()); });
    if (failed)
    {
        require(!replaced && !replaced.committed && !callbacks && f.process().threads().size() == 3);
        require(!f.thread().entry_stack.pool && native::vfs_continuations.pending(f.pid, peer) == handle);
        require(native::entry_stacks.accounting().reservations == before.reservations + 1);
        require(native::entry_stacks.accounting().releases == before.releases + 1);
    }
    else
    {
        require(replaced && replaced.committed && callbacks == 1 && !native::vfs_continuations.pending_count(f.pid));
        require(f.process().threads().size() == 1 && f.thread().entry_stack.pool && f.thread().entry_stack != old_leader);
        require(native::entry_stacks.accounting().reservations == before.reservations + 1);
        require(native::entry_stacks.accounting().releases == before.releases + 2);
    }
    held.release(); require(native::vfs_continuations.drain(kernel.scheduler(), kernel.vfs()).ok()); ++cases;
}
#endif
} // namespace

int main()
{
    static Kernel kernel; require(kernel.boot({}).ok()); require(native::entry_stacks.activate(native::entry_stack_arena).ok());
    static Provider provider; provider.vfs = &kernel.vfs();
    require(kernel.vfs().create("/tmp/lifecycle-provider", fs::NodeType::directory).ok());
    require(kernel.vfs().mount("/tmp/lifecycle-provider", provider).ok());
#ifdef OK_VFS_LIFECYCLE_PREDECESSOR
    predecessor(kernel, provider);
    std::printf("PASS actual runtime VFS lifecycle predecessor checks=%zu retire_reacquire_busy=%zu hosted=1 guest=0\n", checks, cases);
#else
    retirement(kernel, provider, false); retirement(kernel, provider, true);
    owner_and_cleanup_failures(kernel, provider); replacement_wait(kernel, provider); group_release(kernel, provider);
    stage_guards(kernel, provider, false); stage_guards(kernel, provider, true);
    for (unsigned mode = 0; mode < 5; ++mode) image_transaction(kernel, provider, mode);
    retired_leader_transaction(kernel, provider, false); retired_leader_transaction(kernel, provider, true);
    std::printf("PASS actual runtime VFS lifecycle successor checks=%zu cases=%zu hosted=1 guest=0\n", checks, cases);
#endif
}
