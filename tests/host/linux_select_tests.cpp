// Select models use the same actual adapter/runtime and hosted mappings as poll.
#define OK_SELECT_TESTS
#include "linux_poll_tests.cpp"
namespace {
native::PollWait &selected_wait(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : native::poll_waits)
        if (entry.pid == pid && entry.tid == tid)
        {
            auto resolved = native::poll_objects.resolve(entry.object);
            CHECK(resolved);
            CHECK(resolved.value()->pid == pid && resolved.value()->tid == tid);
            return *resolved.value();
        }
    CHECK(false);
    std::abort();
}

template <lu::Profile P>
void selects(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<int,6> slots)
{
    using O=lu::Operation; using C=lu::Codec<P>; using S=lu::SelectCodec<P>;
    constexpr usize w=C::profile.word_bytes; constexpr bool big=C::profile.big_endian;
    constexpr uptr r=hw::user_begin+512,s=hw::user_begin+640,e=hw::user_begin+768,t=hw::user_begin+1024;
    constexpr uptr pack=hw::user_begin+2048,old=hw::user_begin+2560;
    auto *process=kernel.scheduler().find(pid); auto &thread=process->threads()[0];
    constexpr auto numbers=[] {
        if constexpr(P==lu::Profile::i386)return std::array<int,4>{82,142,308,413};
        if constexpr(P==lu::Profile::x86_64)return std::array<int,4>{23,-1,270,-1};
        if constexpr(P==lu::Profile::arm32)return std::array<int,4>{-1,142,335,413};
        if constexpr(P==lu::Profile::mips)return std::array<int,4>{-1,4142,4301,4413};
        if constexpr(P==lu::Profile::mips64)return std::array<int,4>{-1,5022,5260,-1};
        if constexpr(P==lu::Profile::ppc)return std::array<int,4>{82,142,280,413};
        if constexpr(P==lu::Profile::rv32)return std::array<int,4>{-1,-1,-1,413};
        return std::array<int,4>{-1,-1,72,-1};
    }();
    constexpr std::array<O,4> operations{O::select,O::_newselect,O::pselect6,O::pselect6_time64};
    for(usize i=0;i<4;++i) {const auto number=C::syscall_number(operations[i]);CHECK(numbers[i]<0?!number:(number && *number==static_cast<u32>(numbers[i])));}

    auto put=[&](uptr address,usize width,u64 value) {
        for(usize i=0;i<width;++i)mock::memory[address-hw::user_begin+i]=std::byte(value>>(8*(big?width-i-1:i)));
    };
    auto get=[&](uptr address,usize width) {
        u64 value{};for(usize i=0;i<width;++i)value|=u64(std::to_integer<u8>(mock::memory[address-hw::user_begin+i]))<<(8*(big?width-i-1:i));return value;
    };
    native::PollWait golden{};golden.select_sets=true;golden.big_endian=big;golden.sets.count=32;
    golden.sets.word_bytes=w;golden.sets.output[0]=s;golden.ready_sets[0]=0x80008183;
    CHECK(native::poll_output(process->resources().native_space,golden));
    constexpr std::array<u8,8> little{0x83,0x81,0,0x80,0,0,0,0};
    for(usize i=0;i<w;++i)CHECK(mock::memory[s-hw::user_begin+i]==std::byte(little[big?w-1-i:i]));
    bool done=true,bad_stack=false;
    auto call=[&](O op,std::array<u64,6> args) {
        const auto nr=C::syscall_number(op);CHECK(nr);hw::UserFrame frame{};frame.registers[number_slot]=*nr;
        frame.sp=bad_stack?0:hw::user_begin+3072;
        if constexpr(S::old_select)if(op==O::select) {
            for(usize i=0;i<5;++i)put(old+i*w,w,args[i]);
            args={old,0,0,0,0,0};
        }
        for(usize i=0;i<6;++i)if(slots[i]>=0)frame.registers[slots[i]]=static_cast<uptr>(args[i]);else put(hw::user_begin+3072+16+(i-4)*4,4,args[i]);
        done=true;return user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
            [&](hw::Syscall c,bool &complete){return native::dispatch(kernel,pid,c,complete);});
    };
    auto retire=[&] {native::cancel_poll(kernel,pid);thread.wait_reason=sched::ThreadWaitReason::none;thread.wait_key=0;CHECK(kernel.scheduler().set_runnable(pid));CHECK(kernel.posix().activate(pid));};
    auto regular=kernel.posix().open("/select-model",posix::o_CREAT|posix::o_RDWR|posix::o_TRUNC,0600);CHECK(regular);
    const auto fd=regular.value();const u32 bit=u32{1}<<fd;
    CHECK(kernel.scheduler().install_descriptor(process->resources(),31,process->fd_table().entry(fd)->description));
    for(O op:{O::select,O::_newselect,O::pselect6,O::pselect6_time64}) {
        if(!C::syscall_number(op))continue;
        std::fprintf(stderr,"select model %s operation %u\n",C::profile.name,static_cast<unsigned>(op));
        const bool precise=op==O::pselect6||op==O::pselect6_time64;
        const usize tw=op==O::pselect6_time64?8:w;
        mock::memory.fill(std::byte{0x55});mock::now=3000000042;mock::clock_step=0;
        auto stamp=[&](i64 sec,u64 sub){put(t,tw,static_cast<u64>(sec));put(t+tw,tw,sub);};
        auto sets=[&](u64 read,u64 write,u64 except){put(r,w,read);put(s,w,write);put(e,w,except);};
        auto invoke=[&](u64 n=32,uptr rp=hw::user_begin+512,uptr wp=hw::user_begin+640,uptr ep=hw::user_begin+768,uptr tp=hw::user_begin+1024,uptr ap=0){return call(op,{n,rp,wp,ep,tp,ap});};
        stamp(0,0);sets(bit,bit,bit);CHECK(invoke()==2 && done && get(r,w)==bit && get(s,w)==bit && get(e,w)==0);
        CHECK(get(r+w,1)==0x55 && get(s+w,1)==0x55); // Later libc words are untouched.
        sets(0,u64{1}<<31,0);CHECK(invoke()==1 && get(s,w)==(u64{1}<<31));
        // Independent byte goldens: bit31 in big-endian32/64 and little-endian32/64.
        for(usize i=0;i<w;++i)CHECK(mock::memory[s-hw::user_begin+i]==std::byte(i==(big?w-4:3)?0x80:0));
        sets(~u64{0},0,0);CHECK(invoke(1)==0 && get(r,w)==0);
        sets(0,2,0);CHECK(invoke(2)==1 && get(s,w)==2);
        for(usize i=0;i<w;++i)CHECK(mock::memory[s-hw::user_begin+i]==std::byte(i==(big?w-1:0)?2:0));
        sets(~u64{0},~u64{0},~u64{0});CHECK(invoke(0,1,1,1)==0 && done);CHECK(get(r,w)==(w==4?0xffffffffULL:~u64{0}));
        sets(0,(u64{1}<<31)|2,0);CHECK(invoke(31)==1 && get(s,w)==2);
        sets(0,u64{1}<<31,0);CHECK(invoke(0x7fffffff)==1); // Clamp allocated table capacity.
        sets(0,2,0);CHECK(invoke(0xffffffffULL)==-22);if constexpr(w==8)CHECK(invoke((u64{1}<<32)|2)==1);
        sets(u64{1}<<30,2,0);CHECK(invoke()==-9 && get(s,w)==2);
        sets(0,2,u64{1}<<30);CHECK(invoke()==-9); // Invalid exception FD wins over ready stdout.
        sets(bit,bit,0);mock::readonly_begin=s;mock::readonly_end=s+w;
        CHECK(invoke()==-14 && get(r,w)==bit && get(s,w)==bit);mock::readonly_begin=mock::readonly_end=0;
        sets(bit,0,bit);CHECK(invoke(32,r,r,r)==2 && get(r,w)==0); // All snapshots precede read/write/except output.
        sets(bit,bit,bit);mock::readonly_begin=r;mock::readonly_end=r+w;
        const auto outputs=mock::outputs;CHECK(invoke()==-14 && mock::outputs==outputs+1 && get(e,w)==bit);
        mock::readonly_begin=mock::readonly_end=0;
        sets(bit,0,0);mock::partial_output=1;CHECK(invoke()==-14);mock::partial_output=0;
        stamp(-1,0);CHECK(invoke(0xffffffffULL,1,1,1)==-22);
        mock::forbidden=t;CHECK(invoke(0xffffffffULL,1,1,1)==-14);mock::forbidden=0;
        if(precise) {
            stamp(0,0);put(pack,w,0);put(pack+w,w,~u64{0});CHECK(invoke(0,1,1,1,t,pack)==0);
            mock::forbidden=pack;const auto bad_pack=invoke(0xffffffffULL,1,1,1,1,pack);
            CHECK(bad_pack==-14 && mock::last_input==pack);mock::forbidden=0;
            put(pack,w,hw::user_begin+2304);put(pack+w,w,lu::PollCodec<P>::signal_bytes+1);
            stamp(0,100);CHECK(invoke(0,0,0,0,t,pack)==-22 && get(t+tw,tw)==100);
            put(pack+w,w,lu::PollCodec<P>::signal_bytes);mock::forbidden=hw::user_begin+2304;
            CHECK(invoke(0,0,0,0,t,pack)==-14 && get(t+tw,tw)==100);mock::forbidden=0;
            CHECK(invoke(0,0,0,0,t,pack)==-38 && get(t+tw,tw)==100);
            if constexpr(P==lu::Profile::mips) {
                bad_stack=true;CHECK(invoke()==-14);bad_stack=false;
                mock::forbidden=hw::user_begin+3092;CHECK(invoke()==-14);mock::forbidden=0;
            }
        } else {
            stamp(-1,2000000);CHECK(invoke(0,0,0,0)==0 && !done);retire();
            stamp(-1,1000000);CHECK(invoke(0,0,0,0)==0 && done && get(t,tw)==(w==4?0xffffffffULL:~u64{0}) && get(t+tw,tw)==1000000);
            stamp(1,static_cast<u64>(-1000000LL));CHECK(invoke(0,0,0,0)==0 && get(t,tw)==1);
            stamp(1,static_cast<u64>(-1LL));CHECK(invoke()==-22);
            stamp(w==4?0x7fffffffLL:__INT64_MAX__,1000000);CHECK(invoke()==-22);
            stamp(w==4?-2147483648LL:(-__INT64_MAX__-1),static_cast<u64>(-1000000LL));
            sets(0,2,0);CHECK(invoke()==1); // Target signed-long underflow wraps to maximum.
        }
        // Microsecond floor and nanosecond copyback, including post-core errors.
        mock::clock_step=precise?3:3001;stamp(0,17);sets(0,2,0);
        CHECK(invoke()==1 && get(t+tw,tw)==(precise?14:13));
        stamp(0,17);CHECK(invoke(32,1,0,0)==-14 && get(t+tw,tw)==(precise?14:13));
        stamp(0,17);sets(u64{1}<<30,0,0);CHECK(invoke()==-9 && get(t+tw,tw)==(precise?14:13));
        stamp(0,17);sets(0,2,0);mock::readonly_begin=t;mock::readonly_end=t+2*tw;
        CHECK(invoke()==1 && get(t+tw,tw)==17);mock::readonly_begin=mock::readonly_end=0;mock::clock_step=0;
        // Timeout writes last, even when aliasing an output set.
        stamp(1,0);CHECK(invoke(0,0,0,0)==0 && !done);retire();
        mock::clock_step=1000000000;stamp(bit,17);CHECK(invoke(32,t+(big?tw-w:0),0,0,t)==1 && get(t,tw)==bit-1 && get(t+tw,tw)==17);mock::clock_step=0;
        // A copied, retained reader remains selected after user changes and FD reuse.
        auto pair=kernel.posix().pipe();CHECK(pair);const auto rd=pair.value()[0],wr=pair.value()[1];
        const auto desc=process->fd_table().entry(rd)->description,writer=process->fd_table().entry(wr)->description;
        sets(u32{1}<<rd,0,0);stamp(0,17);mock::readonly_begin=r;mock::readonly_end=r+w;
        CHECK(invoke()==0 && !done);mock::readonly_begin=mock::readonly_end=0;retire();
        sets(u32{1}<<rd,0,0);CHECK(invoke(32,r,0,0,0)==0 && !done);
        CHECK(kernel.scheduler().open_description(desc)->references==2);const auto copies=mock::copies;
        put(r,w,2);CHECK(kernel.scheduler().close_descriptor(process->resources(),rd));
        CHECK(kernel.scheduler().install_descriptor(process->resources(),rd,writer));
        const std::byte byte{42};CHECK(kernel.scheduler().write_pipe(kernel.scheduler().open_description(writer)->pipe,{&byte,1},true));
        CHECK(native::poll_ready(kernel,selected_wait(pid,thread.tid)).value()==1 && mock::copies==copies);
        CHECK(native::poll_output(process->resources().native_space,selected_wait(pid,thread.tid)) && get(r,w)==(u32{1}<<rd));
        retire();CHECK(!kernel.scheduler().open_description(desc));
        stamp(0,0);sets(0,u32{1}<<wr,u32{1}<<wr);CHECK(invoke()==1 && get(s,w)==(u32{1}<<wr) && get(e,w)==0);
        sets(0,0,u32{1}<<wr);stamp(0,17);CHECK(invoke()==0 && !done);retire();
        CHECK(kernel.posix().close(rd) && kernel.posix().close(wr));
        pair=kernel.posix().pipe();CHECK(pair);CHECK(kernel.posix().close(pair.value()[1]));
        const auto eof=pair.value()[0];stamp(0,0);sets(u32{1}<<eof,0,u32{1}<<eof);
        CHECK(invoke()==1 && get(r,w)==(u32{1}<<eof) && get(e,w)==0);
        sets(0,0,u32{1}<<eof);stamp(0,17);CHECK(invoke()==0 && !done);retire();CHECK(kernel.posix().close(eof));
        stamp(0,17);CHECK(invoke(0,1,1,1)==0 && !done);CHECK(selected_wait(pid,thread.tid).timeout.deadline()==mock::now+(precise?17:17000));
        CHECK(invoke(0,1,1,1)==-22);retire(); // Duplicate wait owner.
        if(op==O::pselect6_time64 && w==4) {stamp(0,0x1234567800000011ULL);CHECK(invoke(0)==0 && !done && selected_wait(pid,thread.tid).timeout.deadline()==mock::now+17);retire();}
        if(tw==8) {stamp(__INT64_MAX__,precise?999999999:999999);CHECK(invoke(0)==0 && !done && selected_wait(pid,thread.tid).timeout.deadline()==lu::ktime_maximum);retire();}
        if constexpr(S::old_select)if(op==O::select) {mock::forbidden=old;CHECK(invoke()==-14 && mock::last_input==old);mock::forbidden=0;}
    }
    CHECK(kernel.posix().close(31) && kernel.posix().close(fd));
    std::printf("PASS Linux select %s: raw signatures, word/endian sets, copy order, time, readiness and pins\n",C::profile.name);
}

void select_lifetime(Kernel &kernel,sched::ProcessId pid)
{
    auto *process=kernel.scheduler().find(pid);auto &thread=process->threads()[0];
    auto pair=kernel.posix().pipe();CHECK(pair);const auto rd=pair.value()[0],wr=pair.value()[1];
    const auto reader=process->fd_table().entry(rd)->description,writer=process->fd_table().entry(wr)->description;
    native::SelectRequest request{};request.sets={{hw::user_begin,0,0},{u32{1}<<rd,0,0},32,8};
    bool done=true;const auto before=kernel.scheduler().open_description(reader)->references;
    // Retain failure after an earlier successful pin rolls that pin back.
    request.sets.interests[0]|=u32{1}<<wr;auto *second=kernel.scheduler().open_description(writer);const auto saved=second->references;second->references=~usize{0};
    CHECK(native::begin_select(kernel,pid,request,done)==-75 && kernel.scheduler().open_description(reader)->references==before);second->references=saved;
    request.sets.interests[0]=u32{1}<<rd;
    std::array<std::byte,sched::pipe_capacity> full{};
    CHECK(kernel.scheduler().write_pipe(kernel.scheduler().open_description(writer)->pipe,full,true));
    request.sets.interests={0,u32{1}<<wr,0};request.readiness.immediate=true;
    CHECK(native::begin_select(kernel,pid,request,done)==0 && done);
    CHECK(kernel.scheduler().read_pipe(kernel.scheduler().open_description(reader)->pipe,full,true));
    request.sets.interests={u32{1}<<rd,0,0};request.readiness.immediate=false;

    for(auto &wait:native::poll_waits)wait.pid=999;
    CHECK(native::begin_select(kernel,pid,request,done)==-12);
    for(auto &wait:native::poll_waits)wait={};
    CHECK(native::begin_select(kernel,999,request,done)==-22);
    request.readiness.timed=true;request.readiness.timeout={0,17};mock::clock_fail=true;
    CHECK(native::begin_select(kernel,pid,request,done)==-5 && kernel.scheduler().open_description(reader)->references==before);mock::clock_fail=false;
    CHECK(native::prepare_native_thread(thread));
    process->resources().personality=sched::UserPersonality::linux;thread.native_active=true;
    native::started=true;native::runtime_kernel=&kernel;mock::entries=0;mock::now=5000000000;
    auto begin=[&](bool zero=false) {
        const u64 mask=u64{1}<<rd;std::memcpy(mock::memory.data(),&mask,8);
        const posix::ClockTime time{0,17};std::memcpy(mock::memory.data()+128,&time,sizeof(time));
        thread.native_frame={};thread.native_frame.registers[14]=270;thread.native_frame.registers[9]=zero?0:32;
        thread.native_frame.registers[10]=hw::user_begin;thread.native_frame.registers[7]=hw::user_begin+128;
        thread.syscall_pending=true;done=true;
        CHECK(user::linux_bootstrap::dispatch<lu::Profile::x86_64>(kernel,pid,thread.native_frame,done,
            [&](hw::Syscall c,bool &complete){return native::dispatch(kernel,pid,c,complete);})==0 && !done);
    };
    begin();const auto copies=mock::copies;std::memset(mock::memory.data(),0xff,256);mock::now+=5;
    CHECK(native::step(kernel) && !mock::entries && thread.syscall_pending);
    const std::byte byte{42};CHECK(kernel.scheduler().write_pipe(kernel.scheduler().open_description(writer)->pipe,{&byte,1},true));
    CHECK(native::step(kernel) && mock::entries==1 && !thread.syscall_pending && mock::copies==copies);
    CHECK(lu::NativeBridge<lu::Profile::x86_64>::decode_return(mock::last_frame).value().value==1);
    u64 output{};std::memcpy(&output,mock::memory.data(),8);CHECK(output==(u64{1}<<rd));
    posix::ClockTime remaining{};std::memcpy(&remaining,mock::memory.data()+128,sizeof remaining);CHECK(remaining.nanoseconds==12);
    CHECK(kernel.posix().activate(pid));std::array<std::byte,1> data{};CHECK(kernel.scheduler().read_pipe(kernel.scheduler().open_description(reader)->pipe,data,true));
    begin(true);mock::now+=17;CHECK(native::step(kernel) && mock::entries==2 && !thread.syscall_pending);
    CHECK(lu::NativeBridge<lu::Profile::x86_64>::decode_return(mock::last_frame).value().value==0);
    CHECK(kernel.posix().activate(pid));begin();mock::forbidden=hw::user_begin;mock::now+=17;
    CHECK(native::step(kernel) && mock::entries==3 && !thread.syscall_pending);
    CHECK(lu::NativeBridge<lu::Profile::x86_64>::decode_return(mock::last_frame).value().error==14);mock::forbidden=0;
    CHECK(kernel.posix().activate(pid));begin();native::release_process(kernel,pid);
    CHECK(!native::poll_waits[0].pid && kernel.scheduler().open_description(reader)->references==before && !thread.native_active);
    CHECK(native::entry_stacks.accounting().leased_slots==0 && native::runtime_status());
    native::started=false;native::runtime_kernel=nullptr;CHECK(kernel.posix().close(rd) && kernel.posix().close(wr));
    std::puts("PASS select wait lifetime: rollback, capacity, deadline, saved-frame completion, mapping revocation and release");
}
}
int main()
{
    static Kernel kernel;CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    const auto context=kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096});
    const auto made=kernel.scheduler().create_user_process("select-adapter",context);CHECK(made && kernel.posix().activate(made.value()));const auto pid=made.value();
    selects<lu::Profile::i386>(kernel,pid,7,{4,6,5,1,0,2});
    selects<lu::Profile::x86_64>(kernel,pid,14,{9,10,11,5,7,6});
    selects<lu::Profile::aarch64>(kernel,pid,8,{0,1,2,3,4,5});
    selects<lu::Profile::arm32>(kernel,pid,7,{0,1,2,3,4,5});
    selects<lu::Profile::rv64>(kernel,pid,17,{10,11,12,13,14,15});
    selects<lu::Profile::rv32>(kernel,pid,17,{10,11,12,13,14,15});
    selects<lu::Profile::loongarch64>(kernel,pid,11,{4,5,6,7,8,9});
    selects<lu::Profile::mips>(kernel,pid,2,{4,5,6,7,-1,-1});
    selects<lu::Profile::mips64>(kernel,pid,2,{4,5,6,7,8,9});
    selects<lu::Profile::ppc>(kernel,pid,0,{3,4,5,6,7,8});
    select_lifetime(kernel,pid);
    // Cancelling the last wait leaves this fixture runnable, awaiting selection.
    CHECK(kernel.scheduler().find(pid)->threads()[0].state == sched::ProcessState::runnable);
    CHECK(kernel.posix().activate(pid));
    runtime_poll(kernel,pid); // Existing completion semantics remain executable here.
}
