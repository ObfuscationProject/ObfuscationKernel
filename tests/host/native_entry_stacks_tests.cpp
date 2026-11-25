#include "ok/user/native_entry_stacks.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace ok;
using namespace ok::sched;
using namespace ok::user::native;
static usize checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::abort(); } } while (false)
struct alignas(4096) Backing { std::array<std::byte,memory::KernelEntryStackPool::arena_bytes> bytes; };
struct Fixture
{
    std::unique_ptr<Backing> backing{std::make_unique<Backing>()};
    memory::KernelEntryStackPool pool;
    std::unique_ptr<Scheduler> scheduler{std::make_unique<Scheduler>()};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(0x1000,0x8000)};
    Fixture() { CHECK(pool.activate(backing->bytes)); }
    ProcessId process()
    {
        auto pid=scheduler->create_process("entry-owner",context);CHECK(pid);
        CHECK(scheduler->set_runnable(pid.value()));return pid.value();
    }
    ThreadControlBlock &leader(ProcessId pid) { return scheduler->find(pid)->leader(); }
    ThreadId peer(ProcessId pid) { auto tid=scheduler->create_thread(pid,context);CHECK(tid);return tid.value(); }
    void prepare(ProcessId pid) { for(auto &t:scheduler->find(pid)->thread_records()) CHECK(prepare_entry_stack(pool,t)); }
    void drain(ProcessId pid) { for(auto &t:scheduler->find(pid)->thread_records()) CHECK(retire_entry_stack(pool,t)); }
};
void inactive_and_copy_boundaries()
{
    Fixture f;const auto pid=f.process();f.prepare(pid);auto &t=f.leader(pid);
    const auto h=t.entry_stack;const auto top=t.kernel_stack_top;
    CHECK(!prepare_entry_stack(f.pool,t));CHECK(validate_entry_stack(f.pool,t));
    t.kernel_stack_top-=16;CHECK(!retire_entry_stack(f.pool,t));CHECK(f.pool.accounting().leased_slots==1);
    t.kernel_stack_top=top;
    auto view=f.pool.activate_entry(h,{pid,t.tid},0);CHECK(view);
    CHECK(!retire_entry_stack(f.pool,t));CHECK(!f.scheduler->exit_thread(pid,t.tid));
    bool invoked=false;
    CHECK(!replace_entry_stacks(f.pool,*f.scheduler->find(pid),[&]{invoked=true;return Status::success();}));
    CHECK(!invoked && t.entry_stack==h && f.pool.accounting().active_slots==1);
    CHECK(f.pool.deactivate_entry(h,{pid,t.tid},0));CHECK(!view.value().valid());
    CHECK(retire_entry_stack(f.pool,t));CHECK(t.kernel_stack_top==0 && !t.entry_stack.pool);
    CHECK(!f.pool.top(h,{pid,t.tid}));CHECK(f.scheduler->exit_process(pid,0));CHECK(f.scheduler->reap_process(pid));
    CHECK(f.pool.accounting().leased_slots==0);
}
void exec_failure_and_commit()
{
    Fixture f;const auto pid=f.process();const auto peer=f.peer(pid);f.prepare(pid);
    auto *p=f.scheduler->find(pid);const auto first=p->leader().entry_stack,last=p->thread_slot(1).entry_stack;
    const auto stats=f.pool.accounting();
    p->leader().native_frame.pc=77;p->thread_slot(1).native_frame.pc=99;
    bool detached=false;
    const auto rejected=replace_entry_stacks(f.pool,*p,[&]{
        detached=!p->leader().entry_stack.pool && !p->thread_slot(1).entry_stack.pool &&
                 !p->leader().kernel_stack_top && !p->thread_slot(1).kernel_stack_top;
        return Status::no_memory("injected exec failure before commit");
    });
    CHECK(!rejected && detached && p->record_count()==2);
    CHECK(p->leader().entry_stack==first && p->thread_slot(1).entry_stack==last);
    CHECK(p->leader().native_frame.pc==77 && p->thread_slot(1).native_frame.pc==99);
    CHECK(f.pool.accounting().leased_slots==2 && f.pool.accounting().reservations==stats.reservations);
    CHECK(replace_entry_stacks(f.pool,*p,[&]{return f.scheduler->replace_image(pid,f.context,{});}));
    CHECK(p->record_count()==1 && p->leader().entry_stack==first && p->leader().tid==pid);
    CHECK(!f.pool.top(last,{pid,peer}) && f.pool.accounting().leased_slots==1);
    CHECK(validate_entry_stack(f.pool,p->leader()));f.drain(pid);
}
void retired_leader_exec()
{
    Fixture f;const auto pid=f.process();const auto peer=f.peer(pid);f.prepare(pid);auto *p=f.scheduler->find(pid);
    CHECK(retire_entry_stack(f.pool,p->leader()));CHECK(f.scheduler->exit_thread(pid,pid));
    const auto h=p->thread_slot(1).entry_stack;const auto before=f.pool.accounting();
    CHECK(!replace_entry_stacks(f.pool,*p,[]{return Status::no_memory("injected failed exec");}));
    CHECK(!p->leader().entry_stack.pool && p->thread_slot(1).entry_stack==h);
    CHECK(p->leader().state==ProcessState::exited && f.pool.accounting().leased_slots==1);
    CHECK(f.pool.accounting().reservations==before.reservations+1 && f.pool.accounting().releases==before.releases+1);
    CHECK(f.scheduler->activate_thread(pid,peer));
    CHECK(replace_entry_stacks(f.pool,*p,[&]{return f.scheduler->replace_image(pid,f.context,{});}));
    CHECK(p->record_count()==1 && p->leader().tid==pid && p->leader().entry_stack.pool);
    CHECK(!f.pool.top(h,{pid,peer}));CHECK(validate_entry_stack(f.pool,p->leader()));f.drain(pid);
}
void no_memory_preserves_exec()
{
    Fixture f;const auto pid=f.process();f.peer(pid);f.prepare(pid);auto *p=f.scheduler->find(pid);
    CHECK(retire_entry_stack(f.pool,p->leader()));CHECK(f.scheduler->exit_thread(pid,pid));
    std::array<memory::KernelEntryStackHandle,memory::KernelEntryStackPool::capacity-1> holds{};
    for(usize i=0;i<holds.size();++i) {auto h=f.pool.reserve({1000+i,1000+i});CHECK(h);holds[i]=h.value();}
    const auto peer=p->thread_slot(1).entry_stack;bool called=false;
    auto failure=replace_entry_stacks(f.pool,*p,[&]{called=true;return Status::success();});
    CHECK(!failure && failure.status.code()==StatusCode::no_memory && !called);
    CHECK(p->thread_slot(1).entry_stack==peer && !p->leader().entry_stack.pool);
    CHECK(f.pool.accounting().leased_slots==memory::KernelEntryStackPool::capacity);
    for(usize i=0;i<holds.size();++i)CHECK(f.pool.release(holds[i],{1000+i,1000+i}));
    f.drain(pid);CHECK(f.pool.accounting().leased_slots==0);
}
void corruption_after_exec_commit()
{
    Fixture f;const auto pid=f.process();f.peer(pid);f.peer(pid);f.prepare(pid);
    auto *p=f.scheduler->find(pid);const auto damaged=p->thread_slot(1).entry_stack;
    const auto later=p->thread_slot(2).entry_stack;const auto leader=p->leader().entry_stack;
    auto replacement=replace_entry_stacks(f.pool,*p,[&]{
        auto committed=f.scheduler->replace_image(pid,f.context,{});
        CHECK(committed);
        f.backing->bytes[damaged.slot*memory::KernelEntryStackPool::slot_stride]^=std::byte{1};
        return committed;
    });
    CHECK(!replacement && replacement.committed && replacement.retained_count==2);
    CHECK(replacement.retained[0].handle==damaged && replacement.retained[1].handle==later);
    CHECK(p->record_count()==1 && p->leader().entry_stack==leader);
    CHECK(f.pool.accounting().leased_slots==3 && f.pool.accounting().quarantined_slots==1);
    // Stable pool ownership survives; a healthy later lease can still retire.
    CHECK(f.pool.release(later,replacement.retained[1].owner));
    CHECK(!f.pool.release(damaged,replacement.retained[0].owner));
    CHECK(retire_entry_stack(f.pool,p->leader()));
    CHECK(f.pool.accounting().leased_slots==1);
}
void prepared_while_sibling_active()
{
    // Exec prepares while another CPU still runs a sibling on its lease.
    Fixture f;const auto pid=f.process();const auto peer=f.peer(pid);f.prepare(pid);auto *p=f.scheduler->find(pid);
    const auto leader=p->leader().entry_stack,sibling=p->thread_slot(1).entry_stack;
    const auto top=p->thread_slot(1).kernel_stack_top;
    CHECK(f.pool.activate_entry(sibling,{pid,peer},1));
    CHECK(f.pool.validate_active(sibling,{pid,peer},1) && !f.pool.validate_active(sibling,{pid,peer},0));
    CHECK(!f.pool.validate_active(leader,{pid,pid},1));
    const auto before=f.pool.accounting();
    {
        EntryStackPlan plan;CHECK(prepare_entry_stacks(f.pool,*p,plan) && plan.prepared());
        CHECK(plan.leader().handle==leader && f.pool.accounting().reservations==before.reservations);
        CHECK(p->thread_slot(1).entry_stack==sibling && p->thread_slot(1).kernel_stack_top==top);
        // Committing before the sibling left its CPU is an invariant breach that changes nothing.
        bool invoked=false;
        auto refused=commit_entry_stacks(f.pool,*p,plan,[&]{invoked=true;return Status::success();},[]{return Status{};});
        CHECK(!refused && !refused.committed && !invoked && p->thread_slot(1).entry_stack==sibling);
    }
    CHECK(f.pool.accounting().active_slots==1 && f.pool.accounting().leased_slots==2);
    // After eviction the prepared plan commits and releases the sibling's lease.
    EntryStackPlan plan;CHECK(prepare_entry_stacks(f.pool,*p,plan));
    CHECK(f.pool.deactivate_entry(sibling,{pid,peer},1));
    CHECK(commit_entry_stacks(f.pool,*p,plan,[&]{return f.scheduler->replace_image(pid,f.context,{});},[]{return Status{};}));
    CHECK(p->record_count()==1 && p->leader().entry_stack==leader && !f.pool.top(sibling,{pid,peer}));
    f.drain(pid);CHECK(f.pool.accounting().leased_slots==0);
}
void prepared_leader_reservation()
{
    // A leaderless group's new lease is the plan's until commit consumes it.
    Fixture f;const auto pid=f.process();const auto peer=f.peer(pid);f.prepare(pid);auto *p=f.scheduler->find(pid);
    CHECK(retire_entry_stack(f.pool,p->leader()));CHECK(f.scheduler->exit_thread(pid,pid));
    const auto before=f.pool.accounting();
    {
        EntryStackPlan plan;CHECK(prepare_entry_stacks(f.pool,*p,plan));
        CHECK(plan.leader().handle.pool && !p->leader().entry_stack.pool);
        CHECK(f.pool.accounting().leased_slots==before.leased_slots+1);
    }
    CHECK(f.pool.accounting().leased_slots==before.leased_slots && f.pool.accounting().releases==before.releases+1);
    // A thread group that changed after preparation is refused before escrow.
    EntryStackPlan plan;CHECK(prepare_entry_stacks(f.pool,*p,plan));
    const auto second=f.peer(pid);CHECK(prepare_entry_stack(f.pool,p->thread_slot(2)));
    CHECK(!commit_entry_stacks(f.pool,*p,plan,[]{return Status::success();},[]{return Status{};}));
    CHECK(p->thread_slot(2).entry_stack.pool && p->thread_slot(1).entry_stack.pool && !p->leader().entry_stack.pool);
    CHECK(plan.cancel());
    CHECK(retire_entry_stack(f.pool,p->thread_slot(2)));CHECK(f.scheduler->exit_thread(pid,second));
    CHECK(f.scheduler->activate_thread(pid,peer));
    CHECK(prepare_entry_stacks(f.pool,*p,plan));
    CHECK(commit_entry_stacks(f.pool,*p,plan,[&]{return f.scheduler->replace_image(pid,f.context,{});},[]{return Status{};}));
    CHECK(p->record_count()==1 && p->leader().entry_stack.pool && validate_entry_stack(f.pool,p->leader()));
    f.drain(pid);CHECK(f.pool.accounting().leased_slots==0);
}
void process_compaction()
{
    Fixture f;const auto a=f.process(),b=f.process();f.prepare(a);f.prepare(b);
    const auto original=f.leader(b).entry_stack;
    f.drain(a);CHECK(f.scheduler->exit_process(a,0));CHECK(f.scheduler->reap_process(a));
    CHECK(f.leader(b).entry_stack==original && validate_entry_stack(f.pool,f.leader(b)));
    f.drain(b);CHECK(f.pool.accounting().leased_slots==0);
}
int main()
{
    inactive_and_copy_boundaries();exec_failure_and_commit();retired_leader_exec();
    no_memory_preserves_exec();corruption_after_exec_commit();prepared_while_sibling_active();
    prepared_leader_reservation();process_compaction();
    std::printf("PASS native entry stacks checks=%zu scope=hosted-model guest=0 smp=0\n",checks);
}
