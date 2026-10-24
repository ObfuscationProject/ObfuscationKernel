// Actual copied kernel runtime/dispatcher/scheduler; hosted memory/traps only.
#define OK_ENABLE_TEST_POINTS 1
#define OK_NATIVE_MODEL_MEMORY_BYTES 65536
#include "native_clock_model.hpp"
#include <source_location>
namespace
{
constexpr uptr output=hw::user_begin+512, vectors_address=hw::user_begin+1024;
constexpr uptr input=hw::user_begin+8192;
usize checks{},cases{};
void expect(bool value,std::source_location where=std::source_location::current())
{
    ++checks;
    if(!value){std::fprintf(stderr,"PTY_RUNTIME_FAIL line=%u checks=%zu\n",where.line(),checks);std::abort();}
}
struct Group {sched::ProcessId pid{};sched::ThreadId peer{};std::array<i32,2> fds{};};
void activate(Kernel &kernel,Group g,sched::ThreadId tid=0)
{CHECK(kernel.posix().activate(g.pid));CHECK(kernel.scheduler().activate_thread(g.pid,tid?tid:g.pid));}
hw::Syscall call(u64 number,i32 fd,uptr address,usize count)
{return {number,{static_cast<uptr>(fd),address,count,0,0,0}};}
i64 invoke(Kernel &kernel,Group g,hw::Syscall request,bool &complete,sched::ThreadId tid=0)
{
    activate(kernel,g,tid);auto *thread=kernel.scheduler().current_thread();thread->syscall_pending=true;
    const auto result=native::dispatch(kernel,g.pid,request,complete);
    if(complete)thread->syscall_pending=false;
    return result;
}
u16 description(Kernel &kernel,Group g,i32 fd)
{const auto *entry=kernel.scheduler().find(g.pid)->fd_table().entry(fd);CHECK(entry&&entry->used);return entry->description;}
std::array<i32,2> pair(Kernel &kernel,Group g,u32 flags=0)
{
    bool complete=true;activate(kernel,g);auto *thread=kernel.scheduler().current_thread();thread->syscall_pending=true;
    const auto result=native::dispatch(kernel,g.pid,{OK_SYS_PTY_PAIR,{output,flags,0,0,0,0}},complete);
    expect(result==0&&complete);thread->syscall_pending=false;
    std::array<i32,2> fds{};std::memcpy(fds.data(),mock::memory.data()+output-hw::user_begin,sizeof(fds));
    expect(fds[0]>=3&&fds[1]>=3&&fds[0]!=fds[1]);return fds;
}
Group create(Kernel &kernel,u32 flags=0)
{
    const auto made=kernel.scheduler().create_user_process("native-pty-runtime",
        kernel.arch().make_user_context({hw::user_begin,hw::user_begin+65536}));CHECK(made);
    Group g{made.value()};auto *process=kernel.scheduler().find(g.pid);
    process->resources().personality=sched::UserPersonality::native;
    process->resources().native_space={0x100000+g.pid*4096,static_cast<u32>(g.pid%16),1};
    const auto peer=kernel.scheduler().create_thread(g.pid,process->threads()[0].context);CHECK(peer);g.peer=peer.value();
    for(auto &thread:process->threads()){CHECK(native::prepare_native_thread(thread));thread.native_active=true;}
    CHECK(kernel.scheduler().set_runnable(g.pid));g.fds=pair(kernel,g,flags);
    for(usize index=0;index<32768;++index)mock::memory[input-hw::user_begin+index]=static_cast<std::byte>((index*29+7)%251);
    return g;
}
void retire(Kernel &kernel,Group g)
{
    mock::forbidden=mock::write_forbidden=mock::partial_output=0;
    auto *process=kernel.scheduler().find(g.pid);
    if(process&&process->state()!=sched::ProcessState::zombie&&process->state()!=sched::ProcessState::exited)
    {native::release_process(kernel,g.pid);CHECK(kernel.scheduler().exit_process(g.pid,0));}
    CHECK(kernel.scheduler().reap_orphans());
    expect(native::pty_wait_objects.accounting().live_objects==0&&native::pty_wait_objects.accounting().backing_pages==0);
    expect(!kernel.scheduler().pty_audit().live_pairs&&kernel.scheduler().pty_audit().cleared_outside_live_bytes);
    expect(native::runtime_status().ok());++cases;
}
std::span<const std::byte> text(std::string_view value)
{return {reinterpret_cast<const std::byte *>(value.data()),value.size()};}
void write(Kernel &kernel,Group g,i32 fd,std::string_view value)
{activate(kernel,g);const auto wrote=kernel.posix().write(fd,text(value));CHECK(wrote&&wrote.value()==value.size());}
native::PtyWait snapshot(Group g)
{auto resolved=native::pty_wait_objects.resolve(native::pty_wait_object(g.pid,g.peer));CHECK(resolved);return *resolved.value();}
sched::WaitTicket arm(Kernel &kernel,Group g,bool writing=false,usize count=3,bool vector=false)
{
    bool complete=true;const auto result=invoke(kernel,g,call(writing?(vector?OK_SYS_WRITEV:OK_SYS_WRITE):(vector?OK_SYS_READV:OK_SYS_READ),
        g.fds[writing?0:1],vector?vectors_address:(writing?input:output),vector?3:count),complete,g.peer);
    expect(result==-11&&!complete);const auto wait=snapshot(g);auto *thread=kernel.scheduler().find_thread(g.pid,g.peer);
    expect(thread->wait_registration.pending(wait.ticket));expect(wait.description==description(kernel,g,g.fds[writing?0:1]));
    expect(thread->wait_reason==(writing?sched::ThreadWaitReason::pty_write:sched::ThreadWaitReason::pty_read));
    expect(thread->wait_key==native::pty_wait_key(wait.endpoint));return wait.ticket;
}
i64 resume(Kernel &kernel,Group g,bool done=true)
{
    activate(kernel,g,g.peer);bool complete=true;
    const auto result=native::dispatch(kernel,g.pid,call(OK_SYS_READ,-1,hw::user_end-1,7000),complete);
    expect(complete==done);if(complete)kernel.scheduler().find_thread(g.pid,g.peer)->syscall_pending=false;return result;
}
void set_vectors(usize count,bool writing)
{
    const auto base=writing?input:output;
    const usize first=count>2?2:count;
    const std::array<native::PipeVector,3> vectors{{{base,first},{~uptr{0},0},{base+first,count-first}}};
    std::memcpy(mock::memory.data()+vectors_address-hw::user_begin,vectors.data(),sizeof(vectors));
}
void read_identity(Kernel &kernel,bool vector,bool duplicate)
{
    auto g=create(kernel);if(vector)set_vectors(3,false);const auto id=description(kernel,g,g.fds[1]);
    if(duplicate)CHECK(kernel.posix().duplicate(g.fds[1]));
    const auto refs=kernel.scheduler().open_description(id)->references;
    const auto ticket=arm(kernel,g,false,3,vector);expect(kernel.scheduler().open_description(id)->references==refs+1);
    const auto endpoint=snapshot(g).endpoint;activate(kernel,g);auto other=pair(kernel,g);
    CHECK(kernel.posix().close(g.fds[1]));CHECK(kernel.posix().duplicate_to(other[1],g.fds[1]));
    if(vector)std::memset(mock::memory.data()+vectors_address-hw::user_begin,0xff,sizeof(native::PipeVector)*3);
    write(kernel,g,other[0],"wrong");auto *thread=kernel.scheduler().find_thread(g.pid,g.peer);
    expect(thread->wait_registration.pending(ticket));
    CHECK(kernel.scheduler().complete_wait(ticket,sched::WaitCompletion::ready));expect(resume(kernel,g,false)==-11);
    const auto rearmed=snapshot(g);expect(rearmed.endpoint==endpoint&&rearmed.ticket.generation>ticket.generation);
    expect(!kernel.scheduler().complete_wait(ticket,sched::WaitCompletion::ready));
    write(kernel,g,g.fds[0],"abc");expect(resume(kernel,g)==3);
    expect(mock::memory[output-hw::user_begin]==std::byte{'a'}&&mock::memory[output-hw::user_begin+2]==std::byte{'c'});
    expect(!native::pty_wait_object(g.pid,g.peer).pool);
    expect(duplicate?kernel.scheduler().open_description(id)->references==1:!kernel.scheduler().open_description(id));
    activate(kernel,g);std::array<std::byte,5> remaining{};auto read=kernel.posix().read(other[1],remaining);
    expect(read&&read.value()==5&&remaining[0]==std::byte{'w'});retire(kernel,g);
}
void read_fault(Kernel &kernel,bool vector,unsigned mode,bool blocked)
{
    auto g=create(kernel);if(vector)set_vectors(3,false);if(blocked)arm(kernel,g,false,3,vector);
    write(kernel,g,g.fds[0],"abc");if(mode==0)mock::write_forbidden=output;else if(mode==1)mock::forbidden=output;else mock::partial_output=1;
    if(blocked)expect(resume(kernel,g)==-14);else
    {bool complete=true;expect(invoke(kernel,g,call(vector?OK_SYS_READV:OK_SYS_READ,g.fds[1],vector?vectors_address:output,vector?3:3),complete)==-14&&complete);}
    mock::forbidden=mock::write_forbidden=mock::partial_output=0;activate(kernel,g);std::array<std::byte,3> left{};
    auto read=kernel.posix().read(g.fds[1],left);expect(read&&read.value()==3&&left[0]==std::byte{'a'}&&left[2]==std::byte{'c'});
    expect(kernel.scheduler().open_description(description(kernel,g,g.fds[1]))->references==1);retire(kernel,g);
}
void drain(Kernel &kernel,Group g,usize count,usize offset,bool verify=true)
{
    activate(kernel,g);std::array<std::byte,sched::pipe_capacity> bytes{};auto read=kernel.posix().read(g.fds[1],{bytes.data(),count});CHECK(read&&read.value()==count);
    if(verify)for(usize index=0;index<count;++index)expect(bytes[index]==mock::memory[input-hw::user_begin+offset+index]);
}
void write_progress(Kernel &kernel,bool vector,unsigned terminal)
{
    constexpr usize total=2*sched::pipe_capacity+17;auto g=create(kernel);if(vector)set_vectors(total,true);
    arm(kernel,g,true,total,vector);const auto saved=snapshot(g);expect(saved.committed==sched::pipe_capacity);
    if(vector)std::memset(mock::memory.data()+vectors_address-hw::user_begin,0xff,sizeof(native::PipeVector)*3);
    activate(kernel,g);auto other=pair(kernel,g);CHECK(kernel.posix().duplicate_to(other[0],g.fds[0]));
    if(terminal==0)
    {
        drain(kernel,g,sched::pipe_capacity,0);mock::forbidden=input;expect(resume(kernel,g,false)==-11);
        expect(snapshot(g).committed==2*sched::pipe_capacity);expect(snapshot(g).ticket.generation>saved.ticket.generation);
        drain(kernel,g,sched::pipe_capacity,sched::pipe_capacity);expect(resume(kernel,g)==total);drain(kernel,g,17,2*sched::pipe_capacity);
    }
    else if(terminal==1)
    {drain(kernel,g,sched::pipe_capacity,0);mock::forbidden=input+sched::pipe_capacity;expect(resume(kernel,g)==sched::pipe_capacity);}
    else if(terminal==2)
    {CHECK(kernel.posix().close(g.fds[1]));expect(resume(kernel,g)==sched::pipe_capacity);}
    else
    {CHECK(kernel.scheduler().complete_wait(saved.ticket,sched::WaitCompletion::interrupted));expect(resume(kernel,g)==sched::pipe_capacity);drain(kernel,g,sched::pipe_capacity,0);}
    expect(!native::pty_wait_object(g.pid,g.peer).pool&&!kernel.scheduler().open_description(saved.description));retire(kernel,g);
}
void controls(Kernel &kernel)
{
    auto g=create(kernel);bool complete=true;const auto before=native::pty_wait_objects.accounting().allocations;
    for(const auto number:{OK_SYS_READ,OK_SYS_WRITE,OK_SYS_READV,OK_SYS_WRITEV})
        expect(invoke(kernel,g,call(number,g.fds[0],hw::user_end-1,0),complete)==0&&complete);
    CHECK(kernel.posix().fcntl(g.fds[1],posix::f_SETFL,posix::o_NONBLOCK));
    expect(invoke(kernel,g,call(OK_SYS_READ,g.fds[1],output,3),complete)==-11&&complete);
    expect(native::pty_wait_objects.accounting().allocations==before);
    CHECK(kernel.posix().fcntl(g.fds[1],posix::f_SETFL,0));auto ticket=arm(kernel,g);CHECK(kernel.scheduler().complete_wait(ticket,sched::WaitCompletion::interrupted));
    expect(resume(kernel,g)==-4);activate(kernel,g);CHECK(kernel.posix().close(g.fds[0]));
    expect(invoke(kernel,g,call(OK_SYS_READ,g.fds[1],output,3),complete)==0&&complete);
    expect(invoke(kernel,g,call(OK_SYS_WRITE,g.fds[1],input,3),complete)==-5&&complete);retire(kernel,g);
    g=create(kernel);activate(kernel,g);CHECK(kernel.posix().close(g.fds[1]));
    expect(invoke(kernel,g,call(OK_SYS_READ,g.fds[0],output,3),complete)==-5&&complete);
    expect(invoke(kernel,g,call(OK_SYS_WRITE,g.fds[0],input,3),complete)==-5&&complete);retire(kernel,g);
}
void stale(Kernel &kernel,unsigned mode)
{
    auto g=create(kernel);auto ticket=arm(kernel,g);const auto saved=snapshot(g);auto *thread=kernel.scheduler().find_thread(g.pid,g.peer);
    CHECK(kernel.scheduler().complete_wait(ticket,sched::WaitCompletion::ready));
    if(mode==0)thread->native_frame.registers[3]^=1;
    if(mode==1)thread->native_frame.fp_state[7]^=1;
    if(mode==2)kernel.scheduler().find(g.pid)->resources().native_space.generation++;
    if(mode==3)thread->native_frame.pc++;
    if(mode==4)
    {activate(kernel,g,g.peer);auto replacement=kernel.scheduler().register_current_wait(sched::ThreadWaitReason::deadline,123);CHECK(replacement);ticket=replacement.value();}
    native::prune_pty_waits(kernel);expect(!native::pty_wait_object(g.pid,g.peer).pool);
    expect(kernel.scheduler().open_description(saved.description)->references==1);
    if(mode==4)expect(thread->wait_registration.pending(ticket)&&thread->wait_reason==sched::ThreadWaitReason::deadline);
    else expect(thread->syscall_pending);
    retire(kernel,g);
}
void lifecycle(Kernel &kernel,unsigned mode)
{
    auto g=create(kernel);arm(kernel,g);const auto saved=snapshot(g);activate(kernel,g);CHECK(kernel.posix().close(g.fds[1]));
    if(mode==0)native::release_process(kernel,g.pid);
    else if(mode==1)native::finish(kernel,g.pid,0);
    else if(mode==2)
    {CHECK(native::retire_native_thread(*kernel.scheduler().find_thread(g.pid,g.peer)));CHECK(kernel.scheduler().exit_thread(g.pid,g.peer));native::prune_pty_waits(kernel);}
    else
    {auto *p=kernel.scheduler().find(g.pid);native::ExecRetirement retirement{*p,g.pid};
        auto replaced=native::replace_entry_stacks(native::entry_stacks,*p,[&]{return kernel.posix().exec(g.pid,p->threads()[0].context,p->resources().memory);});
        CHECK(replaced&&replaced.committed);retirement.consume(kernel,*p);}
    expect(!kernel.scheduler().open_description(saved.description)&&!native::pty_wait_object(g.pid,g.peer).pool);retire(kernel,g);
}
void pair_controls(Kernel &kernel)
{
    auto g=create(kernel);bool complete=true;const auto open=kernel.scheduler().find(g.pid)->fd_table().open_count();
    for(unsigned mode=0;mode<5;++mode)
    {
        if(mode==0)mock::write_forbidden=output;
        if(mode==1)mock::partial_output=1;
        const auto request=hw::Syscall{OK_SYS_PTY_PAIR,{mode==2?~uptr{0}:output,mode==3?1u:0u,mode==4?1u:0u,0,0,0}};
        expect(invoke(kernel,g,request,complete)<0&&complete);mock::write_forbidden=mock::partial_output=0;
        expect(kernel.scheduler().find(g.pid)->fd_table().open_count()==open&&kernel.scheduler().pty_audit().live_pairs==1);
    }
    auto *process=kernel.scheduler().find(g.pid);process->resources().personality=sched::UserPersonality::linux;
    expect(invoke(kernel,g,{OK_SYS_PTY_PAIR,{output,0,0,0,0,0}},complete)==-38&&complete);
    for(const auto number:{OK_SYS_READ,OK_SYS_WRITE,OK_SYS_READV,OK_SYS_WRITEV})
        expect(invoke(kernel,g,call(number,g.fds[0],output,0),complete)==-95&&complete);
    process->resources().personality=sched::UserPersonality::native;retire(kernel,g);
}
i16 poll_immediate(Kernel &kernel,Group g,i32 fd,i16 events)
{
    ok_pollfd request{fd,events,99};std::memcpy(mock::memory.data()+output-hw::user_begin,&request,sizeof(request));
    bool complete=true;const auto result=invoke(kernel,g,{OK_SYS_POLL,{output,1,0,0,0,0}},complete);
    std::memcpy(&request,mock::memory.data()+output-hw::user_begin,sizeof(request));
    expect(complete&&result==(request.revents?1:0));return request.revents;
}
void poll_readiness(Kernel &kernel)
{
    auto g=create(kernel);expect(poll_immediate(kernel,g,g.fds[1],OK_POLLIN|OK_POLLOUT)==OK_POLLOUT);
    expect(poll_immediate(kernel,g,g.fds[1],0)==0);std::array<std::byte,sched::pipe_capacity> bytes{};
    activate(kernel,g);CHECK(kernel.posix().write(g.fds[0],bytes));
    expect(poll_immediate(kernel,g,g.fds[0],OK_POLLOUT)==0);
    expect(poll_immediate(kernel,g,g.fds[1],OK_POLLIN)==OK_POLLIN);drain(kernel,g,1,0,false);
    expect(poll_immediate(kernel,g,g.fds[0],OK_POLLOUT)==OK_POLLOUT);
    activate(kernel,g);CHECK(kernel.posix().close(g.fds[0]));
    expect(poll_immediate(kernel,g,g.fds[1],0)==(OK_POLLHUP|OK_POLLERR));
    expect(poll_immediate(kernel,g,g.fds[1],OK_POLLIN)==(OK_POLLIN|OK_POLLHUP|OK_POLLERR));
    retire(kernel,g);g=create(kernel);activate(kernel,g);CHECK(kernel.posix().close(g.fds[1]));
    expect(poll_immediate(kernel,g,g.fds[0],0)==OK_POLLHUP);retire(kernel,g);
}
void poll_retained(Kernel &kernel,unsigned mode)
{
    auto g=create(kernel);const auto id=description(kernel,g,g.fds[1]);
    ok_pollfd request{g.fds[1],OK_POLLIN,99};std::memcpy(mock::memory.data()+output-hw::user_begin,&request,sizeof(request));
    bool complete=true;expect(invoke(kernel,g,{OK_SYS_POLL,{output,1,mode==1?1u:~uptr{0},0,0,0}},complete,g.peer)==0&&!complete);
    const auto object=native::poll_object(g.pid,g.peer);auto resolved=native::poll_objects.resolve(object);CHECK(resolved);
    const auto ticket=resolved.value()->ticket;expect(kernel.scheduler().open_description(id)->references==2);
    activate(kernel,g);auto other=pair(kernel,g);CHECK(kernel.posix().close(g.fds[1]));CHECK(kernel.posix().duplicate_to(other[1],g.fds[1]));
    request={other[1],OK_POLLOUT,77};std::memcpy(mock::memory.data()+output-hw::user_begin,&request,sizeof(request));
    write(kernel,g,other[0],"wrong");expect(native::poll_ready(kernel,*resolved.value()).value()==0);
    if(mode==1)mock::now+=1000000;else write(kernel,g,g.fds[0],"p");
    if(mode==2)mock::partial_output=1;
    kernel.scheduler().find_thread(g.pid,g.pid)->native_active=false;native::started=true;native::last_pid=native::last_tid=0;
    static_cast<void>(native::step(kernel));mock::partial_output=0;
    const auto *thread=kernel.scheduler().find_thread(g.pid,g.peer);
    expect(!native::poll_objects.resolve(object)&&!kernel.scheduler().open_description(id));
    expect(!thread->syscall_pending&&static_cast<i64>(thread->native_frame.registers[0])==(mode==2?-14:mode==1?0:1));
    expect(!kernel.scheduler().complete_wait(ticket,sched::WaitCompletion::ready));
    std::memcpy(&request,mock::memory.data()+output-hw::user_begin,sizeof(request));
    expect(request.fd==other[1]&&request.events==OK_POLLOUT);
    if(mode!=2)expect(request.revents==static_cast<i16>(mode==1?0:OK_POLLIN));
    retire(kernel,g);
}
void select_refusal(Kernel &kernel)
{
    auto g=create(kernel);activate(kernel,g);native::SelectRequest request{};
    request.sets.count=static_cast<usize>(g.fds[1]+1);request.sets.word_bytes=8;
    request.sets.interests[0]=u32{1}<<g.fds[1];request.sets.output[0]=output;request.readiness.immediate=true;
    bool complete=true;expect(native::begin_select(kernel,g.pid,request,complete)==-38&&complete);
    expect(kernel.scheduler().open_description(description(kernel,g,g.fds[1]))->references==1);
    expect(!native::poll_object(g.pid,g.pid).pool);retire(kernel,g);
}
void allocation_and_registration(Kernel &kernel,bool writing,bool progress)
{
    auto g=create(kernel);if(writing&&!progress)
    {activate(kernel,g);std::array<std::byte,sched::pipe_capacity> bytes{};CHECK(kernel.posix().write(g.fds[0],bytes));}
    CHECK(native::activate_pool());std::array<memory::ObjectHandle,native::pty_wait_capacity> pressure{};
    for(auto &object:pressure){auto allocated=native::pty_wait_objects.create();CHECK(allocated);object=allocated.value();}
    bool complete=true;const auto id=description(kernel,g,g.fds[writing?0:1]);
    const auto result=invoke(kernel,g,call(writing?OK_SYS_WRITE:OK_SYS_READ,g.fds[writing?0:1],writing?input:output,writing?8192:3),complete,g.peer);
    expect(complete&&result==(progress?static_cast<i64>(sched::pipe_capacity):-12));
    expect(kernel.scheduler().open_description(id)->references==1&&!native::pty_wait_object(g.pid,g.peer).pool);
    for(auto object:pressure)CHECK(native::pty_wait_objects.destroy(object));
    CHECK(native::pty_wait_objects.trim_empty_pages());
    if(writing)drain(kernel,g,sched::pipe_capacity,0,progress);
    retire(kernel,g);
}
void direct_frame_refusal(Kernel &kernel)
{
    auto g=create(kernel);arm(kernel,g);const auto saved=snapshot(g);write(kernel,g,g.fds[0],"old");
    auto *thread=kernel.scheduler().find_thread(g.pid,g.peer);thread->native_frame.registers[4]=123;
    const auto replaced=thread->native_frame;expect(resume(kernel,g,false)==-125);
    expect(native::SyscallOrigins::same_frame(thread->native_frame,replaced)&&thread->syscall_pending);
    expect(!native::pty_wait_object(g.pid,g.peer).pool&&kernel.scheduler().open_description(saved.description)->references==1);
    activate(kernel,g);std::array<std::byte,3> bytes{};auto read=kernel.posix().read(g.fds[1],bytes);
    expect(read&&read.value()==3&&bytes[0]==std::byte{'o'});retire(kernel,g);
}
void endpoint_generation(Kernel &kernel)
{
    auto g=create(kernel);const auto ticket=arm(kernel,g);auto resolved=native::pty_wait_objects.resolve(native::pty_wait_object(g.pid,g.peer));CHECK(resolved);
    ++resolved.value()->endpoint.generation;write(kernel,g,g.fds[0],"gen");
    expect(!kernel.scheduler().find_thread(g.pid,g.peer)->wait_registration.pending(ticket));expect(resume(kernel,g)==-9);
    activate(kernel,g);std::array<std::byte,3> bytes{};const auto read=kernel.posix().read(g.fds[1],bytes);
    expect(read&&read.value()==3&&bytes[0]==std::byte{'g'});retire(kernel,g);
}
void registration_conflict(Kernel &kernel)
{
    auto g=create(kernel);activate(kernel,g,g.peer);auto *thread=kernel.scheduler().current_thread();
    const auto ticket=thread->wait_registration.arm(g.pid,g.peer);CHECK(ticket);
    bool complete=true;expect(invoke(kernel,g,call(OK_SYS_READ,g.fds[1],output,3),complete,g.peer)==-16&&complete);
    expect(thread->wait_registration.pending(ticket.value())&&!native::pty_wait_object(g.pid,g.peer).pool);
    expect(kernel.scheduler().open_description(description(kernel,g,g.fds[1]))->references==1);
    thread->wait_registration.cancel();retire(kernel,g);
}
void lease_mismatch(Kernel &kernel)
{
    auto g=create(kernel);arm(kernel,g);auto *thread=kernel.scheduler().find_thread(g.pid,g.peer);
    const auto top=thread->kernel_stack_top;thread->kernel_stack_top+=16;native::prune_pty_waits(kernel);
    expect(!native::pty_wait_object(g.pid,g.peer).pool&&thread->syscall_pending);
    thread->kernel_stack_top=top;retire(kernel,g);
}
void failed_exec(Kernel &kernel)
{
    auto g=create(kernel);const auto ticket=arm(kernel,g);const auto object=native::pty_wait_object(g.pid,g.peer);
    std::strcpy(reinterpret_cast<char *>(mock::memory.data()+4096),"/no-such-native-pty-image");
    bool complete=true;expect(invoke(kernel,g,{OK_SYS_EXECVE,{hw::user_begin+4096,0,0,0,0,0}},complete)<0&&complete);
    expect(native::pty_wait_object(g.pid,g.peer)==object&&kernel.scheduler().find_thread(g.pid,g.peer)->wait_registration.pending(ticket));
    write(kernel,g,g.fds[0],"exe");expect(resume(kernel,g)==3);retire(kernel,g);
}
void nonblocking_write(Kernel &kernel)
{
    auto g=create(kernel,posix::o_NONBLOCK);bool complete=true;const auto before=native::pty_wait_objects.accounting().allocations;
    expect(invoke(kernel,g,call(OK_SYS_WRITE,g.fds[0],input,8192),complete)==sched::pipe_capacity&&complete);
    expect(invoke(kernel,g,call(OK_SYS_WRITE,g.fds[0],~uptr{0},3),complete)==-14&&complete);
    // A valid but currently inaccessible payload is not touched while full.
    mock::forbidden=input;expect(invoke(kernel,g,call(OK_SYS_WRITE,g.fds[0],input,3),complete)==-11&&complete);mock::forbidden=0;
    expect(native::pty_wait_objects.accounting().allocations==before);drain(kernel,g,sched::pipe_capacity,0);retire(kernel,g);
}
void actual_step(Kernel &kernel)
{
    auto g=create(kernel);arm(kernel,g);write(kernel,g,g.fds[0],"xyz");
    kernel.scheduler().find_thread(g.pid,g.pid)->native_active=false;
    native::started=true;native::last_pid=native::last_tid=0;const auto entries=mock::entries;
    expect(native::step(kernel));const auto *thread=kernel.scheduler().find_thread(g.pid,g.peer);
    expect(!thread->syscall_pending&&static_cast<i64>(thread->native_frame.registers[0])==3&&mock::entries==entries);
    expect(!native::pty_wait_object(g.pid,g.peer).pool&&mock::memory[output-hw::user_begin]==std::byte{'x'});retire(kernel,g);
}
}
int main()
{
    static Kernel kernel;CHECK(kernel.boot({}));CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for(bool vector:{false,true})
    {
        for(bool duplicate:{false,true})read_identity(kernel,vector,duplicate);
        for(unsigned mode=0;mode<3;++mode)for(bool blocked:{false,true})read_fault(kernel,vector,mode,blocked);
        for(unsigned terminal=0;terminal<4;++terminal)write_progress(kernel,vector,terminal);
    }
    controls(kernel);for(unsigned mode=0;mode<5;++mode)stale(kernel,mode);
    for(unsigned mode=0;mode<4;++mode)lifecycle(kernel,mode);
    pair_controls(kernel);actual_step(kernel);direct_frame_refusal(kernel);poll_readiness(kernel);
    for(unsigned mode=0;mode<3;++mode)poll_retained(kernel,mode);
    select_refusal(kernel);allocation_and_registration(kernel,false,false);
    allocation_and_registration(kernel,true,false);allocation_and_registration(kernel,true,true);
    endpoint_generation(kernel);registration_conflict(kernel);lease_mismatch(kernel);failed_exec(kernel);nonblocking_write(kernel);
    const auto stats=native::pty_wait_objects.accounting();expect(stats.allocations==stats.destructions&&!stats.live_objects&&!stats.backing_pages);
    expect(!native::entry_stacks.accounting().leased_slots);
    std::printf("PTY_RUNTIME_PASS actual_runtime=1 hosted=1 guest=0 cases=%zu checks=%zu allocations=%llu\n",cases,checks,static_cast<unsigned long long>(stats.allocations));
}
