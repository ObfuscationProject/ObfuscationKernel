#include "ok/arch/execution.hpp"

using namespace ok;
namespace native = ok::arch::execution;

extern "C"
{
    extern const u8 ok_native_user_begin[], ok_native_user_end[];
    extern const u8 ok_native_user_kernel_read[], ok_native_user_readonly_write[];
    extern const u8 ok_native_user_tlb_stress[];
    extern const u8 ok_native_user_nx[], ok_native_user_unmapped[], ok_native_user_invalid[];
}
namespace
{
alignas(4096) u8 code[4096]{};
alignas(4096) u8 stress_pages[20][4096]{};
alignas(4096) u8 data_a[4096]{};
alignas(4096) u8 data_b[4096]{};
alignas(4096) u8 stack_a[4096]{};
alignas(4096) u8 stack_b[4096]{};
constexpr uptr code_va = native::user_begin;
constexpr uptr data_va = native::user_begin + 8192;
constexpr uptr stack_va = native::user_end - 4096;

void print(std::string_view text)
{
    auto *uart = reinterpret_cast<volatile u8 *>(0xe0004500);
    for (const auto value : text)
    {
        while ((uart[5] & 0x20) == 0)
        {
        }
        uart[0] = static_cast<u8>(value);
    }
}
[[noreturn]] void finish(bool)
{
    for (;;)
        asm volatile("wrteei 0; b ." : : : "memory");
}
void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        print("OK_NATIVE_TEST_FAIL ");
        print(message);
        print("\n");
        finish(false);
    }
}
native::Trap next_non_timer(native::AddressSpace &space, native::UserFrame &frame)
{
    for (usize attempt = 0; attempt < 128; ++attempt)
    {
        const auto result = native::enter(space, frame);
        require(result.ok(), "enter returned an error");
        if (result.value().kind != native::TrapKind::timer)
            return result.value();
    }
    require(false, "no forward progress after timer preemption");
    return {};
}
void check_fault(native::AddressSpace &space, const u8 *entry, native::TrapKind kind, uptr address, uptr error_mask)
{
    auto frame = native::initial_frame(code_va + static_cast<uptr>(entry - ok_native_user_begin), native::user_end);
    const auto trap = next_non_timer(space, frame);
    require(trap.kind == kind, "unexpected CPU fault kind");
    if (entry == ok_native_user_nx)
        require(trap.vector == 3, "hardware UX permission fault after readable data TLB fill");
    if (kind == native::TrapKind::page_fault)
        require(trap.fault_address == address, "unexpected hardware fault address");
    require((trap.error_code & error_mask) == error_mask, "missing hardware page-fault error bits");
}
} // namespace

extern "C" void ok_platform_console_write_char(char value)
{
    print(std::string_view(&value, 1));
}
extern "C" void *memset(void *destination, int value, usize count)
{
    auto *bytes = static_cast<u8 *>(destination);
    for (usize i = 0; i < count; ++i)
        bytes[i] = static_cast<u8>(value);
    return destination;
}
extern "C" void *memcpy(void *destination, const void *source, usize count)
{
    auto *out_bytes = static_cast<u8 *>(destination);
    const auto *in_bytes = static_cast<const u8 *>(source);
    for (usize i = 0; i < count; ++i)
        out_bytes[i] = in_bytes[i];
    return destination;
}

extern "C" [[noreturn]] void kernel_main()
{
    print("OK_NATIVE_TEST boot=begin\n");
    require(native::initialize().ok(), "native initialize");
#ifdef OK_NATIVE_TEST_KERNEL_FAULT
    asm volatile(".long 0");
#endif
    require(static_cast<usize>(ok_native_user_end - ok_native_user_begin) < sizeof(code), "user test text capacity");
    memcpy(code, ok_native_user_begin, ok_native_user_end - ok_native_user_begin);
    const native::PageMapping mappings_a[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_a), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_a), native::page_read | native::page_write},
    };
    const native::PageMapping mappings_b[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_b), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_b), native::page_read | native::page_write},
    };
    auto result_a = native::create_space(mappings_a);
    auto result_b = native::create_space(mappings_b);
    require(result_a.ok() && result_b.ok(), "create hardware address spaces");
    auto space_a = result_a.value();
    auto space_b = result_b.value();
    require(!native::user_range(space_a, 0x100000, 1), "reject kernel pointer");
    require(!native::user_range(space_a, data_va + 4095, 2), "reject partially unmapped buffer");
    require(!native::user_range(space_a, native::user_end - 1, static_cast<usize>(-1)), "reject overflowed buffer");
    require(!native::user_range(space_a, code_va, 1, true), "reject write to code");
    const u8 marker = 0xa5;
    require(native::copy_to_user(space_a, data_va + 128, &marker, 1).ok() && data_a[128] == marker,
            "copy user data through safe physical alias");
    print("OK_NATIVE_TEST user_copy=pass\n");

    auto frame_a = native::initial_frame(code_va, native::user_end);
    frame_a.fs_base = data_va + 64;
    frame_a.gs_base = data_va + 128;
    asm volatile(".machine e500; mfmsr 0; oris 0,0,0x0200; mtmsr 0; isync; "
                 "lis 0,0xa6b7; ori 0,0,0xc8d9; evmergelo 14,0,14; evmra 0,14"
                 :
                 :
                 : "r0", "memory");
    const u32 sprg_marker = 0x1badb002;
    asm volatile("mtspr 272,%0; mtspr 256,%0" : : "r"(sprg_marker) : "memory");
    auto trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall, "PR=1 SC trap");
    alignas(16) u8 kernel_spe[16]{};
    asm volatile("evstdd 14,0(%0); evxor 0,0,0; evmwumiaa 0,0,0; evstdd 0,8(%0)" : : "b"(kernel_spe) : "r0", "memory");
    require(kernel_spe[0] == 0xa6 && kernel_spe[8] == 0xa6, "kernel extended GPR and accumulator restored before C++");
    u32 restored_sprg;
    asm volatile("mfspr %0,272" : "=r"(restored_sprg));
    require(restored_sprg == sprg_marker, "kernel SPRG restored");
    asm volatile("mfspr %0,256" : "=r"(restored_sprg));
    require(restored_sprg == sprg_marker, "kernel USPRG0 restored");
    require(data_a[67] == 0x67 && data_a[131] == 0x54, "actual user r2/r13 TLS state");
    const auto call = native::syscall(frame_a);
    require(call.number == 0x1234 && call.arguments[0] == 0x23 && call.arguments[1] == 42,
            "actual CPL3 and syscall registers");
    require(frame_a.registers[14] == 0x11223344 && frame_a.registers[31] == 0x88776655, "callee-saved user registers");
    require((frame_a.flags & 0x4000) != 0, "actual user PR bit");
    require(frame_a.fp_state[112] == 0x55 && frame_a.fp_state[256] == 0x55, "SPE upper GPR and accumulator saved");
    require(frame_a.fp_state[267] == 2, "SPEFSCR saved");
    require(frame_a.fp_state[276] == 0x12 && frame_a.fp_state[280] == 0x24, "CTR and CR preserved");
    print("OK_NATIVE_TEST pr1=pass syscall=pass registers=pass\n");
    native::set_return_value(frame_a, 7);
    trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall && native::syscall(frame_a).number == 0x2345,
            "syscall return resumes user instruction");
    require(data_a[3] == 7 && data_a[16] == 0x55 && data_a[24] == 0x55 && data_a[43] == 0x9a,
            "user return register and SPE state restored");

    auto frame_b = native::initial_frame(code_va, native::user_end);
    frame_b.fs_base = data_va + 64;
    frame_b.gs_base = data_va + 128;
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall, "second process syscall");
    native::set_return_value(frame_b, 11);
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall && data_b[3] == 11 && data_a[3] == 7,
            "same virtual address has distinct physical process storage");
    print("OK_NATIVE_TEST address_spaces=pass spe=pass kernel_context=pass\n");
    auto timer = native::enter(space_a, frame_a);
    require(timer.ok() && timer.value().kind == native::TrapKind::timer && timer.value().vector == 10,
            "hardware decrementer preempts non-cooperative user loop");
    print("OK_NATIVE_TEST timer_preemption=pass\n");

    check_fault(space_a, ok_native_user_kernel_read, native::TrapKind::page_fault, 0x01000000, 0);
    check_fault(space_a, ok_native_user_readonly_write, native::TrapKind::page_fault, code_va, 0x00800000);
    check_fault(space_a, ok_native_user_nx, native::TrapKind::page_fault, data_va, 0);
    check_fault(space_a, ok_native_user_unmapped, native::TrapKind::page_fault, 0x60000000, 0);
    check_fault(space_a, ok_native_user_invalid, native::TrapKind::exception, 0, 0);
    print("OK_NATIVE_TEST supervisor_isolation=pass readonly=pass nx=pass faults=pass\n");
    native::PageMapping stress_mappings[22]{};
    stress_mappings[0] = mappings_a[0];
    stress_mappings[1] = mappings_a[2];
    for (usize i = 0; i < 20; ++i)
        stress_mappings[i + 2] = {0x41000000 + i * 4096, reinterpret_cast<uptr>(stress_pages[i]),
                                  native::page_read | native::page_write};
    auto stress_result = native::create_space(stress_mappings);
    require(stress_result.ok(), "create TLB eviction address space");
    auto stress_space = stress_result.value();
    auto stress_frame =
        native::initial_frame(code_va + (ok_native_user_tlb_stress - ok_native_user_begin), native::user_end);
    trap = next_non_timer(stress_space, stress_frame);
    require(trap.kind == native::TrapKind::syscall && native::syscall(stress_frame).number == 0x6789 &&
                stress_frame.registers[6] == 1,
            "transparent refill after TLB cache eviction");
    for (usize i = 0; i < 20; ++i)
        require(stress_pages[i][3] == i + 1, "every TLB cache mapping wrote its physical page");
    require(native::destroy_space(stress_space).ok(), "release TLB eviction address space");
    print("OK_NATIVE_TEST tls=pass tlb_eviction=pass\n");
    require(native::destroy_space(space_a).ok() && native::destroy_space(space_b).ok(),
            "release native address spaces");
    require(!native::user_range(space_a, data_va, 1), "stale address-space handle rejected");
    print("OK_NATIVE_TEST_PASS scope=hardware architecture=ppc\n");
    finish(true);
}
