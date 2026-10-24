#include "ok/fs/ext4_blocks.hpp"
#include <cstdio>
#include <cstdlib>
using namespace ok;
int main(int argc,char **argv)
{
 if(argc!=6)return 2;
 std::array<std::byte,256> inode{};
 FILE *file=std::fopen(argv[1],"rb");if(!file)return 3;
 const usize length=std::fread(inode.data(),1,inode.size(),file);if(std::fgetc(file)!=EOF)return 4;std::fclose(file);
 const u32 block=static_cast<u32>(std::strtoul(argv[2],nullptr,10)),blocks=static_cast<u32>(std::strtoul(argv[3],nullptr,10));
 const u64 expected=std::strtoull(argv[4],nullptr,10);
 struct Range {u32 begin,end;};std::array<Range,128> reserved{};usize count=0;
 file=std::fopen(argv[5],"r");if(!file)return 5;
 while(count<reserved.size()&&std::fscanf(file,"%u %u",&reserved[count].begin,&reserved[count].end)==2)++count;
 std::fclose(file);
 auto result=fs::ext4_allocated_blocks512(std::span<const std::byte>{inode}.first(length),block,blocks,[&](u32 start,u32 n){
  for(usize i=0;i<count;++i)if(start<reserved[i].end&&reserved[i].begin<static_cast<u64>(start)+n)return false;
  return true;
 });
 if(!result||result.value()!=expected){std::fprintf(stderr,"allocation mismatch status=%u\n",static_cast<unsigned>(result.status().code()));return 6;}
 std::printf("PASS raw inode allocation blocks512=%llu inode_bytes=%zu\n",static_cast<unsigned long long>(result.value()),length);
}
