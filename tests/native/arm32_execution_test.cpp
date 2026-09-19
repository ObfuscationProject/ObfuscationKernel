#include "ok/arch/execution.hpp"
namespace hw = ok::arch::execution;
using namespace ok;
extern "C" const std::byte ok_arm32_probe_begin[], ok_arm32_probe_timer[], ok_arm32_probe_write_fault[],
    ok_arm32_probe_kernel_fault[], ok_arm32_probe_execute_fault[], ok_arm32_probe_float[], ok_arm32_probe_memory[],
    ok_arm32_probe_thumb[], ok_arm32_probe_end[];
namespace
{
alignas(4096) std::array<std::byte, 4096> code{}, stack{}, other_stack{};
void emit(const char *text)
{
    auto *uart = reinterpret_cast<volatile u8 *>(0x09000000);
    while (*text)
        *uart = static_cast<u8>(*text++);
}
[[noreturn]] void end(bool success)
{
    const u32 reason = success ? 0x20026u : 0x20023u;
    asm volatile("mov r0, #0x18\nmov r1, %0\nsvc 0x123456" ::"r"(reason) : "r0", "r1", "memory");
    for (;;)
        asm volatile("wfe");
}
void check(bool success, const char *message)
{
    if (!success)
    {
        emit("ARM32_NATIVE_FAIL ");
        emit(message);
        emit("\n");
        end(false);
    }
}
uptr entry(const std::byte *label)
{
    return hw::user_begin + static_cast<uptr>(label - ok_arm32_probe_begin);
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
    emit("ARM32_NATIVE_BEGIN\n");
    check(hw::initialize().ok(), "initialize");
    for (usize i = 0; i < static_cast<usize>(ok_arm32_probe_end - ok_arm32_probe_begin); ++i)
        code[i] = ok_arm32_probe_begin[i];
    const std::array<hw::PageMapping, 2> pages{
        {{hw::user_begin, reinterpret_cast<uptr>(code.data()), hw::page_read | hw::page_execute},
         {hw::user_begin + 0x1000, reinterpret_cast<uptr>(stack.data()), hw::page_read | hw::page_write}}};
    auto result = hw::create_space(pages);
    check(result.ok(), "create space");
    auto space = result.value();
    auto frame = hw::initial_frame(hw::user_begin, hw::user_begin + 0x2000 - 16);
    frame.fs_base = 0x4321;
    frame.gs_base = 0x9876;
    u32 kernel_tls_before = 0, kernel_tls_after = 0;
    asm volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(kernel_tls_before));
    auto trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 65 &&
              hw::syscall(frame).arguments[0] == 42 && frame.registers[4] == 0x12345 && frame.registers[9] == 0x67890,
          "User SVC");
    check(hw::syscall(frame).arguments[1] == 0x4321 && hw::syscall(frame).arguments[2] == 0x9876 &&
              frame.fs_base == 0x5678,
          "user TLS");
    asm volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(kernel_tls_after));
    check(kernel_tls_before == kernel_tls_after, "kernel TLS restore");
    hw::set_return_value(frame, 81);
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 66 &&
              hw::syscall(frame).arguments[0] == 82 && frame.registers[4] == 0x12345 && frame.registers[9] == 0x67890,
          "syscall return");
    frame = hw::initial_frame(entry(ok_arm32_probe_thumb) | 1u, hw::user_begin + 0x2000 - 16);
    frame.flags |= 0x800; // Resume an EQ IT instruction with Z clear; it must be skipped.
    frame.registers[0] = 88;
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 70 &&
              frame.registers[0] == 88,
          "Thumb IT context");
    emit("ARM32_NATIVE syscall=pass return=pass tls=pass thumb=pass\n");
    frame = hw::initial_frame(entry(ok_arm32_probe_timer), hw::user_begin + 0x2000 - 16);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::timer && frame.registers[4] != 0, "timer");
    const auto count = frame.registers[4];
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::timer && frame.registers[4] > count, "timer resume");
    emit("ARM32_NATIVE timer=pass context=pass\n");
    frame = hw::initial_frame(entry(ok_arm32_probe_write_fault), hw::user_begin + 0x2000 - 16);
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault && trap.value().vector == 4, "read-only protection");
    frame = hw::initial_frame(entry(ok_arm32_probe_kernel_fault), hw::user_begin + 0x2000 - 16);
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault && trap.value().vector == 4 &&
              trap.value().fault_address == 0x40008000,
          "kernel isolation");
    frame = hw::initial_frame(entry(ok_arm32_probe_execute_fault), hw::user_begin + 0x2000 - 16);
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault && trap.value().vector == 3 &&
              trap.value().fault_address == hw::user_begin + 0x1000,
          "non-executable data page");
    u32 kernel_cpacr = 0, kernel_fpexc = 0;
    asm volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(kernel_cpacr));
    const u32 enabled_cpacr = kernel_cpacr | 0x00f00000;
    asm volatile("mcr p15, 0, %0, c1, c0, 2\nisb" ::"r"(enabled_cpacr) : "memory");
    asm volatile(".fpu neon\nvmrs %0, fpexc" : "=r"(kernel_fpexc));
    const u32 enabled_fpexc = (kernel_fpexc | 0x40000000) & ~0x80000000u;
    asm volatile("vmsr fpexc, %0\nvmov d8, %1, %2" ::"r"(enabled_fpexc), "r"(0xabcdefu), "r"(0x12345678u) : "memory");
    frame = hw::initial_frame(entry(ok_arm32_probe_float), hw::user_begin + 0x2000 - 16);
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 67, "FP store");
    trap = run_until_event(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).arguments[0] == 123 &&
              hw::syscall(frame).arguments[1] == 456 && hw::syscall(frame).arguments[2] == 789 &&
              hw::syscall(frame).arguments[3] == 1023,
          "full-width FP restore");
    u32 kernel_fp_low = 0, kernel_fp_high = 0;
    asm volatile("vmov %0, %1, d8" : "=r"(kernel_fp_low), "=r"(kernel_fp_high));
    check(kernel_fp_low == 0xabcdef && kernel_fp_high == 0x12345678, "kernel callee-saved FP");
    asm volatile("vmsr fpexc, %0\nmcr p15, 0, %1, c1, c0, 2\nisb" ::"r"(kernel_fpexc), "r"(kernel_cpacr) : "memory");
    emit("ARM32_NATIVE protection=pass fp=pass\n");
    auto other_pages = pages;
    other_pages[1].physical_address = reinterpret_cast<uptr>(other_stack.data());
    auto second = hw::create_space(other_pages);
    check(second.ok(), "second space");
    const u32 value = 0x12345678;
    u32 copied = 1;
    check(hw::copy_to_user(space, hw::user_begin + 0x1000, &value, sizeof(value)).ok(), "copy to user");
    check(hw::copy_from_user(second.value(), &copied, hw::user_begin + 0x1000, sizeof(copied)).ok() && copied == 0,
          "space isolation");
    auto memory_a = hw::initial_frame(entry(ok_arm32_probe_memory), hw::user_begin + 0x2000 - 16);
    auto memory_b = hw::initial_frame(entry(ok_arm32_probe_memory), hw::user_begin + 0x2000 - 16);
    trap = run_until_event(space, memory_a);
    check(trap && trap.value().kind == hw::TrapKind::syscall && memory_a.registers[0] == value + 1,
          "first space U load/store");
    trap = run_until_event(second.value(), memory_b);
    check(trap && trap.value().kind == hw::TrapKind::syscall && memory_b.registers[0] == 1,
          "second space U load/store");
    trap = run_until_event(space, memory_a);
    check(trap && trap.value().kind == hw::TrapKind::syscall && memory_a.registers[0] == value + 2,
          "TTBR0 switch back");
    check(hw::copy_from_user(second.value(), &copied, hw::user_begin + 0x1000, sizeof(copied)).ok() && copied == 1,
          "second space survived other user writes");
    emit("ARM32_NATIVE address_spaces=pass\n");
    check(!hw::copy_to_user(space, hw::user_begin, &value, sizeof(value)), "reject read-only copy");
    check(!hw::user_range(space, hw::user_begin + 0x1fff, 2, true), "reject unmapped boundary");
    check(hw::copy_from_user(space, nullptr, 0, 0).ok(), "empty null copy");
    check(!hw::copy_from_user(space, nullptr, hw::user_begin, 1), "null kernel copy target");
    auto invalid = pages;
    invalid[0].flags |= hw::page_write;
    check(!hw::create_space(invalid), "reject writable executable page");
    auto bad_frame = hw::initial_frame(hw::user_begin, hw::user_begin + 0x4000);
    check(!hw::enter(space, bad_frame), "reject unmapped stack");
    const auto stale = space;
    check(hw::destroy_space(space).ok(), "destroy");
    check(!hw::user_range(stale, hw::user_begin, 1), "reject stale space");
    check(hw::destroy_space(second.value()).ok(), "second destroy");
    emit("ARM32_NATIVE_PASS scope=hardware\n");
    end(true);
}
