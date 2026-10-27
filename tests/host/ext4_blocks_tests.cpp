#include "ok/fs/ext4/layout.hpp"
#include <cstdio>
#include <cstdlib>
#include <array>
using namespace ok;
using namespace ok::fs;
static usize checks;
static void check(bool yes) { ++checks; if(!yes) {std::fprintf(stderr,"check %zu failed\n",checks);std::abort();} }
using Bytes=std::array<std::byte,256>;
static void w16(Bytes &p,usize at,u16 v) {p[at]=std::byte(v);p[at+1]=std::byte(v>>8);}
static void w32(Bytes &p,usize at,u32 v) {w16(p,at,static_cast<u16>(v));w16(p,at+2,static_cast<u16>(v>>16));}
static Bytes plain(u32 block,u32 type=mode_regular) {Bytes p{};w16(p,0,static_cast<u16>(type|0644));w32(p,4,block);w32(p,28,block/512);w32(p,32,0x80000);w16(p,40,0xf30a);w16(p,42,1);w16(p,44,4);w16(p,56,1);w32(p,60,100);return p;}
int main()
{
 for(usize length:{usize{128},usize{256}}) for(u32 block:{1024u,2048u,4096u})
 {
  auto range=[](u32 first,u32 count){return first>=64 && first+count<=2048;};
  auto test=[&](Bytes p,StatusCode status,u64 value=0){const auto before=p;auto r=ext4_allocated_blocks512(std::span<const std::byte>{p}.first(length),block,2048,range);check(r.status().code()==status);if(r)check(r.value()==value);check(p==before);};
  test(plain(block),StatusCode::ok,block/512);test(plain(block,mode_directory),StatusCode::ok,block/512);
  for(u32 type:{mode_regular,mode_directory,mode_symlink})
  {
   auto valid=plain(block,type);if(type==mode_symlink)w32(valid,4,80);
   for(u16 maximum:{u16{1},u16{2},u16{3}})
   {
    auto reduced=valid;w16(reduced,44,maximum);test(reduced,StatusCode::unsupported);
   }
   auto invalid=valid;w16(invalid,44,0);test(invalid,StatusCode::io_error);
   invalid=valid;w16(invalid,44,1);w16(invalid,42,2);test(invalid,StatusCode::io_error);
   for(u16 depth:{u16{1},u16{2},u16{3},u16{4},u16{5},u16{32}})
   {
    auto tree=valid;w16(tree,46,depth);
    // Tree metadata can contribute allocation too. Do not compare the inline
    // sector formula before recognizing an unsupported tree-shaped snapshot.
    w32(tree,28,(depth+1)*block/512);test(tree,StatusCode::unsupported);
    w16(tree,42,0);test(tree,StatusCode::io_error);
   }
   invalid=valid;w16(invalid,46,33);test(invalid,StatusCode::io_error);
  }
  auto p=plain(block);w16(p,42,0);w32(p,4,0);w32(p,28,0);test(p,StatusCode::ok,0);
  // A sparse inode with one allocated block after a hole reports allocation,
  // never rounded logical length.
  p=plain(block);w32(p,4,5*block);w32(p,52,4);test(p,StatusCode::ok,block/512);
  p=plain(block);w32(p,4,7*block);w16(p,42,4);w32(p,28,4*block/512);
  for(usize i=0;i<4;++i){w32(p,52+12*i,static_cast<u32>(2*i));w16(p,56+12*i,1);w32(p,60+12*i,static_cast<u32>(100+2*i));}
  test(p,StatusCode::ok,4*block/512);
  auto overlap=p;w32(overlap,72,100);test(overlap,StatusCode::io_error);
  auto logical=p;w32(logical,64,0);test(logical,StatusCode::io_error);
  p=plain(block);w32(p,28,0);test(p,StatusCode::io_error);
  p=plain(block);w16(p,56,0);test(p,StatusCode::io_error);
  p=plain(block);w16(p,56,32769);test(p,StatusCode::unsupported);
  p=plain(block);w16(p,58,1);test(p,StatusCode::unsupported);
  p=plain(block);w32(p,60,63);test(p,StatusCode::io_error);
  p=plain(block);w32(p,60,2048);test(p,StatusCode::io_error);
  p=plain(block);w32(p,52,0xffffffff);test(p,StatusCode::unsupported);
  w16(p,56,2);test(p,StatusCode::io_error);
  p=plain(block);w32(p,4,0);test(p,StatusCode::unsupported);
  p=plain(block);w16(p,40,0);test(p,StatusCode::io_error);
  p=plain(block);w16(p,42,5);test(p,StatusCode::io_error);
  p=plain(block);w16(p,44,5);test(p,StatusCode::io_error);
  p=plain(block);w16(p,46,1);test(p,StatusCode::unsupported);
  p=plain(block);w16(p,46,33);test(p,StatusCode::io_error);
  for(usize at:{usize{104},usize{108},usize{116},usize{118}}){p=plain(block);w16(p,at,1);test(p,StatusCode::unsupported);}
  p=plain(block);w32(p,32,0x40000);test(p,StatusCode::unsupported);
  p=plain(block);w16(p,0,0020600);test(p,StatusCode::unsupported);
  // Symlink bytes and zero allocated blocks are distinct from direct pointers.
  p={};w16(p,0,static_cast<u16>(mode_symlink|0777));w32(p,4,59);test(p,StatusCode::ok,0);
  w32(p,4,60);test(p,StatusCode::io_error);
  w32(p,4,0);test(p,StatusCode::io_error);
  p=plain(block,mode_symlink);w32(p,4,80);test(p,StatusCode::ok,block/512);
  p=plain(block,mode_symlink);w32(p,4,80);w32(p,52,1);test(p,StatusCode::io_error);
  p={};w16(p,0,static_cast<u16>(mode_symlink|0777));w32(p,4,80);w32(p,28,block/512);w32(p,40,100);test(p,StatusCode::ok,block/512);
  w32(p,44,101);test(p,StatusCode::io_error);w32(p,44,0);
  w32(p,28,2*block/512);test(p,StatusCode::io_error);
  w32(p,28,block/512);w32(p,40,0);test(p,StatusCode::io_error);
  for(usize bad:{usize{0},usize{127},usize{129},usize{255}})check(ext4_allocated_blocks512(std::span<const std::byte>{p}.first(bad),block,2048,range).status().code()==StatusCode::invalid_argument);
 }
 std::printf("PASS pure allocation model checks=%zu geometries=6 provider=0 guest=0\n",checks);
}
