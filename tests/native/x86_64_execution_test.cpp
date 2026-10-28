#include "ok/arch/execution.hpp"
#include "execution_table_pages.hpp"

using namespace ok;
namespace native = ok::arch::execution;

extern "C"
{
    extern const u8 ok_native_user_begin[], ok_native_user_end[];
    extern const u8 ok_native_user_kernel_read[], ok_native_user_readonly_write[];
    extern const u8 ok_native_user_nx[], ok_native_user_unmapped[], ok_native_user_invalid[];
    extern const u8 ok_native_user_dense[];
}
namespace
{
alignas(4096) u8 code[4096]{};
alignas(4096) u8 data_a[4096]{};
alignas(4096) u8 data_b[4096]{};
alignas(4096) u8 stack_a[4096]{};
alignas(4096) u8 stack_b[4096]{};
constexpr usize dense_pages = 192;
constexpr uptr dense_va = native::user_begin + 0x01000000;
alignas(4096) u8 dense_data[dense_pages][4096]{};
alignas(4096) u8 dense_replacement[4096]{};
std::array<native::PageMapping, dense_pages + 2> dense_mappings{};
std::array<native::PageMapping, dense_pages + 1> sparse_mappings{};
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
    uptr boot_root;
    asm volatile("mov %%cr3, %0" : "=r"(boot_root));
    auto trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall, "CPL3 SYSCALL trap");
    const auto call = native::syscall(frame_a);
    require(call.number == 0x1234 && call.arguments[0] == 0x23 && call.arguments[1] == 42,
            "actual CPL3 and syscall registers");
    require(frame_a.registers[13] == 0x1122334455667788ULL && frame_a.registers[3] == 0x8877665544332211ULL,
            "callee-saved user registers");
    require(frame_a.fp_state[160] == 0x5a, "SSE user state saved before kernel C++");
    print("OK_NATIVE_TEST cpl3=pass syscall=pass registers=pass\n");
    native::set_return_value(frame_a, 7);
    trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall && native::syscall(frame_a).number == 0x2345,
            "syscall return resumes user instruction");
    require(data_a[0] == 7 && data_a[16] == 0x5a, "user return register and SSE state restored");

    auto frame_b = native::initial_frame(code_va, native::user_end);
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall, "second process syscall");
    native::set_return_value(frame_b, 11);
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall && data_b[0] == 11 && data_a[0] == 7,
            "same virtual address has distinct physical process storage");
    print("OK_NATIVE_TEST address_spaces=pass sse=pass\n");
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
    require(native::destroy_space(space_a).ok() && native::destroy_space(space_b).ok(),
            "release native address spaces");
    require(!native::user_range(space_a, data_va, 1), "stale address-space handle rejected");
    dense_mappings[0] = {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute};
    for (usize i = 0; i < dense_pages; ++i)
        dense_mappings[i + 1] = {dense_va + i * 4096, reinterpret_cast<uptr>(dense_data[i]),
                                 native::page_read | native::page_write};
    dense_mappings[dense_pages + 1] =
        {stack_va, reinterpret_cast<uptr>(stack_a), native::page_read | native::page_write};
    auto dense_result = native::create_space(dense_mappings);
    require(dense_result.ok(), "create more than 128 hardware user mappings");
    auto dense_space = dense_result.value();
    auto dense_frame = native::initial_frame(code_va + static_cast<uptr>(ok_native_user_dense - ok_native_user_begin),
                                            native::user_end);
    trap = next_non_timer(dense_space, dense_frame);
    require(trap.kind == native::TrapKind::syscall && native::syscall(dense_frame).number == 0x3456,
            "ring-3 loop touched 192 pages");
    for (usize i = 0; i < dense_pages; ++i)
        require(dense_data[i][0] == static_cast<u8>(dense_pages - i) && dense_data[i][1] == 0,
                "ring-3 page bytes and zero fill");
    print("OK_NATIVE_DENSE pages=192 touch=pass zero=pass\n");

    require(native::destroy_space(dense_space).ok(), "retire dense table before protection change");
    dense_mappings[101].flags = 0;
    dense_result = native::create_space(dense_mappings);
    require(dense_result.ok(), "create PROT_NONE dense table");
    dense_space = dense_result.value();
    require(!native::user_range(dense_space, dense_va + 100 * 4096, 1) &&
            dense_data[100][0] == static_cast<u8>(dense_pages - 100), "PROT_NONE has no user translation");
    require(native::destroy_space(dense_space).ok(), "retire protected table");
    dense_mappings[101].flags = native::page_read | native::page_write;
    for (usize src = 0, dst = 0; src < dense_mappings.size(); ++src)
        if (src != 101) sparse_mappings[dst++] = dense_mappings[src];
    auto sparse_result = native::create_space(sparse_mappings);
    require(sparse_result.ok(), "create sparse dense table");
    auto sparse_space = sparse_result.value();
    require(!native::user_range(sparse_space, dense_va + 100 * 4096, 1), "unmapped page has no user translation");
    require(native::destroy_space(sparse_space).ok(), "retire sparse table");
    dense_mappings[101].physical_address = reinterpret_cast<uptr>(dense_replacement);
    dense_result = native::create_space(dense_mappings);
    require(dense_result.ok(), "remap zeroed replacement page");
    dense_space = dense_result.value();
    require(native::user_range(dense_space, dense_va + 100 * 4096, 1, true) && dense_replacement[0] == 0,
            "remap restores writable zero page");
    std::array<native::PageMapping, 17> scattered{};
    for (usize i = 0; i < scattered.size(); ++i)
        scattered[i] = {native::user_begin + i * 0x200000, reinterpret_cast<uptr>(dense_data[i]),
                        native::page_read};
    // Translation tables come from runtime storage; a budget that runs out
    // partway through the 17 leaf tables makes growth fail.
    const usize tables_before = native::probe::table_pages_used;
    native::probe::table_page_budget = tables_before + 4;
    require(!native::create_space(scattered) && native::probe::table_pages_used == tables_before &&
                native::user_range(dense_space, dense_va, 1) && dense_data[0][0] == static_cast<u8>(dense_pages),
            "failed table growth releases its tables and preserves old bytes");
    native::probe::table_page_budget = native::probe::table_arena_pages;
    require(native::destroy_space(dense_space).ok(), "retire remapped dense table");
    print("OK_NATIVE_DENSE none=pass unmap=pass remap=pass failure=pass\n");
    print("OK_NATIVE_TEST_PASS scope=hardware architecture=x86_64\n");
    finish(true);
}
