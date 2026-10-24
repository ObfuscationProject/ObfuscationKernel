// Compile only this hosted adversarial test with -fno-access-control. Production
// access control remains enabled; callers cannot obtain these private fields.
#include "ext4_owned_support.hpp"
#include <memory>
using namespace owned_fixture;
using Owner = Ext4OwnedTransaction<>;
int main(int argc, char **argv)
{
    if(argc!=2)return 2;
    std::ifstream input(argv[1],std::ios::binary);check(bool(input),"invariant input");
    const Bytes original((std::istreambuf_iterator<char>(input)),{});
    usize cases{};
    {
        Disk disk(original);Owner::Registry registry;auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;Owner::Pin pin;
        const auto key=registry.register_device(disk,1);check(key.ok(),"generation registration");
        status(owner.open(registry,key.value(),{0,original.size()/512}),"generation open");
        status(owner.pin(1,pin),"generation initial pin");
        auto &entry=owner.cache_.entries_[pin.slot_];entry.report.generation=~u64{0}-1;pin.generation_=entry.report.generation;
        status(pin.release(),"generation release");
        check(owner.prepare(update).code()==StatusCode::overflow && !disk.writes && !disk.flushes &&
              owner.report().phase==Ext4OwnedPhase::rejected && !owner.buffers().transaction_active,"reserve commit and cleanup generations before I/O");
        status(owner.close(),"generation clean close");status(registry.detach(key.value()),"generation clean detach");++cases;
    }
    for(usize mutation=0;mutation<7;++mutation)
    {
        Disk disk(original);Owner::Registry registry;auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;
        const auto key=registry.register_device(disk,1);check(key.ok(),"private registration");
        status(owner.open(registry,key.value(),{0,original.size()/512}),"private open");status(owner.prepare(update),"private prepare");
        auto &entry=owner.cache_.entries_[owner.pins_[1].slot_];
        switch(mutation)
        {
            case 0:owner.sealed_[1][0]^=1;break;
            case 1:entry.staged[0]^=std::byte{1};break;
            case 2:++owner.transaction_.serial_;break;
            case 3:++entry.report.generation;break;
            case 4:++entry.report.block;break;
            case 5:entry.report.transaction_serial=0;break;
            case 6:entry.report.staged_dirty=false;break;
        }
        check(!owner.commit() && !disk.writes && !disk.flushes && owner.report().phase==Ext4OwnedPhase::failed &&
              registry.inspect(key.value()).value().state==block::DeviceState::quarantined,"private image/identity cannot grant publication");++cases;
    }
    for(usize mutation=0;mutation<15;++mutation)
    {
        Disk disk(original);Owner::Registry registry;auto owner_storage = std::make_unique<Owner>(); auto &owner = *owner_storage;
        const auto key=registry.register_device(disk,1);check(key.ok(),"authority registration");
        status(owner.open(registry,key.value(),{0,original.size()/512}),"authority open");status(owner.prepare(update),"authority prepare");
        status(owner.commit(),"authority actual publication");
        auto &a=owner.authority_;auto &entry=owner.cache_.entries_[owner.pins_[1].slot_];
        switch(mutation)
        {
            case 0:a.active=false;break;
            case 1:a.owner=nullptr;break;
            case 2:a.cache=nullptr;break;
            case 3:a.session=nullptr;break;
            case 4:a.images=nullptr;break;
            case 5:++a.identity;break;
            case 6:++a.registration;break;
            case 7:++a.session_serial;break;
            case 8:++a.transaction;break;
            case 9:++a.sequence;break;
            case 10:++a.slots[1];break;
            case 11:++a.generations[1];break;
            case 12:entry.canonical[0]^=std::byte{1};break;
            case 13:entry.staged[0]^=std::byte{1};break;
            case 14:owner.sealed_[1][0]^=1;break;
        }
        const auto before=disk.durable;const auto reads=disk.reads;
        check(!owner.checkpoint() && disk.writes==7 && disk.flushes==5 && disk.reads==reads && disk.durable==before &&
              owner.report().phase==Ext4OwnedPhase::failed && owner.buffers().transaction_active &&
              registry.inspect(key.value()).value().state==block::DeviceState::quarantined,"authority binds exact durable owner/session/version/image");++cases;
    }
    std::printf("PASS hosted owned ext4 invariant cases=%zu production_access_control=unchanged\n",cases);
}
