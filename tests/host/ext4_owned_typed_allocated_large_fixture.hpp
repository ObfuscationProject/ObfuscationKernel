#pragma once
// Author-adapted synthetic fixture from the preserved independent writer test.
// Original SHA256: abfa0e031fc2813e4f555b4709db9f32f36f3b891d4c0a7b6816188274b3d037
// Reusing this fixture is author regression evidence, not independent review.
#include <ok/fs/ext4/extent.hpp>
#include <cstdio>
#include <cstdlib>
namespace typed_owner_allocated_large_fixture {
using namespace ok; using namespace ok::fs;
using Block = Ext4FirstDataBlock;
#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "allocated owner fixture:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)
void put16(std::span<u8> b,usize a,u16 v) { b[a]=u8(v); b[a+1]=u8(v>>8); }
void put32(std::span<u8> b,usize a,u32 v) { for(usize i=0;i<4;++i)b[a+i]=u8(v>>(i*8)); }
void be32(std::span<u8> b,usize a,u32 v) { for(usize i=0;i<4;++i)b[a+i]=u8(v>>(24-i*8)); }
void stamp_inode(std::span<u8> b,u32 n,u32 seed)
{ put16(b,124,0);put16(b,130,0);const auto crc=jbd2::detail::crc32c(ext4_checked::inode_seed(seed,n,b),b);put16(b,124,u16(crc));put16(b,130,u16(crc>>16)); }
void stamp_descriptor(std::span<u8> b,u32 g,u32 seed)
{ put16(b,30,0);put16(b,30,u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed,ext4_checked::encoded(g)),b))); }
u32 pop(u8 m) { u32 n{};for(u32 i=0;i<4;++i)n+=(m>>i)&1;return n; }
struct Entry { u32 number{};Block bytes{}; };
struct Image
{
    Ext4RecoveryInfo binding{};
    Ext4AllocatedEmptyDataScratch planning{};
    Ext4AllocatedEmptyDataPlan plan{};
    Ext4AllocatedEmptyDataJournalFormatScratch formatting{};
    Ext4AllocatedEmptyDataJournalFormat journal{};
    std::array<Block,32> bitmap{},inode_bitmap{};
    Block super{},descriptors{},head{},data{},old_data{},directory{},empty_directory{},table_bytes{};
    std::array<Entry,32> changed{},stable{};usize changes{},stables{};
    u32 groups{},root_blocks{},target{},allocated_records{},seed{},candidate{},target_table{},count{},sequence{};
    u64 version{},volume{};
    static u32 begin(u32 g) { return 1+8192*g; }
    bool allocated(u32 number) const
    {
        if(number==2||number==8||number==target)return true;
        if(count==3)return false;
        return groups==1 ? number>=11&&number<=(count==129?137U:136U) : number>=11&&number<=(count==129?136U:135U);
    }
    void inode(u32 n,std::span<u8,256> out) const
    {
        for(auto &b:out)b=0;if(!allocated(n))return;
        put16(out,0,n==2?0040755:0100640);put16(out,26,n==2?2:1);
        put32(out,32,0x80000);put32(out,100,9);put16(out,40,0xf30a);put16(out,44,4);put16(out,128,32);
        const u64 v=n==target?version:42;put32(out,36,u32(v));put32(out,152,u32(v>>32));
        if(n==2||n==8)
        {
            const u32 len=n==2?root_blocks:1024;
            put32(out,4,len*1024);put32(out,28,len*2);put16(out,42,1);put16(out,56,u16(len));put32(out,60,n==2?3000:1000);
        }
        stamp_inode(out,n,seed);
    }
    void table(u32 group,u32 local,Block &out) const
    { for(u32 i=0;i<4;++i)inode(group*256+(local-4)*4+i+1,std::span<u8,256>{out.data()+i*256,256}); }
    void directory_block(Block &out,bool dots) const
    {
        out={};if(dots){put32(out,0,2);put16(out,4,12);out[6]=1;out[7]=2;out[8]='.';
          put32(out,12,2);put16(out,16,1000);out[18]=2;out[19]=2;out[20]=out[21]='.';}else put16(out,4,1012);
        put16(out,1016,12);out[1019]=0xde;std::array<u8,256> node{};inode(2,node);
        put32(out,1020,jbd2::detail::crc32c(ext4_checked::inode_seed(seed,2,node),std::span<const u8>{out}.first(1012)));
    }
    void store(u32 n,const Block &b)
    { for(usize i=0;i<changes;++i)if(changed[i].number==n){changed[i].bytes=b;return;}REQUIRE(changes<changed.size());changed[changes++]={n,b}; }
    const Block &wal(u32 n) const
    { if(!n)return journal.head;if(n==1)return journal.descriptor;if(n<6)return journal.payloads[n-2];return n==6?journal.commit:journal.end; }
    void render(u32 n,Block &out,bool durable=false) const
    {
        const auto &records=durable?stable:changed;const usize total=durable?stables:changes;
        for(usize i=0;i<total;++i)if(records[i].number==n){out=records[i].bytes;return;}
        if(n==1){out=super;return;}if(n==2){out=descriptors;return;}if(n==1000){out=head;return;}if(n==candidate){out=old_data;return;}
        out={};if(!n)return;const u32 g=(n-1)/8192,l=(n-1)%8192;REQUIRE(g<groups);
        if(l==2)out=bitmap[g];else if(l==3)out=inode_bitmap[g];else if(l>=4&&l<68)table(g,l,out);
        else if(n>=3000&&n<3000+root_blocks)out=n==3000?directory:empty_directory;
    }
    void initialize(u32 g,u32 roots,u32 records,bool recovery,u8 mask,u64 v,u32 seq,u32 size,u32 gap,u64 first)
    {
        groups=g;root_blocks=roots;count=records;version=v;sequence=seq;volume=first;target=g==1?13:269;
        candidate=begin(g-1)+70;target_table=begin((target-1)/256)+4+((target-1)%256)/4;
        changes=stables=0;binding={};super={};descriptors={};head={};data={};old_data={};allocated_records=0;
        bitmap.fill(Block{});inode_bitmap.fill(Block{});
        binding.layout={first,1+u64(g)*8192,1000,1024,1024};binding.metadata_count=g*4;binding.journal_inode=8;
        binding.needs_recovery=true;binding.empty_log=true;binding.journal_sequence=seq;
        for(usize i=0;i<16;++i)binding.uuid[i]=u8(0x20+i);seed=jbd2::detail::crc32c(~u32{0},binding.uuid);
        for(u32 group=0;group<g;++group)
        {
            const u32 b=begin(group);binding.metadata[group*4]={b,2};binding.metadata[group*4+1]={b+2,1};
            binding.metadata[group*4+2]={b+3,1};binding.metadata[group*4+3]={b+4,64};bitmap[group].fill(255);
        }
        const u32 bit=(candidate-1)%8192;bitmap[g-1][bit/8]&=u8(~(1U<<(bit%8)));
        for(u32 n=1;n<=g*256;++n)if(allocated(n)){++allocated_records;const u32 l=(n-1)%256;inode_bitmap[(n-1)/256][l/8]|=u8(1U<<(l%8));}
        REQUIRE(allocated_records==records);u32 total_free_inodes{};
        for(u32 group=0;group<g;++group)
        {
            auto d=std::span<u8>{descriptors}.subspan(group*32,32);const u32 b=begin(group);put32(d,0,b+2);put32(d,4,b+3);put32(d,8,b+4);
            u32 used{};for(u32 n=0;n<256;++n)used+=(inode_bitmap[group][n/8]>>(n%8))&1;const u32 free=256-used;total_free_inodes+=free;
            put16(d,12,group==g-1?1:0);put16(d,14,u16(free));put16(d,16,group?0:1);put16(d,18,4);
            put16(d,24,u16(jbd2::detail::crc32c(seed,bitmap[group])));put16(d,26,u16(jbd2::detail::crc32c(seed,std::span<const u8>{inode_bitmap[group]}.first(32))));stamp_descriptor(d,group,seed);
        }
        put32(super,0,g*256);put32(super,4,u32(binding.layout.filesystem_blocks));put32(super,12,1);put32(super,16,total_free_inodes);
        put32(super,20,1);put32(super,32,8192);put32(super,36,8192);put32(super,40,256);put16(super,56,0xef53);put16(super,58,1);
        put32(super,76,1);put32(super,84,11);put16(super,88,256);put32(super,92,4);put32(super,96,0x46);put32(super,100,0x400);
        for(usize i=0;i<16;++i)super[104+i]=binding.uuid[i];put32(super,224,8);put16(super,254,32);super[373]=1;
        put32(super,1020,jbd2::detail::crc32c(~u32{0},std::span<const u8>{super}.first(1020)));directory_block(directory,true);directory_block(empty_directory,false);
        be32(head,0,jbd2::magic);be32(head,4,u32(jbd2::RecordType::superblock_v2));be32(head,12,1024);be32(head,16,1024);be32(head,20,1);be32(head,24,seq);
        be32(head,40,jbd2::feature_checksum_v3|jbd2::feature_revoke);for(usize i=0;i<16;++i)head[48+i]=binding.uuid[i];be32(head,64,1);head[80]=4;
        be32(head,252,jbd2::detail::crc32c(~u32{0},head));
        for(u32 i=0;i<1024;++i){old_data[i]=u8(73+i*11);if(i>=gap&&i<size)data[i]=u8(1+(i*37)%251);}
        render(target_table,table_bytes);const Ext4AllocatedEmptyDataSource source{g-1,candidate,target_table,super,descriptors,bitmap[g-1],inode_bitmap[(target-1)/256],table_bytes};
        const Ext4AllocatedEmptyDataUpdate update{target,9,size,1700000999,999999999};
        REQUIRE(Ext4AllocatedEmptyDataPlanner::plan_off_stack(binding,update,source,planning,plan));
        REQUIRE(Ext4AllocatedEmptyDataJournalFormatter::format(binding,plan,head,inode_bitmap[(target-1)/256],formatting,journal));
        if(recovery){for(u32 i=0;i<8;++i)store(1000+i,wal(i));for(u32 h=0;h<4;++h)if(mask&(1U<<h))store(plan.targets[h],plan.after[h]);store(candidate,data);binding.empty_log=false;binding.journal_sequence=journal.active_sequence;}
        stable=changed;stables=changes;
    }
};
#undef REQUIRE
} // namespace typed_owner_allocated_large_fixture
