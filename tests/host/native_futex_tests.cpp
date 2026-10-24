// Real scheduler/runtime/ABI code with hosted user copies, never guest execution.
#include "native_clock_model.hpp"

namespace
{
constexpr uptr word = hw::user_begin + 128;
constexpr uptr time_address = hw::user_begin + 1024;
struct Owner
{
    sched::ProcessId pid{};
    std::array<sched::ThreadId, sched::max_threads_per_process> tids{};
};
Owner owner(Kernel &kernel, usize count)
{
    auto result = kernel.scheduler().create_user_process(
        "futex-model", kernel.arch().make_user_context({hw::user_begin, hw::user_end - hw::page_size}));
    CHECK(result);
    Owner result_owner{result.value(), {}};
    auto *process = kernel.scheduler().find(result_owner.pid);
    result_owner.tids[0] = process->threads()[0].tid;
    for (usize i = 1; i < count; ++i)
    {
        auto created = kernel.scheduler().create_thread(result_owner.pid, process->threads()[0].context);
        CHECK(created);
        result_owner.tids[i] = created.value();
    }
    CHECK(kernel.scheduler().set_runnable(result_owner.pid));
    for (auto &thread : process->threads())
        thread.native_active = true;
    return result_owner;
}
void activate(Kernel &kernel, Owner owner, usize index)
{
    CHECK(kernel.scheduler().activate_thread(owner.pid, owner.tids[index]));
}
void put(usize offset, usize width, u64 value, bool big = false)
{
    for (usize i = 0; i < width; ++i)
        mock::memory[offset + i] = static_cast<std::byte>(value >> (8 * (big ? width - i - 1 : i)));
}
native::FutexRequest request(native::FutexOperation operation, u32 value = 7, uptr address = word)
{
    return {operation, address, value, {}, false, false};
}
memory::ObjectHandle wait(Kernel &kernel, Owner owner, usize index, bool timed = false)
{
    activate(kernel, owner, index);
    auto *thread = kernel.scheduler().current_thread();
    thread->syscall_pending = true;
    thread->native_frame.registers[0] = 0x123456;
    auto r = request(native::FutexOperation::wait);
    r.timed = timed;
    r.timeout = {0, 17};
    bool done = true;
    CHECK(native::private_futex(kernel, owner.pid, r, done) == 0 && !done);
    CHECK(thread->wait_reason == sched::ThreadWaitReason::futex && thread->wait_key == word);
    CHECK(thread->state == sched::ProcessState::blocked && !thread->native_deadline.armed());
    for (auto entry : native::futex_waits)
        if (entry.pid == owner.pid && entry.tid == owner.tids[index])
            return entry.object;
    CHECK(false);
    return {};
}
i64 wake(Kernel &kernel, Owner owner, usize index, u32 count, uptr address = word)
{
    activate(kernel, owner, index);
    bool done = true;
    const auto copies = mock::copies;
    const auto result =
        native::private_futex(kernel, owner.pid, request(native::FutexOperation::wake, count, address), done);
    CHECK(done && copies == mock::copies);
    return result;
}
void lifetimes(Kernel &kernel)
{
    auto a = owner(kernel, 4), b = owner(kernel, 2);
    put(128, 4, 7);
    auto first = wait(kernel, a, 1), second = wait(kernel, a, 2);
    auto foreign = wait(kernel, b, 1);
    CHECK(wake(kernel, a, 0, 9, word + 4) == 0);
    CHECK(wake(kernel, a, 0, 0) == 1);
    CHECK(!native::futex_objects.resolve(first) && native::futex_objects.resolve(second));
    auto *peer = kernel.scheduler().find_thread(a.pid, a.tids[1]);
    CHECK(!peer->syscall_pending && peer->native_frame.registers[0] == 0);
    CHECK(peer->wait_registration.completion() == sched::WaitCompletion::ready);
    // Wake does not copy/recompare memory even after unmap/remap of the word.
    mock::forbidden = word;
    put(128, 4, 99);
    CHECK(wake(kernel, a, 0, 0xffffffffU) == 1);
    CHECK(!native::futex_objects.resolve(second) && native::futex_objects.resolve(foreign));
    mock::forbidden = 0;
    CHECK(wake(kernel, b, 0, 1) == 1 && !native::futex_objects.resolve(foreign));
    put(128, 4, 7);
    auto deadline = wait(kernel, a, 1, true);
    CHECK(native::expire_futex(kernel, mock::now + 16));
    CHECK(native::futex_objects.resolve(deadline));
    CHECK(!native::expire_futex(kernel, mock::now + 17));
    CHECK(!native::futex_objects.resolve(deadline));
    CHECK(!peer->syscall_pending && static_cast<i64>(peer->native_frame.registers[0]) == -110);
    CHECK(peer->wait_registration.completion() == sched::WaitCompletion::timeout);
    CHECK(wake(kernel, a, 0, 1) == 0);
    auto early = wait(kernel, a, 1, true);
    CHECK(wake(kernel, a, 0, 1) == 1);
    CHECK(!native::expire_futex(kernel, mock::now + 17));
    CHECK(peer->native_frame.registers[0] == 0 && !native::futex_objects.resolve(early));
    // An old object's cancellation cannot consume a replacement scheduler ticket.
    auto stale = wait(kernel, a, 1);
    auto old_ticket = peer->wait_registration.ticket();
    CHECK(kernel.scheduler().complete_wait(old_ticket, sched::WaitCompletion::cancelled));
    activate(kernel, a, 1);
    auto replacement = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::futex, word);
    CHECK(replacement && replacement.value().generation != old_ticket.generation);
    CHECK(!native::expire_futex(kernel, mock::now));
    CHECK(!native::futex_objects.resolve(stale) && peer->wait_registration.pending(replacement.value()));
    CHECK(peer->native_frame.registers[0] == 0x123456);
    CHECK(kernel.scheduler().complete_wait(replacement.value(), sched::WaitCompletion::cancelled));
    // External cancellation is completed once, never replayed as another WAIT.
    auto cancelled = wait(kernel, a, 1);
    CHECK(kernel.scheduler().complete_wait(peer->wait_registration.ticket(), sched::WaitCompletion::cancelled));
    CHECK(!native::expire_futex(kernel, mock::now));
    CHECK(!native::futex_objects.resolve(cancelled) && !peer->syscall_pending);
    CHECK(static_cast<i64>(peer->native_frame.registers[0]) == -4);
    auto made_runnable = wait(kernel, a, 1);
    CHECK(kernel.scheduler().set_runnable(a.pid));
    CHECK(!native::expire_futex(kernel, mock::now));
    CHECK(!native::futex_objects.resolve(made_runnable) && !peer->syscall_pending);
    CHECK(static_cast<i64>(peer->native_frame.registers[0]) == -4);
    activate(kernel, a, 1);
    auto timed = request(native::FutexOperation::wait); timed.timed = true; timed.timeout = {0,17};
    bool done = true;
    mock::copy_clock_step = 20;
    CHECK(native::private_futex(kernel, a.pid, timed, done) == -110 && done);
    timed.value = 99;
    CHECK(native::private_futex(kernel, a.pid, timed, done) == -11 && done);
    mock::copy_clock_step = 0;
    auto drained = wait(kernel, a, 1);
    native::release_process(kernel, a.pid);
    CHECK(!native::futex_objects.resolve(drained) && !peer->wait_registration.active());
    CHECK(!native::futex_objects.accounting().live_objects && !native::page_pool.object_count());
    CHECK(kernel.scheduler().exit_process(a.pid, 0));
    CHECK(kernel.scheduler().exit_process(b.pid, 0));
}

template <lu::Profile P> void adapter(Kernel &kernel, Owner a, usize number_slot, std::array<usize, 4> slots)
{
    using C = lu::Codec<P>;
    constexpr bool big = C::profile.big_endian;
    auto *thread = kernel.scheduler().find_thread(a.pid, a.tids[0]);
    for (auto op : {lu::Operation::futex, lu::Operation::futex_time64})
    {
        auto number = C::syscall_number(op);
        if (!number)
            continue;
        const usize width =
            C::profile.word_bytes == 8 || number == C::syscall_number(lu::Operation::futex_time64) ? 8 : 4;
        auto stamp = [&](i64 sec, u64 ns)
        {
            put(1024, width, sec, big);
            put(1024 + width, width, ns, big);
        };
        bool done = true;
        auto invoke = [&](u64 operation, u64 value = 7, uptr ptr = word, uptr timeout = 0)
        {
            activate(kernel, a, 0);
            hw::UserFrame frame{};
            frame.registers[number_slot] = *number;
            // MIPS O32 four-argument calls must not inspect an invalid stack.
            frame.sp = 0;
            const std::array<u64, 4> arguments{ptr, operation, value, timeout};
            for (usize i = 0; i < 4; ++i)
                frame.registers[slots[i]] = arguments[i];
            done = true;
            return user::linux_bootstrap::dispatch<P>(kernel, a.pid, frame, done, [&](hw::Syscall call, bool &complete)
                                                      { return native::dispatch(kernel, a.pid, call, complete); });
        };
        auto retire = [&]
        {
            native::cancel_futex(kernel, a.pid);
            activate(kernel, a, 0);
        };
        put(128, 4, 7, big);
        CHECK(invoke(128, 8) == -11 && done);
        stamp(0, 0);
        CHECK(invoke(128, 8, word, time_address) == -11 && done);
        CHECK(invoke(128, 7, word, time_address) == -110 && done);
        CHECK(invoke(128, 7, word + 1, 1) == -14); // Timeout copy first.
        stamp(-1, 0);
        CHECK(invoke(128, 8, word + 1, time_address) == -22);
        stamp(0, 1000000000);
        CHECK(invoke(128, 8, word, time_address) == -22);
        CHECK(invoke(128, 7, word + 1) == -22);
        CHECK(invoke(128, 7, 0) == -14);
        const auto copies = mock::copies;
        for (uptr address : {uptr{0}, uptr{4}, hw::user_begin + 16384, hw::user_end - 4})
            CHECK(invoke(129, 0, address, 1) == 0);
        CHECK(mock::copies == copies);
        CHECK(invoke(129, 1, hw::user_end) == -14);
        CHECK(invoke(129, 1, word + 1) == -22);
        // REQUEUE is covered by the dedicated SDK67 runtime and signal models.
        for (u64 unsupported : {2, 6, 9, 10, 128 + 9, 128 + 256})
            CHECK(invoke(unsupported, 7, 0, 1) == -38);
        // Ordinary operations require canonical backing; this hosted fixture has none.
        CHECK(invoke(0, 8) == -14 && done);
        CHECK(invoke(1, 1, 0) == -14 && done);
        if constexpr (C::profile.word_bytes == 8)
        {
            CHECK(invoke((u64{1} << 32) | 129, u64{1} << 32) == 0);
            CHECK(invoke(128, (u64{1} << 32) | 8) == -11);
        }
        stamp(0, 17);
        CHECK(invoke(128, 7, word, time_address) == 0 && !done);
        CHECK(thread->state == sched::ProcessState::blocked);
        for (auto entry : native::futex_waits)
            if (entry.pid == a.pid)
                CHECK(native::futex_objects.resolve(entry.object).value()->timeout.deadline() == mock::now + 17);
        retire();
        if (width == 8)
        {
            stamp(__INT64_MAX__, 999999999);
            CHECK(invoke(128, 7, word, time_address) == 0 && !done);
            for (auto entry : native::futex_waits)
                if (entry.pid == a.pid)
                    CHECK(native::futex_objects.resolve(entry.object).value()->timeout.deadline() == lu::ktime_maximum);
            retire();
            if constexpr (C::profile.word_bytes == 4)
            {
                stamp(0, 0x1234567800000011ULL);
                CHECK(invoke(128, 7, word, time_address) == 0 && !done);
                retire();
            }
        }
        hw::UserFrame frame{};
        CHECK(user::linux_bootstrap::complete<P>(frame, -110));
        auto decoded = lu::NativeBridge<P>::decode_return(frame);
        constexpr auto timeout_errno = P == lu::Profile::mips || P == lu::Profile::mips64 ? 145 : 110;
        CHECK(decoded && decoded.value().error == timeout_errno);
        CHECK(!native::futex_objects.accounting().live_objects);
    }
}
void pressure(Kernel &kernel)
{
    CHECK(native::activate_pool());
    const auto quota = native::page_pool.user_capacity_count();
    std::array<uptr, native::page_count> users{};
    for (usize i = 0; i < quota; ++i)
    {
        auto p = native::page_pool.allocate();
        CHECK(p);
        users[i] = p.value();
    }
    CHECK(!native::page_pool.allocate());
    // All object quotas coexist despite exhaustion of all user allocations.
    // Raw pipe objects exercise reserve coexistence here, not pipe I/O.
    std::array<memory::ObjectHandle, native::pipe_read_capacity> pipe_reads{};
    for (auto &object : pipe_reads)
    {
        auto made = native::pipe_read_objects.create(); CHECK(made); object = made.value();
    }
    std::array<memory::ObjectHandle, sched::max_processes> polls{};
    for (auto &p : polls)
    {
        auto object = native::poll_objects.create();
        CHECK(object);
        p = object.value();
    }
    std::array<Owner, 9> owners{};
    std::array<memory::ObjectHandle, 64> waits{};
    put(128, 4, 7);
    const auto before = native::futex_objects.accounting();
    usize index = 0;
    for (usize i = 0; i < 8; ++i)
    {
        owners[i] = owner(kernel, 8);
        for (usize j = 0; j < 8; ++j)
            waits[index++] = wait(kernel, owners[i], j);
    }
    // New runtime services and mapping metadata have their own reserve. Hold
    // the unused remainder so this still tests complete physical exhaustion.
    std::array<native::NativePagePool::Lease, native::NativePagePool::capacity> reserve_holds{};
    const auto unused_reserve = native::page_pool.free_count();
    for (usize i = 0; i < unused_reserve; ++i)
    {
        auto held = native::page_pool.allocate_kernel_page(); CHECK(held);
        reserve_holds[i] = std::move(held.value());
    }
    CHECK(!native::page_pool.allocate_kernel_page());
    CHECK(native::page_pool.free_count() == 0 && native::page_pool.object_count() == native::kernel_reserve_pages);
    owners[8] = owner(kernel, 1);
    activate(kernel, owners[8], 0);
    auto *thread = kernel.scheduler().current_thread();
    const auto ticket = thread->wait_registration.ticket();
    const auto allocations = native::page_pool.allocations();
    bool done = true;
    CHECK(native::private_futex(kernel, owners[8].pid, request(native::FutexOperation::wait), done) == -12 && done);
    CHECK(thread->state == sched::ProcessState::running && thread->wait_registration.ticket() == ticket);
    CHECK(native::page_pool.allocations() == allocations);
    native::cancel_futex(kernel, waits[0]);
    auto replacement = wait(kernel, owners[8], 0);
    CHECK(!native::futex_objects.resolve(waits[0]) && replacement != waits[0]);
    for (auto a : owners)
    {
        native::release_process(kernel, a.pid);
        CHECK(kernel.scheduler().exit_process(a.pid, 0));
    }
    CHECK(!native::futex_objects.accounting().live_objects && !native::futex_objects.accounting().backing_pages);
    CHECK(native::futex_objects.accounting().allocations - before.allocations == 65);
    CHECK(native::futex_objects.accounting().destructions - before.destructions == 65);
    CHECK(native::page_pool.free_count() == native::futex_object_pages);
    for (auto p : polls)
        CHECK(native::poll_objects.destroy(p));
    CHECK(native::poll_objects.trim_empty_pages());
    CHECK(native::page_pool.free_count() == native::futex_object_pages + native::poll_object_pages);
    for (auto object : pipe_reads) CHECK(native::pipe_read_objects.destroy(object));
    CHECK(native::pipe_read_objects.trim_empty_pages());
    CHECK(native::page_pool.free_count() == native::futex_object_pages + native::poll_object_pages + native::pipe_read_object_pages);
    for (usize i = 0; i < unused_reserve; ++i) CHECK(native::page_pool.release_kernel_page(reserve_holds[i]));
    CHECK(native::page_pool.free_count() == native::kernel_reserve_pages);
    CHECK(native::page_pool.object_count() == 0 && native::page_pool.quarantined_kernel_page_count() == 0);
    for (usize i = 0; i < quota; ++i)
        CHECK(native::page_pool.release(users[i]));
    CHECK(native::page_pool.allocations() == native::page_pool.releases());
}
} // namespace
int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    lifetimes(kernel);
    auto a = owner(kernel, 1);
    adapter<lu::Profile::i386>(kernel, a, 7, {4, 6, 5, 1});
    adapter<lu::Profile::x86_64>(kernel, a, 14, {9, 10, 11, 5});
    adapter<lu::Profile::aarch64>(kernel, a, 8, {0, 1, 2, 3});
    adapter<lu::Profile::arm32>(kernel, a, 7, {0, 1, 2, 3});
    adapter<lu::Profile::rv64>(kernel, a, 17, {10, 11, 12, 13});
    adapter<lu::Profile::rv32>(kernel, a, 17, {10, 11, 12, 13});
    adapter<lu::Profile::loongarch64>(kernel, a, 11, {4, 5, 6, 7});
    adapter<lu::Profile::mips>(kernel, a, 2, {4, 5, 6, 7});
    adapter<lu::Profile::mips64>(kernel, a, 2, {4, 5, 6, 7});
    adapter<lu::Profile::ppc>(kernel, a, 0, {3, 4, 5, 6});
    CHECK(kernel.scheduler().exit_process(a.pid, 0));
    pressure(kernel);
    CHECK(native::runtime_status());
    std::puts("PASS hosted private futex: profiles=10 registrations=64 overflow=ENOMEM destructions=65 guest=0 "
              "hardware_isolation=0 smp=0");
}
