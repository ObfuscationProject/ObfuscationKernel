#include "ok/arch/execution.hpp"
#include "execution_table_pages.hpp"

using namespace ok;
namespace native = ok::arch::execution;

extern "C"
{
    extern const u8 ok_native_user_begin[], ok_native_user_end[];
    extern const u8 ok_native_user_kernel_read[], ok_native_user_readonly_write[];
    extern const u8 ok_native_user_segment_stack[];
    extern const u8 ok_native_user_nx[], ok_native_user_unmapped[], ok_native_user_invalid[];
}
namespace
{
alignas(4096) u8 code[4096]{};
alignas(4096) u8 data_a[4096]{};
alignas(4096) u8 data_b[4096]{};
alignas(4096) u8 stack_a[4096]{};
alignas(4096) u8 stack_b[4096]{};
constexpr uptr code_va = native::user_begin;
constexpr uptr data_va = native::user_begin + 8192;
constexpr uptr stack_va = native::user_end - 4096;

void out(u16 port, u8 value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
u8 in(u16 port)
{
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
void print(std::string_view text)
{
    for (const auto value : text)
    {
        while ((in(0x3fd) & 0x20) == 0)
        {
        }
        out(0x3f8, static_cast<u8>(value));
    }
}
[[noreturn]] void finish(bool pass)
{
    const u32 value = pass ? 0x10 : 0x11;
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(u16{0xf4}));
    for (;;)
        asm volatile("cli; hlt");
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
    require(trap.fault_address == address, "unexpected hardware CR2");
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
    out(0x3f9, 0);
    out(0x3fb, 0x80);
    out(0x3f8, 3);
    out(0x3f9, 0);
    out(0x3fb, 3);
    out(0x3fa, 0xc7);
    out(0x3fc, 0x0b);
    print("OK_NATIVE_TEST boot=begin\n");
    require(native::initialize().ok(), "native initialize");
#ifdef OK_NATIVE_TEST_KERNEL_FAULT
    asm volatile("ud2");
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
    asm volatile("movd %0, %%xmm7" : : "a"(u32{0x98765432}) : "memory");
    uptr boot_root;
    asm volatile("mov %%cr3, %0" : "=r"(boot_root));
    auto trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall, "CPL3 INT 0x80 trap");
    u32 kernel_xmm;
    asm volatile("movd %%xmm7, %0" : "=a"(kernel_xmm));
    require(kernel_xmm == 0x98765432, "kernel SSE restored before C++ continuation");
    u16 kernel_fs, kernel_gs;
    asm volatile("mov %%fs,%0; mov %%gs,%1" : "=r"(kernel_fs), "=r"(kernel_gs));
    require(kernel_fs == 0 && kernel_gs == 0, "kernel TLS selectors restored");
    require(data_a[64] == 0x10 && data_a[128] == 0xdf, "actual FS/GS TLS bases");
    const auto call = native::syscall(frame_a);
    require(call.number == 0x1234 && call.arguments[0] == 0x23 && call.arguments[1] == 42,
            "actual CPL3 and syscall registers");
    require(frame_a.registers[1] == 0x11223344 && frame_a.registers[2] == 0x88776655, "callee-saved user registers");
    require(frame_a.fp_state[160] == 0x5a, "SSE user state saved before kernel C++");
    print("OK_NATIVE_TEST cpl3=pass syscall=pass registers=pass\n");
    native::set_return_value(frame_a, 7);
    trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall && native::syscall(frame_a).number == 0x2345,
            "syscall return resumes user instruction");
    require(data_a[0] == 7 && data_a[16] == 0x5a, "user return register and SSE state restored");

    require(data_a[32] == 1, "x87 stack restored");
    require((frame_a.registers[9] & 0xffff) == 0 && (frame_a.registers[10] & 0xffff) == 0 &&
                (frame_a.flags & 0x400) != 0,
            "user segment selectors and DF preserved");
    auto frame_b = native::initial_frame(code_va, native::user_end);
    frame_b.fs_base = data_va + 64;
    frame_b.gs_base = data_va + 128;
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall, "second process syscall");
    native::set_return_value(frame_b, 11);
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall && data_b[0] == 11 && data_a[0] == 7,
            "same virtual address has distinct physical process storage");
    print("OK_NATIVE_TEST address_spaces=pass sse=pass x87=pass tls=pass kernel_context=pass\n");
    auto timer = native::enter(space_a, frame_a);
    require(timer.ok() && timer.value().kind == native::TrapKind::timer && timer.value().vector == 32,
            "hardware PIT preempts non-cooperative user loop");
    print("OK_NATIVE_TEST timer_preemption=pass\n");

    check_fault(space_a, ok_native_user_kernel_read, native::TrapKind::page_fault, 0x100000, 5);
    check_fault(space_a, ok_native_user_readonly_write, native::TrapKind::page_fault, code_va, 7);
    check_fault(space_a, ok_native_user_nx, native::TrapKind::page_fault, data_va, 21);
    check_fault(space_a, ok_native_user_unmapped, native::TrapKind::page_fault, 0x60000000, 4);
    check_fault(space_a, ok_native_user_invalid, native::TrapKind::exception, 0, 0);
    print("OK_NATIVE_TEST supervisor_isolation=pass readonly=pass nx=pass faults=pass\n");
    uptr restored_root;
    asm volatile("mov %%cr3, %0" : "=r"(restored_root));
    require(restored_root == boot_root, "restore kernel CR3 after user fault");
    auto segment_frame =
        native::initial_frame(code_va + (ok_native_user_segment_stack - ok_native_user_begin), native::user_end);
    segment_frame.fs_base = data_va + 64;
    trap = next_non_timer(space_a, segment_frame);
    require(trap.kind == native::TrapKind::syscall && native::syscall(segment_frame).number == 0x4567 &&
                segment_frame.sp == 4032 && (segment_frame.registers[12] & 0xffff) == 0x33,
            "user SS selector and logical ESP captured");
    trap = next_non_timer(space_a, segment_frame);
    require(trap.kind == native::TrapKind::syscall && native::syscall(segment_frame).number == 0x5678 &&
                data_a[4092] == 0x10,
            "nonzero-base user SS restored with correct stack addressing");
    print("OK_NATIVE_TEST segmented_stack=pass\n");
    require(native::destroy_space(space_a).ok() && native::destroy_space(space_b).ok(),
            "release native address spaces");
    require(!native::user_range(space_a, data_va, 1), "stale address-space handle rejected");
    print("OK_NATIVE_TEST_PASS scope=hardware architecture=i386\n");
    finish(true);
}
