#include "ok/arch/execution.hpp"
#include "execution_table_pages.hpp"
namespace hw = ok::arch::execution;
using namespace ok;
extern "C" const std::byte ok_la_probe_begin[], ok_la_probe_timer[], ok_la_probe_ro[], ok_la_probe_kernel[],
    ok_la_probe_end[], ok_la_probe_read[], ok_la_probe_fp[], ok_la_probe_invalid[], ok_la_probe_nx[];
namespace
{
alignas(4096) std::array<std::byte, 4096> code{}, stack{}, other{};
void emit(const char *text)
{
    auto *uart = reinterpret_cast<volatile u8 *>(0x1fe001e0);
    while (*text)
    {
        while ((uart[5] & 0x20) == 0)
        {
        }
        uart[0] = static_cast<u8>(*text++);
    }
}
[[noreturn]] void stop()
{
    for (;;)
        asm volatile("idle 0");
}
void check(bool value, const char *message)
{
    if (!value)
    {
        emit("LOONG_NATIVE_FAIL ");
        emit(message);
        emit("\n");
        stop();
    }
}
uptr address(const std::byte *label)
{
    return hw::user_begin + static_cast<uptr>(label - ok_la_probe_begin);
}
} // namespace
extern "C" void *memcpy(void *out, const void *in, usize size)
{
    auto *dst = static_cast<volatile u8 *>(out);
    auto *src = static_cast<const u8 *>(in);
    for (usize i = 0; i < size; ++i)
        dst[i] = src[i];
    return out;
}
extern "C" void *memset(void *out, int value, usize size)
{
    auto *dst = static_cast<volatile u8 *>(out);
    for (usize i = 0; i < size; ++i)
        dst[i] = value;
    return out;
}
extern "C" [[noreturn]] void kernel_main()
{
    emit("LOONG_NATIVE_BEGIN\n");
    check(hw::initialize().ok(), "initialize");
    for (usize i = 0; i < static_cast<usize>(ok_la_probe_end - ok_la_probe_begin); ++i)
        code[i] = ok_la_probe_begin[i];
    std::array<hw::PageMapping, 2> mappings{
        {{hw::user_begin, reinterpret_cast<uptr>(code.data()), hw::page_read | hw::page_execute},
         {hw::user_begin + 4096, reinterpret_cast<uptr>(stack.data()), hw::page_read | hw::page_write}}};
    auto result = hw::create_space(mappings);
    check(result.ok(), "space");
    auto space = result.value();
    auto frame = hw::initial_frame(hw::user_begin, hw::user_begin + 8192 - 16);
    // SP is the shared frame field; the architecture-private register array
    // must never override the stack pointer checked by enter().
    frame.registers[3] = 0;
    uptr kernel_tp;
    asm volatile("move %0, $tp" : "=r"(kernel_tp));
    uptr scratch = 0x1234;
    asm volatile("csrwr %0, 0x30" : "+r"(scratch) : : "memory");
    scratch = 0x5678;
    asm volatile("csrwr %0, 0x31" : "+r"(scratch) : : "memory");
    emit("LOONG_NATIVE enter\n");
    auto trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 65 &&
              hw::syscall(frame).arguments[0] == 42,
          "syscall");
    check(frame.sp == hw::user_begin + 8192 - 16, "canonical user stack");
    uptr restored_ks0, restored_ks1, restored_tp;
    asm volatile("csrrd %0, 0x30; csrrd %1, 0x31; move %2, $tp"
                 : "=r"(restored_ks0), "=r"(restored_ks1), "=r"(restored_tp));
    check(restored_ks0 == 0x1234 && restored_ks1 == 0x5678 && restored_tp == kernel_tp,
          "kernel scratch CSR and TLS restoration");
    emit("LOONG_NATIVE syscall=pass\n");
    hw::set_return_value(frame, 81);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 66 &&
              hw::syscall(frame).arguments[0] == 81,
          "resume");
    emit("LOONG_NATIVE stage=timer\n");
    frame = hw::initial_frame(address(ok_la_probe_timer), hw::user_begin + 8192 - 16);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::timer && frame.registers[23] != 0, "timer");
    const auto counter = frame.registers[23];
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::timer && frame.registers[23] > counter, "timer resume");
    emit("LOONG_NATIVE stage=readonly\n");
    frame = hw::initial_frame(address(ok_la_probe_ro), hw::user_begin + 8192 - 16);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault, "readonly");
    frame = hw::initial_frame(address(ok_la_probe_kernel), hw::user_begin + 8192 - 16);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault, "kernel protection");
    emit("LOONG_NATIVE stage=copies\n");
    u64 value = 123, copied = 0;
    check(hw::copy_to_user(space, hw::user_begin + 4096, &value, sizeof(value)).ok(), "copy to");
    auto second_mappings = mappings;
    second_mappings[1].physical_address = reinterpret_cast<uptr>(other.data());
    auto second = hw::create_space(second_mappings);
    check(second.ok(), "second space");
    check(hw::copy_from_user(second.value(), &copied, hw::user_begin + 4096, sizeof(copied)).ok() && copied == 0,
          "isolation");
    check(!hw::copy_to_user(space, hw::user_begin, &value, sizeof(value)), "safe readonly");
    check(!hw::copy_from_user(space, &copied, hw::user_begin + 8190, sizeof(copied)), "safe bounds");
    emit("LOONG_NATIVE stage=hardware-spaces\n");
    const uptr sp = hw::user_begin + 8192 - 16;
    u64 first_value = 111, second_value = 222;
    check(hw::copy_to_user(space, sp, &first_value, sizeof(first_value)).ok(), "first data");
    check(hw::copy_to_user(second.value(), sp, &second_value, sizeof(second_value)).ok(), "second data");
    for (int i = 0; i < 4; ++i)
    {
        frame = hw::initial_frame(address(ok_la_probe_read), sp);
        auto &selected = (i & 1) ? second.value() : space;
        trap = hw::enter(selected, frame);
        check(trap && trap.value().kind == hw::TrapKind::syscall &&
                  hw::syscall(frame).arguments[0] == ((i & 1) ? second_value : first_value),
              "hardware isolation");
    }
    emit("LOONG_NATIVE stage=fp\n");
    frame = hw::initial_frame(address(ok_la_probe_fp), sp);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 77, "FP setup");
    asm volatile("movgr2fr.d $f24, $zero; movgr2cf $fcc3, $zero" : : : "f24");
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 78 &&
              hw::syscall(frame).arguments[0] == 0x123456789abcdef && hw::syscall(frame).arguments[1] == 1 &&
              hw::syscall(frame).arguments[2] == 0x778899,
          "FP FCC TLS restoration");
#if defined(OK_NATIVE_NX_BRANCH)
    emit("LOONG_NATIVE stage=noexecute-hardware-branch\n");
    frame = hw::initial_frame(address(ok_la_probe_nx), sp);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault && trap.value().vector == 6 &&
              trap.value().fault_address == hw::user_begin + 4096,
          "hardware NX branch");
#else
    emit("LOONG_NATIVE stage=noexecute-entry-guard\n");
    frame = hw::initial_frame(hw::user_begin + 4096, sp);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault, "NX");
#endif
    emit("LOONG_NATIVE stage=unmapped\n");
    frame = hw::initial_frame(hw::user_begin + 0x10000, sp);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault, "unmapped PC");
    emit("LOONG_NATIVE stage=invalid\n");
    frame = hw::initial_frame(address(ok_la_probe_invalid), sp);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::exception, "invalid opcode");
    check(hw::copy_from_user(space, nullptr, 0, 0).ok(), "empty copy");
    check(!hw::copy_from_user(space, nullptr, hw::user_begin, 1), "null destination");
    // Like Linux, entry does not need a mapped stack; this probe reaches its
    // syscall without touching one.
    frame = hw::initial_frame(hw::user_begin, hw::user_begin + 0x10000);
    trap = hw::enter(space, frame);
    while (trap && trap.value().kind == hw::TrapKind::timer) trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 65, "unmapped stack");
    const auto stale = space;
    check(hw::destroy_space(space).ok() && !hw::user_range(stale, hw::user_begin, 1), "stale");
#if defined(OK_NATIVE_KERNEL_FAULT)
    emit("LOONG_NATIVE stage=kernel-fault-guard\n");
    // KS0 deliberately contains an invalid pointer. The PLV0 vector must stop
    // before using it as a user-frame address or switching to a stale stack.
    scratch = 0;
    asm volatile("csrwr %0, 0x30; .word 0" : "+r"(scratch) : : "memory");
    check(false, "kernel fault returned");
#endif
    emit("LOONG_NATIVE_PASS scope=hardware\n");
    stop();
}
