// Actual native sleep adapter, scheduler and completion; hosted clock/copy only.
#if defined(OK_ARCH_TARGET_X86_64)
#define OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE 1
#define OK_ENABLE_TEST_POINTS 1
#endif
#include "native_clock_model.hpp"
#include "ok/user/native_sleep_interruption.hpp"
#include "ok/user/native_signal_target.hpp"
#include "ok/sched/pending_signals.hpp"
namespace
{
constexpr auto profile = lu::configured_native_profile();
using Codec = lu::Codec<profile>;
using Bridge = lu::NativeBridge<profile>;
using Sleep = native::LinuxSleepInterruption<profile>;
using Actions = native::SignalActionCodec<profile>;
using Queue = sched::PendingSignals<Actions::max_signal>;
constexpr uptr input = hw::user_begin + 256, output = hw::user_begin + 512;
constexpr u64 second = 1000000000;
usize checks{}, forms{};
void expect(bool value) { ++checks; CHECK(value); }
void arguments(hw::UserFrame &frame, lu::Operation op, const std::array<u64, 4> &values)
{
    const auto number = Codec::syscall_number(op); CHECK(number);
    if constexpr (profile == lu::Profile::x86_64)
    {
        frame.registers[14] = *number;
        constexpr usize slots[]{9,10,11,5};
        for (usize i=0;i<4;++i) frame.registers[slots[i]]=values[i];
    }
    else if constexpr (profile == lu::Profile::i386)
    {
        frame.registers[7] = *number;
        constexpr usize slots[]{4,6,5,1};
        for (usize i=0;i<4;++i) frame.registers[slots[i]]=values[i];
    }
    else
    {
        frame.registers[Codec::profile.number_register] = *number;
        for (usize i=0;i<4;++i) frame.registers[Codec::profile.argument_registers[i]]=values[i];
    }
}
sched::ProcessId create(Kernel &kernel)
{
    const auto made=kernel.scheduler().create_user_process("sleep-interruption",kernel.arch().make_user_context({hw::user_begin,hw::user_begin+8192})); CHECK(made);
    auto *p=kernel.scheduler().find(made.value());p->resources().personality=sched::UserPersonality::linux;
    auto &thread=p->threads()[0];CHECK(native::prepare_native_thread(thread));thread.native_active=true;
    thread.native_frame.pc=hw::user_begin+64;Bridge::set_stack_pointer(thread.native_frame,hw::user_begin+8192);
    CHECK(kernel.scheduler().set_runnable(p->pid()));CHECK(kernel.scheduler().activate_thread(p->pid(),thread.tid));
    return p->pid();
}
void retire(Kernel &kernel,sched::ProcessId pid)
{
    native::release_process(kernel,pid);CHECK(kernel.scheduler().exit_process(pid,0));CHECK(kernel.scheduler().reap_orphans());
}
void arm(Kernel &kernel,sched::ProcessId pid,lu::Operation op,bool absolute=false,uptr remainder=output)
{
    CHECK(kernel.scheduler().activate_thread(pid,pid));
    auto &thread=*kernel.scheduler().current_thread();
    const bool plain=op==lu::Operation::nanosleep||op==lu::Operation::nanosleep_time32;
    const auto format=Codec::profile.word_bytes==8||op==lu::Operation::clock_nanosleep_time64 ? lu::TimeFormat::kernel_timespec : lu::TimeFormat::legacy_timespec;
    const auto count=Codec::timespec_size(format);std::array<u8,16> bytes{};
    const u64 interval=2*second+123456789;
    const u64 requested=absolute ? mock::now+interval : interval;
    CHECK(Codec::encode_timespec({bytes.data(),count},format,{static_cast<i64>(requested/second),static_cast<i64>(requested%second)}));
    std::memcpy(mock::memory.data()+256,bytes.data(),count);
    arguments(thread.native_frame,op,plain ? std::array<u64,4>{input,remainder,0,0} : std::array<u64,4>{1,absolute ? 1U : 0U,input,remainder});
    bool complete=true;thread.syscall_pending=true;
    auto value=user::linux_bootstrap::dispatch<profile>(kernel,pid,thread.native_frame,complete,
        [&](hw::Syscall call,bool &done){return native::dispatch(kernel,pid,call,done);});
    expect(value==0&&!complete);expect(thread.state==sched::ProcessState::blocked&&thread.native_deadline.armed());
    expect(Sleep::owns(*kernel.scheduler().find(pid),thread,thread.wait_registration.ticket()));
}
Sleep::Completion complete(Kernel &kernel,sched::ProcessId pid,sched::WaitTicket ticket)
{
    auto &process=*kernel.scheduler().find(pid);auto &thread=process.threads()[0];
    return Sleep::complete(kernel.scheduler(),process,thread,ticket,mock::now,
        [&](uptr address,std::span<const u8> bytes){return hw::copy_to_user(process.resources().native_space,address,bytes.data(),bytes.size()).ok();},
        [&](i64 result){return native::set_result(process,thread.native_frame,result).ok();});
}
void returned(const sched::ThreadControlBlock &thread,u32 error)
{
    const auto result=Bridge::decode_return(thread.native_frame);expect(result&&result.value().error==error);
    expect(!thread.syscall_pending&&!thread.native_deadline.armed()&&!thread.wait_registration.active());
    expect(thread.state==sched::ProcessState::runnable&&thread.wait_reason==sched::ThreadWaitReason::none&&thread.wait_key==0);
}
void family(Kernel &kernel,lu::Operation op)
{
    if (!Codec::syscall_number(op)) return;
    ++forms;
    const bool plain=op==lu::Operation::nanosleep||op==lu::Operation::nanosleep_time32;
    for(unsigned mode=0;mode<(plain?5U:7U);++mode)
    {
        const auto pid=create(kernel);auto &thread=kernel.scheduler().find(pid)->threads()[0];
        const bool absolute=mode>=5;const auto rem=mode==2||mode==5 ? 0 : mode==3||mode==6 ? hw::user_end-1 : mode==1 ? input : output;
        std::fill(mock::memory.begin()+512,mock::memory.begin()+528,std::byte{0xa5});
        arm(kernel,pid,op,absolute,rem);const auto ticket=thread.wait_registration.ticket();
        const auto end=thread.native_deadline.deadline();const auto copies=mock::copies;const auto outputs=mock::outputs;
        // Completion cannot re-read changed or inaccessible original requests.
        std::fill(mock::memory.begin()+256,mock::memory.begin()+272,std::byte{0xff});mock::forbidden=mode==1 ? 0 : input;
        mock::now=mode==4 ? end : end-1123456789;
        const auto done=complete(kernel,pid,ticket);mock::forbidden=0;
        expect(done.outcome==(mode==4?Sleep::Outcome::timeout:Sleep::Outcome::interrupted));
        expect(mock::copies==copies);returned(thread,mode==4?0:mode==3?14:4);
        expect(thread.wait_registration.completion()==(mode==4?sched::WaitCompletion::timeout:sched::WaitCompletion::interrupted));
        if(mode==0||mode==1)
        {
            const auto size=Codec::timespec_size(done.request.format);std::array<u8,16> bytes{};
            std::memcpy(bytes.data(),mock::memory.data()+(mode==1?256:512),size);const auto value=Codec::decode_timespec({bytes.data(),size},done.request.format);
            expect(value&&value.value().seconds==1&&value.value().nanoseconds==123456789);
            if(size==8)for(usize i=520;i<528;++i)expect(mock::memory[i]==std::byte{0xa5});
            if(mode==1)for(usize i=512;i<528;++i)expect(mock::memory[i]==std::byte{0xa5});
        }
        else for(usize i=512;i<528;++i)expect(mock::memory[i]==std::byte{0xa5});
        expect(mock::outputs==outputs+((mode==0||mode==1||mode==3)?1:0));
        const auto old_outputs=mock::outputs;expect(complete(kernel,pid,ticket).outcome==Sleep::Outcome::lost);expect(mock::outputs==old_outputs);
        retire(kernel,pid);
    }
}
void ownership(Kernel &kernel,lu::Operation op)
{
    const auto pid=create(kernel);auto &p=*kernel.scheduler().find(pid);auto &t=p.threads()[0];arm(kernel,pid,op);
    const auto old=t.wait_registration.ticket();CHECK(kernel.scheduler().complete_wait(old,sched::WaitCompletion::cancelled));
    arm(kernel,pid,op);const auto ticket=t.wait_registration.ticket();const auto frame=t.native_frame;const auto n=mock::outputs;
    expect(complete(kernel,pid,old).outcome==Sleep::Outcome::lost);expect(mock::outputs==n&&t.wait_registration.pending(ticket));
    auto wrong=ticket;++wrong.thread;expect(complete(kernel,pid,wrong).outcome==Sleep::Outcome::lost);
    ++t.wait_key;expect(!Sleep::owns(p,t,ticket));--t.wait_key;
    arguments(t.native_frame,lu::Operation::getpid,{});expect(!Sleep::owns(p,t,ticket));t.native_frame=frame;
    t.native_active=false;expect(!Sleep::owns(p,t,ticket));t.native_active=true;
    t.syscall_pending=false;expect(!Sleep::owns(p,t,ticket));t.syscall_pending=true;
    mock::write_forbidden=output;const auto result=complete(kernel,pid,ticket);mock::write_forbidden=0;
    expect(result.outcome==Sleep::Outcome::interrupted&&result.result==-14);returned(t,14);retire(kernel,pid);
}
void queue_policy()
{
    Queue queue;CHECK(queue.reset({1,2,3}));sched::SignalActionTable actions;sched::SignalMask blocked;
    *actions.find(10)={.handler=hw::user_begin+64,.flags=0x10000000};
    CHECK(queue.enqueue({10,-6,3,4})==Queue::Enqueue::queued);blocked.set(10);
    auto supported=[](const sched::SignalAction &action){return !(action.flags&~u64{0x10000000});};
    expect(!native::sleep_interrupting_signal<profile>(queue,actions,blocked,supported));blocked.set(10,false);
    expect(native::sleep_interrupting_signal<profile>(queue,actions,blocked,supported));
    actions.find(10)->handler=1;expect(!native::sleep_interrupting_signal<profile>(queue,actions,blocked,supported));
    *actions.find(12)={.handler=hw::user_begin+128,.flags=0x10000000};CHECK(queue.enqueue({12,-6,3,4})==Queue::Enqueue::queued);
    expect(native::sleep_interrupting_signal<profile>(queue,actions,blocked,supported));
    expect(queue.pending_mask().contains(10)&&queue.pending_mask().contains(12));
}
#if defined(OK_ARCH_TARGET_X86_64)
void selected_runtime(Kernel &kernel,lu::Operation op)
{
    for(unsigned mode=0;mode<7;++mode)
    {
        const auto pid=create(kernel);auto &p=*kernel.scheduler().find(pid);auto &t=p.threads()[0];
        auto *actions=native::private_signal_table(kernel,pid);CHECK(actions);
        *actions->find(10)={.handler=hw::user_begin+128,.flags=0x04000000|0x10000000,.restorer=hw::user_begin+192};
        // Process mapping records carry their exact pool frame.
        CHECK(native::allocate(p.resources().native_pages,hw::user_begin,hw::page_read|hw::page_execute));
        auto *queue=native::private_signal_queue(t);CHECK(queue);
        // The pending-before-arm path must be selected despite the PCB block.
        if(mode==0) CHECK(queue->enqueue({10,-6,99,100})==native::PrivateSignalQueue::Enqueue::queued);
        std::fill(mock::memory.begin()+512,mock::memory.begin()+528,std::byte{0xa5});
        arm(kernel,pid,op);const auto ticket=t.wait_registration.ticket();const auto n=mock::entries;
        if(mode) CHECK(queue->enqueue({10,-6,99,100})==native::PrivateSignalQueue::Enqueue::queued);
        if(mode==1)
        {
            t.native_signal_mask.set(10);native::started=true;CHECK(native::step(kernel));
            expect(mock::entries==n&&t.wait_registration.pending(ticket)&&p.state()==sched::ProcessState::blocked);
            t.native_signal_mask.set(10,false);
        }
        if(mode==2)
        {
            actions->find(10)->handler=1;native::started=true;CHECK(native::step(kernel));
            expect(mock::entries==n&&t.wait_registration.pending(ticket));actions->find(10)->handler=hw::user_begin+128;
        }
        if(mode==3)
        {
            actions->find(10)->handler=1;*actions->find(12)=*actions->find(10);actions->find(12)->handler=hw::user_begin+128;
            CHECK(queue->enqueue({12,-6,99,100})==native::PrivateSignalQueue::Enqueue::queued);
        }
        if(mode==4)
        {
            expect(native::private_sleep_signal_ready(kernel,p,t));native::PrivateSignalQueue::Delivery previous;
            CHECK(queue->peek({},previous));CHECK(queue->consume(previous));const auto writes=mock::outputs;
            expect(!native::private_sleep_signal_complete(kernel,p,t));expect(t.wait_registration.pending(ticket)&&mock::outputs==writes);
            CHECK(queue->enqueue({10,-6,123,456})==native::PrivateSignalQueue::Enqueue::queued);
            expect(!queue->consume(previous));expect(t.wait_registration.pending(ticket));
        }
        if(mode==5) {actions->find(10)->handler=0;p.resources().native_pages.clear();}
        if(mode==6) mock::now=t.native_deadline.deadline();
        native::started=true;CHECK(native::step(kernel));
        if(mode==5)
        {
            expect(mock::entries==n&&p.state()==sched::ProcessState::zombie&&p.wait_status()==10);
            expect(!t.native_active&&!t.native_deadline.armed()&&!t.wait_registration.active());
            expect(native::entry_stacks.accounting().leased_slots==0);CHECK(kernel.scheduler().reap_orphans());continue;
        }
        expect(mock::entries==n+1&&mock::last_frame.pc==hw::user_begin+128);
        expect(t.wait_registration.completion()==(mode==6?sched::WaitCompletion::timeout:sched::WaitCompletion::interrupted)&&!t.syscall_pending&&!t.native_deadline.armed());
        expect(!queue->pending_mask().contains(10)&&t.native_signal_mask.contains(mode==3?12:10));
        if(mode==6)for(usize i=512;i<528;++i)expect(mock::memory[i]==std::byte{0xa5});
        p.resources().native_pages.clear();retire(kernel,pid);
    }
}
#endif
}
int main()
{
    static Kernel kernel;CHECK(kernel.boot({}));CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    const lu::Operation all[]{lu::Operation::nanosleep,lu::Operation::nanosleep_time32,lu::Operation::clock_nanosleep,lu::Operation::clock_nanosleep_time32,lu::Operation::clock_nanosleep_time64};
    for(auto op:all)family(kernel,op);
    const auto first=Codec::syscall_number(lu::Operation::nanosleep)?lu::Operation::nanosleep:lu::Operation::nanosleep_time32;
    ownership(kernel,first);queue_policy();
#if defined(OK_ARCH_TARGET_X86_64)
    selected_runtime(kernel,first);
#endif
    expect(native::entry_stacks.accounting().leased_slots==0&&native::runtime_status().ok());
    std::printf("SLEEP_INTERRUPTION_MODEL_PASS forms=%zu checks=%zu hosted=1 guest=0 hardware_isolation=0 smp=0\n",forms,checks);
}
