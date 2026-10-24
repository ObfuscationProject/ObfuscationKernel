// Hosted provider integration; private inspection is target-scoped in xmake.
#include "ext4_mounted_support.hpp"
#include "ext4_mounted_retention_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>
using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;
using Mount = Ext4MountedMetadata<2>;
using Block = Ext4OrphanBlock;
static usize checks{}, callbacks{}, mounted{}, rejects{}, faults{}, drifts{}, ordinary{};
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr,"FAIL namespace C1:%d: %s\n",__LINE__,#value); std::abort(); } } while(false)
static void checked(Status status, const char *label)
{ ++checks; if (!status) { std::fprintf(stderr,"FAIL %s: %.*s\n",label,int(status.message().size()),status.message().data()); std::abort(); } }
static Bytes load(const std::filesystem::path &path)
{
    std::ifstream input(path,std::ios::binary); CHECK(input);
    Bytes result((std::istreambuf_iterator<char>(input)),{}); CHECK(result.size()==32*1024*1024); return result;
}
static Block get(const Bytes &bytes,u32 physical)
{ Block out{}; CHECK(physical<32768); std::copy_n(bytes.begin()+usize(physical)*1024,1024,out.begin()); return out; }
static void put(Bytes &bytes,u32 physical,const Block &block)
{ CHECK(physical<32768); std::copy(block.begin(),block.end(),bytes.begin()+usize(physical)*1024); }
struct Case
{
    Disk disk;
    Mount::Registry registry;
    Mount::Registry::Registration key{};
    std::unique_ptr<Ext4MountedStartupWorkspace> startup=std::make_unique<Ext4MountedStartupWorkspace>();
    std::unique_ptr<Ext4MountedReclaimWorkspace> live=std::make_unique<Ext4MountedReclaimWorkspace>();
    std::unique_ptr<Mount> mount;
    std::function<void(char)> change;
    const void *owner_identity{}, *cache_identity{}, *session_identity{};
    usize admission_reads{}, admission_writes{}, admission_flushes{};
    bool admission_seen{};
    explicit Case(const Bytes &image):disk(image),mount(std::make_unique<Mount>(
        Ext4MountedPolicy::ordered_data_dirty_noatime,live.get(),startup.get()))
    {
        disk.origin=11;
        const auto registration=registry.register_device(disk,49301,1024); CHECK(registration); key=registration.value();
        owner_identity=&mount->owner_; cache_identity=&mount->owner_.cache_; session_identity=&mount->owner_.cache_.session_;
        disk.hook=[&](char operation){
            const auto state=mount->report(); const auto report=mount->startup_report();
            CHECK(state.phase==Ext4MountPhase::opening && !state.epoch && !state.published && !mount->root());
            CHECK(!mount->lifetime_snapshot(2));
            CHECK(&mount->owner_==owner_identity && &mount->owner_.cache_==cache_identity && &mount->owner_.cache_.session_==session_identity);
            CHECK(!mount->owner_.checkpoint() && !mount->owner_.commit() && !mount->owner_.discard() && !mount->owner_.close());
            CHECK(!mount->owner_.prepare(update));
            CHECK(mount->close().code()==StatusCode::busy && mount->sync().code()==StatusCode::busy && mount->freeze_clean().code()==StatusCode::busy);
            const auto io=registry.inspect(key); CHECK(io && io.value().in_flight && io.value().identity==49301 && io.value().session_serial==1);
            Mount::Registry::Session foreign;
            CHECK(registry.acquire(key,{11,65536},block::SessionAccess::readonly,foreign).code()==StatusCode::busy);
            if (report.phase==Ext4StartupPhase::complete && !admission_seen)
            {
                admission_seen=true; admission_reads=disk.reads; admission_writes=disk.writes; admission_flushes=disk.flushes;
                CHECK(report.observed.reads==disk.reads && report.observed.writes==disk.writes && report.observed.flushes==disk.flushes);
            }
            ++callbacks; if(change)change(operation);
        };
    }
    Status run(){return mount->mount(registry,key,{11,65536});}
    void failed(bool zero_io)
    {
        disk.hook={};
        CHECK(mount->report().phase==Ext4MountPhase::failed && !mount->report().epoch && !mount->root());
        CHECK(!mount->lifetime_snapshot(2));
        CHECK(mount->startup_report().strict_ordinary_admission==admission_seen);
        const auto io=registry.inspect(key); CHECK(io && io.value().state==block::DeviceState::quarantined && !io.value().in_flight);
        CHECK(io.value().identity==49301 && io.value().session_serial==1);
        CHECK(io.value().reads==disk.reads && io.value().writes==disk.writes && io.value().flushes==disk.flushes);
        CHECK(io.value().completed_reads<=disk.reads && io.value().completed_writes<=disk.writes && io.value().completed_flushes<=disk.flushes);
        CHECK(mount->startup_report().observed.reads==io.value().reads);
        if(zero_io)CHECK(!disk.writes && !disk.flushes && disk.pending.empty());
        const auto reads=disk.reads,writes=disk.writes,flushes=disk.flushes;
        CHECK(!run() && !mount->sync() && !mount->close() && !mount->freeze_clean());
        CHECK(disk.reads==reads && disk.writes==writes && disk.flushes==flushes);
    }
};
static void trace(const Case &c,const std::filesystem::path &path)
{
    std::ofstream stream(path); CHECK(stream); stream<<"operation\tsector\tbytes\n";
    for(const auto &e:c.disk.trace)
    { if(e.operation!='f')CHECK(e.sector>=11 && e.sector+e.size/512<=65547); stream<<e.operation<<'\t'<<e.sector<<'\t'<<e.size<<'\n'; }
    CHECK(stream);
}
static std::unique_ptr<Case> success(const Bytes &input,const Bytes &golden,const std::filesystem::path &output,const char *name)
{
    auto c=std::make_unique<Case>(input); testing::ext4_clock::reset(); checked(c->run(),name); c->disk.hook={};
    const auto report=c->mount->startup_report(); const auto state=c->mount->report();
    const auto io=c->registry.inspect(c->key).value();
    CHECK(report.phase==Ext4StartupPhase::complete && report.initial_family==Ext4StartupFamily::namespace_active);
    CHECK(report.namespace_result_validated && report.prospective_members==1 && report.namespace_checkpoints==1);
    CHECK(report.chain_members==1 && report.reclaimed==1 && report.released_blocks==1 && report.final_head==0);
    CHECK(report.initial_sequence==20 && report.final_sequence==25 && report.strict_ordinary_admission && report.root_unpublished);
    CHECK(!report.original.reads && !report.original.writes && !report.original.flushes && !testing::ext4_clock::calls());
    CHECK(report.original.identity==49301 && report.original.session_serial==1 &&
          report.original.identity==report.observed.identity && report.original.session_serial==report.observed.session_serial &&
          report.original.registration_generation==report.observed.registration_generation);
    CHECK(report.observed.reads==(c->admission_seen?c->admission_reads:c->disk.reads));
    CHECK(report.observed.writes==c->disk.writes && report.observed.flushes==c->disk.flushes);
    CHECK(io.reads==c->disk.reads && io.writes==c->disk.writes && io.flushes==c->disk.flushes &&
          io.completed_reads==io.reads && io.completed_writes==io.writes && io.completed_flushes==io.flushes && !io.pending_writes && !io.in_flight);
    CHECK(state.phase==Ext4MountPhase::readable && state.epoch==1 && !state.published);
    CHECK(c->startup->namespace_result_.chain[0].number==14);
    CHECK(c->disk.live==golden && c->disk.durable==golden && c->disk.pending.empty());
    save(output/(std::string(name)+"-startup.img"),c->disk.durable); trace(*c,output/(std::string(name)+"-startup.tsv"));
    std::printf("CASE namespace_c1 name=%s reads=%zu writes=%zu flushes=%zu initial_sequence=20 final_sequence=25 reclaimed=1 namespace_checkpoints=1\n",name,c->disk.reads,c->disk.writes,c->disk.flushes);
    ++mounted; return c;
}
static void ordinary_flow(Case &c,const std::filesystem::path &output)
{
    auto vfs=std::make_unique<VirtualFileSystem>();
    auto publication=publish_owned_root(*c.mount,*vfs); CHECK(publication);
    auto file=vfs->resolve({},"/basic-note"); CHECK(file);
    const auto before=file.value().node()->read(0,16); CHECK(before && before.value().size==16);
    testing::ext4_clock::set({{1800000666,123456789},{200,1},1,time::RealtimeOrigin::administrator_software});
    for(usize index=0;index<2;++index)
    { checked(file.value().node()->write(0,std::span{before.value().data}.first(16)),"ordinary same-byte write"); CHECK(c.mount->report().epoch==index+2); }
    CHECK(c.registry.inspect(c.key).value().session_serial==1);
    checked(c.mount->sync(),"ordinary sync");
    checked(file.value().release(),"ordinary file release");
    checked(detach_and_drain(*c.mount,*vfs,publication.value()),"ordinary detach and drain");
    checked(c.mount->freeze_clean(),"ordinary clean freeze");
    auto reopened=std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    checked(reopened->mount(c.registry,c.key,{11,65536}),"ordinary reopen");
    auto reopened_vfs=std::make_unique<VirtualFileSystem>();
    auto reopened_publication=publish_owned_root(*reopened,*reopened_vfs); CHECK(reopened_publication);
    auto again=reopened_vfs->resolve({},"/basic-note"); CHECK(again);
    const auto after=again.value().node()->read(0,16); CHECK(after && after.value().size==16 && after.value().data==before.value().data);
    CHECK(c.registry.inspect(c.key).value().session_serial>1);
    checked(again.value().release(),"reopened file release");
    checked(detach_and_drain(*reopened,*reopened_vfs,reopened_publication.value()),"reopened detach and drain");
    checked(reopened->freeze_clean(),"reopened freeze"); save(output/"ordinary-final.img",c.disk.durable); ++ordinary;
}
static Bytes unsupported_history(const Bytes &input,bool multigroup)
{
    Disk disk(input); auto binding=std::make_unique<Ext4NamespaceRecoveryBinding>();
    checked(binding->prepare(disk,{0,65536}),"negative strong binding"); auto bytes=input;
    for(usize i=0;i<binding->count();++i)put(bytes,binding->target(i),*binding->before(i));
    auto before=binding->before_; Ext4NamespaceWitness witness{};
    checked(Ext4NamespaceWitnessCodec::decode(binding->descriptor_,std::span<const Block>{binding->after_}.first(binding->count()),
        std::span<Block>{before}.first(binding->count()),witness),"negative witness");
    auto update=witness.update; const auto at=inode_offset(bytes,update.destination);
    if(!multigroup){w32(bytes,at+100,~u32{0}); inode_crc(bytes,update.destination,at); update.destination_generation=~u32{0};}
    else
    {
        const u32 new_block=16384,local=(new_block-1)%8192; const auto d=descriptor(bytes,1),bitmap=usize(r32(bytes,d))*1024;
        CHECK(!(bytes[bitmap+local/8]&(1U<<(local%8))) && !(r16(bytes,d+18)&2));
        bytes[bitmap+local/8]|=u8(1U<<(local%8)); w16(bytes,d+12,r16(bytes,d+12)-1); w32(bytes,1036,r32(bytes,1036)-1);
        w16(bytes,d+24,u16(crc(seed(bytes),bytes,bitmap,1024))); group_crc(bytes,1,d); super_crc(bytes);
        w32(bytes,at+4,2048); w32(bytes,at+28,4); w16(bytes,at+42,2); w32(bytes,at+64,1); w16(bytes,at+68,1); w16(bytes,at+70,0); w32(bytes,at+72,new_block);
        inode_crc(bytes,update.destination,at);
    }
    const auto table=[&](u32 inode){return u32(inode_offset(bytes,inode)/1024);};
    const auto super=get(bytes,1),descriptors=get(bytes,2),parent=get(bytes,table(update.parent)),source=get(bytes,table(update.source)),
        destination=get(bytes,table(update.destination)),directory=get(bytes,binding->target(binding->count()-2));
    std::array<Block,4> inode_bitmaps{},block_bitmaps{};
    for(u32 group=0;group<4;++group){block_bitmaps[group]=get(bytes,r32(bytes,descriptor(bytes,group)));inode_bitmaps[group]=get(bytes,r32(bytes,descriptor(bytes,group)+4));}
    const Ext4NamespaceEmptyOrphanAuthorization orphan{binding->old_binding()->uuid,0,true};
    const Ext4NamespaceSource images{table(update.parent),table(update.source),table(update.destination),binding->target(binding->count()-2),
        super,descriptors,parent,source,destination,directory,inode_bitmaps,block_bitmaps,orphan};
    auto scratch=std::make_unique<Ext4NamespaceJournalScratch>(); auto plan=std::make_unique<Ext4NamespaceJournalPlan>();
    checked(Ext4NamespaceDurableJournalPlanner::plan(*binding->old_binding(),update,images,binding->old_head(),*scratch,*plan),"negative valid namespace plan");
    const u32 first=binding->journal_first(); put(bytes,first,plan->journal_head);put(bytes,first+1,plan->descriptor);
    for(usize i=0;i<plan->count;++i)put(bytes,first+u32(i)+2,plan->payloads[i]);
    put(bytes,first+u32(plan->count)+2,plan->commit);put(bytes,first+u32(plan->count)+3,plan->end);
    Disk checked_disk(bytes);auto strong=std::make_unique<Ext4NamespaceRecoveryBinding>();checked(strong->prepare(checked_disk,{0,65536}),"negative remains strong namespace");
    CHECK(strong->proof()->removed_inode==14); return bytes;
}
static void rejection(const Bytes &input,const char *name,StatusCode expected=StatusCode::ok)
{
    auto c=std::make_unique<Case>(input);const auto status=c->run(); CHECK(!status && (expected==StatusCode::ok || status.code()==expected));
    c->failed(true); CHECK(c->disk.live==input && c->disk.durable==input); ++rejects;
    std::printf("REJECT namespace_c1 case=%s writes=0 flushes=0 reason=%.*s\n",name,int(status.message().size()),status.message().data());
}
static void fault(const Bytes &input,char kind,usize ordinal)
{
    auto c=std::make_unique<Case>(input);
    if(kind=='r')c->disk.fail_read=ordinal;
    if(kind=='w')c->disk.fail_write=ordinal;
    if(kind=='f')c->disk.fail_flush=ordinal;
    CHECK(!c->run());c->failed(false); const auto io=c->registry.inspect(c->key).value();
    if(kind=='r')CHECK(c->disk.reads==ordinal && io.completed_reads+1==io.reads);
    if(kind=='w')CHECK(c->disk.writes==ordinal && io.completed_writes+1==io.writes);
    if(kind=='f')CHECK(c->disk.flushes==ordinal && io.completed_flushes+1==io.flushes);
    ++faults;
}
static void drift(const Bytes &input,usize kind)
{
    auto c=std::make_unique<Case>(input);bool changed{};
    c->change=[&](char operation){
        const auto report=c->mount->startup_report();auto &seal=c->startup->namespace_result_seal_;
        if(changed || !seal.ready)return;
        if(kind<3 && !seal.terminal)return;
        if(kind>=3 && (!report.namespace_checkpoints || operation!='r'))return;
        changed=true;
        if(kind==0 || kind==3)c->startup->namespace_result_.inode_tables[0][0][900]^=1;
        else if(kind==1)seal.ready=false;
        else if(kind==2)c->startup->namespace_history_.empty_head_[900]^=1;
        else {const auto at=inode_offset(c->disk.live,2);w32(c->disk.live,at+16,r32(c->disk.live,at+16)+1);inode_crc(c->disk.live,2,at);c->disk.durable=c->disk.live;}
    };
    CHECK(!c->run() && changed);c->failed(kind<3);++drifts;
}
int main(int argc,char **argv)
{
    CHECK(argc==3);const std::filesystem::path corpus=argv[1],output=argv[2];std::filesystem::create_directory(output);
    const auto golden=load(corpus/"c1-terminal-golden.img"),before=load(corpus/"mask-00-input.img");
    usize total_reads{},total_writes{},total_flushes{};
    for(usize mask=0;mask<32;++mask)
    {
        char name[24];std::snprintf(name,sizeof(name),"mask-%02zx",mask);
        auto c=success(load(corpus/(std::string(name)+"-input.img")),golden,output,name);
        if(!mask){total_reads=c->disk.reads;total_writes=c->disk.writes;total_flushes=c->disk.flushes;ordinary_flow(*c,output);}
    }
    rejection(unsupported_history(before,false),"generation-exhausted",StatusCode::overflow);
    rejection(unsupported_history(before,true),"data-multigroup",StatusCode::unsupported);
    auto bad=before;bad[journal(bad)+1024+264]^=1;rejection(bad,"witness-corrupt");
    bad=before;bad[journal(bad)+7*1024+52]^=1;rejection(bad,"commit-corrupt");
    for(usize ordinal:{usize{1},total_reads/2,total_reads})fault(before,'r',ordinal);
    for(usize ordinal:{usize{1},total_writes/2,total_writes})fault(before,'w',ordinal);
    for(usize ordinal:{usize{1},total_flushes/2,total_flushes})fault(before,'f',ordinal);
    for(usize kind=0;kind<5;++kind)drift(before,kind);
    std::printf("PASS mounted_namespace_c1 checks=%zu mounted=%zu rejects=%zu faults=%zu drifts=%zu ordinary=%zu callbacks=%zu same_owner=1 namespace_checkpoints=32 reclaimed=32\n",checks,mounted,rejects,faults,drifts,ordinary,callbacks);
}
