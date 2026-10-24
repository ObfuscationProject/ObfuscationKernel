#pragma once
#include "native_clock_model.hpp"
#include <source_location>

namespace {
usize checks{};
void expect(bool value, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "CHILD_EXIT_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
constexpr uptr site = hw::user_begin + 1024, handler = hw::user_begin + 2048, info_address = hw::user_begin + 512;
using Life = native::PrivateChildExit;
using Actions = Life::Actions;
constexpr auto sigchld = Actions::child_signal;
void frame(sched::ThreadControlBlock &thread)
{
    thread.native_frame = {}; thread.native_frame.flags = 0x202;
    thread.native_frame.sp = hw::user_begin + 8192; thread.native_frame.pc = site;
    const auto fp = native::X86SignalFrame::reset_fp();
    for (usize i = 0; i < fp.size(); ++i) thread.native_frame.fp_state[i] = static_cast<u8>(fp[i]);
}
u64 create(Kernel &kernel)
{
    const auto made = kernel.scheduler().create_user_process("child-exit-runtime",
        kernel.arch().make_user_context({site, hw::user_begin + 8192})); CHECK(made);
    auto &process = *kernel.scheduler().find(made.value());
    process.resources().personality = sched::UserPersonality::linux;
    process.resources().native_space = {0x100000 + process.pid() * 4096, static_cast<u32>(process.pid() % 16), 1};
    CHECK(native::allocate(process.resources().native_pages, hw::user_begin, hw::page_read | hw::page_execute));
    auto &thread = process.threads()[0]; CHECK(native::prepare_native_thread(thread)); thread.native_active = true; frame(thread);
    CHECK(native::private_signal_process(kernel, process.pid()));
    CHECK(kernel.scheduler().set_runnable(process.pid()));
    native::runtime_kernel = &kernel; native::started = true; return process.pid();
}
[[maybe_unused]] u64 create_init(Kernel &kernel)
{
    const auto made = kernel.scheduler().create_init_process(
        kernel.arch().make_user_context({site, hw::user_begin + 8192})); CHECK(made);
    auto &process = *kernel.scheduler().find(made.value());
    process.resources().personality = sched::UserPersonality::linux;
    process.resources().native_space = {0x100000 + process.pid() * 4096, static_cast<u32>(process.pid() % 16), 1};
    CHECK(native::allocate(process.resources().native_pages, hw::user_begin, hw::page_read | hw::page_execute));
    auto &thread = process.threads()[0]; CHECK(native::prepare_native_thread(thread)); thread.native_active = true; frame(thread);
    CHECK(native::private_signal_process(kernel, process.pid()));
    CHECK(kernel.scheduler().set_runnable(process.pid()));
    native::runtime_kernel = &kernel; native::started = true; return process.pid();
}
void activate(Kernel &kernel, u64 pid, u64 tid)
{ CHECK(kernel.posix().activate(pid)); CHECK(kernel.scheduler().activate_thread(pid, tid)); }
u64 fork(Kernel &kernel, u64 parent, u64 caller = 0)
{
    activate(kernel, parent, caller ? caller : parent); mock::spaces_enabled = true;
    bool complete = true; const auto result = native::dispatch(kernel, parent, {OK_SYS_FORK, {}}, complete);
    expect(result > 0 && complete); return static_cast<u64>(result);
}
[[maybe_unused]] u64 peer(Kernel &kernel, u64 pid)
{
    const auto result = kernel.scheduler().create_thread_prepared(pid, kernel.scheduler().find(pid)->threads()[0].context,
        [](auto &thread) { thread.native_active = true; frame(thread); return native::prepare_native_thread(thread); });
    CHECK(result); return result.value();
}
[[maybe_unused]] void action(Kernel &kernel, u64 pid, u64 disposition, u64 flags = 0)
{
    auto *entry = native::private_signal_table(kernel, pid)->find(sigchld); CHECK(entry);
    *entry = {}; entry->handler = disposition; entry->flags = flags;
    if (disposition > 1) { entry->flags |= 0x04000004; entry->restorer = handler + 4; }
}
[[maybe_unused]] native::PrivateProcessQueue::Delivery pending(u64 pid)
{
    native::PrivateProcessQueue::Delivery ticket;
    CHECK(native::private_signal_find(pid)->shared.peek({}, ticket)); return ticket;
}
[[maybe_unused]] void raw_exit(Kernel &kernel, u64 pid, u64 tid, u64 code, bool group = false)
{
    activate(kernel, pid, tid); bool complete = true;
    expect(native::dispatch(kernel, pid, {group ? OK_SYS_EXIT_GROUP : OK_SYS_EXIT, {code}}, complete) == 0 && !complete);
    expect(native::runtime_status().ok());
}
void release(Kernel &kernel, u64 pid)
{
    // Administrative fixture cleanup has no notification expectation.
    native::release_process(kernel, pid);
    const auto *process = kernel.scheduler().find(pid);
    if (process && process->state() != sched::ProcessState::zombie) CHECK(kernel.scheduler().exit_process(pid, 0));
    CHECK(kernel.scheduler().reap_process(pid));
    expect(native::runtime_status().ok());
}
}
