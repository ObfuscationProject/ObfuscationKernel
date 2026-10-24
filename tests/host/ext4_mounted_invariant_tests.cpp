// Compiled with -fno-access-control only to corrupt otherwise private authority.
#include "ext4_mounted_support.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>
using namespace owned_fixture;
using Mount = Ext4MountedMetadata<1>;
struct Fixture
{
    Disk disk;
    Mount::Registry registry;
    Mount mount;
    explicit Fixture(const Bytes &b) : disk(b)
    {
        auto key=registry.register_device(disk,17);check(key.ok(),"invariant registration");
        status(mount.mount(registry,key.value(),{0,b.size()/512}),"invariant mount");
    }
};
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    std::ifstream in(argv[1],std::ios::binary);Bytes clean((std::istreambuf_iterator<char>(in)),{});
    {
        auto f=std::make_unique<Fixture>(clean);auto *root=f->mount.root();
        for(usize i=1;i<f->mount.nodes_.size();++i)f->mount.nodes_[i].inode_=1000+static_cast<u32>(i);
        const auto writes=f->disk.writes;const auto reads=f->disk.reads;
        check(root->lookup_child("seed").status().code()==StatusCode::no_memory,"node capacity refusal");
        check(f->mount.report().phase==Ext4MountPhase::readable&&root->query_metadata().ok()&&f->disk.writes==writes,"capacity is nonfatal without writes");
        f->mount.nodes_[1].inode_=0;
        check(root->lookup_child("seed").ok()&&f->disk.reads>=reads,"healthy capacity control succeeds");
    }
    {
        auto f=std::make_unique<Fixture>(clean);auto *root=f->mount.root();f->mount.report_.epoch=~u64{0};
        check(f->mount.update_metadata(update).code()==StatusCode::overflow&&f->mount.report().phase==Ext4MountPhase::readable&&
              root->query_metadata().ok()&&!f->disk.writes,"epoch exhaustion is prewrite local refusal");
    }
    {
        auto f=std::make_unique<Fixture>(clean);auto *root=f->mount.root();++f->mount.identity_.first_sector;
        check(!f->mount.update_metadata(update)&&!root->query_metadata()&&!f->disk.writes,"foreign partition identity rejected before writes");
    }
    {
        auto f=std::make_unique<Fixture>(clean);auto *root=f->mount.root();bool once{};
        f->disk.hook=[&](char op){if(op=='r'&&!once){once=true;++f->mount.owner_.publication_domain_;}};
        check(!f->mount.update_metadata(update)&&!f->disk.writes&&f->mount.report().phase==Ext4MountPhase::failed,"foreign prepublication epoch rejected before writes");
        f->disk.hook={};check(!root->query_metadata(),"epoch poison checked node");
    }
    {
        auto f=std::make_unique<Fixture>(clean);auto *root=f->mount.root();++f->mount.binding_.metadata[0].first;
        check(!f->mount.update_metadata(update)&&f->disk.writes==11&&!root->query_metadata(),"binding renewal rechecks complete metadata domain");
    }
    {
        auto f=std::make_unique<Fixture>(clean);
        const auto io=f->disk.reads+f->disk.writes+f->disk.flushes;
        check(f->mount.owner_.cache_.session_.abort(Status::fault("external Session abort before root publication")).ok(),
              "external Session abort succeeds");
        check(!f->mount.root() && f->mount.report().phase==Ext4MountPhase::failed &&
              f->mount.buffers().state==MetadataOwnerState::failed,
              "root publication rejects an externally aborted original Session");
        check(f->disk.reads+f->disk.writes+f->disk.flushes==io,
              "aborted root publication makes no transport calls");
    }
    {
        auto f=std::make_unique<Fixture>(clean);auto *root=f->mount.root();bool changed{};
        f->disk.hook=[&](char op){if(op=='r'&&f->disk.flushes==9&&!changed){changed=true;
            for(auto &entry:f->mount.owner_.cache_.entries_)if(entry.report.block==f->mount.owner_.blocks_[1])entry.canonical[20]^=std::byte{1};}};
        check(!f->mount.update_metadata(update)&&changed&&!root->query_metadata(),"renewal rejects exact cache image disagreement");
    }
    std::puts("PASS mounted private invariants cases=7 capacity_control=nonfatal epoch_binding=checked exact_cache=checked aborted_root=denied");
}
