// Hosted directory composition and transaction/replay models, never user execution.
#define main regular_file_tests_main
#include "ext4_journalled_mode_tests.cpp"
#undef main

u32 child_number(const Bytes &bytes)
{
    auto m = std::make_unique<Mounted>(bytes);
    auto child = m->mount.root()->lookup_child("child"); CHECK(child);
    auto info = child.value()->query_metadata(); CHECK(info && info.value().type == NodeType::directory);
    return static_cast<u32>(info.value().attributes.inode);
}
void directory_expected(Mounted &m, const Bytes &before, u32 number, u16 permissions)
{
    Ext4InodeMetadataUpdate change{number,1000,1001,1700000077,123456789,permissions};
    CHECK(m.disk.live == mounted_completion(mounted_publication(before,change),number));
    CHECK(m.disk.live == m.disk.durable);
    const auto at = inode_offset(before,number);
    CHECK((r16(m.disk.live,at) & 0170000) == 0040000);
    for (usize byte = 0; byte < 256; ++byte) {
        const bool mutable_byte = byte < 2 || (byte >= 12 && byte < 16) ||
            (byte >= 36 && byte < 40) || (byte >= 124 && byte < 126) ||
            (byte >= 130 && byte < 136) || (byte >= 152 && byte < 156);
        if (!mutable_byte) CHECK(m.disk.live[at+byte] == before[at+byte]);
    }
    CHECK(m.mount.report().phase == Ext4MountPhase::readable && !m.mount.buffers().read_pins);
}
void directory_operations(const Bytes &clean, u32 number)
{
    clock_model::set(sample()); auto m = std::make_unique<Mounted>(clean);
    auto s = std::make_unique<Services>(*m->mount.root()); auto &p=s->service;
    auto node=m->mount.root()->lookup_child("child"); CHECK(node && node.value()->retain());
    auto fd=p.open("/journal/child",posix::o_RDONLY|posix::o_DIRECTORY); CHECK(fd);
    auto dup=p.duplicate(fd.value()); CHECK(dup);
    auto leaf=p.open("/alias/child/leaf",posix::o_RDONLY); CHECK(leaf);
    auto original=p.fstat(fd.value()); CHECK(original && original.value().mode==0040755);
    auto before=m->disk.live;
    CHECK(p.change_mode_at(posix::at_FDCWD,"/alias/child",02750,0).ok());
    directory_expected(*m,before,number,02750);
    auto changed=p.fstat(dup.value()); CHECK(changed && changed.value().mode==0042750);
    CHECK(changed.value().attributes.inode==number && changed.value().link_count==original.value().link_count);
    CHECK(changed.value().size==original.value().size && changed.value().blocks==original.value().blocks);
    CHECK(changed.value().attributes.access.seconds==original.value().attributes.access.seconds);
    CHECK(changed.value().attributes.modification.seconds==original.value().attributes.modification.seconds);
    std::array<std::byte,1024> entries{}; auto listed=p.getdents64(fd.value(),entries); CHECK(listed && listed.value()>0);
    CHECK(node.value()->list());
    CHECK(p.chdir("/journal/child"));
    CHECK(p.stat("leaf") && p.statat(fd.value(),"leaf",0));
    before=m->disk.live; CHECK(p.change_mode_fd(dup.value(),0000).ok()); directory_expected(*m,before,number,0000);
    CHECK(!p.stat("/alias/child/leaf") && !p.stat("leaf") && !p.statat(fd.value(),"leaf",0));
    CHECK(!p.fchdir(fd.value()));
    CHECK(!p.open("/journal/child",posix::o_RDONLY|posix::o_DIRECTORY));
    CHECK(!p.change_mode_at(fd.value(),".",0755,0).ok());
    CHECK(p.seek(fd.value(),0,posix::SeekWhence::set));
    auto retained=p.getdents64(dup.value(),entries);CHECK(retained && retained.value()>0);
    CHECK(p.seek(fd.value(),0,posix::SeekWhence::set));
    usize linux_entries{};
    auto linux_listing=p.enumerate_directory<posix::DirectoryPolicy::linux>(fd.value(),[&](const DirectoryEntry &,u64)->Result<usize>{++linux_entries;return usize{1};});
    CHECK(linux_listing && linux_entries==3);

    CHECK(p.fstat(leaf.value()) && p.fstat(fd.value()).value().mode==0040000);
    std::array<std::byte,64> payload{}; auto read=p.read(leaf.value(),payload); constexpr std::string_view expected_payload="journalled stored metadata fixture";
    CHECK(read && read.value()==expected_payload.size());
    for(usize i=0;i<expected_payload.size();++i)CHECK(payload[i]==std::byte(expected_payload[i]));
    before=m->disk.live; CHECK(p.change_mode_at(dup.value(),"",06755,posix::at_EMPTY_PATH).ok());
    directory_expected(*m,before,number,06755); CHECK(p.stat("leaf") && p.fchdir(fd.value()));
    auto fork=p.fork(); CHECK(fork); CHECK(p.activate(fork.value()));
    CHECK(p.fstat(fd.value()).value().mode==0046755); CHECK(p.chdir("/"));
    CHECK(p.close(fd.value()) && p.close(dup.value()) && p.close(leaf.value())); CHECK(p.activate(s->pid));
    CHECK(p.set_credentials(owner(72))); before=m->disk.live;
    CHECK(p.change_mode_fd(fd.value(),06755).ok()); directory_expected(*m,before,number,04755);
    CHECK(p.set_credentials(user::root_credentials())); before=m->disk.live;
    CHECK(p.change_mode_fd(fd.value(),01755).ok()); directory_expected(*m,before,number,01755);
    CHECK(p.set_credentials(owner()));
    before=m->disk.live; const auto writes=m->disk.writes,flushes=m->disk.flushes;
    clock_model::fail(); const auto clock_calls=clock_model::calls();
    CHECK(p.set_credentials({.uid=1000,.gid=1001,.euid=71,.egid=1001}));
    CHECK(p.change_mode_fd(fd.value(),0700).status().code()==StatusCode::permission);
    CHECK(clock_model::calls()==clock_calls); unchanged(*m,before,writes,flushes);
    CHECK(p.set_credentials(owner()));
    CHECK(p.change_mode_at(posix::at_FDCWD,"/journal",0700,0).kind()==Change::Kind::unavailable);
    CHECK(p.change_mode_at(posix::at_FDCWD,"/journal/child-link",0700,posix::at_SYMLINK_NOFOLLOW).kind()==Change::Kind::unavailable);
    CHECK(p.change_mode_fd(fd.value(),0700).status().code()==StatusCode::io_error);
    unchanged(*m,before,writes,flushes);
    for (i64 seconds : {-1LL,2147483648LL}) {
        clock_model::set(sample(seconds)); CHECK(p.change_mode_fd(fd.value(),0700).status().code()==StatusCode::overflow);
        unchanged(*m,before,writes,flushes);
    }
    clock_model::set(sample()); before=m->disk.live;
    CHECK(p.change_mode_at(posix::at_FDCWD,"/alias/child-link",0755,0).ok()); directory_expected(*m,before,number,0755);
    CHECK(node.value()->release()); CHECK(p.chdir("/"));
}
void directory_replay(const Bytes &clean,u32 number)
{
    Ext4InodeMetadataUpdate change{number,1000,1001,1700000077,123456789,02750};
    const auto published=mounted_publication(clean,change), complete=mounted_completion(published,number);
    {
        owned_fixture::Disk disk(clean); auto writer=std::make_unique<Ext4MetadataTransaction>();
        CHECK(writer->commit(disk,{0,clean.size()/512},change)); CHECK(disk.durable==published);
        auto completion=std::make_unique<Ext4RecoveryCompletion<>>(); CHECK(completion->run(disk,{0,clean.size()/512}));
        CHECK(disk.durable==complete); disk.crash();
        auto again=std::make_unique<Ext4RecoveryCompletion<>>(); CHECK(again->run(disk,{0,clean.size()/512}));
        CHECK(disk.durable==complete && !disk.writes && disk.flushes==1);
    }
    const usize j=journal(clean),at=j+3072+inode_offset(clean,number)%1024;
    auto rejected=[&](const Bytes &bytes) {
        owned_fixture::Disk disk(bytes); auto recovery=std::make_unique<Ext4RecoveryCompletion<>>();
        CHECK(!recovery->run(disk,{0,bytes.size()/512}));
        CHECK(!disk.writes && !disk.flushes && disk.live==bytes && disk.durable==bytes);
    };
    // Every disallowed directory inode byte must remain immutable even after
    // both ext4 and JBD2 checksums have been repaired independently.
    for (usize byte=0;byte<256;++byte) {
        const bool allowed=byte<2 || (byte>=12&&byte<16) || (byte>=36&&byte<40) ||
            (byte>=124&&byte<126) || (byte>=130&&byte<136) || (byte>=152&&byte<156);
        if (allowed) continue;
        auto bad=published;bad[at+byte]^=1;inode_crc(bad,number,at);linux_payload_crc(bad,true);rejected(bad);
    }
    for (u16 type : {u16(0100000),u16(0120000),u16(0020000)}) {
        auto bad=published;w16(bad,at,type|0755);inode_crc(bad,number,at);linux_payload_crc(bad,true);rejected(bad);
    }
    // Preserve the existing replay policy for independently valid regular
    // metadata updates in this same inode-table block; reject structural bytes.
    const u32 first=number-(number-1)%4;
    for(u32 neighbor=first;neighbor<first+4;++neighbor) {
        if(neighbor==number)continue;
        const auto home=inode_offset(clean,neighbor);
        if((r16(clean,home)&0170000)!=0100000)continue;
        const usize payload_inode=j+3072+home%1024;
        auto valid=published;w16(valid,payload_inode,r16(valid,payload_inode)^1);
        inode_crc(valid,neighbor,payload_inode);linux_payload_crc(valid,true);
        owned_fixture::Disk disk(valid);auto recovery=std::make_unique<Ext4RecoveryCompletion<>>();
        CHECK(recovery->run(disk,{0,valid.size()/512}));CHECK(disk.live==mounted_completion(valid,number));
        auto bad=published;bad[payload_inode+26]^=1;inode_crc(bad,neighbor,payload_inode);linux_payload_crc(bad,true);rejected(bad);
    }
    for (u32 reserved : {2u,8u}) {
        auto bad=mounted_publication(clean,{reserved,0,0,1700000077,123456789,0700});rejected(bad);
    }
    {
        clock_model::reset();
        auto maintenance=std::make_unique<Mounted>(clean,Ext4MountedPolicy::maintenance_only);
        CHECK(maintenance->mount.update_metadata(change).code()==StatusCode::unsupported);
        unchanged(*maintenance,clean,0,0);CHECK(!clock_model::calls());
        CHECK(maintenance->mount.report().phase==Ext4MountPhase::readable && maintenance->mount.report().epoch==1);
        CHECK(!maintenance->mount.buffers().transaction_active && !maintenance->mount.buffers().read_pins);
        CHECK(maintenance->mount.update_metadata(update));
        CHECK(maintenance->mount.report().epoch==2 && maintenance->mount.report().phase==Ext4MountPhase::readable);
        CHECK(maintenance->disk.live==mounted_completion(mounted_publication(clean,update),update.inode));
        CHECK(!clock_model::calls());
    }
    for (bool uid : {false,true}) {
        auto bad_update=change;if(uid)++bad_update.uid;else ++bad_update.gid;
        owned_fixture::Disk disk(clean);auto writer=std::make_unique<Ext4MetadataTransaction>();
        CHECK(!writer->commit(disk,{0,clean.size()/512},bad_update)); CHECK(!disk.writes&&!disk.flushes&&disk.live==clean);
    }
    for(bool allocation:{false,true}) {
        auto broken=clean;const auto inode=inode_offset(broken,number);
        if(allocation) {
            const u32 ipg=r32(broken,1064),group=(number-1)/ipg,local=(number-1)%ipg;
            const usize gd=descriptor(broken,group),bitmap=usize(r32(broken,gd+4))*1024;
            broken[bitmap+local/8]&=u8(~(1U<<(local%8)));
            w16(broken,gd+26,u16(crc(seed(broken),broken,bitmap,ipg/8)));group_crc(broken,group,gd);
        } else {w32(broken,inode+36,0xffffffff);w32(broken,inode+152,0xffffffff);inode_crc(broken,number,inode);}
        owned_fixture::Disk disk(broken);auto writer=std::make_unique<Ext4MetadataTransaction>();
        CHECK(!writer->commit(disk,{0,broken.size()/512},change));CHECK(!disk.writes&&!disk.flushes&&disk.live==broken);
    }
    // Directory contents are validated before even an initial durability flush.
    auto broken=clean;const auto data=usize(r32(clean,inode_offset(clean,number)+60))*1024;
    w32(broken,data,2);directory_crc(broken,number,data);
    rejected(mounted_publication(broken,change));
    auto namespace_write=published;
    std::copy_n(clean.begin()+data,1024,namespace_write.begin()+j+3072);
    big32(namespace_write,j+1024+44,u32(data/1024));linux_payload_crc(namespace_write,true);rejected(namespace_write);
    // All successful whole-operation write/flush prefixes from public chmod
    // independently recover to the exact old/new complete volume.
    clock_model::set(sample());auto mounted=std::make_unique<Mounted>(clean);
    auto node=mounted->mount.root()->lookup_child("child");CHECK(node);
    CHECK(node.value()->change_mode(change.permissions,owner()).ok());
    Bytes cut=clean;usize ordinal=0,write_index=0;
    for(const auto &event:mounted->disk.trace) {
        if(event.operation=='r')continue;
        if(event.operation=='w') {
            const auto &write=mounted->disk.submitted[write_index++];
            std::copy(write.bytes.begin(),write.bytes.end(),cut.begin()+write.sector*512);
        }
        ++ordinal;auto expected=cut;
        if(ordinal>=11) {
            std::copy_n(complete.begin()+(inode_offset(clean,number)/1024)*1024,1024,expected.begin()+(inode_offset(clean,number)/1024)*1024);
            std::copy_n(complete.begin()+j,1024,expected.begin()+j);
        }
        std::copy_n(clean.begin()+1024,1024,expected.begin()+1024);
        owned_fixture::Disk disk(cut);auto recovery=std::make_unique<Ext4RecoveryCompletion<>>();
        CHECK(recovery->run(disk,{0,clean.size()/512}));CHECK(disk.live==expected&&disk.durable==expected);
    }
    CHECK(ordinal==20 && write_index==11);
}
void directory_failures(const Bytes &clean)
{
    clock_model::set(sample());usize reads{};
    {
        auto m=std::make_unique<Mounted>(clean);auto node=m->mount.root()->lookup_child("child");CHECK(node);
        const auto start=m->disk.reads;CHECK(node.value()->change_mode(0750,owner()).ok());reads=m->disk.reads-start;
    }
    for(char kind:{'r','w','f'})for(usize point=1;point<=(kind=='r'?reads:kind=='w'?11:9);++point) {
        auto m=std::make_unique<Mounted>(clean);auto node=m->mount.root()->lookup_child("child");CHECK(node&&node.value()->retain());
        if(kind=='r')m->disk.fail_read=m->disk.reads+point;
        if(kind=='w')m->disk.fail_write=point;
        if(kind=='f')m->disk.fail_flush=point;
        CHECK(!node.value()->change_mode(0750,owner()).ok());CHECK(m->mount.report().phase==Ext4MountPhase::failed);
        const auto calls=m->disk.reads+m->disk.writes+m->disk.flushes;
        CHECK(!node.value()->query_metadata()&&!node.value()->list());
        CHECK(m->disk.reads+m->disk.writes+m->disk.flushes==calls);CHECK(node.value()->release());
    }
}
int main(int argc,char **argv)
{
    CHECK(regular_file_tests_main(argc,argv)==0);
    std::ifstream file(argv[1],std::ios::binary);CHECK(file);Bytes clean((std::istreambuf_iterator<char>(file)),{});
    const auto number=child_number(clean);
    directory_operations(clean,number);directory_replay(clean,number);directory_failures(clean);
    std::printf("PASS directory checked mode checks=%zu profiles=10 hosted=1 user_execution=0 hardware=0\n",checks);
    return 0;
}
