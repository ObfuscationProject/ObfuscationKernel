#include <ok/fs/ext4/txn.hpp>
#include "ext4_completion_support.hpp"
#include <algorithm>
#include <filesystem>
#include <set>

using namespace ok;
using namespace ok::fs;
using namespace fixture;
namespace
{
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
Bytes expected(const Bytes &original)
{
    auto b=original;const usize j=journal(b),size=block_size(b);
    check(size==1024,"bounded completion fixture");
    for(usize i=0;i<size;++i){b[size+i]=original[j+size*2+i];b[7*size+i]=original[j+size*3+i];}
    big32(b,j+24,be32(original,j+24)+2);big32(b,j+28,0);big32(b,j+252,0);
    big32(b,j+252,crc(~u32{0},b,j,1024));w32(b,1120,r32(b,1120)&~4U);super_crc(b);
    return b;
}
Status recover(Disk &disk, Ext4RecoveryCompletion<> &completion)
{
    return completion.run(disk,{disk.origin,disk.live.size()/512});
}
void save(const std::filesystem::path &where,const Bytes &bytes)
{
    std::ofstream out(where,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    check(bool(out),"write retained completion fixture");
}
}
int main(int argc,char **argv)
{
    if(argc!=3&&argc!=4)return 2;
    std::ifstream input(argv[1],std::ios::binary);check(bool(input),"completion fixture open");
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
        Disk disk(original);Ext4RecoveryCompletion<> completion;const auto result=recover(disk,completion);
        check(!result&&result.code()==StatusCode::no_memory&&disk.writes==0&&disk.flushes==0&&disk.durable==original,
              "ordinary inode inspection budget did not reject without mutation");
        std::puts("PASS hosted ext4 completion budget cases=1 writes=0 flushes=0");return 0;
    }
    const Bytes oracle=expected(original);
    const std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
    Disk golden(original);Ext4RecoveryCompletion<> success;
    status(recover(golden,success),"complete reference recovery");
    check(success.report().complete()&&success.report().validated_actions==2&&success.report().checked_inodes==2&&
          success.report().write_attempts==4&&success.report().flush_attempts==4&&success.report().journal_sequence==4,
          "completion report");
    check(golden.live==oracle&&golden.durable==oracle,"independent completion oracle mismatch");
    save(out/"completed.img",golden.durable);save(out/"expected.img",oracle);
    std::vector<Trace> operations;
    for(const auto &event:golden.trace)if(event.operation!='r')operations.push_back(event);
    check(operations.size()==8,"completion mutation order length");
    const std::array<char,8> order{'f','w','w','f','w','f','w','f'};
    for(usize i=0;i<order.size();++i)check(operations[i].operation==order[i],"completion durability order");
    check(operations[1].sector==2&&operations[2].sector==14&&operations[4].sector==journal(original)/512&&operations[6].sector==2,
          "completion ordered destinations");
    const usize reference_reads=golden.reads;usize cases=1,restarts=0,torn_rejections=0,boundaries=0;
    check(success.run(golden,{0,original.size()/512}).code()==StatusCode::already_exists,"completion reused");
    {
        Disk disk(oracle);Ext4RecoveryCompletion<> again;status(recover(disk,again),"clean idempotent recovery");
        check(again.report().already_clean&&disk.writes==0&&disk.flushes==1&&disk.durable==oracle,"clean retry durability");++cases;
    }
    for(usize failure=1;failure<=reference_reads;++failure)
    {
        Disk disk(original);disk.fail_read=failure;Ext4RecoveryCompletion<> interrupted;
        check(!recover(disk,interrupted)&&!interrupted.report().complete(),"read fault ignored");
        disk.crash();Ext4RecoveryCompletion<> retry;status(recover(disk,retry),"restart after read error");
        check(disk.durable==oracle,"read-failure restart oracle");++cases;++restarts;
    }
    for(usize failure=1;failure<=4;++failure)
    {
        // Every byte prefix is enumerated. Adjacent prefixes with identical
        // durable bytes are equivalent for the restarted state machine; execute
        // each distinct state once, retaining the boundary count separately.
        for(u32 mask=0;mask<(failure==2?2U:1U);++mask)
        {
            for(usize cut=0;cut<=1024;++cut)
            {
                ++boundaries;
                const auto &write=golden.submitted[failure-1];
                const u8 old_byte=cut ? (failure==4 ? golden.submitted[0].bytes[cut-1] : original[write.sector*512+cut-1]) : 0;
                if(cut&&write.bytes[cut-1]==old_byte&&cut!=512)continue;
                Disk disk(original);disk.fail_write=failure;disk.cut=cut;disk.persist_mask=mask;
                Ext4RecoveryCompletion<> interrupted;
                check(!recover(disk,interrupted)&&interrupted.report().write_attempts==failure&&!interrupted.report().complete(),"write fault ignored");
                if(cut==512)save(out/("torn-write-"+std::to_string(failure)+".img"),disk.durable);
                disk.crash();const auto before=disk.durable;Ext4RecoveryCompletion<> retry;
                const auto result=recover(disk,retry);
                if(result){check(disk.durable==oracle,"write-cut restart oracle");++restarts;}
                else
                {
                    check(retry.report().failed_phase==Ext4CompletionPhase::validating&&disk.writes==0&&disk.flushes==0&&disk.durable==before,
                          "torn state not rejected before mutation");++torn_rejections;
                }
                ++cases;
            }
        }
    }
    for(usize failure=1;failure<=4;++failure)
        for(u32 mask=0;mask<(failure==2?4U:failure==1?1U:2U);++mask)
        {
            Disk disk(original);disk.fail_flush=failure;disk.persist_mask=mask;Ext4RecoveryCompletion<> interrupted;
            check(!recover(disk,interrupted)&&interrupted.report().flush_attempts==failure&&!interrupted.report().complete(),"flush fault ignored");
            disk.crash();Ext4RecoveryCompletion<> retry;status(recover(disk,retry),"restart after subset flush");
            check(disk.durable==oracle,"flush-subset restart oracle");++cases;++restarts;
        }
    {
        Disk disk(original);disk.writable=false;Ext4RecoveryCompletion<> rejected;
        check(!recover(disk,rejected)&&disk.writes==0&&disk.flushes==0&&disk.durable==original,"readonly mutation");++cases;
    }
    {
        Disk disk(original);disk.origin=(u64{1}<<35)+11;Ext4RecoveryCompletion<> offset;
        status(recover(disk,offset),"u64 completion volume origin");check(disk.durable==oracle,"u64 completion oracle");++cases;
    }
    const auto rejected=[&](Bytes bytes,const char *reason) {
        Disk disk(bytes);Ext4RecoveryCompletion<> completion;const auto result=recover(disk,completion);
        check(!result&&disk.writes==0&&disk.flushes==0&&disk.durable==bytes&&disk.live==bytes,reason);++cases;
    };
    {
        auto b=original;const usize at=journal(b)+3*1024+inode_offset(b,12)%1024;
        w32(b,at+60,r32(b,at+60)+1);inode_crc(b,12,at);linux_payload_crc(b,true);
        rejected(b,"mapping-changing recovery wrote before rejection");
    }
    {
        auto b=original;const usize j=journal(b),target=r32(b,descriptor(b,0));
        std::copy_n(b.begin()+target*1024,1024,b.begin()+j+3*1024);
        big32(b,j+1024+44,u32(target));linux_payload_crc(b,true);
        rejected(b,"unchanged bitmap target exceeded completion profile");
    }
    {
        auto b=original;w32(b,1024+232,12);super_crc(b);rejected(b,"orphan chain admitted");
    }
    {
        auto b=original;big32(b,journal(b)+40,25);big32(b,journal(b)+252,0);
        big32(b,journal(b)+252,crc(~u32{0},b,journal(b),1024));rejected(b,"unsupported journal profile admitted");
    }
    {
        auto b=original;big32(b,journal(b)+88,1);big32(b,journal(b)+252,0);
        big32(b,journal(b)+252,crc(~u32{0},b,journal(b),1024));rejected(b,"cyclic journal head admitted");
    }
    {
        auto b=original;std::fill_n(b.begin()+journal(b)+4*1024,1024,0);rejected(b,"incomplete transaction admitted");
    }
    {
        auto b=oracle;w32(b,1120,r32(b,1120)|4);super_crc(b);b[inode_offset(b,12)+124]^=1;
        rejected(b,"empty journal cleared recovery before checking allocated inode");
    }
    for (bool empty_record : {false, true})
    {
        auto b = original; const usize first = journal(b);
        big32(b, first + 40, jbd2::feature_revoke | jbd2::feature_checksum_v3);
        big32(b, first + 252, 0); big32(b, first + 252, crc(~u32{0}, b, first, 1024));
        std::copy_n(b.begin() + first + 4096, 1024, b.begin() + first + 5120);
        std::fill_n(b.begin() + first + 4096, 1024, 0);
        std::fill_n(b.begin() + first + 6144, 1024, 0);
        big32(b, first + 4096, jbd2::magic); big32(b, first + 4100, 5);
        big32(b, first + 4104, be32(b, first + 24));
        big32(b, first + 4108, empty_record ? 16 : 20);
        if (!empty_record) big32(b, first + 4112, r32(b, descriptor(b, 0) + 4));
        journal_record_crc(b, first + 4096, 1020);
        Disk inspect(b); Ext4ReplayPreview<> preview;
        status(preview.open(inspect, {0, b.size() / 512}), "valid non-suppressing revoke preview");
        check(preview.report().planned_actions == 2 && preview.report().revoked_actions == 0 &&
              !inspect.writes && !inspect.flushes, "valid revoke changed canonical actions");
        rejected(b, empty_record ? "empty active revoke admitted" : "non-suppressing active revoke admitted");
    }
    std::printf("PASS hosted ext4 completion cases=%zu restarts=%zu torn_rejections=%zu write_byte_boundaries=%zu reads=%zu writes=4 flushes=4\n",
                cases,restarts,torn_rejections,boundaries,reference_reads);
}
