// Actual runtime/scheduler with hosted memory and injected pool faults only.
#define OK_ENABLE_TEST_POINTS 1
#define OK_NATIVE_MODEL_MEMORY_BYTES 16384
#include "native_clock_model.hpp"

namespace ok::user::native
{
struct NativePagePoolFaultProbe
{
    static void unpin(NativePagePool &pool, memory::PhysicalFrame frame, usize count)
    { pool.refused_unpin_ = frame; pool.refuse_unpins_ = count; }
};
}
namespace
{
constexpr uptr source = hw::user_begin + 128;
constexpr uptr destination = hw::user_begin + hw::page_size + 128;
constexpr uptr alias = hw::user_begin + 2 * hw::page_size + 128;
using Domain = native::FutexKeyDomain;
struct Owner
{
    sched::ProcessId pid{};
    std::array<sched::ThreadId, 8> tids{};
};
void put(uptr address, u32 value, bool big = false)
{
    for (usize i = 0; i < 4; ++i)
        mock::memory[address - hw::user_begin + i] = std::byte(value >> (8 * (big ? 3 - i : i)));
}
Owner owner(Kernel &kernel, usize count)
{
    const auto made = kernel.scheduler().create_user_process("requeue-model",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + mock::memory.size()}));
    CHECK(made);
    auto *process = kernel.scheduler().find(made.value());
    process->resources().personality = sched::UserPersonality::linux;
    Owner result{made.value(), {}};
    result.tids[0] = process->threads()[0].tid;
    for (usize index = 1; index < count; ++index)
    {
        const auto peer = kernel.scheduler().create_thread(result.pid, process->threads()[0].context);
        CHECK(peer); result.tids[index] = peer.value();
    }
    for (auto &thread : process->threads()) thread.native_active = true;
    CHECK(kernel.scheduler().set_runnable(result.pid));
    return result;
}
void activate(Kernel &kernel, Owner group, usize index = 0)
{ CHECK(kernel.scheduler().activate_thread(group.pid, group.tids[index])); }
memory::ObjectHandle wait(Kernel &kernel, Owner group, usize index, uptr address = source,
                          Domain domain = Domain::private_address_space, bool timed = false)
{
    activate(kernel, group, index);
    kernel.scheduler().current_thread()->syscall_pending = true;
    bool complete = true;
    CHECK(native::futex(kernel, group.pid,
        {native::FutexOperation::wait, address, 7, {0, 17}, timed, false, domain}, complete) == 0 && !complete);
    const auto object = native::futex_object(group.pid, group.tids[index]);
    CHECK(object.pool); return object;
}
i64 requeue(Kernel &kernel, Owner group, u32 wakes, u32 moves,
            uptr from = source, uptr to = destination, bool compare = false,
            u32 expected = 7, Domain domain = Domain::private_address_space, bool big = false)
{
    activate(kernel, group);
    return native::futex_requeue(kernel, group.pid, {from, to, wakes, moves, expected, compare, big, domain});
}
i64 wake(Kernel &kernel, Owner group, uptr address, u32 limit = 1,
         Domain domain = Domain::private_address_space)
{
    activate(kernel, group);
    bool complete = true;
    const auto result = native::futex(kernel, group.pid,
        {native::FutexOperation::wake, address, limit, {}, false, false, domain}, complete);
    CHECK(complete); return result;
}
void cleanup(Kernel &kernel, Owner group)
{
    CHECK(native::cancel_futex(kernel, group.pid));
    auto *process = kernel.scheduler().find(group.pid);
    for (auto &thread : process->threads())
        if (thread.entry_stack.pool) CHECK(native::retire_native_thread(thread));
    auto &pages = process->resources().native_pages;
    for (usize i = 0; i < pages.size(); ++i) CHECK(native::page_pool.release_reference(*pages.frame(i)));
    CHECK(pages.clear());
    CHECK(kernel.scheduler().exit_process(group.pid, 0));
    CHECK(kernel.scheduler().reap_orphans());
}
memory::PhysicalFrame frame(bool shared = true)
{
    CHECK(native::activate_pool());
    auto result = native::page_pool.allocate_frame(); CHECK(result);
    if (shared) CHECK(native::page_pool.mark_shared(result.value()));
    return result.value();
}
void map(Kernel &kernel, Owner group, uptr address, memory::PhysicalFrame backing, bool retain = false)
{
    if (retain) CHECK(native::page_pool.retain_shared(backing));
    auto &pages = kernel.scheduler().find(group.pid)->resources().native_pages;
    CHECK(pages.reserve(native::page_pool, pages.size() + 1, true));
    CHECK(pages.push_back({address, backing.address, hw::page_read | hw::page_write}, backing));
}
void reset_injected_failure()
{
    // Test-only recovery after all failed owners have been explicitly retired.
    // Production exposes no reset/recovery operation for this retained failure.
    CHECK(!native::runtime_failure);
    CHECK(!native::futex_objects.accounting().live_objects);
    native::runtime_failure = Status::success();
}
void ordering(Kernel &kernel)
{
    const auto group = owner(kernel, 6), foreign = owner(kernel, 2);
    put(source, 7); put(destination, 7);
    auto a = wait(kernel, group, 1), b = wait(kernel, group, 2, source, Domain::private_address_space, true);
    auto c = wait(kernel, group, 3), d = wait(kernel, group, 4, destination);
    const auto outside = wait(kernel, foreign, 1);
    CHECK(wake(kernel, group, source) == 1 && !native::futex_objects.resolve(a));
    a = wait(kernel, group, 1); // The low slot is recycled after B/C were queued.
    const auto before = *native::futex_objects.resolve(b).value();
    const auto binding = native::futex_origin_binding(b, before);
    CHECK(requeue(kernel, group, 0, 1) == 1);
    const auto &moved = *native::futex_objects.resolve(b).value();
    CHECK(moved.key.address == destination && moved.address == source);
    CHECK(native::futex_origin_binding(b, moved) == binding && moved.domain == before.domain);
    CHECK(moved.timeout.deadline() == before.timeout.deadline() && moved.timeout.generation() == before.timeout.generation());
    CHECK(kernel.scheduler().find_thread(group.pid, group.tids[2])->wait_key == source);
    CHECK(wake(kernel, group, destination) == 1 && !native::futex_objects.resolve(d));
    CHECK(wake(kernel, group, destination) == 1 && !native::futex_objects.resolve(b));
    const auto sequence = native::futex_objects.resolve(c).value()->queue_sequence;
    CHECK(requeue(kernel, group, 0, 1, source, source) == 1);
    CHECK(native::futex_objects.resolve(c).value()->queue_sequence == sequence);
    CHECK(wake(kernel, group, source) == 1 && !native::futex_objects.resolve(c));
    CHECK(wake(kernel, group, source, 0) == 1 && !native::futex_objects.resolve(a));
    CHECK(native::futex_objects.resolve(outside));
    cleanup(kernel, group); cleanup(kernel, foreign);
}
void admission(Kernel &kernel)
{
    const auto group = owner(kernel, 4);
    put(source, 7); put(destination, 7);
    const auto one = wait(kernel, group, 1), two = wait(kernel, group, 2);
    const auto before = native::futex_origin_binding(one, *native::futex_objects.resolve(one).value());
    CHECK(requeue(kernel, group, 1, 1, source, destination, true, 8) == -11);
    CHECK(native::futex_origin_binding(one, *native::futex_objects.resolve(one).value()) == before);
    const auto copies = mock::copies;
    CHECK(requeue(kernel, group, 0xffffffffU, 1, hw::user_end, destination + 1) == -22);
    CHECK(requeue(kernel, group, 1, 0x80000000U, hw::user_end, destination + 1) == -22);
    CHECK(mock::copies == copies);
    CHECK(requeue(kernel, group, 0, 0, hw::user_end, destination + 1) == -14);
    CHECK(requeue(kernel, group, 0, 0, source + 1, hw::user_end) == -22);
    CHECK(requeue(kernel, group, 0, 0, source, destination + 1, true, 8) == -22);
    CHECK(requeue(kernel, group, 0, 0, source, destination + 1, false, 0, Domain::anonymous_shared_operation) == -14);
    CHECK(requeue(kernel, group, 0, 0, 0, 0) == 0); // No private VMA/user_begin precheck.
    mock::forbidden = source;
    CHECK(requeue(kernel, group, 0, 0) == 0); // Private plain keys do not read either word.
    CHECK(requeue(kernel, group, 0, 0, source, destination, true) == -14);
    mock::forbidden = 0;
    put(source, 0x01020304U, true);
    CHECK(requeue(kernel, group, 0, 0, source, destination, true, 0x01020304U,
                  Domain::private_address_space, true) == 0);
    put(source, 7);
    const auto sequence = native::futex_queue_sequence;
    native::futex_queue_sequence = ~u64{0}; // Reach the otherwise impractical boundary.
    CHECK(requeue(kernel, group, 1, 1) == -75);
    CHECK(native::futex_objects.resolve(one) && native::futex_objects.resolve(two));
    CHECK(requeue(kernel, group, 0, 1, source, source) == 1);
    activate(kernel, group, 3);
    bool complete = true;
    CHECK(native::futex(kernel, group.pid, {native::FutexOperation::wait, source, 7}, complete) == -75 && complete);
    native::futex_queue_sequence = sequence;
    CHECK(requeue(kernel, group, 0x7fffffffU, 0x7fffffffU) == 2);
    CHECK(!native::futex_objects.resolve(one) && !native::futex_objects.resolve(two));
    cleanup(kernel, group);
}
void shared(Kernel &kernel)
{
    const auto group = owner(kernel, 3), peer = owner(kernel, 2);
    const auto old = frame(), next = frame();
    map(kernel, group, hw::user_begin, old);
    map(kernel, group, hw::user_begin + hw::page_size, next);
    map(kernel, peer, hw::user_begin + 2 * hw::page_size, old, true);
    put(source, 7); put(destination, 7); put(alias, 7);
    const auto a = wait(kernel, group, 1, source, Domain::anonymous_shared_operation);
    const auto b = wait(kernel, peer, 1, alias, Domain::anonymous_shared_operation);
    const auto binding = native::futex_origin_binding(b, *native::futex_objects.resolve(b).value());
    CHECK(requeue(kernel, group, 0, 0, source, alias, true, 8, Domain::anonymous_shared_operation) == -14);
    mock::write_forbidden = destination;
    CHECK(requeue(kernel, group, 0, 2, source, destination, true, 7, Domain::anonymous_shared_operation) == 2);
    mock::write_forbidden = 0;
    CHECK(native::page_pool.pin_count(old).value() == 0 && native::page_pool.pin_count(next).value() == 2);
    CHECK(native::futex_origin_binding(b, *native::futex_objects.resolve(b).value()) == binding);
    CHECK(wake(kernel, group, source, 1, Domain::anonymous_shared_operation) == 0);
    // Retire all source mapping references after transfer; the destination pin
    // owns each continuation independently of its original virtual mapping.
    auto &peer_pages = kernel.scheduler().find(peer.pid)->resources().native_pages;
    CHECK(native::page_pool.release_reference(old)); CHECK(peer_pages.clear());
    auto &pages = kernel.scheduler().find(group.pid)->resources().native_pages;
    native::MappingPages replacement;
    CHECK(replacement.reserve(native::page_pool, 1, true));
    CHECK(replacement.push_back({hw::user_begin + hw::page_size, next.address, hw::page_read | hw::page_write}, next));
    CHECK(native::page_pool.release_reference(old)); CHECK(pages.clear()); pages = std::move(replacement);
    CHECK(!native::page_pool.valid(old));
    CHECK(wake(kernel, group, destination, 9, Domain::anonymous_shared_operation) == 2);
    CHECK(!native::futex_objects.resolve(a) && !native::futex_objects.resolve(b));
    cleanup(kernel, peer); cleanup(kernel, group);

    const auto same = owner(kernel, 2);
    const auto backing = frame(); map(kernel, same, hw::user_begin, backing); put(source, 7);
    const auto object = wait(kernel, same, 1, source, Domain::anonymous_shared_operation);
    CHECK(requeue(kernel, same, 0, 1, source, source + 4, false, 0, Domain::anonymous_shared_operation) == 1);
    CHECK(native::page_pool.pin_count(backing).value() == 1);
    CHECK(native::futex_objects.resolve(object).value()->key.word_offset == 132);
    cleanup(kernel, same);

    const auto ordinary = owner(kernel, 2);
    const auto shared_backing = frame(), private_backing = frame(false);
    map(kernel, ordinary, hw::user_begin, shared_backing);
    map(kernel, ordinary, hw::user_begin + hw::page_size, private_backing); put(source, 7);
    const auto nonshared = wait(kernel, ordinary, 1, source, Domain::anonymous_shared_operation);
    CHECK(requeue(kernel, ordinary, 0, 1, source, destination, false, 0, Domain::anonymous_shared_operation) == 1);
    CHECK(!native::futex_objects.resolve(nonshared).value()->pin_owned);
    CHECK(!native::futex_objects.resolve(nonshared).value()->key.shared);
    CHECK(native::page_pool.pin_count(shared_backing).value() == 0);
    cleanup(kernel, ordinary);
}
void preflight(Kernel &kernel, bool rollback_failure)
{
    const auto group = owner(kernel, 4);
    const auto old = frame(), next = frame();
    map(kernel, group, hw::user_begin, old); map(kernel, group, hw::user_begin + hw::page_size, next);
    put(source, 7);
    const auto a = wait(kernel, group, 1, source, Domain::anonymous_shared_operation);
    const auto b = wait(kernel, group, 2, source, Domain::anonymous_shared_operation);
    const auto c = wait(kernel, group, 3, source, Domain::anonymous_shared_operation);
    for (usize i = 0; i < 65534; ++i) CHECK(native::page_pool.pin(next));
    if (rollback_failure) native::NativePagePoolFaultProbe::unpin(native::page_pool, next, 1);
    CHECK(requeue(kernel, group, 1, 2, source, destination, false, 0, Domain::anonymous_shared_operation) ==
          (rollback_failure ? -5 : -14));
    CHECK(native::futex_objects.resolve(a) && native::futex_objects.resolve(b) && native::futex_objects.resolve(c));
    CHECK(kernel.scheduler().find_thread(group.pid, group.tids[1])->wait_registration.active());
    CHECK(native::page_pool.pin_count(old).value() == 3);
    CHECK(native::futex_objects.resolve(b).value()->pending_transfer_pin_owned == rollback_failure);
    CHECK(native::page_pool.pin_count(next).value() == (rollback_failure ? 65535U : 65534U));
    CHECK(native::cancel_futex(kernel, group.pid));
    if (rollback_failure) reset_injected_failure();
    CHECK(native::page_pool.pin_count(next).value() == 65534);
    for (usize i = 0; i < 65534; ++i) CHECK(native::page_pool.unpin(next));
    cleanup(kernel, group);
}
void retained_failure(Kernel &kernel, bool pending_failure)
{
    const auto group = owner(kernel, 2);
    const auto old = frame(), next = frame();
    map(kernel, group, hw::user_begin, old); map(kernel, group, hw::user_begin + hw::page_size, next);
    put(source, 7);
    const auto object = wait(kernel, group, 1, source, Domain::anonymous_shared_operation);
    native::NativePagePoolFaultProbe::unpin(native::page_pool, old, pending_failure ? 1 : 2);
    CHECK(requeue(kernel, group, 0, 1, source, destination, false, 0, Domain::anonymous_shared_operation) == -5);
    auto &waiter = *native::futex_objects.resolve(object).value();
    CHECK(waiter.pin_owned && waiter.pending_transfer_pin_owned && waiter.key.address == source);
    CHECK(native::same_futex_frame(waiter.key.frame, old) && native::same_futex_frame(waiter.pending_transfer_frame, next));
    CHECK(native::page_pool.pin_count(old).value() == 1 && native::page_pool.pin_count(next).value() == 1);
    CHECK(requeue(kernel, group, 0, 1) == -5 && wake(kernel, group, source) == -5);
    CHECK(native::futex_wake_operation(kernel, group.pid, {source, destination, 1, 1, 0}) == -5);
    CHECK(!native::expire_futex(kernel, ~u64{0}));
    if (pending_failure) native::NativePagePoolFaultProbe::unpin(native::page_pool, next, 1);
    CHECK(!native::cancel_futex(kernel, object));
    CHECK(native::futex_objects.resolve(object));
    CHECK(waiter.pin_owned == !pending_failure && waiter.pending_transfer_pin_owned);
    CHECK(native::page_pool.pin_count(old).value() == (pending_failure ? 0U : 1U));
    CHECK(native::cancel_futex(kernel, object));
    CHECK(native::page_pool.pin_count(old).value() == 0 && native::page_pool.pin_count(next).value() == 0);
    reset_injected_failure(); cleanup(kernel, group);
}
void destructive_failure(Kernel &kernel, unsigned mode)
{
    const auto group = owner(kernel, 3);
    const auto backing = frame(); map(kernel, group, hw::user_begin, backing); put(source, 7);
    auto *process = kernel.scheduler().find(group.pid);
    for (auto &thread : process->threads()) CHECK(native::prepare_native_thread(thread));
    const auto object = wait(kernel, group, 1, source, Domain::anonymous_shared_operation);
    auto *thread = kernel.scheduler().find_thread(group.pid, group.tids[1]);
    const auto stack = thread->entry_stack;
    native::NativePagePoolFaultProbe::unpin(native::page_pool, backing, 1);
    if (mode == 0)
    {
        activate(kernel, group);
        native::release_process(kernel, group.pid);
        CHECK(thread->entry_stack == stack && thread->native_active && process->resources().native_pages.size() == 1);
    }
    if (mode == 1)
    {
        CHECK(kernel.scheduler().set_runnable(group.pid)); activate(kernel, group, 1);
        bool complete = true;
        CHECK(native::dispatch(kernel, group.pid, {static_cast<uptr>(syscall::Number::exit), {}}, complete) == -14);
        CHECK(!complete && thread->entry_stack == stack && thread->native_active);
        CHECK(thread->state != sched::ProcessState::exited);
    }
    if (mode == 2)
    {
        activate(kernel, group);
        // Exercise the actual exec stack-escrow helper and checked cleanup
        // callback. This is not ELF execution or a full exec syscall test.
        auto result = native::replace_entry_stacks(native::entry_stacks, *process,
            [] { return Status::success(); }, [&] { return native::cancel_futex(kernel, group.pid); });
        CHECK(!result && result.committed && result.retained_count == 2);
        for (usize i = 0; i < result.retained_count; ++i)
        {
            const auto &saved = result.retained[i];
            CHECK(native::entry_stacks.top(saved.handle, saved.owner));
            auto *peer = kernel.scheduler().find_thread(saved.owner.pid, saved.owner.tid);
            peer->entry_stack = saved.handle; peer->kernel_stack_top = saved.top;
        }
        CHECK(thread->entry_stack == stack && process->resources().native_pages.size() == 1);
    }
    CHECK(native::futex_objects.resolve(object));
    CHECK(native::cancel_futex(kernel, group.pid)); reset_injected_failure(); cleanup(kernel, group);
}
void wake_cleanup_failure(Kernel &kernel)
{
    const auto group = owner(kernel, 3);
    const auto backing = frame(); map(kernel, group, hw::user_begin, backing); put(source, 7);
    const auto first = wait(kernel, group, 1, source, Domain::anonymous_shared_operation);
    const auto second = wait(kernel, group, 2, source, Domain::anonymous_shared_operation);
    native::NativePagePoolFaultProbe::unpin(native::page_pool, backing, 1);
    CHECK(wake(kernel, group, source, 2, Domain::anonymous_shared_operation) == -5);
    CHECK(native::futex_objects.resolve(first) && native::futex_objects.resolve(second));
    CHECK(kernel.scheduler().find_thread(group.pid, group.tids[1])->wait_registration.completion() == sched::WaitCompletion::ready);
    CHECK(kernel.scheduler().find_thread(group.pid, group.tids[2])->wait_registration.active());
    CHECK(native::page_pool.pin_count(backing).value() == 2);
    CHECK(native::cancel_futex(kernel, group.pid)); reset_injected_failure(); cleanup(kernel, group);
}
void stale_generation(Kernel &kernel)
{
    const auto group = owner(kernel, 2); put(source, 7);
    const auto object = wait(kernel, group, 1);
    native::PollWaitIndex *index{};
    for (auto &entry : native::futex_waits) if (entry.object == object) index = &entry;
    CHECK(index); ++index->object.generation; // Test-only stale-handle injection.
    CHECK(requeue(kernel, group, 1, 1) == -5);
    CHECK(native::futex_objects.resolve(object));
    CHECK(kernel.scheduler().find_thread(group.pid, group.tids[1])->wait_registration.active());
    index->object = object; // Restore injected metadata before explicit retirement.
    CHECK(native::cancel_futex(kernel, object)); reset_injected_failure(); cleanup(kernel, group);
}
void stale_and_timeout(Kernel &kernel)
{
    const auto group = owner(kernel, 3); put(source, 7);
    const auto timed = wait(kernel, group, 1, source, Domain::private_address_space, true);
    const auto old = *native::futex_objects.resolve(timed).value();
    CHECK(requeue(kernel, group, 0, 1) == 1);
    CHECK(!native::expire_futex(kernel, old.timeout.deadline()));
    CHECK(!native::futex_objects.resolve(timed));
    CHECK(kernel.scheduler().find_thread(group.pid, group.tids[1])->wait_registration.completion() == sched::WaitCompletion::timeout);
    const auto stale = wait(kernel, group, 1);
    auto *thread = kernel.scheduler().find_thread(group.pid, group.tids[1]);
    CHECK(kernel.scheduler().complete_wait(thread->wait_registration.ticket(), sched::WaitCompletion::cancelled));
    activate(kernel, group, 1);
    const auto replacement = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::futex, source); CHECK(replacement);
    CHECK(requeue(kernel, group, 1, 1) == 0 && thread->wait_registration.pending(replacement.value()));
    CHECK(!native::expire_futex(kernel, mock::now) && !native::futex_objects.resolve(stale));
    CHECK(thread->wait_registration.pending(replacement.value()));
    CHECK(kernel.scheduler().complete_wait(replacement.value(), sched::WaitCompletion::cancelled));
    cleanup(kernel, group);
}
void snapshot_ticket_substitution(Kernel &kernel)
{
    const auto group = owner(kernel, 2); put(source, 7);
    const auto object = wait(kernel, group, 1);
    auto *record = native::futex_objects.resolve(object).value();
    const auto original = *record;
    const auto snapshot = native::snapshot_futex_queue(kernel, original.key);
    CHECK(snapshot && snapshot.value().count == 1);
    const auto &selected = snapshot.value().entries[0];
    CHECK(selected.object == object && selected.sequence == original.queue_sequence &&
          native::exact_futex_key(selected.key, original.key) && selected.pid == group.pid &&
          selected.tid == group.tids[1] && selected.binding == native::futex_origin_binding(object, original));
    CHECK(kernel.scheduler().complete_wait(original.ticket, sched::WaitCompletion::cancelled));
    activate(kernel, group, 1);
    const auto replacement = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::futex, source);
    CHECK(replacement && replacement.value() != original.ticket);
    record->ticket = replacement.value(); // Same-object/same-sequence test-only ticket substitution.
    CHECK(native::pending_futex_wait(kernel, *record)); // Current-state validation alone would accept it.
    CHECK(record->queue_sequence == selected.sequence && native::exact_futex_key(record->key, selected.key));
    CHECK(!native::selected_futex_wait(kernel, selected, original.key));
    CHECK(!native::runtime_failure && native::futex_objects.resolve(object));
    auto *thread = kernel.scheduler().find_thread(group.pid, group.tids[1]);
    CHECK(thread->syscall_pending && thread->wait_registration.pending(replacement.value()));
    CHECK(record->ticket == replacement.value() && record->queue_sequence == original.queue_sequence);
    record->ticket = original.ticket; // Restore only injected metadata for explicit old-object retirement.
    CHECK(native::cancel_futex(kernel, object));
    CHECK(thread->wait_registration.pending(replacement.value()));
    CHECK(kernel.scheduler().complete_wait(replacement.value(), sched::WaitCompletion::cancelled));
    reset_injected_failure(); cleanup(kernel, group);
}
template <lu::Profile P> void adapter(Kernel &kernel, Owner group, usize number_slot, std::array<usize, 6> slots)
{
    using Codec = lu::Codec<P>;
    constexpr bool big = Codec::profile.big_endian;
    for (auto call : {lu::Operation::futex, lu::Operation::futex_time64})
    {
        const auto number = Codec::syscall_number(call); if (!number) continue;
        const auto invoke = [&](u32 operation, u64 wakes = 0, u64 moves = 0, u64 expected = 7,
                                uptr stack = hw::user_begin + 1024) {
            activate(kernel, group);
            hw::UserFrame registers{}; registers.registers[number_slot] = *number; registers.sp = stack;
            const std::array<u64, 6> arguments{source, operation, wakes, moves, destination, expected};
            for (usize i = 0; i < (P == lu::Profile::mips ? 4U : 6U); ++i) registers.registers[slots[i]] = arguments[i];
            if constexpr (P == lu::Profile::mips)
            { put(hw::user_begin + 1040, destination, big); put(hw::user_begin + 1044, static_cast<u32>(expected), big); }
            bool complete = true;
            const auto result = user::linux_bootstrap::dispatch<P>(kernel, group.pid, registers, complete,
                [&](hw::Syscall request, bool &done) { return native::dispatch(kernel, group.pid, request, done); });
            CHECK(complete); return result;
        };
        put(source, 7, big);
        CHECK(invoke(131, 0, 0x100000000ULL) == 0); // Scalar high word is ignored.
        CHECK(invoke(132, 0, 0, 0x100000007ULL) == 0);
        CHECK(invoke(132, 0, 0, 8) == -11);
        CHECK(invoke(131, 0xffffffffU) == -22 && invoke(131, 0, 0x80000000U) == -22);
        CHECK(invoke(3 + 128 + 256, 0xffffffffU, 0xffffffffU, 0, 0) == -38);
        CHECK(invoke(4 + 512, 0, 0, 0, 0) == -38);
        if constexpr (P == lu::Profile::mips)
        {
            mock::forbidden = hw::user_begin + 1044;
            CHECK(invoke(131) == 0 && mock::last_input_size == 4);
            CHECK(invoke(132) == -14 && mock::last_input_size == 8);
            mock::forbidden = 0;
            CHECK(invoke(131, 0, 0, 0, 0) == -14);
            CHECK(invoke(129, 0, 0, 0, 0) == 0); // No unused WAIT/WAKE tail read.
        }
    }
}
}
int main()
{
    static Kernel kernel; CHECK(kernel.boot({}));
    native::runtime_kernel = &kernel;
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    ordering(kernel); admission(kernel); shared(kernel); stale_and_timeout(kernel);
    preflight(kernel, false); preflight(kernel, true);
    retained_failure(kernel, false); retained_failure(kernel, true);
    wake_cleanup_failure(kernel); stale_generation(kernel); snapshot_ticket_substitution(kernel);
    for (unsigned mode = 0; mode < 3; ++mode) destructive_failure(kernel, mode);
    const auto group = owner(kernel, 1);
    adapter<lu::Profile::i386>(kernel, group, 7, {4, 6, 5, 1, 0, 2});
    adapter<lu::Profile::x86_64>(kernel, group, 14, {9, 10, 11, 5, 7, 6});
    adapter<lu::Profile::aarch64>(kernel, group, 8, {0, 1, 2, 3, 4, 5});
    adapter<lu::Profile::arm32>(kernel, group, 7, {0, 1, 2, 3, 4, 5});
    adapter<lu::Profile::rv64>(kernel, group, 17, {10, 11, 12, 13, 14, 15});
    adapter<lu::Profile::rv32>(kernel, group, 17, {10, 11, 12, 13, 14, 15});
    adapter<lu::Profile::loongarch64>(kernel, group, 11, {4, 5, 6, 7, 8, 9});
    adapter<lu::Profile::mips>(kernel, group, 2, {4, 5, 6, 7, 0, 0});
    adapter<lu::Profile::mips64>(kernel, group, 2, {4, 5, 6, 7, 8, 9});
    adapter<lu::Profile::ppc>(kernel, group, 0, {3, 4, 5, 6, 7, 8});
    cleanup(kernel, group);
    CHECK(native::runtime_status() && !native::futex_objects.accounting().live_objects);
    CHECK(!native::page_pool.pinned_count() && !native::page_pool.object_count());
    CHECK(!native::entry_stacks.accounting().leased_slots);
    std::puts("PASS hosted FUTEX_REQUEUE profiles=10 runtime=pass lifetime=pass ordering=pass guest=0 hardware_isolation=0 smp=0");
}
