// Included only by the opt-in debug runtime, within ok::user::native.
namespace
{
struct PeerFixture
{
    sched::ProcessId pid{};
    std::array<sched::ThreadId, 4> tids{};
    std::array<memory::ObjectHandle, 3> word_objects{};
    std::array<u64, 3> word_generations{};
    usize round{}, waits{}, wakes{}, parks{}, controls{};
    usize users_before{}, objects_before{}, pins_before{}, pending_before{};
    bool admitted{}, active{};
};
PeerFixture peer_fixture;
constexpr uptr peer_address = hw::user_begin + 0x10000;
constexpr uptr peer_word = peer_address + offsetof(tests::PeerState, word);
constexpr uptr peer_park = peer_address + offsetof(tests::PeerState, park);
constexpr uptr peer_stack_top = hw::user_end - 0x100000;
memory::ObjectHandle peer_object(usize role)
{
    for (const auto &entry : futex_waits)
        if (entry.pid == peer_fixture.pid && entry.tid == peer_fixture.tids[role])
            return entry.object;
    return {};
}
Status peer_failure(const char *message)
{
    emit("OK_PRIVATE_FUTEX_PEERS status=fail reason="); emit(message); emit("\n");
    return Status::fault(message);
}
Status setup_peer_fixture(Kernel &kernel, sched::ProcessId pid)
{
    auto *p = kernel.scheduler().find(pid);
    if (!p || p->resources().personality != Personality::linux || p->threads().size() != 1)
        return peer_failure("initial-image-is-not-single-linux-elf");
    peer_fixture = {};
    peer_fixture.pid = pid;
    peer_fixture.users_before = page_pool.user_count() - p->resources().native_pages.size();
    peer_fixture.objects_before = page_pool.object_count();
    peer_fixture.pins_before = page_pool.pinned_count();
    peer_fixture.pending_before = page_pool.pending_count();
    tests::PeerState shared{};
    shared.magic = tests::peer_magic; shared.version = 1; shared.word = 7;
    for (usize round = 0; round < 2; ++round)
        for (usize role = 0; role < 3; ++role)
            shared.results[round][role] = shared.observed[round][role] = shared.wakes[round][role] = tests::peer_unwritten;
    if (!hw::copy_to_user(p->resources().native_space, peer_address, &shared, sizeof(shared)))
        return peer_failure("shared-page-is-not-writable");
    const auto entry = p->threads()[0].native_frame.pc;
    const auto context = p->threads()[0].context;
    peer_fixture.tids[0] = p->threads()[0].tid;
    for (usize role = 1; role < 4; ++role)
    {
        auto thread = kernel.scheduler().create_thread(pid, context);
        if (!thread) return thread.status();
        peer_fixture.tids[role] = thread.value();
    }
    for (usize role = 0; role < 4; ++role)
    {
        auto *thread = kernel.scheduler().find_thread(pid, peer_fixture.tids[role]);
        const uptr sp = peer_stack_top - role * 4 * hw::page_size - 32;
        const uptr token = role;
        if (!hw::copy_to_user(p->resources().native_space, sp, &token, sizeof(token)))
            return peer_failure("role-stack-is-not-writable");
        if (role != 0)
            if (auto status = prepare_native_thread(*thread); !status) return status;
        thread->native_frame = hw::initial_frame(entry, sp);
        thread->native_active = true;
        thread->syscall_pending = false;
        thread->state = role == 3 ? sched::ProcessState::blocked : sched::ProcessState::runnable;
    }
    p->set_state(sched::ProcessState::runnable);
    peer_fixture.active = true;
    emit("OK_PRIVATE_FUTEX_PEERS phase=setup scope=actual-user threads=4 shared_space=1 clone=0 pthread=0 smp=0\n");
    return Status::success();
}
Status record_peer_trap(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid, const hw::UserFrame &frame)
{
    if (!peer_fixture.active || pid != peer_fixture.pid) return Status::success();
    usize role = 4;
    for (usize i = 0; i < 4; ++i) if (peer_fixture.tids[i] == tid) role = i;
    if (role == 4) return peer_failure("unknown-trap-owner");
    const uptr top = peer_stack_top - role * 4 * hw::page_size;
    if (frame.sp >= top || frame.sp < top - 4 * hw::page_size)
        return peer_failure("user-stack-left-private-slice");
    using Bridge = lu::NativeBridge<lu::configured_native_profile()>;
    using Codec = lu::Codec<lu::configured_native_profile()>;
    auto *process = kernel.scheduler().find(pid);
    auto call = Bridge::decode_call_copying_stack(frame, 4, [&](uptr address, std::span<u8> output) {
        return hw::copy_from_user(process->resources().native_space, output.data(), address, output.size());
    });
    if (!call || call.value().number != *Codec::syscall_number(lu::Operation::futex))
        return peer_failure("unexpected-user-syscall");
    const auto &a = call.value().arguments;
    if (a[0] == peer_word && a[1] == 128 && role < 3 && a[2] == 7)
        ++peer_fixture.waits;
    else if (a[0] == peer_word && a[1] == 129 && role == 3 && peer_fixture.admitted)
    {
        constexpr u32 counts[] = {0, 1, 0xffffffffU};
        if (static_cast<u32>(a[2]) != counts[peer_fixture.wakes % 3])
            return peer_failure("wake-count-order");
        ++peer_fixture.wakes;
    }
    else if (a[0] == peer_park && a[1] == 128 && a[2] == 0)
        ++peer_fixture.parks;
    else return peer_failure("unexpected-user-futex");
    emit("OK_PRIVATE_FUTEX_TRAP tid="); emit_number(tid); emit(" role="); emit_number(role);
    emit(" op="); emit_number(a[1]); emit(" address="); emit_hex(static_cast<uptr>(a[0]));
    emit(" value="); emit_number(static_cast<u32>(a[2])); emit("\n");
    return Status::success();
}
Status checkpoint_peer_fixture(Kernel &kernel)
{
    if (!peer_fixture.active) return Status::success();
    auto *p = kernel.scheduler().find(peer_fixture.pid);
    if (!p) return peer_failure("process-disappeared");
    tests::PeerState shared{};
    if (!hw::copy_from_user(p->resources().native_space, &shared, peer_address, sizeof(shared)) ||
        shared.magic != tests::peer_magic || shared.version != 1)
        return peer_failure("shared-state-corrupted");
    for (usize i = 0; i < 4; ++i) if (shared.failure[i]) return peer_failure("user-control-return");
    const auto round = peer_fixture.round;
    if (!peer_fixture.admitted)
    {
        for (usize i = 0; i < 3; ++i)
        {
            auto *t = kernel.scheduler().find_thread(p->pid(), peer_fixture.tids[i]);
            if (!t || !t->native_active) return peer_failure("waiter-disappeared");
            if (shared.entered[i] != round + 1 || t->wait_reason != sched::ThreadWaitReason::futex ||
                t->wait_key != peer_word || !t->wait_registration.active()) return Status::success();
        }
        for (usize i = 0; i < 3; ++i)
        {
            auto *t = kernel.scheduler().find_thread(p->pid(), peer_fixture.tids[i]);
            if (t->wait_registration.ticket().generation <= peer_fixture.word_generations[i])
                return peer_failure("wait-generation-reused");
            peer_fixture.word_generations[i] = t->wait_registration.ticket().generation;
            peer_fixture.word_objects[i] = peer_object(i);
            if (!futex_objects.resolve(peer_fixture.word_objects[i])) return peer_failure("admission-object-missing");
        }
        auto *peer = kernel.scheduler().find_thread(p->pid(), peer_fixture.tids[3]);
        if (round == 0)
        {
            peer->state = sched::ProcessState::runnable;
            p->set_state(sched::ProcessState::runnable);
        }
        else
        {
            if (!finish_futex(kernel, peer_object(3), sched::WaitCompletion::ready, 0))
                return peer_failure("peer-control-completion");
            ++peer_fixture.controls;
        }
        peer_fixture.admitted = true;
        emit("OK_PRIVATE_FUTEX_PEERS phase=admitted round="); emit_number(round); emit(" waiters=3\n");
        return Status::success();
    }
    for (usize i = 0; i < 4; ++i)
    {
        auto *t = kernel.scheduler().find_thread(p->pid(), peer_fixture.tids[i]);
        if (!t || !t->native_active) return peer_failure("participant-disappeared");
        if (shared.completed[i] != round + 1 || t->wait_reason != sched::ThreadWaitReason::futex ||
            t->wait_key != peer_park || !t->wait_registration.active()) return Status::success();
    }
    for (usize i = 0; i < 3; ++i)
        if (shared.results[round][i] != 0 || shared.observed[round][i] != 99 || shared.wakes[round][i] != 1 ||
            futex_objects.resolve(peer_fixture.word_objects[i])) return peer_failure("user-wake-result-or-old-handle");
    if (peer_fixture.waits != 3 * (round + 1) || peer_fixture.wakes != 3 * (round + 1) ||
        peer_fixture.parks != 4 * (round + 1)) return peer_failure("unexpected-syscall-count");
    emit("OK_PRIVATE_FUTEX_PEERS phase=round-complete round="); emit_number(round); emit(" results=0,0,0 wakes=1,1,1 changed_word=99\n");
    if (round == 0)
    {
        const u32 initial = 7;
        if (!hw::copy_to_user(p->resources().native_space, peer_word, &initial, sizeof(initial)))
            return peer_failure("next-round-word");
        ++peer_fixture.round;
        peer_fixture.admitted = false;
        for (usize i = 0; i < 3; ++i)
        {
            if (!finish_futex(kernel, peer_object(i), sched::WaitCompletion::ready, 0))
                return peer_failure("waiter-control-completion");
            ++peer_fixture.controls;
        }
        return Status::success();
    }
    release_process(kernel, peer_fixture.pid);
    if (!kernel.scheduler().exit_process(peer_fixture.pid, 0)) return peer_failure("fixture-retirement");
    static_cast<void>(kernel.scheduler().reap_orphans());
    for (const auto &entry : futex_waits) if (entry.pid == peer_fixture.pid) return peer_failure("index-not-drained");
    if (futex_objects.accounting().live_objects || page_pool.object_count() != peer_fixture.objects_before ||
        page_pool.user_count() != peer_fixture.users_before || page_pool.pinned_count() != peer_fixture.pins_before ||
        page_pool.pending_count() != peer_fixture.pending_before) return peer_failure("resources-not-drained");
    peer_fixture.active = false;
    started = false;
    emit("OK_PRIVATE_FUTEX_PEERS status=pass version=1 scope=actual-user rounds=2 waiters=6 wakes=6 parks=8 controls=4 threads=4 remaining_objects=0 clone=0 pthread=0 smp=0 physical=0\n");
    return Status::success();
}
} // namespace
