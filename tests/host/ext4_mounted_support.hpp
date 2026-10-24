#pragma once
#include "ext4_owned_support.hpp"
namespace owned_fixture {
inline Bytes mounted_publication(const Bytes &original, Ext4InodeMetadataUpdate update)
{
 auto b=original;const usize j=journal(b),at=inode_offset(b,update.inode),target=at/1024,offset=at%1024;
 const u32 sequence=be32(b,j+24)+1;
 std::fill_n(b.begin()+j+1024,5*1024,0);
 std::copy_n(original.begin()+1024,1024,b.begin()+j+2048);
 w32(b,j+2048+96,r32(b,j+2048+96)|4);super_crc(b,j+2048);
 std::copy_n(original.begin()+target*1024,1024,b.begin()+j+3072);
 const usize inode=j+3072+offset;
 w16(b,inode,(r16(original,at)&0170000)|update.permissions);w16(b,inode+2,u16(update.uid));w16(b,inode+120,u16(update.uid>>16));
 w16(b,inode+24,u16(update.gid));w16(b,inode+122,u16(update.gid>>16));
 w32(b,inode+12,update.ctime_seconds);w32(b,inode+132,update.ctime_nanoseconds<<2);
 const u64 version=u64{r32(b,inode+36)}|(u64{r32(b,inode+152)}<<32);
 w32(b,inode+36,u32(version+1));w32(b,inode+152,u32((version+1)>>32));inode_crc(b,update.inode,inode);
 for(usize block:{1,4}){big32(b,j+block*1024,jbd2::magic);big32(b,j+block*1024+4,block==1?1:2);big32(b,j+block*1024+8,sequence);}
 big32(b,j+1024+12,1);big32(b,j+1024+44,u32(target));big32(b,j+1024+48,10);
 linux_payload_crc(b,false);linux_payload_crc(b,true);
 big32(b,j+4096+52,update.ctime_seconds);big32(b,j+4096+56,update.ctime_nanoseconds);journal_record_crc(b,j+4096,16);
 std::copy_n(b.begin()+j+2048,1024,b.begin()+1024);
 big32(b,j+24,sequence);big32(b,j+28,1);big32(b,j+252,0);big32(b,j+252,crc(~u32{0},b,j,1024));
 if(be32(b,j+3072)==jbd2::magic){big32(b,j+3072,0);big32(b,j+1024+48,11);linux_payload_crc(b,true);}
 return b;
}
inline Bytes mounted_completion(const Bytes &published, u32 ino)
{
 auto b=published;const usize j=journal(b),at=inode_offset(b,ino);
 std::copy_n(b.begin()+j+3072,1024,b.begin()+(at/1024)*1024);
 if(be32(b,j+1024+48)&jbd2::flag_escape)big32(b,(at/1024)*1024,jbd2::magic);
 w32(b,1120,r32(b,1120)&~4U);super_crc(b);
 big32(b,j+24,be32(b,j+24)+2);big32(b,j+28,0);big32(b,j+252,0);big32(b,j+252,crc(~u32{0},b,j,1024));return b;
}
}
