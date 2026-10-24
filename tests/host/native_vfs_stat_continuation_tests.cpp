#include "ok/user/native_vfs_stat_continuation.hpp"
#include "ok/user/native_entry_stacks.hpp"
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <type_traits>

using namespace ok;
namespace hw = arch::execution;
namespace lu = abi::linux_user;
namespace native = user::native;
static usize checks, hosted_entries, request_copies, output_copies, deferred_cases, rearm_cases;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line=%d check=%zu: %s\n", __LINE__, checks, #x); std::abort(); } } while (false)
namespace mock { bool invalid_stack{}; }
namespace ok::arch::execution
{
Result<Trap> enter_owned(AddressSpace &, UserFrame &, const KernelEntryStackView &view)
{
    ++hosted_entries;
    return Trap{.kind = TrapKind::syscall, .kernel_stack_pointer = mock::invalid_stack ? 0 : view.top() - 128};
}
Status destroy_space(AddressSpace &space) { space = {}; return {}; }
}
static_assert(!std::is_copy_constructible_v<fs::VirtualFileSystem::OperationContention>);
static_assert(!std::is_move_assignable_v<fs::VirtualFileSystem::OperationContention>);
static_assert(!std::is_constructible_v<fs::VirtualFileSystem::OperationContention, const fs::VirtualFileSystem &, u64>);
static_assert(!std::is_copy_constructible_v<native::VfsStatContinuation<lu::Profile::x86_64>>);
static_assert(static_cast<u8>(sched::ThreadWaitReason::vfs_operation) == 10);

struct Provider final : fs::Node
{
    fs::VirtualFileSystem *vfs{};
    mutable std::atomic<bool> pause_next{}, entered{}, release_pause{};
    mutable usize callbacks{};
    bool fail_busy{}, fail_release{};
    std::string_view name() const override { return "provider"; }
    fs::Metadata metadata() const override
    {
        // Synthetic complete provider fields exercise the real strict stat codec.
        const fs::EpochTimestamp stamp{123456, 7, fs::TimestampSource::readonly_ext4};
        return {.type = fs::NodeType::directory, .size = 321, .mode = fs::mode_for(fs::NodeType::directory, 0755),
            .blocks = 1, .attributes = {.inode = 7, .valid = 63, .access = stamp, .modification = stamp, .change = stamp}};
    }
    Result<fs::Metadata> query_metadata() const override
    {
        ++callbacks;
        // This callback takes the public short VFS memory lock; qualification
        // timeout detects accidental lock retention around provider calls.
        static_cast<void>(vfs->mode());
        if (pause_next.exchange(false))
        {
            entered = true;
            while (!release_pause.load()) std::this_thread::yield();
        }
        return fail_busy ? Result<fs::Metadata>(Status::busy("VFS operation requires deferred retry")) : Result<fs::Metadata>(metadata());
    }
    Status release() override { return fail_release ? Status::busy("VFS operation requires deferred retry") : Status{}; }
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
    Pause(fs::VirtualFileSystem &vfs, Provider &node) : provider(node)
    {
        provider.entered = false; provider.release_pause = false; provider.pause_next = true;
        holder = std::thread([&vfs, &node] { const auto result = vfs.query_metadata(node); if (!result) std::abort(); });
        while (!provider.entered.load()) std::this_thread::yield();
    }
    void release() { provider.release_pause = true; if (holder.joinable()) holder.join(); }
    ~Pause() { release(); }
};
struct alignas(4096) Backing { std::array<std::byte, memory::KernelEntryStackPool::arena_bytes> bytes; };
template <lu::Profile P> struct Fixture
{
    using Adapter = native::VfsStatContinuation<P>;
    using Handle = typename Adapter::Handle;
    std::unique_ptr<Backing> backing{std::make_unique<Backing>()};
    memory::KernelEntryStackPool pool;
    std::unique_ptr<sched::Scheduler> scheduler{std::make_unique<sched::Scheduler>()};
    std::unique_ptr<fs::VirtualFileSystem> vfs{std::make_unique<fs::VirtualFileSystem>()};
    std::unique_ptr<Adapter> adapter;
    Provider provider;
    sched::ProcessId earlier{}, pid{};
    FixedString<256> user_path{"/tmp/provider"};
    u64 user_output = hw::user_begin + 1024;
    usize reads{}, stack_reads{}, admits{}, copies{};
    bool writable = true, copy_fails{};
    std::array<u8, 256> copied{};
    u64 output_address{};
    Fixture()
    {
        CHECK(pool.activate(backing->bytes)); adapter = std::make_unique<Adapter>(pool);
        const auto context = arch::configured_arch_operations().make_user_context({hw::user_begin, hw::user_begin + 8192});
        auto first = scheduler->create_process("earlier", context); CHECK(first); earlier = first.value();
        auto created = scheduler->create_user_process("vfs-stat-owner", context); CHECK(created); pid = created.value();
        auto *process = scheduler->find(pid);
        process->resources().personality = sched::UserPersonality::linux;
        process->resources().native_space = {0x100000, 1, 1};
        CHECK(native::prepare_entry_stack(pool, thread())); thread().native_active = true;
        CHECK(adapter->prepare(thread())); CHECK(scheduler->set_runnable(pid)); CHECK(scheduler->activate_thread(pid, pid));
        provider.vfs = vfs.get(); CHECK(vfs->create("/tmp/provider", fs::NodeType::directory)); CHECK(vfs->mount("/tmp/provider", provider));
    }
    ~Fixture()
    {
        CHECK(adapter->cancel_process(*scheduler, pid));
        CHECK(adapter->retire(thread()));
        const auto ownership = adapter->ownership();
        CHECK(!ownership.prepared && !ownership.pending && !ownership.captured);
        if (thread().entry_stack.pool) CHECK(native::retire_entry_stack(pool, thread()));
    }
    sched::ThreadControlBlock &thread() { return *scheduler->find_thread(pid, pid); }
    hw::UserFrame frame()
    {
        hw::UserFrame f{}; f.pc = hw::user_begin + 200; f.sp = hw::user_begin + 8192;
        f.fs_base = 3; f.gs_base = 4; f.fp_state[19] = 85;
        const auto number = *lu::Codec<P>::syscall_number(lu::Operation::statx);
        const uptr fd = static_cast<uptr>(static_cast<u64>(static_cast<i64>(posix::at_FDCWD)) & lu::Codec<P>::word_mask);
        const uptr path = hw::user_begin + 512, flags = 0, mask = 0x7ff, output = static_cast<uptr>(user_output);
        if constexpr (P == lu::Profile::x86_64)
        { f.registers[14] = number; f.registers[9] = fd; f.registers[10] = path; f.registers[12] = flags; f.registers[5] = mask; f.registers[7] = output; }
        else if constexpr (P == lu::Profile::i386)
        { f.registers[7] = number; f.registers[4] = fd; f.registers[6] = path; f.registers[5] = flags; f.registers[1] = mask; f.registers[0] = output; }
        else if constexpr (P == lu::Profile::aarch64 || P == lu::Profile::arm32)
        { f.registers[P == lu::Profile::arm32 ? 7 : 8] = number; f.registers[0] = fd; f.registers[1] = path; f.registers[2] = flags; f.registers[3] = mask; f.registers[4] = output; }
        else if constexpr (P == lu::Profile::rv32 || P == lu::Profile::rv64)
        { f.registers[17] = number; f.registers[10] = fd; f.registers[11] = path; f.registers[12] = flags; f.registers[13] = mask; f.registers[14] = output; }
        else if constexpr (P == lu::Profile::loongarch64)
        { f.registers[11] = number; f.registers[4] = fd; f.registers[5] = path; f.registers[6] = flags; f.registers[7] = mask; f.registers[8] = output; }
        else if constexpr (P == lu::Profile::mips || P == lu::Profile::mips64)
        { f.registers[2] = number; f.registers[4] = fd; f.registers[5] = path; f.registers[6] = flags; f.registers[7] = mask; f.registers[8] = output; }
        else
        { f.registers[0] = number; f.registers[3] = fd; f.registers[4] = path; f.registers[5] = flags; f.registers[6] = mask; f.registers[7] = output; }
        return f;
    }
    void capture()
    {
        CHECK(scheduler->activate_thread(pid, pid)); thread().syscall_pending = false; thread().native_frame = frame();
        auto view = pool.activate_entry(thread().entry_stack, {pid, pid}, 0); CHECK(view);
        auto trap = adapter->enter(*scheduler, *scheduler->find(pid), thread(), view.value()); CHECK(trap);
        CHECK(pool.deactivate_entry(thread().entry_stack, {pid, pid}, 0)); thread().syscall_pending = true;
    }
    auto admit() { return [&](u64 address, usize count) { ++admits; return writable && address == hw::user_begin + 1024 && count == 256; }; }
    auto copy() { return [&](u64 address, std::span<const u8> bytes) { ++copies; ++output_copies; output_address = address; if (copy_fails) return Status::fault("injected guarded copy failure"); std::copy(bytes.begin(), bytes.end(), copied.begin()); return Status{}; }; }
    i64 begin(bool &complete)
    {
        return adapter->begin(*scheduler, *vfs, pid, pid,
            [&](u64, FixedString<256> &path) { ++reads; ++request_copies; return path.assign(user_path.view()); },
            [&](uptr address, std::span<u8> bytes) {
                ++stack_reads; CHECK(address == thread().native_frame.sp + 16 && bytes.size() == 4);
                for (usize i = 0; i < 4; ++i) bytes[i] = static_cast<u8>(user_output >> (8 * (lu::Codec<P>::profile.big_endian ? 3 - i : i)));
                return Status{};
            }, admit(), copy(), complete);
    }
    i64 resume(Handle h, bool &complete)
    { CHECK(scheduler->activate_thread(pid, pid)); return adapter->resume(*scheduler, h, admit(), copy(), complete); }
};

template <lu::Profile P> void copied_request_and_rearm()
{
    Fixture<P> f; f.capture(); const auto pc = f.thread().native_frame.pc;
    Pause held(*f.vfs, f.provider); bool complete = true;
    CHECK(f.begin(complete) == -11 && !complete); ++deferred_cases;
    const auto handle = f.adapter->pending(f.pid, f.pid); const auto ticket = f.adapter->ticket(handle);
    CHECK(handle.request && f.thread().syscall_pending && f.thread().state == sched::ProcessState::blocked);
    CHECK(f.adapter->ownership().prepared == 1 && f.adapter->ownership().pending == 1);
    CHECK(!f.adapter->retire(f.thread()));
    CHECK(f.adapter->pending(f.pid, f.pid) == handle);
    CHECK(f.thread().wait_reason == sched::ThreadWaitReason::vfs_operation && f.thread().wait_registration.active());
    CHECK(f.reads == 1 && f.copies == 0 && f.admits == 0);
    CHECK(!f.adapter->prepare(f.thread())); CHECK(f.user_path.assign("/tmp/wrong-after-block")); f.user_output += 256;
    held.release(); CHECK(f.adapter->drain(*f.scheduler, *f.vfs));
    CHECK(f.thread().state == sched::ProcessState::runnable && !f.thread().wait_registration.active());
    CHECK(f.thread().wait_registration.completion() == sched::WaitCompletion::ready && f.thread().syscall_pending);
    {
        Pause successor(*f.vfs, f.provider); complete = true;
        CHECK(f.resume(handle, complete) == -11 && !complete); ++rearm_cases;
        const auto next = f.adapter->ticket(handle); CHECK(next.generation > ticket.generation);
        CHECK(!f.scheduler->complete_wait(ticket, sched::WaitCompletion::ready));
        CHECK(f.vfs->cancel_certified_operation_wait(ticket, f.adapter.get()).code() == StatusCode::not_found);
        CHECK(f.adapter->pending(f.pid, f.pid) == handle && f.reads == 1 && f.copies == 0);
        successor.release(); CHECK(f.adapter->drain(*f.scheduler, *f.vfs));
    }
    complete = true; const auto resumed = f.resume(handle, complete);
    if (resumed != 0) std::fprintf(stderr, "profile=%u result=%lld admits=%zu copies=%zu\n", static_cast<unsigned>(P), static_cast<long long>(resumed), f.admits, f.copies);
    CHECK(resumed == 0 && complete);
    CHECK(f.reads == 1 && f.copies == 1 && f.output_address == hw::user_begin + 1024);
    CHECK(f.stack_reads == (P == lu::Profile::mips ? 1U : 0U)); CHECK(f.thread().native_frame.pc == pc);
    CHECK(!f.adapter->pending(f.pid, f.pid).request && f.adapter->ticket(handle).generation == 0);
    CHECK(f.adapter->cancel(*f.scheduler, handle).code() == StatusCode::not_found);
}

void terminal_and_lifecycle_guards()
{
    using F = Fixture<lu::Profile::x86_64>;
    {
        F f; f.capture(); f.provider.fail_busy = true; const auto before = f.provider.callbacks; bool complete = true;
        CHECK(f.begin(complete) == -16 && complete); CHECK(f.provider.callbacks > before);
        CHECK(!f.adapter->pending(f.pid, f.pid).request && !f.thread().wait_registration.active() && f.copies == 0);
        f.provider.fail_busy = false;
    }
    {
        F f; f.capture(); f.provider.fail_release = true; bool complete = true;
        CHECK(f.begin(complete) == -16 && complete); CHECK(!f.adapter->pending(f.pid, f.pid).request && f.copies == 0);
        f.provider.fail_release = false;
    }
    {
        F f; f.capture(); Pause held(*f.vfs, f.provider); bool complete = true; CHECK(f.begin(complete) == -11 && !complete);
        const auto handle = f.adapter->pending(f.pid, f.pid); f.writable = false;
        held.release(); CHECK(f.adapter->drain(*f.scheduler, *f.vfs)); complete = true;
        CHECK(f.resume(handle, complete) == -14 && complete && f.copies == 0 && f.reads == 1);
    }
    {
        F f; f.capture(); Pause held(*f.vfs, f.provider); bool complete = true; CHECK(f.begin(complete) == -11 && !complete);
        const auto handle = f.adapter->pending(f.pid, f.pid); const auto ticket = f.adapter->ticket(handle);
        CHECK(f.adapter->cancel(*f.scheduler, handle)); CHECK(f.thread().state == sched::ProcessState::runnable);
        CHECK(f.scheduler->activate_thread(f.pid, f.pid)); auto replacement = f.scheduler->register_current_wait(sched::ThreadWaitReason::futex, 99); CHECK(replacement);
        CHECK(f.adapter->cancel(*f.scheduler, handle).code() == StatusCode::not_found);
        CHECK(!f.scheduler->complete_wait(ticket, sched::WaitCompletion::cancelled));
        CHECK(f.thread().wait_reason == sched::ThreadWaitReason::futex && f.thread().wait_key == 99);
        held.release(); CHECK(f.adapter->drain(*f.scheduler, *f.vfs)); CHECK(f.thread().wait_registration.pending(replacement.value()));
        CHECK(f.scheduler->complete_wait(replacement.value(), sched::WaitCompletion::ready));
    }
    {
        F f; f.capture(); Pause held(*f.vfs, f.provider); bool complete = true; CHECK(f.begin(complete) == -11 && !complete);
        const auto handle = f.adapter->pending(f.pid, f.pid);
        CHECK(f.scheduler->exit_process(f.earlier, 0)); CHECK(f.scheduler->reap_process(f.earlier));
        held.release(); CHECK(f.adapter->drain(*f.scheduler, *f.vfs)); complete = true;
        CHECK(f.resume(handle, complete) == 0 && complete && f.reads == 1);
    }
    for (int change = 0; change < 5; ++change)
    {
        F f; f.capture(); Pause held(*f.vfs, f.provider); bool complete = true; CHECK(f.begin(complete) == -11 && !complete);
        const auto handle = f.adapter->pending(f.pid, f.pid); const auto old = f.adapter->ticket(handle);
        if (change == 0) ++f.scheduler->find(f.pid)->resources().native_space.generation;
        if (change == 1) ++f.thread().native_frame.fp_state[17];
        if (change == 2) f.thread().syscall_pending = false;
        if (change == 3) { CHECK(f.scheduler->complete_wait(old, sched::WaitCompletion::ready)); CHECK(f.scheduler->activate_thread(f.pid, f.pid)); CHECK(f.scheduler->register_current_wait(sched::ThreadWaitReason::futex, 88)); }
        if (change == 4) { auto view = f.pool.activate_entry(f.thread().entry_stack, {f.pid, f.pid}, 0); CHECK(view); CHECK(f.pool.deactivate_entry(f.thread().entry_stack, {f.pid, f.pid}, 0)); }
        CHECK(f.adapter->prune(*f.scheduler)); CHECK(!f.adapter->pending(f.pid, f.pid).request);
        if (change == 3) { CHECK(f.thread().wait_reason == sched::ThreadWaitReason::futex && f.thread().wait_key == 88); CHECK(f.scheduler->complete_wait(f.thread().wait_registration.ticket(), sched::WaitCompletion::ready)); }
        else CHECK(!f.thread().wait_registration.active());
        held.release(); CHECK(f.adapter->drain(*f.scheduler, *f.vfs));
    }
    {
        F f; f.capture(); Pause held(*f.vfs, f.provider); std::array<sched::WaitTicket, fs::VirtualFileSystem::operation_wait_capacity> fill{};
        for (usize i = 0; i < fill.size(); ++i) { fill[i] = {1000 + i, 1000 + i, 1}; CHECK(f.vfs->queue_operation_wait(fill[i])); }
        bool complete = true; CHECK(f.begin(complete) == -12 && complete); CHECK(!f.adapter->pending(f.pid, f.pid).request);
        CHECK(!f.thread().wait_registration.active() && f.thread().state == sched::ProcessState::runnable && f.copies == 0);
        for (auto ticket : fill) CHECK(f.vfs->cancel_operation_wait(ticket));
        held.release();
    }
    {
        F f; auto &t = f.thread(); auto view = f.pool.activate_entry(t.entry_stack, {f.pid, f.pid}, 0); CHECK(view);
        auto copied = t; CHECK(!f.adapter->enter(*f.scheduler, *f.scheduler->find(f.pid), copied, view.value()));
        mock::invalid_stack = true; CHECK(!f.adapter->enter(*f.scheduler, *f.scheduler->find(f.pid), t, view.value())); mock::invalid_stack = false;
        CHECK(f.pool.deactivate_entry(t.entry_stack, {f.pid, f.pid}, 0)); t.syscall_pending = true;
        bool complete = true; CHECK(f.begin(complete) == -125 && complete && f.reads == 0);
    }
}

void certificate_and_consumer_guards()
{
    Fixture<lu::Profile::x86_64> f;
    auto other = std::make_unique<fs::VirtualFileSystem>();
    int owner, foreign;
    const sched::WaitTicket ticket{100, 100, 1};
    {
        Pause held(*f.vfs, f.provider);
        auto admission = f.vfs->try_stat_absolute("/tmp/provider", {}); CHECK(admission.contention.valid());
        auto moved = std::move(admission.contention); CHECK(moved.valid() && !admission.contention.valid());
        CHECK(!other->queue_certified_operation_wait(std::move(moved), ticket, &owner)); CHECK(!moved.valid());
        auto again = f.vfs->try_stat_absolute("/tmp/provider", {}); CHECK(again.contention.valid());
        auto queued = f.vfs->queue_certified_operation_wait(std::move(again.contention), ticket, &owner); CHECK(queued && queued.value());
        CHECK(!again.contention.valid()); CHECK(!f.vfs->queue_certified_operation_wait(std::move(again.contention), {100,100,2}, &owner));
        CHECK(f.vfs->cancel_operation_wait(ticket).code() == StatusCode::not_found);
        CHECK(f.vfs->cancel_certified_operation_wait(ticket, &foreign).code() == StatusCode::not_found);
        held.release(); CHECK(!f.vfs->take_ready_operation_wait()); CHECK(!f.vfs->take_ready_certified_operation_wait(&foreign));
        CHECK(f.adapter->drain(*f.scheduler, *f.vfs)); auto ready = f.vfs->take_ready_certified_operation_wait(&owner); CHECK(ready && ready.value() == ticket);
    }
    {
        Pause held(*f.vfs, f.provider); auto admission = f.vfs->try_stat_absolute("/tmp/provider", {}); CHECK(admission.contention.valid());
        held.release(); auto queued = f.vfs->queue_certified_operation_wait(std::move(admission.contention), ticket, &owner);
        CHECK(queued && !queued.value() && !admission.contention.valid());
    }
    {
        Pause held(*f.vfs, f.provider); auto admission = f.vfs->try_stat_absolute("/tmp/provider", {}); CHECK(admission.contention.valid());
        held.release(); Pause successor(*f.vfs, f.provider);
        auto queued = f.vfs->queue_certified_operation_wait(std::move(admission.contention), ticket, &owner);
        CHECK(queued && !queued.value()); successor.release();
    }
    f.provider.fail_busy = true; auto provider_error = f.vfs->try_stat_absolute("/tmp/provider", {});
    CHECK(!provider_error.result && provider_error.result.status().code() == StatusCode::busy && !provider_error.contention.valid());
    f.provider.fail_busy = false;
}
int main()
{
    copied_request_and_rearm<lu::Profile::i386>(); copied_request_and_rearm<lu::Profile::x86_64>();
    copied_request_and_rearm<lu::Profile::aarch64>(); copied_request_and_rearm<lu::Profile::arm32>();
    copied_request_and_rearm<lu::Profile::rv64>(); copied_request_and_rearm<lu::Profile::rv32>();
    copied_request_and_rearm<lu::Profile::loongarch64>(); copied_request_and_rearm<lu::Profile::mips>();
    copied_request_and_rearm<lu::Profile::mips64>(); copied_request_and_rearm<lu::Profile::ppc>();
    terminal_and_lifecycle_guards(); certificate_and_consumer_guards();
    std::printf("PASS owned VFS stat continuation checks=%zu profiles=10 deferred=%zu rearmed=%zu request_copies=%zu output_copies=%zu hosted_entries=%zu guest=0 kernel_stack_parking=0 signal_restart=0\n", checks, deferred_cases, rearm_cases, request_copies, output_copies, hosted_entries);
    return 0;
}
