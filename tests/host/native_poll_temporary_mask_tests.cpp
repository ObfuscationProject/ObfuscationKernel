// Actual private x86 runtime/scheduler; synthetic traps and user-copy backends.
#define OK_POLL_TEMP_MASK_MODEL 1
#define OK_ENABLE_TEST_POINTS 1
#define OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_clock_model.hpp"
#include <source_location>
namespace
{
usize checks{};
void expect(bool value, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "POLL_TEMP_MASK_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
constexpr uptr output = hw::user_begin + 128, timeout = hw::user_begin + 256;
constexpr uptr site = hw::user_begin + 1024, handler = hw::user_begin + 2048;
struct Group { sched::ProcessId pid{}; sched::ThreadId waiter{}; };
void put(uptr address, u64 value, usize bytes = 8)
{ for (usize i = 0; i < bytes; ++i) mock::memory[address - hw::user_begin + i] = std::byte(value >> (i * 8)); }
u64 get(uptr address, usize bytes = 8)
{
    u64 out{};
    for (usize i = 0; i < bytes; ++i) out |= static_cast<u64>(mock::memory[address - hw::user_begin + i]) << (i * 8);
    return out;
}
Group create(Kernel &kernel)
{
    mock::now = 3000000042;
    auto made = kernel.scheduler().create_user_process("poll-mask-model",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 8192})); CHECK(made);
    auto *p = kernel.scheduler().find(made.value());
    p->resources().personality = sched::UserPersonality::linux;
    p->resources().native_space = {0x100000 + made.value() * 4096, static_cast<u32>(made.value() % 16), 1};
    CHECK(native::allocate(p->resources().native_pages, hw::user_begin, hw::page_read | hw::page_write | hw::page_execute));
    auto peer = kernel.scheduler().create_thread(p->pid(), p->threads()[0].context); CHECK(peer);
    for (auto &thread : p->threads()) { CHECK(native::prepare_native_thread(thread)); thread.native_active = true; }
    p->threads()[0].native_active = false;
    CHECK(kernel.scheduler().set_runnable(p->pid()));
    CHECK(kernel.scheduler().activate_thread(p->pid(), p->pid())); CHECK(kernel.posix().activate(p->pid()));
    native::runtime_kernel = &kernel; native::started = true;
    return {p->pid(), peer.value()};
}
void retire(Kernel &kernel, Group group)
{
    native::release_process(kernel, group.pid);
    CHECK(kernel.scheduler().exit_process(group.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    expect(native::poll_objects.accounting().live_objects == 0);
    expect(native::runtime_status().ok());
}
void leader(Kernel &kernel, Group group)
{ CHECK(kernel.scheduler().activate_thread(group.pid, group.pid)); CHECK(kernel.posix().activate(group.pid)); }
constexpr uptr mask_address = hw::user_begin + 320, mask_pair = hw::user_begin + 352;
sched::SignalMask old_mask()
{ sched::SignalMask mask; mask.set(10); mask.set(14); return mask; }
sched::SignalMask temporary_mask()
{ sched::SignalMask mask; mask.set(12); return mask; }
void caught(Kernel &kernel, Group g, u32 signal = 10)
{
    auto &a = *native::private_signal_table(kernel,g.pid)->find(signal);
    a.handler = handler; a.restorer = handler+4; a.flags = 0x04000004 | 0x10000000;
    a.mask.set(15);
}
void enqueue(Kernel &kernel, Group g, u32 signal=10)
{
    auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    expect(native::private_signal_queue(*t)->enqueue({signal,-6,static_cast<u32>(g.pid),0})==native::PrivateSignalQueue::Enqueue::queued);
}
void invoke(Kernel &kernel, Group g, i32 fd, bool select, bool timed=true, unsigned error=0, uptr stack=hw::user_begin+8192)
{
    auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);auto &f=t->native_frame;
    f={};f.flags=0x202;f.sp=stack;f.pc=site+2;
    const auto fp=native::X86SignalFrame::reset_fp();
    for(usize i=0;i<fp.size();++i)f.fp_state[i]=static_cast<u8>(fp[i]);
    f.registers[14]=select?270:271;
    put(timeout,3);put(timeout+8,0);
    auto requested=temporary_mask();requested.set(9);requested.set(19);
    put(mask_address,requested.words[0]);put(mask_pair,mask_address);put(mask_pair+8,error==1?16:8);
    if(select)
    {
        put(output,u64{1}<<fd);f.registers[9]=error==3?~u64{0}:fd+1;f.registers[10]=output;
        f.registers[7]=timed?timeout:0;f.registers[6]=mask_pair;
    }
    else
    {
        put(output,static_cast<u32>(fd),4);put(output+4,OK_POLLIN,2);put(output+6,99,2);
        f.registers[9]=output;f.registers[10]=error==3?33:1;f.registers[11]=timed?timeout:0;
        f.registers[5]=mask_address;f.registers[7]=error==1?16:8;
    }
    if(error==2)mock::forbidden=mask_address;
    if(error==4)mock::forbidden=output;
    if(error==5)put(timeout+8,1000000000);
    if(error==6){put(timeout,0);put(timeout+8,0);}
    mock::entry_trap={hw::TrapKind::syscall,256};native::last_pid=native::last_tid=0;
    expect(native::step(kernel));mock::forbidden=0;
}
u64 saved_mask(const hw::UserFrame &frame)
{ return get(frame.sp+native::X86SignalFrame::mask_offset); }
void sigreturn(Kernel &kernel, Group g, hw::UserFrame frame)
{
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    t->native_frame=frame;t->native_frame.sp+=8;t->native_frame.registers[14]=15;
    bool complete=true;i64 result=-999;
    expect(native::private_signal_call(kernel,*p,*t,complete,result));expect(!complete && result==0);
}
void interruption(Kernel &kernel,bool select,bool nested)
{
    auto g=create(kernel);auto pipes=kernel.posix().pipe(0);CHECK(pipes);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    t->native_signal_mask=old_mask();caught(kernel,g);caught(kernel,g,14);
    enqueue(kernel,g); // Pending before entry, blocked only by original mask.
    invoke(kernel,g,pipes.value()[0],select);
    expect(t->syscall_pending && t->native_signal_mask.words==temporary_mask().words);
    const auto object=native::poll_object(g.pid,g.waiter);
    expect(native::poll_signal_masks.find(*t)->phase==native::PollSignalMasks::Phase::waiting);
    const auto copies=mock::copies;put(mask_address,~u64{0});mock::forbidden=mask_address;
    mock::entry_trap={hw::TrapKind::timer};expect(native::step(kernel));mock::forbidden=0;
    expect(mock::copies==copies);const auto outer=t->native_frame;
    expect(outer.pc==handler && saved_mask(outer)==old_mask().words[0]);
    expect(get(outer.sp+native::X86SignalFrame::context_offset+104)==static_cast<u64>(-4));
    expect(!native::poll_objects.resolve(object) && !t->syscall_pending && !native::poll_signal_masks.find(*t));
    auto handler_mask=temporary_mask();handler_mask.set(10);handler_mask.set(15);
    expect(t->native_signal_mask.words==handler_mask.words);
    if(nested)
    {
        enqueue(kernel,g,14);expect(native::private_signal_deliver(kernel,*p,*t));
        const auto inner=t->native_frame;expect(saved_mask(inner)==handler_mask.words[0]);
        expect(t->native_signal_mask.contains(14));sigreturn(kernel,g,inner);
        expect(t->native_signal_mask.words==handler_mask.words && t->native_frame.sp==outer.sp);
    }
    // User edits to uc_sigmask survive: no kernel restoration remains after commit.
    sched::SignalMask edited;edited.set(2);put(outer.sp+native::X86SignalFrame::mask_offset,edited.words[0]);
    sigreturn(kernel,g,outer);expect(t->native_signal_mask.words==edited.words);
    expect(t->native_frame.pc==site+2 && t->native_frame.registers[14]==static_cast<u64>(-4));
    retire(kernel,g);
}
void ordinary(Kernel &kernel,bool select,unsigned mode)
{
    auto g=create(kernel);auto pipes=kernel.posix().pipe(0);CHECK(pipes);
    auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);t->native_signal_mask=old_mask();caught(kernel,g);
    if(mode==0){const std::array<std::byte,1> byte{std::byte{'R'}};CHECK(kernel.posix().write(pipes.value()[1],byte));}
    invoke(kernel,g,pipes.value()[0],select,true,mode==1?6:0);
    if(mode>=2)
    {
        expect(t->syscall_pending);enqueue(kernel,g);
        const auto *wait=native::poll_objects.resolve(native::poll_object(g.pid,g.waiter)).value();
        if(mode==2)mock::now=wait->timeout.deadline();
        else
        {
            leader(kernel,g);const std::array<std::byte,1> byte{std::byte{'R'}};CHECK(kernel.posix().write(pipes.value()[1],byte));
            if(mode==4)mock::write_forbidden=select?output:output+6;
        }
        mock::entry_trap={hw::TrapKind::timer};expect(native::step(kernel));mock::write_forbidden=0;
        expect(native::private_signal_queue(*t)->pending_mask().contains(10));
    }
    expect(!t->syscall_pending && !native::poll_signal_masks.find(*t));
    expect(t->native_signal_mask.words==old_mask().words);
    expect(t->native_frame.pc==site+2);
    expect(t->native_frame.registers[14]==static_cast<u64>(mode==4?-14:(mode==0||mode==3)?1:0));
    retire(kernel,g);
}
void invalid(Kernel &kernel,bool select,unsigned error)
{
    auto g=create(kernel);auto pipes=kernel.posix().pipe(0);CHECK(pipes);
    auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);t->native_signal_mask=old_mask();
    invoke(kernel,g,pipes.value()[0],select,true,error);
    expect(t->native_frame.registers[14]==static_cast<u64>((error==2||error==4)?-14:-22));
    expect(!t->syscall_pending && !native::poll_signal_masks.find(*t));
    expect(t->native_signal_mask.words==old_mask().words);
    retire(kernel,g);
}
void interrupted_fault(Kernel &kernel)
{
    auto g=create(kernel);auto pipes=kernel.posix().pipe(0);CHECK(pipes);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    t->native_signal_mask=old_mask();caught(kernel,g);invoke(kernel,g,pipes.value()[0],false);
    enqueue(kernel,g);mock::write_forbidden=output+6;
    expect(native::private_poll_signal_complete(kernel,*p,*t));mock::write_forbidden=0;
    expect(t->native_frame.registers[14]==static_cast<u64>(-14));
    expect(t->native_signal_mask.words==old_mask().words && !native::poll_signal_masks.find(*t));
    expect(native::private_signal_deliver(kernel,*p,*t));expect(t->native_frame.pc==site+2);
    expect(native::private_signal_queue(*t)->pending_mask().contains(10));retire(kernel,g);
}
void ignored_after_completion(Kernel &kernel)
{
    auto g=create(kernel);auto pipes=kernel.posix().pipe(0);CHECK(pipes);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    t->native_signal_mask=old_mask();caught(kernel,g);invoke(kernel,g,pipes.value()[0],true);
    enqueue(kernel,g);expect(native::private_poll_signal_complete(kernel,*p,*t));
    expect(native::poll_signal_masks.saved(*t));
    native::private_signal_table(kernel,g.pid)->find(10)->handler=1;
    expect(native::private_signal_deliver(kernel,*p,*t));
    expect(t->native_signal_mask.words==old_mask().words && !native::poll_signal_masks.find(*t));
    retire(kernel,g);
}
void fatal(Kernel &kernel,bool frame_fault)
{
    auto g=create(kernel);auto pipes=kernel.posix().pipe(0);CHECK(pipes);
    auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);t->native_signal_mask=old_mask();
    caught(kernel,g);invoke(kernel,g,pipes.value()[0],false);enqueue(kernel,g);
    if(frame_fault)
    {
        native::X86SignalFrame::Placement place;CHECK(native::X86SignalFrame::place(t->native_frame.sp,hw::user_begin,hw::user_end,place));
        mock::write_forbidden=place.address;
    }
    else native::private_signal_table(kernel,g.pid)->find(10)->handler=0;
    mock::entry_trap={hw::TrapKind::timer};expect(native::step(kernel));mock::write_forbidden=0;
    expect(!t->native_active && !native::poll_signal_masks.find(*t));
    expect(t->native_signal_mask.words==old_mask().words);
    CHECK(kernel.scheduler().reap_orphans());expect(!kernel.scheduler().find(g.pid));
}
void cleanup(Kernel &kernel,unsigned mode)
{
    auto g=create(kernel);auto pipes=kernel.posix().pipe(0);CHECK(pipes);
    auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);t->native_signal_mask=old_mask();
    invoke(kernel,g,pipes.value()[0],false);const auto object=native::poll_object(g.pid,g.waiter);
    const auto ticket=t->wait_registration.ticket();
    if(mode==0)
    {
        native::cancel_poll(kernel,object);expect(t->native_signal_mask.words==old_mask().words);
    }
    else if(mode==1)
    {
        // A stale ticket must not restore the live registration's temporary mask.
        auto stale=ticket;++stale.generation;native::poll_signal_masks.complete(*t,object,stale,false);
        expect(t->native_signal_mask.words==temporary_mask().words && native::poll_signal_masks.find(*t));
    }
    else if(mode==2)
    {
        expect(native::retire_native_thread(*t).ok());expect(!native::poll_signal_masks.find(*t));
        expect(t->native_signal_mask.words==old_mask().words);
    }
    else
    {
        // Exec snapshots the original mask before replacing TCB/leases; the
        // committed hook drops all old peer records without touching new TCBs.
        expect(native::poll_signal_masks.exec_mask(*t).words==old_mask().words);
        native::poll_signal_masks.discard_process(g.pid);expect(!native::poll_signal_masks.find(*t));
        t->native_signal_mask=old_mask();
    }
    retire(kernel,g);
}

void blocks_until_normal_return(Kernel &kernel,bool select)
{
    auto g=create(kernel);auto pipes=kernel.posix().pipe(posix::o_NONBLOCK);CHECK(pipes);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    caught(kernel,g,12);invoke(kernel,g,pipes.value()[0],select);enqueue(kernel,g,12);
    expect(!native::private_poll_signal_ready(kernel,*p,*t));
    mock::entry_trap={hw::TrapKind::timer};const auto entries=mock::entries;expect(native::step(kernel));
    expect(mock::entries==entries && t->syscall_pending);
    leader(kernel,g);const std::array<std::byte,1> byte{std::byte{'R'}};CHECK(kernel.posix().write(pipes.value()[1],byte));
    expect(native::step(kernel));expect(t->native_frame.pc==handler && saved_mask(t->native_frame)==0);
    expect(get(t->native_frame.sp+native::X86SignalFrame::context_offset+104)==1);
    expect(!native::poll_signal_masks.find(*t));retire(kernel,g);
}
void nested_wait(Kernel &kernel)
{
    auto g=create(kernel);auto pipes=kernel.posix().pipe(0);CHECK(pipes);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    t->native_signal_mask=old_mask();caught(kernel,g);caught(kernel,g,14);enqueue(kernel,g);
    invoke(kernel,g,pipes.value()[0],false);mock::entry_trap={hw::TrapKind::timer};expect(native::step(kernel));
    const auto outer=t->native_frame;const auto outer_mask=t->native_signal_mask;
    invoke(kernel,g,pipes.value()[0],true,true,0,outer.sp-256);
    expect(t->syscall_pending && native::poll_signal_masks.find(*t)->original.words==outer_mask.words);
    enqueue(kernel,g,14);expect(native::private_poll_signal_complete(kernel,*p,*t));
    expect(native::private_signal_deliver(kernel,*p,*t));const auto inner=t->native_frame;
    expect(saved_mask(inner)==outer_mask.words[0]);sigreturn(kernel,g,inner);
    expect(t->native_signal_mask.words==outer_mask.words);sigreturn(kernel,g,outer);
    expect(t->native_signal_mask.words==old_mask().words && !native::poll_signal_masks.find(*t));retire(kernel,g);
}

template<lu::Profile P>
void adapter_masks(Kernel &kernel,usize number_slot,std::array<int,6> slots)
{
    using C=lu::Codec<P>;using M=native::SignalMaskCodec<P>;using O=lu::Operation;
    constexpr usize w=C::profile.word_bytes;constexpr bool big=C::profile.big_endian;
    auto g=create(kernel);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    auto encode=[&](uptr address,u64 value,usize width=C::profile.word_bytes){for(usize i=0;i<width;++i)mock::memory[address-hw::user_begin+i]=std::byte(value>>(8*(big?width-i-1:i)));};
    sched::SignalMask requested;for(u32 s:{1U,9U,M::stop,32U,63U,64U,65U,128U})requested.set(s);
    const auto bytes=M::encode(requested),expected=M::sanitize(requested);
    for(O op:{O::ppoll,O::ppoll_time64,O::pselect6,O::pselect6_time64})
    {
        const auto number=C::syscall_number(op);if(!number)continue;
        const bool select=op==O::pselect6||op==O::pselect6_time64;
        const usize tw=(op==O::ppoll_time64||op==O::pselect6_time64)?8:w;
        auto call=[&](unsigned error=0)
        {
            CHECK(kernel.posix().activate(g.pid));CHECK(kernel.scheduler().activate_thread(g.pid,g.waiter));
            t->native_signal_mask=old_mask();
            for(usize i=0;i<bytes.size();++i)mock::memory[mask_address-hw::user_begin+i]=bytes[i];
            encode(timeout,3,tw);encode(timeout+tw,error==1?1000000000:0,tw);
            encode(mask_pair,mask_address);encode(mask_pair+w,(error==2||error==1)?M::bytes+1:M::bytes);
            const u64 count=error>=3?0xffffffff:0;
            std::array<u64,6> args=select?std::array<u64,6>{count,0,0,0,timeout,mask_pair}:
                std::array<u64,6>{0,count,timeout,mask_address,(error==2||error==1)?M::bytes+1:M::bytes,0};
            auto &f=t->native_frame;f={};f.pc=site+4;f.sp=hw::user_begin+4096;f.registers[number_slot]=*number;
            for(usize i=0;i<6;++i)if(slots[i]>=0)f.registers[slots[i]]=args[i];else encode(f.sp+16+(i-4)*4,args[i],4);
            mock::forbidden=error==3?mask_address:0;bool complete=true;
            const i64 result=user::linux_bootstrap::dispatch<P>(kernel,g.pid,f,complete,
                [&](hw::Syscall c,bool &done){return native::dispatch(kernel,g.pid,c,done);});mock::forbidden=0;
            if(error)
            {
                expect(result==(error==3?-14:-22)&&complete);
                expect(!native::poll_signal_masks.find(*t)&&t->native_signal_mask.words==old_mask().words);
            }
            else
            {
                expect(result==0&&!complete);
                expect(t->native_signal_mask.words==expected.words);
                expect(native::poll_signal_masks.find(*t)->original.words==old_mask().words);
                native::cancel_poll(kernel,native::poll_object(g.pid,g.waiter));
                expect(t->native_signal_mask.words==old_mask().words&&!native::poll_signal_masks.find(*t));
            }
        };
        call();for(unsigned error=1;error<=4;++error)call(error);
    }
    retire(kernel,g);
}
} // namespace
int main()
{
    static Kernel kernel;CHECK(kernel.boot({}));CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for(bool select:{false,true})
    {
        for(bool nested:{false,true})interruption(kernel,select,nested);
        for(unsigned mode=0;mode<5;++mode)ordinary(kernel,select,mode);
        for(unsigned error=1;error<6;++error)invalid(kernel,select,error);
    }
    for(bool select:{false,true})blocks_until_normal_return(kernel,select);
    nested_wait(kernel);interrupted_fault(kernel);ignored_after_completion(kernel);
    for(bool frame_fault:{false,true})fatal(kernel,frame_fault);
    for(unsigned mode=0;mode<4;++mode)cleanup(kernel,mode);
    adapter_masks<lu::Profile::i386>(kernel,7,{4,6,5,1,0,2});
    adapter_masks<lu::Profile::x86_64>(kernel,14,{9,10,11,5,7,6});
    adapter_masks<lu::Profile::aarch64>(kernel,8,{0,1,2,3,4,5});
    adapter_masks<lu::Profile::arm32>(kernel,7,{0,1,2,3,4,5});
    adapter_masks<lu::Profile::rv64>(kernel,17,{10,11,12,13,14,15});
    adapter_masks<lu::Profile::rv32>(kernel,17,{10,11,12,13,14,15});
    adapter_masks<lu::Profile::loongarch64>(kernel,11,{4,5,6,7,8,9});
    adapter_masks<lu::Profile::mips>(kernel,2,{4,5,6,7,-1,-1});
    adapter_masks<lu::Profile::mips64>(kernel,2,{4,5,6,7,8,9});
    adapter_masks<lu::Profile::ppc>(kernel,0,{3,4,5,6,7,8});
    expect(native::entry_stacks.accounting().leased_slots==0);
    expect(native::poll_objects.accounting().allocations==native::poll_objects.accounting().destructions);
    std::printf("POLL_TEMP_MASK_PASS hosted=1 native=0 checks=%zu\n",checks);
}
