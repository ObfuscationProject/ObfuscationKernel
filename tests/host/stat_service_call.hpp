#pragma once
#include "ok/abi/linux_stat_call.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
namespace
{
using namespace ok;
using namespace ok::abi::linux_user;
#define VERIFY(x) do{if(!(x)){std::fprintf(stderr,"FAIL service adapter %d: %s\n",__LINE__,#x);std::abort();}}while(false)
unsigned service_calls{};
template<Profile P> void call_service(posix::PosixService &service, sched::Scheduler &scheduler,
                                    sched::ProcessId pid, const posix::FileStatus &expected,
                                    const char *path, bool qualified)
{
    auto *process=scheduler.find(pid);VERIFY(process);
    auto invoke=[&](StatCall kind,std::span<const u64> args,std::string_view name,bool writable,i64 wanted,bool compare=false){
        ++service_calls;
        std::array<u8,300> out{};out.fill(0xa5);const auto before=out;
        std::string order;
        const auto result=StatCallAdapter<P>::execute(kind,args,
            [&](u64 address,FixedString<256> &value)->Status{
                order+='R';VERIFY(address==0x1000);
                for(char ch:name)if(!value.append(ch))return Status::name_too_long();
                return {};
            },
            [&](StatRequest request,std::string_view value)->Result<posix::FileStatus>{
                order+='Q';
                // This explicit live-fd check does not remap provider EINVAL.
                if(request.direct_fd || (value.empty() && request.fd!=posix::at_FDCWD))
                {
                    const auto *slot=process->fd_table().entry(request.fd);
                    const auto *description=slot&&slot->used?scheduler.open_description(slot->description):nullptr;
                    if(!description||!description->used)return Status::bad_descriptor();
                    return service.fstat(request.fd);
                }
                return service.statat(request.fd,value,request.flags);
            },
            [&](u64 address,usize length)->bool{order+='A';VERIFY(address==0x2000&&length<=256);return writable;},
            [&](u64 address,std::span<const u8> bytes)->Status{
                order+='C';VERIFY(address==0x2000);
                for(usize i=0;i<bytes.size();++i)out[i+8]=bytes[i];
                return {};
            });
        if(result!=wanted)std::fprintf(stderr,"call=%u profile=%u kind=%u name=%.*s flags=%llu actual=%lld expected=%lld order=%s\n",
            service_calls,static_cast<unsigned>(P),static_cast<unsigned>(kind),static_cast<int>(name.size()),name.data(),
            static_cast<unsigned long long>(args.size()>2?args[2]:0),static_cast<long long>(result),static_cast<long long>(wanted),order.c_str());
        VERIFY(result==wanted);
        if(result) VERIFY(out==before&&order.find('C')==std::string::npos);
        if(compare&&result==0)
        {
            const auto layout=kind==StatCall::statx?StatLayout::statx:StatLayout::kernel_stat;
            const auto length=StatCodec<P>::size(layout);std::array<u8,256> encoded{};
            VERIFY(StatCodec<P>::encode(expected,layout,{encoded.data(),length}));
            for(usize i=0;i<out.size();++i)VERIFY(out[i]==(i>=8&&i<8+length?encoded[i-8]:0xa5));
        }
    };
    const i64 success=qualified?0:-95;
    std::array<u64,5> args{0xffffff9c,0x1000,0x100,0x7ff,0x2000};
    invoke(StatCall::statx,args,path,true,success,true);
    args[0]=0xffffffff;invoke(StatCall::statx,args,path,true,success,true); // Absolute ignores dirfd.
    args[0]=0xffffff9c;invoke(StatCall::statx,args,path,false,qualified?-14:-95);
    invoke(StatCall::statx,args,"/missing",false,-2);
    args[2]=0x8000;invoke(StatCall::statx,args,path,false,-22);
    args[2]=0x100;invoke(StatCall::statx,args,"",false,-2);
    args[1]=0;invoke(StatCall::statx,args,"",false,-14);
    args[0]=0xffffffff;args[2]=0x1000;invoke(StatCall::statx,args,"",false,-9);
    args[0]=0xffffff9c;args[1]=0x1000;args[2]=0;
    if(expected.type==fs::NodeType::symlink)
        // The block-link fixture contains one 80-byte component, which exceeds
        // the common resolver's existing bounded component capacity.
        invoke(StatCall::statx,args,path,false,expected.size==80?-36:-2);
    if(expected.type!=fs::NodeType::symlink)
    {
        auto fd=service.open(path,posix::o_RDONLY);VERIFY(fd);
        args[0]=static_cast<u32>(fd.value());args[1]=0;args[2]=0x1900;
        invoke(StatCall::statx,args,"",true,success,true);
        if constexpr(StatCodec<P>::has_kernel_stat)
        {
            const std::array<u64,2> direct{static_cast<u32>(fd.value()),0x2000};
            invoke(StatCall::fstat,direct,"",true,success,true);
        }
        VERIFY(service.close(fd.value()));invoke(StatCall::statx,args,"",false,-9);
    }
    if(expected.type==fs::NodeType::directory)
    {
        VERIFY(service.chdir(path));args={0xffffff9c,0,0x1000,0x7ff,0x2000};
        invoke(StatCall::statx,args,"",true,success,true);VERIFY(service.chdir("/"));
    }
    if constexpr(StatCodec<P>::has_kernel_stat)
    {
        const std::array<u64,4> legacy{0xffffff9c,0x1000,0x2000,0x1900};
        invoke(StatCall::newfstatat,legacy,path,true,success,true);
    }
    if constexpr(P==Profile::x86_64)
    {
        const std::array<u64,2> legacy{0x1000,0x2000};
        invoke(StatCall::lstat,legacy,path,true,success,true);
        if(expected.type!=fs::NodeType::symlink)invoke(StatCall::stat,legacy,path,true,success,true);
    }
}
void call_profiles(posix::PosixService &service,sched::Scheduler &scheduler,sched::ProcessId pid,
                   const posix::FileStatus &metadata,const char *path,bool qualified)
{
    call_service<Profile::i386>(service,scheduler,pid,metadata,path,qualified);
    call_service<Profile::x86_64>(service,scheduler,pid,metadata,path,qualified);
    call_service<Profile::aarch64>(service,scheduler,pid,metadata,path,qualified);
    call_service<Profile::arm32>(service,scheduler,pid,metadata,path,qualified);
    call_service<Profile::rv64>(service,scheduler,pid,metadata,path,qualified);
    call_service<Profile::rv32>(service,scheduler,pid,metadata,path,qualified);
    call_service<Profile::loongarch64>(service,scheduler,pid,metadata,path,qualified);
    call_service<Profile::mips>(service,scheduler,pid,metadata,path,qualified);
    call_service<Profile::mips64>(service,scheduler,pid,metadata,path,qualified);
    call_service<Profile::ppc>(service,scheduler,pid,metadata,path,qualified);
    std::printf("ADAPTER calls=%u\n",service_calls);
}
#undef VERIFY
}
