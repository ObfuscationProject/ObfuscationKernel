#include "roadmap_tests.hpp"

#include "ok/syscall/linux.hpp"
#include "ok/abi/linux_sleep.hpp"
#include "ok/abi/linux_stat.hpp"
#include "ok/abi/linux_native.hpp"
#include "ok/time/realtime_clock.hpp"
#include "ok/user/clock_set.hpp"
#include "ok/uapi/syscall.h"

#include <array>
#include <cstddef>
#include <span>

namespace ok
{
static_assert(OK_UAPI_VERSION_MAJOR == 0);
static_assert(OK_UAPI_VERSION_MINOR == 16);
static_assert(OK_UAPI_X86_64_SYSCALL == 1);
static_assert(OK_SYS_READ == static_cast<u64>(syscall::Number::read));
static_assert(OK_SYS_WRITE == static_cast<u64>(syscall::Number::write));
static_assert(OK_SYS_OPENAT == static_cast<u64>(syscall::Number::openat));
static_assert(OK_SYS_SYMLINK == static_cast<u64>(syscall::Number::symlink));
static_assert(OK_SYS_SYMLINKAT == static_cast<u64>(syscall::Number::symlinkat));
static_assert(OK_SYS_READLINK == static_cast<u64>(syscall::Number::readlink));
static_assert(OK_SYS_READLINKAT == static_cast<u64>(syscall::Number::readlinkat));
static_assert(OK_SYS_MMAP == static_cast<u64>(syscall::Number::mmap));
static_assert(OK_SYS_ARCH_PRCTL == static_cast<u64>(syscall::Number::arch_prctl));
static_assert(OK_SYS_CLOCK_SETTIME == static_cast<u64>(syscall::Number::clock_settime));
static_assert(OK_SYS_FSTAT_V2 == static_cast<u64>(syscall::Number::fstat_v2));
static_assert(OK_SYS_NEWFSTATAT_V2 == static_cast<u64>(syscall::Number::newfstatat_v2));
static_assert(OK_SYS_GETRANDOM == static_cast<u64>(syscall::Number::getrandom));
static_assert(OK_SYS_CONSOLE_CLAIM == static_cast<u64>(syscall::Number::console_claim));
static_assert(OK_SYS_CONSOLE_REVOKE == static_cast<u64>(syscall::Number::console_revoke));
static_assert(OK_SYS_CONSOLE_FLUSH == static_cast<u64>(syscall::Number::console_flush));
static_assert(OK_SYS_SYNC_PERSISTENT == static_cast<u64>(syscall::Number::sync_persistent));
static_assert(OK_SYS_OK_DEBUG == static_cast<u64>(syscall::Number::ok_debug));
static_assert(OK_EINVAL == syscall::linux_EINVAL);
static_assert(OK_ENOSYS == syscall::linux_ENOSYS);
static_assert(OK_EFAULT == syscall::linux_EFAULT);
static_assert(OK_AT_FDCWD == posix::at_FDCWD);
static_assert(OK_O_CREAT == posix::o_CREAT);
static_assert(OK_O_DIRECTORY == posix::o_DIRECTORY);
static_assert(OK_PROT_READ == posix::prot_READ);
static_assert(OK_MAP_ANONYMOUS == posix::map_ANONYMOUS);
static_assert(OK_ARCH_SET_FS == posix::arch_SET_FS);
static_assert(OK_FUTEX_WAKE == posix::futex_WAKE);
static_assert(sizeof(ok_timespec) == sizeof(posix::ClockTime));
static_assert(offsetof(ok_timespec, seconds) == offsetof(posix::ClockTime, seconds));
static_assert(offsetof(ok_timespec, nanoseconds) == offsetof(posix::ClockTime, nanoseconds));
static_assert(sizeof(ok_iovec) == sizeof(posix::IoVector));
static_assert(offsetof(ok_iovec, base) == offsetof(posix::IoVector, base));
static_assert(offsetof(ok_iovec, length) == offsetof(posix::IoVector, length));
static_assert(sizeof(ok_stat) == offsetof(posix::FileStatus, attributes));
static_assert(offsetof(ok_stat, size) == offsetof(posix::FileStatus, size));
static_assert(offsetof(ok_stat, mode) == offsetof(posix::FileStatus, mode));
static_assert(offsetof(ok_stat, blocks) == offsetof(posix::FileStatus, blocks));
static_assert(sizeof(ok_rlimit) == sizeof(posix::ResourceLimit));
static_assert(offsetof(ok_rlimit, current) == offsetof(posix::ResourceLimit, current));
static_assert(offsetof(ok_rlimit, maximum) == offsetof(posix::ResourceLimit, maximum));
static_assert(sizeof(ok_sysinfo) == sizeof(posix::SystemInfo));
static_assert(offsetof(ok_sysinfo, process_count) == offsetof(posix::SystemInfo, process_count));

namespace
{

bool linux_bytes_equal(std::span<const std::byte> bytes, std::string_view text)
{
    if (bytes.size() != text.size())
    {
        return false;
    }
    for (usize i = 0; i < text.size(); ++i)
    {
        if (bytes[i] != static_cast<std::byte>(text[i]))
        {
            return false;
        }
    }
    return true;
}

bool linux_ends_with(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

syscall::LinuxSyscallFrame linux_frame(syscall::Number number, std::array<u64, 6> args = {})
{
    return syscall::LinuxSyscallFrame{
        .syscall_number = static_cast<u64>(number),
        .rdi = args[0],
        .rsi = args[1],
        .rdx = args[2],
        .r10 = args[3],
        .r8 = args[4],
        .r9 = args[5],
    };
}

i64 linux_dispatch(syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller, syscall::Number number,
                   std::array<u64, 6> args = {})
{
    auto frame = linux_frame(number, args);
    return dispatcher.dispatch_x86_64(frame, caller);
}

Status expect_errno(Status status, i64 expected)
{
    return syscall::ErrnoMapper::errno_for(status) == expected ? Status::success()
                                                               : Status::fault("Linux errno mapping validation failed");
}

Status verify_linux_stat_projection()
{
    // Kernel debug codec model only. Guest syscall and provider qualification
    // have separate OS-owned execution/image tests.
    namespace lu = abi::linux_user;
    using Codec = lu::StatCodec<lu::configured_native_profile()>;
    posix::FileStatus metadata{};
    metadata.type = fs::NodeType::regular;
    metadata.mode = fs::mode_for(fs::NodeType::regular, 0644);
    metadata.size = 33;
    metadata.link_count = 0; // A held unlinked inode remains representable.
    metadata.block_size = 1024;
    metadata.blocks = 2;
    auto &attributes = metadata.attributes;
    attributes.inode = 27;
    attributes.device = {0, 9};
    for (auto field : {fs::Attribute::inode, fs::Attribute::device, fs::Attribute::access_time,
                      fs::Attribute::modification_time, fs::Attribute::change_time, fs::Attribute::blocks512})
        attributes.mark(field);
    attributes.access = {-1, 999999999, fs::TimestampSource::readonly_ext4};
    attributes.modification = {2208988800, 1234, fs::TimestampSource::readonly_ext4};
    attributes.change = {0, 0, fs::TimestampSource::readonly_ext4};
    std::array<u8, 256> bytes{};
    const auto encoded = Codec::encode(metadata, lu::StatLayout::statx, bytes);
    if (!encoded || encoded.value() != bytes.size())
        return Status::fault("Linux statx debug projection failed");
    for (usize index = 144; index < bytes.size(); ++index)
        if (bytes[index] != 0)
            return Status::fault("Linux statx reserved bytes are not zero");
    auto reject_without_output = [&](StatusCode expected) {
        bytes.fill(0xa5);
        const auto result = Codec::encode(metadata, lu::StatLayout::statx, bytes);
        if (result || result.status().code() != expected)
            return false;
        for (const auto value : bytes)
            if (value != 0xa5)
                return false;
        return true;
    };
    attributes.device = {};
    if (!reject_without_output(StatusCode::unsupported))
        return Status::fault("Linux statx zero device admission failed");
    attributes.device = {0, 9};
    metadata.blocks = 0x8000000000000000ull;
    if (!reject_without_output(StatusCode::overflow))
        return Status::fault("Linux statx signed libc block bound failed");
    metadata.blocks = 2;
    attributes.valid &= ~static_cast<u32>(fs::Attribute::blocks512);
    if (!reject_without_output(StatusCode::unsupported))
        return Status::fault("Linux statx allocation qualification failed");
    attributes.mark(fs::Attribute::blocks512);
    if constexpr (Codec::has_kernel_stat)
    {
        const auto size = Codec::size(lu::StatLayout::kernel_stat);
        const auto legacy = Codec::encode(metadata, lu::StatLayout::kernel_stat, {bytes.data(), size});
        if (!legacy || legacy.value() != size)
            return Status::fault("Linux kernel-stat debug projection failed");
    }
    return Status::success();
}

Status verify_linux_abi_decode_and_errno()
{
    // Local clock state only: debug models must never set the live boot clock.
    time::RealtimeClock local_clock;
    static_cast<void>(local_clock.reject_boot(Status::unsupported("model has no RTC")));
    if (!local_clock.set({100, 9}, {1000, 1}, 1))
        return Status::fault("software clock model publication failed");
    const auto first = local_clock.read({1017, 1});
    if (!first || first.value().value != time::EpochTime{100, 26} ||
        first.value().origin != time::RealtimeOrigin::administrator_software ||
        local_clock.boot_info().state != time::AnchorState::unavailable ||
        !local_clock.set({90, 0}, {1020, 1}, 1) || local_clock.info().wall_generation != 2)
        return Status::fault("software clock model provenance or backwards step failed");
    if (local_clock.read({1021, 2}) || local_clock.set({200, 0}, {1022, 1}, 1))
        return Status::fault("software clock model revived after continuity loss");
    usize samples = 0, commits = 0;
    const auto denied = user::set_realtime_checked(
        {100, 0}, user::default_user_credentials(),
        [&]() -> Result<time::MonotonicPoint> { ++samples; return time::MonotonicPoint{1000, 1}; },
        [&](time::EpochTime, time::MonotonicPoint) { ++commits; return Status::success(); });
    if (denied.code() != StatusCode::permission || samples || commits)
        return Status::fault("software clock model permission ordering failed");
    // Deadline arithmetic is a model gate; actual sleeping is a separate guest test.
    namespace lu = abi::linux_user;
    if (lu::linux_sleep_deadline(42, {1, 2}, false) != 1000000044 ||
        lu::linux_sleep_deadline(42, {0, 0}, true) != 0 ||
        lu::linux_sleep_deadline(42, {9223372036LL, 0}, false) != lu::ktime_maximum ||
        lu::linux_sleep_deadline(lu::ktime_maximum - 1, {0, 2}, false) != lu::ktime_maximum)
        return Status::fault("Linux bounded sleep deadline model failed");
    syscall::LinuxSyscallAbi abi;
    const auto frame = linux_frame(syscall::Number::mmap, std::array<u64, 6>{1, 2, 3, 4, 5, 6});
    const auto request = abi.decode_x86_64(frame, 99);
    if (request.number != syscall::Number::mmap || request.caller != 99 || request.args[0] != 1 ||
        request.args[1] != 2 || request.args[2] != 3 || request.args[3] != 4 || request.args[4] != 5 ||
        request.args[5] != 6)
    {
        return Status::fault("Linux x86_64 syscall ABI argument decode failed");
    }
    if (auto status = expect_errno(Status::invalid_argument("invalid"), syscall::linux_EINVAL); !status.ok())
    {
        return status;
    }
    if (auto status = expect_errno(Status::not_found("missing"), syscall::linux_ENOENT); !status.ok())
    {
        return status;
    }
    if (auto status = expect_errno(Status::no_memory("oom"), syscall::linux_ENOMEM); !status.ok())
    {
        return status;
    }
    if (auto status = expect_errno(Status::unsupported("unknown"), syscall::linux_ENOSYS); !status.ok())
    {
        return status;
    }
    if (auto status = expect_errno(Status::would_block("again"), syscall::linux_EAGAIN); !status.ok())
    {
        return status;
    }
    if (auto status = expect_errno(Status::already_exists("exists"), syscall::linux_EEXIST); !status.ok())
    {
        return status;
    }
    if (auto status = expect_errno(Status::overflow("overflow"), syscall::linux_EOVERFLOW); !status.ok())
    {
        return status;
    }
    if (auto status = expect_errno(Status::not_initialized("io"), syscall::linux_EIO); !status.ok())
    {
        return status;
    }
    if (auto status = expect_errno(Status::fault("fault"), syscall::linux_EFAULT); !status.ok())
    {
        return status;
    }
    return Status::success();
}

Status verify_linux_dispatch_edges(syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller)
{
    auto unknown = linux_frame(static_cast<syscall::Number>(999999));
    if (dispatcher.dispatch_x86_64(unknown, caller) != -syscall::linux_ENOSYS ||
        unknown.return_value != -syscall::linux_ENOSYS)
    {
        return Status::fault("unknown Linux syscall did not return -ENOSYS");
    }
    auto bad_write = linux_frame(syscall::Number::write, std::array<u64, 6>{1, 0, 1, 0, 0, 0});
    if (dispatcher.dispatch_x86_64(bad_write, caller) != -syscall::linux_EFAULT)
    {
        return Status::fault("invalid Linux user pointer did not return -EFAULT");
    }
    return Status::success();
}

Status verify_linux_write(Kernel &kernel, syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller)
{
    constexpr std::string_view text{"linux-write"};
    const auto before = kernel.console().buffer().size();
    const auto result =
        linux_dispatch(dispatcher, caller, syscall::Number::write,
                       std::array<u64, 6>{1, reinterpret_cast<uptr>(text.data()), text.size(), 0, 0, 0});
    const auto buffer = kernel.console().buffer();
    if (result != static_cast<i64>(text.size()) || buffer.size() != before + text.size() ||
        !linux_ends_with(buffer, text))
    {
        return Status::fault("Linux write syscall did not reach console");
    }
    return Status::success();
}

Status verify_linux_file_io(Kernel &kernel, syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller)
{
    constexpr char path[] = "/tmp/linux-openat";
    constexpr std::string_view text{"linux-file"};
    static_cast<void>(kernel.posix().unlink(path));

    const auto fd = linux_dispatch(dispatcher, caller, syscall::Number::openat,
                                   std::array<u64, 6>{static_cast<u64>(static_cast<i64>(posix::at_FDCWD)),
                                                      reinterpret_cast<uptr>(path),
                                                      posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC, 0644, 0, 0});
    if (fd < 0)
    {
        return Status::fault("Linux openat syscall smoke test failed");
    }
    const auto written = linux_dispatch(
        dispatcher, caller, syscall::Number::write,
        std::array<u64, 6>{static_cast<u64>(fd), reinterpret_cast<uptr>(text.data()), text.size(), 0, 0, 0});
    const auto seek = linux_dispatch(dispatcher, caller, syscall::Number::lseek,
                                     std::array<u64, 6>{static_cast<u64>(fd), 0, 0, 0, 0, 0});
    std::array<std::byte, 16> out{};
    const auto read = linux_dispatch(
        dispatcher, caller, syscall::Number::read,
        std::array<u64, 6>{static_cast<u64>(fd), reinterpret_cast<uptr>(out.data()), text.size(), 0, 0, 0});
    const auto closed = linux_dispatch(dispatcher, caller, syscall::Number::close,
                                       std::array<u64, 6>{static_cast<u64>(fd), 0, 0, 0, 0, 0});
    if (written != static_cast<i64>(text.size()) || seek != 0 || read != static_cast<i64>(text.size()) || closed != 0 ||
        !linux_bytes_equal(std::span<const std::byte>{out.data(), text.size()}, text))
    {
        return Status::fault("Linux openat/read/write/close syscall smoke test failed");
    }
    return Status::success();
}

// Native table record projection; this does not enable Linux stat wire layouts.
Status verify_native_stat_projection(Kernel &kernel, sched::ProcessId caller)
{
    constexpr char path[] = "/tmp/linux-openat";
    auto opened = kernel.posix().open(path, posix::o_RDONLY);
    if (!opened) return opened.status();
    Status result;
    for (auto number : {syscall::Number::stat, syscall::Number::lstat, syscall::Number::fstat, syscall::Number::newfstatat})
    {
        struct Output { ok_stat value{}; std::array<u8, 128> guard{}; } output;
        output.guard.fill(0xa5);
        auto *raw = reinterpret_cast<u8 *>(&output.value);
        for (usize i = 0; i < sizeof(ok_stat); ++i) raw[i] = 0xa5;
        syscall::Request request{.number = number, .caller = caller};
        if (number == syscall::Number::newfstatat)
            request.args = {static_cast<u64>(static_cast<i64>(posix::at_FDCWD)), reinterpret_cast<uptr>(path), reinterpret_cast<uptr>(&output.value), 0, 0, 0};
        else
            request.args = {number == syscall::Number::fstat ? static_cast<u64>(opened.value()) : reinterpret_cast<uptr>(path), reinterpret_cast<uptr>(&output.value), 0, 0, 0, 0};
        auto response = kernel.syscalls().dispatch(request);
        if (!response.status || response.value || output.value.size != 10 ||
            output.value.type != static_cast<u8>(fs::NodeType::regular) || output.value.link_count != 1)
            result = Status::fault("native stat projection changed existing fields");

        std::array<bool, sizeof(ok_stat)> fields{};
        auto field = [&](usize offset, usize size) { for (usize i = 0; i < size; ++i) fields[offset + i] = true; };
        field(offsetof(ok_stat, type), sizeof(output.value.type));
        field(offsetof(ok_stat, size), sizeof(output.value.size));
        field(offsetof(ok_stat, mode), sizeof(output.value.mode));
        field(offsetof(ok_stat, uid), sizeof(output.value.uid));
        field(offsetof(ok_stat, gid), sizeof(output.value.gid));
        field(offsetof(ok_stat, link_count), sizeof(output.value.link_count));
        field(offsetof(ok_stat, block_size), sizeof(output.value.block_size));
        field(offsetof(ok_stat, blocks), sizeof(output.value.blocks));
        for (usize i = 0; i < sizeof(ok_stat); ++i)
            if (!fields[i] && raw[i]) result = Status::fault("native stat exposed unspecified padding");
        for (auto byte : output.guard)
            if (byte != 0xa5) result = Status::fault("native stat wrote beyond unchanged UAPI record");
    }
    auto closed = kernel.posix().close(opened.value());
    return result ? closed : result;
}

Status verify_linux_getdents(syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller)
{
    constexpr char root[] = "/";
    const auto fd =
        linux_dispatch(dispatcher, caller, syscall::Number::openat,
                       std::array<u64, 6>{static_cast<u64>(static_cast<i64>(posix::at_FDCWD)),
                                          reinterpret_cast<uptr>(root), posix::o_RDONLY | posix::o_DIRECTORY, 0, 0, 0});
    if (fd < 0)
    {
        return Status::fault("Linux getdents directory open failed");
    }
    std::array<std::byte, 256> dirents{};
    const auto bytes = linux_dispatch(
        dispatcher, caller, syscall::Number::getdents64,
        std::array<u64, 6>{static_cast<u64>(fd), reinterpret_cast<uptr>(dirents.data()), dirents.size(), 0, 0, 0});
    const auto closed = linux_dispatch(dispatcher, caller, syscall::Number::close,
                                       std::array<u64, 6>{static_cast<u64>(fd), 0, 0, 0, 0, 0});
    if (bytes <= 0 || closed != 0)
    {
        return Status::fault("Linux getdents64 syscall smoke test failed");
    }
    return Status::success();
}

Status verify_linux_memory(syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller)
{
    const auto current_break = linux_dispatch(dispatcher, caller, syscall::Number::brk);
    if (current_break <= 0)
    {
        return Status::fault("Linux brk query syscall smoke test failed");
    }
    const auto next_break = linux_dispatch(dispatcher, caller, syscall::Number::brk,
                                           std::array<u64, 6>{static_cast<u64>(current_break + 4096), 0, 0, 0, 0, 0});
    if (next_break != current_break + 4096)
    {
        return Status::fault("Linux brk set syscall smoke test failed");
    }
    const auto mapping = linux_dispatch(dispatcher, caller, syscall::Number::mmap,
                                        std::array<u64, 6>{0, 4096, posix::prot_READ | posix::prot_WRITE,
                                                           posix::map_PRIVATE | posix::map_ANONYMOUS,
                                                           static_cast<u64>(static_cast<i64>(-1)), 0});
    if (mapping <= 0)
    {
        return Status::fault("Linux mmap syscall smoke test failed");
    }
    const auto unmapped = linux_dispatch(dispatcher, caller, syscall::Number::munmap,
                                         std::array<u64, 6>{static_cast<u64>(mapping), 4096, 0, 0, 0, 0});
    return unmapped == 0 ? Status::success() : Status::fault("Linux munmap syscall smoke test failed");
}

Status verify_linux_time(syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller)
{
    posix::ClockTime now{};
    const auto clock = linux_dispatch(dispatcher, caller, syscall::Number::clock_gettime,
                                      std::array<u64, 6>{1, reinterpret_cast<uptr>(&now), 0, 0, 0, 0});
    i64 seconds = 123;
    const auto time = linux_dispatch(dispatcher, caller, syscall::Number::time,
                                     std::array<u64, 6>{reinterpret_cast<uptr>(&seconds), 0, 0, 0, 0, 0});
    // Standalone trusted-model tests run before the native runtime's one-time
    // RTC acquisition. They must not publish boot-relative time as UTC.
    if (clock != 0 || time != -5 || seconds != 123 || now.nanoseconds < 0)
    {
        return Status::fault("Linux time syscall smoke test failed");
    }
    return Status::success();
}

Status verify_linux_futex_random_and_tls(syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller)
{
    u32 futex_word = 0;
    const auto futex =
        linux_dispatch(dispatcher, caller, syscall::Number::futex,
                       std::array<u64, 6>{reinterpret_cast<uptr>(&futex_word), posix::futex_WAKE, 1, 0, 0, 0});
    std::array<std::byte, 8> random{};
    random.fill(std::byte{0xa5});
    const auto random_bytes =
        linux_dispatch(dispatcher, caller, syscall::Number::getrandom,
                       std::array<u64, 6>{reinterpret_cast<uptr>(random.data()), random.size(), 0, 0, 0, 0});
    const auto set_fs = linux_dispatch(dispatcher, caller, syscall::Number::arch_prctl,
                                       std::array<u64, 6>{posix::arch_SET_FS, 0x7000, 0, 0, 0, 0});
    uptr fs_base = 0;
    const auto get_fs =
        linux_dispatch(dispatcher, caller, syscall::Number::arch_prctl,
                       std::array<u64, 6>{posix::arch_GET_FS, reinterpret_cast<uptr>(&fs_base), 0, 0, 0, 0});
    bool untouched = true;
    for (auto byte : random) untouched &= byte == std::byte{0xa5};
    if (futex != 0 || random_bytes != -syscall::linux_EAGAIN || !untouched ||
        set_fs != 0 || get_fs != 0 || fs_base != 0x7000)
    {
        return Status::fault("Linux futex, random, or TLS syscall smoke test failed");
    }

    syscall::LinuxCompatProcess process;
    process.set_tls_base(fs_base);
    syscall::LinuxVdsoPlaceholder vdso;
    if (process.tls_base() != fs_base || !process.auxv().add(3, 0x400000).ok() ||
        process.auxv().entries().size() != 1 || vdso.enabled() || vdso.base() != 0)
    {
        return Status::fault("Linux compat process TLS, auxv, or vdso placeholder validation failed");
    }

    syscall::LinuxInitialStackBuilder stack_builder;
    static constexpr std::array<std::string_view, 2> argv{"hello", "world"};
    static constexpr std::array<std::string_view, 1> envp{"PATH=/"};
    auto stack = stack_builder.build(0x800000, argv, envp, process.auxv().entries());
    if (!stack || stack.value().argc != argv.size() || stack.value().stack_pointer == 0 ||
        (stack.value().stack_pointer % 16u) != 0 || stack.value().argv_pointer >= stack.value().envp_pointer ||
        stack.value().envp_pointer >= stack.value().auxv_pointer || stack_builder.words().size() != stack.value().words)
    {
        return Status::fault("Linux initial stack layout validation failed");
    }
    return Status::success();
}

} // namespace

Status run_linux_abi_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    syscall::LinuxSyscallDispatcher dispatcher{&kernel.syscalls()};
    const auto caller = kernel.scheduler().current_pid();

    if (auto status = verify_linux_stat_projection(); !status.ok())
        return status;

    if (auto status = verify_linux_abi_decode_and_errno(); !status.ok())
    {
        return status;
    }
    if (auto status = verify_linux_dispatch_edges(dispatcher, caller); !status.ok())
    {
        return status;
    }
    if (auto status = verify_linux_write(kernel, dispatcher, caller); !status.ok())
    {
        return status;
    }
    if (auto status = verify_linux_file_io(kernel, dispatcher, caller); !status.ok())
    {
        return status;
    }
    if (auto status = verify_native_stat_projection(kernel, caller); !status.ok())
    {
        return status;
    }
    if (auto status = verify_linux_getdents(dispatcher, caller); !status.ok())
    {
        return status;
    }
    if (auto status = verify_linux_memory(dispatcher, caller); !status.ok())
    {
        return status;
    }
    if (auto status = verify_linux_time(dispatcher, caller); !status.ok())
    {
        return status;
    }
    if (auto status = verify_linux_futex_random_and_tls(dispatcher, caller); !status.ok())
    {
        return status;
    }

    report.linux_abi = true;
    report.linux_syscalls = true;
    return Status::success();
}

} // namespace ok
