// Hosted adversarial fixture; production never disables access control.
#include "ext4_mounted_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <memory>
using namespace owned_fixture;
using Mount=Ext4MountedMetadata<1>;
struct Fixture
{
    Disk disk;
    Mount::Registry registry;
    Mount mount;
    Mount::Registry::Registration key;
    explicit Fixture(const Bytes &b):disk(b)
    {
        auto registration=registry.register_device(disk,17);check(registration.ok(),"persistent registration");key=registration.value();
        status(mount.mount(registry,key,{0,b.size()/512}),"persistent mount");check(mount.root(),"persistent publication");
    }
};
constexpr Ext4InodeMetadataUpdate next{16,5000,6000,1700000001,42,0604};
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    std::ifstream in(argv[1],std::ios::binary);Bytes clean((std::istreambuf_iterator<char>(in)),{});usize cases{};
    {
        auto f=std::make_unique<Fixture>(clean);status(f->mount.update_metadata(update),"serial first success");
        const auto bytes=f->disk.durable;const auto writes=f->disk.writes,flushes=f->disk.flushes;
        f->mount.owner_.cache_.transaction_serial_=~u64{0};
        for(usize retry=0;retry<2;++retry)
        {
            check(f->mount.update_metadata(next).code()==StatusCode::overflow,"persistent transaction serial refuses wrap");
            check(f->mount.report().phase==Ext4MountPhase::readable&&f->mount.report().epoch==2&&f->mount.root()->query_metadata().ok(),"serial refusal retains old readable epoch");
            check(f->mount.owner_.cache_.transaction_serial_==~u64{0}&&f->disk.writes==writes&&f->disk.flushes==flushes&&f->disk.durable==bytes,"rearm never resets exhausted serial or writes");
        }
        ++cases;
    }
    {
        auto f=std::make_unique<Fixture>(clean);status(f->mount.update_metadata(update),"generation first success");
        Mount::Owner::Pin pin;status(f->mount.owner_.pin(1,pin),"persistent generation pin");
        auto &entry=f->mount.owner_.cache_.entries_[pin.slot_];entry.report.generation=~u64{0}-1;pin.generation_=entry.report.generation;
        status(pin.release(),"persistent generation release");const auto bytes=f->disk.durable;const auto writes=f->disk.writes,flushes=f->disk.flushes;
        check(f->mount.update_metadata(next).code()==StatusCode::overflow,"persistent generation reserves publication and checkpoint increments");
        check(f->mount.report().phase==Ext4MountPhase::readable&&f->mount.report().epoch==2&&f->mount.root()->query_metadata().ok(),"generation refusal retains old readable view");
        check(f->disk.durable==bytes&&f->disk.writes==writes&&f->disk.flushes==flushes&&!f->mount.buffers().transaction_active&&!f->mount.buffers().read_pins,"generation reservation rolls back without mutation");
        ++cases;
    }
    {
        auto f=std::make_unique<Fixture>(clean);const auto *address=&f->mount.owner_.transaction_;
        status(f->mount.update_metadata(update),"reuse first success");const auto serial=f->mount.owner_.cache_.transaction_serial_;
        std::array<u64,8> generations{};for(usize i=0;i<8;++i)generations[i]=f->mount.owner_.cache_.entries_[i].report.generation;
        status(f->mount.update_metadata(next),"reuse different participant success");
        check(&f->mount.owner_.transaction_==address&&f->mount.owner_.cache_.transaction_serial_==serial+1&&f->mount.buffers().session_serial==1&&f->mount.report().epoch==3,"same-address reconstruction preserves lifetime counters");
        for(usize i=0;i<8;++i)check(f->mount.owner_.cache_.entries_[i].report.generation>=generations[i],"canonical generation never resets on reconstruction");
        ++cases;
    }
    // Reproduce a second operation's private checkpoint boundary after the first
    // public update drained/reconstructed its operation objects at the same address.
    for(usize mutation=0;mutation<9;++mutation)
    {
        auto f=std::make_unique<Fixture>(clean);u64 previous_transaction{},previous_domain{};
        f->disk.hook=[&](char op){if(op=='r'&&f->mount.owner_.report_.phase==Ext4OwnedPhase::checkpointing){previous_transaction=f->mount.owner_.authority_.transaction;previous_domain=f->mount.owner_.authority_.domain;}};
        status(f->mount.update_metadata(update),"stale authority first public success");f->disk.hook={};
        check(previous_transaction&&previous_domain&&!f->mount.owner_.authority_.active,"old durable authority consumed before reuse");
        f->mount.report_.phase=Ext4MountPhase::updating;f->mount.owner_.publication_domain_=f->mount.report_.epoch;
        status(f->mount.owner_.prepare(next),"second private prepare");Mount::reset(f->mount.view_);
        status(f->mount.owner_.commit(),"second private publication");
        auto foreign=std::make_unique<Mount>();auto &a=f->mount.owner_.authority_;
        check(a.transaction!=previous_transaction&&a.domain!=previous_domain,"fresh operation binds distinct serial and epoch");
        switch(mutation)
        {
            case 0:a.transaction=previous_transaction;break;
            case 1:a.domain=previous_domain;break;
            case 2:++a.first_sector;break;
            case 3:--a.sectors;break;
            case 4:a.owner=&foreign->owner_;break;
            case 5:a.cache=&foreign->owner_.cache_;break;
            case 6:a.session=&foreign->owner_.cache_.session_;break;
            case 7:a.images=&foreign->owner_.sealed_;break;
            case 8:--a.generations[1];break;
        }
        const auto bytes=f->disk.durable;const auto writes=f->disk.writes,flushes=f->disk.flushes,reads=f->disk.reads;
        const auto status=f->mount.owner_.checkpoint();check(!status,"stale or foreign persistent authority rejected");
        check(f->disk.writes==writes&&f->disk.flushes==flushes&&f->disk.reads==reads&&f->disk.durable==bytes,"persistent authority refused before checkpoint I/O");
        static_cast<void>(f->mount.fail(status));
        check(f->mount.report().phase==Ext4MountPhase::failed&&!f->mount.nodes_[0].query_metadata()&&!f->registry.detach(f->key),"persistent authority failure retains quarantine");
        ++cases;
    }
    std::printf("PASS mounted persistent invariants cases=%zu exhaustion_after_success=2 same_address_reuse=1 stale_foreign_authority_after_success=9\n",cases);
}
