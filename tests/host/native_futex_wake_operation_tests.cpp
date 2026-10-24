// Actual scheduler/runtime/ABI execution with hosted copies; no guest or SMP.
#include "native_clock_model.hpp"
#include "ok/abi/linux_futex_wake_operation.hpp"

namespace
{
constexpr uptr first = hw::user_begin + 128, second = first + 4;
constexpr u32 encode(u32 op, u32 cmp, i32 arg, i32 cmp_arg)
{
    return (op << 28) | (cmp << 24) | ((static_cast<u32>(arg) & 0xfffU) << 12) |
        (static_cast<u32>(cmp_arg) & 0xfffU);
}
void put(uptr address, u32 value, bool big = false)
{
    for (usize i = 0; i < 4; ++i)
        mock::memory[address - hw::user_begin + i] = static_cast<std::byte>(value >> (8 * (big ? 3 - i : i)));
}
u32 get(uptr address, bool big = false)
{
    u32 result{};
    for (usize i = 0; i < 4; ++i)
        result |= static_cast<u32>(mock::memory[address - hw::user_begin + i]) << (8 * (big ? 3 - i : i));
    return result;
}
struct Owner
{
    sched::ProcessId pid{};
    std::array<sched::ThreadId, 8> tids{};
};
Owner owner(Kernel &kernel, usize count)
{
    auto created = kernel.scheduler().create_user_process("wake-operation-model",
        kernel.arch().make_user_context({hw::user_begin, hw::user_end - hw::page_size}));
    CHECK(created);
    Owner result{created.value(), {}};
    auto *process = kernel.scheduler().find(result.pid);
    result.tids[0] = process->threads()[0].tid;
    for (usize i = 1; i < count; ++i)
    {
        const auto made = kernel.scheduler().create_thread(result.pid, process->threads()[0].context);
        CHECK(made); result.tids[i] = made.value();
    }
    CHECK(kernel.scheduler().set_runnable(result.pid));
    for (auto &thread : process->threads()) thread.native_active = true;
    return result;
}
void activate(Kernel &kernel, Owner a, usize index = 0)
{
    CHECK(kernel.scheduler().activate_thread(a.pid, a.tids[index]));
}
memory::ObjectHandle wait(Kernel &kernel, Owner a, usize index, uptr address)
{
    activate(kernel, a, index);
    kernel.scheduler().current_thread()->syscall_pending = true;
    bool complete = true;
    CHECK(native::private_futex(kernel, a.pid,
        {native::FutexOperation::wait, address, get(address), {}, false, false}, complete) == 0 && !complete);
    for (auto entry : native::futex_waits)
        if (entry.pid == a.pid && entry.tid == a.tids[index]) return entry.object;
    CHECK(false); return {};
}
i64 execute(Kernel &kernel, Owner a, u32 op, u32 limit1 = 1, u32 limit2 = 1,
            uptr address1 = first, uptr address2 = second, bool big = false)
{
    activate(kernel, a);
    return native::futex_wake_operation(kernel, a.pid, {address1, address2, limit1, limit2, op, big});
}
void arithmetic()
{
    using Operation = lu::FutexWakeOperation;
    for (i32 value = -2048; value < 2048; ++value)
    {
        const auto decoded = Operation::decode(encode(1, 0, value, value));
        CHECK(decoded.operand == value && decoded.compare_operand == value);
    }
    const std::array<u32, 5> expected{0xfffffffeU, 0xfffffffdU, 0xffffffffU, 1U, 1U};
    for (u32 op = 0; op < expected.size(); ++op)
    {
        const auto result = Operation::decode(encode(op, 0, -2, 0)).modify(0xffffffffU);
        CHECK(result && result.value() == expected[op]);
    }
    CHECK(Operation::decode(encode(1, 0, 1, 0)).modify(0xffffffffU).value() == 0);
    for (auto shift : {-2048, -1, 0, 31, 32, 2047})
    {
        const auto result = Operation::decode(encode(8, 0, shift, 0)).modify(0);
        CHECK(result && result.value() == (u32{1} << (static_cast<u32>(shift) & 31U)));
    }
    for (u32 op : {5U, 6U, 7U, 13U, 14U, 15U}) CHECK(!Operation::decode(encode(op, 0, 0, 0)).modify(0));
    const std::array<bool, 6> negative{true, false, false, true, false, true};
    const std::array<bool, 6> less{false, true, true, true, false, false};
    const std::array<bool, 6> greater{false, true, false, false, true, true};
    for (u32 cmp = 0; cmp < 6; ++cmp)
    {
        CHECK(Operation::decode(encode(0, cmp, 0, -1)).compare(0xffffffffU).value() == negative[cmp]);
        CHECK(Operation::decode(encode(0, cmp, 0, 0)).compare(0x80000000U).value() == less[cmp]);
        CHECK(Operation::decode(encode(0, cmp, 0, 0)).compare(0x7fffffffU).value() == greater[cmp]);
    }
    for (u32 cmp = 6; cmp < 16; ++cmp) CHECK(!Operation::decode(encode(0, cmp, 0, 0)).compare(0));
}
void runtime(Kernel &kernel)
{
    const auto a = owner(kernel, 8), b = owner(kernel, 2);
    put(first, 7); put(second, 7);
    auto one = wait(kernel, a, 1, first), two = wait(kernel, a, 2, second);
    auto foreign = wait(kernel, b, 1, first);
    const auto objects = native::futex_objects.accounting().allocations;
    CHECK(execute(kernel, a, encode(1, 0, 3, 7)) == 2 && get(second) == 10);
    CHECK(!native::futex_objects.resolve(one) && !native::futex_objects.resolve(two));
    CHECK(native::futex_objects.resolve(foreign));
    CHECK(native::futex_objects.accounting().allocations == objects);
    CHECK(kernel.scheduler().find_thread(a.pid, a.tids[1])->wait_registration.completion() == sched::WaitCompletion::ready);
    CHECK(kernel.scheduler().find_thread(a.pid, a.tids[2])->wait_registration.completion() == sched::WaitCompletion::ready);
    one = wait(kernel, a, 1, first); two = wait(kernel, a, 2, second);
    CHECK(execute(kernel, a, encode(0, 0, 7, 99)) == 1 && get(second) == 7);
    CHECK(!native::futex_objects.resolve(one) && native::futex_objects.resolve(two));
    CHECK(execute(kernel, a, encode(0, 0, 7, 7), 1, 0xffffffffU) == 1);
    CHECK(!native::futex_objects.resolve(two));
    one = wait(kernel, a, 1, first); two = wait(kernel, a, 2, first);
    auto three = wait(kernel, a, 3, first);
    CHECK(execute(kernel, a, encode(1, 0, 1, 7), 0, 1, first, first) == 2 && get(first) == 8);
    CHECK(!native::futex_objects.resolve(one) && !native::futex_objects.resolve(two));
    CHECK(native::futex_objects.resolve(three));
    const auto check_pending = [&] { CHECK(native::futex_objects.resolve(three) && native::futex_objects.resolve(foreign)); };
    put(second, 7);
    mock::forbidden = second;
    CHECK(execute(kernel, a, encode(0, 0, 4, 7)) == -14 && get(second) == 7); check_pending();
    CHECK(execute(kernel, a, encode(5, 0, 4, 7)) == -38 && get(second) == 7); check_pending();
    mock::forbidden = 0; mock::write_forbidden = second;
    CHECK(execute(kernel, a, encode(0, 0, 4, 7)) == -14 && get(second) == 7); check_pending();
    CHECK(execute(kernel, a, encode(5, 0, 4, 7)) == -38 && get(second) == 7); check_pending();
    mock::write_forbidden = 0;
    CHECK(execute(kernel, a, encode(5, 0, 4, 7)) == -38 && get(second) == 7); check_pending();
    CHECK(execute(kernel, a, encode(0, 6, 4, 7)) == -38 && get(second) == 4); check_pending();
    CHECK(execute(kernel, a, encode(0, 0, 1, 0), 1, 1, first + 1) == -22); check_pending();
    CHECK(execute(kernel, a, encode(0, 0, 1, 0), 1, 1, first, hw::user_end) == -14); check_pending();
    CHECK(execute(kernel, a, encode(0, 0, 1, 0), 1, 1, first, second + 1) == -22); check_pending();
    put(second, 0x12345678U, true);
    CHECK(execute(kernel, a, encode(4, 0, -1, 0), 1, 1, first + 8, second, true) == 0);
    CHECK(get(second, true) == 0xedcba987U); check_pending();
    native::cancel_futex(kernel, a.pid); native::cancel_futex(kernel, b.pid);
    CHECK(kernel.scheduler().exit_process(a.pid, 0)); CHECK(kernel.scheduler().exit_process(b.pid, 0));
    CHECK(!native::futex_objects.accounting().live_objects);
}
template <lu::Profile P> void adapter(Kernel &kernel, Owner a, usize number_slot, std::array<usize, 6> slots)
{
    using Codec = lu::Codec<P>;
    constexpr bool big = Codec::profile.big_endian;
    for (auto call : {lu::Operation::futex, lu::Operation::futex_time64})
    {
        const auto number = Codec::syscall_number(call);
        if (!number) continue;
        const auto invoke = [&](u32 operation, uptr stack = hw::user_begin + 1024) {
            activate(kernel, a);
            hw::UserFrame frame{}; frame.registers[number_slot] = *number; frame.sp = stack;
            const std::array<u64, 6> arguments{first, operation, 0, 0xffffffffU, second, encode(1, 0, -1, 9)};
            for (usize i = 0; i < (P == lu::Profile::mips ? 4U : 6U); ++i) frame.registers[slots[i]] = arguments[i];
            if constexpr (P == lu::Profile::mips)
            {
                put(hw::user_begin + 1040, static_cast<u32>(arguments[4]), big);
                put(hw::user_begin + 1044, static_cast<u32>(arguments[5]), big);
            }
            bool complete = true;
            const auto result = user::linux_bootstrap::dispatch<P>(kernel, a.pid, frame, complete,
                [&](hw::Syscall request, bool &done) { return native::dispatch(kernel, a.pid, request, done); });
            CHECK(complete); return result;
        };
        put(second, 9, big);
        CHECK(invoke(133) == 0 && get(second, big) == 8);
        CHECK(invoke(5 + 256) == -38 && get(second, big) == 8);
        if constexpr (P == lu::Profile::mips)
        {
            CHECK(invoke(133, 0) == -14 && get(second, big) == 8);
            // The baseline four-input WAKE never fetches an unused O32 tail.
            CHECK(invoke(129, 0) == 0 && get(second, big) == 8);
        }
    }
}
} // namespace
int main()
{
    arithmetic();
    static Kernel kernel; CHECK(kernel.boot({}));
    runtime(kernel);
    const auto a = owner(kernel, 1);
    adapter<lu::Profile::i386>(kernel, a, 7, {4, 6, 5, 1, 0, 2});
    adapter<lu::Profile::x86_64>(kernel, a, 14, {9, 10, 11, 5, 7, 6});
    adapter<lu::Profile::aarch64>(kernel, a, 8, {0, 1, 2, 3, 4, 5});
    adapter<lu::Profile::arm32>(kernel, a, 7, {0, 1, 2, 3, 4, 5});
    adapter<lu::Profile::rv64>(kernel, a, 17, {10, 11, 12, 13, 14, 15});
    adapter<lu::Profile::rv32>(kernel, a, 17, {10, 11, 12, 13, 14, 15});
    adapter<lu::Profile::loongarch64>(kernel, a, 11, {4, 5, 6, 7, 8, 9});
    adapter<lu::Profile::mips>(kernel, a, 2, {4, 5, 6, 7, 0, 0});
    adapter<lu::Profile::mips64>(kernel, a, 2, {4, 5, 6, 7, 8, 9});
    adapter<lu::Profile::ppc>(kernel, a, 0, {3, 4, 5, 6, 7, 8});
    CHECK(kernel.scheduler().exit_process(a.pid, 0));
    CHECK(native::runtime_status());
    std::puts("PASS hosted FUTEX_WAKE_OP profiles=10 runtime=pass arithmetic=pass guest=0 hardware_isolation=0 smp=0");
}
