#include "ext4_owned_support.hpp"
#include <memory>
#include <atomic>
#include <thread>
#include <type_traits>
using namespace owned_fixture;
using Owner = Ext4OwnedTransaction<>;
using Registry = Owner::Registry;
namespace
{
usize cases{};
void verify(bool value, const char *why) { ++cases; fixture::check(value, why); }
Registry::Registration open(Disk &disk, Registry &registry, Owner &owner, usize limit = 1024)
{
    const auto key = registry.register_device(disk, 17, limit); verify(key.ok(), "owned registration");
    status(owner.open(registry, key.value(), {disk.origin, disk.live.size()/512}), "owned open");
    return key.value();
}
void image(Owner &owner, u64 block, const Bytes &expected)
{
    Owner::Pin pin; std::array<std::byte, 1024> bytes{};
    status(owner.pin(block, pin), "owned image pin"); status(pin.read(0, bytes), "owned image read");
    verify(std::equal(bytes.begin(), bytes.end(), reinterpret_cast<const std::byte *>(expected.data()+block*1024)), "owned canonical image");
    status(pin.release(), "owned image release");
}
void quarantined(Owner &owner, Registry &registry, Registry::Registration key, Disk &disk)
{
    const usize operations = disk.reads + disk.writes + disk.flushes;
    verify(owner.report().phase == Ext4OwnedPhase::failed, "owned failure state");
    verify(owner.buffers().state == MetadataOwnerState::failed, "owned cache failure state");
    verify(registry.inspect(key).value().state == block::DeviceState::quarantined, "owned failure quarantine");
    Owner::Pin pin;
    verify(owner.pin(1, pin).code() == StatusCode::denied && owner.commit().code() == StatusCode::denied &&
           owner.checkpoint().code() == StatusCode::denied && owner.discard().code() == StatusCode::denied &&
           owner.close().code() == StatusCode::denied, "owned failure denies reuse");
    Registry::Session competing;
    verify(registry.acquire(key, {0,disk.live.size()/512}, block::SessionAccess::readonly, competing).code()==StatusCode::denied,
           "owned failure denies competing owner");
    verify(registry.detach(key).code()==StatusCode::denied && operations==disk.reads+disk.writes+disk.flushes,
           "owned failure retains transport without I/O");
}
}
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    std::ifstream input(argv[1], std::ios::binary); verify(bool(input), "owned input");
    const Bytes original((std::istreambuf_iterator<char>(input)), {});
    const auto publication = published_oracle(original), completion = completed_oracle(publication);
    const usize target = inode_offset(original, 12)/1024, control = journal(original)/1024;
    const std::filesystem::path out=argv[2]; std::filesystem::create_directories(out);
    static_assert(!std::is_copy_constructible_v<Owner> && !std::is_move_constructible_v<Owner>);
    usize prepare_reads{}, checkpoint_reads{};
    {
        Disk disk(original); Registry registry; auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage; const auto key=open(disk,registry,owner);
        Owner::Pin old; status(owner.pin(target,old),"old reader");
        std::array<std::byte,1024> old_bytes{},after{}; status(old.read(0,old_bytes),"old snapshot");
        status(owner.prepare(update),"owned prepare"); prepare_reads=disk.reads;
        verify(disk.writes==0 && disk.flushes==0 && disk.durable==original,"owned preparation cannot mutate");
        status(old.read(0,after),"old retained read"); verify(after==old_bytes,"prepared private bytes hidden");
        verify(owner.commit().code()==StatusCode::busy && disk.writes==0 && disk.flushes==0,"reader prevents publication before mutation");
        verify(owner.close().code()==StatusCode::busy,"prepared must discard or publish");
        status(old.release(),"old reader release");
        bool callback=false;
        disk.hook=[&](char) {
            Owner::Pin reentrant;
            verify(owner.pin(target,reentrant).code()==StatusCode::busy,"publication blocks reentrant reader");
            verify(owner.close().code()==StatusCode::busy && owner.checkpoint().code()==StatusCode::busy,"publication blocks reentrant operation");
            verify(owner.report().phase==Ext4OwnedPhase::publishing,"publication observation does not block I/O"); callback=true;
        };
        status(owner.commit(),"owned durable publication"); disk.hook={};
        verify(callback && owner.report().writer.published() && disk.writes==7 && disk.flushes==5,"owned publication report");
        verify(disk.live==publication && disk.durable==publication,"owned exact publication oracle");
        auto logical=publication;std::copy_n(completion.begin()+target*1024,1024,logical.begin()+target*1024);
        image(owner,target,logical); image(owner,1,publication); image(owner,control,publication);
        verify(owner.buffers().read_pins==3 && owner.buffers().transaction_active,"durable versions retain all three credits");
        verify(owner.discard().code()==StatusCode::busy && owner.close().code()==StatusCode::busy,"durable transaction cannot discard or close");
        Owner::Pin held;status(owner.pin(target,held),"durable reader");
        verify(owner.checkpoint().code()==StatusCode::busy && disk.writes==7 && disk.flushes==5,"durable reader prevents checkpoint I/O");
        status(held.release(),"durable reader release");
        save(out/"published.img",disk.durable);
        const usize before=disk.reads;
        status(owner.checkpoint(),"owned checkpoint"); checkpoint_reads=disk.reads-before;
        verify(disk.live==completion && disk.durable==completion && disk.writes==11 && disk.flushes==9,"owned exact completion oracle");
        image(owner,target,completion); image(owner,1,completion); image(owner,control,completion);
        verify(!owner.buffers().transaction_active && !owner.buffers().read_pins,"checkpoint retires all private credits");
        save(out/"completed.img",disk.durable);
        status(owner.close(),"owned drained close"); status(registry.detach(key),"owned detach");
    }
    {
        Disk disk(original); Registry registry; auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage; const auto key=open(disk,registry,owner);
        status(owner.prepare(update),"discard prepare"); Owner::Pin old;status(owner.pin(target,old),"discard reader");
        status(owner.discard(),"discard prepared version with old reader");
        verify(owner.close().code()==StatusCode::busy,"discard close waits for reader");
        image(owner,target,original); status(old.release(),"discard reader release"); status(owner.close(),"discard close");
        verify(!disk.writes&&!disk.flushes&&disk.durable==original,"discard leaves disk unchanged");status(registry.detach(key),"discard detach");
    }
    {
        Disk disk(original); disk.origin=(u64{1}<<35)+11; Registry registry; auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage; const auto key=open(disk,registry,owner);
        status(owner.prepare(update),"partition prepare");status(owner.commit(),"partition commit");status(owner.checkpoint(),"partition checkpoint");
        verify(disk.durable==completion,"partition origin translated once");status(owner.close(),"partition close");status(registry.detach(key),"partition detach");
    }
    {
        Disk disk(original); Registry registry; auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage; const auto key=open(disk,registry,owner);
        Owner::Pin occupied;status(owner.pin(0,occupied),"unrelated retained slot");
        verify(owner.prepare(update).code()==StatusCode::no_memory && !disk.writes && !disk.flushes,"reserve every cache credit before mutation");
        verify(owner.report().phase==Ext4OwnedPhase::rejected && !owner.buffers().transaction_active,"capacity rejection drains private work");
        status(occupied.release(),"capacity release");status(owner.close(),"capacity close");status(registry.detach(key),"capacity detach");
    }
    {
        Disk disk(original); Registry registry; auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;const auto key=open(disk,registry,owner);
        std::atomic<bool> entered{},release{};
        disk.hook=[&](char op){if(op=='r'){entered.store(true);while(!release.load())std::this_thread::yield();}};
        Owner::Pin pending;Status pinned;
        std::thread reader([&]{pinned=owner.pin(1,pending);});while(!entered.load())std::this_thread::yield();
        verify(owner.prepare(update).code()==StatusCode::busy && owner.close().code()==StatusCode::busy,"reader admission gates engine session reads");
        release.store(true);reader.join();disk.hook={};status(pinned,"concurrent reader completed");status(pending.release(),"concurrent reader release");
        status(owner.prepare(update),"prepare after admission");status(owner.discard(),"concurrent discard");status(owner.close(),"concurrent close");status(registry.detach(key),"concurrent detach");
    }
    for(usize point=1;point<=prepare_reads;++point)
    {
        Disk disk(original);Registry registry;auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;const auto key=open(disk,registry,owner);disk.fail_read=point;
        verify(!owner.prepare(update)&&disk.writes==0&&disk.flushes==0&&disk.durable==original,"owned prepare read failure no mutation");
        quarantined(owner,registry,key,disk);
    }
    for(usize point=1;point<=checkpoint_reads;++point)
    {
        Disk disk(original);Registry registry;auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;const auto key=open(disk,registry,owner);
        status(owner.prepare(update),"checkpoint read-fault prepare");status(owner.commit(),"checkpoint read-fault publish");disk.fail_read=disk.reads+point;
        verify(!owner.checkpoint(),"owned checkpoint read failure");quarantined(owner,registry,key,disk);
        verify(owner.buffers().transaction_active && owner.buffers().read_pins==3,"checkpoint failure retains ownership");
    }
    usize restarts{}, torn{};
    for(usize kind=0;kind<2;++kind)
        for(usize point=1;point<=(kind?9:11);++point)
            for(usize cut: {usize{0},usize{512},usize{1024}})
            {
                Disk disk(original);
                {
                    Registry registry;auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;const auto key=open(disk,registry,owner);
                    status(owner.prepare(update),"fault prepare");
                    if(kind)disk.fail_flush=point;else disk.fail_write=point;
                    disk.cut=cut;disk.persist_mask=cut?15:0;
                    const auto published=owner.commit();const auto result=published?owner.checkpoint():published;
                    verify(!result,"owned write/flush fault propagated");quarantined(owner,registry,key,disk);
                    verify(owner.buffers().transaction_active && owner.buffers().read_pins==3,"fault retains private transaction");
                }
                disk.crash();Ext4RecoveryCompletion<> recover;const auto result=recover.run(disk,{0,disk.live.size()/512});
                if(!result){verify(disk.writes==0&&disk.flushes==0,"torn control recovery refuses mutation");++torn;}
                else {
                    auto old=original;std::copy_n(disk.durable.begin()+control*1024+1024,5*1024,old.begin()+control*1024+1024);
                    verify(disk.durable==completion||disk.durable==old,"fresh recovery old/new whole disk");++restarts;
                }
            }
    {
        Disk disk(original);Registry registry;const auto key=registry.register_device(disk,1);verify(key.ok(),"destruction registration");
        {auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;status(owner.open(registry,key.value(),{0,original.size()/512}),"destruction open");status(owner.prepare(update),"destruction prepare");status(owner.commit(),"destruction publish");}
        verify(registry.inspect(key.value()).value().state==block::DeviceState::quarantined,"durable destructor cannot release journal ownership");
    }
    {
        auto corrupt=original;corrupt[inode_offset(corrupt,12)+124]^=1;Disk disk(corrupt);Registry registry;auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;const auto key=open(disk,registry,owner);
        Owner::Pin old;status(owner.pin(target,old),"corrupt old pin");verify(!owner.prepare(update),"integrity rejected");
        std::array<std::byte,1> byte{};verify(old.read(0,byte).code()==StatusCode::denied,"failure invalidates retained reader access");
        quarantined(owner,registry,key,disk);status(old.release(),"quarantined reader retirement");
    }
    for(usize request=0;request<3;++request)
    {
        Disk disk(original);Registry registry;auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;const auto key=open(disk,registry,owner);
        auto invalid=update;
        // Directory inode 2 is supported only when its ownership is preserved.
        if(request==0)invalid.inode=2;
        if(request==1)invalid.permissions=010000;
        if(request==2)invalid.inode=1000; // Unallocated fixture inode.
        const auto rejected=owner.prepare(invalid);
        verify(!rejected&&owner.report().phase==Ext4OwnedPhase::rejected&&!disk.writes&&!disk.flushes,
               "local unsupported/input rejection remains healthy");
        status(owner.close(),"rejected clean close");status(registry.detach(key),"rejected clean detach");
    }
    {
        Disk disk(original);Registry registry;auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;
        const auto key=registry.register_device(disk,1,512);verify(key.ok(),"short transfer registration");
        verify(owner.open(registry,key.value(),{0,original.size()/512}).code()==StatusCode::unsupported&&
               disk.reads==0&&registry.inspect(key.value()).value().state==block::DeviceState::ready,"short transfer rejects cleanly before I/O");
        status(registry.detach(key.value()),"short transfer detach");
    }
    std::printf("PASS hosted owned ext4 checks=%zu prepare_reads=%zu checkpoint_reads=%zu restarts=%zu torn=%zu writes=11 flushes=9\n",cases,prepare_reads,checkpoint_reads,restarts,torn);
}
