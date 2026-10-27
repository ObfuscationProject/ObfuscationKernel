#include "unavailable_realtime.hpp"
// Actual Linux adapter/common native copy boundary with hosted mappings only.
#include "native_runtime_units.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
using namespace ok;
namespace native = ok::user::native;
namespace hw = ok::arch::execution;
namespace lu = ok::abi::linux_user;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                                            \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)
namespace mock
{
std::array<std::byte, 8192> memory{};
uptr forbidden{};
usize copies{};
usize outputs{};
usize partial_output{};
uptr last_input{};
usize last_input_size{};
u64 now = 3000000042;
bool clock_fail{};
usize entries{};
uptr readonly_begin{}, readonly_end{};
u64 clock_step{};
hw::UserFrame last_frame{};
} // namespace mock
namespace ok::arch
{
Status initialize_clock()
{
    return Status::success();
}
Result<u64> monotonic_nanoseconds()
{
    const auto value = mock::now; mock::now += mock::clock_step;
    return mock::clock_fail ? Result<u64>(Status::io_error()) : Result<u64>(value);
}
u64 clock_resolution_ns()
{
    return 1;
}
} // namespace ok::arch
namespace ok::arch::execution
{
bool supported() noexcept
{
    return false;
}
Status initialize()
{
    return Status::unsupported();
}
Result<AddressSpace> create_space(MappingView)
{
    return Status::unsupported();
}
Status destroy_space(AddressSpace &space)
{
    space = {};
    return Status::success();
}
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &view)
{
    if (!view.valid()) return Status::invalid_argument("hosted entry requires a live lease");
    auto result = enter(space, frame);
    if (result) result.value().kernel_stack_pointer = view.lower(); // Synthetic hosted capture only.
    return result;
}
Result<Trap> enter(AddressSpace &, UserFrame &frame)
{
    ++mock::entries; mock::last_frame=frame;
    return Trap{TrapKind::timer};
}
UserFrame initial_frame(uptr pc, uptr sp)
{
    UserFrame f{};
    f.pc = pc;
    f.sp = sp;
    return f;
}
Syscall syscall(const UserFrame &)
{
    return {};
}
void set_return_value(UserFrame &f, i64 v)
{
    f.registers[0] = static_cast<uptr>(v);
}
bool user_range(const AddressSpace &, uptr address, usize size, bool write)
{
    if (size && write && address < mock::readonly_end && address + size > mock::readonly_begin) return false;
    return !size || (address >= user_begin && address - user_begin < mock::memory.size() &&
                     size <= mock::memory.size() - (address - user_begin) &&
                     (!mock::forbidden || address >= mock::forbidden + 1 || size <= mock::forbidden - address));
}
Status copy_from_user(const AddressSpace &s, void *out, uptr address, usize size)
{
    ++mock::copies;
    mock::last_input=address; mock::last_input_size=size;
    if (!user_range(s, address, size, false))
        return Status::fault();
    if (size)
        std::memcpy(out, mock::memory.data() + address - user_begin, size);
    return Status::success();
}
Status copy_to_user(const AddressSpace &s, uptr address, const void *in, usize size)
{
    ++mock::outputs;
    if (!user_range(s, address, size, true))
        return Status::fault();
    if (mock::partial_output)
    {
        std::memcpy(mock::memory.data() + address - user_begin, in, mock::partial_output);
        return Status::fault();
    }
    if (size)
        std::memcpy(mock::memory.data() + address - user_begin, in, size);
    return Status::success();
}
} // namespace ok::arch::execution

namespace {
template <lu::Profile P>
void polls(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<int,5> slots)
{
    using O=lu::Operation;using C=lu::Codec<P>;
    constexpr bool big=C::profile.big_endian;
    constexpr usize signal_bytes=(P==lu::Profile::mips||P==lu::Profile::mips64)?16:8;
    constexpr u16 write_normal=signal_bytes==16?4:0x100;
    constexpr uptr fds=hw::user_begin+128, time=hw::user_begin+1024, mask=hw::user_begin+2048;
    auto *process=kernel.scheduler().find(pid);auto &thread=process->threads()[0];
    auto put=[&](usize offset,usize width,u64 value) {
        for(usize i=0;i<width;++i)mock::memory[offset+i]=static_cast<std::byte>(value>>(8*(big?width-i-1:i)));
    };
    auto get=[&](usize offset,usize width) {
        u64 value{};for(usize i=0;i<width;++i)value|=u64(std::to_integer<u8>(mock::memory[offset+i]))<<(8*(big?width-i-1:i));
        return value;
    };
    auto fd=[&](usize index,i32 descriptor,u16 events) {put(128+8*index,4,static_cast<u32>(descriptor));put(132+8*index,2,events);put(134+8*index,2,0x5555);};
    auto stamp=[&](i64 sec,u64 ns,usize width) {put(1024,width,static_cast<u64>(sec));put(1024+width,width,ns);};
    bool done=true,bad_stack=false;
    auto call=[&](O op,std::array<u64,5> args) {
        const auto nr=C::syscall_number(op);CHECK(nr);hw::UserFrame frame{};frame.registers[number_slot]=*nr;
        frame.sp=bad_stack?0:hw::user_begin+3072;
        for(usize i=0;i<5;++i) {
            if(slots[i]>=0)frame.registers[slots[i]]=static_cast<uptr>(args[i]);
            else put(3072+16,4,args[i]);
        }
        done=true;
        return user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
            [&](hw::Syscall c,bool &complete){return native::dispatch(kernel,pid,c,complete);});
    };
    auto retire=[&] {native::cancel_poll(kernel,pid);thread.wait_reason=sched::ThreadWaitReason::none;thread.wait_key=0;CHECK(kernel.scheduler().set_runnable(pid));CHECK(kernel.posix().activate(pid));};
    for(O op:{O::poll,O::ppoll,O::ppoll_time64}) {
        if(!C::syscall_number(op))continue;
        const bool precise=op!=O::poll;const usize width=op==O::ppoll_time64?8:C::profile.word_bytes;
        mock::now=3000000042;mock::clock_step=0;mock::memory.fill(std::byte{0x55});
        auto invoke=[&](u64 n=1,u64 timeout=0,uptr ptr=hw::user_begin+128,uptr sig=0,u64 size=((P==lu::Profile::mips||P==lu::Profile::mips64)?16:8)) {
            return call(op,{ptr,n,precise?time:timeout,sig,size});
        };
        stamp(0,0,width);fd(0,-1,0xffff);CHECK(invoke()==0 && done && get(134,2)==0);
        CHECK(get(128,4)==0xffffffffU && get(132,2)==0xffff);
        fd(0,999,0);CHECK(invoke()==1 && done && get(134,2)==0x20);
        fd(0,1,static_cast<u16>(4|write_normal));CHECK(invoke()==1 && get(134,2)==(4|write_normal));
        fd(0,1,2);CHECK(invoke()==0 && get(134,2)==0); // No priority data on console output.
        fd(0,1,write_normal);CHECK(invoke()==1 && get(134,2)==write_normal);
        CHECK(invoke(33,0,0)==-22);CHECK(invoke(1,0,0)==-14);CHECK(invoke(0,0,0)==0);
        if constexpr(C::profile.word_bytes==8) {
            fd(0,999,0);CHECK(invoke((u64{1}<<32)|1)==1 && get(134,2)==0x20);
            if(!precise)CHECK(invoke(1,u64{1}<<32)==1);
        }
        mock::readonly_begin=fds;mock::readonly_end=fds+6;
        fd(0,999,0);CHECK(invoke()==1 && get(134,2)==0x20);
        mock::readonly_end=fds+8;
        fd(0,999,0);CHECK(invoke()==-14);mock::readonly_begin=mock::readonly_end=0;
        if(precise) {
            stamp(-1,0,width);CHECK(invoke(33,0,0,0,0)==-22);
            stamp(0,1000000000,width);CHECK(invoke(33,0,0,0,0)==-22);
            CHECK(call(op,{0,33,0,mask,signal_bytes+1})==-22);
            stamp(0,0,width);CHECK(invoke(33,0,0,mask,signal_bytes+1)==-22);
            CHECK(invoke(33,0,0,1,signal_bytes)==-14);
            CHECK(invoke(33,0,0,mask,signal_bytes)==-38);
            CHECK(invoke(0,0,0,0,~u64{0})==0); // Null mask ignores sigsetsize.
            mock::forbidden=time+1;CHECK(invoke(33,0,0,mask,signal_bytes+1)==-14);mock::forbidden=0;
            fd(0,999,0);stamp(0,17,width);mock::clock_step=3;
            CHECK(invoke()==1 && get(1024,width)==0 && get(1024+width,width)==14);
            stamp(0,17,width);CHECK(invoke(33,0,0)==-22 && get(1024+width,width)==14);
            stamp(0,17,width);CHECK(invoke(1,0,0)==-14 && get(1024+width,width)==14);
            mock::clock_step=0;
            stamp(0,17,width);mock::readonly_begin=time;mock::readonly_end=time+2*width;
            CHECK(invoke()==1 && get(1024+width,width)==17);mock::readonly_begin=mock::readonly_end=0;
            // Readonly descriptors do not reject a blocked request until copyout.
            auto pipes=kernel.posix().pipe();CHECK(pipes);fd(0,pipes.value()[0],1);stamp(0,17,width);
            mock::readonly_begin=fds;mock::readonly_end=fds+8;
            CHECK(invoke()==0 && !done && native::poll_objects.resolve(native::poll_waits[0].object).value()->timeout.deadline()==mock::now+17);
            mock::readonly_begin=mock::readonly_end=0;retire();
            CHECK(kernel.posix().close(pipes.value()[0]) && kernel.posix().close(pipes.value()[1]));
            if(op==O::ppoll_time64 && C::profile.word_bytes==4) {
                stamp(0,0x1234567800000011ULL,8);fd(0,-1,0);
                CHECK(invoke()==0 && !done && native::poll_objects.resolve(native::poll_waits[0].object).value()->timeout.deadline()==mock::now+17);retire();
            }
            if(width==8) {
                stamp(__INT64_MAX__,999999999,8);fd(0,-1,0);
                CHECK(invoke()==0 && !done && native::poll_objects.resolve(native::poll_waits[0].object).value()->timeout.deadline()==lu::ktime_maximum);
                CHECK(native::poll_objects.resolve(native::poll_waits[0].object).value()->timeout_output.end.seconds==__INT64_MAX__ && native::poll_objects.resolve(native::poll_waits[0].object).value()->timeout_output.end.nanoseconds==0);
                retire();
                fd(0,999,0);stamp(0x80000000LL,17,8);CHECK(invoke()==1 && get(1024,8)==0x80000000ULL);
            }
            if constexpr(P==lu::Profile::mips) {
                stamp(0,0,width);bad_stack=true;CHECK(invoke()==-14);bad_stack=false;
                mock::forbidden=hw::user_begin+3092;CHECK(invoke()==1);mock::forbidden=0; // Exactly fifth word.
            }
        }
        // Frozen duplicate entries retain the same open description after fd reuse.
        auto pair=kernel.posix().pipe();CHECK(pair);
        const auto rd=pair.value()[0],wr=pair.value()[1];const auto desc=process->fd_table().entry(rd)->description;
        const auto writer=process->fd_table().entry(wr)->description;
        fd(0,rd,0x41);fd(1,rd,0x40);stamp(1,0,width);
        CHECK(call(op,{fds,2,precise?0:static_cast<u64>(~u32{0}),0,0})==0 && !done);
        CHECK(kernel.scheduler().open_description(desc)->references==3 && !native::poll_objects.resolve(native::poll_waits[0].object).value()->timeout.armed());
        fd(0,wr,4);CHECK(kernel.scheduler().close_descriptor(process->resources(),rd));
        CHECK(kernel.scheduler().install_descriptor(process->resources(),rd,writer));
        const std::byte byte{42};CHECK(kernel.scheduler().write_pipe(kernel.scheduler().open_description(writer)->pipe,{&byte,1},true));
        CHECK(native::poll_ready(kernel,*native::poll_objects.resolve(native::poll_waits[0].object).value()).value()==2);
        CHECK(native::poll_output(process->resources().native_space,*native::poll_objects.resolve(native::poll_waits[0].object).value()));
        CHECK(get(128,4)==static_cast<u32>(wr) && get(132,2)==4 && get(134,2)==0x41 && get(142,2)==0x40);
        retire();CHECK(!kernel.scheduler().open_description(desc));
        fd(0,wr,static_cast<u16>(4|write_normal));stamp(0,0,width);CHECK(invoke()==1 && get(134,2)==(8|4|write_normal));
        CHECK(kernel.posix().close(rd) && kernel.posix().close(wr));
        pair=kernel.posix().pipe();CHECK(pair);CHECK(kernel.posix().close(pair.value()[1]));
        fd(0,pair.value()[0],0x41);CHECK(invoke()==1 && get(134,2)==16);CHECK(kernel.posix().close(pair.value()[0]));
        CHECK(thread.wait_reason==sched::ThreadWaitReason::none);
    }
    std::printf("PASS Linux poll %s: layouts, masks, retained descriptions, errors, deadlines and raw timeout copyback\n",C::profile.name);
}
void runtime_poll(Kernel &kernel,sched::ProcessId pid)
{
    auto *process=kernel.scheduler().find(pid);auto &thread=process->threads()[0];
    CHECK(native::prepare_native_thread(thread));
    process->resources().personality=sched::UserPersonality::linux;thread.native_active=true;
    native::started=true;native::runtime_kernel=&kernel;mock::entries=0;mock::now=5000000000;
    auto pair=kernel.posix().pipe();CHECK(pair);const auto rd=pair.value()[0],wr=pair.value()[1];
    const auto reader=process->fd_table().entry(rd)->description,writer=process->fd_table().entry(wr)->description;
    auto begin=[&](int count,i64 ns) {
        const ok_pollfd item{rd,1,99};std::memcpy(mock::memory.data(),&item,sizeof(item));
        const posix::ClockTime time{0,ns};std::memcpy(mock::memory.data()+128,&time,sizeof(time));
        thread.native_frame={};thread.native_frame.registers[14]=271;thread.native_frame.registers[9]=hw::user_begin;
        thread.native_frame.registers[10]=count;thread.native_frame.registers[11]=hw::user_begin+128;
        thread.native_frame.registers[7]=8;thread.syscall_pending=true;
        bool done=true;
        CHECK(user::linux_bootstrap::dispatch<lu::Profile::x86_64>(kernel,pid,thread.native_frame,done,
            [&](hw::Syscall c,bool &complete){return native::dispatch(kernel,pid,c,complete);})==0 && !done);
    };
    begin(1,17);const auto copies=mock::copies;const auto saved=mock::memory;
    std::memset(mock::memory.data(),0xff,256);mock::now+=5;
    CHECK(native::step(kernel) && mock::entries==0 && thread.syscall_pending);
    const std::byte byte{42};CHECK(kernel.scheduler().write_pipe(kernel.scheduler().open_description(writer)->pipe,{&byte,1},true));
    CHECK(native::step(kernel) && mock::entries==1 && !thread.syscall_pending);
    CHECK(mock::copies==copies && lu::NativeBridge<lu::Profile::x86_64>::decode_return(mock::last_frame).value().value==1);
    CHECK(mock::memory[0]==std::byte{0xff} && mock::memory[4]==std::byte{0xff} && mock::memory[6]==std::byte{1} && mock::memory[7]==std::byte{0});
    posix::ClockTime remaining{};std::memcpy(&remaining,mock::memory.data()+128,sizeof(remaining));CHECK(remaining.seconds==0 && remaining.nanoseconds==12);
    CHECK(native::poll_waits[0].pid==0 && kernel.scheduler().open_description(reader)->references==1);
    CHECK(kernel.posix().activate(pid));std::array<std::byte,1> read{};CHECK(kernel.scheduler().read_pipe(kernel.scheduler().open_description(reader)->pipe,read,true));
    begin(0,17);mock::now+=17;CHECK(native::step(kernel) && mock::entries==2 && !thread.syscall_pending);
    CHECK(lu::NativeBridge<lu::Profile::x86_64>::decode_return(mock::last_frame).value().value==0);
    std::memcpy(&remaining,mock::memory.data()+128,sizeof(remaining));CHECK(!remaining.seconds && !remaining.nanoseconds);
    CHECK(kernel.posix().activate(pid));begin(1,17);mock::readonly_begin=hw::user_begin;mock::readonly_end=hw::user_begin+8;
    mock::now+=17;CHECK(native::step(kernel) && mock::entries==3 && !thread.syscall_pending);
    CHECK(lu::NativeBridge<lu::Profile::x86_64>::decode_return(mock::last_frame).value().error==14);
    std::memcpy(&remaining,mock::memory.data()+128,sizeof(remaining));CHECK(!remaining.seconds && !remaining.nanoseconds);
    mock::readonly_begin=mock::readonly_end=0;CHECK(kernel.posix().activate(pid));begin(1,17);
    CHECK(kernel.scheduler().open_description(reader)->references==2);native::release_process(kernel,pid);
    CHECK(native::poll_waits[0].pid==0 && !thread.native_active && kernel.scheduler().open_description(reader)->references==1);
    CHECK(native::entry_stacks.accounting().leased_slots==0 && native::runtime_status());
    native::started=false;native::runtime_kernel=nullptr;
    CHECK(kernel.posix().close(rd) && kernel.posix().close(wr));static_cast<void>(saved);
    std::puts("PASS hosted runtime poll: readiness/timeout/EFAULT completion once, frozen input, raw remainder and teardown");
}
}
#ifndef OK_SELECT_TESTS
int main()
{
    static Kernel kernel;CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    const auto context=kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096});
    const auto made=kernel.scheduler().create_user_process("poll-adapter",context);CHECK(made && kernel.posix().activate(made.value()));
    const auto pid=made.value();
    polls<lu::Profile::i386>(kernel,pid,7,{4,6,5,1,0});
    polls<lu::Profile::x86_64>(kernel,pid,14,{9,10,11,5,7});
    polls<lu::Profile::aarch64>(kernel,pid,8,{0,1,2,3,4});
    polls<lu::Profile::arm32>(kernel,pid,7,{0,1,2,3,4});
    polls<lu::Profile::rv64>(kernel,pid,17,{10,11,12,13,14});
    polls<lu::Profile::rv32>(kernel,pid,17,{10,11,12,13,14});
    polls<lu::Profile::loongarch64>(kernel,pid,11,{4,5,6,7,8});
    polls<lu::Profile::mips>(kernel,pid,2,{4,5,6,7,-1});
    polls<lu::Profile::mips64>(kernel,pid,2,{4,5,6,7,8});
    polls<lu::Profile::ppc>(kernel,pid,0,{3,4,5,6,7});
    runtime_poll(kernel,pid);
}
#endif
