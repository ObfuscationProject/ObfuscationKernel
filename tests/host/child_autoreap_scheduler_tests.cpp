#include "ok/sched/scheduler.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace ok;
using namespace ok::sched;
static usize checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::abort(); } } while (false)

namespace {
usize before_calls, after_calls, fail_before, fail_after;
constexpr std::string_view before_failure = "injected pre-erase relocation failure";
constexpr std::string_view after_failure = "injected post-erase relocation failure";
}
namespace ok::user::native {
Status private_signal_before_process_compaction(sched::SchedulerState &)
{
    return ++before_calls == fail_before ? Status::fault(before_failure) : Status::success();
}
Status private_signal_after_process_compaction(sched::SchedulerState &)
{
    return ++after_calls == fail_after ? Status::io_error(after_failure) : Status::success();
}
}

struct Fixture
{
    struct MetadataBacking
    {
        alignas(user::native::NativePagePool::page_size)
            std::array<std::byte, user::native::NativePagePool::page_size> bytes{};
        memory::FrameAllocator frames;
        memory::FrameMetadataStorage<2048> frames_metadata;
        user::native::NativePagePool pool;
    };
    std::unique_ptr<MetadataBacking> metadata;
    std::unique_ptr<Scheduler> storage{std::make_unique<Scheduler>()};
    Scheduler &scheduler{*storage};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(0x1000, 0x8000)};
    Fixture() { before_calls = after_calls = fail_before = fail_after = 0; }
    ProcessId process()
    {
        const auto result = scheduler.create_process("autoreap", context);
        CHECK(result); CHECK(scheduler.set_runnable(result.value())); return result.value();
    }
    ProcessId child(ProcessId parent)
    {
        CHECK(scheduler.activate(parent));
        const auto result = scheduler.fork_process(parent);
        CHECK(result); return result.value();
    }
    void finish(ProcessId child, i32 code = 23)
    { CHECK(scheduler.exit_process(child, code)); }
    void retain_mapping(ProcessResources &resources)
    {
        metadata = std::make_unique<MetadataBacking>();
        const memory::MemoryRegion region{reinterpret_cast<uptr>(metadata->bytes.data()),
            metadata->bytes.size(), memory::RegionType::usable};
        CHECK(metadata->frames.initialize({&region, 1}, user::native::NativePagePool::page_size, metadata->frames_metadata.bytes));
        CHECK(metadata->pool.activate(metadata->frames, user::native::NativePagePool::Source::linker_reserved, 1));
        CHECK(resources.native_pages.reserve(metadata->pool, 1));
        CHECK(resources.native_pages.push_back({}));
    }
};

struct StorageSnapshot
{
    struct Task { ProcessId pid; ThreadId tid; const void *address; };
    std::array<Task, max_processes> processes{};
    std::array<Task, max_processes * max_threads_per_process> threads{};
    usize process_count{}, thread_count{};
    explicit StorageSnapshot(const Scheduler &scheduler)
    {
        for (const auto &process : scheduler.processes()) {
            processes[process_count++] = {process.pid(), 0, &process};
            for (const auto &thread : process.threads())
                threads[thread_count++] = {process.pid(), thread.tid, &thread};
        }
    }
    void check(Scheduler &scheduler) const
    {
        CHECK(scheduler.process_count() == process_count);
        for (usize i = 0; i < process_count; ++i)
            CHECK(scheduler.find(processes[i].pid) == processes[i].address);
        for (usize i = 0; i < thread_count; ++i)
            CHECK(scheduler.find_thread(threads[i].pid, threads[i].tid) == threads[i].address);
    }
};

template <typename Action>
void rejected_without_mutation(Scheduler &scheduler, Action action)
{
    auto image = std::make_unique<std::array<unsigned char, sizeof(Scheduler)>>();
    std::memcpy(image->data(), &scheduler, sizeof(scheduler));
    const auto before = before_calls, after = after_calls;
    CHECK(!action());
    CHECK(std::memcmp(image->data(), &scheduler, sizeof(scheduler)) == 0);
    CHECK(before_calls == before && after_calls == after);
}

void detach_lifetime_and_waiters()
{
    Fixture f;
    const auto parent = f.process(), first = f.child(parent), child = f.child(parent), last = f.child(parent);
    const auto grandchild = f.child(child);
    const auto peer = f.scheduler.create_thread(parent, f.context); CHECK(peer);
    CHECK(f.scheduler.create_thread(child, f.context));
    CHECK(f.scheduler.activate_thread(parent, parent));
    const auto selected = f.scheduler.register_current_wait(ThreadWaitReason::child, child); CHECK(selected);
    CHECK(f.scheduler.activate_thread(parent, peer.value()));
    const auto any = f.scheduler.register_current_wait(ThreadWaitReason::child, 0); CHECK(any);
    f.finish(child, 257);
    CHECK(f.scheduler.find(grandchild)->parent_pid() == 0);
    CHECK(f.scheduler.find(child)->children().empty());
    const auto wait_status = f.scheduler.find(child)->wait_status();
    CHECK(wait_status == 256);
    CHECK(f.scheduler.find_thread(parent, parent)->wait_registration.completion() == WaitCompletion::ready);
    CHECK(f.scheduler.find_thread(parent, peer.value())->wait_registration.completion() == WaitCompletion::ready);
    StorageSnapshot snapshot(f.scheduler);
    CHECK(f.scheduler.detach_completed_child_to_reaper(parent, child));
    snapshot.check(f.scheduler);
    CHECK(f.scheduler.find(child)->state() == ProcessState::zombie);
    CHECK(f.scheduler.find(child)->parent_pid() == 0);
    CHECK(f.scheduler.find(child)->exit_code() == 257 && f.scheduler.find(child)->wait_status() == wait_status);
    CHECK(f.scheduler.find(parent)->children().size() == 2);
    CHECK(f.scheduler.find(parent)->children()[0] == first && f.scheduler.find(parent)->children()[1] == last);
    CHECK(f.scheduler.find(grandchild)->state() == ProcessState::runnable);
    CHECK(!f.scheduler.wait_process(parent, child));
    CHECK(before_calls == 0 && after_calls == 0);
    rejected_without_mutation(f.scheduler, [&] { return f.scheduler.detach_completed_child_to_reaper(parent, child); });
    OrphanReapProgress progress{99, 99, true};
    CHECK(f.scheduler.reap_orphans_checked(progress));
    CHECK(progress.reaped == 1 && !progress.failed_pid && !progress.failed_after_erase);
    CHECK(!f.scheduler.find(child) && f.scheduler.find(grandchild));
    CHECK(before_calls == 1 && after_calls == 1);
    CHECK(!f.scheduler.complete_wait(selected.value(), WaitCompletion::interrupted));
    CHECK(!f.scheduler.complete_wait(any.value(), WaitCompletion::interrupted));
    CHECK(f.scheduler.reap_orphans_checked(progress));
    CHECK(progress.reaped == 0 && !progress.failed_pid && !progress.failed_after_erase);
}

void signal_status_and_completed_state()
{
    Fixture f;
    const auto parent = f.process(), child = f.child(parent);
    CHECK(f.scheduler.terminate_process(child, 128));
    f.scheduler.find(child)->set_state(ProcessState::exited);
    const auto status = f.scheduler.find(child)->wait_status();
    CHECK(status == 128);
    StorageSnapshot snapshot(f.scheduler);
    CHECK(f.scheduler.detach_completed_child_to_reaper(parent, child));
    snapshot.check(f.scheduler);
    CHECK(f.scheduler.find(child)->wait_status() == status);
    CHECK(f.scheduler.find(parent)->children().empty());
}

void detach_rejection_matrix()
{
    for (unsigned mode = 0; mode != 28; ++mode) {
        Fixture f;
        const auto parent = f.process(), child = f.child(parent), other = f.process();
        f.finish(child);
        auto *owner = f.scheduler.find(parent);
        auto *process = f.scheduler.find(child);
        auto &thread = process->threads()[0];
        ProcessId request_parent = parent, request_child = child;
        switch (mode) {
        case 0: request_parent = 0; break;
        case 1: request_child = 0; break;
        case 2: request_parent = max_task_id; break;
        case 3: request_child = max_task_id; break;
        case 4: request_parent = child; break;
        case 5: process->set_parent(other); break;
        case 6: CHECK(owner->remove_child(child)); break;
        case 7: CHECK(owner->children().push_back(child)); break;
        case 8: process->set_state(ProcessState::runnable); break;
        case 9: process->set_state(ProcessState::created); break;
        case 10: owner->set_state(ProcessState::zombie); break;
        case 11: process->resources().native_space.root = 0x1000; break;
        case 12: f.retain_mapping(process->resources()); break;
        case 13: thread.entry_stack.pool = reinterpret_cast<memory::KernelEntryStackPool *>(0x1000); break;
        case 14: thread.kernel_stack_top = 0x2000; break;
        case 15: thread.state = ProcessState::runnable; break;
        case 16: thread.native_active = true; break;
        case 17: thread.syscall_pending = true; break;
        case 18: CHECK(thread.wait_registration.arm(child, child)); break;
        case 19: CHECK(thread.native_deadline.arm(10, 0, 20, false)); break;
        case 20: thread.wait_reason = ThreadWaitReason::child; break;
        case 21: thread.wait_key = 12; break;
        case 22: CHECK(process->resources().files.open_slot(9)); break;
        case 23: CHECK(process->resources().memory.add_area({.base = 0x1000, .length = 0x1000})); break;
        case 24: process->resources().mappings[0].used = true; break;
        case 25: process->resources().cwd_node = reinterpret_cast<fs::Node *>(0x1000); break;
        case 26: CHECK(process->children().push_back(other)); break;
        case 27: CHECK(f.scheduler.find(other)->children().push_back(child)); break;
        }
        rejected_without_mutation(f.scheduler, [&] {
            return f.scheduler.detach_completed_child_to_reaper(request_parent, request_child);
        });
    }
    Fixture f;
    const auto parent = f.process(), child = f.child(parent);
    f.finish(child);
    auto *process = f.scheduler.find(child);
    process->set_state(ProcessState::runnable);
    process->threads()[0].state = ProcessState::runnable;
    CHECK(f.scheduler.activate(child));
    process->set_state(ProcessState::zombie);
    process->threads()[0].state = ProcessState::exited;
    rejected_without_mutation(f.scheduler, [&] { return f.scheduler.detach_completed_child_to_reaper(parent, child); });
}

void checked_reap_failures_and_legacy_behavior()
{
    {
        Fixture f;
        const auto first = f.process(), blocked = f.process(), last = f.process();
        f.finish(first); f.finish(blocked); f.finish(last);
        f.scheduler.find(blocked)->resources().native_space.root = 0x1000;
        OrphanReapProgress progress{99, 99, true};
        const auto status = f.scheduler.reap_orphans_checked(progress);
        CHECK(!status && status.code() == StatusCode::busy);
        CHECK(progress.reaped == 1 && progress.failed_pid == blocked && !progress.failed_after_erase);
        CHECK(!f.scheduler.find(first) && f.scheduler.find(blocked) && f.scheduler.find(last));
        CHECK(before_calls == 1 && after_calls == 1);
        f.scheduler.find(blocked)->resources().native_space = {};
        CHECK(f.scheduler.reap_orphans_checked(progress));
        CHECK(progress.reaped == 2 && !progress.failed_pid && !progress.failed_after_erase);
    }
    for (bool after : {false, true}) {
        Fixture f;
        const auto first = f.process(), failed = f.process(), last = f.process();
        f.finish(first); f.finish(failed); f.finish(last);
        if (after) fail_after = 2; else fail_before = 2;
        OrphanReapProgress progress;
        const auto status = f.scheduler.reap_orphans_checked(progress);
        CHECK(!status && status.code() == (after ? StatusCode::io_error : StatusCode::fault));
        CHECK(status.message() == (after ? after_failure : before_failure));
        CHECK(progress.reaped == (after ? 2U : 1U));
        CHECK(progress.failed_pid == failed && progress.failed_after_erase == after);
        CHECK(!f.scheduler.find(first) && bool(f.scheduler.find(failed)) == !after && f.scheduler.find(last));
        CHECK(before_calls == 2 && after_calls == (after ? 2U : 1U));
        fail_before = fail_after = 0;
        CHECK(f.scheduler.reap_orphans_checked(progress));
        CHECK(progress.reaped == (after ? 1U : 2U) && !progress.failed_pid && !progress.failed_after_erase);
    }
    {
        Fixture f;
        const auto blocked = f.process(), eligible = f.process(), parent = f.process(), owned = f.child(parent);
        f.finish(blocked); f.finish(eligible); f.finish(owned);
        f.scheduler.find(blocked)->resources().native_space.root = 0x1000;
        CHECK(f.scheduler.reap_orphans() == 1);
        CHECK(f.scheduler.find(blocked) && !f.scheduler.find(eligible) && f.scheduler.find(owned));
        CHECK(f.scheduler.find(owned)->parent_pid() == parent);
    }
}

int main()
{
    detach_lifetime_and_waiters();
    signal_status_and_completed_state();
    detach_rejection_matrix();
    checked_reap_failures_and_legacy_behavior();
    std::printf("PASS child autoreap scheduler checks=%zu guest=0 smp=0 automatic_notifications=0\n", checks);
}
