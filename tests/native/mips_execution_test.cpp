#include "ok/arch/execution.hpp"
#include "execution_table_pages.hpp"
namespace hw = ok::arch::execution;
using namespace ok;
extern "C" const std::byte ok_mips_probe_begin[], ok_mips_probe_end[], ok_mips_probe_timer[], ok_mips_probe_readonly[],
    ok_mips_probe_kernel[], ok_mips_probe_unmapped[], ok_mips_probe_float[], ok_mips_probe_memory[],
    ok_mips_probe_tls[], ok_mips_probe_delay[], ok_mips_probe_invalid[], ok_mips_probe_capture[],
    ok_mips_probe_tls_delay[], ok_mips_probe_nx[];
namespace
{
alignas(4096) std::array<std::byte, 4096> code{}, stack{}, other_stack{};
void emit(const char *text)
{
    constexpr uptr uart = static_cast<uptr>(static_cast<__INTPTR_TYPE__>(-1207958536LL));
    while (*text)
    {
        while (!(*reinterpret_cast<volatile u8 *>(uart + 5) & 32))
        {
        }
        *reinterpret_cast<volatile u8 *>(uart) = static_cast<u8>(*text++);
    }
}
[[noreturn]] void end()
{
    for (;;)
        asm volatile("nop");
}
void check(bool success, const char *message)
{
    if (!success)
    {
        emit("MIPS_NATIVE_FAIL ");
        emit(message);
        emit("\n");
        end();
    }
}
uptr entry(const std::byte *label)
{
    return hw::user_begin + static_cast<uptr>(label - ok_mips_probe_begin);
}
hw::Trap run(hw::AddressSpace &space, hw::UserFrame &frame)
{
    for (usize i = 0; i < 10; ++i)
    {
        auto result = hw::enter(space, frame);
        check(result.ok(), "enter status");
        if (result.value().kind != hw::TrapKind::timer)
            return result.value();
    }
    check(false, "too many timers");
    return {};
}
} // namespace
extern "C" void *memcpy(void *to, const void *from, usize size)
{
    auto *out = static_cast<volatile std::byte *>(to);
    const auto *in = static_cast<const std::byte *>(from);
    for (usize i = 0; i < size; ++i)
        out[i] = in[i];
    return to;
}
extern "C" void *memset(void *to, int value, usize size)
{
    auto *out = static_cast<volatile u8 *>(to);
    for (usize i = 0; i < size; ++i)
        out[i] = static_cast<u8>(value);
    return to;
}
extern "C" [[noreturn]] void kernel_main()
{
    emit("MIPS_NATIVE_BEGIN\n");
    check(hw::initialize().ok(), "initialize");
    check(static_cast<usize>(ok_mips_probe_end - ok_mips_probe_begin) <= code.size(), "probe fits page");
    for (usize i = 0; i < static_cast<usize>(ok_mips_probe_end - ok_mips_probe_begin); ++i)
        code[i] = ok_mips_probe_begin[i];
    const std::array<hw::PageMapping, 2> pages{
        {{hw::user_begin, reinterpret_cast<uptr>(code.data()), hw::page_read | hw::page_execute},
         {hw::user_begin + 4096, reinterpret_cast<uptr>(stack.data()), hw::page_read | hw::page_write}}};
    auto created = hw::create_space(pages);
    check(created.ok(), "create space");
    auto space = created.value();
    constexpr uptr sp = hw::user_begin + 8192 - 16;
    auto frame = hw::initial_frame(hw::user_begin, sp);
    auto trap = run(space, frame);
    check(trap.kind == hw::TrapKind::syscall && hw::syscall(frame).number == 65 &&
              hw::syscall(frame).arguments[0] == 42 && frame.registers[28] == 0x12345,
          "user syscall");
    hw::set_return_value(frame, 81);
    trap = run(space, frame);
    check(trap.kind == hw::TrapKind::syscall && hw::syscall(frame).number == 66 &&
              hw::syscall(frame).arguments[0] == 82 && frame.registers[28] == 0x12345 && frame.registers[17] == 0x123 &&
              frame.registers[18] == 0x456 && frame.sp == sp,
          "return GPR HI LO");
    emit("MIPS_NATIVE syscall=pass return=pass hilo=pass\n");
    frame = hw::initial_frame(entry(ok_mips_probe_capture), sp);
    for (usize i = 1; i < 32; ++i)
        if (i != 26 && i != 27 && i != 29)
            frame.registers[i] = (uptr{0x76543210} << (sizeof(uptr) == 8 ? 32 : 0)) | i;
    for (usize i = 0; i < 256; ++i)
        frame.fp_state[i] = static_cast<u8>(i);
    const u32 rounding = 1;
    memcpy(frame.fp_state.data() + 256, &rounding, sizeof(rounding));
    const auto saved_frame = frame;
    trap = run(space, frame);
    check(trap.kind == hw::TrapKind::syscall, "register snapshot syscall");
    for (usize i = 1; i < 32; ++i)
        if (i != 26 && i != 27)
            check(frame.registers[i] == saved_frame.registers[i], "all ABI integer registers");
    for (usize i = 0; i < 260; ++i)
        check(frame.fp_state[i] == saved_frame.fp_state[i], "all FP registers and FCSR");
    emit("MIPS_NATIVE abi_gpr=pass abi_fp=pass fcsr=pass\n");
    frame = hw::initial_frame(entry(ok_mips_probe_timer), sp);
    auto timed = hw::enter(space, frame);
    check(timed && timed.value().kind == hw::TrapKind::timer && frame.registers[16] != 0, "timer");
    const auto count = frame.registers[16];
    timed = hw::enter(space, frame);
    check(timed && timed.value().kind == hw::TrapKind::timer && frame.registers[16] > count, "timer resume");
    emit("MIPS_NATIVE timer=pass context=pass\n");
    frame = hw::initial_frame(entry(ok_mips_probe_readonly), sp);
    trap = run(space, frame);
    check(trap.kind == hw::TrapKind::page_fault && trap.vector == 1 && trap.fault_address == hw::user_begin,
          "read-only protection");
    frame = hw::initial_frame(entry(ok_mips_probe_kernel), sp);
    trap = run(space, frame);
    check(trap.kind == hw::TrapKind::page_fault && trap.vector == 4 &&
              static_cast<u32>(trap.fault_address) == 0x80000000,
          "kernel isolation");
    frame = hw::initial_frame(entry(ok_mips_probe_unmapped), sp);
    trap = run(space, frame);
    check(trap.kind == hw::TrapKind::page_fault && trap.vector == 2 && trap.fault_address == 0x60000000,
          "unmapped protection");
    frame = hw::initial_frame(entry(ok_mips_probe_invalid), sp);
    check(run(space, frame).kind == hw::TrapKind::exception, "user breakpoint");
    emit("MIPS_NATIVE protection=pass nx=unavailable\n");
    asm volatile("mtc1 %0, $f20" ::"r"(12345) : "$f20");
    frame = hw::initial_frame(entry(ok_mips_probe_float), sp);
    trap = run(space, frame);
    check(trap.kind == hw::TrapKind::syscall && hw::syscall(frame).number == 67, "FP store");
    trap = run(space, frame);
    check(trap.kind == hw::TrapKind::syscall && hw::syscall(frame).arguments[0] == 123, "FP restore");
    int kernel_fp;
    asm volatile("mfc1 %0, $f20" : "=r"(kernel_fp));
    check(kernel_fp == 12345, "kernel FP preserved");
    frame = hw::initial_frame(entry(ok_mips_probe_tls), sp);
    frame.fs_base = 0x76543210;
    trap = run(space, frame);
    check(trap.kind == hw::TrapKind::syscall && hw::syscall(frame).arguments[0] == frame.fs_base, "TLS RDHWR");
    frame = hw::initial_frame(entry(ok_mips_probe_tls_delay), sp);
    frame.fs_base = 0x65432100;
    trap = run(space, frame);
    check(trap.kind == hw::TrapKind::syscall && hw::syscall(frame).number == 73 &&
              hw::syscall(frame).arguments[0] == frame.fs_base,
          "TLS in branch delay slot");
    emit("MIPS_NATIVE fp=pass tls=pass\n");
    frame = hw::initial_frame(entry(ok_mips_probe_delay), sp);
    check(run(space, frame).kind == hw::TrapKind::syscall && hw::syscall(frame).number == 71, "delay syscall");
    check(run(space, frame).kind == hw::TrapKind::syscall && hw::syscall(frame).number == 72, "delay continuation");
    auto other_pages = pages;
    other_pages[1].physical_address = reinterpret_cast<uptr>(other_stack.data());
    auto second = hw::create_space(other_pages);
    check(second.ok(), "second space");
    const u32 value = 0x12345678;
    u32 copied = 1;
    check(hw::copy_to_user(space, hw::user_begin + 4096, &value, sizeof(value)).ok(), "copy to user");
    check(hw::copy_from_user(second.value(), &copied, hw::user_begin + 4096, sizeof(copied)).ok() && copied == 0,
          "space isolation");
    auto memory_a = hw::initial_frame(entry(ok_mips_probe_memory), sp);
    auto memory_b = hw::initial_frame(entry(ok_mips_probe_memory), sp);
    trap = run(space, memory_a);
    check(trap.kind == hw::TrapKind::syscall && memory_a.registers[4] == value + 1, "first user load/store");
    trap = run(second.value(), memory_b);
    check(trap.kind == hw::TrapKind::syscall && memory_b.registers[4] == 1, "second user load/store");
    trap = run(space, memory_a);
    check(trap.kind == hw::TrapKind::syscall && memory_a.registers[4] == value + 2, "TLB switch back");
    check(hw::copy_from_user(second.value(), &copied, hw::user_begin + 4096, sizeof(copied)).ok() && copied == 1,
          "second space survived other writes");
    emit("MIPS_NATIVE address_spaces=pass delay_slot=pass\n");
    // Prove the documented hardware limitation instead of claiming NX coverage.
    // A branch into a writable data page executes its syscall on these CPUs.
    const u32 syscall_instruction = 0x0000000c;
    check(hw::copy_to_user(space, hw::user_begin + 4096, &syscall_instruction, sizeof(syscall_instruction)).ok(),
          "NX limitation instruction");
    frame = hw::initial_frame(entry(ok_mips_probe_nx), sp);
    check(run(space, frame).kind == hw::TrapKind::syscall, "hardware NX limitation remains explicit");
    check(!hw::copy_to_user(space, hw::user_begin, &value, sizeof(value)), "reject read-only copy");
    check(!hw::user_range(space, hw::user_begin + 8191, 2, true), "reject unmapped boundary");
    check(hw::copy_from_user(space, nullptr, 0, 0).ok(), "empty null copy");
    check(!hw::copy_from_user(space, nullptr, hw::user_begin, 1), "null destination");
    auto invalid = pages;
    invalid[0].flags |= hw::page_write;
    check(!hw::create_space(invalid), "reject writable executable mapping");
    // Like Linux, entry does not need a mapped stack; this probe reaches its
    // syscall without touching one.
    frame = hw::initial_frame(hw::user_begin, hw::user_begin + 16384);
    check(run(space, frame).kind == hw::TrapKind::syscall, "enter with an unmapped stack");
    const auto stale = space;
    check(hw::destroy_space(space).ok() && !hw::user_range(stale, hw::user_begin, 1), "destroy/stale");
    check(hw::destroy_space(second.value()).ok(), "second destroy");
    emit("MIPS_NATIVE_PASS scope=hardware\n");
#ifdef OK_NATIVE_KERNEL_FAULT
    static_cast<void>(*reinterpret_cast<volatile u32 *>(0x1000));
#endif
    end();
}
