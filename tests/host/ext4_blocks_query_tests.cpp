#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4_fs.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <cstring>
using namespace ok;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
class Disk final:public driver::BlockDevice
{
 public:
 std::vector<std::byte> bytes;usize writes{},flushes{},reads{};bool fail_read{};
 explicit Disk(const char *name){FILE *file=std::fopen(name,"rb");CHECK(file);CHECK(!std::fseek(file,0,SEEK_END));bytes.resize(static_cast<usize>(std::ftell(file)));CHECK(!std::fseek(file,0,SEEK_SET));CHECK(std::fread(bytes.data(),1,bytes.size(),file)==bytes.size());std::fclose(file);}
 driver::BlockGeometry geometry()const override{return {bytes.size()/512,512,true};}
 Status read_blocks(u64 start,std::span<std::byte> out)override {++reads;if(fail_read)return Status::io_error("injected model read failure");if(start>bytes.size()/512||out.size()>bytes.size()-start*512)return Status::fault();std::memcpy(out.data(),bytes.data()+start*512,out.size());return {};}
 Status write_blocks(u64,std::span<const std::byte>)override{++writes;return Status::io_error("read-only query attempted mutation");}
 Status flush()override{++flushes;return {};}
};
int main(int argc,char **argv)
{
 CHECK(argc==8);Disk disk(argv[1]);testing::ext4_clock::reset();auto fs=std::make_unique<fs::Ext4FileSystem>();
 const bool maintained=std::strcmp(argv[2],"maintained")==0;
 const bool writable=std::strcmp(argv[2],"readonly")!=0;
 if(maintained)testing::ext4_clock::set({{1700000000,0},{0,1},1,time::RealtimeOrigin::administrator_software});
 CHECK(fs->mount(disk,fs::Ext4MountOptions{writable,maintained?fs::Ext4TimestampPolicy::maintained:fs::Ext4TimestampPolicy::unqualified}));
 testing::ext4_clock::reset();
 auto *root=fs->root();CHECK(root);auto found=root->lookup_child(argv[3]);CHECK(found);auto *node=found.value();
 const auto offset=static_cast<usize>(std::strtoull(argv[4],nullptr,10));CHECK(offset+128<=disk.bytes.size());
 const auto field=static_cast<usize>(std::strtoull(argv[5],nullptr,10));
 if(field)disk.bytes[offset+field]^=std::byte{1};
 const bool capacity=std::strncmp(argv[6],"capacity-",9)==0;
 const bool tree=std::strncmp(argv[6],"depth-",6)==0;
 if(capacity){
  const auto maximum=std::strtoul(argv[6]+9,nullptr,10);CHECK(maximum>=1&&maximum<=3);
  disk.bytes[offset+44]=std::byte(maximum);disk.bytes[offset+45]=std::byte{0};
 }
 if(tree){
  const auto depth=std::strtoul(argv[6]+6,nullptr,10);CHECK(depth==4||depth==5);
  disk.bytes[offset+46]=std::byte(depth);disk.bytes[offset+47]=std::byte{0};
 }
 if(std::strcmp(argv[6],"logical-overflow")==0){
  for(usize i=52;i<56;++i)disk.bytes[offset+i]=std::byte{255};
  disk.bytes[offset+56]=std::byte{2};disk.bytes[offset+57]=std::byte{0};
 }
 const bool io=std::strcmp(argv[6],"io")==0||std::strcmp(argv[6],"logical-overflow")==0;const bool unavailable=std::strcmp(argv[6],"unavailable")==0||capacity||tree;
 if(std::strcmp(argv[6],"read-fault")==0)disk.fail_read=true;
 const usize flushes=disk.flushes;auto metadata=node->query_metadata();
 if(io||disk.fail_read)CHECK(!metadata&&metadata.status().code()==StatusCode::io_error);
 else{CHECK(metadata);CHECK(metadata.value().attributes.has(fs::Attribute::blocks512)==!unavailable);if(!unavailable)CHECK(metadata.value().blocks==std::strtoull(argv[7],nullptr,10));
  const bool qualified_time=!writable||maintained;
  for(auto field:{fs::Attribute::access_time,fs::Attribute::modification_time,fs::Attribute::change_time})CHECK(metadata.value().attributes.has(field)==qualified_time);
 }
 if(maintained&&(io||disk.fail_read)){
  const auto reads=disk.reads;auto again=node->query_metadata();CHECK(!again&&again.status().code()==StatusCode::io_error);CHECK(disk.reads==reads);
 }
 if(maintained&&unavailable){
  // Bounded refusal must not quarantine unrelated healthy inode queries.
  auto healthy=root->query_metadata();CHECK(healthy&&healthy.value().attributes.has(fs::Attribute::blocks512));
 }
 CHECK(testing::ext4_clock::calls()==0&&disk.writes==0&&disk.flushes==flushes);
 std::puts("PASS hosted provider allocation query; clock samples=0 writes=0 query flushes=0");
}
