#include "kernel_roadmap_tests.hpp"

#include "ok/syscall/linux.hpp"

#include <array>
#include <span>

namespace ok
{
namespace
{

bool p5_bytes_equal(std::span<const std::byte> bytes, std::string_view text)
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

bool p5_ends_with(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

syscall::LinuxSyscallFrame p5_frame(syscall::Number number, std::array<u64, 6> args = {})
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

i64 p5_dispatch(syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller, syscall::Number number,
                std::array<u64, 6> args = {})
{
    auto frame = p5_frame(number, args);
    return dispatcher.dispatch_x86_64(frame, caller);
}

Status expect_errno(Status status, i64 expected)
{
    return syscall::ErrnoMapper::errno_for(status) == expected
               ? Status::success()
               : Status::fault("Linux errno mapping validation failed");
}

Status verify_linux_abi_decode_and_errno()
{
    syscall::LinuxSyscallAbi abi;
    const auto frame = p5_frame(syscall::Number::mmap, std::array<u64, 6>{1, 2, 3, 4, 5, 6});
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
    auto unknown = p5_frame(static_cast<syscall::Number>(999999));
    if (dispatcher.dispatch_x86_64(unknown, caller) != -syscall::linux_ENOSYS ||
        unknown.return_value != -syscall::linux_ENOSYS)
    {
        return Status::fault("unknown Linux syscall did not return -ENOSYS");
    }
    auto bad_write = p5_frame(syscall::Number::write, std::array<u64, 6>{1, 0, 1, 0, 0, 0});
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
    const auto result = p5_dispatch(dispatcher, caller, syscall::Number::write,
                                    std::array<u64, 6>{1, reinterpret_cast<uptr>(text.data()), text.size(), 0, 0, 0});
    const auto buffer = kernel.console().buffer();
    if (result != static_cast<i64>(text.size()) || buffer.size() != before + text.size() ||
        !p5_ends_with(buffer, text))
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

    const auto fd = p5_dispatch(dispatcher, caller, syscall::Number::openat,
                                std::array<u64, 6>{static_cast<u64>(static_cast<i64>(posix::at_FDCWD)),
                                                   reinterpret_cast<uptr>(path),
                                                   posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC, 0644, 0, 0});
    if (fd < 0)
    {
        return Status::fault("Linux openat syscall smoke test failed");
    }
    const auto written = p5_dispatch(dispatcher, caller, syscall::Number::write,
                                     std::array<u64, 6>{static_cast<u64>(fd),
                                                        reinterpret_cast<uptr>(text.data()), text.size(), 0, 0, 0});
    const auto seek = p5_dispatch(dispatcher, caller, syscall::Number::lseek,
                                  std::array<u64, 6>{static_cast<u64>(fd), 0, 0, 0, 0, 0});
    std::array<std::byte, 16> out{};
    const auto read = p5_dispatch(dispatcher, caller, syscall::Number::read,
                                  std::array<u64, 6>{static_cast<u64>(fd), reinterpret_cast<uptr>(out.data()),
                                                     text.size(), 0, 0, 0});
    const auto closed = p5_dispatch(dispatcher, caller, syscall::Number::close,
                                    std::array<u64, 6>{static_cast<u64>(fd), 0, 0, 0, 0, 0});
    if (written != static_cast<i64>(text.size()) || seek != 0 || read != static_cast<i64>(text.size()) || closed != 0 ||
        !p5_bytes_equal(std::span<const std::byte>{out.data(), text.size()}, text))
    {
        return Status::fault("Linux openat/read/write/close syscall smoke test failed");
    }
    return Status::success();
}

Status verify_linux_getdents(syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller)
{
    constexpr char root[] = "/";
    const auto fd = p5_dispatch(dispatcher, caller, syscall::Number::openat,
                                std::array<u64, 6>{static_cast<u64>(static_cast<i64>(posix::at_FDCWD)),
                                                   reinterpret_cast<uptr>(root),
                                                   posix::o_RDONLY | posix::o_DIRECTORY, 0, 0, 0});
    if (fd < 0)
    {
        return Status::fault("Linux getdents directory open failed");
    }
    std::array<std::byte, 256> dirents{};
    const auto bytes = p5_dispatch(dispatcher, caller, syscall::Number::getdents64,
                                   std::array<u64, 6>{static_cast<u64>(fd), reinterpret_cast<uptr>(dirents.data()),
                                                      dirents.size(), 0, 0, 0});
    const auto closed = p5_dispatch(dispatcher, caller, syscall::Number::close,
                                    std::array<u64, 6>{static_cast<u64>(fd), 0, 0, 0, 0, 0});
    if (bytes <= 0 || closed != 0)
    {
        return Status::fault("Linux getdents64 syscall smoke test failed");
    }
    return Status::success();
}

Status verify_linux_memory(syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller)
{
    const auto current_break = p5_dispatch(dispatcher, caller, syscall::Number::brk);
    if (current_break <= 0)
    {
        return Status::fault("Linux brk query syscall smoke test failed");
    }
    const auto next_break = p5_dispatch(dispatcher, caller, syscall::Number::brk,
                                        std::array<u64, 6>{static_cast<u64>(current_break + 4096), 0, 0, 0, 0, 0});
    if (next_break != current_break + 4096)
    {
        return Status::fault("Linux brk set syscall smoke test failed");
    }
    const auto mapping =
        p5_dispatch(dispatcher, caller, syscall::Number::mmap,
                    std::array<u64, 6>{0, 4096, posix::prot_READ | posix::prot_WRITE,
                                       posix::map_PRIVATE | posix::map_ANONYMOUS,
                                       static_cast<u64>(static_cast<i64>(-1)), 0});
    if (mapping <= 0)
    {
        return Status::fault("Linux mmap syscall smoke test failed");
    }
    const auto unmapped = p5_dispatch(dispatcher, caller, syscall::Number::munmap,
                                      std::array<u64, 6>{static_cast<u64>(mapping), 4096, 0, 0, 0, 0});
    return unmapped == 0 ? Status::success() : Status::fault("Linux munmap syscall smoke test failed");
}

Status verify_linux_time(syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller)
{
    posix::ClockTime now{};
    const auto clock = p5_dispatch(dispatcher, caller, syscall::Number::clock_gettime,
                                   std::array<u64, 6>{0, reinterpret_cast<uptr>(&now), 0, 0, 0, 0});
    i64 seconds = 0;
    const auto time = p5_dispatch(dispatcher, caller, syscall::Number::time,
                                  std::array<u64, 6>{reinterpret_cast<uptr>(&seconds), 0, 0, 0, 0, 0});
    if (clock != 0 || time < 0 || seconds != time || now.nanoseconds < 0)
    {
        return Status::fault("Linux time syscall smoke test failed");
    }
    return Status::success();
}

Status verify_linux_futex_random_and_tls(syscall::LinuxSyscallDispatcher &dispatcher, sched::ProcessId caller)
{
    u32 futex_word = 0;
    const auto futex = p5_dispatch(dispatcher, caller, syscall::Number::futex,
                                   std::array<u64, 6>{reinterpret_cast<uptr>(&futex_word), posix::futex_WAKE, 1, 0, 0, 0});
    std::array<std::byte, 8> random{};
    const auto random_bytes = p5_dispatch(dispatcher, caller, syscall::Number::getrandom,
                                          std::array<u64, 6>{reinterpret_cast<uptr>(random.data()), random.size(), 0, 0,
                                                             0, 0});
    const auto set_fs = p5_dispatch(dispatcher, caller, syscall::Number::arch_prctl,
                                    std::array<u64, 6>{posix::arch_SET_FS, 0x7000, 0, 0, 0, 0});
    uptr fs_base = 0;
    const auto get_fs = p5_dispatch(dispatcher, caller, syscall::Number::arch_prctl,
                                    std::array<u64, 6>{posix::arch_GET_FS, reinterpret_cast<uptr>(&fs_base), 0, 0, 0, 0});
    if (futex != 0 || random_bytes != static_cast<i64>(random.size()) || set_fs != 0 || get_fs != 0 ||
        fs_base != 0x7000)
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
    return Status::success();
}

} // namespace

Status run_linux_abi_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    syscall::LinuxSyscallDispatcher dispatcher{&kernel.syscalls()};
    const auto caller = kernel.scheduler().current_pid();

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
