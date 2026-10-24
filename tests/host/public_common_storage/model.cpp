// Normal-access public API model; failure points are armed between operations.
#include <ok/fs/ext4_mounted_metadata.hpp>
#include "../ext4_realtime_model.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
namespace {
using namespace ok;
using namespace ok::fs;
using Bytes = std::vector<u8>;
using Mount = Ext4MountedMetadata<2>;
using Owner = Ext4OwnedTransaction<2, 8>;
constexpr usize image_size = 32 * 1024 * 1024;
usize checks{}, recoveries{}, failures{}, request_failures{}, zeros{}, refills{}, noops{}, reuse_refusals{},
    legacy_overwrites{}, optional_revoke_saves{};
std::filesystem::path outputs;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr,"public common:%d: %s\n",__LINE__,#expression); std::abort(); } } while(false)
void checked(Status status, const char *label) {
    ++checks;
    if (!status) { std::fprintf(stderr,"public common %s: code=%u detail=%.*s\n",label,
        unsigned(status.code()),int(status.message().size()),status.message().data()); std::abort(); }
}
Bytes load(const std::filesystem::path &path) {
    std::ifstream stream(path,std::ios::binary); CHECK(stream);
    Bytes result((std::istreambuf_iterator<char>(stream)),{}); CHECK(result.size()==image_size); return result;
}
void save(const std::filesystem::path &path,const Bytes &bytes) {
    CHECK(!std::filesystem::exists(path)); std::ofstream stream(path,std::ios::binary); CHECK(stream);
    stream.write(reinterpret_cast<const char *>(bytes.data()),std::streamsize(bytes.size())); CHECK(stream);
}
struct Disk final : driver::BlockDevice {
    struct Write { usize offset{}; Bytes bytes; };
    Bytes live,durable;
    std::vector<Write> pending;
    usize reads{},writes{},flushes{},fail_read{},fail_write{},fail_flush{};
    explicit Disk(const Bytes &input):live(input),durable(input) {}
    driver::BlockGeometry geometry() const override { return {image_size/512,512,true}; }
    Status read_blocks(u64 sector,std::span<std::byte> output) override {
        ++reads;
        if(reads==fail_read) return Status::io_error("public model armed READ refusal");
        if(sector>=image_size/512 || output.empty() || output.size()%512 || output.size()>image_size-usize(sector)*512)
            return Status::invalid_argument("public model READ geometry");
        std::copy_n(live.begin()+usize(sector)*512,output.size(),reinterpret_cast<u8 *>(output.data())); return {};
    }
    Status write_blocks(u64 sector,std::span<const std::byte> input) override {
        ++writes;
        if(writes==fail_write) return Status::io_error("public model armed WRITE refusal");
        if(sector>=image_size/512 || sector%2 || input.size()!=1024 || input.size()>image_size-usize(sector)*512)
            return Status::invalid_argument("public model WRITE geometry");
        Write record{usize(sector)*512,Bytes(reinterpret_cast<const u8 *>(input.data()),
            reinterpret_cast<const u8 *>(input.data())+input.size())};
        std::copy(record.bytes.begin(),record.bytes.end(),live.begin()+record.offset);
        pending.push_back(std::move(record)); return {};
    }
    Status flush() override {
        ++flushes;
        if(flushes==fail_flush) return Status::io_error("public model armed FLUSH refusal");
        for(const auto &write:pending) std::copy(write.bytes.begin(),write.bytes.end(),durable.begin()+write.offset);
        pending.clear(); return {};
    }
};
struct Case final {
    Disk disk;
    Mount::Registry registry{};
    Mount::Registry::Registration key{};
    std::unique_ptr<Mount::CommonStorage> storage{new Mount::CommonStorage};
    std::unique_ptr<Mount> mount{new Mount(*storage)};
    explicit Case(const Bytes &input):disk(input) {
        auto registered=registry.register_device(disk,70001); CHECK(registered); key=registered.value();
        CHECK(mount->policy()==Ext4MountedPolicy::ordered_data_dirty_noatime);
    }
    Status open() { return mount->mount(registry,key,{0,image_size/512}); }
    void exact() {
        const auto actual=registry.inspect(key); CHECK(actual);
        const auto buffers=mount->buffers();
        CHECK(actual.value().session_serial==buffers.session_serial &&
            actual.value().registration_generation==buffers.registration_generation &&
            actual.value().reads==disk.reads && actual.value().completed_reads==disk.reads &&
            actual.value().writes==disk.writes && actual.value().completed_writes==disk.writes &&
            actual.value().flushes==disk.flushes && actual.value().completed_flushes==disk.flushes &&
            !actual.value().in_flight && !actual.value().pending_writes);
    }
    void reuse() {
        Disk other(disk.durable); Mount::Registry foreign;
        auto registered=foreign.register_device(other,70002); CHECK(registered);
        auto candidate=std::make_unique<Mount>(*storage);
        CHECK(candidate->mount(foreign,registered.value(),{0,image_size/512}).code()==StatusCode::busy);
        CHECK(!other.reads && !other.writes && !other.flushes && !candidate->root() && !candidate->retain_root());
        CHECK(foreign.inspect(registered.value()).value().session_serial==0);
        CHECK(foreign.detach(registered.value())); ++reuse_refusals;
    }
};
struct Attached final {
    Case &c;
    std::unique_ptr<VirtualFileSystem> vfs{new VirtualFileSystem};
    VfsMountHandle handle{};
    explicit Attached(Case &which):c(which) {
        auto root=c.mount->retain_root(); CHECK(root);
        auto clone=root.value().clone(); CHECK(clone);
        CHECK(c.mount->close().code()==StatusCode::busy);
        CHECK(clone.value().release() && !clone.value().valid());
        auto attached=vfs->mount_owned("/",std::move(root).value()); CHECK(attached); handle=attached.value();
        CHECK(!root.value().valid());
    }
    void detach() { CHECK(vfs->detach(handle)); }
};
void clock_ready() {
    testing::ext4_clock::reset(); testing::ext4_clock::set({{1700000444,123456789},
        {1700000444000000000ULL,1},1,time::RealtimeOrigin::administrator_software});
}
std::array<usize,3> success(const Bytes &input,const Bytes &golden,Ext4StartupFamily family,const char *label) {
    auto c=std::make_unique<Case>(input);
    CHECK(!c->mount->root() && !c->mount->retain_root());
    checked(c->open(),label); c->exact();
    CHECK(c->disk.live==golden && c->disk.durable==golden && c->disk.pending.empty());
    const auto startup=c->mount->startup_report();
    CHECK(startup.phase==Ext4StartupPhase::complete && startup.initial_family==family && startup.status);
    CHECK(startup.original.session_serial==c->mount->buffers().session_serial && !startup.final_head);
    CHECK(c->mount->report().phase==Ext4MountPhase::readable && c->mount->report().epoch==1);
    const auto trace=std::array<usize,3>{c->disk.reads,c->disk.writes,c->disk.flushes};
    save(outputs/(std::string(label)+"-terminal.img"),c->disk.durable);
    {
        Attached attach(*c);
        auto root=attach.vfs->resolve({},"/"); CHECK(root && root.value().node());
        auto metadata=root.value().node()->query_metadata(); CHECK(metadata && metadata.value().type==NodeType::directory);
        auto listing=root.value().node()->list(); CHECK(listing);
        const auto before=std::array<usize,3>{c->disk.reads,c->disk.writes,c->disk.flushes};
        CHECK(c->mount->close().code()==StatusCode::busy);
        CHECK(before==(std::array<usize,3>{c->disk.reads,c->disk.writes,c->disk.flushes}));
        CHECK(root.value().release()); attach.detach();
    }
    c->exact(); c->reuse(); checked(c->mount->close(),"public close");
    CHECK(c->mount->report().phase==Ext4MountPhase::closed &&
        c->registry.inspect(c->key).value().state==block::DeviceState::ready);
    c->mount.reset(); c->reuse(); ++recoveries; return trace;
}
Bytes prepare_legacy(const Bytes &input,const Bytes &golden) {
    Disk disk(input); Owner::Registry registry;
    auto key=registry.register_device(disk,71001); CHECK(key);
    auto owner=std::make_unique<Owner>(true);
    checked(owner->open(registry,key.value(),{0,image_size/512}),"public native legacy open");
    Ext4FirstDataBlock data{}; data.fill('L');
    checked(owner->prepare_first_data({14,1024,1700000300,123456789},0,data),"public native legacy prepare");
    checked(owner->commit(),"public native legacy commit");
    CHECK(disk.live==disk.durable && disk.pending.empty());
    auto active=disk.durable; save(outputs/"native-legacy-active.img",active);
    checked(owner->checkpoint(),"public native legacy checkpoint");
    CHECK(disk.live==golden && disk.durable==golden && disk.pending.empty());
    save(outputs/"native-legacy-checkpoint.img",disk.durable);
    checked(owner->close(),"public native legacy close"); CHECK(registry.detach(key.value())); return active;
}
void failed_open(const Bytes &input,usize read,usize write,usize flush) {
    auto c=std::make_unique<Case>(input);
    c->disk.fail_read=read; c->disk.fail_write=write; c->disk.fail_flush=flush;
    const auto status=c->open(); CHECK(!status && status.code()==StatusCode::io_error);
    CHECK(c->mount->report().phase==Ext4MountPhase::failed && !c->mount->report().epoch &&
        !c->mount->report().published && !c->mount->root() && !c->mount->retain_root());
    const auto native=c->registry.inspect(c->key); CHECK(native);
    CHECK(native.value().state==block::DeviceState::quarantined && native.value().first_error==StatusCode::io_error);
    const auto startup=c->mount->startup_report();
    CHECK(startup.phase==Ext4StartupPhase::failed && startup.status.code()==StatusCode::io_error);
    c->reuse(); c->mount.reset(); c->reuse(); ++failures;
}
void transport_controls(const Bytes &input,const std::array<usize,3> &trace) {
    for(usize read=1;read<=6;++read) failed_open(input,read,0,0);
    if(trace[0]>6) failed_open(input,trace[0],0,0);
    for(usize category=1;category<3;++category) if(trace[category]) {
        failed_open(input,0,category==1?1:0,category==2?1:0);
        if(trace[category]>1) failed_open(input,0,category==1?trace[1]:0,category==2?trace[2]:0);
    }
}
void requests(const Bytes &input,const char *name,bool ordinary_overwrite=false) {
    auto c=std::make_unique<Case>(input); checked(c->open(),"public request mount");
    Attached attach(*c); auto found=attach.vfs->resolve({},std::string("/")+name); CHECK(found);
    auto *node=found.value().node(); CHECK(node && node->filesystem_identity()==c->mount.get());
    CHECK(node->retain() && node->release());
    std::array<std::byte,66> text{}; for(usize at=0;at<text.size();++at) text[at]=std::byte('A'+at%26);
    for(usize cycle=0;cycle<2;++cycle) {
        auto old=node->query_metadata(); CHECK(old);
        clock_ready(); checked(node->truncate(0),"public zero");
        if(old.value().size) ++zeros;
        auto metadata=node->query_metadata(); CHECK(metadata && !metadata.value().size && !metadata.value().blocks);
        const auto writes=c->disk.writes,flushes=c->disk.flushes,epoch=c->mount->report().epoch;
        clock_ready(); checked(node->truncate(0),"public zero noop");
        CHECK(c->disk.writes==writes && c->disk.flushes==flushes && !testing::ext4_clock::calls() &&
            c->mount->report().epoch==epoch); ++noops;
        const auto refused=node->write(1024,text); CHECK(!refused && refused.code()==StatusCode::unsupported);
        CHECK(c->disk.writes==writes && c->disk.flushes==flushes && c->mount->report().epoch==epoch);
        const auto empty_before=std::array<usize,2>{c->disk.writes,c->disk.flushes};
        CHECK(node->write(0,{}));
        CHECK(empty_before==(std::array<usize,2>{c->disk.writes,c->disk.flushes}) &&
            !testing::ext4_clock::calls() && c->mount->report().epoch==epoch);
        clock_ready(); checked(node->write(7,text),"public empty refill"); ++refills; c->exact();
        metadata=node->query_metadata(); CHECK(metadata && metadata.value().size==73 && metadata.value().blocks==2);
        auto read=node->read(0,73); CHECK(read && read.value().size==73);
        for(usize at=0;at<7;++at) CHECK(read.value().data[at]==std::byte{});
        for(usize at=0;at<text.size();++at) CHECK(read.value().data[at+7]==text[at]);
        if(ordinary_overwrite) {
            // Compose the admitted canonical legacy route with the next typed request.
            const std::array<std::byte,3> overwrite{std::byte{'x'},std::byte{'y'},std::byte{'z'}};
            clock_ready(); checked(node->write(10,overwrite),"public ordinary overwrite"); ++legacy_overwrites;
            read=node->read(10,3); CHECK(read && read.value().size==3);
            for(usize at=0;at<3;++at) CHECK(read.value().data[at]==overwrite[at]);
            clock_ready(); checked(node->truncate(50),"public ordinary same-block resize");
            metadata=node->query_metadata(); CHECK(metadata && metadata.value().size==50);
            const auto prior=std::array<usize,2>{c->disk.writes,c->disk.flushes};
            CHECK(node->truncate(2048).code()==StatusCode::unsupported);
            CHECK(prior==(std::array<usize,2>{c->disk.writes,c->disk.flushes}));
        }
        CHECK(c->mount->close().code()==StatusCode::busy); c->exact();
    }
    save(outputs/(std::string(name)+"-requests-terminal.img"),c->disk.durable);
    CHECK(found.value().release()); attach.detach(); checked(c->mount->close(),"public request close");
    c->reuse();
    std::printf("PUBLIC_REQUEST_COMPLETE name=%s\n",name); std::fflush(stdout);
}
void optional_revoke_notes_saves(const Bytes &input) {
    auto c=std::make_unique<Case>(input); checked(c->open(),"optional revoke Notes mount");
    Attached attach(*c); auto found=attach.vfs->resolve({},"/empty"); CHECK(found);
    auto *node=found.value().node(); CHECK(node);
    const auto initial=node->query_metadata(); CHECK(initial);
    const auto inode=u32(initial.value().attributes.inode);
    const auto lifetime=c->mount->lifetime_snapshot(inode); CHECK(lifetime);
    const auto serial=c->mount->buffers().session_serial;
    save(outputs/"revoke-legacy-before.img",c->disk.durable);
    for (usize cycle=0; cycle<2; ++cycle) {
        clock_ready(); checked(node->truncate(0),"optional revoke staged truncate");
        std::string expected;
        for (std::string_view word : {std::string_view(cycle ? "Updated" : "First"),
                                     std::string_view("English"),std::string_view("document.")}) {
            if (!expected.empty()) {
                const std::byte space{' '}; clock_ready();
                checked(node->write(expected.size(),{&space,1}),"optional revoke staged space"); expected+=' ';
            }
            clock_ready(); checked(node->write(expected.size(),{reinterpret_cast<const std::byte *>(word.data()),word.size()}),
                "optional revoke staged word"); expected+=word;
        }
        const std::byte newline{'\n'}; clock_ready();
        checked(node->write(expected.size(),{&newline,1}),"optional revoke staged newline"); expected+='\n';
        checked(node->sync(),"optional revoke staged fsync");
        const auto metadata=node->query_metadata(); CHECK(metadata && metadata.value().size==expected.size() &&
            metadata.value().attributes.inode==inode && metadata.value().blocks==2);
        auto read=node->read(0,expected.size()); CHECK(read && read.value().size==expected.size());
        for(usize at=0;at<expected.size();++at) CHECK(read.value().data[at]==std::byte(expected[at]));
        const auto little=[&](usize at) { const auto &b=c->disk.durable; return u32(b[at])|u32(b[at+1])<<8|
            u32(b[at+2])<<16|u32(b[at+3])<<24; };
        CHECK(inode && inode<=little(1064));
        const usize table=usize(little(2048+8))*1024;
        const usize home=table+usize(inode-1)*256;
        CHECK(home+64<c->disk.durable.size());
        const usize data=usize(little(home+60))*1024; CHECK(data && data+1024<=c->disk.durable.size());
        for(usize at=0;at<1024;++at) CHECK(c->disk.durable[data+at]==(at<expected.size()?u8(expected[at]):u8{0}));
        const auto after=c->mount->lifetime_snapshot(inode); CHECK(after && after.value().key==lifetime.value().key &&
            after.value().holds==lifetime.value().holds && after.value().phase==lifetime.value().phase);
        CHECK(c->mount->buffers().session_serial==serial && c->disk.live==c->disk.durable && c->disk.pending.empty());
        c->exact(); ++optional_revoke_saves;
    }
    save(outputs/"revoke-legacy-after.img",c->disk.durable);
    const auto terminal=c->disk.durable;
    CHECK(found.value().release()); attach.detach(); checked(c->mount->close(),"optional revoke Notes close"); c->reuse();
    auto cold=std::make_unique<Case>(terminal); checked(cold->open(),"optional revoke cold mount");
    CHECK(!cold->disk.writes && cold->disk.live==terminal && cold->disk.durable==terminal);
    Attached cold_attach(*cold);
    auto reopened=cold_attach.vfs->resolve({},"/empty"); CHECK(reopened);
    auto *reopened_node=reopened.value().node(); CHECK(reopened_node);
    constexpr std::string_view final="Updated English document.\n";
    const auto metadata=reopened_node->query_metadata(); CHECK(metadata && metadata.value().size==final.size() &&
        metadata.value().attributes.inode==inode);
    auto read=reopened_node->read(0,final.size()); CHECK(read && read.value().size==final.size());
    for(usize at=0;at<final.size();++at) CHECK(read.value().data[at]==std::byte(final[at]));
    CHECK(reopened.value().release()); cold_attach.detach();
    checked(cold->mount->close(),"optional revoke cold close");
}
void failed_request(const Bytes &input,const char *name,bool refill,char transport) {
    auto c=std::make_unique<Case>(input); checked(c->open(),"public failure request mount");
    Attached attach(*c); auto found=attach.vfs->resolve({},std::string("/")+name); CHECK(found);
    auto *node=found.value().node(); CHECK(node);
    if(transport=='r') c->disk.fail_read=c->disk.reads+1;
    if(transport=='w') c->disk.fail_write=c->disk.writes+1;
    if(transport=='f') c->disk.fail_flush=c->disk.flushes+1;
    std::array<std::byte,66> text{}; text.fill(std::byte{'P'}); clock_ready();
    const auto status=refill?node->write(7,text):node->truncate(0);
    CHECK(!status && status.code()==StatusCode::io_error);
    const auto native=c->registry.inspect(c->key); CHECK(native &&
        native.value().state==block::DeviceState::quarantined && native.value().first_error==StatusCode::io_error);
    CHECK(c->mount->report().phase==Ext4MountPhase::failed && !c->mount->retain_root());
    const auto before=std::array<usize,3>{c->disk.reads,c->disk.writes,c->disk.flushes};
    CHECK(!node->read(0,73) && !node->write(7,text) && !node->truncate(0));
    CHECK(before==(std::array<usize,3>{c->disk.reads,c->disk.writes,c->disk.flushes}));
    static_cast<void>(found.value().release()); attach.detach(); c->reuse(); ++request_failures;
}
void malformed(const Bytes &input,const char *label) {
    auto broken=input;
    // This pre-opening media corruption changes only payload bytes; the native
    // typed record remains recognized and cannot enter a legacy writer.
    const auto little=[&](usize at) { return u32(broken[at])|u32(broken[at+1])<<8|
        u32(broken[at+2])<<16|u32(broken[at+3])<<24; };
    // The fixed saved fixture uses 1 KiB blocks, 256-byte inodes and its
    // internal journal extent starts in inode 8's first physical extent.
    const usize table=usize(little(2048+8))*1024;
    CHECK(table+7*256+64<broken.size());
    const usize head=usize(little(table+7*256+60))*1024;
    CHECK(head && head+2048<broken.size() && broken[head]==0xc0 && broken[head+1]==0x3b &&
        broken[head+2]==0x39 && broken[head+3]==0x98 && broken[head+7]==4);
    broken[head+1024+136]^=1;
    auto c=std::make_unique<Case>(broken); const auto status=c->open(); CHECK(!status);
    CHECK(c->disk.reads==5 && !c->disk.writes && !c->disk.flushes && !c->mount->root() && !c->mount->retain_root());
    save(outputs/(std::string(label)+"-malformed.img"),broken); c->reuse();
}
} // namespace
int main(int argc,char **argv) {
    CHECK(argc==3); const std::filesystem::path inputs=argv[1]; outputs=argv[2];
    const auto ordinary=load(inputs/"ordinary.img"),orphan=load(inputs/"orphan.img"),
        orphan_golden=load(inputs/"orphan-golden.img"),ns=load(inputs/"namespace.img"),
        ns_golden=load(inputs/"namespace-golden.img"),reclaim=load(inputs/"reclaim.img"),
        seed=load(inputs/"legacy-seed.img"),legacy_golden=load(inputs/"legacy-golden.img"),
        zero=load(inputs/"zero-active.img"),zero_golden=load(inputs/"zero-completed.img"),
        allocated=load(inputs/"allocated-active.img"),allocated_golden=load(inputs/"allocated-completed.img");
    const auto legacy=prepare_legacy(seed,legacy_golden);
    struct Row { const Bytes &input,&golden; Ext4StartupFamily family; const char *label; };
    for(const auto &row:std::array<Row,7>{{
        {ordinary,ordinary,Ext4StartupFamily::ordinary,"ordinary"},
        {orphan,orphan_golden,Ext4StartupFamily::empty_orphan,"orphan"},
        {ns,ns_golden,Ext4StartupFamily::namespace_active,"namespace"},
        {reclaim,orphan_golden,Ext4StartupFamily::reclaim_active,"reclaim"},
        {legacy,legacy_golden,Ext4StartupFamily::legacy_active,"legacy"},
        {zero,zero_golden,Ext4StartupFamily::typed_zero_active,"zero"},
        {allocated,allocated_golden,Ext4StartupFamily::typed_allocated_active,"allocated"}}}) {
        const auto trace=success(row.input,row.golden,row.family,row.label); transport_controls(row.input,trace);
        std::printf("PUBLIC_FAMILY_COMPLETE name=%s reads=%zu writes=%zu flushes=%zu\n",row.label,
            trace[0],trace[1],trace[2]); std::fflush(stdout);
    }
    requests(zero,"zero",true); requests(allocated,"empty",true); requests(seed,"empty-note",true);
    optional_revoke_notes_saves(allocated);
    for(char operation:{'r','w','f'}) {
        failed_request(allocated,"empty",false,operation);
        failed_request(allocated,"empty",true,operation);
        failed_request(zero,"zero",true,operation);
    }
    malformed(zero,"zero"); malformed(allocated,"allocated");
    std::printf("{\"passed\":true,\"recoveries\":%zu,\"opening_failures\":%zu,\"request_failures\":%zu,\"zeros\":%zu,\"refills\":%zu,\"noops\":%zu,\"reuse_refusals\":%zu,\"legacy_overwrites\":%zu,\"optional_revoke_saves\":%zu,\"checks\":%zu}\n",
        recoveries,failures,request_failures,zeros,refills,noops,reuse_refusals,legacy_overwrites,optional_revoke_saves,checks);
}
