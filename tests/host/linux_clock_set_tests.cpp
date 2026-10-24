// Actual Linux/native dispatch and checked copies, with hosted mapping/provider.
#include "native_clock_model.hpp"

namespace {
unsigned forms{};
template <lu::Profile P>
void profile(Kernel &kernel,sched::ProcessId pid,usize number_slot,std::array<usize,2> slots)
{
    using Codec=lu::Codec<P>;using O=lu::Operation;
    auto *process=kernel.scheduler().find(pid);
    for(O operation:{O::clock_settime,O::clock_settime32,O::clock_settime64})
    {
        auto number=Codec::syscall_number(operation);if(!number)continue;++forms;
        const usize width=operation==O::clock_settime64?8:Codec::profile.word_bytes;
        constexpr uptr pointer=hw::user_begin+128;
        auto encode=[&](i64 seconds,u64 fraction,usize offset=128) {
            mock::memory.fill(std::byte{0x55});
            const u64 values[]{static_cast<u64>(seconds),fraction};
            for(usize f=0;f<2;++f)for(usize i=0;i<width;++i)
                mock::memory[offset+f*width+i]=static_cast<std::byte>(values[f]>>(8*(Codec::profile.big_endian?width-i-1:i)));
        };
        auto call=[&](u64 clock=0,uptr address=hw::user_begin+128) {
            hw::UserFrame frame{};frame.registers[number_slot]=*number;
            frame.registers[slots[0]]=clock;frame.registers[slots[1]]=address;
            bool done=true;
            const auto result=user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
                [&](hw::Syscall request,bool &complete){return native::dispatch(kernel,pid,request,complete);});
            CHECK(done);
            CHECK(user::linux_bootstrap::complete<P>(frame,result));
            auto decoded=lu::NativeBridge<P>::decode_return(frame);CHECK(decoded);
            const auto expected=result==-75?*Codec::errno_number(lu::Error::e_overflow):
                                result==-38?*Codec::errno_number(lu::Error::e_nosys):static_cast<u32>(result<0?-result:0);
            CHECK(decoded.value().error==expected && decoded.value().value==0);
            return result;
        };
        process->set_credentials(user::root_credentials());
        encode(100,99);auto before=mock::memory;
        const auto copies=mock::copies,reads=mock::clock_reads,commits=mock::setting_calls;
        CHECK(call(1,0)==-22 && call(~u64{0},0)==-22);
        CHECK(mock::copies==copies && mock::clock_reads==reads && mock::setting_calls==commits);
        CHECK(call(0,0)==-14 && mock::clock_reads==reads && mock::setting_calls==commits);
        mock::forbidden=pointer+2*width-1;CHECK(call()==-14);mock::forbidden=0;
        CHECK(call()==0 && mock::memory==before && mock::setting_calls==commits+1);
        CHECK(mock::last_setting==time::EpochTime(100,99) && mock::setting_point.nanoseconds==mock::now);
        CHECK(mock::last_input_size==2*width);
        CHECK(call(u64{1}<<32)==0); // Linux clockid_t narrows to low32.
        process->set_credentials(user::default_user_credentials());
        const auto denied_reads=mock::clock_reads,denied_commits=mock::setting_calls;
        CHECK(call()==-1 && mock::clock_reads==denied_reads && mock::setting_calls==denied_commits);
        CHECK(call(0,0)==-14);
        encode(-1,0);CHECK(call()==-22);
        encode(100,1000000000);CHECK(call()==-22);
        encode(100,~u64{0});CHECK(call()==-22);
        encode(0,0);CHECK(call()==-1); // Permission precedes floor.
        CHECK(mock::clock_reads==denied_reads && mock::setting_calls==denied_commits);
        process->set_credentials(user::root_credentials());
        CHECK(call()==-22 && mock::setting_calls==denied_commits);
        encode(3,42);CHECK(call()==0); // Exact monotonic floor.
        encode(3,41);CHECK(call()==-22);
        encode(100,99);mock::clock_fail=true;CHECK(call()==-5);mock::clock_fail=false;
        mock::setting_status=Status::overflow();CHECK(call()==-75);
        mock::setting_status=Status::unsupported();CHECK(call()==-38);mock::setting_status=Status::success();
        if(width==8){
            encode(user::maximum_set_realtime_seconds,0);CHECK(call()==-22);
            encode(user::maximum_set_realtime_seconds-1,999999999);CHECK(call()==0);
            encode(100,(u64{0x87654321}<<32)|17);
            CHECK(call()==(Codec::profile.word_bytes==4?0:-22));
        }
        encode(101,7,129);CHECK(call(0,pointer+1)==0);
        const usize offset=mock::memory.size()-2*width;
        encode(102,8,offset);CHECK(call(0,hw::user_begin+offset)==0);
        CHECK(call(0,hw::user_begin+offset+1)==-14);
    }
}

void native_boundary(Kernel &kernel,sched::ProcessId pid)
{
    using N=syscall::Number;auto *process=kernel.scheduler().find(pid);
    auto encode=[&](i64 seconds,i64 fraction) { const ok_timespec value{seconds,fraction};std::memcpy(mock::memory.data()+128,&value,sizeof(value)); };
    auto call=[&](uptr identity=0,uptr address=hw::user_begin+128) {
        bool done=true;return native::dispatch(kernel,pid,{static_cast<uptr>(N::clock_settime),{identity,address}},done);
    };
    process->set_credentials(user::root_credentials());encode(100,9);
    const auto copies=mock::copies,commits=mock::setting_calls;
    CHECK(call(1,0)==-22 && call(uptr{1}<<32,0)==-22 && mock::copies==copies);
    CHECK(call(0,0)==-14 && mock::setting_calls==commits);
    CHECK(call()==0 && mock::last_input_size==16);
    encode(100,i64{1}<<32);CHECK(call()==-22); // Native has no Linux32 padding.
    process->set_credentials(user::default_user_credentials());encode(100,0);
    CHECK(call()==-1);encode(-1,0);CHECK(call()==-22);
    process->set_credentials(user::root_credentials());encode(3,42);CHECK(call()==0);
    posix::ClockTime value{100,12};
    auto response=kernel.syscalls().dispatch({N::clock_settime,pid,{0,reinterpret_cast<uptr>(&value)}});
    CHECK(response.status && mock::last_setting==time::EpochTime(100,12));
    process->set_credentials(user::default_user_credentials());CHECK(kernel.posix().activate(pid));
    response=kernel.syscalls().dispatch({N::clock_settime,pid,{0,reinterpret_cast<uptr>(&value)}});
    CHECK(response.status.code()==StatusCode::permission);
}
}
int main()
{
    static Kernel kernel;CHECK(kernel.boot({}));
    const auto context=kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096});
    const auto made=kernel.scheduler().create_user_process("clock-set-model",context);
    CHECK(made && kernel.posix().activate(made.value()));const auto pid=made.value();
    mock::setting_enabled=true;
    profile<lu::Profile::i386>(kernel,pid,7,{4,6});
    profile<lu::Profile::x86_64>(kernel,pid,14,{9,10});
    profile<lu::Profile::aarch64>(kernel,pid,8,{0,1});
    profile<lu::Profile::arm32>(kernel,pid,7,{0,1});
    profile<lu::Profile::rv64>(kernel,pid,17,{10,11});
    profile<lu::Profile::rv32>(kernel,pid,17,{10,11});
    profile<lu::Profile::loongarch64>(kernel,pid,11,{4,5});
    profile<lu::Profile::mips>(kernel,pid,2,{4,5});
    profile<lu::Profile::mips64>(kernel,pid,2,{4,5});
    profile<lu::Profile::ppc>(kernel,pid,0,{3,4});
    CHECK(forms==14);native_boundary(kernel,pid);
    std::puts("PASS all ten /14 Linux setter forms and native/trusted boundaries; hosted models only");
}
