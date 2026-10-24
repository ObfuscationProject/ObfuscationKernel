// Actual scheduler and directed admission; hosted callback, not guest execution.
#include "native_clock_model.hpp"
#include "ok/user/native_signal_target.hpp"

namespace
{
using user::Credentials;
using user::directed_signal_permitted;
using user::find_directed_signal_target;
unsigned profiles{}, deliveries{}, permissions{};

template <lu::Profile P>
void queues(Kernel &kernel,sched::ProcessId destination)
{
    using Codec=user::native::SignalActionCodec<P>;
    using Queue=sched::PendingSignals<Codec::max_signal,2>;
    auto *recipient=kernel.scheduler().find(destination);
    auto *thread=kernel.scheduler().find_thread(destination,destination);CHECK(recipient && thread);
    Queue queue;CHECK(queue.reset({destination,destination,1}));
    sched::SignalActionTable actions{};unsigned acquisitions=0;
    auto tables=[&](sched::ProcessId id) { CHECK(id==destination);return &actions; };
    auto pending=[&](sched::ThreadControlBlock &target) {
        CHECK(target.owner==destination && target.tid==destination);++acquisitions;return &queue;
    };
    auto send=[&](sched::PendingSignalInfo info) {
        return user::enqueue_directed_signal<P>(*recipient,*thread,info,tables,pending);
    };
    const sched::PendingSignalInfo first{10,-6,101,102},second{10,-6,103,104};
    actions.find(10)->handler=sched::SignalAction::ignored_handler;
    CHECK(send(first)==0 && acquisitions==0 && !queue.pending_mask().contains(10));
    thread->native_signal_mask.set(10);
    CHECK(send(first)==0 && acquisitions==1 && queue.pending_mask().contains(10));
    typename Queue::Delivery item{};CHECK(!queue.peek(thread->native_signal_mask,item));
    actions.find(10)->handler=0x40001000;thread->native_signal_mask.set(10,false);
    CHECK(send(second)==0 && queue.peek({},item) && item.info==first);
    const auto stale=item;CHECK(queue.consume(item) && !queue.peek({},item));
    const sched::PendingSignalInfo realtime1{64,-6,105,106},realtime2{64,-6,107,108};
    CHECK(send(realtime1)==0 && send(realtime2)==0 && queue.realtime_size()==2);
    CHECK(send({64,-6,109,110})==-11 && queue.realtime_size()==2);
    CHECK(queue.peek({},item) && item.info==realtime1 && queue.consume(item));
    CHECK(queue.peek({},item) && item.info==realtime2 && queue.consume(item));
    CHECK(queue.reset({destination,destination,2}) && send(second)==0 && !queue.consume(stale));
    CHECK(queue.peek({},item) && item.info==second && item.owner.generation==2);
    auto absent_actions=[](sched::ProcessId)->sched::SignalActionTable * { return nullptr; };
    auto absent_pending=[](sched::ThreadControlBlock &)->Queue * { return nullptr; };
    CHECK(user::enqueue_directed_signal<P>(*recipient,*thread,first,absent_actions,pending)==-12);
    CHECK(user::enqueue_directed_signal<P>(*recipient,*thread,first,tables,absent_pending)==-12);
    CHECK(queue.peek({},item) && item.info==second);
    sched::SignalMask available{},blocked{};
    for(u32 signal:{1U,10U,32U,33U,64U,65U,128U})available.set(signal);
    for(u32 signal:{10U,33U,64U,65U,128U})blocked.set(signal);
    const auto available_before=available,blocked_before=blocked;
    constexpr usize bytes=user::native::SignalMaskCodec<P>::bytes;
    std::array<std::byte,bytes> expected{};
    for(u32 signal:{10U,33U,64U,65U,128U})if(signal<=Codec::max_signal)
    {
        constexpr usize word=lu::Codec<P>::profile.word_bytes;
        const usize bit=signal-1,slot=bit/(word*8),byte=(bit%(word*8))/8;
        expected[slot*word+(lu::Codec<P>::profile.big_endian?word-1-byte:byte)]|=std::byte{static_cast<u8>(1U<<(bit%8))};
    }
    unsigned copies=0;
    auto write=[&](u64 address,std::span<const std::byte> data) {
        ++copies;CHECK(address==0x1000 && !data.empty() && data.size()<=bytes);
        for(usize i=0;i<data.size();++i)CHECK(data[i]==expected[i]);
        return true;
    };
    for(usize size=0;size<=bytes;++size)
        CHECK(user::query_directed_signal_pending<P>(available,blocked,0x1000,size,write)==0);
    CHECK(copies==bytes && available==available_before && blocked==blocked_before);
    CHECK(user::query_directed_signal_pending<P>(available,blocked,0,0,write)==0);
    CHECK(user::query_directed_signal_pending<P>(available,blocked,0,bytes+1,write)==-22 && copies==bytes);
    CHECK(user::query_directed_signal_pending<P>(available,blocked,0,bytes,
        [](u64 address,std::span<const std::byte> data){CHECK(!address && !data.empty());return false;})==-14);
}

void policy()
{
    const Credentials from{.uid=101,.gid=0,.euid=102,.egid=0,.suid=103};
    const Credentials to{.uid=201,.euid=202,.suid=203};
    auto expect=[&](Credentials a,Credentials b,bool group,bool allowed) {
        CHECK(directed_signal_permitted(a,b,group)==allowed);++permissions;
    };
    expect(from,to,false,false);expect(from,to,true,true);
    for(bool effective:{false,true})for(bool saved:{false,true})
    {
        auto target=to;(saved?target.suid:target.uid)=effective?from.euid:from.uid;
        expect(from,target,false,true);
    }
    auto target=to;target.euid=from.uid;expect(from,target,false,false);
    target.euid=from.euid;expect(from,target,false,false);
    target=to;target.uid=target.suid=from.suid;expect(from,target,false,false);
    auto sender=from;sender.euid=0;expect(sender,to,false,true);
    sender=from;sender.uid=0;expect(sender,to,false,false);
    sender=from;sender.kernel_space=true;expect(sender,to,false,false);
    sender.uid=0xfffffffeU;target=to;target.suid=sender.uid;expect(sender,target,false,true);
}

template <lu::Profile P>
void admission(Kernel &kernel,sched::ProcessId source,sched::ProcessId destination)
{
    using Codec=user::native::SignalActionCodec<P>;
    auto &scheduler=kernel.scheduler();
    const Credentials from{.uid=101,.euid=102,.suid=0},to{.uid=201,.euid=202,.suid=101};
    auto *sender=scheduler.find(source),*recipient=scheduler.find(destination);
    sender->set_credentials(from);recipient->set_credentials(to);
    auto *thread=scheduler.find_thread(destination,destination);CHECK(thread);
    thread->native_active=true;thread->state=sched::ProcessState::runnable;
    recipient->resources().personality=sched::UserPersonality::linux;
    const auto mask=thread->native_signal_mask;sched::PendingSignalInfo sent{};
    auto deliver=[&](auto &process,auto &task,sched::PendingSignalInfo info)->i64 {
        CHECK(process.pid()==destination && task.owner==destination && task.tid==destination);
        ++deliveries;sent=info;return 0;
    };
    auto call=[&](bool tkill,u64 group,u64 tid,u64 signal) {
        std::array<u64,6> a{};
        if(tkill){a[0]=tid;a[1]=signal;}else{a[0]=group;a[1]=tid;a[2]=signal;}
        return user::send_directed_signal<P>(scheduler,*sender,tkill,a,deliver);
    };
    const unsigned initial=deliveries;const u64 invalid=Codec::max_signal+1;
    CHECK(call(true,0,0,0)==-22 && call(false,0,destination,0)==-22);
    CHECK(call(true,0,0xffffffffU,0)==-22 && call(false,0xffffffffU,destination,0)==-22);
    CHECK(call(true,0,sched::max_task_id,invalid)==-3 && call(false,source,destination,invalid)==-3);
    CHECK(call(true,0,destination,invalid)==-22 && call(true,0,destination,0xffffffffU)==-22);
    CHECK(call(false,destination,destination,0)==0 && call(true,0,destination,0)==0);
    auto denied=to;denied.suid=203;recipient->set_credentials(denied);
    CHECK(call(true,0,destination,invalid)==-22 && call(true,0,destination,0)==-1);
    CHECK(call(false,destination,destination,10)==-1);recipient->set_credentials(to);
    thread->native_active=false;
    CHECK(call(true,0,destination,0)==0 && call(true,0,destination,10)==-95);
    thread->native_active=true;thread->state=sched::ProcessState::blocked;
    CHECK(call(true,0,destination,0)==0 && call(true,0,destination,10)==-95);
    thread->state=sched::ProcessState::runnable;
    for(u32 stop:{Codec::stop_signal,Codec::mips?24U:20U,Codec::mips?26U:21U,
                 Codec::mips?27U:22U,Codec::mips?25U:18U})CHECK(call(true,0,destination,stop)==-95);
    CHECK(deliveries==initial);
    const u64 high=u64{0x12345678}<<32;
    CHECK(call(false,high|destination,high|destination,high)==0);
    CHECK(call(true,0,high|destination,high|10)==0);
    CHECK(sent.signal==10 && sent.code==-6 && sent.sender_pid==source && sent.sender_uid==101);
    const auto snapshot=sent;sender->set_credentials(user::root_credentials());
    CHECK(sent==snapshot);sender->set_credentials(from);
    CHECK(call(false,destination,destination,Codec::max_signal)==0);
    CHECK(sent.signal==Codec::max_signal && deliveries==initial+2 && thread->native_signal_mask==mask);
    queues<P>(kernel,destination);
    ++profiles;
}

void lifecycle(Kernel &kernel,sched::ProcessId source,sched::ProcessId destination,arch::CpuContext context)
{
    auto &scheduler=kernel.scheduler();CHECK(!find_directed_signal_target(scheduler,0,0));
    auto peer=scheduler.create_thread(destination,context);CHECK(peer);
    auto found=find_directed_signal_target(scheduler,0,peer.value());
    CHECK(found && found.process->pid()==destination && found.thread->tid==peer.value());
    CHECK(!find_directed_signal_target(scheduler,source,peer.value()));
    CHECK(scheduler.exit_thread(destination,peer.value()));
    CHECK(!find_directed_signal_target(scheduler,0,peer.value()));
    auto replacement=scheduler.create_thread(destination,context);CHECK(replacement && replacement.value()!=peer.value());
    CHECK(!find_directed_signal_target(scheduler,0,peer.value()));
    CHECK(find_directed_signal_target(scheduler,destination,replacement.value()));
    const auto prepared=scheduler.create_thread_prepared(destination,context,[&](auto &unpublished) {
        CHECK(!find_directed_signal_target(scheduler,0,unpublished.tid));
        return Status::invalid_argument("intentional preparation rejection");
    });CHECK(!prepared);
    auto *leader=scheduler.find_thread(destination,destination);CHECK(leader);
    leader->native_active=false;CHECK(scheduler.exit_thread(destination,destination));
    found=find_directed_signal_target(scheduler,0,destination);CHECK(found && found.retired_leader);
    auto *sender=scheduler.find(source);sender->set_credentials(user::root_credentials());
    const auto reject=[](auto &,auto &,sched::PendingSignalInfo)->i64 { CHECK(false);return -12; };
    CHECK(user::send_directed_signal<lu::Profile::x86_64>(scheduler,*sender,true,{destination,0},reject)==0);
    CHECK(user::send_directed_signal<lu::Profile::x86_64>(scheduler,*sender,true,{destination,10},reject)==0);
    CHECK(scheduler.exit_process(destination,0));
    CHECK(find_directed_signal_target(scheduler,0,destination).retired_leader);
    CHECK(!find_directed_signal_target(scheduler,0,replacement.value()));
    auto later=scheduler.create_user_process("directed-later",context);CHECK(later);
    CHECK(scheduler.reap_process(destination));CHECK(!find_directed_signal_target(scheduler,0,destination));
    found=find_directed_signal_target(scheduler,0,later.value());
    CHECK(found && found.process->pid()==later.value() && found.thread->owner==later.value());
    CHECK(user::send_directed_signal<lu::Profile::x86_64>(scheduler,*scheduler.find(source),true,
                                                       {destination,10},reject)==-3);
}
}

int main()
{
    policy();static Kernel kernel;CHECK(kernel.boot({}));
    auto context=kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096});
    auto source=kernel.scheduler().create_user_process("directed-source",context);CHECK(source);
    auto destination=kernel.scheduler().create_user_process("directed-target",context);CHECK(destination);
    CHECK(kernel.posix().activate(source.value()));
    admission<lu::Profile::x86_64>(kernel,source.value(),destination.value());
    admission<lu::Profile::i386>(kernel,source.value(),destination.value());
    admission<lu::Profile::aarch64>(kernel,source.value(),destination.value());
    admission<lu::Profile::arm32>(kernel,source.value(),destination.value());
    admission<lu::Profile::rv64>(kernel,source.value(),destination.value());
    admission<lu::Profile::rv32>(kernel,source.value(),destination.value());
    admission<lu::Profile::loongarch64>(kernel,source.value(),destination.value());
    admission<lu::Profile::mips>(kernel,source.value(),destination.value());
    admission<lu::Profile::mips64>(kernel,source.value(),destination.value());
    admission<lu::Profile::ppc>(kernel,source.value(),destination.value());
    lifecycle(kernel,source.value(),destination.value(),context);
    CHECK(profiles==10 && permissions==13 && deliveries==20);
    std::puts("DIRECTED_SIGNAL_MODEL_PASS profiles=10 permission_vectors=13 callback_deliveries=20 lifecycle=1 guest=0");
}
