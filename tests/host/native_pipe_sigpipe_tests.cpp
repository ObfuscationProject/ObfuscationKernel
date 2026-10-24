// Actual runtime dispatch, owned waits and private signal delivery; mocked hardware.
#define OK_ENABLE_TEST_POINTS 1
#ifdef OK_SIGPIPE_PRIVATE_TEST
#define OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE 1
#endif
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_clock_model.hpp"
#include <source_location>
namespace
{
constexpr uptr site = hw::user_begin + 1024, array_address = hw::user_begin + 512;
constexpr uptr data = hw::user_begin + 3072, handler = hw::user_begin + 2048;
usize checks{};
void expect(bool ok, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!ok) { std::fprintf(stderr,"PIPE_SIGPIPE_FAIL line=%u checks=%zu\n",where.line(),checks); std::abort(); }
}
struct Group { sched::ProcessId pid{}; sched::ThreadId writer{}; std::array<i32, 2> fds{}; };
Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("pipe-sigpipe",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin+8192})); CHECK(made);
    Group g{made.value()}; auto *p = kernel.scheduler().find(g.pid);
    p->resources().personality = sched::UserPersonality::linux;
    auto creds = p->credentials(); creds.uid=1001; creds.euid=2002; p->set_credentials(creds);
    p->resources().native_space = {0x100000 + g.pid*4096, static_cast<u32>(g.pid%16), 1};
    CHECK(native::allocate(p->resources().native_pages, hw::user_begin, hw::page_read|hw::page_execute));
    auto peer = kernel.scheduler().create_thread(g.pid,p->threads()[0].context); CHECK(peer); g.writer=peer.value();
    for (auto &t : p->threads()) { CHECK(native::prepare_native_thread(t)); t.native_active=true; }
    p->threads()[0].native_active=false;
    CHECK(kernel.scheduler().set_runnable(g.pid)); CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));
    auto fds=kernel.posix().pipe(0); CHECK(fds); g.fds=fds.value();
    native::runtime_kernel=&kernel; native::started=true; native::last_pid=native::last_tid=0;
    return g;
}
void activate(Kernel &kernel, Group g, bool writer=false)
{ CHECK(kernel.scheduler().activate_thread(g.pid,writer?g.writer:g.pid)); }
void retire(Kernel &kernel, Group g)
{
    mock::forbidden=mock::write_forbidden=0;
    auto *p=kernel.scheduler().find(g.pid);
    if (p && p->state()!=sched::ProcessState::zombie && p->state()!=sched::ProcessState::exited)
    { native::release_process(kernel,g.pid); CHECK(kernel.scheduler().exit_process(g.pid,0)); }
    CHECK(kernel.scheduler().reap_orphans());
    expect(native::pipe_write_objects.accounting().live_objects==0);
    expect(native::pipe_write_objects.accounting().backing_pages==0);
    expect(native::runtime_status().ok());
}
void frame(Kernel &kernel, Group g, bool vector, usize count=3, i32 fd=-2, uptr pointer=data)
{
    auto *t=kernel.scheduler().find_thread(g.pid,g.writer); auto &f=t->native_frame;
    f={}; f.flags=0x202; f.sp=hw::user_begin+8192; f.pc=site+2;
    // A valid FXSAVE image is required by actual hosted sigreturn below.
    f.fp_state[0]=0x7f; f.fp_state[1]=3; f.fp_state[24]=0x80; f.fp_state[25]=0x1f;
    f.registers[14]=vector?20:1; f.registers[9]=static_cast<uptr>(fd==-2?g.fds[1]:fd);
    f.registers[10]=vector?array_address:pointer; f.registers[11]=vector?2:count;
    const std::array<native::PipeVector,2> values{{{pointer,count},{~uptr{0},0}}};
    std::memcpy(mock::memory.data()+array_address-hw::user_begin,values.data(),sizeof(values));
    mock::memory[site-hw::user_begin]=std::byte{0x0f}; mock::memory[site-hw::user_begin+1]=std::byte{0x05};
    mock::entry_trap={hw::TrapKind::syscall,256}; native::last_pid=native::last_tid=0;
}
void fill(Kernel &kernel, Group g, usize bytes=sched::pipe_capacity)
{
    activate(kernel,g); std::array<std::byte,sched::pipe_capacity> contents{};
    auto result=kernel.posix().write(g.fds[1],{contents.data(),bytes}); CHECK(result&&result.value()==bytes);
}
void close_reader(Kernel &kernel, Group g)
{ activate(kernel,g); CHECK(kernel.posix().close(g.fds[0])); }
i64 result(Kernel &kernel, Group g)
{ return static_cast<i64>(kernel.scheduler().find_thread(g.pid,g.writer)->native_frame.registers[14]); }
void run(Kernel &kernel, Group g)
{
    native::last_pid=native::last_tid=0; expect(native::step(kernel));
    auto *t=kernel.scheduler().find_thread(g.pid,g.writer); expect(t&&!t->syscall_pending);
    expect(t->native_frame.pc==site+2); expect(!native::pipe_write_object(g.pid,g.writer).pool);
}
#ifdef OK_SIGPIPE_PRIVATE_TEST
using Queue=native::PrivateSignalQueue;
Queue &queue(Kernel &kernel, Group g)
{ auto *q=native::private_signal_queue(*kernel.scheduler().find_thread(g.pid,g.writer)); CHECK(q); return *q; }
void action(Kernel &kernel, Group g, unsigned disposition, bool blocked, bool restart)
{
    auto *a=native::private_signal_table(kernel,g.pid)->find(13); CHECK(a);
    a->handler=disposition==0?handler:disposition==1?sched::SignalAction::ignored_handler:sched::SignalAction::default_handler;
    a->restorer=handler+4; a->flags=0x04000004|(restart?0x10000000:0);
    kernel.scheduler().find_thread(g.pid,g.writer)->native_signal_mask.set(13,blocked);
}
void pending(Kernel &kernel, Group g, bool expected, sched::PendingSignalInfo info={})
{
    auto &q=queue(kernel,g); expect(q.pending_mask().contains(13)==expected);
    auto *leader=kernel.scheduler().find_thread(g.pid,g.pid); auto *other=native::private_signal_queue(*leader); CHECK(other);
    expect(!other->pending_mask().contains(13));
    if (expected)
    {
        sched::SignalMask mask; for(u32 signal=1;signal<=64;++signal) if(signal!=13) mask.set(signal);
        Queue::Delivery got; expect(q.peek(mask,got));
        if (!info.signal) info={13,0,static_cast<u32>(g.pid),1001};
        expect(got.info==info);
    }
}
void caught(Kernel &kernel, Group g, sched::PendingSignalInfo info={})
{
    auto *p=kernel.scheduler().find(g.pid); auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
    t->native_signal_mask.set(13,false); activate(kernel,g,true);
    const auto original=t->native_frame; const auto allocations=native::pipe_write_objects.accounting().allocations;
    expect(!native::private_pipe_write_signal_ready(kernel,*p,*t));
    expect(native::private_signal_deliver(kernel,*p,*t)); expect(t->native_frame.pc==handler);
    expect(t->native_frame.registers[9]==13);
    std::array<std::byte,native::X86SignalFrame::prefix_bytes> image{};
    std::memcpy(image.data(),mock::memory.data()+t->native_frame.sp-hw::user_begin,image.size());
    using F=native::X86SignalFrame;
    if (!info.signal) info={13,0,static_cast<u32>(g.pid),1001};
    expect(F::get(image,F::info_offset,4)==13 && static_cast<i32>(F::get(image,F::info_offset+8,4))==info.code);
    expect(F::get(image,F::info_offset+16,4)==info.sender_pid && F::get(image,F::info_offset+20,4)==info.sender_uid);
    expect(F::get(image,F::context_offset+128,8)==site+2);
    expect(static_cast<i64>(F::get(image,F::context_offset+13*8,8))==-32);
    expect(!queue(kernel,g).pending_mask().contains(13));
    t->native_frame.sp+=8; t->native_frame.registers[14]=15; bool complete=true; i64 returned=-999;
    expect(native::private_signal_call(kernel,*p,*t,complete,returned)); expect(!complete&&returned==0);
    expect(t->native_frame.pc==site+2 && result(kernel,g)==-32);
    expect(t->native_frame.registers==original.registers);
    expect(native::pipe_write_objects.accounting().allocations==allocations && !native::pipe_write_object(g.pid,g.writer).pool);
    // Another user-entry boundary sees no replay of the completed write.
    mock::entry_trap={hw::TrapKind::timer}; native::last_pid=native::last_tid=0;
    expect(native::step(kernel)); expect(mock::last_frame.pc==site+2 && static_cast<i64>(mock::last_frame.registers[14])==-32);
    expect(!queue(kernel,g).pending_mask().contains(13));
}
void terminal(Kernel &kernel,bool vector,bool deferred,unsigned disposition,bool blocked,bool restart)
{
    auto g=create(kernel); action(kernel,g,disposition,blocked,restart); frame(kernel,g,vector);
    memory::ObjectHandle old{}; sched::WaitTicket ticket{}; native::SyscallOrigins::Identity origin{};
    if (deferred)
    {
        fill(kernel,g); expect(native::step(kernel)); auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
        old=native::pipe_write_object(g.pid,g.writer); expect(old.pool&&t->syscall_pending);
        ticket=t->wait_registration.ticket(); origin=native::syscall_origins.identity(*kernel.scheduler().find(g.pid),*t);
        expect(native::syscall_origins.live(origin)); pending(kernel,g,false);
    }
    close_reader(kernel,g); run(kernel,g); expect(result(kernel,g)==-32);
    expect(!native::syscall_origins.live(origin));
    if (deferred) { expect(!native::pipe_write_objects.resolve(old)); expect(!kernel.scheduler().complete_wait(ticket,sched::WaitCompletion::ready)); }
    pending(kernel,g,blocked||disposition!=1);
    auto *p=kernel.scheduler().find(g.pid); auto *t=kernel.scheduler().find_thread(g.pid,g.writer);
    if (blocked)
    {
        const auto original=t->native_frame; expect(native::private_signal_deliver(kernel,*p,*t));
        expect(native::SyscallOrigins::same_frame(original,t->native_frame)); pending(kernel,g,true);
        t->native_signal_mask.set(13,false);
    }
    if (disposition==0) caught(kernel,g);
    else if(disposition==1)
    { expect(native::private_signal_deliver(kernel,*p,*t)); pending(kernel,g,false); expect(result(kernel,g)==-32); }
    else
    { expect(!native::private_signal_deliver(kernel,*p,*t)); expect(p->state()==sched::ProcessState::zombie && p->wait_status()==13); }
    retire(kernel,g);
}
void coalescing(Kernel &kernel,bool vector,bool deferred)
{
    auto g=create(kernel); action(kernel,g,0,true,true); frame(kernel,g,vector);
    auto *t=kernel.scheduler().find_thread(g.pid,g.writer); t->native_signal_mask.set(10);
    const sched::PendingSignalInfo prior{13,-6,777,888}, unrelated{10,-6,333,444};
    expect(queue(kernel,g).enqueue(prior)==Queue::Enqueue::queued);
    expect(queue(kernel,g).enqueue(unrelated)==Queue::Enqueue::queued);
    Queue::Delivery before; sched::SignalMask mask; mask.set(10); expect(queue(kernel,g).peek(mask,before));
    if(deferred) { fill(kernel,g); expect(native::step(kernel)); expect(t->syscall_pending); }
    close_reader(kernel,g); run(kernel,g); expect(result(kernel,g)==-32); pending(kernel,g,true,prior);
    Queue::Delivery after; expect(queue(kernel,g).peek(mask,after)); expect(before==after);
    expect(queue(kernel,g).pending_mask().contains(10)); caught(kernel,g,prior);
    expect(queue(kernel,g).pending_mask().contains(10)); expect(queue(kernel,g).peek({},after)); expect(after.info==unrelated);
    retire(kernel,g);
}
void controls(Kernel &kernel,bool vector,unsigned mode)
{
    auto g=create(kernel); action(kernel,g,0,true,true);
    if(mode==0) { close_reader(kernel,g); frame(kernel,g,vector,0); }
    if(mode==1) { close_reader(kernel,g); frame(kernel,g,vector,3,-1); }
    if(mode==2) { close_reader(kernel,g); frame(kernel,g,vector,3,g.fds[1],hw::user_end-1); }
    if(mode==3) { fill(kernel,g,sched::pipe_capacity-1); CHECK(kernel.posix().fcntl(g.fds[1],posix::f_SETFL,posix::o_NONBLOCK));
        frame(kernel,g,vector,sched::pipe_capacity+1); }
    if(mode==4) { close_reader(kernel,g); frame(kernel,g,vector,3,g.fds[0]); }
    if(mode==5) { activate(kernel,g); CHECK(kernel.posix().close(g.fds[1])); frame(kernel,g,vector,3,g.fds[0]);
        kernel.scheduler().find_thread(g.pid,g.writer)->native_frame.registers[14]=vector?19:0; }
    if(mode>=6)
    {
        fill(kernel,g); frame(kernel,g,vector); expect(native::step(kernel));
        auto *t=kernel.scheduler().find_thread(g.pid,g.writer); expect(t->syscall_pending);
        if(mode==6) { mock::forbidden=data; close_reader(kernel,g); }
        else CHECK(kernel.scheduler().complete_wait(t->wait_registration.ticket(),sched::WaitCompletion::cancelled));
    }
    run(kernel,g);
    const i64 expected=mode==0?0:mode==1||mode==4?-9:mode==2||mode==6?-14:mode==3?1:mode==5?0:-125;
    expect(result(kernel,g)==expected); pending(kernel,g,false); retire(kernel,g);
}
#endif
}
int main()
{
    static_assert(lu::configured_native_profile()==lu::Profile::x86_64);
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
#ifdef OK_SIGPIPE_PRIVATE_TEST
    for(bool vector:{false,true})
    {
        for(bool deferred:{false,true})
        {
            for(unsigned disposition=0;disposition<3;++disposition)
                for(bool blocked:{false,true})
                    for(bool restart:{false,true}) terminal(kernel,vector,deferred,disposition,blocked,restart);
            coalescing(kernel,vector,deferred);
        }
        for(unsigned mode=0;mode<8;++mode) controls(kernel,vector,mode);
    }
#else
    for(bool vector:{false,true}) for(bool deferred:{false,true})
    {
        auto g=create(kernel); frame(kernel,g,vector);
        if(deferred) { fill(kernel,g); expect(native::step(kernel)); expect(native::pipe_write_object(g.pid,g.writer).pool); }
        close_reader(kernel,g); run(kernel,g); expect(result(kernel,g)==-32);
        mock::entry_trap={hw::TrapKind::timer}; native::last_pid=native::last_tid=0;
        const auto entries=mock::entries;
        expect(native::step(kernel)); expect(static_cast<i64>(mock::last_frame.registers[14])==-32);
        expect(mock::entries==entries+1 && kernel.scheduler().find(g.pid));
        expect(kernel.scheduler().find_thread(g.pid,g.writer)->native_active); retire(kernel,g);
    }
#endif
    expect(native::entry_stacks.accounting().leased_slots==0);
    expect(native::pipe_write_objects.accounting().allocations==native::pipe_write_objects.accounting().destructions);
    std::printf("PIPE_SIGPIPE_PASS checks=%zu private=%u hosted=1 native=0\n",checks,
#ifdef OK_SIGPIPE_PRIVATE_TEST
        1U
#else
        0U
#endif
    );
}
