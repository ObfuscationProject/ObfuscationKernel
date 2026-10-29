#pragma once
// Derived native x86_64 runtime model; hosted flat mappings and synthetic backend stack only.
#include "native_runtime_units.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
using namespace ok;
namespace native = ok::user::native;
namespace hw = ok::arch::execution;
namespace lu = ok::abi::linux_user;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                                            \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)
namespace mock
{
#ifndef OK_NATIVE_MODEL_MEMORY_BYTES
#define OK_NATIVE_MODEL_MEMORY_BYTES 8192
#endif
std::array<std::byte, OK_NATIVE_MODEL_MEMORY_BYTES> memory{};
uptr forbidden{};
uptr write_forbidden{};
usize copies{};
void (*after_usercopy)(){};
u64 copy_clock_step{};
usize outputs{};
usize partial_output{};
uptr last_input{};
usize last_input_size{};
u64 now = 3000000042;
bool clock_fail{};
StatusCode clock_failure = StatusCode::io_error;
usize clock_reads{};
bool realtime_fail = true;
StatusCode realtime_failure = StatusCode::not_initialized;
time::EpochTime epoch{1709210096, 123456789};
usize realtime_reads{};
bool setting_enabled{};
usize setting_calls{};
time::EpochTime last_setting{};
time::MonotonicPoint setting_point{};
Status setting_status{};
usize entries{};
usize syscall_decodes{}; // Observes genuine calls to the hosted architecture syscall decoder.
hw::UserFrame last_frame{};
#ifdef OK_SYSCALL_ORIGIN_MODEL
hw::Trap entry_trap{hw::TrapKind::timer};
void (*owned_hook)(hw::AddressSpace &, hw::UserFrame &, const hw::KernelEntryStackView &){};
#ifdef OK_LINUX_UDP_RESTART_CAPTURE_MODEL
void (*backend_result_hook)(hw::AddressSpace &, hw::UserFrame &, const hw::KernelEntryStackView &, hw::Trap &){};
#endif
bool invalid_capture{};
bool spaces_enabled{};
u32 space_generation{100};
uptr fail_destroy_root{};
#endif
} // namespace mock
namespace ok::arch
{
Status initialize_realtime() { return Status::not_initialized("hosted RTC unavailable"); }
Result<time::MonotonicPoint> monotonic_sample()
{
    auto value = monotonic_nanoseconds();
    return value ? Result<time::MonotonicPoint>(time::MonotonicPoint{value.value(), 1})
                 : Result<time::MonotonicPoint>(value.status());
}
Status set_realtime(time::EpochTime epoch, time::MonotonicPoint point)
{
    ++mock::setting_calls;
    if (!mock::setting_enabled)
        return Status::unsupported("this hosted regression does not exercise clock setting");
    if (!mock::setting_status)
        return mock::setting_status;
    mock::last_setting = epoch;
    mock::setting_point = point;
    return Status::success();
}

time::AnchorInfo realtime_info() { return {}; }
Result<time::EpochTime> realtime()
{
    ++mock::realtime_reads;
    if (mock::realtime_fail) return Status{mock::realtime_failure};
    return mock::epoch;
}
Status initialize_clock()
{
    return Status::success();
}
Result<u64> monotonic_nanoseconds()
{
    ++mock::clock_reads;
    return mock::clock_fail ? Result<u64>(Status{mock::clock_failure}) : Result<u64>(mock::now);
}
u64 clock_resolution_ns()
{
    return mock::clock_fail ? 0 : 1;
}
} // namespace ok::arch
namespace ok::arch::execution
{
bool supported() noexcept
{
    return false;
}
OwnershipReport ownership_report() noexcept { return {}; }
Status initialize()
{
    return Status::unsupported();
}
Result<AddressSpace> create_space(MappingView)
{
#ifdef OK_SYSCALL_ORIGIN_MODEL
    if (mock::spaces_enabled)
    {
        const auto generation = ++mock::space_generation;
        return AddressSpace{0x1000 + generation * 0x1000, static_cast<u32>(generation % 16), generation};
    }
#endif
    return Status::unsupported();
}
Status destroy_space(AddressSpace &space)
{
#ifdef OK_SYSCALL_ORIGIN_MODEL
    if (space.root && space.root == mock::fail_destroy_root)
    { mock::fail_destroy_root = 0; return Status::io_error("hosted table retirement control"); }
#endif
    space = {};
    return Status::success();
}
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &view)
{
    if (!view.valid()) return Status::invalid_argument("hosted entry requires a live lease");
    auto result = enter(space, frame);
    if (result) result.value().kernel_stack_pointer = view.lower(); // Synthetic hosted capture only.
#ifdef OK_SYSCALL_ORIGIN_MODEL
    if (result && mock::invalid_capture) result.value().kernel_stack_pointer = view.top() + 8;
#ifdef OK_LINUX_UDP_RESTART_CAPTURE_MODEL
    if (result && mock::backend_result_hook) mock::backend_result_hook(space, frame, view, result.value());
#endif
    if (mock::owned_hook) mock::owned_hook(space, frame, view);
#endif
    return result;
}
Result<Trap> enter(AddressSpace &, UserFrame &frame)
{
    ++mock::entries; mock::last_frame=frame;
#ifdef OK_SYSCALL_ORIGIN_MODEL
    return mock::entry_trap;
#endif
    return Trap{TrapKind::timer};
}
UserFrame initial_frame(uptr pc, uptr sp)
{
    UserFrame f{};
    f.pc = pc;
    f.sp = sp;
    return f;
}
Syscall syscall(const UserFrame &frame)
{
    ++mock::syscall_decodes;
    return {frame.registers[14], {frame.registers[9], frame.registers[10], frame.registers[11],
            frame.registers[5], frame.registers[7], frame.registers[6]}};
}
void set_return_value(UserFrame &f, i64 v)
{
    f.registers[14] = static_cast<uptr>(v);
}
bool user_range(const AddressSpace &, uptr address, usize size, bool write)
{
    return !size || (address >= user_begin && address - user_begin < mock::memory.size() &&
                     size <= mock::memory.size() - (address - user_begin) &&
                     (!write || !mock::write_forbidden || address > mock::write_forbidden || size <= mock::write_forbidden - address) &&
                     (!mock::forbidden || address >= mock::forbidden + 1 || size <= mock::forbidden - address));
}
Status copy_from_user(const AddressSpace &s, void *out, uptr address, usize size)
{
    ++mock::copies;
    mock::now += mock::copy_clock_step;
    mock::last_input=address; mock::last_input_size=size;
    if (!user_range(s, address, size, false))
        return Status::fault();
    if (size)
        std::memcpy(out, mock::memory.data() + address - user_begin, size);
    if (mock::after_usercopy) mock::after_usercopy();
    return Status::success();
}
Status copy_to_user(const AddressSpace &s, uptr address, const void *in, usize size)
{
    ++mock::outputs;
    if (!user_range(s, address, size, true))
        return Status::fault();
    if (mock::partial_output)
    {
        std::memcpy(mock::memory.data() + address - user_begin, in, mock::partial_output);
        return Status::fault();
    }
    if (size)
        std::memcpy(mock::memory.data() + address - user_begin, in, size);
    if (mock::after_usercopy) mock::after_usercopy();
    return Status::success();
}
} // namespace ok::arch::execution
