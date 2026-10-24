// Hosted common service/provider/codec composition; no syscall dispatcher.
#include "ext4_realtime_model.hpp"
#include "ok/abi/linux_stat_call.hpp"
#include "stat_service_call.hpp"
#include "ok/fs/ext4_fs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
using namespace ok;
using namespace ok::abi::linux_user;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
class Disk final:public driver::BlockDevice {
 std::vector<std::byte> bytes;
 public: usize writes{},flushes{};
 explicit Disk(const char *name){FILE*f=std::fopen(name,"rb");CHECK(f);CHECK(!std::fseek(f,0,SEEK_END));bytes.resize(static_cast<usize>(std::ftell(f)));CHECK(!std::fseek(f,0,SEEK_SET));CHECK(std::fread(bytes.data(),1,bytes.size(),f)==bytes.size());std::fclose(f);}
 driver::BlockGeometry geometry()const override{return {bytes.size()/512,512,true};}
 Status read_blocks(u64 at,std::span<std::byte> out)override{if(at>bytes.size()/512||out.size()>bytes.size()-at*512)return Status::fault();std::memcpy(out.data(),bytes.data()+at*512,out.size());return {};}
 Status write_blocks(u64,std::span<const std::byte>)override{++writes;return Status::io_error("query attempted disk mutation");}
 Status flush()override{++flushes;return {};}
};
template<Profile P> void emit(const posix::FileStatus &s,const char *directory,const char *node,const char *arch,bool qualified){
 for(auto layout:{StatLayout::statx,StatLayout::kernel_stat}){
  const usize count=StatCodec<P>::size(layout);if(!count)continue;
  std::array<u8,256> bytes{};bytes.fill(0x5a);const auto before=bytes;
  const auto result=StatCodec<P>::encode(s,layout,{bytes.data(),count});
  if(!qualified){CHECK(!result&&result.status().code()==StatusCode::unsupported&&bytes==before);continue;}
  CHECK(result&&result.value()==count);
  char path[1024];std::snprintf(path,sizeof(path),"%s/%s-%s-%s.bin",directory,node,arch,layout==StatLayout::statx?"statx":"kernel");
  FILE*f=std::fopen(path,"wb");CHECK(f);CHECK(std::fwrite(bytes.data(),1,count,f)==count);CHECK(!std::fclose(f));
 }
}
void all(const posix::FileStatus &s,const char *out,const char *name,bool qualified){
 emit<Profile::i386>(s,out,name,"i386",qualified);emit<Profile::x86_64>(s,out,name,"x86_64",qualified);
 emit<Profile::aarch64>(s,out,name,"aarch64",qualified);emit<Profile::arm32>(s,out,name,"arm32",qualified);
 emit<Profile::rv64>(s,out,name,"rv64",qualified);emit<Profile::rv32>(s,out,name,"rv32",qualified);
 emit<Profile::loongarch64>(s,out,name,"loongarch64",qualified);emit<Profile::mips>(s,out,name,"mips",qualified);
 emit<Profile::mips64>(s,out,name,"mips64",qualified);emit<Profile::ppc>(s,out,name,"ppc",qualified);
}
int main(int argc,char **argv){
 CHECK(argc==4);const bool qualified=std::strcmp(argv[3],"readonly")==0;
 Disk disk(argv[1]);auto fs=std::make_unique<fs::Ext4FileSystem>();testing::ext4_clock::reset();CHECK(fs->mount(disk,!qualified));
 auto vfs=std::make_unique<fs::VirtualFileSystem>();CHECK(vfs->mount("/",*fs->root()));
 auto scheduler=std::make_unique<sched::Scheduler>();driver::ConsoleDriver console;CHECK(console.start());posix::PosixService service;CHECK(service.initialize(*vfs,console,*scheduler));
 auto pid=scheduler->create_user_process("stat-service",arch::configured_arch_operations().make_user_context({0x400000,0x800000,0}));CHECK(pid&&service.activate(pid.value()));CHECK(service.set_identity(1000,1000));
 const auto flushes=disk.flushes;fs::FilesystemDevice device{};bool first=true;
 for(const auto *name:{"empty","four","sparse","dir","fast","block"}){
  char path[64];std::snprintf(path,sizeof(path),"/%s",name);auto value=service.statat(posix::at_FDCWD,path,posix::at_SYMLINK_NOFOLLOW);CHECK(value);
  const auto &s=value.value();CHECK(s.attributes.has(fs::Attribute::inode)&&s.attributes.has(fs::Attribute::device)&&s.attributes.has(fs::Attribute::blocks512));
  if(first){device=s.attributes.device;first=false;}CHECK(device==s.attributes.device);
  all(s,argv[2],name,qualified);
  call_profiles(service,*scheduler,pid.value(),s,path,qualified);
  std::printf("NODE %s %llu %u %u %u %u %u %u %llu %llu\n",name,static_cast<unsigned long long>(s.attributes.inode),s.attributes.device.major,s.attributes.device.minor,s.mode,s.uid,s.gid,s.link_count,static_cast<unsigned long long>(s.size),static_cast<unsigned long long>(s.blocks));
  if(s.type!=fs::NodeType::symlink){auto fd=service.open(path,posix::o_RDONLY);CHECK(fd);auto held=service.fstat(fd.value());CHECK(held&&held.value().attributes.inode==s.attributes.inode&&held.value().attributes.device==device&&held.value().blocks==s.blocks);CHECK(service.close(fd.value()));}
 }
 CHECK(service.chdir("/dir"));auto cwd=service.statat(posix::at_FDCWD,"",posix::at_EMPTY_PATH);auto named=service.stat("/dir");CHECK(cwd&&named&&cwd.value().attributes.inode==named.value().attributes.inode&&cwd.value().attributes.device==named.value().attributes.device);
 CHECK(service.chdir("/"));CHECK(disk.writes==0&&disk.flushes==flushes&&testing::ext4_clock::calls()==0);
 std::puts("PASS hosted common service/provider/codec; profiles=10 guest=0 dispatcher=0");
}
