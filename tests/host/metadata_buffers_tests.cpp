#include <ok/fs/metadata_buffers.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <type_traits>
#include <vector>

using namespace ok;
using namespace ok::block;
using namespace ok::fs;
using Buffers = MetadataBuffers<2,1024,2>;
using Registry = Buffers::Registry;
namespace
{
usize checks{};
void check(bool condition,const char *why)
{
    ++checks;
    if (!condition) { std::fprintf(stderr,"FAIL metadata buffers: %s\n",why);std::exit(1); }
}
struct Device final : driver::BlockDevice
{
    driver::BlockGeometry shape{64,512,true};
    std::vector<std::byte> bytes=std::vector<std::byte>(64*4096);
    usize reads{},writes{},flushes{};
    bool fail_read{},change_after{};
    u64 origin{},last_sector{};
    std::function<void()> callback;
    Device() { for(usize n=0;n<bytes.size();++n)bytes[n]=std::byte{static_cast<u8>(n*7+n/512)}; }
    driver::BlockGeometry geometry() const override { return shape; }
    Status read_blocks(u64 first,std::span<std::byte> out) override
    {
        ++reads;last_sector=first;
        if(callback)callback();
        if(first<origin||first-origin>=64||out.size()>bytes.size()-(first-origin)*shape.block_size)
            return Status::io_error("model extent");
        std::copy_n(bytes.begin()+(first-origin)*shape.block_size,out.size(),out.begin());
        if(change_after)--shape.block_count;
        return fail_read?Status::io_error("injected completed read error"):Status{};
    }
    Status write_blocks(u64,std::span<const std::byte>) override { ++writes;return Status::io_error("unexpected home write"); }
    Status flush() override { ++flushes;return Status::io_error("unexpected flush"); }
};
template<usize Size> void transfer_cases()
{
    using Cache=MetadataBuffers<1,Size,2>;
    for(usize sector=512;sector<=Size;sector*=2)
        for(int relation=-1;relation<=1;++relation)
        {
            Device disk;disk.shape.block_size=sector;Registry registry;Cache cache,again;typename Cache::Pin pin;
            const usize limit=relation<0?Size/2:relation==0?Size:Size*2;
            const auto key=registry.register_device(disk,1,limit);
            if(limit<sector)
            {
                check(!key&&key.status().code()==StatusCode::invalid_argument&&disk.reads==0,"sub-sector transport bound rejects registration");
                check(registry.register_device(disk,1,Size).ok(),"invalid registration bound releases capacity");
                continue;
            }
            check(key.ok(),"metadata transfer-bound registration");
            const auto result=cache.open(registry,key.value(),{0,64},SessionAccess::readonly);
            if(limit<Size)
            {
                check(result.code()==StatusCode::unsupported&&disk.reads==0,"short transfer limit rejected during open without I/O");
                check(registry.inspect(key.value()).value().state==DeviceState::ready,"short transfer limit leaves healthy session reusable");
                typename Cache::Registry::Session direct;
                check(registry.acquire(key.value(),{0,64},SessionAccess::readonly,direct).ok()&&direct.close().ok(),"short transfer geometry permits clean reacquire");
            }
            else
            {
                check(result.ok()&&cache.pin(0,pin).ok()&&disk.reads==1,"equal or larger transfer bound admits one block");
                check(pin.release().ok()&&cache.close().ok(),"transfer-bound owner drains");
            }
            check(registry.detach(key.value()).ok(),"transfer preflight permits clean detach");
            const auto replacement=registry.register_device(disk,1,Size);
            check(replacement.ok()&&again.open(registry,replacement.value(),{0,64},SessionAccess::readonly).ok()&&again.close().ok(),"healthy re-registration after transfer preflight");
            check(!disk.writes&&!disk.flushes,"transfer admission has no write or flush");
        }
}
template<usize Size> void geometry_cases()
{
    using Cache=MetadataBuffers<1,Size,2>;
    for(usize sector=512;sector<=4096;sector*=2)
    {
        Device disk;disk.shape.block_size=sector;Registry registry;Cache cache;typename Cache::Pin pin;
        const auto key=registry.register_device(disk,1);check(key.ok(),"geometry registration");
        const auto result=cache.open(registry,key.value(),{0,64},SessionAccess::readonly);
        if(sector>Size)
        {
            check(result.code()==StatusCode::invalid_argument&&disk.reads==0,"unsupported block/sector ratio");
            check(registry.detach(key.value()).ok(),"geometry rejection releases session");
            continue;
        }
        check(result.ok(),"supported block/sector ratio");
        check(cache.pin(3,pin).ok()&&disk.last_sector==3*(Size/sector),"whole metadata block transfer");
        std::array<std::byte,Size> data{};
        check(pin.read(0,data).ok()&&std::equal(data.begin(),data.end(),disk.bytes.begin()+3*Size),"metadata block exact bytes");
        check(pin.release().ok()&&cache.close().ok()&&registry.detach(key.value()).ok(),"geometry release");
        check(!disk.writes&&!disk.flushes,"metadata reads never mutate");
    }
}
}
int main()
{
    static_assert(!std::is_copy_constructible_v<Buffers> && !std::is_move_constructible_v<Buffers>);
    static_assert(!std::is_copy_constructible_v<Buffers::Pin> && !std::is_move_constructible_v<Buffers::Pin>);
    static_assert(!std::is_copy_constructible_v<Buffers::Transaction> && !std::is_move_constructible_v<Buffers::Transaction>);
    std::array<std::byte,1024> bytes{},other{};
    std::array<std::byte,4> edited{std::byte{0xaa},std::byte{0xbb},std::byte{0xcc},std::byte{0xdd}};
    {
        Device disk;Registry registry;Buffers cache,alias;Buffers::Pin a,b,c,d,excess;
        const auto key=registry.register_device(disk,77);check(key.ok(),"register canonical owner");
        check(cache.open(registry,key.value(),{2,30},SessionAccess::writable).ok(),"open canonical owner");
        const auto identity=cache.report();
        check(identity.device_identity==77&&identity.registration_generation==1&&identity.session_serial==1&&
              identity.first_sector==2&&identity.sectors==30&&identity.blocks==15,"immutable partition/session identity");
        check(alias.open(registry,key.value(),{32,16},SessionAccess::readonly).code()==StatusCode::busy,"disjoint canonical alias excluded");
        check(registry.detach(key.value()).code()==StatusCode::busy,"canonical owner retains registry session");
        check(cache.pin(15,a).code()==StatusCode::invalid_argument&&disk.reads==0,"block bounds before I/O");
        check(cache.pin(1,a).ok()&&disk.reads==1&&disk.last_sector==4,"canonical miss");
        check(cache.pin(1,b).ok()&&disk.reads==1,"duplicate hit shares canonical version");
        check(a.report().value().generation==b.report().value().generation&&a.report().value().read_pins==2,"shared pin accounting");
        check(a.read(0,bytes).ok()&&b.read(0,other).ok()&&bytes==other&&std::equal(bytes.begin(),bytes.end(),disk.bytes.begin()+2048),"canonical exact read");
        check(a.read(1024,edited).code()==StatusCode::invalid_argument&&a.read(1023,edited).code()==StatusCode::invalid_argument&&
              a.read(0,{}).code()==StatusCode::invalid_argument,"byte range checks");
        check(cache.pin(2,c).ok()&&disk.reads==2,"second canonical slot");
        check(cache.pin(3,excess).code()==StatusCode::no_memory&&disk.reads==2,"pinned pressure before I/O");
        check(cache.close().code()==StatusCode::busy,"close retains pins");
        const auto generation=a.report().value().generation;
        check(a.release().ok()&&!a.release()&&!a.read(0,bytes),"released pin cannot read or double retire");
        check(b.release().ok()&&cache.pin(3,d).ok()&&d.report().value().generation>generation,"clean eviction increments generation");
        check(!a.read(0,bytes)&&cache.pin(1,a).code()==StatusCode::already_exists,"old one-shot pin cannot alias replacement");
        check(c.release().ok()&&d.release().ok()&&cache.close().ok(),"close drained owner");
        check(cache.pin(0,excess).code()==StatusCode::not_initialized&&registry.detach(key.value()).ok(),"closed owner cannot reload");
        check(cache.open(registry,key.value(),{0,64},SessionAccess::readonly).code()==StatusCode::already_exists,"owner cannot rebind identity");
        check(disk.writes==0&&disk.flushes==0,"canonical owner has no writeback path");
    }
    {
        Device disk,foreign_disk;Registry registry;Buffers cache,foreign;Buffers::Pin a,b,same,foreign_pin,pressure;
        const auto key=registry.register_device(disk,1),foreign_key=registry.register_device(foreign_disk,2);
        check(key.ok()&&foreign_key.ok(),"transaction devices");
        check(cache.open(registry,key.value(),{0,64},SessionAccess::writable).ok()&&
              foreign.open(registry,foreign_key.value(),{0,64},SessionAccess::writable).ok(),"transaction owners");
        check(cache.pin(1,a).ok()&&cache.pin(2,b).ok()&&cache.pin(1,same).ok()&&foreign.pin(1,foreign_pin).ok(),"transaction participants");
        Buffers::Transaction transaction,competing;
        std::array<Buffers::Pin*,2> invalid{&a,&foreign_pin},duplicate{&a,&same},participants{&a,&b};
        check(cache.begin(invalid,transaction).code()==StatusCode::bad_descriptor,"foreign participant rejection");
        check(!a.report().value().transaction_serial&&!cache.report().transaction_active,"all-participant preflight leaves no reservation");
        check(cache.begin(duplicate,transaction).code()==StatusCode::invalid_argument,"canonical alias cannot consume duplicate credit");
        check(cache.begin({},transaction).code()==StatusCode::invalid_argument,"empty transaction rejects");
        check(cache.begin(participants,transaction).ok(),"all credits reserved before mutation");
        check(cache.begin(participants,competing).code()==StatusCode::busy,"exclusive private-version owner");
        check(a.read(0,bytes).ok()&&transaction.read(1,0,other).ok()&&bytes==other,"private version starts canonical");
        check(transaction.edit(1,3,edited).ok()&&a.report().value().staged_dirty,"private mutation recorded");
        check(transaction.read(1,0,other).ok()&&std::equal(edited.begin(),edited.end(),other.begin()+3),"private mutation readback");
        check(a.read(0,other).ok()&&other==bytes&&same.read(0,other).ok()&&other==bytes,"canonical readers retain old authoritative version");
        check(transaction.edit(8,0,edited).code()==StatusCode::bad_descriptor&&transaction.edit(1,1023,edited).code()==StatusCode::invalid_argument,"transaction ownership/range rejection");
        check(a.release().ok()&&b.release().ok()&&same.release().ok(),"reader pins can retire while transaction retains blocks");
        check(cache.pin(3,pressure).code()==StatusCode::no_memory&&disk.reads==2,"transaction pin forbids clean eviction");
        check(cache.close().code()==StatusCode::busy&&disk.writes==0&&disk.flushes==0,"transaction cannot implicitly close or write home");
        check(transaction.discard().ok()&&!transaction.discard()&&!transaction.edit(1,0,edited),"private transaction one-shot discard");
        check(cache.pin(1,pressure).ok()&&pressure.read(0,other).ok()&&other==bytes,"discard preserves canonical bytes");
        check(pressure.release().ok()&&cache.close().ok(),"discard releases all private credits");
        check(foreign_pin.release().ok()&&foreign.close().ok(),"foreign owner unchanged");
    }
    {
        Device disk;Registry registry;Buffers cache;const auto key=registry.register_device(disk,1);check(key.ok(),"scope register");
        check(cache.open(registry,key.value(),{0,64},SessionAccess::writable).ok(),"scope open");
        {Buffers::Pin pin;check(cache.pin(1,pin).ok(),"scope pin");
         {Buffers::Transaction transaction;std::array<Buffers::Pin*,1> pins{&pin};
          check(cache.begin(pins,transaction).ok()&&transaction.edit(1,0,edited).ok(),"scope private edit");}
         check(!pin.report().value().transaction_serial&&!pin.report().value().staged_dirty,"transaction destructor discards private state");}
        check(!cache.report().read_pins&&cache.close().ok()&&!disk.writes&&!disk.flushes,"pin destructor retires and close never writes");
    }
    {
        Device disk;Registry registry;Buffers cache;Buffers::Pin pin;Buffers::Transaction tx;
        const auto key=registry.register_device(disk,1);check(key.ok(),"readonly register");
        check(cache.open(registry,key.value(),{0,64},SessionAccess::readonly).ok()&&cache.pin(0,pin).ok(),"readonly canonical read");
        std::array<Buffers::Pin*,1> pins{&pin};
        check(cache.begin(pins,tx).code()==StatusCode::denied&&!cache.report().transaction_active,"readonly cannot own private mutations");
        check(pin.release().ok()&&cache.close().ok(),"readonly close");
    }
    for(int fault=0;fault<2;++fault)
    {
        Device disk;Registry registry;Buffers cache,alias;Buffers::Pin pin,later;
        const auto key=registry.register_device(disk,1);check(key.ok(),"fault registration");
        check(cache.open(registry,key.value(),{0,64},SessionAccess::writable).ok(),"fault open");
        disk.fail_read=fault==0;disk.change_after=fault==1;
        check(!cache.pin(0,pin),"fault rejects buffer publication");
        check(cache.report().state==MetadataOwnerState::failed&&!cache.report().in_flight&&!cache.report().read_pins,"fault retires request pin but retains failed owner");
        const auto reads=disk.reads;
        check(!cache.pin(1,later)&&!cache.close()&&!pin.read(0,bytes)&&disk.reads==reads,"failed owner admits no more I/O or copies");
        check(registry.inspect(key.value()).value().state==DeviceState::quarantined&&registry.detach(key.value()).code()==StatusCode::denied,"failed backing remains registered");
        check(alias.open(registry,key.value(),{0,64},SessionAccess::readonly).code()==StatusCode::denied,"failed device cannot get new cache alias");
        check(disk.writes==0&&disk.flushes==0,"failure never writes corrective metadata");
    }
    {
        Device disk;Registry registry;Buffers cache;Buffers::Pin stable,loading,alias,rejected;
        const auto key=registry.register_device(disk,1);check(key.ok(),"thread register");
        check(cache.open(registry,key.value(),{0,64},SessionAccess::writable).ok()&&cache.pin(0,stable).ok(),"thread canonical prefix");
        std::atomic<bool> entered{},leave{};Status result;
        disk.callback=[&]{entered.store(true,std::memory_order_release);while(!leave.load(std::memory_order_acquire))std::this_thread::yield();};
        std::thread worker([&]{result=cache.pin(1,loading);});
        while(!entered.load(std::memory_order_acquire))std::this_thread::yield();
        check(cache.report().in_flight&&cache.report().read_pins==2,"request reserves backing and pending pin");
        check(stable.read(0,bytes).ok()&&cache.pin(0,alias).ok(),"cached reads continue during unrelated load");
        check(cache.pin(1,rejected).code()==StatusCode::busy&&cache.pin(2,rejected).code()==StatusCode::busy,"loading alias and parallel misses excluded");
        Buffers::Transaction tx;std::array<Buffers::Pin*,1> pins{&stable};
        check(cache.begin(pins,tx).code()==StatusCode::busy&&cache.close().code()==StatusCode::busy,"load prevents transaction/close retirement");
        leave.store(true,std::memory_order_release);worker.join();check(result.ok(),"thread load completes");
        check(cache.report().read_pins==3&&!cache.report().in_flight,"completed request publishes one pin");
        check(stable.release().ok()&&alias.release().ok()&&loading.release().ok()&&cache.close().ok(),"thread drains ownership");
    }
    {
        Device disk;Registry registry;Buffers cache;Buffers::Pin stable,failed,later;Buffers::Transaction tx;
        const auto key=registry.register_device(disk,1);check(key.ok(),"transaction failure register");
        check(cache.open(registry,key.value(),{0,64},SessionAccess::writable).ok()&&cache.pin(0,stable).ok(),"transaction failure canonical prefix");
        std::array<Buffers::Pin*,1> pins{&stable};
        check(cache.begin(pins,tx).ok()&&tx.edit(0,0,edited).ok(),"transaction failure private prefix");
        disk.fail_read=true;check(!cache.pin(1,failed),"independent load fails owner with private state");
        check(!stable.read(0,bytes)&&!tx.read(0,0,bytes)&&!tx.edit(0,0,edited),"quarantine blocks cached and private exposure");
        check(stable.release().ok()&&tx.discard().ok()&&!cache.report().read_pins&&!cache.report().transaction_active,"quarantined handles can retire logical ownership");
        check(!cache.pin(2,later)&&!cache.close()&&registry.detach(key.value()).code()==StatusCode::denied,"retirement cannot reset quarantined backing");
    }
    {
        Device disk;Registry registry;Buffers cache;Buffers::Pin pin;
        const auto key=registry.register_device(disk,1);check(key.ok(),"reentrant abort register");
        check(cache.open(registry,key.value(),{0,64},SessionAccess::writable).ok(),"reentrant abort open");
        disk.callback=[&]{check(cache.report().in_flight,"metadata lock released across transport");
                         check(cache.abort(Status::interrupted("integrity abort")).ok(),"reentrant integrity abort");
                         check(cache.report().in_flight,"integrity abort retains pending request backing");};
        disk.fail_read=true;
        check(!cache.pin(0,pin),"aborted load not published");
        check(cache.report().first_error==StatusCode::interrupted&&registry.inspect(key.value()).value().first_error==StatusCode::interrupted,"first error survives later transport failure");
        check(!cache.report().in_flight&&!cache.report().read_pins,"aborted completion drains only request reservation");
    }
    {
        Device disk;Registry registry;const auto key=registry.register_device(disk,1);check(key.ok(),"abandoned owner register");
        {Buffers cache;check(cache.open(registry,key.value(),{0,64},SessionAccess::writable).ok(),"abandoned owner open");}
        check(registry.inspect(key.value()).value().state==DeviceState::quarantined,"owner destruction without explicit close quarantines");
    }
    {
        Device disk;Registry registry;Buffers invalid,valid;const auto key=registry.register_device(disk,1);check(key.ok(),"partial block register");
        check(invalid.open(registry,key.value(),{0,63},SessionAccess::readonly).code()==StatusCode::invalid_argument&&disk.reads==0,"partial trailing block rejects");
        check(valid.open(registry,key.value(),{0,64},SessionAccess::readonly).ok()&&valid.close().ok(),"invalid shape releases session");
    }
    {
        Device disk;disk.origin=(u64{1}<<40)+3;disk.shape.block_count=disk.origin+64;Registry registry;Buffers cache;Buffers::Pin pin;
        const auto key=registry.register_device(disk,1);check(key.ok(),"wide register");
        check(cache.open(registry,key.value(),{disk.origin,64},SessionAccess::readonly).ok()&&cache.pin(31,pin).ok()&&disk.last_sector==disk.origin+62,"u64 partition-relative metadata key");
        check(pin.release().ok()&&cache.close().ok(),"wide close");
    }
    geometry_cases<1024>();geometry_cases<2048>();geometry_cases<4096>();
    transfer_cases<1024>();transfer_cases<2048>();transfer_cases<4096>();
    std::printf("PASS hosted metadata buffers checks=%zu host_threads=1 home_writes=0 native_smp=0\n",checks);
}
