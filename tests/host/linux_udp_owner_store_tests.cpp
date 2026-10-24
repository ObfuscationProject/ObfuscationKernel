// Production store, genuine shared capture/admission and actual scheduler/OFD.
// Synthetic x86_64 traps and concrete hosted page callbacks are model controls.
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_udp_receive_model.hpp"
#include <source_location>
namespace
{
constexpr auto profile=lu::Profile::x86_64;
using Origins=native::SyscallOriginTable<profile>;
using Access=native::LinuxUdpImmediateAccess<profile>;
using Admission=native::LinuxUdpReceiveAdmission<profile>;
using C=lu::UdpConstants<profile>;
constexpr uptr output=hw::user_begin+128;
constexpr net::NativeUdpLink network_link{{2,1,2,3,4,5},{2,6,7,8,9,10},{192,0,2,2},{192,0,2,1}};
usize checks{},cases{},captures{},callbacks{};
void expect(bool value,std::source_location at=std::source_location::current())
{
    ++checks;
    if(!value) { std::fprintf(stderr,"LINUX_UDP_STORE_FAIL line=%u checks=%zu\n",at.line(),checks); std::abort(); }
}
struct Provider
{
    static constexpr usize page_size=4096,capacity=16;
    struct Lease { std::span<std::byte> bytes{}; const Provider *owner{}; usize index{}; u64 generation{}; };
    alignas(page_size) std::array<std::array<std::byte,page_size>,capacity> backing{};
    std::array<u64,capacity> generations{};
    u64 sequence{};
    usize attempts{},allocations{},releases{},release_failures{};
    bool fail_release{},short_page{};
    void (*hook)(void *){};
    void *context{};
    Result<Lease> allocate_kernel_page()
    {
        ++attempts;
        if(auto callback=std::exchange(hook,nullptr)) { ++callbacks; callback(context); }
        for(usize index=0;index<capacity;++index) if(!generations[index])
        {
            const auto generation=++sequence; generations[index]=generation; ++allocations;
            return Lease{{backing[index].data(),short_page?page_size-1:page_size},this,index,generation};
        }
        return Status::no_memory("hosted store page budget exhausted");
    }
    Status release_kernel_page(Lease lease)
    {
        ++releases;
        if(fail_release) { ++release_failures; return Status::io_error("hosted real page release refusal"); }
        if(lease.owner!=this || lease.index>=capacity || !lease.generation || generations[lease.index]!=lease.generation)
            return Status::denied("hosted store foreign lease");
        generations[lease.index]=0; return {};
    }
};
using Store=native::LinuxUdpReceiveOwnerStore<profile,Provider>;
static_assert(memory::ObjectPageProvider<Provider> && Store::pages<=Provider::capacity);
static_assert(!std::is_copy_constructible_v<Store> && !std::is_move_constructible_v<Store>);
template<class T> concept Resettable=requires(T &store) { store.reset(); };
static_assert(!Resettable<Store>);
[[maybe_unused]] Origins static_issuer{native::entry_stacks};
[[maybe_unused]] Provider static_provider;
[[maybe_unused]] constinit Store static_store{static_issuer,static_provider};
[[maybe_unused]] constinit auto &static_objects=static_store.objects();
[[maybe_unused]] constinit auto &static_members=static_store.members();
struct Group { sched::ProcessId pid{}; i32 fd{}; u16 description{}; u32 endpoint{}; };
auto &process(Kernel &kernel,Group group) { return *kernel.scheduler().find(group.pid); }
auto &thread(Kernel &kernel,Group group) { return *kernel.scheduler().find_thread(group.pid,group.pid); }
Group create(Kernel &kernel)
{
    const auto made=kernel.scheduler().create_user_process("linux-udp-owner-store-host",
        kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096})); CHECK(made);
    Group group{made.value()}; auto &p=process(kernel,group); auto &t=thread(kernel,group);
    p.resources().personality=sched::UserPersonality::linux;
    p.resources().native_space={0x100000+group.pid*4096,static_cast<u32>(group.pid%16),1};
    CHECK(native::prepare_native_thread(t)); t.native_active=true;
    CHECK(kernel.scheduler().set_runnable(group.pid)); CHECK(kernel.scheduler().activate_thread(group.pid,group.pid));
    CHECK(kernel.posix().activate(group.pid));
    const auto opened=kernel.native_udp().open(); CHECK(opened); group.endpoint=opened.value();
    CHECK(kernel.native_udp().bind(group.endpoint,{},static_cast<u16>(58000+cases++)));
    const auto installed=kernel.posix().install_native_udp(group.endpoint,false,false); CHECK(installed); group.fd=installed.value();
    group.description=p.fd_table().entry(group.fd)->description;
    native::runtime_kernel=&kernel; native::started=true; CHECK(native::runtime_status()); return group;
}
void observe(Kernel &kernel,Group group,Origins &issuer)
{
    auto &p=process(kernel,group); auto &t=thread(kernel,group);
    CHECK(issuer.prepare(t));
    hw::UserFrame frame{}; frame.pc=hw::user_begin+256; frame.sp=hw::user_begin+4096;
    frame.registers[14]=*C::syscall_number(lu::UdpCall::recvfrom);
    frame.registers[9]=group.fd; frame.registers[10]=output; frame.registers[11]=8;
    t.native_frame=frame; t.syscall_pending=false;
    const auto active=native::entry_stacks.activate_entry(t.entry_stack,{group.pid,group.pid},0); CHECK(active);
    mock::entry_trap={hw::TrapKind::syscall,256};
    CHECK(native::shared_capture.enter(issuer,native::vfs_continuations,p,t,active.value(),kernel.scheduler()));
    CHECK(native::entry_stacks.deactivate_entry(t.entry_stack,{group.pid,group.pid},0));
    t.syscall_pending=true; ++captures;
}
void release(Kernel &kernel,Group group,Origins &issuer)
{
    auto &t=thread(kernel,group); issuer.revoke(t); t.syscall_pending=false;
    native::release_process(kernel,group.pid); CHECK(native::runtime_status());
    CHECK(kernel.scheduler().exit_process(group.pid,0)); CHECK(kernel.scheduler().reap_orphans());
    expect(!kernel.native_udp().binding(group.endpoint));
}
void valid_and_foreign(Kernel &kernel,bool foreign)
{
    auto group=create(kernel); Origins issuer{native::entry_stacks},other{native::entry_stacks};
    Provider provider; Store store{issuer,provider}; auto &origin=foreign?other:issuer;
    observe(kernel,group,origin); auto &p=process(kernel,group); auto &t=thread(kernel,group);
    {
        auto access=Access::acquire(origin,p,t); CHECK(access);
        Status failure{}; native::linux_udp::OriginalUdpDescription original{kernel.scheduler(),failure};
        native::linux_udp::ImmediateLifetimeScope lifetime{access.value(),original};
        native::linux_udp::OperationScratch scratch;
        auto prepared=scratch.prepare_blocking_receive<profile>(kernel,p,t,access.value(),original); CHECK(prepared);
        expect(prepared.value().admission.has_value() && !prepared.value().target_result);
        expect(access.value().live() && original.held() && kernel.scheduler().open_description(group.description)->references==2);
        auto staged=store.stage(kernel,p,t,access.value(),original,*prepared.value().admission);
        if(foreign)
        {
            expect(!staged.ok() && staged.status.code()==StatusCode::denied && !staged.contained);
            expect(provider.attempts==0 && store.objects().accounting().live_objects==0);
            expect(access.value().live() && original.held());
            for(const auto &member:store.members()) expect(!member.pid);
        }
        else
        {
            expect(staged.ok() && staged.object.pool && !staged.contained);
            expect(!access.value().live() && !original.held());
            const auto owned=store.objects().resolve(staged.object); CHECK(owned); auto &owner=*owned.value();
            expect(owner.phase==native::LinuxUdpReceivePhase::captured_staged && owner.capture && owner.binding &&
                !owner.ticket.generation && owner.retained_description==group.description);
            expect(store.member(staged.object)->pid==group.pid && store.member(staged.object)->reservation==1);
            expect(!store.stage(kernel,p,t,access.value(),original,*prepared.value().admission).ok());
            CHECK(issuer.retire_linux_udp_receive(*owner.capture,*owner.binding));
            CHECK(kernel.scheduler().release_description(owner.retained_description));
            owner.retained_description=sched::invalid_description; *store.member(staged.object)={};
            CHECK(store.objects().destroy(staged.object)); CHECK(store.objects().trim_empty_pages());
            expect(provider.allocations==1 && provider.releases==1);
        }
        CHECK(lifetime.finish()); expect(failure.ok());
        expect(kernel.scheduler().open_description(group.description)->references==1);
    }
    release(kernel,group,origin);
}
enum class Mutation { revoke,image,same_owner_aba,replacement };
void callback_refusal(Kernel &kernel,Mutation mutation,bool cleanup_failure)
{
    auto group=create(kernel); Origins issuer{native::entry_stacks}; Provider provider; Store store{issuer,provider};
    observe(kernel,group,issuer); auto &p=process(kernel,group); auto &t=thread(kernel,group);
    {
        auto access=Access::acquire(issuer,p,t); CHECK(access);
        Status failure{}; native::linux_udp::OriginalUdpDescription original{kernel.scheduler(),failure};
        native::linux_udp::ImmediateLifetimeScope lifetime{access.value(),original};
        native::linux_udp::OperationScratch scratch;
        auto prepared=scratch.prepare_blocking_receive<profile>(kernel,p,t,access.value(),original); CHECK(prepared);
        expect(prepared.value().admission.has_value());
        struct Context { Kernel &kernel; Group group; Origins &issuer; Store &store; Access &access;
            native::linux_udp::OriginalUdpDescription &original; Admission &admission; Mutation mutation; bool nested{}; };
        Context context{kernel,group,issuer,store,access.value(),original,*prepared.value().admission,mutation};
        provider.context=&context; provider.fail_release=cleanup_failure;
        provider.hook=[](void *raw)
        {
            auto &context=*static_cast<Context *>(raw); auto &p=process(context.kernel,context.group); auto &t=thread(context.kernel,context.group);
            if(context.mutation==Mutation::revoke) context.issuer.revoke(t);
            else if(context.mutation==Mutation::image) ++p.resources().native_space.generation;
            else
            {
                const auto old=context.store.members()[0]; expect(old.pid==p.pid() && old.tid==t.tid && old.reservation==1 && !old.object.pool);
                context.store.members()[0]={};
                // Genuine nested stage reserves the same owner with a new
                // generation; the mutating object pool refuses its allocation.
                const auto nested=context.store.stage(context.kernel,p,t,context.access,context.original,context.admission);
                expect(!nested.ok() && nested.status.code()==StatusCode::busy && !nested.contained); context.nested=true;
                if(context.mutation==Mutation::same_owner_aba)
                    context.store.members()[0]=old;
                else
                    context.store.members()[0]={old.pid,old.tid,{},old.reservation+1};
                // Mutable membership is an inspection fault control. It never
                // creates a capture, admission, object or published owner.
            }
        };
        const auto before=t.native_frame;
        const auto staged=store.stage(kernel,p,t,access.value(),original,*prepared.value().admission);
        expect(!staged.ok() && !staged.object.pool && staged.contained==cleanup_failure);
        expect(staged.status.code()==((mutation==Mutation::same_owner_aba || mutation==Mutation::replacement)?StatusCode::fault:StatusCode::denied));
        expect(original.held() && kernel.scheduler().open_description(group.description)->references==2);
        expect(Origins::same_frame(before,t.native_frame) && !t.wait_registration.active());
        const auto accounting=store.objects().accounting();
        expect(accounting.live_objects==0 && accounting.allocations==1 && accounting.destructions==1);
        expect(provider.attempts==1 && provider.allocations==1 && provider.releases==1);
        if(context.nested)
        {
            const auto &member=store.members()[0];
            expect(member.pid==group.pid && member.tid==group.pid && !member.object.pool &&
                member.reservation==(mutation==Mutation::same_owner_aba?1U:2U));
        }
        else for(const auto &member:store.members()) expect(!member.pid);
        if(cleanup_failure)
        {
            expect(store.contained() && store.failure().code()==staged.status.code() && store.failure().message()==staged.status.message());
            expect(accounting.quarantined_pages==1 && provider.release_failures==1);
            const auto attempts=provider.attempts,releases=provider.releases;
            const auto retry=store.stage(kernel,p,t,access.value(),original,*prepared.value().admission);
            expect(!retry.ok() && retry.contained && retry.status.message()==staged.status.message());
            expect(provider.attempts==attempts && provider.releases==releases && store.objects().accounting().quarantined_pages==1);
        }
        else expect(!store.contained() && !accounting.quarantined_pages);
        CHECK(lifetime.finish()); expect(failure.ok());
        expect(kernel.scheduler().open_description(group.description)->references==1);
    }
    release(kernel,group,issuer);
}
void invalid_provider_quarantine(Kernel &kernel)
{
    auto group=create(kernel); Origins issuer{native::entry_stacks}; Provider provider; Store store{issuer,provider};
    observe(kernel,group,issuer); auto &p=process(kernel,group); auto &t=thread(kernel,group);
    {
        auto access=Access::acquire(issuer,p,t); CHECK(access);
        Status failure{}; native::linux_udp::OriginalUdpDescription original{kernel.scheduler(),failure};
        native::linux_udp::ImmediateLifetimeScope lifetime{access.value(),original};
        native::linux_udp::OperationScratch scratch;
        auto prepared=scratch.prepare_blocking_receive<profile>(kernel,p,t,access.value(),original); CHECK(prepared);
        CHECK(prepared.value().admission); provider.short_page=true; provider.fail_release=true;
        const auto staged=store.stage(kernel,p,t,access.value(),original,*prepared.value().admission);
        expect(!staged.ok() && staged.contained && staged.status.code()==StatusCode::fault);
        expect(access.value().live() && original.held() && store.contained());
        expect(store.failure().message()==staged.status.message() && provider.release_failures==1);
        expect(store.objects().accounting().quarantined_pages==1 && store.objects().accounting().live_objects==0);
        const auto retry=store.stage(kernel,p,t,access.value(),original,*prepared.value().admission);
        expect(!retry.ok() && retry.contained && provider.attempts==1 && provider.releases==1);
        CHECK(lifetime.finish()); expect(failure.ok());
    }
    release(kernel,group,issuer);
}
}
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(kernel.native_udp().configure(network_link));
    valid_and_foreign(kernel,false); valid_and_foreign(kernel,true);
    for(auto mutation:{Mutation::revoke,Mutation::image,Mutation::same_owner_aba,Mutation::replacement})
        for(bool cleanup_failure:{false,true}) callback_refusal(kernel,mutation,cleanup_failure);
    invalid_provider_quarantine(kernel);
    expect(native::runtime_status().ok() && native::entry_stacks.accounting().leased_slots==0 && kernel.native_udp().active_sockets()==0);
    std::printf("LINUX_UDP_OWNER_STORE_PASS checks=%zu cases=%zu captures=%zu callbacks=%zu hosted=1 profile=x86_64 guest=0 actual_nic=0\n",checks,cases,captures,callbacks);
}
