// Actual private x86_64 capture/admission/owner/scheduler/OFD/receive mechanics.
// Trap, flat mappings and peer frames are hosted controls; no guest or NIC runs.
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_udp_receive_model.hpp"
#include <source_location>
#include <vector>
static_assert(lu::configured_native_profile() == lu::Profile::x86_64);
namespace
{
using Access = native::LinuxUdpImmediateAccess<lu::Profile::x86_64>;
using Admission = native::LinuxUdpReceiveAdmissionToken;
using Capture = native::SyscallOrigins::LinuxUdpReceiveCapture;
using Borrow = native::SyscallOrigins::LinuxUdpReceiveBorrow;
static_assert(!std::is_copy_constructible_v<Admission> && !std::is_default_constructible_v<Admission>);
static_assert(!std::is_copy_constructible_v<Capture> && !std::is_default_constructible_v<Capture>);
static_assert(!std::is_copy_constructible_v<Borrow> && !std::is_default_constructible_v<Borrow>);
static_assert(!std::is_constructible_v<Admission, lu::UdpRequest, Status>);
static_assert(native::SyscallOrigins::supported_binding<native::SyscallOrigins::LinuxUdpReceiveBinding>);
static_assert(!std::same_as<Borrow, native::SyscallOrigins::LinuxUdpRestartPermit>);
using E = lu::UdpErrno;
using C = lu::UdpConstants<lu::Profile::x86_64>;
constexpr auto profile = lu::Profile::x86_64;
constexpr uptr output = hw::user_begin + 128, source = hw::user_begin + 512, length_address = hw::user_begin + 640;
constexpr net::NativeUdpLink udp_link{{2,1,2,3,4,5},{2,6,7,8,9,10},{192,0,2,2},{192,0,2,1}};
constexpr std::array<std::byte,3> bytes{std::byte{'a'},std::byte{'b'},std::byte{'c'}};
net::NativeUdpService peer;
u32 sender{};
usize checks{}, captured_entries{}, cases{};
void expect(bool value, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr,"LINUX_UDP_OWNER_FAIL line=%u checks=%zu\n",where.line(),checks); std::abort(); }
}
i64 error(E e) { return -static_cast<i64>(*C::errno_number(e)); }
struct Group { sched::ProcessId pid{}; i32 fd{}; u16 description{}, port{}; u32 endpoint{}; };
Group create(Kernel &kernel, u16 port)
{
    auto made = kernel.scheduler().create_user_process("linux-udp-private-owner-host",
        kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096})); CHECK(made);
    auto *process = kernel.scheduler().find(made.value()); CHECK(process);
    process->resources().personality = sched::UserPersonality::linux;
    process->resources().native_space = {0x100000+made.value()*4096,static_cast<u32>(made.value()%16),1};
    auto &thread = process->threads()[0]; CHECK(native::prepare_native_thread(thread)); thread.native_active=true;
    CHECK(kernel.scheduler().set_runnable(made.value())); CHECK(kernel.scheduler().activate_thread(made.value(),made.value()));
    CHECK(kernel.posix().activate(made.value()));
    auto endpoint = kernel.native_udp().open(); CHECK(endpoint); CHECK(kernel.native_udp().bind(endpoint.value(),{},port));
    auto fd = kernel.posix().install_native_udp(endpoint.value(),false,false); CHECK(fd);
    auto *entry = process->fd_table().entry(fd.value()); CHECK(entry);
    native::runtime_kernel=&kernel; native::started=true; native::runtime_failure={};
    ++cases;
    return {made.value(),fd.value(),entry->description,port,endpoint.value()};
}
auto &process(Kernel &kernel, Group g) { return *kernel.scheduler().find(g.pid); }
auto &thread(Kernel &kernel, Group g) { return *kernel.scheduler().find_thread(g.pid,g.pid); }
void select(Kernel &kernel, Group g)
{ CHECK(kernel.scheduler().activate_thread(g.pid,g.pid)); }
void observe(Kernel &kernel, Group g, u32 flags=0, bool address=false, usize count=8, uptr payload=output)
{
    select(kernel,g);
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    t.syscall_pending=false;
    hw::UserFrame frame{}; frame.pc=hw::user_begin+256; frame.sp=hw::user_begin+4096; frame.fp_state[23]=83;
    frame.registers[14]=*C::syscall_number(lu::UdpCall::recvfrom);
    const std::array<usize,6> slots{9,10,11,5,7,6};
    const std::array<uptr,6> args{static_cast<uptr>(g.fd),payload,count,flags,address?source:0,address?length_address:0};
    for (usize i=0;i<6;++i) frame.registers[slots[i]]=args[i];
    t.native_frame=frame;
    auto view=native::entry_stacks.activate_entry(t.entry_stack,{g.pid,g.pid},0); CHECK(view);
    mock::entry_trap={hw::TrapKind::syscall,256};
    auto observed=native::shared_capture.enter(native::syscall_origins,native::vfs_continuations,p,t,view.value(),kernel.scheduler());
    if (!observed) std::fprintf(stderr,"observe pid=%llu cases=%zu status=%u message=%.*s\n",
        static_cast<unsigned long long>(g.pid),cases,static_cast<unsigned>(observed.status().code()),
        static_cast<int>(observed.status().message().size()),observed.status().message().data());
    CHECK(observed);
    CHECK(native::entry_stacks.deactivate_entry(t.entry_stack,{g.pid,g.pid},0));
    t.syscall_pending=true; ++captured_entries;
}
void packet(Kernel &kernel, Group g, std::span<const std::byte> payload=bytes)
{
    std::array<std::byte,net::NativeUdpService::frame_capacity> frame{},reply{};
    auto built=peer.build_datagram(sender,udp_link.local_ip,g.port,payload,frame); CHECK(built);
    auto ingested=kernel.native_udp().ingest({frame.data(),built.value()},reply); CHECK(ingested);
    expect(ingested.value().queued_handle==g.endpoint);
}
void length(u32 value) { std::memcpy(mock::memory.data()+640,&value,4); }
u32 length() { u32 value{}; std::memcpy(&value,mock::memory.data()+640,4); return value; }
memory::ObjectHandle arm(Kernel &kernel,Group g,u32 flags=0,bool address=false,usize count=8)
{
    observe(kernel,g,flags,address,count);
    auto current=Access::acquire(native::syscall_origins,process(kernel,g),thread(kernel,g)); CHECK(current);
    Status failure{}; native::linux_udp::OriginalUdpDescription original{kernel.scheduler(),failure};
    native::linux_udp::ImmediateLifetimeScope lifetime{current.value(),original};
    auto prepared=native::linux_udp_operations.prepare_blocking_receive<profile>(kernel,process(kernel,g),thread(kernel,g),current.value(),original); CHECK(prepared);
    expect(prepared.value().admission.has_value() && !prepared.value().target_result);
    expect(kernel.scheduler().open_description(g.description)->references==2);
    auto begun=native::begin_linux_udp_receive(kernel,process(kernel,g),thread(kernel,g),current.value(),original,std::move(*prepared.value().admission)); CHECK(begun);
    expect(begun.value().kind==native::LinuxUdpReceiveBeginKind::deferred && !begun.value().target_result);
    expect(!current.value().live() && !original.held()); CHECK(lifetime.finish()); expect(failure.ok());
    auto object=native::linux_udp_receive_object(g.pid,g.pid); CHECK(object.pool);
    auto owned=native::linux_udp_receive_objects.resolve(object); CHECK(owned);
    expect(owned.value()->phase==native::LinuxUdpReceivePhase::published);
    expect(owned.value()->request_sequence==owned.value()->ticket.generation && thread(kernel,g).wait_registration.pending(owned.value()->ticket));
    expect(owned.value()->operation.flags==flags && owned.value()->operation.count==count);
    expect(kernel.scheduler().open_description(g.description)->references==2);
    return object;
}
void resume(Kernel &kernel,Group g,memory::ObjectHandle object,i64 expected)
{
    CHECK(native::scan_linux_udp_receives(kernel)); select(kernel,g);
    const auto entries=mock::entries;
    CHECK(native::resume_linux_udp_receive(kernel,process(kernel,g),thread(kernel,g),object));
    expect(mock::entries==entries && !thread(kernel,g).syscall_pending);
    expect(static_cast<i64>(thread(kernel,g).native_frame.registers[14])==expected);
    expect(!native::linux_udp_receive_object(g.pid,g.pid).pool && !native::linux_udp_receive_objects.resolve(object));
    expect(kernel.scheduler().open_description(g.description)->references==1);
}
void retire(Kernel &kernel,Group g)
{
    CHECK(native::retire_linux_udp_receives(kernel,g.pid,{},native::LinuxUdpReceiveRetirement::process_exit));
    native::runtime_failure={}; native::started=true;
    native::release_process(kernel,g.pid); CHECK(kernel.scheduler().exit_process(g.pid,0)); CHECK(kernel.scheduler().reap_orphans());
    expect(native::runtime_status().ok() && native::linux_udp_receive_objects.accounting().live_objects==0);
}
void normal_and_rearm(Kernel &kernel)
{
    auto g=create(kernel,55001); length(16);
    auto object=arm(kernel,g,0,true,2); auto owned=native::linux_udp_receive_objects.resolve(object); CHECK(owned);
    const auto copied=owned.value()->operation; const auto sequence=owned.value()->request_sequence; const auto old_ticket=owned.value()->ticket;
    packet(kernel,g); CHECK(native::scan_linux_udp_receives(kernel)); CHECK(kernel.native_udp().consume(g.endpoint)); select(kernel,g);
    CHECK(native::resume_linux_udp_receive(kernel,process(kernel,g),thread(kernel,g),object));
    owned=native::linux_udp_receive_objects.resolve(object); CHECK(owned);
    expect(owned.value()->request_sequence==sequence && owned.value()->operation==copied && owned.value()->ticket.generation>old_ticket.generation);
    expect(thread(kernel,g).wait_registration.pending(owned.value()->ticket));
    length(1); packet(kernel,g); resume(kernel,g,object,2); expect(length()==16);
    expect(mock::memory[128]==std::byte{'a'} && mock::memory[129]==std::byte{'b'});
    CHECK(native::retire_linux_udp_receive(kernel,object,native::LinuxUdpReceiveRetirement::completed)); retire(kernel,g);
}
void receive_order(Kernel &kernel)
{
    for (u32 flags : {0U,2U,32U,34U})
    {
        auto g=create(kernel,static_cast<u16>(55010+flags)); auto object=arm(kernel,g,flags,false,2);
        packet(kernel,g); mock::write_forbidden=output; resume(kernel,g,object,error(E::e_fault)); mock::write_forbidden=0;
        const auto retained=kernel.native_udp().peek(g.endpoint); expect(bool(retained)==bool(flags&2));
        if (retained) CHECK(kernel.native_udp().consume(g.endpoint));
        retire(kernel,g);
    }
    auto g=create(kernel,55050); auto object=arm(kernel,g,32,false,1); packet(kernel,g); resume(kernel,g,object,3); retire(kernel,g);
    g=create(kernel,55051); object=arm(kernel,g,0,false,0); packet(kernel,g,{}); resume(kernel,g,object,0); retire(kernel,g);
    g=create(kernel,55052); length(0xffffffffU); object=arm(kernel,g,0,true,2); packet(kernel,g);
    resume(kernel,g,object,error(E::e_inval)); expect(kernel.native_udp().peek(g.endpoint).status().code()==StatusCode::would_block); retire(kernel,g);
}
void original_reuse(Kernel &kernel)
{
    auto g=create(kernel,55060);
    auto sibling=kernel.scheduler().create_thread_prepared(g.pid,
        kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096}),
        [](sched::ThreadControlBlock &created) { return native::prepare_native_thread(created); }); CHECK(sibling);
    auto *actor=kernel.scheduler().find_thread(g.pid,sibling.value()); CHECK(actor); actor->native_active=true;
    auto object=arm(kernel,g);
    CHECK(kernel.scheduler().activate_thread(g.pid,sibling.value()));
    CHECK(kernel.posix().close(g.fd)); expect(kernel.scheduler().open_description(g.description)->references==1);
    auto replacement=kernel.native_udp().open(); CHECK(replacement); auto fd=kernel.posix().install_native_udp(replacement.value(),false,false); CHECK(fd);
    expect(fd.value()==g.fd); packet(kernel,g); CHECK(native::scan_linux_udp_receives(kernel)); select(kernel,g);
    CHECK(native::resume_linux_udp_receive(kernel,process(kernel,g),thread(kernel,g),object));
    expect(!kernel.scheduler().open_description(g.description) && !kernel.native_udp().binding(g.endpoint));
    expect(kernel.native_udp().binding(replacement.value()).ok()); retire(kernel,g);
}
void staged_change_and_pressure(Kernel &kernel)
{
    for (bool pressure : {false,true})
    {
        auto g=create(kernel,static_cast<u16>(55070+pressure)); observe(kernel,g);
        auto access=Access::acquire(native::syscall_origins,process(kernel,g),thread(kernel,g)); CHECK(access);
        Status failure{}; native::linux_udp::OriginalUdpDescription original{kernel.scheduler(),failure};
        native::linux_udp::ImmediateLifetimeScope lifetime{access.value(),original};
        auto prepared=native::linux_udp_operations.prepare_blocking_receive<profile>(kernel,process(kernel,g),thread(kernel,g),access.value(),original); CHECK(prepared);
        if (pressure)
            for (auto &entry:native::linux_udp_receive_waits) entry={g.pid+1000, g.pid+1000, {}};
        else packet(kernel,g);
        auto begun=native::begin_linux_udp_receive(kernel,process(kernel,g),thread(kernel,g),access.value(),original,std::move(*prepared.value().admission)); CHECK(begun);
        if (pressure)
        {
            expect(begun.value().kind==native::LinuxUdpReceiveBeginKind::immediate && begun.value().target_result==error(E::e_nomem));
            expect(access.value().live() && original.held());
            for(auto &entry:native::linux_udp_receive_waits) entry={};
            thread(kernel,g).syscall_pending=false; CHECK(native::set_linux_udp_result(process(kernel,g),thread(kernel,g).native_frame,*begun.value().target_result));
        }
        else
            expect(begun.value().kind==native::LinuxUdpReceiveBeginKind::owned_completed && !begun.value().target_result && !thread(kernel,g).syscall_pending);
        CHECK(lifetime.finish()); expect(kernel.scheduler().open_description(g.description)->references==1); retire(kernel,g);
    }
}
Kernel *callback_kernel{}; memory::ObjectHandle callback_object{}; Group callback_group{}; bool callback_revoke{};
void during_copy()
{
    mock::after_usercopy=nullptr;
    auto owned=native::linux_udp_receive_objects.resolve(callback_object); CHECK(owned);
    expect(native::syscall_origins.linux_udp_receive_borrowed(*owned.value()->capture));
    const auto objects=native::linux_udp_receive_objects.accounting().destructions;
    const auto status=native::retire_linux_udp_receive(*callback_kernel,callback_object,native::LinuxUdpReceiveRetirement::rollback);
    expect(status.code()==StatusCode::busy);
    expect(native::linux_udp_receive_object(callback_group.pid,callback_group.pid)==callback_object);
    expect(native::linux_udp_receive_objects.accounting().destructions==objects);
    expect(callback_kernel->scheduler().open_description(callback_group.description)->references==2);
    if (callback_revoke) native::syscall_origins.revoke(thread(*callback_kernel,callback_group));
    // Revocation is pending until the exact borrower drops its table count.
    expect(native::syscall_origins.linux_udp_receive_borrowed(*owned.value()->capture));
}
void borrow_failure_and_publication(Kernel &kernel)
{
    for(bool revoke:{false,true})
    {
        auto g=create(kernel,static_cast<u16>(55080+revoke)); auto object=arm(kernel,g); packet(kernel,g);
        CHECK(native::scan_linux_udp_receives(kernel)); select(kernel,g); callback_kernel=&kernel; callback_object=object; callback_group=g; callback_revoke=revoke;
        const auto before=thread(kernel,g).native_frame;
        mock::after_usercopy=during_copy;
        const auto status=native::resume_linux_udp_receive(kernel,process(kernel,g),thread(kernel,g),object);
        expect(!status && !native::started && !native::runtime_failure);
        expect(thread(kernel,g).syscall_pending && native::SyscallOrigins::same_frame(before,thread(kernel,g).native_frame));
        expect(!native::linux_udp_receive_object(g.pid,g.pid).pool && kernel.scheduler().open_description(g.description)->references==1);
        CHECK(native::retire_linux_udp_receive(kernel,object,native::LinuxUdpReceiveRetirement::rollback)); retire(kernel,g);
    }
    auto g=create(kernel,55082); auto object=arm(kernel,g); packet(kernel,g); CHECK(native::scan_linux_udp_receives(kernel)); select(kernel,g);
    const auto original=thread(kernel,g).native_frame;
    const auto status=native::complete_linux_udp_receive(kernel,object,-4096); expect(!status && !native::started);
    expect(!thread(kernel,g).syscall_pending && native::SyscallOrigins::same_frame(original,thread(kernel,g).native_frame));
    expect(!native::linux_udp_receive_object(g.pid,g.pid).pool && kernel.scheduler().open_description(g.description)->references==1);
    expect(!native::complete_linux_udp_receive(kernel,object,1)); retire(kernel,g);
}
void admission_immediate_and_changed_flags(Kernel &kernel)
{
    for (unsigned mode=0;mode<4;++mode)
    {
        auto g=create(kernel,static_cast<u16>(55100+mode));
        const u32 flags=mode==1?64:mode==2?1:0;
        if(mode==0) packet(kernel,g);
        if(mode==3) CHECK(kernel.posix().fcntl(g.fd,posix::f_SETFL,posix::o_NONBLOCK));
        observe(kernel,g,flags);
        auto access=Access::acquire(native::syscall_origins,process(kernel,g),thread(kernel,g)); CHECK(access);
        Status failure{}; native::linux_udp::OriginalUdpDescription original{kernel.scheduler(),failure};
        native::linux_udp::ImmediateLifetimeScope lifetime{access.value(),original};
        auto prepared=native::linux_udp_operations.prepare_blocking_receive<profile>(kernel,process(kernel,g),thread(kernel,g),access.value(),original); CHECK(prepared);
        expect(!prepared.value().admission && prepared.value().target_result);
        expect(*prepared.value().target_result==(mode==0?3:mode==2?error(E::e_opnotsupp):error(E::e_again)));
        expect(access.value().live() && !native::linux_udp_receive_object(g.pid,g.pid).pool);
        thread(kernel,g).syscall_pending=false; CHECK(native::set_linux_udp_result(process(kernel,g),thread(kernel,g).native_frame,*prepared.value().target_result));
        CHECK(lifetime.finish()); retire(kernel,g);
    }
    auto g=create(kernel,55104); observe(kernel,g);
    auto access=Access::acquire(native::syscall_origins,process(kernel,g),thread(kernel,g)); CHECK(access);
    Status failure{}; native::linux_udp::OriginalUdpDescription original{kernel.scheduler(),failure};
    native::linux_udp::ImmediateLifetimeScope lifetime{access.value(),original};
    auto prepared=native::linux_udp_operations.prepare_blocking_receive<profile>(kernel,process(kernel,g),thread(kernel,g),access.value(),original); CHECK(prepared);
    CHECK(kernel.posix().fcntl(g.fd,posix::f_SETFL,posix::o_NONBLOCK));
    auto begun=native::begin_linux_udp_receive(kernel,process(kernel,g),thread(kernel,g),access.value(),original,std::move(*prepared.value().admission)); CHECK(begun);
    expect(begun.value().kind==native::LinuxUdpReceiveBeginKind::owned_completed && !begun.value().target_result);
    expect(static_cast<i64>(thread(kernel,g).native_frame.registers[14])==error(E::e_again)); CHECK(lifetime.finish()); retire(kernel,g);
    g=create(kernel,55105); observe(kernel,g);
    auto stale_access=Access::acquire(native::syscall_origins,process(kernel,g),thread(kernel,g)); CHECK(stale_access);
    native::linux_udp::OriginalUdpDescription stale_original{kernel.scheduler(),failure};
    native::linux_udp::ImmediateLifetimeScope stale_lifetime{stale_access.value(),stale_original};
    auto stale_prepared=native::linux_udp_operations.prepare_blocking_receive<profile>(kernel,process(kernel,g),thread(kernel,g),stale_access.value(),stale_original); CHECK(stale_prepared);
    Admission moved{std::move(*stale_prepared.value().admission)};
    auto invalid=native::begin_linux_udp_receive(kernel,process(kernel,g),thread(kernel,g),stale_access.value(),stale_original,std::move(*stale_prepared.value().admission));
    expect(!invalid && !native::started && stale_access.value().live() && stale_original.held());
    expect(!native::linux_udp_receive_object(g.pid,g.pid).pool && !thread(kernel,g).wait_registration.active());
    CHECK(stale_lifetime.finish()); retire(kernel,g);
}
void admission_error_order(Kernel &kernel)
{
    for (unsigned mode=0;mode<5;++mode)
    {
        auto g=create(kernel,static_cast<u16>(55200+mode));
        const bool closed=mode==4;
        if (closed) CHECK(kernel.posix().close(g.fd));
        else packet(kernel,g);
        auto request=g;
        if (mode<3) request.fd=-1;
        const u32 flags=mode==1 || mode>=3 ? 1 : 0;
        // All request words are constructed before genuine origin observation.
        observe(kernel,request,flags,false,8,mode==2 ? hw::user_end+1 : output);
        auto &p=process(kernel,g); auto &t=thread(kernel,g);
        auto access=Access::acquire(native::syscall_origins,p,t); CHECK(access);
        const auto frame=t.native_frame;
        const auto ticket=t.wait_registration.ticket();
        const auto completion=t.wait_registration.completion();
        const auto wait_reason=t.wait_reason; const auto wait_key=t.wait_key;
        const auto lease=t.entry_stack; const auto stack=t.kernel_stack_top;
        const auto owner_accounting=native::linux_udp_receive_objects.accounting();
        const auto members=native::linux_udp_receive_waits;
        const auto entries=mock::entries, copies=mock::copies, writes=mock::outputs;
        const auto stacks=native::entry_stacks.accounting();
        const auto active_sockets=kernel.native_udp().active_sockets();
        const auto raw_call=access.value().raw_syscall();
        const auto operation=access.value().operation();
        Status failure{}; native::linux_udp::OriginalUdpDescription original{kernel.scheduler(),failure};
        native::linux_udp::ImmediateLifetimeScope lifetime{access.value(),original};
        auto prepared=native::linux_udp_operations.prepare_blocking_receive<profile>(kernel,p,t,access.value(),original); CHECK(prepared);
        const i64 expected=mode==2 ? error(E::e_fault) : mode==3 ? error(E::e_opnotsupp) : error(E::e_badf);
        expect(!prepared.value().admission && prepared.value().target_result==expected);
        expect(access.value().live() && access.value().raw_syscall()==raw_call && access.value().operation()==operation);
        expect(!Access::acquire(native::syscall_origins,p,t));
        expect(t.syscall_pending && native::SyscallOrigins::same_frame(frame,t.native_frame));
        expect(!t.wait_registration.active() && t.wait_registration.ticket()==ticket &&
            t.wait_registration.completion()==completion && t.wait_reason==wait_reason && t.wait_key==wait_key);
        expect(t.entry_stack==lease && t.kernel_stack_top==stack && mock::entries==entries &&
            mock::copies==copies && mock::outputs==writes);
        const auto after=native::linux_udp_receive_objects.accounting();
        expect(after.live_objects==owner_accounting.live_objects && after.backing_pages==owner_accounting.backing_pages &&
            after.quarantined_pages==owner_accounting.quarantined_pages && after.empty_pages==owner_accounting.empty_pages &&
            after.peak_objects==owner_accounting.peak_objects && after.allocations==owner_accounting.allocations &&
            after.destructions==owner_accounting.destructions);
        for (usize i=0;i<members.size();++i)
            expect(members[i].pid==native::linux_udp_receive_waits[i].pid &&
                members[i].tid==native::linux_udp_receive_waits[i].tid &&
                members[i].object==native::linux_udp_receive_waits[i].object);
        const auto after_stacks=native::entry_stacks.accounting();
        expect(after_stacks.leased_slots==stacks.leased_slots && after_stacks.active_slots==stacks.active_slots &&
            after_stacks.quarantined_slots==stacks.quarantined_slots && after_stacks.peak_slots==stacks.peak_slots &&
            after_stacks.backing_bytes==stacks.backing_bytes && after_stacks.leased_usable_bytes==stacks.leased_usable_bytes &&
            after_stacks.reservations==stacks.reservations && after_stacks.releases==stacks.releases &&
            after_stacks.activations==stacks.activations);
        expect(kernel.native_udp().active_sockets()==active_sockets);
        expect(original.held()==(mode==3));
        if (closed) expect(!kernel.scheduler().open_description(g.description));
        else
        {
            expect(kernel.scheduler().open_description(g.description)->references==(mode==3 ? 2 : 1));
            const auto queued=kernel.native_udp().peek(g.endpoint); CHECK(queued);
            expect(queued.value().payload.size()==bytes.size());
        }
        expect(native::runtime_failure.ok() && native::started && failure.ok());
        // Follow the ordinary publisher while the original immediate capture lives.
        CHECK(native::set_linux_udp_result(p,t.native_frame,*prepared.value().target_result));
        t.syscall_pending=false;
        auto expected_frame=frame; expected_frame.registers[14]=static_cast<uptr>(expected);
        expect(!t.syscall_pending && native::SyscallOrigins::same_frame(expected_frame,t.native_frame));
        CHECK(lifetime.finish());
        expect(!access.value().live() && !original.held() && failure.ok() && native::runtime_failure.ok() && native::started);
        expect(!Access::acquire(native::syscall_origins,p,t) && !native::linux_udp_receive_object(g.pid,g.pid).pool);
        if (!closed) expect(kernel.scheduler().open_description(g.description)->references==1);
        CHECK(lifetime.finish());
        retire(kernel,g);
    }
}
void failed_rearm_exact_ticket(Kernel &kernel)
{
    auto g=create(kernel,55110); auto object=arm(kernel,g); auto resolved=native::linux_udp_receive_objects.resolve(object); CHECK(resolved);
    auto &owned=*resolved.value(); const auto original=*owned.binding;
    CHECK(kernel.scheduler().complete_wait(owned.ticket,sched::WaitCompletion::ready)); select(kernel,g);
    auto registration=kernel.scheduler().register_current_wait(sched::ThreadWaitReason::udp_receive,g.endpoint); CHECK(registration);
    owned.ticket=registration.value(); auto changed=original; changed.ticket=owned.ticket; ++changed.operation.count;
    expect(!native::syscall_origins.rearm_linux_udp_receive(*owned.capture,thread(kernel,g).native_frame,original,changed));
    const auto newer=owned.ticket; expect(newer.generation>original.ticket.generation);
    CHECK(native::retire_linux_udp_receive(kernel,object,native::LinuxUdpReceiveRetirement::rollback));
    expect(!thread(kernel,g).wait_registration.active() && thread(kernel,g).wait_registration.ticket()==newer &&
        thread(kernel,g).wait_registration.completion()==sched::WaitCompletion::cancelled);
    expect(kernel.scheduler().open_description(g.description)->references==1 && !native::linux_udp_receive_object(g.pid,g.pid).pool);
    retire(kernel,g);
}
void borrow_capacity(Kernel &kernel)
{
    auto g=create(kernel,55111); auto object=arm(kernel,g); auto resolved=native::linux_udp_receive_objects.resolve(object); CHECK(resolved);
    auto &owned=*resolved.value(); CHECK(kernel.scheduler().complete_wait(owned.ticket,sched::WaitCompletion::ready)); select(kernel,g);
    std::vector<Borrow> borrowed; borrowed.reserve(65535);
    for(usize i=0;i<65535;++i)
    {
        auto acquired=native::syscall_origins.acquire_linux_udp_receive_access(*owned.capture,*owned.binding); CHECK(acquired);
        borrowed.emplace_back(std::move(acquired.value()));
    }
    expect(!native::syscall_origins.acquire_linux_udp_receive_access(*owned.capture,*owned.binding));
    expect(native::syscall_origins.linux_udp_receive_borrowed(*owned.capture)); borrowed.clear();
    expect(!native::syscall_origins.linux_udp_receive_borrowed(*owned.capture));
    CHECK(native::retire_linux_udp_receive(kernel,object,native::LinuxUdpReceiveRetirement::rollback)); retire(kernel,g);
}
void exact_retirement(Kernel &kernel)
{
    auto g=create(kernel,55090); auto object=arm(kernel,g);
    auto owned=native::linux_udp_receive_objects.resolve(object); CHECK(owned); const auto ticket=owned.value()->ticket;
    CHECK(native::retire_linux_udp_receive(kernel,object,native::LinuxUdpReceiveRetirement::exec));
    expect(!thread(kernel,g).wait_registration.active() && thread(kernel,g).wait_registration.ticket()==ticket);
    observe(kernel,g); auto access=Access::acquire(native::syscall_origins,process(kernel,g),thread(kernel,g)); CHECK(access); access.value().reset();
    auto newer=arm(kernel,g); expect(newer.generation>object.generation); CHECK(native::retire_linux_udp_receive(kernel,object,native::LinuxUdpReceiveRetirement::rollback));
    expect(native::linux_udp_receive_object(g.pid,g.pid)==newer);
    CHECK(kernel.scheduler().complete_wait(thread(kernel,g).wait_registration.ticket(),sched::WaitCompletion::interrupted));
    expect(!native::scan_linux_udp_receives(kernel) && !native::started); expect(thread(kernel,g).syscall_pending);
    expect(!native::linux_udp_receive_object(g.pid,g.pid).pool); retire(kernel,g);
}
}
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(kernel.native_udp().configure(udp_link)); CHECK(peer.configure({udp_link.peer_mac,udp_link.local_mac,udp_link.peer_ip,udp_link.local_ip}));
    auto opened=peer.open(); CHECK(opened); sender=opened.value();
    normal_and_rearm(kernel); receive_order(kernel); original_reuse(kernel); staged_change_and_pressure(kernel);
    borrow_failure_and_publication(kernel); admission_immediate_and_changed_flags(kernel); admission_error_order(kernel);
    failed_rearm_exact_ticket(kernel); borrow_capacity(kernel); exact_retirement(kernel);
    expect(native::entry_stacks.accounting().leased_slots==0 && kernel.native_udp().active_sockets()==0);
    const auto owners=native::linux_udp_receive_objects.accounting(); expect(owners.allocations==owners.destructions && owners.live_objects==0);
    CHECK(peer.close(sender));
    std::printf("LINUX_UDP_RECEIVE_OWNERSHIP_MODEL_PASS checks=%zu cases=%zu entries=%zu owners=%llu hosted=1 profile=x86_64 guest=0 actual_nic=0\n",
        checks,cases,captured_entries,static_cast<unsigned long long>(owners.allocations));
}
