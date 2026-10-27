#include "ext4_mounted_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <ok/posix/posix.hpp>
#include <memory>
#include <atomic>
#include <cstring>
#include <thread>
using namespace owned_fixture;
using Mount = Ext4MountedMetadata<1>;
usize checks{};
void verify(bool value, const char *reason) { ++checks; check(value, reason); }
struct Mounted
{
    Disk disk;
    Mount::Registry registry;
    Mount mount;
    Mount::Registry::Registration key;
    explicit Mounted(const Bytes &bytes, u64 origin = 0) : disk(bytes)
    {
        disk.origin = origin;
        auto registration = registry.register_device(disk, 17); verify(registration.ok(), "register mounted device");
        key = registration.value(); status(mount.mount(registry, key, {origin,bytes.size()/512}), "mount clean checked image");
    }
};
struct Services
{
    sched::Scheduler scheduler;
    fs::VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService service;
    Services()
    {
        verify(console.start().ok() && service.initialize(vfs,console,scheduler).ok(), "mounted service initialize");
        auto p=scheduler.create_user_process("mounted",arch::configured_arch_operations().make_user_context({0x400000,0x800000,0}));
        verify(p.ok() && service.activate(p.value()).ok(), "mounted service process");
    }
};
void quarantine(Mounted &m, Node &root)
{
    const auto total=m.disk.reads+m.disk.writes+m.disk.flushes;
    verify(m.mount.report().phase==Ext4MountPhase::failed, "mounted failure state");
    verify(!root.query_metadata() && !root.lookup_child("seed") && !root.list() && !root.sync(), "failed checked node operations");
    verify(root.create_child("new", {NodeType::directory, 0700, 1, 2}).status().code() == StatusCode::denied,
           "failed initial creation preserves quarantine status");
    verify(!m.mount.update_metadata(update) && !m.mount.close() && !m.registry.detach(m.key), "failed ownership cannot restart");
    Mount::Registry::Session alias;
    verify(!m.registry.acquire(m.key,{m.disk.origin,m.disk.live.size()/512},block::SessionAccess::readonly,alias), "quarantine blocks alias");
    verify(m.disk.reads+m.disk.writes+m.disk.flushes==total, "quarantine makes no transport calls");
}
int main(int argc,char **argv)
{
    if(argc!=4)return 2;
    std::ifstream in(argv[1],std::ios::binary);Bytes clean((std::istreambuf_iterator<char>(in)),{});
    const std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
    auto escaped=clean;big32(escaped,inode_offset(escaped,12)/1024*1024,jbd2::magic);
    const auto first=mounted_completion(mounted_publication(escaped,update),12);
    Ext4InodeMetadataUpdate next{16,5000,6000,1700000001,42,0604};
    const auto second=mounted_completion(mounted_publication(first,next),16);
    usize operation_reads{},renewal_reads{};
    {
        auto m=std::make_unique<Mounted>(escaped,11);
        auto services=std::make_unique<Services>();
        auto *root=m->mount.root();verify(root,"published mounted root");
        status(services->vfs.create("/journal",NodeType::directory),"mount directory");
        status(services->vfs.create("/alias",NodeType::directory),"alias directory");
        status(services->vfs.mount("/journal",*root),"VFS mount");status(services->vfs.mount("/alias",*root),"VFS alias");
        auto fd=services->service.open("/journal/seed",posix::o_RDONLY);verify(fd.ok(),"checked POSIX mounted open");
        auto dup=services->service.duplicate(fd.value());verify(dup.ok(),"retained duplicate");
        auto before=services->service.fstat(fd.value());verify(before.ok(),"checked initial fstat");
        verify(before.value().blocks==8 && before.value().block_size==1024,"exact block accounting");
        verify(!before.value().attributes.has(Attribute::change_time),"mutable provider has no readonly timestamp provenance");
        auto alias=services->service.stat("/alias/seed");verify(alias.ok() && alias.value().attributes.device==before.value().attributes.device,"mounted aliases share device identity");
        auto child=root->lookup_child("seed");verify(child.ok(),"checked seed node");
        auto directory=services->service.open("/journal",posix::o_RDONLY);verify(directory.ok(),"open mounted directory");
        std::array<std::byte,4> output{};
        verify(services->service.read(directory.value(),output).status().code()==StatusCode::is_directory,"POSIX directory read is ordinary error");
        verify(child.value()->lookup_child("name").status().code()==StatusCode::not_directory,"lookup on file is ordinary error");
        verify(child.value()->list().status().code()==StatusCode::not_directory,"list file is ordinary error");
        verify(root->query_metadata().ok() && m->mount.report().phase==Ext4MountPhase::readable,"ordinary errors preserve readable view");
        status(services->service.close(directory.value()),"close mounted directory");
        const auto parent=services->service.getpid();auto fork=services->service.fork();verify(fork.ok(),"fork retains mounted descriptions");
        const auto total=m->disk.writes;
        verify(!child.value()->chmod(0777) && !child.value()->chown(5,6) && !child.value()->truncate(0),"public mutation rejected");
        verify(!root->create("new",NodeType::regular) && m->disk.writes==total,"namespace mutation rejects without writes");
        verify(root->create_child("new", {NodeType::directory, 0700, 1, 2}).status().code()==StatusCode::unsupported &&
               m->disk.writes==total, "initial attributes reject before namespace writes");
        verify(root->list().ok(),"mounted directory listing");
        verify(!m->mount.close(),"published provider cannot close");
        bool reentrant{};m->disk.hook=[&](char){ reentrant=true;verify(!root->query_metadata() && !m->mount.update_metadata(update),"transport reentrancy blocked");
            verify(root->create_child("new", {NodeType::directory, 0700, 1, 2}).status().code()==StatusCode::busy,
                   "in-flight initial creation remains busy"); };
        auto reads=m->disk.reads;status(m->mount.update_metadata(update),"first mounted update");m->disk.hook={};
        verify(reentrant && m->disk.durable==first,"escaped partition update full-image oracle");
        operation_reads=m->disk.reads-reads;
        verify(m->mount.buffers().session_serial==1 && m->mount.buffers().first_sector==11 && m->mount.report().epoch==2,"persistent session and epoch");
        auto after=services->service.fstat(dup.value());verify(after.ok() && after.value().mode==0100640 && after.value().uid==2000,"retained description observes new metadata");
        verify(after.value().attributes.inode==before.value().attributes.inode && after.value().attributes.device==before.value().attributes.device,"metadata update preserves identities");
        status(services->service.activate(fork.value()),"activate mounted child");
        auto inherited=services->service.fstat(fd.value());verify(inherited.ok()&&inherited.value().uid==2000,"fork description observes committed metadata");
        auto live_alias=services->service.stat("/alias/seed");verify(live_alias.ok()&&live_alias.value().uid==2000&&live_alias.value().mode==0100640,"live alias observes committed metadata");
        status(services->service.close(fd.value()),"close inherited fd");status(services->service.close(dup.value()),"close inherited duplicate");
        status(services->service.activate(parent),"restore mounted parent");
        status(m->mount.update_metadata(next),"different inode table block");verify(m->disk.durable==second,"different block exact oracle");
        auto third=update;third.permissions=0600;third.uid=7000;
        auto expected=mounted_completion(mounted_publication(second,third),12);
        status(m->mount.update_metadata(third),"replace former participant");verify(m->disk.durable==expected,"repeated participant replacement exact oracle");
        auto invalid=update;invalid.ctime_nanoseconds=1000000000;auto writes=m->disk.writes;
        verify(!m->mount.update_metadata(invalid) && m->mount.report().phase==Ext4MountPhase::readable && writes==m->disk.writes,"local rejection preserves readable mount");
        status(m->mount.update_metadata(next),"successful retry after local rejection");
        expected=mounted_completion(mounted_publication(expected,next),16);verify(m->disk.durable==expected,"retry full-image oracle");
        status(services->service.close(fd.value()),"close retained fd");status(services->service.close(dup.value()),"close duplicate");
        save(out/"completed-escaped.img",m->disk.durable);save(out/"published-escaped.img",mounted_publication(second,third));
    }
    {
        auto m=std::make_unique<Mounted>(escaped,11);auto *root=m->mount.root();
        status(m->mount.update_metadata(update),"measure first successful operation");
        auto reads=m->disk.reads;status(m->mount.update_metadata(next),"measure repeated reads");renewal_reads=m->disk.reads-reads;
        verify(m->disk.durable==second,"second operation measured oracle");
        verify(root->query_metadata().ok(),"renewed metadata read");
    }
    for(char kind:{'w','f'}) for(usize point=1;point<=(kind=='w'?11:9);++point)
    {
        auto m=std::make_unique<Mounted>(escaped,11);auto *root=m->mount.root();
        status(m->mount.update_metadata(update),"first update before write/flush failure");
        if(kind=='w')m->disk.fail_write=m->disk.writes+point;else m->disk.fail_flush=m->disk.flushes+point;
        verify(!m->mount.update_metadata(next),"injected second-operation write/flush failure");quarantine(*m,*root);
    }
    for(usize point=1;point<=renewal_reads;++point)
    {
        auto m=std::make_unique<Mounted>(escaped,11);auto *root=m->mount.root();
        status(m->mount.update_metadata(update),"first update before read failure");m->disk.fail_read=m->disk.reads+point;
        verify(!m->mount.update_metadata(next),"injected second-operation read failure");quarantine(*m,*root);
    }
    {
        auto m=std::make_unique<Mounted>(clean);status(m->mount.update_metadata(update),"ordinary first update");
        const auto published=mounted_publication(m->disk.durable,next);status(m->mount.update_metadata(next),"ordinary repeated update");
        verify(m->disk.durable==mounted_completion(published,16),"ordinary repeated full-image oracle");
        save(out/"completed.img",m->disk.durable);save(out/"published.img",published);
    }
    {
        auto measured=std::make_unique<Mounted>(escaped);const auto admission_reads=measured->disk.reads;
        for(usize point=1;point<=admission_reads;++point)
        {
            Disk disk(escaped);Mount::Registry registry;auto mount=std::make_unique<Mount>();
            const auto key=registry.register_device(disk,17);verify(key.ok(),"admission fault registration");disk.fail_read=point;
            verify(!mount->mount(registry,key.value(),{0,escaped.size()/512})&&!mount->root()&&!disk.writes,"admission read fault refuses publication");
            verify(mount->report().phase==Ext4MountPhase::failed&&!registry.detach(key.value()),"admission read fault quarantines ownership");
        }
    }
    for(bool unsupported_flush:{false,true})
    {
        auto m=std::make_unique<Mounted>(escaped);auto *root=m->mount.root();verify(root->query_metadata().ok(),"cache metadata before sync failure");
        m->disk.fail_flush=m->disk.flushes+1;
        if(unsupported_flush)m->disk.flush_error=Status::unsupported("injected unsupported transport flush");
        verify(!root->sync(),"sync transport failure");quarantine(*m,*root);
    }
    for(usize profile=0;profile<4;++profile)
    {
        auto image=profile==0?mounted_publication(clean,update):clean;
        if(profile==1){w32(image,1116,0);super_crc(image);}
        if(profile==2){w32(image,1120,r32(image,1120)|0x200);super_crc(image);}
        if(profile==3){w32(image,1124,r32(image,1124)|0x10000);super_crc(image);}
        Disk disk(image);Mount::Registry registry;auto mount=std::make_unique<Mount>();auto key=registry.register_device(disk,17);
        verify(key.ok()&&!mount->mount(registry,key.value(),{0,image.size()/512})&&!mount->root()&&!disk.writes&&!disk.flushes,
               "dirty and unsupported admission refuses publication without mutation");
    }
    {
        std::ifstream source(argv[3],std::ios::binary);Bytes image((std::istreambuf_iterator<char>(source)),{});
        auto m=std::make_unique<Mounted>(image);auto services=std::make_unique<Services>();auto *root=m->mount.root();
        status(services->vfs.create("/journal",NodeType::directory),"namespace mount directory");status(services->vfs.mount("/journal",*root),"namespace mount");
        std::array<std::byte,16> target{};auto link=services->service.readlinkat(posix::at_FDCWD,"/journal/seed-link",target);
        verify(link.ok()&&link.value()==4&&std::memcmp(target.data(),"seed",4)==0,"checked mounted symlink target");
        auto follow=services->service.open("/journal/seed-link",posix::o_RDONLY);verify(follow.ok(),"checked mounted symlink traversal");
        std::array<std::byte,4> bytes{};auto read=services->service.read(follow.value(),bytes);
        verify(read.ok()&&read.value()==4&&bytes[0]==std::byte{29},"symlink file data path");status(services->service.close(follow.value()),"close symlink descriptor");
        status(services->service.chdir("/journal/child"),"mounted child cwd");auto cwd=services->service.current_directory();
        verify(cwd.ok()&&cwd.value()=="/journal/child","mounted parent ancestry");
        status(services->service.chdir(".."),"mounted parent traversal");cwd=services->service.current_directory();
        verify(cwd.ok()&&cwd.value()=="/journal","mounted parent cwd");
        status(m->mount.update_metadata(next),"same-block first target");auto adjacent=next;adjacent.inode=13;adjacent.uid=7007;
        auto expected=mounted_completion(mounted_publication(m->disk.durable,adjacent),13);
        status(m->mount.update_metadata(adjacent),"same table block different inode");verify(m->disk.durable==expected,"same-block neighbor exact oracle");
        verify(root->query_metadata().ok(),"namespace remains readable after same-block replacement");
    }
    {
        constexpr usize prefix=11*512,suffix=19*512;
        Bytes guard(prefix+escaped.size()+suffix,0x9a);std::copy(escaped.begin(),escaped.end(),guard.begin()+prefix);
        Disk disk(guard);Mount::Registry registry;auto mount=std::make_unique<Mount>();const auto key=registry.register_device(disk,17);
        verify(key.ok(),"physical guard registration");status(mount->mount(registry,key.value(),{11,escaped.size()/512}),"physical guard mount");
        auto *root=mount->root();verify(root&&root->lookup_child("seed").ok(),"physical guard checked view");
        status(mount->update_metadata(update),"physical guarded escaped first update");status(mount->update_metadata(next),"physical guarded different participant update");
        auto expected=guard;std::copy(second.begin(),second.end(),expected.begin()+prefix);
        verify(disk.durable==expected,"repetition preserves actual physical prefix and suffix guards");
    }
    {
        auto m=std::make_unique<Mounted>(escaped);auto *root=m->mount.root();
        auto child=root->lookup_child("seed");verify(child.ok(),"reader seed");
        std::atomic<bool> entered{},release{};Result<FileBuffer> read{Status::fault()};
        m->disk.hook=[&](char op){if(op=='r'){entered=true;while(!release.load())std::this_thread::yield();}};
        std::thread thread([&]{read=child.value()->read(0,4);});while(!entered.load())std::this_thread::yield();
        verify(!m->mount.update_metadata(update) && !root->query_metadata(),"concurrent read admission excludes operation");
        release=true;thread.join();m->disk.hook={};verify(read.ok(),"admitted reader completed");
        status(m->mount.update_metadata(update),"update after reader drained");
    }
    std::printf("PASS mounted metadata hosted checks=%zu first_reads=%zu repeated_reads=%zu faults=all-writes-flushes-and-repeated-reads\n",checks,operation_reads,renewal_reads);
}
