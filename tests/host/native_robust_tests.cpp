// Actual robust-list adapter and native teardown with hosted mappings only.
#include "native_clock_model.hpp"
namespace
{
constexpr auto profile = lu::configured_native_profile();
using Bridge = lu::NativeBridge<profile>;
using Codec = lu::Codec<profile>;
namespace flags = user::linux_bootstrap::clone_flags;
constexpr bool big = Codec::profile.big_endian;
constexpr bool backwards = profile != lu::Profile::x86_64 && profile != lu::Profile::loongarch64;
constexpr uptr word = hw::user_begin + 128;
void put(usize offset, u32 value)
{
    CHECK(offset <= mock::memory.size() - 4);
    for (usize i=0;i<4;++i) mock::memory[offset+i]=static_cast<std::byte>(value >> (8*(big ? 3-i : i)));
}
u32 get(usize offset)
{
    u32 result=0;
    for(usize i=0;i<4;++i) result|=static_cast<u32>(mock::memory[offset+i]) << (8*(big ? 3-i : i));
    return result;
}
void arguments(hw::UserFrame &frame, lu::Operation op, std::array<u64,5> values, usize count = 5)
{
    const auto number=Codec::syscall_number(op);CHECK(number);
    if constexpr(profile==lu::Profile::x86_64)
    {
        frame.registers[14]=*number;
        constexpr usize slots[]{9,10,11,5,7};
        for(usize i=0;i<count;++i)frame.registers[slots[i]]=values[i];
    }
    else if constexpr(profile==lu::Profile::i386)
    {
        frame.registers[7]=*number;
        constexpr usize slots[]{4,6,5,1,0};
        for(usize i=0;i<count;++i)frame.registers[slots[i]]=values[i];
    }
    else
    {
        frame.registers[Codec::profile.number_register]=*number;
        for(usize i=0;i<count;++i)
        {
            if constexpr(profile==lu::Profile::mips)
                if(i==4) {put(frame.sp-hw::user_begin+16,values[i]);break;}
            frame.registers[Codec::profile.argument_registers[i]]=values[i];
        }
    }
}
i64 invoke(Kernel &kernel,sched::ProcessId pid,hw::UserFrame &frame,bool &complete)
{
    return user::linux_bootstrap::dispatch<profile>(kernel,pid,frame,complete,
        [&](hw::Syscall call,bool &done){return native::dispatch(kernel,pid,call,done);},
        [](sched::ThreadControlBlock &created){return native::prepare_native_thread(created);});
}
sched::ProcessId process(Kernel &kernel)
{
    auto p=kernel.scheduler().create_user_process("native-robust",kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096}));CHECK(p);
    auto *owner=kernel.scheduler().find(p.value());owner->resources().personality=sched::UserPersonality::linux;
    // Shared-domain futex keys resolve through an actual mapping record.
    CHECK(native::allocate(owner->resources().native_pages,hw::user_begin,hw::page_read|hw::page_write|hw::page_execute));
    auto &leader=owner->threads()[0];CHECK(native::prepare_native_thread(leader));
    leader.native_active=true;leader.native_frame.pc=hw::user_begin;
    Bridge::set_stack_pointer(leader.native_frame,hw::user_begin+4096);
    CHECK(kernel.scheduler().set_runnable(p.value()));
    CHECK(kernel.scheduler().activate_thread(p.value(),p.value()));return p.value();
}
sched::ThreadId clone(Kernel &kernel,sched::ProcessId pid,u32 extra,uptr ptid=hw::user_begin+132,uptr ctid=word)
{
    CHECK(kernel.scheduler().activate_thread(pid,pid));
    auto &frame=kernel.scheduler().current_thread()->native_frame;
    frame.fp_state[5]=0xa7;
    uptr tls=hw::user_begin+256;
    if constexpr(profile==lu::Profile::i386)
    {
        put(512,Bridge::i386_tls_entry);put(516,tls);put(520,0xfffff);put(524,0x51);tls=hw::user_begin+512;
    }
    const uptr stack=hw::user_begin+8192-(profile==lu::Profile::x86_64 ? 8 : 0);
    arguments(frame,lu::Operation::clone,{flags::shared_group|extra,stack,ptid,backwards ? tls : ctid,backwards ? ctid : tls});
    bool done=true;const auto result=invoke(kernel,pid,frame,done);CHECK(result>0 && done);
    const auto *child=kernel.scheduler().find_thread(pid,result);CHECK(child && child->native_active && !child->syscall_pending);
    CHECK(child->native_frame.sp==stack && child->native_frame.fp_state[5]==0xa7);
    auto returned=Bridge::decode_return(child->native_frame);CHECK(returned && returned.value().value==0 && returned.value().error==0);
    return result;
}
void exit(Kernel &kernel,sched::ProcessId pid,sched::ThreadId tid,bool group=false)
{
    CHECK(kernel.scheduler().activate_thread(pid,tid));
    bool done=true;CHECK(native::dispatch(kernel,pid,{group ? OK_SYS_EXIT_GROUP : OK_SYS_EXIT,{17}},done)==0 && !done);
}
constexpr usize word_bytes = Codec::profile.word_bytes;
constexpr uptr head = hw::user_begin + 1024, node = hw::user_begin + 1152;
constexpr uptr owner_word = node + 2 * word_bytes;
void put_word(uptr address, u64 value)
{
    for (usize i = 0; i < word_bytes; ++i)
        mock::memory[address - hw::user_begin + i] = static_cast<std::byte>(value >> (8 * (big ? word_bytes - 1 - i : i)));
}
u64 get_word(uptr address)
{
    u64 value{};
    for (usize i = 0; i < word_bytes; ++i)
        value |= static_cast<u64>(mock::memory[address - hw::user_begin + i]) << (8 * (big ? word_bytes - 1 - i : i));
    return value;
}
i64 call(Kernel &kernel, sched::ProcessId pid, lu::Operation op, std::array<u64, 5> values)
{
    auto &frame = kernel.scheduler().current_thread()->native_frame;
    arguments(frame, op, values, op == lu::Operation::set_robust_list ? 2 : 3); bool complete = true;
    const auto result = invoke(kernel, pid, frame, complete); CHECK(complete); return result;
}
void list(u32 tid)
{
    put_word(head, node); put_word(head + word_bytes, 2 * word_bytes); put_word(head + 2 * word_bytes, 0);
    put_word(node, head); put(owner_word - hw::user_begin, tid | 0x80000000U);
}
void registration(Kernel &kernel)
{
    const auto pid = process(kernel); auto *p = kernel.scheduler().find(pid);
    CHECK(call(kernel, pid, lu::Operation::set_robust_list, {1, 3 * word_bytes}) == 0);
    CHECK(call(kernel, pid, lu::Operation::set_robust_list, {head, 3 * word_bytes + 1}) == -22);
    CHECK(p->threads()[0].tls.robust_list == 1);
    const uptr output = hw::user_begin + 1600;
    CHECK(call(kernel, pid, lu::Operation::get_robust_list, {0, output, output + word_bytes}) == 0);
    CHECK(get_word(output) == 1 && get_word(output + word_bytes) == 3 * word_bytes);
    CHECK(call(kernel, pid, lu::Operation::get_robust_list, {pid, output, output}) == 0);
    CHECK(get_word(output) == 1);
    put_word(output, 99); put_word(output + word_bytes, 77); mock::write_forbidden = output;
    CHECK(call(kernel, pid, lu::Operation::get_robust_list, {0, output, output + word_bytes}) == -14);
    CHECK(get_word(output) == 99 && get_word(output + word_bytes) == 3 * word_bytes);
    put_word(output + word_bytes, 77);
    CHECK(call(kernel, pid, lu::Operation::get_robust_list, {0, output + word_bytes, output}) == -14);
    CHECK(get_word(output + word_bytes) == 77); mock::write_forbidden = 0;
    const auto peer = clone(kernel, pid, 0);
    CHECK(kernel.scheduler().find_thread(pid, peer)->tls.robust_list == 0);
    CHECK(call(kernel, pid, lu::Operation::get_robust_list, {peer, output, output + word_bytes}) == 0);
    CHECK(get_word(output) == 0);
    const auto outside = process(kernel);
    CHECK(kernel.scheduler().activate_thread(pid, pid));
    const auto outputs = mock::outputs;
    CHECK(call(kernel, pid, lu::Operation::get_robust_list, {outside, output, output + word_bytes}) == -1);
    CHECK(call(kernel, pid, lu::Operation::get_robust_list, {0xffffffffU, output, output + word_bytes}) == -3);
    CHECK(call(kernel, pid, lu::Operation::get_robust_list, {999999, output, output + word_bytes}) == -3);
    CHECK(mock::outputs == outputs);
    exit(kernel, pid, peer);
    CHECK(kernel.scheduler().activate_thread(pid, pid));
    CHECK(call(kernel, pid, lu::Operation::get_robust_list, {peer, output, output + word_bytes}) == -3);
    CHECK(call(kernel, pid, lu::Operation::set_robust_list, {0, 3 * word_bytes}) == 0);
    exit(kernel, pid, pid); exit(kernel, outside, outside); CHECK(kernel.scheduler().reap_orphans());
}
void owner_death(Kernel &kernel)
{
    const auto pid = process(kernel); auto *p = kernel.scheduler().find(pid);
    const auto owner = clone(kernel, pid, 0), shared = clone(kernel, pid, 0), private_waiter = clone(kernel, pid, 0);
    list(owner);
    CHECK(kernel.scheduler().activate_thread(pid, owner));
    CHECK(call(kernel, pid, lu::Operation::set_robust_list, {head, 3 * word_bytes}) == 0);
    for (auto waiter : {shared, private_waiter})
    {
        CHECK(kernel.scheduler().activate_thread(pid, waiter));
        kernel.scheduler().current_thread()->syscall_pending = true;
        native::FutexRequest request; request.address = owner_word; request.value = owner | 0x80000000U;
        request.big_endian = big;
        request.domain = waiter == shared ? native::FutexKeyDomain::anonymous_shared_operation : native::FutexKeyDomain::private_address_space;
        bool complete = true; CHECK(native::futex(kernel, pid, request, complete) == 0 && !complete);
    }
    exit(kernel, pid, owner);
    CHECK(get(owner_word - hw::user_begin) == 0xc0000000U);
    CHECK(kernel.scheduler().find_thread(pid, shared)->state == sched::ProcessState::runnable);
    CHECK(kernel.scheduler().find_thread(pid, private_waiter)->state == sched::ProcessState::blocked);
    auto *retired = kernel.scheduler().find_thread(pid, owner);
    CHECK(retired->tls.robust_list == 0 && retired->tls.robust_list_length == 0);
    // The inaccessible owner address stops this list but cannot stop group exit.
    p->threads()[0].tls.robust_list = 1;
    exit(kernel, pid, pid, true); CHECK(p->state() == sched::ProcessState::zombie);
    CHECK(p->threads()[0].tls.robust_list == 0); CHECK(kernel.scheduler().reap_orphans());
}
void group_death(Kernel &kernel)
{
    const auto pid = process(kernel); auto *p = kernel.scheduler().find(pid);
    const auto peer = clone(kernel, pid, 0);
    list(peer); kernel.scheduler().find_thread(pid, peer)->tls.robust_list = head;
    exit(kernel, pid, pid, true);
    CHECK(get(owner_word - hw::user_begin) == 0xc0000000U);
    for (const auto &thread : p->threads()) CHECK(thread.tls.robust_list == 0);
    CHECK(kernel.scheduler().reap_orphans());
}
void exec_retirement(Kernel &kernel)
{
    for (bool caller_old_owner : {false, true})
    {
        const auto pid = process(kernel); auto *p = kernel.scheduler().find(pid);
        const auto peer = clone(kernel, pid, 0);
        // Two registrations intentionally share a list. The sibling owns the
        // first word; the caller has a separate pending word in its own head.
        list(pid); p->threads()[0].tls.robust_list = head;
        const uptr peer_head = head + 256, pending = node + 256;
        put_word(peer_head, peer_head); put_word(peer_head + word_bytes, 2 * word_bytes);
        put_word(peer_head + 2 * word_bytes, pending); put_word(pending, peer_head);
        put(pending + 2 * word_bytes - hw::user_begin, (caller_old_owner ? peer : pid) | 0x80000000U);
        auto *caller = kernel.scheduler().find_thread(pid, peer); caller->tls.robust_list = peer_head;
        caller->tls.clear_tid_address = word; put(word - hw::user_begin, peer);
        p->threads()[0].tls.clear_tid_address = hw::user_begin + 132; put(132, pid);
        native::ExecRetirement retirement{*p, peer};
        CHECK(retirement.threads.size() == 2);
        auto failed = native::replace_entry_stacks(native::entry_stacks, *p,
            [] { return Status::busy("injected precommit refusal"); });
        CHECK(!failed && !failed.committed && p->threads().size() == 2);
        CHECK(p->threads()[0].tls.robust_list == head && caller->tls.robust_list == peer_head);
        CHECK(get(owner_word - hw::user_begin) == (pid | 0x80000000U));
        auto replaced = native::replace_entry_stacks(native::entry_stacks, *p, [&] {
            return kernel.scheduler().replace_image(pid, kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 4096}), {});
        });
        CHECK(replaced && replaced.committed && p->threads().size() == 1);
        retirement.consume(kernel, *p);
        CHECK(retirement.threads.empty() && p->threads()[0].tls.robust_list == 0);
        CHECK(get(owner_word - hw::user_begin) == 0xc0000000U);
        CHECK(get(pending + 2 * word_bytes - hw::user_begin) == (caller_old_owner ? (peer | 0x80000000U) : 0xc0000000U));
        CHECK(get(132) == 0 && get(word - hw::user_begin) == peer);
        const auto outputs = mock::outputs; retirement.consume(kernel, *p); CHECK(mock::outputs == outputs);
        exit(kernel, pid, pid); CHECK(kernel.scheduler().reap_orphans());
    }
}
}
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); native::started = true; native::runtime_kernel = &kernel;
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    registration(kernel); owner_death(kernel); group_death(kernel); exec_retirement(kernel);
    const auto stacks = native::entry_stacks.accounting();
    CHECK(!stacks.leased_slots && !stacks.active_slots && !stacks.quarantined_slots);
    CHECK(stacks.reservations == stacks.releases && native::runtime_status());
    std::puts("PASS native robust registration owner-death cleanup tests guest=0 hardware_isolation=0 smp=0");
}
