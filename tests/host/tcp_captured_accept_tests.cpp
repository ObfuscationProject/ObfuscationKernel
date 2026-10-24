// Actual captured origin, scheduler and copy path with synthetic owned traps.
#include "../../src/user/tcp_captured_accept.hpp"
#include "../../src/user/native_shared_capture.hpp"
#include <ok/net/tcp_accept_copied.hpp>
#include <ok/user/native_entry_stacks.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace ok;
namespace hw = arch::execution;
namespace lu = abi::linux_user;
namespace native = user::native;
using namespace ok::net;
static usize checks{}, profiles{}, cases{}, traps{}, range_queries{}, reads{}, writes{};
static u64 table_identity{63000};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "TCP_CAPTURED_ACCEPT_FAIL line=%d checks=%zu: %s\n", __LINE__, checks, #x); std::abort(); } } while(false)
constexpr tcp::Endpoint server{{10, 0, 0, 2}, 8080}, peer{{10, 0, 0, 3}, 32001};
constexpr uptr array_address = hw::user_begin + 64, length_address = hw::user_begin + 256;
constexpr uptr peer_address = hw::user_begin + 512;
struct Packet
{
    std::array<std::byte, 1500> bytes{}; usize length{};
    wire::Bytes view() const { return {bytes.data(), length}; }
};
Packet packet(tcp::Endpoint from, u32 seq, u32 ack, u8 flags)
{
    Packet out; auto built = wire::build_tcp(out.bytes, from.address, server.address, 1, 64,
        {from.port, server.port, seq, ack, flags, 4096}, {}, {});
    CHECK(built); out.length = built.value(); return out;
}
struct Iss { Result<u32> operator()(tcp::Endpoint, tcp::Endpoint) { return u32{500}; } };

namespace model
{
enum class Action : u8 { none, fault, rotate_epoch, change_frame, change_image, redispatch, completed_wait, reinstall, close, reset, retire };
enum class Kind : u8 { range, read, write };
struct Event { Kind kind{}; uptr address{}; usize size{}, references{}; bool writable{}; };
struct Context
{
    sched::Scheduler *scheduler{}; memory::KernelEntryStackPool *pool{}; NativeTcpStreamService *service{};
    TcpStreamOperations *operations{};
    u64 pid{}; u16 description{}; lu::Profile profile{}; hw::AddressSpace space{};
    Action action{}; usize action_at{}, calls{}, readable_limit{4096}, writable_limit{4096};
    bool guards{};
    std::array<std::byte, 4096> memory{};
    std::array<Event, 32> events{};
} context;
hw::Trap trap()
{
    switch (context.profile)
    {
    case lu::Profile::i386: return {hw::TrapKind::syscall, 128};
    case lu::Profile::x86_64: return {hw::TrapKind::syscall, 256};
    case lu::Profile::aarch64: return {hw::TrapKind::syscall, 0x15, 0x54000000};
    case lu::Profile::arm32: return {hw::TrapKind::syscall, 2};
    case lu::Profile::rv64: case lu::Profile::rv32: return {hw::TrapKind::syscall, 8, 8};
    case lu::Profile::loongarch64: return {hw::TrapKind::syscall, 11};
    case lu::Profile::mips: case lu::Profile::mips64: return {hw::TrapKind::syscall, 8, 32};
    default: return {hw::TrapKind::syscall, 8};
    }
}
bool record(Kind kind, uptr address, usize size, bool writable)
{
    auto &c = context; CHECK(c.calls < c.events.size());
    const auto *open = c.scheduler->open_description(c.description);
    c.events[c.calls++] = {kind, address, size, open ? open->references : 0, writable};
    return c.calls == c.action_at && c.action == Action::fault;
}
void callback()
{
    auto &c = context; auto &scheduler = *c.scheduler;
    auto *process = scheduler.find(c.pid); auto *thread = scheduler.find_thread(c.pid, c.pid); CHECK(process && thread);
    if (c.guards && process->resources().prepared_descriptor_operations)
    {
        CHECK(process->resources().prepared_descriptor_operations == 1);
        CHECK(scheduler.drain_resources(process->resources()).code() == StatusCode::busy);
        CHECK(scheduler.drain_all_resources().code() == StatusCode::busy);
        CHECK(!scheduler.fork_process(c.pid));
        const auto cpu = arch::configured_arch_operations().make_user_context({hw::user_begin, hw::user_begin + 4096});
        CHECK(scheduler.replace_image(c.pid, cpu, {}).code() == StatusCode::busy);
        CHECK(scheduler.exit_process(c.pid, 0).code() == StatusCode::busy);
        CHECK(scheduler.reap_process(c.pid).code() == StatusCode::busy);
        CHECK(!c.operations->open(c.pid));
    }
    if (c.calls != c.action_at) return;
    const auto action = std::exchange(c.action, Action::none);
    if (action == Action::rotate_epoch)
    {
        auto view = c.pool->activate_entry(thread->entry_stack, {c.pid, c.pid}, 0); CHECK(view);
        CHECK(c.pool->deactivate_entry(thread->entry_stack, {c.pid, c.pid}, 0));
    }
    if (action == Action::change_frame) ++thread->native_frame.fp_state[23];
    if (action == Action::change_image) ++process->resources().native_space.root;
    if (action == Action::redispatch) CHECK(scheduler.activate_thread(c.pid, c.pid));
    if (action == Action::completed_wait)
    {
        auto wait = scheduler.register_current_wait(sched::ThreadWaitReason::deadline, 64); CHECK(wait);
        CHECK(scheduler.complete_wait(wait.value(), sched::WaitCompletion::cancelled));
        CHECK(scheduler.activate_thread(c.pid, c.pid));
    }
    if (action == Action::reinstall || action == Action::close)
    {
        const auto old = process->fd_table().entry(9)->installation;
        CHECK(scheduler.close_descriptor(process->resources(), 9));
        if (action == Action::reinstall)
        {
            CHECK(scheduler.install_descriptor(process->resources(), 9, c.description));
            CHECK(process->fd_table().entry(9)->installation != old);
        }
    }
    if (action == Action::reset)
    {
        Iss iss; CHECK(c.service->endpoints().ingest(packet(peer, 11, 501, wire::tcp_rst | wire::tcp_ack).view(), 2, iss));
    }
    if (action == Action::retire) CHECK(c.service->endpoints().retire());
}
bool interval(uptr address, usize bytes, usize limit)
{ return address >= hw::user_begin && address <= hw::user_begin + limit && bytes <= hw::user_begin + limit - address; }
}

namespace ok::arch::execution
{
Result<Trap> enter_owned(AddressSpace &, UserFrame &, const KernelEntryStackView &view)
{ ++traps; auto trap = model::trap(); trap.kernel_stack_pointer = view.top() - 128; return trap; }
// Synthetic native frame signature used only by this owned backend fixture.
Syscall syscall(const UserFrame &frame)
{
    Syscall result{frame.registers[30], {}};
    for (usize at = 0; at < 6; ++at) result.arguments[at] = frame.registers[24 + at];
    return result;
}
Status destroy_space(AddressSpace &space) { space = {}; return {}; }
bool user_range(const AddressSpace &space, uptr address, usize bytes, bool writable)
{
    ++range_queries; CHECK(native::BackendCapture::same_space(space, model::context.space));
    const bool fault = model::record(model::Kind::range, address, bytes, writable);
    const bool valid = model::interval(address, bytes, writable ? model::context.writable_limit : model::context.readable_limit);
    model::callback(); return !fault && valid;
}
Status copy_from_user(const AddressSpace &space, void *output, uptr address, usize bytes)
{
    ++reads; CHECK(native::BackendCapture::same_space(space, model::context.space));
    const bool fault = model::record(model::Kind::read, address, bytes, false);
    CHECK(model::interval(address, bytes, model::context.readable_limit));
    std::memcpy(output, model::context.memory.data() + address - user_begin, fault ? std::min(bytes, usize{1}) : bytes);
    model::callback(); return fault ? Status::fault("injected partial captured input") : Status{};
}
Status copy_to_user(const AddressSpace &space, uptr address, const void *input, usize bytes)
{
    ++writes; CHECK(native::BackendCapture::same_space(space, model::context.space));
    const bool fault = model::record(model::Kind::write, address, bytes, true);
    CHECK(model::interval(address, bytes, model::context.writable_limit));
    std::memcpy(model::context.memory.data() + address - user_begin, input, fault ? std::min(bytes, usize{1}) : bytes);
    model::callback(); return fault ? Status::fault("injected partial captured output") : Status{};
}
}

struct alignas(4096) StackBacking { std::array<std::byte, memory::KernelEntryStackPool::arena_bytes> bytes{}; };
template <lu::Profile P> struct Fixture
{
    using Codec = lu::Codec<P>; using Access = native::TcpImmediateAccess<P>;
    std::unique_ptr<StackBacking> backing{std::make_unique<StackBacking>()};
    memory::KernelEntryStackPool pool;
    native::SyscallOriginTable<P> origins{pool}; native::VfsStatContinuation<P> vfs{pool}; native::SharedNativeCapture<P> shared{pool};
    std::unique_ptr<sched::Scheduler> scheduler{std::make_unique<sched::Scheduler>()};
    NativeTcpStreamService service{tcp::TableIdentity{table_identity++}};
    TcpStreamOperations operations{*scheduler, service};
    u64 pid{}; u16 id{}, other_id{}; Status failure{}; bool failed_holder{};
    Fixture()
    {
        ++cases; CHECK(pool.activate(backing->bytes));
        auto created = scheduler->create_user_process("captured-tcp-accept-host",
            arch::configured_arch_operations().make_user_context({hw::user_begin, hw::user_begin + 4096})); CHECK(created); pid = created.value();
        process().resources().personality = sched::UserPersonality::linux;
        process().resources().native_space = {0x100000, 1, 1};
        CHECK(native::prepare_entry_stack(pool, thread())); thread().native_active = true;
        CHECK(origins.prepare(thread())); CHECK(vfs.prepare(thread()));
        CHECK(scheduler->set_runnable(pid)); CHECK(scheduler->activate_thread(pid, pid));
        CHECK(scheduler->set_native_tcp_service(service));
        auto opened = operations.open(pid, true, true); CHECK(opened);
        id = process().fd_table().entry(opened.value())->description;
        CHECK(scheduler->install_descriptor(process().resources(), 9, id)); CHECK(scheduler->close_descriptor(process().resources(), opened.value()));
        CHECK(operations.bind(pid, 9, server)); CHECK(operations.listen(pid, 9, 2));
        auto other = operations.open(pid, true); CHECK(other); other_id = process().fd_table().entry(other.value())->description;
        CHECK(scheduler->install_descriptor(process().resources(), 5, other_id)); CHECK(scheduler->install_descriptor(process().resources(), 18, other_id));
        CHECK(scheduler->close_descriptor(process().resources(), other.value()));
        auto alternate = server; ++alternate.port; CHECK(operations.bind(pid, 5, alternate)); CHECK(operations.listen(pid, 5, 2));
        model::context = {.scheduler=scheduler.get(), .pool=&pool, .service=&service, .operations=&operations,
            .pid=pid, .description=id, .profile=P, .space=process().resources().native_space};
        model::context.memory.fill(std::byte{0xa7});
        completed();
    }
    ~Fixture()
    {
        // This fixture never begins a VFS request; revoke its shared observer.
        origins.retire(thread()); vfs.revoke(thread()); CHECK(native::retire_entry_stack(pool, thread()));
        CHECK(pool.accounting().leased_slots == 0);
        if (!failed_holder) { CHECK(operations.status()); CHECK(failure); CHECK(scheduler->drain_all_resources()); CHECK(service.endpoints().retire()); }
        model::context = {};
    }
    sched::ProcessControlBlock &process() { return *scheduler->find(pid); }
    sched::ThreadControlBlock &thread() { return *scheduler->find_thread(pid, pid); }
    void completed()
    {
        Iss iss; CHECK(service.endpoints().ingest(packet(peer, 10, 0, wire::tcp_syn).view(), 0, iss));
        Packet out; auto response = service.endpoints().prepare(out.bytes, 0); CHECK(response && response.value());
        out.length = response.value()->length; CHECK(service.endpoints().complete(response.value()->ticket, true, 0));
        auto ip = wire::ipv4(out.view()); CHECK(ip); auto tcp = wire::tcp(ip.value()); CHECK(tcp);
        CHECK(service.endpoints().ingest(packet(peer, 11, tcp.value().fields.sequence + 1, wire::tcp_ack).view(), 1, iss));
        CHECK(service.endpoints().pools().unaccepted == 1);
    }
    static void set_register(hw::UserFrame &frame, usize index, u64 value)
    {
        if constexpr (P == lu::Profile::i386)
        { constexpr usize slots[]{7,6,5,4,0,2,1,0}; CHECK(index<8 && index!=4); frame.registers[slots[index]]=static_cast<uptr>(value); }
        else if constexpr (P == lu::Profile::x86_64)
        { constexpr usize slots[]{14,11,12,13,10,9,8,0,7,6,5,4,3,2,1,0}; CHECK(index<16 && index!=7); frame.registers[slots[index]]=static_cast<uptr>(value); }
        else frame.registers[index] = static_cast<uptr>(value);
    }
    void word(usize offset, u32 value)
    {
        CHECK(offset + 4 <= model::context.memory.size());
        for (usize at=0; at<4; ++at) model::context.memory[offset+at] = std::byte(value >> (Codec::profile.big_endian ? (3-at)*8 : at*8));
    }
    void observe(bool linux, bool accept4, bool socketcall, u32 flags, uptr array = array_address)
    {
        process().resources().personality = linux ? sched::UserPersonality::linux : sched::UserPersonality::native;
        CHECK(scheduler->activate_thread(pid, pid));
        auto &frame = thread().native_frame; frame={}; frame.pc=hw::user_begin+1024; frame.sp=hw::user_end-1; frame.fp_state[23]=73;
        if constexpr (P == lu::Profile::arm32) { frame.flags=0x10; frame.registers[15]=frame.pc; }
        std::array<u64,6> args{9,peer_address,length_address,flags,~u64{0},~u64{0}};
        if (linux)
        {
            const auto number = socketcall ? lu::tcp_socketcall_numbers[static_cast<usize>(P)] :
                accept4 ? lu::tcp_accept4_numbers[static_cast<usize>(P)] : lu::tcp_accept_numbers[static_cast<usize>(P)];
            CHECK(number != lu::unavailable_number); set_register(frame, Codec::profile.number_register, number);
            if (socketcall) { args={accept4 ? 18u : 5u,array,0,0,0,0}; word(64,9); word(68,peer_address); word(72,length_address); word(76,flags); }
            for (usize at=0; at<args.size(); ++at) if (Codec::profile.argument_registers[at]>=0)
                set_register(frame, static_cast<usize>(Codec::profile.argument_registers[at]), args[at]);
        }
        else
        {
            frame.registers[30]=static_cast<uptr>(accept4 ? syscall::Number::accept4 : syscall::Number::accept);
            for (usize at=0; at<args.size(); ++at) frame.registers[24+at]=static_cast<uptr>(args[at]);
        }
        word(256,16); thread().syscall_pending=false;
        auto view=pool.activate_entry(thread().entry_stack,{pid,pid},0); CHECK(view);
        auto observed=shared.enter(origins,vfs,process(),thread(),view.value(),*scheduler); CHECK(observed);
        CHECK(pool.deactivate_entry(thread().entry_stack,{pid,pid},0)); thread().syscall_pending=true;
    }
    void unchanged()
    {
        CHECK(process().resources().prepared_descriptor_operations==0 && service.prepared_accepts()==0);
        CHECK(service.endpoints().pools().unaccepted==1 && service.endpoints().pools().accept_holds==0);
        CHECK(process().fd_table().open_count()==6 && service.active_streams()==2);
    }
};

template <lu::Profile P> void success(bool linux, bool accept4, bool socketcall)
{
    Fixture<P> f; const u32 flags = accept4 ? (linux ? lu::UdpConstants<P>::sock_nonblock|lu::UdpConstants<P>::sock_cloexec : OK_SOCK_NONBLOCK|OK_SOCK_CLOEXEC) : ~u32{0};
    f.observe(linux,accept4,socketcall,flags);
    auto admitted=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(admitted); auto &access=admitted.value();
    const auto saved=f.thread().native_frame; native::TcpOriginalDescription original{*f.scheduler,f.service,f.failure};
    auto captured=native::TcpCapturedAccept<P>::capture(*f.scheduler,saved,access,original); CHECK(captured);
    const auto &request=captured.value(); CHECK(request.original==native::TcpDescriptionKind::retained_tcp && request.arguments[0]==9);
    CHECK(request.arguments[1]==peer_address && request.arguments[2]==length_address && request.arguments[3]==(accept4 ? flags : 0));
    CHECK(original.matches(f.pid,9) && f.scheduler->open_description(f.id)->references==2);
    CHECK(f.scheduler->open_description(f.other_id)->references==2);
    CHECK(model::context.calls==(socketcall ? 4u : 0u));
    if (socketcall)
    {
        CHECK(model::context.events[0].address==array_address && model::context.events[0].size==4 && model::context.events[0].references==1);
        CHECK(model::context.events[1].kind==model::Kind::read && model::context.events[1].size==4);
        CHECK(model::context.events[2].address==array_address+4 && model::context.events[2].size==(accept4 ? 12u : 8u) && model::context.events[2].references==2);
        CHECK(model::context.events[3].kind==model::Kind::read && model::context.events[3].address==array_address+4);
    }
    model::context.calls=0; model::context.guards=true;
    TcpAcceptCopied<P> adapter{f.operations}; auto prepared=adapter.prepare(f.pid,9,request.arguments[1],request.arguments[2],static_cast<u32>(request.arguments[3]),accept4,linux,access,original); CHECK(prepared);
    CHECK(model::context.calls==9 && access.live() && f.scheduler->open_description(f.id)->references==3);
    auto accepted=prepared.value().commit(); CHECK(accepted && accepted.value()!=9 && accepted.value()!=5 && accepted.value()!=18);
    const auto *slot=f.process().fd_table().entry(accepted.value()); const auto *open=f.scheduler->open_description(slot->description);
    CHECK(slot->used && slot->close_on_exec==accept4 && bool(open->flags&TcpStreamOperations::nonblock)==accept4);
    CHECK(f.service.endpoints().pools().unaccepted==0 && f.service.prepared_accepts()==0);
    std::array<u8,16> bytes{}; for (usize at=0;at<16;++at) bytes[at]=std::to_integer<u8>(model::context.memory[512+at]);
    auto decoded=lu::UdpWire<P>::decode_address(bytes); CHECK(decoded && decoded.value().address==peer.address && decoded.value().port==peer.port);
    CHECK(f.scheduler->open_description(f.id)->references==3);
    // Captured result publication keeps both original pins until image reset.
    if (linux) { auto published=lu::NativeBridge<P>::encode_return(f.thread().native_frame,{static_cast<u64>(accepted.value()),0}); CHECK(published); }
    access.reset(); CHECK(prepared.value().retire()); CHECK(original.release());
    CHECK(f.scheduler->open_description(f.id)->references==1 && f.process().resources().prepared_descriptor_operations==0);
    CHECK(f.scheduler->close_descriptor(f.process().resources(),accepted.value()));
}

template <lu::Profile P> void output_failures(bool linux)
{
    for (auto action : {model::Action::fault, model::Action::rotate_epoch, model::Action::change_frame,
                        model::Action::change_image, model::Action::reinstall})
        for (usize boundary=1;boundary<=9;++boundary)
        {
            Fixture<P> f; f.observe(linux,true,false,0);
            f.word(256,64);
            auto admitted=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(admitted); auto &access=admitted.value();
            native::TcpOriginalDescription original{*f.scheduler,f.service,f.failure};
            auto captured=native::TcpCapturedAccept<P>::capture(*f.scheduler,f.thread().native_frame,access,original); CHECK(captured);
            model::context.action=action; model::context.action_at=boundary; model::context.calls=0; model::context.guards=true;
            TcpAcceptCopied<P> adapter{f.operations}; auto prepared=adapter.prepare(f.pid,9,peer_address,length_address,0,true,linux,access,original);
            CHECK(!prepared && model::context.calls==boundary); f.unchanged();
            CHECK(f.scheduler->open_description(f.id)->references==2);
            if (action==model::Action::fault && boundary==7)
                CHECK(model::context.memory[512]!=std::byte{0xa7} && model::context.memory[513]==std::byte{0xa7});
            if (action==model::Action::fault && boundary==9)
                CHECK(model::context.memory[lu::Codec<P>::profile.big_endian ? 259 : 256]==std::byte{lu::Codec<P>::profile.big_endian ? u8{64} : u8{16}});
            access.reset(); CHECK(original.release()); CHECK(f.scheduler->open_description(f.id)->references==1);
        }
}

template <lu::Profile P> void socketcall_failures(bool accept4)
{
    for (auto action : {model::Action::fault,model::Action::rotate_epoch,model::Action::change_frame,
                        model::Action::change_image,model::Action::redispatch,model::Action::completed_wait})
        for (usize boundary=1;boundary<=4;++boundary)
        {
            Fixture<P> f; f.observe(true,accept4,true,0);
            auto admitted=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(admitted); auto &access=admitted.value();
            const auto saved=f.thread().native_frame; native::TcpOriginalDescription original{*f.scheduler,f.service,f.failure};
            model::context.action=action; model::context.action_at=boundary;
            auto captured=native::TcpCapturedAccept<P>::capture(*f.scheduler,saved,access,original);
            CHECK(!captured && model::context.calls==boundary); f.unchanged();
            CHECK(f.scheduler->open_description(f.id)->references==(boundary<=2 ? 1u : 2u));
            CHECK(!original.matches(f.pid,5) && !original.matches(f.pid,18));
            access.reset(); CHECK(original.release()); CHECK(f.scheduler->open_description(f.id)->references==1);
        }
    for (usize boundary : {3u,4u})
    {
        Fixture<P> f; f.observe(true,accept4,true,0);
        auto admitted=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(admitted); auto &access=admitted.value();
        const auto saved=f.thread().native_frame; native::TcpOriginalDescription original{*f.scheduler,f.service,f.failure};
        model::context.action=model::Action::reinstall; model::context.action_at=boundary;
        auto captured=native::TcpCapturedAccept<P>::capture(*f.scheduler,saved,access,original);
        CHECK(!captured && model::context.calls==boundary); f.unchanged(); CHECK(f.scheduler->open_description(f.id)->references==2);
        access.reset(); CHECK(original.release()); CHECK(f.scheduler->open_description(f.id)->references==1);
    }
    // A mapped fd head does not imply mapped tail. Selection is held exactly
    // once, then released without descriptor/child admission after refusal.
    for (usize mode=0;mode<3;++mode)
    {
        Fixture<P> f; f.observe(true,accept4,true,0);
        auto admitted=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(admitted); auto &access=admitted.value();
        native::TcpOriginalDescription original{*f.scheduler,f.service,f.failure};
        if (mode==0) model::context.readable_limit=68;
        if (mode==1) { model::context.action=model::Action::fault; model::context.action_at=4; }
        if (mode==2) model::context.readable_limit=accept4 ? 76 : 72;
        auto captured=native::TcpCapturedAccept<P>::capture(*f.scheduler,f.thread().native_frame,access,original);
        CHECK(!captured && model::context.calls==(mode==1 ? 4u : 3u)); f.unchanged();
        CHECK(f.scheduler->open_description(f.id)->references==2);
        access.reset(); CHECK(original.release()); CHECK(f.scheduler->open_description(f.id)->references==1);
    }
    for (uptr pointer : {uptr{0}, static_cast<uptr>(lu::Codec<P>::word_mask-7)})
    {
        Fixture<P> f; f.observe(true,accept4,true,0,pointer);
        auto admitted=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(admitted); auto &access=admitted.value();
        native::TcpOriginalDescription original{*f.scheduler,f.service,f.failure};
        CHECK(!native::TcpCapturedAccept<P>::capture(*f.scheduler,f.thread().native_frame,access,original));
        CHECK(model::context.calls==0 && f.scheduler->open_description(f.id)->references==1); f.unchanged(); access.reset(); CHECK(original.release());
    }
    for (u32 fd : {u32{1},u32{0xffffffff}})
    {
        Fixture<P> f; f.observe(true,accept4,true,0); f.word(64,fd);
        auto admitted=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(admitted); auto &access=admitted.value();
        native::TcpOriginalDescription original{*f.scheduler,f.service,f.failure};
        auto captured=native::TcpCapturedAccept<P>::capture(*f.scheduler,f.thread().native_frame,access,original);
        CHECK(captured && captured.value().original==(fd==1 ? native::TcpDescriptionKind::not_socket : native::TcpDescriptionKind::bad_fd));
        CHECK(model::context.calls==2 && f.scheduler->open_description(f.id)->references==1); f.unchanged(); access.reset(); CHECK(original.release());
    }
    if (accept4)
    {
        Fixture<P> f; const u32 invalid=lu::UdpConstants<P>::sock_nonblock==0x80 ? 0x800 : 0x80; f.observe(true,true,true,invalid);
        auto admitted=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(admitted); auto &access=admitted.value();
        native::TcpOriginalDescription original{*f.scheduler,f.service,f.failure};
        auto captured=native::TcpCapturedAccept<P>::capture(*f.scheduler,f.thread().native_frame,access,original); CHECK(captured && captured.value().arguments[3]==invalid);
        TcpAcceptCopied<P> adapter{f.operations}; CHECK(!adapter.prepare(f.pid,9,peer_address,length_address,invalid,true,true,access,original));
        CHECK(model::context.calls==4); f.unchanged(); access.reset(); CHECK(original.release());
    }
}

template <lu::Profile P> void flags_and_peer_edges()
{
    for (bool linux : {false,true})
    {
        Fixture<P> f; const u32 invalid=linux && lu::UdpConstants<P>::sock_nonblock==0x80 ? 0x800 : 0x80;
        f.observe(linux,true,false,invalid); auto admitted=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(admitted); auto &access=admitted.value();
        native::TcpOriginalDescription original{*f.scheduler,f.service,f.failure}; auto captured=native::TcpCapturedAccept<P>::capture(*f.scheduler,f.thread().native_frame,access,original); CHECK(captured);
        TcpAcceptCopied<P> adapter{f.operations}; CHECK(!adapter.prepare(f.pid,9,peer_address,length_address,invalid,true,linux,access,original));
        CHECK(model::context.calls==0); f.unchanged(); access.reset(); CHECK(original.release());
    }
    for (auto action : {model::Action::reset,model::Action::retire,model::Action::close})
    {
        Fixture<P> f; f.observe(true,true,false,0); auto admitted=Fixture<P>::Access::acquire(f.origins,f.process(),f.thread()); CHECK(admitted); auto &access=admitted.value();
        native::TcpOriginalDescription original{*f.scheduler,f.service,f.failure}; CHECK(native::TcpCapturedAccept<P>::capture(*f.scheduler,f.thread().native_frame,access,original));
        model::context.action=action; model::context.action_at=7; model::context.guards=true;
        TcpAcceptCopied<P> adapter{f.operations}; auto prepared=adapter.prepare(f.pid,9,peer_address,length_address,0,true,true,access,original);
        // Terminal protocol edges can retain their immutable preview. They
        // must refuse commit and retire without reviving a queue edge.
        if (action==model::Action::close) CHECK(!prepared);
        else { CHECK(prepared && !prepared.value().commit()); CHECK(prepared.value().retire()); }
        CHECK(f.process().resources().prepared_descriptor_operations==0 && f.service.prepared_accepts()==0 && f.service.endpoints().pools().accept_holds==0);
        CHECK(!f.process().fd_table().entry(3)->used && f.process().fd_table().entry(3)->reservation==0);
        access.reset(); CHECK(original.release());
        CHECK(f.service.endpoints().pools().unaccepted==0);
    }
}

void sticky_holder()
{
    Fixture<lu::Profile::x86_64> f; f.observe(true,true,false,0);
    auto admitted=decltype(f)::Access::acquire(f.origins,f.process(),f.thread()); CHECK(admitted); auto &access=admitted.value();
    native::TcpOriginalDescription original{*f.scheduler,f.service,f.failure}; CHECK(native::TcpCapturedAccept<lu::Profile::x86_64>::capture(*f.scheduler,f.thread().native_frame,access,original));
    TcpAcceptCopied<lu::Profile::x86_64> adapter{f.operations}; auto prepared=adapter.prepare(f.pid,9,peer_address,length_address,0,true,true,access,original); CHECK(prepared);
    std::optional<PreparedTcpAccept> held{std::move(prepared.value())};
    const auto fd=held->descriptor(); const auto token=f.process().fd_table().entry(fd)->reservation;
    const auto id=[&] { for (u16 at=3;at<sched::max_open_descriptions;++at) if (auto *open=f.scheduler->open_description(at);open && open->reservation==token) return at; return sched::invalid_description; }(); CHECK(token && id!=sched::invalid_description);
    struct FailingIss
    {
        PreparedTcpAccept &holder; TcpStreamOperations &operations;
        Result<u32> operator()(tcp::Endpoint,tcp::Endpoint)
        { CHECK(holder.retire().code()==StatusCode::busy); CHECK(!operations.status()); return u32{700}; }
    } iss{*held,f.operations};
    auto another=peer; ++another.port; CHECK(f.service.endpoints().ingest(packet(another,20,0,wire::tcp_syn).view(),2,iss));
    CHECK(held->retire().code()==StatusCode::busy && !held->commit());
    access.reset(); CHECK(original.release()); held.reset();
    CHECK(f.service.prepared_accepts()==1 && f.service.endpoints().pools().accept_holds==1 && f.service.endpoints().pools().accept_listener_holds==1);
    CHECK(f.process().resources().prepared_descriptor_operations==1 && f.process().fd_table().entry(fd)->reservation==token);
    CHECK(!f.process().fd_table().entry(fd)->used && f.scheduler->open_description(id)->reservation==token);
    CHECK(f.scheduler->open_description(f.id)->references==2 && f.scheduler->drain_all_resources().code()==StatusCode::busy);
    CHECK(!f.scheduler->retain_description(id) && !f.operations.open(f.pid));
    CHECK(f.scheduler->close_descriptor(f.process().resources(),9)); CHECK(f.scheduler->open_description(f.id)->references==1);
    CHECK(f.service.endpoints().retire() && f.service.endpoints().pools().accept_holds==1);
    CHECK(f.process().fd_table().entry(fd)->reservation==token && f.scheduler->drain_all_resources().code()==StatusCode::busy);
    f.failed_holder=true;
}

template <lu::Profile P> void exercise()
{
    ++profiles; success<P>(false,false,false); success<P>(false,true,false); success<P>(true,true,false);
    if constexpr (lu::tcp_accept_numbers[static_cast<usize>(P)]!=lu::unavailable_number) success<P>(true,false,false);
    output_failures<P>(true); output_failures<P>(false); flags_and_peer_edges<P>();
    if constexpr (lu::tcp_socketcall_numbers[static_cast<usize>(P)]!=lu::unavailable_number)
    { success<P>(true,false,true); success<P>(true,true,true); socketcall_failures<P>(false); socketcall_failures<P>(true); }
}
int main()
{
    exercise<lu::Profile::i386>(); exercise<lu::Profile::x86_64>(); exercise<lu::Profile::aarch64>(); exercise<lu::Profile::arm32>();
    exercise<lu::Profile::rv64>(); exercise<lu::Profile::rv32>(); exercise<lu::Profile::loongarch64>(); exercise<lu::Profile::mips>();
    exercise<lu::Profile::mips64>(); exercise<lu::Profile::ppc>(); sticky_holder(); CHECK(profiles==10);
    std::printf("TCP_CAPTURED_ACCEPT_PASS checks=%zu profiles=%zu cases=%zu owned_synthetic_traps=%zu ranges=%zu reads=%zu writes=%zu actual_origin_memory_integration=1 guest_execution=0 syscall_execution=0 device_execution=0\n",checks,profiles,cases,traps,range_queries,reads,writes);
}
