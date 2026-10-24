#pragma once
#include <ok/fs/ext4_owned_transaction.hpp>
#include <functional>
#include "ext4_transaction_support.hpp"
#include <algorithm>
#include <filesystem>
using namespace ok;
using namespace ok::fs;
using namespace fixture;
namespace owned_fixture {
inline void optional_revoke_fixture(Bytes &bytes)
{
    const usize first = journal(bytes);
    const u32 features = be32(bytes, first + 40);
    check(features == ok::fs::jbd2::feature_checksum_v3 ||
          features == (ok::fs::jbd2::feature_revoke | ok::fs::jbd2::feature_checksum_v3),
          "owned optional revoke source fixture feature word");
    big32(bytes, first + 40, ok::fs::jbd2::feature_revoke | ok::fs::jbd2::feature_checksum_v3);
    big32(bytes, first + 252, 0);
    big32(bytes, first + 252, crc(~u32{0}, bytes, first, 1024));
}
struct Write { u64 sector; Bytes bytes; };
struct Trace { char operation; u64 sector; usize size; };
struct Disk final : driver::BlockDevice
{
    explicit Disk(const Bytes &data) : live(data), durable(data) {}
    Bytes live, durable;
    std::vector<Write> pending;
    std::vector<Write> submitted;
    std::vector<Trace> trace;
    usize reads{}, writes{}, flushes{}, fail_read{}, fail_write{}, fail_flush{},
          fail_write_after{}, fail_flush_after{}, cut{};
    u32 persist_mask{};
    u64 origin{};
    bool writable{true};
    Status read_error{Status::io_error("injected completion read")};
    Status flush_error{Status::io_error("injected completion flush")};
    std::function<void(char)> hook;
    driver::BlockGeometry geometry() const override { return {origin+live.size()/512,512,writable}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if(hook)hook('r');
        ++reads;trace.push_back({'r',sector,out.size()});
        if (reads==fail_read)return read_error;
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
        if(hook)hook('w');
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
        if(writes==fail_write_after)
        {
            persist(write,write.bytes.size());
            return Status::io_error("injected completion write after effect");
        }
        pending.push_back(std::move(write));return {};
    }
    Status flush() override
    {
        if(hook)hook('f');
        ++flushes;trace.push_back({'f',0,0});
        if(flushes==fail_flush){selective();return flush_error;}
        for(const auto &write:pending)persist(write,write.bytes.size());
        pending.clear();
        if(flushes==fail_flush_after)return Status::io_error("injected completion flush after effect");
        return {};
    }
    void crash()
    {
        live=durable;pending.clear();trace.clear();
        reads=writes=flushes=fail_read=fail_write=fail_flush=fail_write_after=fail_flush_after=cut=persist_mask=0;
    }
};

constexpr Ext4InodeMetadataUpdate update{12,2000,2001,1700000000,123456789,0640};
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
