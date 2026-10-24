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
    ThreadControlBlock &leader(ProcessId pid) { return scheduler->find(pid)->threads()[0]; }
    ThreadId peer(ProcessId pid) { auto tid=scheduler->create_thread(pid,context);CHECK(tid);return tid.value(); }
    void prepare(ProcessId pid) { for(auto &t:scheduler->find(pid)->threads()) CHECK(prepare_entry_stack(pool,t)); }
    void drain(ProcessId pid) { for(auto &t:scheduler->find(pid)->threads()) CHECK(retire_entry_stack(pool,t)); }
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
    auto *p=f.scheduler->find(pid);const auto first=p->threads()[0].entry_stack,last=p->threads()[1].entry_stack;
    const auto stats=f.pool.accounting();
    p->threads()[0].native_frame.pc=77;p->threads()[1].native_frame.pc=99;
    bool detached=false;
    const auto rejected=replace_entry_stacks(f.pool,*p,[&]{
        detached=!p->threads()[0].entry_stack.pool && !p->threads()[1].entry_stack.pool &&
                 !p->threads()[0].kernel_stack_top && !p->threads()[1].kernel_stack_top;
        return Status::no_memory("injected exec failure before commit");
    });
    CHECK(!rejected && detached && p->threads().size()==2);
    CHECK(p->threads()[0].entry_stack==first && p->threads()[1].entry_stack==last);
    CHECK(p->threads()[0].native_frame.pc==77 && p->threads()[1].native_frame.pc==99);
    CHECK(f.pool.accounting().leased_slots==2 && f.pool.accounting().reservations==stats.reservations);
    CHECK(replace_entry_stacks(f.pool,*p,[&]{return f.scheduler->replace_image(pid,f.context,{});}));
    CHECK(p->threads().size()==1 && p->threads()[0].entry_stack==first && p->threads()[0].tid==pid);
    CHECK(!f.pool.top(last,{pid,peer}) && f.pool.accounting().leased_slots==1);
    CHECK(validate_entry_stack(f.pool,p->threads()[0]));f.drain(pid);
}
void retired_leader_exec()
{
    Fixture f;const auto pid=f.process();const auto peer=f.peer(pid);f.prepare(pid);auto *p=f.scheduler->find(pid);
    CHECK(retire_entry_stack(f.pool,p->threads()[0]));CHECK(f.scheduler->exit_thread(pid,pid));
    const auto h=p->threads()[1].entry_stack;const auto before=f.pool.accounting();
    CHECK(!replace_entry_stacks(f.pool,*p,[]{return Status::no_memory("injected failed exec");}));
    CHECK(!p->threads()[0].entry_stack.pool && p->threads()[1].entry_stack==h);
    CHECK(p->threads()[0].state==ProcessState::exited && f.pool.accounting().leased_slots==1);
    CHECK(f.pool.accounting().reservations==before.reservations+1 && f.pool.accounting().releases==before.releases+1);
    CHECK(f.scheduler->activate_thread(pid,peer));
    CHECK(replace_entry_stacks(f.pool,*p,[&]{return f.scheduler->replace_image(pid,f.context,{});}));
    CHECK(p->threads().size()==1 && p->threads()[0].tid==pid && p->threads()[0].entry_stack.pool);
    CHECK(!f.pool.top(h,{pid,peer}));CHECK(validate_entry_stack(f.pool,p->threads()[0]));f.drain(pid);
}
void no_memory_preserves_exec()
{
    Fixture f;const auto pid=f.process();f.peer(pid);f.prepare(pid);auto *p=f.scheduler->find(pid);
    CHECK(retire_entry_stack(f.pool,p->threads()[0]));CHECK(f.scheduler->exit_thread(pid,pid));
    std::array<memory::KernelEntryStackHandle,memory::KernelEntryStackPool::capacity-1> holds{};
    for(usize i=0;i<holds.size();++i) {auto h=f.pool.reserve({1000+i,1000+i});CHECK(h);holds[i]=h.value();}
    const auto peer=p->threads()[1].entry_stack;bool called=false;
    auto failure=replace_entry_stacks(f.pool,*p,[&]{called=true;return Status::success();});
    CHECK(!failure && failure.status.code()==StatusCode::no_memory && !called);
    CHECK(p->threads()[1].entry_stack==peer && !p->threads()[0].entry_stack.pool);
    CHECK(f.pool.accounting().leased_slots==memory::KernelEntryStackPool::capacity);
    for(usize i=0;i<holds.size();++i)CHECK(f.pool.release(holds[i],{1000+i,1000+i}));
    f.drain(pid);CHECK(f.pool.accounting().leased_slots==0);
}
void corruption_after_exec_commit()
{
    Fixture f;const auto pid=f.process();f.peer(pid);f.peer(pid);f.prepare(pid);
    auto *p=f.scheduler->find(pid);const auto damaged=p->threads()[1].entry_stack;
    const auto later=p->threads()[2].entry_stack;const auto leader=p->threads()[0].entry_stack;
    auto replacement=replace_entry_stacks(f.pool,*p,[&]{
        auto committed=f.scheduler->replace_image(pid,f.context,{});
        CHECK(committed);
        f.backing->bytes[damaged.slot*memory::KernelEntryStackPool::slot_stride]^=std::byte{1};
        return committed;
    });
    CHECK(!replacement && replacement.committed && replacement.retained_count==2);
    CHECK(replacement.retained[0].handle==damaged && replacement.retained[1].handle==later);
    CHECK(p->threads().size()==1 && p->threads()[0].entry_stack==leader);
    CHECK(f.pool.accounting().leased_slots==3 && f.pool.accounting().quarantined_slots==1);
    // Stable pool ownership survives; a healthy later lease can still retire.
    CHECK(f.pool.release(later,replacement.retained[1].owner));
    CHECK(!f.pool.release(damaged,replacement.retained[0].owner));
    CHECK(retire_entry_stack(f.pool,p->threads()[0]));
    CHECK(f.pool.accounting().leased_slots==1);
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
    no_memory_preserves_exec();corruption_after_exec_commit();process_compaction();
    std::printf("PASS native entry stacks checks=%zu scope=hosted-model guest=0 smp=0\n",checks);
}
