#include "ok/arch/execution.hpp"
#include "execution_table_pages.hpp"
namespace hw = ok::arch::execution;
using namespace ok;
extern "C" const std::byte ok_rv32_probe_begin[], ok_rv32_probe_timer[], ok_rv32_probe_write_fault[],
    ok_rv32_probe_kernel_fault[], ok_rv32_probe_execute_fault[], ok_rv32_probe_float[], ok_rv32_probe_memory[],
    ok_rv32_probe_end[];
namespace
{
alignas(4096) std::array<std::byte, 4096> code{}, stack{}, other_stack{};
void emit(const char *text)
{
    auto *uart = reinterpret_cast<volatile u8 *>(0x10000000);
    while (*text)
        *uart = static_cast<u8>(*text++);
}
[[noreturn]] void end(bool success)
{
    *reinterpret_cast<volatile u32 *>(0x100000) = success ? 0x5555u : 0x3333u;
    for (;;)
        asm volatile("wfi");
}
void check(bool success, const char *message)
{
    if (!success)
    {
        emit("RV32_NATIVE_FAIL ");
        emit(message);
        emit("\n");
        end(false);
    }
}
uptr entry(const std::byte *label)
{
    return hw::user_begin + static_cast<uptr>(label - ok_rv32_probe_begin);
}
// Timer delivery may precede a syscall or fault on a busy QEMU host.
Result<hw::Trap> run_until_event(hw::AddressSpace &space, hw::UserFrame &frame)
{
    auto result = hw::enter(space, frame);
    for (usize retry = 0; result && result.value().kind == hw::TrapKind::timer && retry < 32; ++retry)
        result = hw::enter(space, frame);
    return result;
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
    emit("RV32_NATIVE_BEGIN\n");
    check(hw::initialize().ok(), "initialize");
    for (usize i = 0; i < static_cast<usize>(ok_rv32_probe_end - ok_rv32_probe_begin); ++i)
        code[i] = ok_rv32_probe_begin[i];
    const std::array<hw::PageMapping, 2> pages{
        {{hw::user_begin, reinterpret_cast<uptr>(code.data()), hw::page_read | hw::page_execute},
         {hw::user_begin + 0x1000, reinterpret_cast<uptr>(stack.data()), hw::page_read | hw::page_write}}};
    auto result = hw::create_space(pages);
    check(result.ok(), "create space");
    auto space = result.value();
    auto frame = hw::initial_frame(hw::user_begin, hw::user_begin + 0x2000 - 16);
    auto trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 65 &&
              hw::syscall(frame).arguments[0] == 42 && frame.registers[3] == 0x12345 && frame.registers[4] == 0x67890,
          "U ecall");
    hw::set_return_value(frame, 81);
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 66 &&
              hw::syscall(frame).arguments[0] == 82 && frame.registers[3] == 0x12345 && frame.registers[4] == 0x67890,
          "syscall return");
    emit("RV32_NATIVE syscall=pass return=pass\n");
    frame = hw::initial_frame(entry(ok_rv32_probe_timer), hw::user_begin + 0x2000 - 16);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::timer && frame.registers[8] != 0, "timer");
    const auto count = frame.registers[8];
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::timer && frame.registers[8] > count, "timer resume");
    emit("RV32_NATIVE timer=pass context=pass\n");
    frame = hw::initial_frame(entry(ok_rv32_probe_write_fault), hw::user_begin + 0x2000 - 16);
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault && trap.value().vector == 15, "read-only protection");
    frame = hw::initial_frame(entry(ok_rv32_probe_kernel_fault), hw::user_begin + 0x2000 - 16);
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault && trap.value().vector == 13 &&
              trap.value().fault_address == 0x80000000,
          "kernel isolation");
    frame = hw::initial_frame(entry(ok_rv32_probe_execute_fault), hw::user_begin + 0x2000 - 16);
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault && trap.value().vector == 12 &&
              trap.value().fault_address == hw::user_begin + 0x1000,
          "non-executable data page");
    uptr kernel_status = 0;
    asm volatile("csrr %0, mstatus" : "=r"(kernel_status));
    const uptr fp_enabled = kernel_status | 0x6000;
    asm volatile("csrw mstatus, %0" ::"r"(fp_enabled) : "memory");
    asm volatile("fmv.w.x fs0, %0" ::"r"(uptr{0xabcdef}) : "memory");
    frame = hw::initial_frame(entry(ok_rv32_probe_float), hw::user_begin + 0x2000 - 16);
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 67, "FP store");
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).arguments[0] == 123 &&
              hw::syscall(frame).arguments[1] == 456 && hw::syscall(frame).arguments[2] == 789 &&
              hw::syscall(frame).arguments[3] == 1023,
          "full-width FP restore");
    uptr kernel_fp = 0;
    asm volatile("fmv.x.w %0, fs0" : "=r"(kernel_fp));
    check(kernel_fp == 0xabcdef, "kernel callee-saved FP");
    asm volatile("csrw mstatus, %0" ::"r"(kernel_status) : "memory");
    emit("RV32_NATIVE protection=pass fp=pass\n");
    auto other_pages = pages;
    other_pages[1].physical_address = reinterpret_cast<uptr>(other_stack.data());
    auto second = hw::create_space(other_pages);
    check(second.ok(), "second space");
    const u32 value = 0x12345678;
    u32 copied = 1;
    check(hw::copy_to_user(space, hw::user_begin + 0x1000, &value, sizeof(value)).ok(), "copy to user");
    check(hw::copy_from_user(second.value(), &copied, hw::user_begin + 0x1000, sizeof(copied)).ok() && copied == 0,
          "space isolation");
    auto memory_a = hw::initial_frame(entry(ok_rv32_probe_memory), hw::user_begin + 0x2000 - 16);
    auto memory_b = hw::initial_frame(entry(ok_rv32_probe_memory), hw::user_begin + 0x2000 - 16);
    trap = run_until_event(space, memory_a);
    check(trap && trap.value().kind == hw::TrapKind::syscall && memory_a.registers[10] == value + 1,
          "first space U load/store");
    trap = run_until_event(second.value(), memory_b);
    check(trap && trap.value().kind == hw::TrapKind::syscall && memory_b.registers[10] == 1,
          "second space U load/store");
    trap = run_until_event(space, memory_a);
    check(trap && trap.value().kind == hw::TrapKind::syscall && memory_a.registers[10] == value + 2,
          "SATP switch back");
    check(hw::copy_from_user(second.value(), &copied, hw::user_begin + 0x1000, sizeof(copied)).ok() && copied == 1,
          "second space survived other user writes");
    emit("RV32_NATIVE address_spaces=pass\n");
    check(!hw::copy_to_user(space, hw::user_begin, &value, sizeof(value)), "reject read-only copy");
    check(!hw::user_range(space, hw::user_begin + 0x1fff, 2, true), "reject unmapped boundary");
    check(hw::copy_from_user(space, nullptr, 0, 0).ok(), "empty null copy");
    check(!hw::copy_from_user(space, nullptr, hw::user_begin, 1), "null kernel copy target");
    auto invalid = pages;
    invalid[0].flags |= hw::page_write;
    check(!hw::create_space(invalid), "reject writable executable page");
    auto bad_frame = hw::initial_frame(hw::user_begin, hw::user_begin + 0x4000);
    // Like Linux, entry does not need a mapped stack; this probe reaches its
    // syscall without touching one.
    auto unmapped = hw::enter(space, bad_frame);
    while (unmapped && unmapped.value().kind == hw::TrapKind::timer) unmapped = hw::enter(space, bad_frame);
    check(unmapped && unmapped.value().kind == hw::TrapKind::syscall, "enter with an unmapped stack");
    const auto stale = space;
    check(hw::destroy_space(space).ok(), "destroy");
    check(!hw::user_range(stale, hw::user_begin, 1), "reject stale space");
    check(hw::destroy_space(second.value()).ok(), "second destroy");
    emit("RV32_NATIVE_PASS scope=hardware\n");
    end(true);
}
