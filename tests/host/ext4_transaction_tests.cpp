#include <ok/fs/ext4_metadata_transaction.hpp>
#include "ext4_transaction_support.hpp"
#include <algorithm>
#include <filesystem>
using namespace ok;
using namespace ok::fs;
using namespace fixture;
namespace {
struct Write { u64 sector; Bytes bytes; };
struct Trace { char operation; u64 sector; usize size; };
struct Disk final : driver::BlockDevice
{
    explicit Disk(const Bytes &data) : live(data), durable(data) {}
    Bytes live, durable;
    std::vector<Write> pending;
    std::vector<Write> submitted;
    std::vector<Trace> trace;
    usize reads{}, writes{}, flushes{}, fail_read{}, fail_write{}, fail_flush{}, cut{};
    u32 persist_mask{};
    u64 origin{};
    bool writable{true};
    driver::BlockGeometry geometry() const override { return {origin+live.size()/512,512,writable}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        ++reads;trace.push_back({'r',sector,out.size()});
        if (reads==fail_read)return Status::io_error("injected completion read");
        if(sector<origin)return Status::io_error("completion model volume origin");
        sector-=origin;
        if (sector>=live.size()/512 || out.size()%512 || out.size()/512>live.size()/512-sector)
            return Status::io_error("completion model read bounds");
        std::copy_n(live.begin()+sector*512,out.size(),reinterpret_cast<u8*>(out.data()));return {};
    }
    void persist(const Write &write, usize count)
    {
        std::copy_n(write.bytes.begin(),count,durable.begin()+write.sector*512);
    }
    void selective()
    {
        for(usize i=0;i<pending.size();++i)if(persist_mask&(1U<<i))persist(pending[i],pending[i].bytes.size());
    }
    Status write_blocks(u64 sector, std::span<const std::byte> bytes) override
    {
        ++writes;trace.push_back({'w',sector,bytes.size()});
        if(sector<origin)return Status::io_error("completion model write origin");
        sector-=origin;
        if (!writable || sector>=live.size()/512 || bytes.size()!=1024 || bytes.size()/512>live.size()/512-sector)
            return Status::io_error("completion model write bounds");
        Write write{sector,Bytes(reinterpret_cast<const u8*>(bytes.data()),reinterpret_cast<const u8*>(bytes.data())+bytes.size())};
        submitted.push_back(write);
        if(writes==fail_write)
        {
            selective();persist(write,cut);
            std::copy_n(write.bytes.begin(),cut,live.begin()+sector*512);
            return Status::io_error("injected completion partial write");
        }
        std::copy(write.bytes.begin(),write.bytes.end(),live.begin()+sector*512);
        pending.push_back(std::move(write));return {};
    }
    Status flush() override
    {
        ++flushes;trace.push_back({'f',0,0});
        if(flushes==fail_flush){selective();return Status::io_error("injected completion flush");}
        for(const auto &write:pending)persist(write,write.bytes.size());
        pending.clear();return {};
    }
    void crash()
    {
        live=durable;pending.clear();trace.clear();reads=writes=flushes=fail_read=fail_write=fail_flush=cut=persist_mask=0;
    }
};

constexpr Ext4InodeMetadataUpdate update{12,2000,2001,1700000000,123456789,0640};
Status commit(Disk &disk,Ext4MetadataTransaction &writer)
{ return writer.commit(disk,{disk.origin,disk.live.size()/512},update); }
void save(const std::filesystem::path &path,const Bytes &data)
{
 std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(data.data()),data.size());
 check(bool(out),"save transaction artifact");
}
Bytes published_oracle(const Bytes &original)
{
 auto b=original;const usize j=journal(b),at=inode_offset(b,12),target=at/1024,offset=at%1024;
 const u32 sequence=be32(b,j+24)+1;
 std::fill_n(b.begin()+j+1024,5*1024,0);
 std::copy_n(original.begin()+1024,1024,b.begin()+j+2048);
 w32(b,j+2048+96,r32(b,j+2048+96)|4);super_crc(b,j+2048);
 std::copy_n(original.begin()+target*1024,1024,b.begin()+j+3072);
 const usize inode=j+3072+offset;
 w16(b,inode,0100000|update.permissions);w16(b,inode+2,u16(update.uid));w16(b,inode+120,u16(update.uid>>16));
 w16(b,inode+24,u16(update.gid));w16(b,inode+122,u16(update.gid>>16));
 w32(b,inode+12,update.ctime_seconds);w32(b,inode+132,update.ctime_nanoseconds<<2);
 const u64 version=u64{r32(b,inode+36)}|(u64{r32(b,inode+152)}<<32);
 w32(b,inode+36,u32(version+1));w32(b,inode+152,u32((version+1)>>32));inode_crc(b,12,inode);
 for(usize block:{1,4}){big32(b,j+block*1024,jbd2::magic);big32(b,j+block*1024+4,block==1?1:2);big32(b,j+block*1024+8,sequence);}
 big32(b,j+1024+12,1);big32(b,j+1024+44,u32(target));big32(b,j+1024+48,10);
 linux_payload_crc(b,false);linux_payload_crc(b,true);
 big32(b,j+4096+52,update.ctime_seconds);big32(b,j+4096+56,update.ctime_nanoseconds);journal_record_crc(b,j+4096,16);
 std::copy_n(b.begin()+j+2048,1024,b.begin()+1024);
 big32(b,j+24,sequence);big32(b,j+28,1);big32(b,j+252,0);big32(b,j+252,crc(~u32{0},b,j,1024));
 return b;
}
Bytes completed_oracle(const Bytes &published)
{
 auto b=published;const usize j=journal(b),at=inode_offset(b,12);
 std::copy_n(b.begin()+j+3072,1024,b.begin()+(at/1024)*1024);
 w32(b,1120,r32(b,1120)&~4U);super_crc(b);
 big32(b,j+24,be32(b,j+24)+2);big32(b,j+28,0);big32(b,j+252,0);big32(b,j+252,crc(~u32{0},b,j,1024));
 return b;
}
}
int main(int argc,char **argv)
{
 if(argc!=3&&argc!=4)return 2;
 std::ifstream input(argv[1],std::ios::binary);check(bool(input),"transaction fixture input");
 Bytes original((std::istreambuf_iterator<char>(input)),{});
    if (argc == 4 && std::string_view(argv[3]) == "revoke")
    {
        const usize first = journal(original);
        big32(original, first + 40, jbd2::feature_revoke | jbd2::feature_checksum_v3);
        big32(original, first + 252, 0);
        big32(original, first + 252, crc(~u32{0}, original, first, 1024));
    }
 if(argc==4&&std::string_view(argv[3])=="budget")
 {
  Disk disk(original);Ext4MetadataTransaction writer;const auto result=commit(disk,writer);
  check(!result&&result.code()==StatusCode::no_memory&&disk.writes==0&&disk.flushes==0&&disk.durable==original,"inode budget mutation");
  std::puts("PASS hosted ext4 transaction budget cases=1 writes=0 flushes=0");return 0;
 }
 const std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
 const Bytes publication=published_oracle(original),completion=completed_oracle(publication);
 Disk golden(original);Ext4MetadataTransaction writer;status(commit(golden,writer),"reference transaction");
 check(writer.report().published()&&writer.report().sequence==5&&golden.writes==7&&golden.flushes==5,"writer report");
 check(golden.durable==publication&&golden.live==publication,"independent publication oracle");
 const usize reads=golden.reads;usize cases=1,restarts=0,torn=0,boundaries=0,old_results=0,new_results=0;
 save(out/"published.img",golden.durable);save(out/"expected-published.img",publication);
 const auto before_repeat=golden.durable;check(commit(golden,writer).code()==StatusCode::already_exists&&golden.durable==before_repeat,"writer reuse");
 std::vector<Trace> operations;for(const auto &t:golden.trace)if(t.operation!='r')operations.push_back(t);
 const std::array<char,12> order{'f','w','w','w','w','f','w','f','w','f','w','f'};
 check(operations.size()==order.size(),"writer operation count");
 for(usize i=0;i<order.size();++i)check(order[i]==operations[i].operation,"writer ordering");
 const usize j=journal(original);check(operations[1].sector==j/512+2&&operations[4].sector==j/512+10&&
 operations[6].sector==j/512+8&&operations[8].sector==2&&operations[10].sector==j/512,"writer destinations");
 Ext4RecoveryCompletion<> done;status(done.run(golden,{0,original.size()/512}),"reference checkpoint");
 check(golden.durable==completion,"independent checkpoint oracle");save(out/"completed.img",golden.durable);
 const auto restart=[&](Disk &disk) {
  disk.crash();const auto before=disk.durable;Ext4RecoveryCompletion<> recovery;
  const auto result=recovery.run(disk,{disk.origin,disk.live.size()/512});
  if(!result){check(disk.writes==0&&disk.flushes==0&&disk.durable==before,"torn control mutated");++torn;return;}
  ++restarts;
  if(disk.durable==completion){++new_results;return;}
  auto expected_old=original;std::copy_n(before.begin()+j+1024,5*1024,expected_old.begin()+j+1024);
  check(disk.durable==expected_old,"unpublished recovery changed original metadata");++old_results;
  disk.crash();Ext4MetadataTransaction retry;status(commit(disk,retry),"retry unpublished writer");
  check(disk.durable==publication,"unpublished retry reused stale records");
 };
 for(usize point=1;point<=reads;++point){Disk disk(original);disk.fail_read=point;Ext4MetadataTransaction failed;
  check(!commit(disk,failed)&&disk.writes==0&&disk.flushes==0&&disk.durable==original,"read failure mutated");++cases;}
 for(usize point=1;point<=7;++point)
  for(u32 mask=0;mask<(point<=4?(1U<<(point-1)):1U);++mask)
   for(usize cut=0;cut<=1024;++cut)
   {
    ++boundaries;const auto &w=golden.submitted[point-1];
    if(cut&&w.bytes[cut-1]==original[w.sector*512+cut-1]&&cut!=512)continue;
    Disk disk(original);disk.fail_write=point;disk.cut=cut;disk.persist_mask=mask;Ext4MetadataTransaction failed;
    check(!commit(disk,failed)&&failed.report().write_attempts==point&&!failed.report().published(),"partial write error ignored");
    if(mask==0&&cut==512)save(out/("write-prefix-"+std::to_string(point)+".img"),disk.durable);
    restart(disk);++cases;
   }
 for(usize point=1;point<=5;++point)
  for(u32 mask=0;mask<(point==1?1U:point==2?16U:2U);++mask)
  {Disk disk(original);disk.fail_flush=point;disk.persist_mask=mask;Ext4MetadataTransaction failed;
   check(!commit(disk,failed)&&failed.report().flush_attempts==point&&!failed.report().published(),"flush error ignored");restart(disk);++cases;}
 const auto reject=[&](Bytes data,Ext4InodeMetadataUpdate request,const char *why){Disk disk(data);Ext4MetadataTransaction failed;
  check(!failed.commit(disk,{0,data.size()/512},request)&&disk.writes==0&&disk.flushes==0&&disk.durable==data,why);++cases;};
 {auto request=update;request.permissions=010000;reject(original,request,"invalid permission bits");}
 {auto request=update;request.ctime_nanoseconds=1000000000;reject(original,request,"invalid nanoseconds");}
 {auto request=update;request.ctime_seconds=0x80000000;reject(original,request,"unsupported ctime epoch");}
 {auto request=update;request.inode=2;reject(original,request,"directory mutation");}
 {auto request=update;request.inode=13;reject(original,request,"unallocated inode mutation");}
 {auto b=original;w32(b,1120,r32(b,1120)|4);super_crc(b);reject(b,update,"dirty empty journal writer");}
 reject(publication,update,"pending transaction writer");
 {auto b=original;big32(b,j+40,be32(b,j+40)|0x80000000U);big32(b,j+252,0);big32(b,j+252,crc(~u32{0},b,j,1024));reject(b,update,"unknown journal feature");}
 {auto b=original;big32(b,j+24,0xfffffffd);big32(b,j+252,0);big32(b,j+252,crc(~u32{0},b,j,1024));reject(b,update,"sequence wrap");}
 {auto b=original;w32(b,inode_offset(b,12)+36,~u32{0});w32(b,inode_offset(b,12)+152,~u32{0});inode_crc(b,12,inode_offset(b,12));reject(b,update,"inode version wrap");}
 {auto b=original;b[inode_offset(b,12)+124]^=1;reject(b,update,"bad inode checksum");}
 {auto b=original;w32(b,1024+232,12);super_crc(b);reject(b,update,"orphan chain");}
 {auto b=original;w32(b,1120,r32(b,1120)|0x80000000U);super_crc(b);reject(b,update,"unknown ext4 feature");}
 {auto b=original;big32(b,j+88,1);big32(b,j+252,0);big32(b,j+252,crc(~u32{0},b,j,1024));reject(b,update,"cyclic journal head");}
 {Disk disk(original);disk.writable=false;Ext4MetadataTransaction failed;check(!commit(disk,failed)&&disk.writes==0&&disk.flushes==0,"readonly writer");++cases;}
 {Disk disk(original);disk.origin=(u64{1}<<35)+11;Ext4MetadataTransaction moved;status(commit(disk,moved),"u64 origin");check(disk.durable==publication,"u64 publication");++cases;}
 {auto b=original;std::copy_n(publication.begin()+j+1024,1024,b.begin()+j+5120);big32(b,j+5120+8,6);journal_record_crc(b,j+5120,1020);
  Disk disk(b);Ext4MetadataTransaction next;status(commit(disk,next),"stale next-sequence boundary");check(disk.durable==publication,"zero sentinel not overwritten");++cases;}

 {Disk disk(completion);Ext4MetadataTransaction next;status(commit(disk,next),"next completed transaction");
  const auto second=published_oracle(completion);check(disk.durable==second&&next.report().sequence==8,"completed ring reuse oracle");
  Ext4RecoveryCompletion<> recovery;status(recovery.run(disk,{0,original.size()/512}),"second checkpoint");
  check(disk.durable==completed_oracle(second)&&recovery.report().journal_sequence==10,"second checkpoint oracle");++cases;}
 {auto b=original;big32(b,j+24,0xfffffffcU);big32(b,j+252,0);big32(b,j+252,crc(~u32{0},b,j,1024));
  Disk disk(b);Ext4MetadataTransaction last;status(commit(disk,last),"last nonwrapping transaction");
  const auto pub=published_oracle(b);check(disk.durable==pub,"last sequence oracle");
  Ext4RecoveryCompletion<> recovery;status(recovery.run(disk,{0,b.size()/512}),"last checkpoint");
  check(disk.durable==completed_oracle(pub)&&recovery.report().journal_sequence==0xffffffffU,"last sequence completion");
  reject(disk.durable,update,"completed sequence exhaustion");++cases;}
 std::printf("PASS hosted ext4 transaction cases=%zu reads=%zu restarts=%zu old=%zu new=%zu torn_rejections=%zu write_byte_boundaries=%zu writes=7 flushes=5\n",
  cases,reads,restarts,old_results,new_results,torn,boundaries);
}
