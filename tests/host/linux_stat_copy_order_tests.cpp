// Hosted maintained provider with late stat output failure; no hardware copy.
#include "ext4_realtime_model.hpp"
#include "ok/abi/linux_stat_call.hpp"
#include "ok/fs/ext4_fs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <string>
using namespace ok;
using namespace ok::abi::linux_user;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
class Disk final:public driver::BlockDevice
{
 public:
 std::vector<std::byte> bytes;usize writes{},flushes{};
 explicit Disk(const char *path){FILE*f=std::fopen(path,"rb");CHECK(f);CHECK(!std::fseek(f,0,SEEK_END));bytes.resize(static_cast<usize>(std::ftell(f)));CHECK(!std::fseek(f,0,SEEK_SET));CHECK(std::fread(bytes.data(),1,bytes.size(),f)==bytes.size());CHECK(!std::fclose(f));}
 driver::BlockGeometry geometry()const override{return {bytes.size()/512,512,true};}
 Status read_blocks(u64 at,std::span<std::byte> out)override{if(at>bytes.size()/512||out.size()>bytes.size()-at*512)return Status::fault();std::memcpy(out.data(),bytes.data()+at*512,out.size());return {};}
 Status write_blocks(u64 at,std::span<const std::byte> in)override{if(at>bytes.size()/512||in.size()>bytes.size()-at*512)return Status::fault();++writes;std::memcpy(bytes.data()+at*512,in.data(),in.size());return {};}
 Status flush()override{++flushes;return {};}
};
int main(int argc,char **argv)
{
 CHECK(argc==6);Disk disk(argv[1]);const usize offset=std::strtoull(argv[3],nullptr,10),stride=std::strtoull(argv[4],nullptr,10);
 CHECK(offset+stride<=disk.bytes.size());
 const std::string mode=argv[5];CHECK(mode=="late-output"||mode=="bad-flags"||mode=="unavailable-clock"||mode=="nofollow");
 auto put=[&](usize at,u32 value){for(usize i=0;i<4;++i)disk.bytes[offset+at+i]=std::byte(static_cast<u8>(value>>(8*i)));};
 put(4,4);for(usize i=40;i<100;++i)disk.bytes[offset+i]=std::byte{};
 std::memcpy(disk.bytes.data()+offset+40,"four",4);
 testing::ext4_clock::set({{1700000000,0},{0,1},1,time::RealtimeOrigin::administrator_software});
 auto fs=std::make_unique<fs::Ext4FileSystem>();CHECK(fs->mount(disk,fs::Ext4MountOptions{true,fs::Ext4TimestampPolicy::maintained}));
 auto vfs=std::make_unique<fs::VirtualFileSystem>();CHECK(vfs->mount("/",*fs->root()));
 auto scheduler=std::make_unique<sched::Scheduler>();driver::ConsoleDriver console;CHECK(console.start());posix::PosixService service;CHECK(service.initialize(*vfs,console,*scheduler));
 auto pid=scheduler->create_user_process("copy-order",arch::configured_arch_operations().make_user_context({0x400000,0x800000,0}));CHECK(pid&&service.activate(pid.value()));CHECK(service.set_identity(1000,1000));
 testing::ext4_clock::reset();
 if(mode!="unavailable-clock")testing::ext4_clock::set({{1800000100,123456789},{0,1},1,time::RealtimeOrigin::administrator_software});
 const auto before=disk.bytes;const auto writes=disk.writes,flushes=disk.flushes;
 std::array<u8,256> output{};output.fill(0xa5);const auto sentinel=output;std::string order;
 const std::array<u64,5> args{0xffffff9c,0x1000,mode=="bad-flags"?0x8000u:mode=="nofollow"?0x100u:0u,0x7ff,0x2000};
 auto result=StatCallAdapter<Profile::x86_64>::execute(StatCall::statx,args,
    [&](u64,FixedString<256> &path)->Status{order+='R';return path.assign("/fast");},
    [&](StatRequest request,std::string_view path)->Result<posix::FileStatus>{order+='Q';return service.statat(request.fd,path,request.flags);},
    [&](u64,usize)->bool{order+='A';return false;},
    [&](u64,std::span<const u8>)->Status{order+='C';return {};});
 CHECK(output==sentinel&&order.find('C')==std::string::npos);
 if(mode=="late-output")
 {
    CHECK(result==-14&&order=="RQA"&&testing::ext4_clock::calls()==1&&disk.writes>writes&&disk.flushes>flushes);
    CHECK(before!=disk.bytes);
    for(usize i=0;i<before.size();++i)
    {
        // The first ordinary writer event also persists the existing ext4
        // dirty marker; output admission does not roll that event back.
        const bool permitted=before[i]==disk.bytes[i]||(i>=offset+8&&i<offset+12)||(stride==256&&i>=offset+140&&i<offset+144)||
            (i==1024+58&&before[i]==std::byte{1}&&disk.bytes[i]==std::byte{0});
        if(!permitted)std::fprintf(stderr,"unexpected changed byte=%zu before=%u after=%u inode=%zu\n",i,std::to_integer<unsigned>(before[i]),std::to_integer<unsigned>(disk.bytes[i]),offset);
        CHECK(permitted);
    }
 }
 else
 {
    CHECK(result==(mode=="bad-flags"?-22:mode=="nofollow"?-14:-5));
    CHECK(order==(mode=="bad-flags"?"":mode=="nofollow"?"RQA":"RQ"));
    CHECK(testing::ext4_clock::calls()==(mode=="unavailable-clock"?1u:0u));
    CHECK(before==disk.bytes&&disk.writes==writes&&disk.flushes==flushes);
 }
 FILE*f=std::fopen(argv[2],"wb");CHECK(f);CHECK(std::fwrite(disk.bytes.data()+offset,1,stride,f)==stride);CHECK(!std::fclose(f));
 std::printf("PASS hosted late stat copy policy mode=%s samples=%zu writes=%zu flushes=%zu order=%s guest=0\n",mode.c_str(),testing::ext4_clock::calls(),disk.writes-writes,disk.flushes-flushes,order.c_str());
}
