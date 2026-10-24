// Actual entry wrapper, runtime dispatch and scheduler with hosted trap/memory.
#define OK_ENABLE_TEST_POINTS 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_clock_model.hpp"
#include <source_location>
namespace ok::user::native
{
struct SyscallOriginTestAccess
{
    static u64 capture(SyscallOrigins &table, u64 next)
    { const auto previous = table.capture_sequence_; table.capture_sequence_ = next; return previous; }
    static u64 image(SyscallOrigins &table, u64 next)
    { const auto previous = table.image_sequence_; table.image_sequence_ = next; return previous; }
};
}
namespace
{
using Origins = native::SyscallOrigins;
constexpr auto profile = lu::configured_native_profile();
constexpr uptr output = hw::user_begin + 128;
usize checks{};
void expect(bool value, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "PIPE_VECTOR_ORIGIN_FAIL line=%u checks=%zu\n", where.line(), checks); std::abort(); }
}
struct Group { sched::ProcessId pid{}; sched::ThreadId reader{}; };
Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("syscall-origin",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 8192})); CHECK(made);
    auto *p = kernel.scheduler().find(made.value());
    p->resources().personality = sched::UserPersonality::linux;
    p->resources().native_space = {0x100000 + made.value() * 4096, static_cast<u32>(made.value() % 16), 1};
    CHECK(native::allocate(p->resources().native_pages, hw::user_begin, hw::page_read | hw::page_execute));
    CHECK(native::allocate(p->resources().native_pages, hw::user_begin + 4096, hw::page_read | hw::page_execute));
    auto peer = kernel.scheduler().create_thread(p->pid(), p->threads()[0].context); CHECK(peer);
    for (auto &thread : p->threads()) { CHECK(native::prepare_native_thread(thread)); thread.native_active = true; }
    p->threads()[0].native_active = false;
    CHECK(kernel.scheduler().set_runnable(p->pid()));
    CHECK(kernel.scheduler().activate_thread(p->pid(), p->pid())); CHECK(kernel.posix().activate(p->pid()));
    native::runtime_kernel = &kernel; native::started = true;
    return {p->pid(), peer.value()};
}
void retire(Kernel &kernel, Group g)
{
    auto *p = kernel.scheduler().find(g.pid);
    if (!p) return;
    native::release_process(kernel, g.pid);
    CHECK(kernel.scheduler().exit_process(g.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    expect(native::pipe_read_objects.accounting().live_objects == 0);
    expect(native::runtime_status().ok());
}
hw::Trap syscall_trap()
{
    if constexpr (profile == lu::Profile::x86_64) return {hw::TrapKind::syscall, 256};
    if constexpr (profile == lu::Profile::i386) return {hw::TrapKind::syscall, 128};
    if constexpr (profile == lu::Profile::aarch64) return {hw::TrapKind::syscall, 0x15, 0x56000000};
    if constexpr (profile == lu::Profile::arm32) return {hw::TrapKind::syscall, 2};
    if constexpr (profile == lu::Profile::rv32 || profile == lu::Profile::rv64) return {hw::TrapKind::syscall, 8, 8};
    if constexpr (profile == lu::Profile::loongarch64) return {hw::TrapKind::syscall, 11};
    if constexpr (profile == lu::Profile::mips || profile == lu::Profile::mips64) return {hw::TrapKind::syscall, 8, 32};
    return {hw::TrapKind::syscall, 8};
}
usize instruction_bytes(bool thumb = false)
{ return profile == lu::Profile::i386 || profile == lu::Profile::x86_64 || (profile == lu::Profile::arm32 && thumb) ? 2 : 4; }
void opcode(uptr site, bool thumb)
{
    std::array<u8, 4> code{};
    if constexpr (profile == lu::Profile::x86_64) code = {0x0f, 0x05};
    if constexpr (profile == lu::Profile::i386) code = {0xcd, 0x80};
    if constexpr (profile == lu::Profile::aarch64) code = {0x01, 0x00, 0x00, 0xd4};
    if constexpr (profile == lu::Profile::arm32) code = thumb ? std::array<u8, 4>{0x00, 0xdf} : std::array<u8, 4>{0x00, 0x00, 0x00, 0xef};
    if constexpr (profile == lu::Profile::rv32 || profile == lu::Profile::rv64) code = {0x73, 0, 0, 0};
    if constexpr (profile == lu::Profile::loongarch64) code = {0, 0, 0x2b, 0};
    if constexpr (profile == lu::Profile::mips || profile == lu::Profile::mips64) code = {0, 0, 0, 0x0c};
    if constexpr (profile == lu::Profile::ppc) code = {0x44, 0, 0, 0x02};
    for (usize i = 0; i < instruction_bytes(thumb); ++i) mock::memory[site - hw::user_begin + i] = std::byte{code[i]};
}
hw::UserFrame read_frame(i32 fd, uptr site = hw::user_begin + 1024, usize count = 3, bool thumb = false)
{
    hw::UserFrame frame{};
    for (usize i = 0; i < frame.registers.size(); ++i) frame.registers[i] = 0x1100 + i;
    for (usize i = 0; i < frame.fp_state.size(); ++i) frame.fp_state[i] = static_cast<u8>(i * 7);
    frame.pc = site + instruction_bytes(thumb); frame.sp = hw::user_begin + 8192;
    frame.fs_base = 0x7654; frame.gs_base = 0x4567;
    if constexpr (profile == lu::Profile::x86_64)
    { frame.registers[14] = 0; frame.registers[9] = fd; frame.registers[10] = output; frame.registers[11] = count; }
    else if constexpr (profile == lu::Profile::i386)
    { frame.registers[7] = 3; frame.registers[4] = fd; frame.registers[6] = output; frame.registers[5] = count; }
    else if constexpr (profile == lu::Profile::aarch64 || profile == lu::Profile::arm32)
    {
        frame.registers[profile == lu::Profile::arm32 ? 7 : 8] = profile == lu::Profile::arm32 ? 3 : 63;
        frame.registers[0] = fd; frame.registers[1] = output; frame.registers[2] = count;
        if constexpr (profile == lu::Profile::arm32) { frame.flags = thumb ? 0x30 : 0x10; frame.registers[15] = frame.pc; }
    }
    else if constexpr (profile == lu::Profile::rv32 || profile == lu::Profile::rv64)
    { frame.registers[17] = 63; frame.registers[10] = fd; frame.registers[11] = output; frame.registers[12] = count; }
    else if constexpr (profile == lu::Profile::loongarch64)
    { frame.registers[11] = 63; frame.registers[4] = fd; frame.registers[5] = output; frame.registers[6] = count; }
    else if constexpr (profile == lu::Profile::mips || profile == lu::Profile::mips64)
    { frame.registers[2] = profile == lu::Profile::mips ? 4003 : 5000; frame.registers[4] = fd; frame.registers[5] = output; frame.registers[6] = count; frame.registers[7] = 0x12345678; }
    else { frame.registers[0] = 3; frame.registers[3] = fd; frame.registers[4] = output; frame.registers[5] = count; }
    opcode(site, thumb); return frame;
}

constexpr usize number_register()
{
    if constexpr (profile == lu::Profile::x86_64) return 14;
    if constexpr (profile == lu::Profile::i386 || profile == lu::Profile::arm32) return 7;
    if constexpr (profile == lu::Profile::aarch64) return 8;
    if constexpr (profile == lu::Profile::rv32 || profile == lu::Profile::rv64) return 17;
    if constexpr (profile == lu::Profile::loongarch64) return 11;
    if constexpr (profile == lu::Profile::mips || profile == lu::Profile::mips64) return 2;
    return 0;
}
template <bool Writing> void exercise(Kernel &kernel, unsigned mode, bool thumb)
{
    auto g = create(kernel); auto fds = kernel.posix().pipe(0); CHECK(fds);
    if constexpr (Writing)
    {
        std::array<std::byte, sched::pipe_capacity> data{};
        auto result = kernel.posix().write(fds.value()[1], data); CHECK(result && result.value() == data.size());
    }
    auto *p = kernel.scheduler().find(g.pid); auto *thread = kernel.scheduler().find_thread(g.pid, g.reader);
    const auto fd = fds.value()[Writing ? 1 : 0];
    auto frame = read_frame(fd, hw::user_begin+1024, mode == 2 ? 3 : 4, thumb);
    frame.registers[number_register()] = *lu::Codec<profile>::syscall_number(Writing
        ? (mode == 1 ? lu::Operation::write : lu::Operation::writev)
        : (mode == 1 ? lu::Operation::read : lu::Operation::readv));
    thread->native_frame = frame; mock::entry_trap = syscall_trap();
    auto active = native::entry_stacks.activate_entry(thread->entry_stack, {g.pid, g.reader}, 0); CHECK(active);
    expect(native::syscall_origins.enter(*p, *thread, active.value(), kernel.scheduler()).ok());
    CHECK(native::entry_stacks.deactivate_entry(thread->entry_stack, {g.pid, g.reader}, 0));
    const auto id = native::syscall_origins.identity(*p, *thread); expect(native::syscall_origins.live(id));
    CHECK(kernel.scheduler().activate_thread(g.pid, g.reader)); thread->syscall_pending = true;
    native::PipeVectors vectors{}; vectors.count = 4;
    vectors.entries[0] = {hw::user_begin+3072, 1}; vectors.entries[1] = {hw::user_end-1, 0};
    vectors.entries[2] = {hw::user_begin+3088, 1}; vectors.entries[3] = {hw::user_begin+3104, 1};
    const auto description = p->fd_table().entry(fd)->description; bool complete = true; i64 result;
    if constexpr (Writing) result = native::begin_pipe_write(kernel, *p, g.reader, fd, description, output, 3, complete, vectors);
    else result = native::begin_pipe_read(kernel, *p, g.reader, fd, description, output, 3, complete, vectors);
    expect(result == -11 && !complete);
    if (mode == 1 || mode == 2)
    {
        expect(!native::syscall_origins.live(id)); retire(kernel, g); return;
    }
    const auto binding = [&] {
        if constexpr (Writing)
        {
            const auto object = native::pipe_write_object(g.pid, g.reader);
            return native::pipe_write_origin_binding(object, *native::pipe_write_objects.resolve(object).value());
        }
        else
        {
            const auto object = native::pipe_read_object(g.pid, g.reader);
            return native::pipe_origin_binding(object, *native::pipe_read_objects.resolve(object).value());
        }
    }();
    auto permit = native::syscall_origins.prepare_restart(id, frame, binding); expect(permit.ok());
    expect(permit.value().plan().frame.pc == frame.pc-instruction_bytes(thumb));
    const auto decoded = lu::NativeBridge<profile>::decode_call(permit.value().plan().frame, 3); expect(decoded.ok());
    expect(decoded.value().number == frame.registers[number_register()] && decoded.value().arguments[2] == 4);
    for (unsigned mutation = 0; mutation < 12; ++mutation)
    {
        auto wrong = binding;
        switch (mutation)
        {
        case 0: ++wrong.vectors.entries[0].base; break;
        case 1: ++wrong.vectors.entries[0].length; break;
        case 2: ++wrong.vectors.entries[1].base; break;
        case 3: ++wrong.vectors.count; break;
        case 4: wrong.vectors.count = 0; break;
        case 5: wrong.vectors.count = 17; break;
        case 6: wrong.vectors.entries[2].length = static_cast<usize>(__INTPTR_MAX__); break;
        case 7: ++wrong.vectors.entries[15].length; break;
        case 8: ++wrong.count; break;
        case 9: ++wrong.request; break;
        case 10: ++wrong.fd; break;
        case 11: ++wrong.ticket.generation; break;
        }
        expect(!native::syscall_origins.prepare_restart(id, frame, wrong));
        expect(!native::syscall_origins.validate_restart(permit.value(), id, frame, wrong));
        expect(!native::syscall_origins.consume_restart(permit.value(), id, frame, wrong));
    }
    expect(native::syscall_origins.consume_restart(permit.value(), id, frame, binding));
    expect(!native::syscall_origins.consume_restart(permit.value(), id, frame, binding)); retire(kernel, g);
}
} // namespace
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        exercise<false>(kernel, mode, false); exercise<true>(kernel, mode, false);
        if constexpr (profile == lu::Profile::arm32) { exercise<false>(kernel, mode, true); exercise<true>(kernel, mode, true); }
    }
    expect(native::pipe_write_objects.accounting().live_objects == 0);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    std::printf("PIPE_VECTOR_ORIGIN_PASS hosted=1 native=0 checks=%zu\n", checks);
}
