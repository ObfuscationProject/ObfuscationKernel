#include "ok/boot/e820.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace ok;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(false)
std::array<std::byte,16+128*24> blob;
void w32(usize at,u32 value){for(usize i=0;i<4;++i)blob[at+i]=std::byte(value>>(i*8));}
void w64(usize at,u64 value){w32(at,value);w32(at+4,value>>32);}
int main(){
    w32(0,boot::e820_handoff_magic);w32(4,1);w32(8,3);w32(12,24);
    w64(16,0x1'0000'0000ull);w64(24,0x200000);w32(32,1);w32(36,1);
    w64(40,0x1'0000'1000ull);w64(48,0x1000);w32(56,2);w32(60,1);
    w64(64,0x1'0000'5000ull);w64(72,0x1000);w32(80,1);w32(84,0);
    boot::E820Map map;CHECK(boot::parse_e820_handoff({blob.data(),88},map));
    CHECK(map.count==3 && map.records[0].address==0x1'0000'0000ull && map.records[0].usable());
    CHECK(!map.records[1].usable() && !map.records[2].usable());
    const auto expected=map;
    auto rejected=[&](usize n){CHECK(!boot::parse_e820_handoff({blob.data(),n},map));CHECK(std::memcmp(&map,&expected,sizeof map)==0);};
    for(usize size=0;size<88;++size)rejected(size);
    w32(0,0);rejected(88);w32(0,boot::e820_handoff_magic);
    for(u32 status:{0u,2u,3u,4u}){w32(4,status);rejected(88);}w32(4,1);
    for(u32 count:{0u,129u,~u32{0}}){w32(8,count);rejected(blob.size());}w32(8,3);
    w32(12,20);rejected(blob.size());w32(12,24);
    w64(16,~u64{0}-10);w64(24,11);rejected(88);w64(16,0x1'0000'0000ull);w64(24,0x200000);
    for(usize i=3;i<128;++i){w64(16+i*24,i*4096);w64(24+i*24,4096);w32(32+i*24,i%4?1:99);w32(36+i*24,1);}
    w32(8,128);CHECK(boot::parse_e820_handoff(blob,map));CHECK(map.count==128 && !map.records[4].usable());
    w64(24,0x3'0000'0000ull);w64(72,0);w32(80,99);w32(84,0x80000002);
    CHECK(boot::parse_e820_handoff(blob,map));
    CHECK(map.records[0].size==0x3'0000'0000ull);
    CHECK(map.records[2].size==0 && map.records[2].type==99 && map.records[2].attributes==0x80000002 && !map.records[2].usable());
    puts("PASS hosted E820: exact 64-bit ranges, reservations, disabled/unknown records, capacity, truncation and unchanged failure output");
}
