// Actual canonical OFD/provider/accept controls with hosted frame aliases.
#include <ok/net/tcp_stream_checked.hpp>
#include <ok/user/native_shared_files.hpp>
#include "tcp_original_ofd.hpp"
#include "tcp_immediate_operation.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
using namespace ok;
using namespace ok::net;
namespace native = user::native;
static usize checks{};
static u64 identity{64000};
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr,"TCP_SHARED_COMPOSITION_FAIL line=%d checks=%zu: %s\n",__LINE__,checks,#value); std::abort(); } } while(false)
constexpr tcp::Endpoint server{{10,0,0,2},8080}, peer{{10,0,0,3},32100};
struct Packet
{
    std::array<std::byte,1500> bytes{}; usize length{};
    wire::Bytes view() const { return {bytes.data(),length}; }
};
Packet packet(u32 seq,u32 ack,u8 flags)
{
    Packet out; auto built=wire::build_tcp(out.bytes,peer.address,server.address,1,64,{peer.port,server.port,seq,ack,flags,4096},{},{});
    CHECK(built);out.length=built.value();return out;
}
struct Iss { Result<u32> operator()(tcp::Endpoint,tcp::Endpoint) { return u32{500}; } };
struct Fixture
{
    alignas(4096) std::array<std::byte,24*4096> backing{};
    memory::FrameAllocator frames; native::NativePagePool pages; native::SharedMemoryFiles store;
    std::unique_ptr<sched::Scheduler> scheduler{std::make_unique<sched::Scheduler>()};
    NativeTcpStreamService tcp{tcp::TableIdentity{identity++}}; TcpStreamOperations operations{*scheduler,tcp};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(4096,32768)};
    u64 pid{}; i32 listener{}, file{}; u16 listener_id{},file_id{}; native::SharedFileHandle handle{}; memory::PhysicalFrame alias{}; Status failure{};
    Fixture()
    {
        const memory::MemoryRegion region{reinterpret_cast<uptr>(backing.data()),backing.size(),memory::RegionType::usable};
        CHECK(frames.initialize({&region,1},4096));CHECK(pages.activate(frames,native::NativePagePool::Source::linker_reserved,2));
        CHECK(store.bind(pages));CHECK(scheduler->set_shared_memory_files(store));CHECK(scheduler->set_native_tcp_service(tcp));
        auto created=scheduler->create_process("tcp-shared-composition",context);CHECK(created);pid=created.value();
        CHECK(scheduler->set_runnable(pid));CHECK(scheduler->activate(pid));
        auto opened=operations.open(pid,true,true);CHECK(opened);listener=opened.value();listener_id=process().fd_table().entry(listener)->description;
        CHECK(operations.bind(pid,listener,server));CHECK(operations.listen(pid,listener,2));
        auto reservation=store.reserve("English shared notes",1200,1300);CHECK(reservation);auto published=reservation.value().publish();CHECK(published);handle=published.value();
        auto description=scheduler->allocate_description({.readable=true,.writable=true,.flags=2,.native_shared_file=handle});CHECK(description);file_id=description.value();file=4;
        CHECK(scheduler->install_descriptor(process().resources(),file,file_id,true));CHECK(store.initialize_size(handle,4096));
        CHECK(store.add_seals(handle,native::SharedMemoryFiles::seal_shrink | native::SharedMemoryFiles::seal_grow));
        auto page=store.page(handle,0);CHECK(page);alias=page.value();CHECK(store.retain_alias(alias));
        const std::array content{std::byte{'E'},std::byte{'n'},std::byte{'g'}};CHECK(store.write(handle,0,content).value()==content.size());
        Iss iss;CHECK(tcp.endpoints().ingest(packet(10,0,wire::tcp_syn).view(),0,iss));
        Packet out;auto response=tcp.endpoints().prepare(out.bytes,0);CHECK(response && response.value());out.length=response.value()->length;
        CHECK(tcp.endpoints().complete(response.value()->ticket,true,0));auto ip=wire::ipv4(out.view());CHECK(ip);auto segment=wire::tcp(ip.value());CHECK(segment);
        CHECK(tcp.endpoints().ingest(packet(11,segment.value().fields.sequence+1,wire::tcp_ack).view(),1,iss));CHECK(tcp.endpoints().pools().unaccepted==1);
    }
    ~Fixture()
    {
        CHECK(failure && operations.status());CHECK(scheduler->drain_all_resources());CHECK(store.release_alias(alias));
        const auto ledger=store.ledger();CHECK(!ledger.files && !ledger.aliases && !ledger.retained_frames && !ledger.reserved_cells && pages.used_count()==0);
        CHECK(tcp.endpoints().retire());
    }
    sched::ProcessControlBlock &process() { return *scheduler->find(pid); }
    sched::OpenFileDescription &listener_description() { return *scheduler->open_description(listener_id); }
    void inject() { listener_description().native_shared_file=handle; }
    void restore() { listener_description().native_shared_file={}; }
    void unchanged()
    {
        CHECK(scheduler->open_description(listener_id)->references==1 && scheduler->open_description(file_id)->references==1);
        CHECK(!process().resources().prepared_descriptor_operations && !process().resources().unix_receive_operations);
        CHECK(tcp.endpoints().pools().unaccepted==1 && !tcp.endpoints().pools().accept_holds && !tcp.prepared_accepts());
        CHECK(store.info(handle).value().description && store.ledger().files==1 && store.ledger().aliases==1);
    }
};
void authority_controls(bool old)
{
    auto owner=std::make_unique<Fixture>();auto &f=*owner;
    CHECK(!f.scheduler->allocate_description({.readable=true,.writable=true,.native_tcp_handle=f.listener_description().native_tcp_handle,.native_shared_file=f.handle}));
    f.inject();CHECK(native::canonical_tcp_fd(*f.scheduler,f.process(),f.listener)==old);f.restore();f.unchanged();
    {
        native::TcpOriginalDescription original{*f.scheduler,f.tcp,f.failure};f.inject();auto acquired=original.acquire(f.process(),f.listener);
        CHECK(bool(acquired)==old);if (old) CHECK(acquired.value()==native::TcpDescriptionKind::retained_tcp);
        f.restore();CHECK(original.release());f.unchanged();
    }
    {
        native::TcpOriginalDescription original{*f.scheduler,f.tcp,f.failure};auto acquired=original.acquire(f.process(),f.listener);CHECK(acquired);
        f.inject();CHECK(original.live()==old && original.matches(f.pid,f.listener)==old);f.restore();CHECK(original.release());f.unchanged();
    }
    {
        f.inject();auto prepared=f.operations.begin_accept(f.pid,f.listener);CHECK(bool(prepared)==old);f.restore();
        if (prepared) { CHECK(prepared.value().retire()); } f.unchanged();
    }
    {
        auto prepared=f.operations.prepare_accept(f.pid,f.listener);CHECK(prepared);f.inject();CHECK(prepared.value().live()==old);
        if (!old) { CHECK(!prepared.value().commit()); } f.restore();CHECK(prepared.value().retire());f.unchanged();
    }
    {
        auto reservation=f.scheduler->prepare_descriptor(f.pid);CHECK(reservation);const auto token=f.process().fd_table().entry(reservation.value().descriptor())->reservation;
        const u16 id=[&] { for (u16 at=3;at<sched::max_open_descriptions;++at) if (auto *description=f.scheduler->open_description(at);description && description->reservation==token) return at;return sched::invalid_description; }();CHECK(id!=sched::invalid_description);
        auto *description=f.scheduler->open_description(id);description->native_shared_file=f.handle;CHECK(reservation.value().validate().ok()==old);
        description->native_shared_file={};CHECK(reservation.value().cancel());f.unchanged();
    }
    {
        f.inject();bool called{};auto result=f.operations.with_stream(f.pid,f.listener,[&](TcpStreamHandle)->Result<usize>{ called=true;return usize{1}; });
        CHECK(bool(result)==old && called==old);f.restore();f.unchanged();
    }
}
void shared_rights_and_accept()
{
    auto owner=std::make_unique<Fixture>();auto &f=*owner;
    auto pair=f.scheduler->create_unix_socketpair(f.process().resources());CHECK(pair);
    const std::array payload{std::byte{'E'}};const std::array rights{f.file,f.listener};std::array<std::byte,1> preview{};
    CHECK(f.scheduler->send_unix_rights(f.process().resources(),pair.value()[0],payload,rights).value()==1);
    CHECK(f.scheduler->close_descriptor(f.process().resources(),f.file));CHECK(f.store.info(f.handle).value().description);
    auto tcp=f.operations.begin_accept(f.pid,f.listener,true,true);CHECK(tcp);
    auto unix=f.scheduler->prepare_unix_receive(f.process().resources(),pair.value()[1],preview,2,false,true);CHECK(unix && preview[0]==std::byte{'E'});
    const auto tcp_fd=tcp.value().descriptor(),shared_fd=unix.value().descriptors()[0],listener_fd=unix.value().descriptors()[1];
    CHECK(tcp_fd==f.file && tcp_fd!=shared_fd && tcp_fd!=listener_fd && shared_fd!=listener_fd);
    const auto tcp_token=f.process().fd_table().entry(tcp_fd)->reservation,unix_token=f.process().fd_table().entry(shared_fd)->reservation;
    CHECK(tcp_token && unix_token && tcp_token!=unix_token);
    CHECK(f.process().resources().prepared_descriptor_operations==1 && f.process().resources().unix_receive_operations==1);
    CHECK(f.scheduler->drain_all_resources().code()==StatusCode::busy && !f.scheduler->fork_process(f.pid));
    CHECK(f.scheduler->replace_image(f.pid,f.context,{}).code()==StatusCode::busy && f.scheduler->exit_process(f.pid,0).code()==StatusCode::busy);
    CHECK(!f.scheduler->install_descriptor(f.process().resources(),tcp_fd,f.file_id));
    CHECK(tcp.value().prepare_child());auto accepted=tcp.value().commit();CHECK(accepted && accepted.value()==tcp_fd);
    CHECK(unix.value().commit().value()==1);CHECK(tcp.value().retire());
    CHECK(!f.process().resources().prepared_descriptor_operations && !f.process().resources().unix_receive_operations);
    const auto *shared=f.process().fd_table().entry(shared_fd),*listener=f.process().fd_table().entry(listener_fd),*connection=f.process().fd_table().entry(tcp_fd);
    CHECK(shared->used && shared->description==f.file_id && shared->close_on_exec && shared->installation==unix_token);
    CHECK(listener->used && listener->description==f.listener_id && listener->close_on_exec);
    CHECK(connection->used && connection->installation==tcp_token && connection->close_on_exec);
    CHECK(!f.scheduler->open_description(connection->description)->native_shared_file && f.scheduler->open_description(connection->description)->native_tcp_handle);
    CHECK(f.scheduler->open_description(f.file_id)->native_shared_file==f.handle && !f.scheduler->open_description(f.file_id)->native_tcp_handle);
    CHECK(f.scheduler->open_description(f.listener_id)->references==2 && !f.scheduler->open_description(f.file_id)->unix_queue_references);
    reinterpret_cast<std::byte *>(f.alias.address)[0]=std::byte{'N'};std::array<std::byte,3> read{};
    CHECK(f.store.read(f.handle,0,read).value()==read.size() && read[0]==std::byte{'N'});
    auto child=f.scheduler->fork_process(f.pid);CHECK(child);CHECK(f.scheduler->open_description(f.file_id)->references==2);
    CHECK(f.scheduler->close_descriptor(f.process().resources(),shared_fd));CHECK(f.store.info(f.handle).value().description);
    CHECK(f.scheduler->exit_process(child.value(),0));CHECK(!f.store.info(f.handle).value().description);
    CHECK(f.store.ledger().files==1 && f.store.ledger().aliases==1);
}
int main(int argc,char **argv)
{
    const bool old=argc==2 && std::strcmp(argv[1],"--expect-old-omission")==0;
    authority_controls(old);if (!old) shared_rights_and_accept();
    std::printf("TCP_SHARED_COMPOSITION_%s checks=%zu provider_guards=7 old_omission=%d hosted_alias=1 actual_PTE_alias=0 syscall_execution=0 guest_execution=0\n",old ? "DEFECT_PASS" : "PASS",checks,old ? 1 : 0);
}
