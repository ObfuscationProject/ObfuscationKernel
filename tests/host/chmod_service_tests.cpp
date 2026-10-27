// Hosted common service and checked provider composition; no user instructions.
#include "ext4_realtime_model.hpp"
#include "ok/abi/linux_chmod_call.hpp"
#include "ok/fs/ext4/layout.hpp"
#include "ok/posix/posix.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace ok;
namespace clock_model = testing::ext4_clock;
using Change = fs::ModeChangeResult;
using Bytes = std::vector<std::byte>;
namespace
{
usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::abort(); } } while (false)
class Disk final : public driver::BlockDevice
{
  public:
    Bytes bytes;
    usize writes{};
    explicit Disk(const Bytes &data) : bytes(data) {}
    driver::BlockGeometry geometry() const override { return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 at, std::span<std::byte> out) override
    {
        CHECK(at <= bytes.size()/512 && out.size() <= bytes.size()-at*512);
        std::memcpy(out.data(), bytes.data()+at*512, out.size()); return {};
    }
    Status write_blocks(u64 at, std::span<const std::byte> in) override
    {
        CHECK(at <= bytes.size()/512 && in.size() <= bytes.size()-at*512);
        ++writes; std::memcpy(bytes.data()+at*512, in.data(), in.size()); return {};
    }
    Status flush() override { return {}; }
};
void set_time(i64 seconds = 1700000077)
{
    clock_model::set({{seconds, 123456789}, {200, 1}, 1, time::RealtimeOrigin::administrator_software});
}
constexpr fs::Ext4MountOptions maintained{true, fs::Ext4TimestampPolicy::maintained};
void error(Change result, StatusCode code)
{
    CHECK(result.kind() == Change::Kind::failed && result.status().code() == code);
}
struct Service
{
    fs::VirtualFileSystem vfs;
    sched::Scheduler scheduler;
    driver::ConsoleDriver console;
    posix::PosixService service;
    u32 pid{};
    explicit Service(fs::Node *root = nullptr)
    {
        if (root) CHECK(vfs.mount("/", *root));
        CHECK(console.start() && service.initialize(vfs, console, scheduler));
        auto p = scheduler.create_user_process("chmod-service", arch::configured_arch_operations().make_user_context({0x400000,0x800000,0}));
        CHECK(p && service.activate(p.value())); pid=p.value();
    }
    ~Service()
    {
        auto &resources=scheduler.find(pid)->resources();
        for (i32 fd=3; fd<static_cast<i32>(posix::max_open_files); ++fd)
            if (resources.files.contains(fd)) CHECK(service.close(fd));
        if (resources.cwd_node) { CHECK(resources.cwd_node->release()); resources.cwd_node=nullptr; }
    }
};
Bytes prepare(const Bytes &original)
{
    Disk disk(original); auto filesystem=std::make_unique<fs::Ext4FileSystem>(); set_time(1700000000);
    CHECK(filesystem->mount(disk, maintained)); auto *root=filesystem->root();
    CHECK(root->create_child("mode-file", {fs::NodeType::regular,0644,1000,2000}));
    auto dir=root->create_child("mode-dir", {fs::NodeType::directory,0777,1000,2000}); CHECK(dir);
    CHECK(dir.value()->create_child("child", {fs::NodeType::regular,0644,1000,2000}));
    CHECK(root->create_child("mode-empty", {fs::NodeType::directory,0777,1000,2000}));
    CHECK(root->create_symlink("mode-link", "mode-file",1000,2000));
    CHECK(root->create_child("mode-locked", {fs::NodeType::directory,0000,1000,2000}));
    CHECK(filesystem->sync()); return disk.bytes;
}
user::Credentials owner(u32 group=2000)
{
    return {.uid=71,.gid=72,.euid=1000,.egid=group};
}
template <abi::linux_user::Profile P>
void adapter(Service &fixture)
{
    using namespace abi::linux_user;
    auto &service=fixture.service;
    auto read=[](u64 address, FixedString<256> &path) -> Status {
        if (address==0x1000) return path.assign("/mode-file");
        if (address==0x2000) return path.assign("");
        return Status::fault();
    };
    auto change=[&](ChmodRequest request,std::string_view path) {
        return request.direct_fd ? service.change_mode_fd(request.fd,request.permissions)
                                 : service.change_mode_at(request.fd,path,request.permissions,request.flags);
    };
    const std::array<u64,4> at{0xffffff9cu,0x1000,02755,0};
    const auto before=clock_model::calls();
    CHECK(ChmodCallAdapter<P>::execute(ChmodCall::fchmodat2,at,read,change)==0);
    CHECK(clock_model::calls()==before+1);
    CHECK((service.stat("/mode-file").value().mode&07777)==02755);
    const auto fd=service.open("/mode-file",posix::o_RDONLY); CHECK(fd);
    const std::array<u64,4> empty{static_cast<u32>(fd.value()),0x2000,06750,0x1000};
    CHECK(ChmodCallAdapter<P>::execute(ChmodCall::fchmodat2,empty,read,change)==0);
    const std::array<u64,2> direct{static_cast<u32>(fd.value()),0640};
    CHECK(ChmodCallAdapter<P>::execute(ChmodCall::fchmod,direct,read,change)==0);
    CHECK(service.close(fd.value()));
    CHECK(ChmodCallAdapter<P>::execute(ChmodCall::fchmod,direct,read,change)==-9);
    const std::array<u64,4> null{0xffffff9cu,0,0600,0x1000};
    const auto samples=clock_model::calls();
    CHECK(ChmodCallAdapter<P>::execute(ChmodCall::fchmodat2,null,read,change)==-14);
    CHECK(clock_model::calls()==samples);
}
void maintained_service(const Bytes &prepared)
{
    Disk disk(prepared); auto filesystem=std::make_unique<fs::Ext4FileSystem>(); set_time();
    CHECK(filesystem->mount(disk,maintained)); auto f=std::make_unique<Service>(filesystem->root()); auto &s=f->service;
    // With no explicit cwd pin, AT_EMPTY_PATH addresses the namespace root.
    auto before=clock_model::calls(); CHECK(s.change_mode_at(posix::at_FDCWD,"",0755,posix::at_EMPTY_PATH).ok());
    CHECK(clock_model::calls()==before+1);
    CHECK(s.set_credentials(owner()));
    const auto old=s.stat("/mode-file"); CHECK(old);
    CHECK(s.change_mode_at(-999,"/mode-file",07777).ok()); // Absolute paths ignore dirfd.
    auto changed=s.stat("/mode-file"); CHECK(changed);
    CHECK((changed.value().mode&07777)==07777 && changed.value().uid==1000 && changed.value().gid==2000);
    CHECK(changed.value().attributes.change.seconds==1700000077);
    CHECK(changed.value().attributes.access.seconds==old.value().attributes.access.seconds);
    CHECK(changed.value().attributes.modification.seconds==old.value().attributes.modification.seconds);
    CHECK(s.set_credentials(owner(72)));
    CHECK(s.change_mode_at(posix::at_FDCWD,"/mode-file",07777).ok());
    CHECK((s.stat("/mode-file").value().mode&07777)==05777);
    CHECK(s.set_credentials({.uid=1000,.gid=2000,.euid=71,.egid=2000}));
    before=clock_model::calls(); auto stable=disk.bytes; const auto writes=disk.writes;
    error(s.change_mode_at(posix::at_FDCWD,"/mode-file",0600),StatusCode::permission);
    CHECK(disk.bytes==stable && disk.writes==writes && clock_model::calls()==before);
    CHECK(s.set_credentials({.uid=71,.gid=72,.euid=0,.egid=72}));
    CHECK(s.change_mode_at(posix::at_FDCWD,"/mode-file",02755).ok());
    CHECK((s.stat("/mode-file").value().mode&07777)==02755);
    CHECK(s.set_credentials(owner()));
    error(s.change_mode_at(-999,"mode-file",0600),StatusCode::bad_descriptor);
    error(s.change_mode_at(posix::at_FDCWD,"",0600),StatusCode::not_found);
    error(s.change_mode_at(-999,"",0600,posix::at_EMPTY_PATH),StatusCode::bad_descriptor);
    error(s.change_mode_at(posix::at_FDCWD,"/mode-file",0600,0x80000000),StatusCode::invalid_argument);
    error(s.change_mode_at(posix::at_FDCWD,"/mode-locked/missing",0600),StatusCode::denied);
    CHECK(s.change_mode_fd(0,0600).kind()==Change::Kind::unavailable);
    auto pipes=s.pipe(); CHECK(pipes);
    CHECK(s.change_mode_fd(pipes.value()[0],0600).kind()==Change::Kind::unavailable);
    CHECK(s.change_mode_at(pipes.value()[1],"",0600,posix::at_EMPTY_PATH).kind()==Change::Kind::unavailable);
    CHECK(s.close(pipes.value()[0]) && s.close(pipes.value()[1]));
    auto file=s.open("/mode-file",posix::o_RDONLY); CHECK(file);
    error(s.change_mode_at(file.value(),"child",0600),StatusCode::not_directory);
    stable=disk.bytes; before=clock_model::calls();
    CHECK(s.change_mode_at(posix::at_FDCWD,"/mode-link",0600,posix::at_SYMLINK_NOFOLLOW).kind()==Change::Kind::unavailable);
    CHECK(disk.bytes==stable && clock_model::calls()==before);
    CHECK(s.change_mode_at(posix::at_FDCWD,"/mode-link",0644).ok());
    CHECK((s.fstat(file.value()).value().mode&07777)==0644);
    using namespace abi::linux_user;
    adapter<Profile::i386>(*f); adapter<Profile::x86_64>(*f); adapter<Profile::aarch64>(*f);
    adapter<Profile::arm32>(*f); adapter<Profile::rv32>(*f); adapter<Profile::rv64>(*f);
    adapter<Profile::loongarch64>(*f); adapter<Profile::mips>(*f); adapter<Profile::mips64>(*f); adapter<Profile::ppc>(*f);
    auto dir=s.open("/mode-dir",posix::o_RDONLY); CHECK(dir); CHECK(s.chdir("/mode-dir"));
    // Namespace mutation requires parent write access; root performs that step.
    CHECK(s.set_identity(0,0)); CHECK(s.rename("/mode-dir","/mode-renamed")); CHECK(s.set_credentials(owner()));
    CHECK(s.change_mode_at(dir.value(),"child",0601).ok());
    CHECK((s.stat("/mode-renamed/child").value().mode&07777)==0601);
    CHECK(s.change_mode_at(posix::at_FDCWD,"",0000,posix::at_EMPTY_PATH).ok());
    CHECK(s.change_mode_at(dir.value(),"",0700,posix::at_EMPTY_PATH).ok());
    CHECK(s.change_mode_at(posix::at_FDCWD,"",0755,posix::at_EMPTY_PATH).ok());
    CHECK(s.chdir("/mode-empty")); auto detached=s.open("/mode-empty",posix::o_RDONLY); CHECK(detached);
    CHECK(s.set_identity(0,0)); CHECK(s.rmdir("/mode-empty")); CHECK(s.set_credentials(owner()));
    CHECK(s.change_mode_at(posix::at_FDCWD,"",0701,posix::at_EMPTY_PATH).ok());
    CHECK((s.fstat(detached.value()).value().mode&07777)==0701);
    CHECK(s.chdir("/")); CHECK(s.close(detached.value()));
    CHECK(s.set_identity(0,0)); CHECK(s.unlink("/mode-file")); CHECK(s.set_credentials(owner()));
    CHECK(s.change_mode_fd(file.value(),0610).ok());
    CHECK((s.fstat(file.value()).value().mode&07777)==0610);
    CHECK(s.close(file.value())); error(s.change_mode_fd(file.value(),0600),StatusCode::bad_descriptor);
    CHECK(s.close(dir.value())); CHECK(filesystem->sync());
}
void refused_service(const Bytes &prepared, bool readonly)
{
    Disk disk(prepared); auto filesystem=std::make_unique<fs::Ext4FileSystem>(); set_time();
    CHECK(filesystem->mount(disk,!readonly)); auto f=std::make_unique<Service>(filesystem->root()); auto &s=f->service;
    CHECK(s.set_identity(71,72)); const auto before=clock_model::calls();
    const auto kind=readonly ? Change::Kind::read_only : Change::Kind::unavailable;
    CHECK(s.change_mode_at(posix::at_FDCWD,"/mode-file",0600).kind()==kind);
    CHECK(s.change_mode_at(posix::at_FDCWD,"/mode-link",0600,posix::at_SYMLINK_NOFOLLOW).kind()==kind);
    CHECK(s.set_credentials(owner())); auto fd=s.open("/mode-file",posix::o_RDONLY); CHECK(fd);
    CHECK(s.change_mode_fd(fd.value(),0600).kind()==kind);
    CHECK(s.change_mode_at(fd.value(),"",0600,posix::at_EMPTY_PATH).kind()==kind);
    CHECK(s.close(fd.value())); CHECK(disk.bytes==prepared && disk.writes==0 && clock_model::calls()==before);
}
void unsupported_inode_service(const Bytes &prepared)
{
    Disk disk(prepared); auto filesystem=std::make_unique<fs::Ext4FileSystem>(); set_time();
    CHECK(filesystem->mount(disk,maintained));
    auto node=filesystem->root()->lookup("mode-file"); CHECK(node);
    auto metadata=node->query_metadata(); CHECK(metadata);
    const auto number=static_cast<usize>(metadata.value().attributes.inode);
    auto read16=[&](usize at) { return std::to_integer<u16>(disk.bytes.at(at)) |
                                  (std::to_integer<u16>(disk.bytes.at(at+1))<<8); };
    auto read32=[&](usize at) { return static_cast<ok::u32>(read16(at)) |
                                  (static_cast<ok::u32>(read16(at+2))<<16); };
    const usize block=usize{1024}<<read32(1048), per=read32(1064);
    const usize table=read32((read32(1044)+1)*block+((number-1)/per)*32+8);
    disk.bytes.at(table*block+((number-1)%per)*read16(1112)+108)=std::byte{1};
    auto f=std::make_unique<Service>(filesystem->root()); CHECK(f->service.set_credentials(owner()));
    const auto bytes=disk.bytes; const auto samples=clock_model::calls(); const auto writes=disk.writes;
    CHECK(f->service.change_mode_at(posix::at_FDCWD,"/mode-file",0600).kind()==Change::Kind::unavailable);
    CHECK(disk.bytes==bytes && disk.writes==writes && clock_model::calls()==samples);
}
class FaultNode final : public fs::Node
{
  public:
    FaultNode *child{}; usize pins{},changes{},legacy{}; bool cleanup{};
    Change result=Change::applied();
    std::string_view name() const override { return "node"; }
    fs::Metadata metadata() const override { return {.type=child?fs::NodeType::directory:fs::NodeType::regular,.mode=0777}; }
    Result<fs::FileBuffer> read(usize,usize) const override { return fs::FileBuffer{}; }
    Status write(usize,std::span<const std::byte>) override { return {}; }
    Status create(std::string_view,fs::NodeType) override { return {}; }
    fs::Node *lookup(std::string_view name) override { return name=="file" ? child : nullptr; }
    Status retain() override { ++pins; return {}; }
    Status release() override { CHECK(pins); --pins; return cleanup?Status::io_error():Status::success(); }
    Status chmod(u32) override { ++legacy; return {}; }
    Status chown(u32,u32) override { return Status::unsupported(); }
    Change change_mode(u32,user::Credentials) override { ++changes; return result; }
};
void boundaries()
{
    posix::PosixService unbound;
    error(unbound.change_mode_fd(0,0600),StatusCode::not_initialized);
    error(unbound.change_mode_at(posix::at_FDCWD,"/",0600),StatusCode::not_initialized);
    FaultNode root,file; root.child=&file; auto f=std::make_unique<Service>(&root);
    file.cleanup=true; error(f->service.change_mode_at(posix::at_FDCWD,"/file",0600),StatusCode::io_error);
    CHECK(file.changes==1 && file.pins==0 && root.pins==0 && file.legacy==0);
    file.result=Change::failure(Status::permission());
    error(f->service.change_mode_at(posix::at_FDCWD,"/file",0600),StatusCode::permission);
    CHECK(file.changes==2 && file.pins==0 && root.pins==0 && file.legacy==0);
    file.result=Change::read_only();
    CHECK(f->service.change_mode_at(posix::at_FDCWD,"/file",0600).kind()==Change::Kind::read_only);
    CHECK(file.pins==0 && root.pins==0 && file.legacy==0);
    auto ram=std::make_unique<Service>(); CHECK(ram->service.mkdir("/internal",0777));
    CHECK(ram->service.chmod("/internal",02777));
    CHECK(ram->service.change_mode_at(posix::at_FDCWD,"/internal",0600).kind()==Change::Kind::unavailable);
    CHECK((ram->service.stat("/internal").value().mode&07777)==02777);
    CHECK(ram->service.chown("/internal",1000,2000)); CHECK(ram->service.set_credentials(owner(72)));
    CHECK(ram->service.chmod("/internal",07777)); CHECK((ram->service.stat("/internal").value().mode&07777)==05777);
    CHECK(ram->service.set_identity(71,72)); CHECK(ram->service.chmod("/internal",0600).code()==StatusCode::permission);
}
}
int main(int argc,char **argv)
{
    CHECK(argc==2); FILE *input=std::fopen(argv[1],"rb"); CHECK(input); CHECK(!std::fseek(input,0,SEEK_END));
    const auto size=std::ftell(input); CHECK(size>0); std::rewind(input); Bytes original(static_cast<usize>(size));
    CHECK(std::fread(original.data(),1,original.size(),input)==original.size()); CHECK(!std::fclose(input));
    boundaries(); const auto prepared=prepare(original); maintained_service(prepared);
    refused_service(prepared,true); refused_service(prepared,false); unsupported_inode_service(prepared);
    std::printf("CHMOD_SERVICE_MODEL checks=%zu profiles=10 guest=0 dispatcher=0\n",checks);
}
