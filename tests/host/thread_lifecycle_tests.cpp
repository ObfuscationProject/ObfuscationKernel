#include "ok/sched/scheduler.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace ok;
using namespace ok::sched;
static usize checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::abort(); } } while (false)
namespace ok::sched {
struct SchedulerIdentityTestAccess {
    static void exhaust_after_one(SchedulerState &scheduler) { scheduler.next_task_id_ = max_task_id; }
};
}
struct Fixture
{
    std::unique_ptr<Scheduler> owned{std::make_unique<Scheduler>()};
    Scheduler &scheduler{*owned};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(0x1000,0x8000)};
    ProcessId process()
    {
        auto p=scheduler.create_process("threads",context);CHECK(p);CHECK(scheduler.set_runnable(p.value()));
        CHECK(scheduler.find(p.value())->threads()[0].tid==p.value());return p.value();
    }
    ThreadId peer(ProcessId pid) {auto p=scheduler.create_thread(pid,context);CHECK(p);return p.value();}
};
void identities_and_reuse()
{
    Fixture f;
    const auto a=f.process(), peer=f.peer(a), b=f.process(), other=f.peer(b), c=f.process();
    CHECK(a<peer && peer<b && b<other && other<c);
    auto *p=f.scheduler.find(a);
    auto *thread=f.scheduler.find_thread(a,peer);
    thread->tls.clear_tid_address=0x1234;
    thread->native_active=true;
    thread->native_frame.pc=0x5555;
    CHECK(f.scheduler.activate_thread(a,peer));
    auto ticket=f.scheduler.register_current_wait(ThreadWaitReason::futex,0x6789);CHECK(ticket);
    CHECK(thread->native_deadline.arm(10,0,20,false));
    auto remaining=f.scheduler.exit_thread(a,peer);
    CHECK(remaining && remaining.value()==1);
    CHECK(thread->state==ProcessState::exited && !thread->native_active && !thread->syscall_pending);
    CHECK(!thread->native_deadline.armed() && !thread->wait_registration.active());
    CHECK(f.scheduler.current_pid()==0 && f.scheduler.current_tid(0)==0);
    CHECK(!f.scheduler.complete_wait(ticket.value(),WaitCompletion::ready));
    CHECK(!f.scheduler.activate_thread(a,peer));
    CHECK(!f.scheduler.exit_thread(a,peer));
    CHECK(f.scheduler.set_runnable(a));
    CHECK(thread->state==ProcessState::exited);
    const auto fresh=f.peer(a);
    CHECK(fresh>c && p->threads().size()==2 && !f.scheduler.find_thread(a,peer));
    thread=f.scheduler.find_thread(a,fresh);
    CHECK(thread && thread->native_frame.pc==0 && !thread->tls.clear_tid_address);
    CHECK(!thread->wait_registration.active() && thread->wait_registration.ticket().generation==0);
    CHECK(!f.scheduler.complete_wait(ticket.value(),WaitCompletion::timeout));
    for (unsigned iteration=0;iteration<1000;++iteration)
    {
        const auto latest=p->threads()[1].tid;
        CHECK(f.scheduler.exit_thread(a,latest).value()==1);
        CHECK(f.peer(a)>latest && p->threads().size()==2);
    }
}
void peer_lifetime_and_group_boundary()
{
    Fixture f;const auto pid=f.process(), a=f.peer(pid), b=f.peer(pid);
    auto *p=f.scheduler.find(pid);auto *second=f.scheduler.find_thread(pid,b);
    p->resources().program_break=0x20000;
    p->resources().native_space={.root=0x8000,.slot=3,.generation=11};
    auto credentials=p->credentials();credentials.uid=177;p->set_credentials(credentials);
    second->native_active=true;second->native_frame.pc=0x9911;
    second->tls.clear_tid_address=0x2211;
    CHECK(f.scheduler.activate_thread(pid,b));
    auto peer_wait=f.scheduler.register_current_wait(ThreadWaitReason::poll,77);CHECK(peer_wait);
    CHECK(f.scheduler.activate_thread(pid,a));
    CHECK(f.scheduler.exit_thread(pid,a).value()==2);
    CHECK(second->wait_registration.pending(peer_wait.value()));
    CHECK(second->native_frame.pc==0x9911 && second->tls.clear_tid_address==0x2211);
    CHECK(p->resources().program_break==0x20000 && p->resources().native_space.root==0x8000 && p->credentials().uid==177);
    // The leader can retire while its peer and process identity remain live.
    CHECK(f.scheduler.activate_thread(pid,pid));
    CHECK(f.scheduler.exit_thread(pid,pid).value()==1);
    CHECK(p->state()==ProcessState::blocked && p->threads()[0].state==ProcessState::exited);
    CHECK(f.scheduler.complete_wait(peer_wait.value(),WaitCompletion::ready));
    CHECK(p->state()==ProcessState::runnable);
    CHECK(f.scheduler.activate_thread(pid,b));
    CHECK(f.scheduler.current_tid(0)==b);
    CHECK(f.scheduler.exit_thread(pid,b).value()==0);
    CHECK(p->state()==ProcessState::blocked && !f.scheduler.set_runnable(pid));
    CHECK(!f.scheduler.create_thread(pid,f.context));
    CHECK(!f.scheduler.fork_process(pid));
    CHECK(!f.scheduler.replace_image(pid,f.context,{}));
    CHECK(!f.scheduler.activate(pid));
    CHECK(p->resources().native_space.root==0x8000);
    CHECK(!f.scheduler.reap_process(pid));
    // Only explicit group teardown publishes exit status; VM retirement remains
    // the native runtime's job, rather than an implicit scheduler side effect.
    CHECK(f.scheduler.exit_process(pid,13));
    CHECK(p->state()==ProcessState::zombie && p->exit_code()==13);
    CHECK(!f.scheduler.reap_process(pid));
    p->resources().native_space={};
    CHECK(f.scheduler.reap_process(pid));
}
void capacity_and_cpu_ownership()
{
    Fixture f;const auto pid=f.process();std::array<ThreadId,max_threads_per_process> tids{pid};
    for(usize i=1;i<tids.size();++i)tids[i]=f.peer(pid);
    CHECK(!f.scheduler.create_thread(pid,f.context));
    const auto next=f.process();CHECK(next==tids.back()+1);
    CHECK(f.scheduler.configure_cpus(2));
    CHECK(f.scheduler.activate_thread(pid,tids[1],1));
    auto wrong=f.scheduler.exit_thread(pid,tids[1],0);
    CHECK(!wrong && wrong.status().code()==StatusCode::busy);
    CHECK(f.scheduler.find_thread(pid,tids[1])->state==ProcessState::running);
    CHECK(f.scheduler.current_tid(1)==tids[1]);
    CHECK(f.scheduler.activate_thread(pid,tids[2],0));
    CHECK(f.scheduler.exit_thread(pid,tids[2],0).value()==7);
    CHECK(f.scheduler.current_tid(1)==tids[1] && f.scheduler.find(pid)->state()==ProcessState::running);
    CHECK(f.scheduler.exit_thread(pid,tids[1],1).value()==6);
    CHECK(f.scheduler.current_tid(1)==0 && f.scheduler.cpu_stats(1).current_tid==0);
    CHECK(!f.scheduler.exit_thread(pid,999999));
    CHECK(!f.scheduler.exit_thread(next,pid));
    CHECK(!f.scheduler.exit_thread(pid,pid,2));
}
void exhaustion_and_owned_resources()
{
    static_assert(max_task_id == 0x3fffffffU);
    static_assert((max_task_id & 0xc0000000U) == 0);
    Fixture f;const auto pid=f.process();
    auto *thread=f.scheduler.find_thread(pid,pid);
    thread->kernel_stack_top=0x4000;
    CHECK(!f.scheduler.exit_thread(pid,pid));
    CHECK(thread->state==ProcessState::runnable && thread->kernel_stack_top==0x4000);
    thread->kernel_stack_top=0;
    SchedulerIdentityTestAccess::exhaust_after_one(f.scheduler);
    auto last=f.scheduler.create_thread(pid,f.context);CHECK(last && last.value()==max_task_id);
    CHECK(!f.scheduler.create_thread(pid,f.context));
    CHECK(!f.scheduler.create_process("exhausted",f.context));
    CHECK(f.scheduler.exit_thread(pid,last.value()).value()==1);
    CHECK(!f.scheduler.create_thread(pid,f.context));
    CHECK(f.scheduler.find_thread(pid,pid)->tid==pid);
    Fixture final_process;
    SchedulerIdentityTestAccess::exhaust_after_one(final_process.scheduler);
    auto last_process = final_process.scheduler.create_process("last-owner", final_process.context);
    CHECK(last_process && last_process.value() == 0x3fffffffU);
    CHECK(!final_process.scheduler.create_thread(last_process.value(), final_process.context));
    CHECK(!final_process.scheduler.create_process("no-wrap", final_process.context));
}
void distinct_cpu_processes()
{
    Fixture f;const auto a=f.process(), b=f.process();
    CHECK(f.scheduler.configure_cpus(2));
    CHECK(f.scheduler.activate_thread(a,a,0));
    CHECK(f.scheduler.activate_thread(b,b,1));
    CHECK(f.scheduler.current_pid()==a && f.scheduler.current_thread()->owner==a);
    CHECK(f.scheduler.activate(0,1));
    CHECK(f.scheduler.current_pid()==a);
    CHECK(f.scheduler.activate_thread(b,b,1));
    CHECK(f.scheduler.exit_thread(b,b,1).value()==0);
    CHECK(f.scheduler.current_pid()==a && f.scheduler.current_pid(1)==0);
    CHECK(f.scheduler.register_current_wait(ThreadWaitReason::poll,99));
}

void prepared_thread_publication()
{
    Fixture f; const auto pid = f.process();
    auto *process = f.scheduler.find(pid);
    ThreadId failed_tid{};
    usize calls{};
    const auto before = process->threads().size();
    auto failed = f.scheduler.create_thread_prepared(pid, f.context,
        [&](ThreadControlBlock &thread) -> Status {
            ++calls; failed_tid = thread.tid;
            CHECK(thread.owner == pid && thread.state == ProcessState::created);
            CHECK(!f.scheduler.find_thread(pid, thread.tid));
            CHECK(process->threads().size() == before);
            return Status::no_memory("injected entry stack reservation failure");
        });
    CHECK(!failed && failed.status().code() == StatusCode::no_memory && calls == 1);
    CHECK(process->threads().size() == before && process->state() == ProcessState::runnable);
    auto prepared = f.scheduler.create_thread_prepared(pid, f.context,
        [&](ThreadControlBlock &thread) -> Status {
            ++calls;
            CHECK(thread.tid > failed_tid && !f.scheduler.find_thread(pid, thread.tid));
            CHECK(thread.state == ProcessState::created && process->threads().size() == before);
            thread.native_frame.pc = 0x991199;
            thread.native_active = true;
            thread.tls.clear_tid_address = 0x8888;
            return Status::success();
        });
    CHECK(prepared && calls == 2);
    auto *published = f.scheduler.find_thread(pid, prepared.value());
    CHECK(published && published->state == ProcessState::runnable && published->native_active);
    CHECK(published->native_frame.pc == 0x991199 && published->tls.clear_tid_address == 0x8888);
    while (!process->threads().full()) CHECK(f.scheduler.create_thread(pid, f.context));
    auto full = f.scheduler.create_thread_prepared(pid, f.context,
        [&](ThreadControlBlock &) { ++calls; return Status::success(); });
    CHECK(!full && full.status().code() == StatusCode::would_block && calls == 2);
    CHECK(!f.scheduler.create_thread_prepared(999999, f.context,
        [&](ThreadControlBlock &) { ++calls; return Status::success(); }));
    CHECK(calls == 2);
}

void entry_stack_lifetime_barriers()
{
    // Deliberately incomplete nonowning tokens exercise conservative scheduler
    // barriers independently of canonical pool validation by the native runtime.
    memory::KernelEntryStackPool identity;
    for (bool handle_only : {false, true})
    {
        Fixture f; const auto pid = f.process(); const auto peer = f.peer(pid);
        auto *process = f.scheduler.find(pid); auto *thread = f.scheduler.find_thread(pid, peer);
        if (handle_only) thread->entry_stack = {&identity, 0, 1};
        else thread->kernel_stack_top = 0x44000;
        process->resources().program_break = 0x91919;
        auto result = f.scheduler.exit_thread(pid, peer);
        CHECK(!result && result.status().code() == StatusCode::busy);
        CHECK(thread->state == ProcessState::runnable);
        auto exec = f.scheduler.replace_image(pid, f.context, {});
        CHECK(!exec && exec.code() == StatusCode::busy);
        CHECK(process->threads().size() == 2 && process->resources().program_break == 0x91919);
        CHECK(!f.scheduler.exit_process(pid, 77));
        CHECK(!f.scheduler.terminate_process(pid, 9));
        CHECK(process->state() == ProcessState::runnable && process->exit_code() == 0);
        process->set_state(ProcessState::zombie);
        CHECK(!f.scheduler.reap_process(pid));
        CHECK(f.scheduler.find_thread(pid, peer) == thread);
        process->set_state(ProcessState::runnable);
        while (!process->threads().full()) CHECK(f.scheduler.create_thread(pid, f.context));
        thread->state = ProcessState::exited;
        CHECK(!f.scheduler.create_thread(pid, f.context));
        CHECK(f.scheduler.find_thread(pid, peer) == thread);
        thread->entry_stack = {}; thread->kernel_stack_top = 0;
        auto reused = f.scheduler.create_thread(pid, f.context);
        CHECK(reused && reused.value() != peer && !f.scheduler.find_thread(pid, peer));
        CHECK(f.scheduler.exit_process(pid, 77));
        CHECK(f.scheduler.reap_process(pid));
    }
}

void staged_fork_publication()
{
    Fixture f; const auto pid = f.process();
    memory::KernelEntryStackPool identity;
    auto *parent = f.scheduler.find(pid);
    parent->threads()[0].entry_stack = {&identity, 7, 19};
    parent->threads()[0].kernel_stack_top = 0x51510;
    parent->threads()[0].tls.robust_list = 0x61610;
    parent->threads()[0].tls.robust_list_length = 24;
    auto staged = f.scheduler.fork_process(pid, false); CHECK(staged);
    auto *child = f.scheduler.find(staged.value());
    CHECK(child && child->state() == ProcessState::created && child->threads()[0].state == ProcessState::created);
    CHECK(!child->threads()[0].entry_stack.pool && child->threads()[0].kernel_stack_top == 0);
    CHECK(child->threads()[0].tls.robust_list == 0 && child->threads()[0].tls.robust_list_length == 0);
    CHECK(parent->threads()[0].tls.robust_list == 0x61610 && parent->threads()[0].tls.robust_list_length == 24);
    CHECK(child->parent_pid() == pid);
    CHECK(f.scheduler.set_runnable(staged.value()));
    CHECK(child->state() == ProcessState::runnable && child->threads()[0].state == ProcessState::runnable);
    CHECK(f.scheduler.exit_process(staged.value(), 0)); CHECK(f.scheduler.reap_process(staged.value()));
    auto abandoned = f.scheduler.fork_process(pid, false); CHECK(abandoned);
    CHECK(f.scheduler.exit_process(abandoned.value(), 1)); CHECK(f.scheduler.reap_process(abandoned.value()));
    auto ordinary = f.scheduler.fork_process(pid); CHECK(ordinary);
    CHECK(f.scheduler.find(ordinary.value())->state() == ProcessState::runnable);
}

int main()
{
    identities_and_reuse();peer_lifetime_and_group_boundary();capacity_and_cpu_ownership();exhaustion_and_owned_resources();distinct_cpu_processes();
    prepared_thread_publication();entry_stack_lifetime_barriers();staged_fork_publication();
    std::printf("PASS thread lifecycle checks=%zu guest=0 smp=0\n",checks);
}
