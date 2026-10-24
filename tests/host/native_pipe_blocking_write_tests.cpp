// Actual runtime, dispatcher, scheduler and private signals; mocked hardware only.
#define OK_ENABLE_TEST_POINTS 1
#ifdef OK_BLOCKING_WRITE_PRIVATE_TEST
#define OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE 1
#endif
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#define OK_NATIVE_MODEL_MEMORY_BYTES 65536
#include "native_clock_model.hpp"
#include <source_location>
namespace ok::sched
{
struct WaitRegistrationTestAccess
{
    static void seed_idle(WaitRegistration &wait,u64 generation)
    { CHECK(!wait.active_);wait.ticket_.generation=generation; }
};
}
namespace
{
constexpr uptr site=hw::user_begin+1024, array_address=hw::user_begin+512;
constexpr uptr data=hw::user_begin+8192, handler=hw::user_begin+2048;
usize checks{};
void expect(bool value, std::source_location where=std::source_location::current())
{
    ++checks;
    if(!value) { std::fprintf(stderr,"PIPE_BLOCKING_WRITE_FAIL line=%u checks=%zu\n",where.line(),checks); std::abort(); }
}
struct Group { sched::ProcessId pid{}; sched::ThreadId writer{}; std::array<i32,2> fds{}; };
Group create(Kernel &kernel)
{
    auto made=kernel.scheduler().create_user_process("pipe-blocking",
        kernel.arch().make_user_context({hw::user_begin,hw::user_begin+65536})); CHECK(made);
    Group g{made.value()}; auto *p=kernel.scheduler().find(g.pid);
    p->resources().personality=sched::UserPersonality::linux;
    p->resources().native_space={0x100000+g.pid*4096,static_cast<u32>(g.pid%16),1};
    CHECK(native::allocate(p->resources().native_pages,hw::user_begin,hw::page_read|hw::page_execute));
    auto peer=kernel.scheduler().create_thread(g.pid,p->threads()[0].context); CHECK(peer);g.writer=peer.value();
    for(auto &t:p->threads()){CHECK(native::prepare_native_thread(t));t.native_active=true;}
    p->threads()[0].native_active=false;CHECK(kernel.scheduler().set_runnable(g.pid));
    CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));auto fds=kernel.posix().pipe(0);CHECK(fds);g.fds=fds.value();
    native::runtime_kernel=&kernel;native::started=true;native::last_pid=native::last_tid=0;
    for(usize i=0;i<32768;++i)mock::memory[data-hw::user_begin+i]=static_cast<std::byte>((i*29+7)%251);
    return g;
}
void leader(Kernel &kernel,Group g){CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));}
void retire(Kernel &kernel,Group g)
{
    mock::forbidden=mock::write_forbidden=0;
    auto *p=kernel.scheduler().find(g.pid);
    if(p&&p->state()!=sched::ProcessState::zombie&&p->state()!=sched::ProcessState::exited)
    {native::release_process(kernel,g.pid);CHECK(kernel.scheduler().exit_process(g.pid,0));}
    CHECK(kernel.scheduler().reap_orphans());expect(native::pipe_write_objects.accounting().live_objects==0);
    expect(native::pipe_write_objects.accounting().backing_pages==0);expect(native::runtime_status().ok());
}
void frame(Kernel &kernel,Group g,bool vector,usize count)
{
    auto *t=kernel.scheduler().find_thread(g.pid,g.writer);auto &f=t->native_frame;
    f={};f.flags=0x202;f.sp=hw::user_begin+65536;f.pc=site+2;
    f.fp_state[0]=0x7f;f.fp_state[1]=3;f.fp_state[24]=0x80;f.fp_state[25]=0x1f;
    f.registers[14]=vector?20:1;f.registers[9]=static_cast<uptr>(g.fds[1]);
    f.registers[10]=vector?array_address:data;f.registers[11]=vector?4:count;
    const usize first=count>2000?2000:count,second=count-first>2500?2500:count-first;
    const std::array<native::PipeVector,4> vectors{{{data,first},{~uptr{0},0},
        {data+first,second},{data+first+second,count-first-second}}};
    std::memcpy(mock::memory.data()+array_address-hw::user_begin,vectors.data(),sizeof(vectors));
    mock::memory[site-hw::user_begin]=std::byte{0x0f};mock::memory[site-hw::user_begin+1]=std::byte{0x05};
    mock::entry_trap={hw::TrapKind::syscall,256};native::last_pid=native::last_tid=0;
}
memory::ObjectHandle object(Group g){return native::pipe_write_object(g.pid,g.writer);}
native::PipeWriteWait snapshot(Group g)
{auto w=native::pipe_write_objects.resolve(object(g));expect(bool(w));return *w.value();}
void step(Kernel &kernel){native::last_pid=native::last_tid=0;expect(native::step(kernel));}
i64 result(Kernel &kernel,Group g)
{return static_cast<i64>(kernel.scheduler().find_thread(g.pid,g.writer)->native_frame.registers[14]);}
void filled(Kernel &kernel,Group g,usize count=sched::pipe_capacity)
{
    leader(kernel,g);std::array<std::byte,sched::pipe_capacity> bytes{};
    auto w=kernel.posix().write(g.fds[1],{bytes.data(),count});CHECK(w&&w.value()==count);
}
void drain(Kernel &kernel,Group g,usize count,usize offset,bool verify=true)
{
    leader(kernel,g);std::array<std::byte,sched::pipe_capacity> bytes{};
    auto read=kernel.posix().read(g.fds[0],{bytes.data(),count});CHECK(read&&read.value()==count);
    if(verify)for(usize i=0;i<count;++i)expect(bytes[i]==mock::memory[data-hw::user_begin+offset+i]);
}
void complete(Kernel &kernel,Group g,usize count)
{
    expect(!object(g).pool);auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
    expect(!t->syscall_pending&&t->native_frame.pc==site+2);expect(result(kernel,g)==static_cast<i64>(count));
}
void full_request(Kernel &kernel,bool vector)
{
    auto g=create(kernel);constexpr usize total=3*sched::pipe_capacity+37;frame(kernel,g,vector,total);step(kernel);
    auto first=snapshot(g);expect(first.count==total&&first.committed==sched::pipe_capacity);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
    const auto origin=native::syscall_origins.identity(*p,*t);
    expect(native::syscall_origins.validate_bound(origin,t->native_frame,native::pipe_write_origin_binding(object(g),first)));
    expect(!native::syscall_origins.prepare_restart(origin,t->native_frame,native::pipe_write_origin_binding(object(g),first)));
    // Copied metadata and retained description must survive replacement names.
    std::memset(mock::memory.data()+array_address-hw::user_begin,0xff,sizeof(native::PipeVector)*4);
    leader(kernel,g);auto replacement=kernel.posix().pipe(0);CHECK(replacement);
    CHECK(kernel.posix().duplicate_to(replacement.value()[1],g.fds[1]));
    const auto *replacement_fd=p->fd_table().entry(g.fds[1]);CHECK(replacement_fd);
    const auto replacement_pipe=kernel.scheduler().open_description(replacement_fd->description)->pipe;
    for(usize offset=0;offset<3*sched::pipe_capacity;offset+=sched::pipe_capacity)
    {
        drain(kernel,g,sched::pipe_capacity,offset);
        // A consumed-prefix mapping fault must not poison the fresh suffix.
        mock::forbidden=data;mock::memory[data-hw::user_begin+offset+sched::pipe_capacity]=std::byte{0x65};
        step(kernel);
        if(offset<2*sched::pipe_capacity)
        {
            auto current=snapshot(g);expect(current.committed==offset+2*sched::pipe_capacity);
            expect(current.ticket.generation>first.ticket.generation&&current.description==first.description);
        }
    }
    complete(kernel,g,total);drain(kernel,g,37,3*sched::pipe_capacity);
    auto readiness=kernel.scheduler().pipe_readiness(replacement_pipe);CHECK(readiness);expect(readiness.value().buffered==0);
    expect(!kernel.scheduler().open_description(first.description));retire(kernel,g);
}
void final_suffix(Kernel &kernel,bool vector)
{
    auto g=create(kernel);frame(kernel,g,vector,sched::pipe_capacity+17);step(kernel);
    drain(kernel,g,7,0);step(kernel);expect(snapshot(g).committed==sched::pipe_capacity+7);
    drain(kernel,g,sched::pipe_capacity,7);step(kernel);complete(kernel,g,sched::pipe_capacity+17);
    drain(kernel,g,10,sched::pipe_capacity+7);retire(kernel,g);
}
void terminal(Kernel &kernel,bool vector,unsigned mode)
{
    auto g=create(kernel);frame(kernel,g,vector,2*sched::pipe_capacity+3);step(kernel);
    const auto wait=snapshot(g);const auto old=object(g);auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
    if(mode==0){leader(kernel,g);CHECK(kernel.posix().close(g.fds[0]));}
    if(mode==1){drain(kernel,g,sched::pipe_capacity,0);mock::forbidden=data+sched::pipe_capacity;}
    if(mode==2){leader(kernel,g);CHECK(kernel.posix().fcntl(g.fds[1],posix::f_SETFL,posix::o_NONBLOCK));
        CHECK(kernel.scheduler().complete_wait(wait.ticket,sched::WaitCompletion::ready));}
    if(mode==3)CHECK(kernel.scheduler().complete_wait(wait.ticket,sched::WaitCompletion::cancelled));
    if(mode==4){drain(kernel,g,7,0);CHECK(kernel.posix().fcntl(g.fds[1],posix::f_SETFL,posix::o_NONBLOCK));}
    step(kernel);complete(kernel,g,sched::pipe_capacity+(mode==4?7:0));expect(!native::pipe_write_objects.resolve(old));
    expect(!kernel.scheduler().complete_wait(wait.ticket,sched::WaitCompletion::ready));
#ifdef OK_BLOCKING_WRITE_PRIVATE_TEST
    auto *queue=native::private_signal_queue(*t);CHECK(queue);expect(queue->pending_mask().contains(13)==(mode==0));
#else
    static_cast<void>(t);
#endif
    if(mode!=0&&mode!=1)drain(kernel,g,sched::pipe_capacity,mode==4?7:0);
    retire(kernel,g);
}
void allocation_failure(Kernel &kernel,bool vector,bool progress)
{
    auto g=create(kernel);if(!progress)filled(kernel,g);frame(kernel,g,vector,2*sched::pipe_capacity);
    // Exhaust actual object allocation while retaining every pressure handle.
    CHECK(native::activate_pool());std::array<memory::ObjectHandle,native::pipe_write_capacity> pressure{};
    for(auto &entry:pressure){auto allocated=native::pipe_write_objects.create();CHECK(allocated);entry=allocated.value();}
    step(kernel);
    for(auto entry:pressure)CHECK(native::pipe_write_objects.destroy(entry));
    CHECK(native::pipe_write_objects.trim_empty_pages());
    expect(result(kernel,g)==(progress?static_cast<i64>(sched::pipe_capacity):-12));
    expect(!object(g).pool);drain(kernel,g,sched::pipe_capacity,0,progress);retire(kernel,g);
}
void cancellation_replacement(Kernel &kernel,bool vector,bool progress)
{
    auto g=create(kernel);if(!progress)filled(kernel,g);frame(kernel,g,vector,2*sched::pipe_capacity);step(kernel);
    const auto wait=snapshot(g);const auto old=object(g);auto *p=kernel.scheduler().find(g.pid);
    auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
    CHECK(kernel.scheduler().complete_wait(wait.ticket,sched::WaitCompletion::cancelled));
    t->native_frame.registers[14]=39; // A replacement getpid is not the retained write.
    CHECK(kernel.scheduler().activate_thread(g.pid,g.writer));bool done=true;
    expect(native::resume_pipe_write(kernel,*p,old,done)==-125&&!done);
    expect(t->native_frame.registers[14]==39&&t->syscall_pending&&!object(g).pool);
    drain(kernel,g,sched::pipe_capacity,0,progress);retire(kernel,g);
}
void stale_admission(Kernel &kernel,bool vector,bool progress,bool cancelled,unsigned mutation)
{
    auto g=create(kernel);if(!progress)filled(kernel,g);frame(kernel,g,vector,sched::pipe_capacity+5);step(kernel);
    const auto wait=snapshot(g);const auto old=object(g);auto *p=kernel.scheduler().find(g.pid);
    auto *t=kernel.scheduler().find_thread(g.pid,g.writer);expect(wait.origin_capture!=0);
    if(cancelled)CHECK(kernel.scheduler().complete_wait(wait.ticket,sched::WaitCompletion::cancelled));
    else drain(kernel,g,sched::pipe_capacity,0,progress);
    if(mutation==0){t->native_frame.registers[14]=39;t->native_frame.pc+=2;}
    if(mutation==1)++p->resources().native_space.generation;
    if(mutation==2)native::syscall_origins.revoke(*t);
    if(mutation==3)CHECK(native::syscall_origins.prepare(*t));
    const auto frame_before=t->native_frame;
    CHECK(kernel.scheduler().activate_thread(g.pid,g.writer));bool done=true;
    expect(native::resume_pipe_write(kernel,*p,old,done)==-125&&!done);
    expect(native::SyscallOrigins::same_frame(frame_before,t->native_frame)&&t->syscall_pending);
    expect(!object(g).pool&&!native::pipe_write_objects.resolve(old));
    auto ready=kernel.scheduler().pipe_readiness(wait.pipe);CHECK(ready);
    expect(ready.value().buffered==(cancelled?sched::pipe_capacity:0));
    if(cancelled)drain(kernel,g,sched::pipe_capacity,0,progress);
    retire(kernel,g);
}
void generation_failure(Kernel &kernel,bool vector,bool on_resume,bool progress)
{
    auto g=create(kernel);if(!progress)filled(kernel,g);frame(kernel,g,vector,3*sched::pipe_capacity);
    auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
    sched::WaitRegistrationTestAccess::seed_idle(t->wait_registration,~u64{0}-(on_resume?1:0));
    step(kernel);
    usize expected=progress?sched::pipe_capacity:0;
    if(on_resume)
    {
        auto wait=snapshot(g);expect(wait.ticket.generation==~u64{0});
        if(progress){drain(kernel,g,sched::pipe_capacity,0);expected+=sched::pipe_capacity;}
        else CHECK(kernel.scheduler().complete_wait(wait.ticket,sched::WaitCompletion::ready));
        step(kernel);
    }
    expect(!object(g).pool&&result(kernel,g)==(expected?static_cast<i64>(expected):-75));
    drain(kernel,g,sched::pipe_capacity,on_resume&&progress?sched::pipe_capacity:0,progress);retire(kernel,g);
}
void lifecycle(Kernel &kernel,bool vector,unsigned mode)
{
    auto g=create(kernel);frame(kernel,g,vector,2*sched::pipe_capacity);step(kernel);
    const auto wait=snapshot(g);const auto old=object(g);auto *p=kernel.scheduler().find(g.pid);
    leader(kernel,g);CHECK(kernel.posix().close(g.fds[1]));
    if(mode==0)native::release_process(kernel,g.pid);
    if(mode==1)
    {
        CHECK(native::retire_native_thread(*kernel.scheduler().find_thread(g.pid,g.writer)));
        CHECK(kernel.scheduler().exit_thread(g.pid,g.writer));native::prune_pipe_writes(kernel);
    }
    if(mode==2)
    {
        native::ExecRetirement retirement{*p,g.pid};
        auto replaced=native::replace_entry_stacks(native::entry_stacks,*p,[&]{
            return kernel.posix().exec(g.pid,p->threads()[0].context,p->resources().memory);
        });CHECK(replaced&&replaced.committed);retirement.consume(kernel,*p);
    }
    expect(!object(g).pool&&!native::pipe_write_objects.resolve(old));
    expect(!kernel.scheduler().open_description(wait.description));
    expect(!kernel.scheduler().complete_wait(wait.ticket,sched::WaitCompletion::ready));
    drain(kernel,g,sched::pipe_capacity,0);retire(kernel,g);
}
void limits(Kernel &kernel,bool vector)
{
    auto g=create(kernel);frame(kernel,g,vector,2*sched::pipe_capacity);
    auto *p=kernel.scheduler().find(g.pid);const auto description=p->fd_table().entry(g.fds[1])->description;
    CHECK(kernel.scheduler().activate_thread(g.pid,g.writer));bool done=true;
    expect(native::begin_pipe_write(kernel,*p,g.writer,g.fds[1],description,data,static_cast<usize>(__INTPTR_MAX__)+1,done)==-22&&done);
    expect(native::begin_pipe_write(kernel,*p,g.writer,g.fds[1],description,~uptr{0}-1,3,done)==-14&&done);
    native::PipeVectors vectors{};vectors.count=2;vectors.entries[0]={data,static_cast<usize>(__INTPTR_MAX__)};vectors.entries[1]={data,1};
    expect(native::begin_pipe_write(kernel,*p,g.writer,g.fds[1],description,array_address,1,done,vectors)==-22&&done);
    leader(kernel,g);CHECK(kernel.posix().fcntl(g.fds[1],posix::f_SETFL,posix::o_NONBLOCK));
    step(kernel);complete(kernel,g,sched::pipe_capacity);drain(kernel,g,sched::pipe_capacity,0);
    frame(kernel,g,vector,2*sched::pipe_capacity);filled(kernel,g);step(kernel);
    expect(result(kernel,g)==-11&&!object(g).pool);drain(kernel,g,sched::pipe_capacity,0,false);retire(kernel,g);
}
#ifdef OK_BLOCKING_WRITE_PRIVATE_TEST
void capture_identity_signal(Kernel &kernel,bool vector,bool progress)
{
    auto g=create(kernel);if(!progress)filled(kernel,g);frame(kernel,g,vector,sched::pipe_capacity+5);step(kernel);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
    auto *action=native::private_signal_table(kernel,g.pid)->find(10);CHECK(action);
    action->handler=handler;action->restorer=handler+4;action->flags=0x14000004;
    auto *queue=native::private_signal_queue(*t);CHECK(queue);
    expect(queue->enqueue({10,-6,777,888})==native::PrivateSignalQueue::Enqueue::queued);
    auto wait=native::pipe_write_objects.resolve(object(g));CHECK(wait);
    const auto admitted=wait.value()->origin_capture;expect(admitted!=0);
    wait.value()->origin_capture=admitted+1;
    expect(!native::private_pipe_write_signal_ready(kernel,*p,*t));
    expect(!native::private_pipe_write_signal_complete(kernel,*p,*t));
    wait.value()->origin_capture=admitted;
    expect(native::private_pipe_write_signal_ready(kernel,*p,*t));
    retire(kernel,g);
}
void zero_progress_signal(Kernel &kernel,bool vector,bool restart)
{
    auto g=create(kernel);filled(kernel,g);frame(kernel,g,vector,3*sched::pipe_capacity+41);step(kernel);
    const auto wait=snapshot(g);expect(wait.count==3*sched::pipe_capacity+41&&!wait.committed);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
    auto *action=native::private_signal_table(kernel,g.pid)->find(10);CHECK(action);
    action->handler=handler;action->restorer=handler+4;action->flags=0x04000004|(restart?0x10000000:0);
    auto *queue=native::private_signal_queue(*t);CHECK(queue);
    expect(queue->enqueue({10,-6,777,888})==native::PrivateSignalQueue::Enqueue::queued);
    expect(native::private_pipe_write_signal_complete(kernel,*p,*t));
    expect(!object(g).pool&&!t->syscall_pending);
    expect(t->native_frame.pc==(restart?site:site+2));
    expect(result(kernel,g)==(restart?(vector?20:1):-4));
    expect(t->native_frame.registers[11]==(vector?4:3*sched::pipe_capacity+41));
    drain(kernel,g,sched::pipe_capacity,0,false);retire(kernel,g);
}
void sigpipe_prefix(Kernel &kernel,bool vector,bool restart,bool terminate)
{
    auto g=create(kernel);frame(kernel,g,vector,2*sched::pipe_capacity+3);step(kernel);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
    auto *action=native::private_signal_table(kernel,g.pid)->find(13);CHECK(action);
    action->handler=terminate?sched::SignalAction::default_handler:handler;
    action->restorer=handler+4;action->flags=0x04000004|(restart?0x10000000:0);
    leader(kernel,g);CHECK(kernel.posix().close(g.fds[0]));step(kernel);complete(kernel,g,sched::pipe_capacity);
    if(terminate)
    {
        expect(!native::private_signal_deliver(kernel,*p,*t));
        expect(p->state()==sched::ProcessState::zombie&&p->wait_status()==13);
    }
    else
    {
        expect(native::private_signal_deliver(kernel,*p,*t)&&t->native_frame.pc==handler);
        using F=native::X86SignalFrame;std::array<std::byte,F::prefix_bytes> image{};
        std::memcpy(image.data(),mock::memory.data()+t->native_frame.sp-hw::user_begin,image.size());
        expect(F::get(image,F::context_offset+128,8)==site+2);
        expect(F::get(image,F::context_offset+13*8,8)==sched::pipe_capacity);
        expect(F::get(image,F::info_offset,4)==13&&F::get(image,F::info_offset+8,4)==0);
        t->native_frame.sp+=8;t->native_frame.registers[14]=15;bool done=true;i64 returned=-999;
        expect(native::private_signal_call(kernel,*p,*t,done,returned)&&!done&&returned==0);
        expect(result(kernel,g)==static_cast<i64>(sched::pipe_capacity)&&t->native_frame.pc==site+2);
        expect(!object(g).pool);
    }
    retire(kernel,g);
}
void caught_prefix(Kernel &kernel,bool vector,bool restart,bool initial_progress)
{
    auto g=create(kernel);if(!initial_progress)filled(kernel,g);frame(kernel,g,vector,2*sched::pipe_capacity+3);step(kernel);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
    auto wait=snapshot(g);const auto old=object(g);const auto origin=native::syscall_origins.identity(*p,*t);
    const auto binding=native::pipe_write_origin_binding(old,wait);
    const auto permit=native::syscall_origins.prepare_restart(origin,t->native_frame,binding);
    expect(bool(permit)==!initial_progress);
    if(!initial_progress)
    {
        drain(kernel,g,sched::pipe_capacity,0,false);step(kernel);wait=snapshot(g);
        expect(!native::syscall_origins.validate_restart(permit.value(),origin,t->native_frame,binding));
        expect(!native::syscall_origins.consume_restart(permit.value(),origin,t->native_frame,binding));
    }
    auto *action=native::private_signal_table(kernel,g.pid)->find(10);CHECK(action);
    action->handler=handler;action->restorer=handler+4;action->flags=0x04000004|(restart?0x10000000:0);
    auto *queue=native::private_signal_queue(*t);CHECK(queue);
    expect(queue->enqueue({10,-6,777,888})==native::PrivateSignalQueue::Enqueue::queued);
    const auto args=t->native_frame.registers;expect(native::private_pipe_write_signal_ready(kernel,*p,*t));
    expect(native::private_pipe_write_signal_complete(kernel,*p,*t));complete(kernel,g,sched::pipe_capacity);
    expect(!native::pipe_write_objects.resolve(old)&&!native::syscall_origins.live(origin));
    expect(native::private_signal_deliver(kernel,*p,*t)&&t->native_frame.pc==handler);
    using F=native::X86SignalFrame;std::array<std::byte,F::prefix_bytes> image{};
    std::memcpy(image.data(),mock::memory.data()+t->native_frame.sp-hw::user_begin,image.size());
    expect(F::get(image,F::context_offset+128,8)==site+2);
    expect(F::get(image,F::context_offset+13*8,8)==sched::pipe_capacity);
    t->native_frame.sp+=8;t->native_frame.registers[14]=15;bool done=true;i64 returned=-999;
    expect(native::private_signal_call(kernel,*p,*t,done,returned)&&!done&&returned==0);
    expect(result(kernel,g)==static_cast<i64>(sched::pipe_capacity)&&t->native_frame.pc==site+2);
    expect(t->native_frame.registers[9]==args[9]&&t->native_frame.registers[10]==args[10]&&t->native_frame.registers[11]==args[11]);
    drain(kernel,g,sched::pipe_capacity,0);retire(kernel,g);
}
#endif
}
int main()
{
    static Kernel kernel;CHECK(kernel.boot({}));CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for(bool vector:{false,true})
    {
        full_request(kernel,vector);final_suffix(kernel,vector);
        for(unsigned mode=0;mode<5;++mode)terminal(kernel,vector,mode);
        allocation_failure(kernel,vector,false);allocation_failure(kernel,vector,true);
        for(bool progress:{false,true})cancellation_replacement(kernel,vector,progress);
        for(bool progress:{false,true})for(bool cancelled:{false,true})for(unsigned mutation=0;mutation<4;++mutation)
            stale_admission(kernel,vector,progress,cancelled,mutation);
        limits(kernel,vector);
        for(bool resume:{false,true})for(bool progress:{false,true})generation_failure(kernel,vector,resume,progress);
        for(unsigned mode=0;mode<3;++mode)lifecycle(kernel,vector,mode);
#ifdef OK_BLOCKING_WRITE_PRIVATE_TEST
        for(bool progress:{false,true})capture_identity_signal(kernel,vector,progress);
        for(bool restart:{false,true})for(bool initial:{false,true})caught_prefix(kernel,vector,restart,initial);
        for(bool restart:{false,true})
        {
            zero_progress_signal(kernel,vector,restart);
            for(bool terminate:{false,true})sigpipe_prefix(kernel,vector,restart,terminate);
        }
#endif
    }
    expect(native::entry_stacks.accounting().leased_slots==0);
    expect(native::pipe_write_objects.accounting().allocations==native::pipe_write_objects.accounting().destructions);
    std::printf("PIPE_BLOCKING_WRITE_PASS checks=%zu private=%u hosted=1 native=0\n",checks,
#ifdef OK_BLOCKING_WRITE_PRIVATE_TEST
        1U
#else
        0U
#endif
    );
}
