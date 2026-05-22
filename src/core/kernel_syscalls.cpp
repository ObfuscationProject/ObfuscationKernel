#include "ok/core/kernel.hpp"

namespace ok
{
namespace
{

std::string_view bounded_c_string(uptr address)
{
    if (address == 0)
    {
        return {};
    }
    const auto *text = reinterpret_cast<const char *>(address);
    usize size = 0;
    while (size < 255 && text[size] != '\0')
    {
        ++size;
    }
    return {text, size};
}

posix::SeekWhence seek_whence(u64 value)
{
    switch (value)
    {
    case 1:
        return posix::SeekWhence::current;
    case 2:
        return posix::SeekWhence::end;
    default:
        return posix::SeekWhence::set;
    }
}

syscall::Response status_response(Status status)
{
    return syscall::Response{.value = status.ok() ? 0 : -1, .status = status};
}

syscall::Response signed_response(Result<i64> result)
{
    return syscall::Response{.value = result ? result.value() : -1,
                             .status = result ? Status::success() : result.status()};
}

syscall::Response size_response(Result<usize> result)
{
    return syscall::Response{.value = result ? static_cast<i64>(result.value()) : -1,
                             .status = result ? Status::success() : result.status()};
}

syscall::Response fd_response(Result<posix::Fd> result)
{
    return syscall::Response{.value = result ? static_cast<i64>(result.value()) : -1,
                             .status = result ? Status::success() : result.status()};
}

syscall::Response pointer_response(Result<uptr> result)
{
    return syscall::Response{.value = result ? static_cast<i64>(result.value()) : -1,
                             .status = result ? Status::success() : result.status()};
}

syscall::Response unsupported(std::string_view name)
{
    return syscall::Response{.value = -1, .status = Status::unsupported(name)};
}

Status register_posix(syscall::Table &table, syscall::Number number, std::string_view name, posix::PosixService &posix,
                      syscall::Callback callback)
{
    return table.register_callback(number, name, &posix, callback);
}

} // namespace

Status Kernel::register_builtin_syscalls(posix::PosixService &posix)
{
    auto add = [&](syscall::Number number, std::string_view name, syscall::Callback callback) {
        return register_posix(syscalls_, number, name, posix, callback);
    };

    if (auto status = add(syscall::Number::getpid, "getpid", [](void *context, const syscall::Request &) {
            return syscall::Response{.value = static_cast<i64>(static_cast<posix::PosixService *>(context)->getpid()),
                                     .status = Status::success()};
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::getppid, "getppid", [](void *context, const syscall::Request &) {
            return syscall::Response{.value = static_cast<i64>(static_cast<posix::PosixService *>(context)->getppid()),
                                     .status = Status::success()};
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::gettid, "gettid", [](void *context, const syscall::Request &) {
            return syscall::Response{.value = static_cast<i64>(static_cast<posix::PosixService *>(context)->getpid()),
                                     .status = Status::success()};
        });
        !status.ok())
    {
        return status;
    }
    for (auto number : {syscall::Number::getuid, syscall::Number::geteuid, syscall::Number::getgid,
                        syscall::Number::getegid})
    {
        if (auto status = add(number, "identity", [](void *, const syscall::Request &) {
                return syscall::Response{.value = 0, .status = Status::success()};
            });
            !status.ok())
        {
            return status;
        }
    }

    if (auto status = add(syscall::Number::read, "read", [](void *context, const syscall::Request &request) {
            if (request.args[1] == 0 && request.args[2] != 0)
            {
                return syscall::Response{.value = -1, .status = Status::fault("read buffer is null")};
            }
            auto *buffer = reinterpret_cast<std::byte *>(request.args[1]);
            auto result = static_cast<posix::PosixService *>(context)->read(
                static_cast<posix::Fd>(request.args[0]),
                std::span<std::byte>{buffer, static_cast<usize>(request.args[2])});
            return size_response(result);
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::write, "write", [](void *context, const syscall::Request &request) {
            if (request.args[1] == 0 && request.args[2] != 0)
            {
                return syscall::Response{.value = -1, .status = Status::fault("write buffer is null")};
            }
            const auto *bytes = reinterpret_cast<const std::byte *>(request.args[1]);
            auto result = static_cast<posix::PosixService *>(context)->write(
                static_cast<posix::Fd>(request.args[0]),
                std::span<const std::byte>{bytes, static_cast<usize>(request.args[2])});
            return size_response(result);
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::pread64, "pread64", [](void *context, const syscall::Request &request) {
            auto *buffer = reinterpret_cast<std::byte *>(request.args[1]);
            auto result = static_cast<posix::PosixService *>(context)->pread(
                static_cast<posix::Fd>(request.args[0]),
                std::span<std::byte>{buffer, static_cast<usize>(request.args[2])}, static_cast<usize>(request.args[3]));
            return size_response(result);
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::pwrite64, "pwrite64", [](void *context, const syscall::Request &request) {
            const auto *bytes = reinterpret_cast<const std::byte *>(request.args[1]);
            auto result = static_cast<posix::PosixService *>(context)->pwrite(
                static_cast<posix::Fd>(request.args[0]),
                std::span<const std::byte>{bytes, static_cast<usize>(request.args[2])},
                static_cast<usize>(request.args[3]));
            return size_response(result);
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::readv, "readv", [](void *context, const syscall::Request &request) {
            auto *vectors = reinterpret_cast<const posix::IoVector *>(request.args[1]);
            auto result = static_cast<posix::PosixService *>(context)->readv(
                static_cast<posix::Fd>(request.args[0]),
                std::span<const posix::IoVector>{vectors, static_cast<usize>(request.args[2])});
            return size_response(result);
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::writev, "writev", [](void *context, const syscall::Request &request) {
            auto *vectors = reinterpret_cast<const posix::IoVector *>(request.args[1]);
            auto result = static_cast<posix::PosixService *>(context)->writev(
                static_cast<posix::Fd>(request.args[0]),
                std::span<const posix::IoVector>{vectors, static_cast<usize>(request.args[2])});
            return size_response(result);
        });
        !status.ok())
    {
        return status;
    }

    if (auto status = add(syscall::Number::open, "open", [](void *context, const syscall::Request &request) {
            auto result = static_cast<posix::PosixService *>(context)->open(
                bounded_c_string(request.args[0]), static_cast<u32>(request.args[1]), static_cast<u32>(request.args[2]));
            return fd_response(result);
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::openat, "openat", [](void *context, const syscall::Request &request) {
            auto result = static_cast<posix::PosixService *>(context)->openat(
                static_cast<posix::Fd>(request.args[0]), bounded_c_string(request.args[1]),
                static_cast<u32>(request.args[2]), static_cast<u32>(request.args[3]));
            return fd_response(result);
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::creat, "creat", [](void *context, const syscall::Request &request) {
            auto result = static_cast<posix::PosixService *>(context)->open(
                bounded_c_string(request.args[0]), posix::o_CREAT | posix::o_WRONLY | posix::o_TRUNC,
                static_cast<u32>(request.args[1]));
            return fd_response(result);
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::close, "close", [](void *context, const syscall::Request &request) {
            return status_response(static_cast<posix::PosixService *>(context)->close(
                static_cast<posix::Fd>(request.args[0])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::close_range, "close_range", [](void *context, const syscall::Request &request) {
            const auto last = request.args[1] >= posix::max_open_files
                                  ? static_cast<posix::Fd>(posix::max_open_files - 1)
                                  : static_cast<posix::Fd>(request.args[1]);
            return status_response(static_cast<posix::PosixService *>(context)->close_range(
                static_cast<posix::Fd>(request.args[0]), last));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::dup, "dup", [](void *context, const syscall::Request &request) {
            return fd_response(static_cast<posix::PosixService *>(context)->duplicate(
                static_cast<posix::Fd>(request.args[0]), 0));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::dup2, "dup2", [](void *context, const syscall::Request &request) {
            return fd_response(static_cast<posix::PosixService *>(context)->duplicate_to(
                static_cast<posix::Fd>(request.args[0]), static_cast<posix::Fd>(request.args[1])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::dup3, "dup3", [](void *context, const syscall::Request &request) {
            return fd_response(static_cast<posix::PosixService *>(context)->duplicate_to(
                static_cast<posix::Fd>(request.args[0]), static_cast<posix::Fd>(request.args[1]),
                static_cast<u32>(request.args[2])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::lseek, "lseek", [](void *context, const syscall::Request &request) {
            auto result = static_cast<posix::PosixService *>(context)->seek(
                static_cast<posix::Fd>(request.args[0]), static_cast<i64>(request.args[1]), seek_whence(request.args[2]));
            return size_response(result);
        });
        !status.ok())
    {
        return status;
    }

    if (auto status = add(syscall::Number::stat, "stat", [](void *context, const syscall::Request &request) {
            auto result = static_cast<posix::PosixService *>(context)->stat(bounded_c_string(request.args[0]));
            if (result && request.args[1] != 0)
            {
                *reinterpret_cast<posix::FileStatus *>(request.args[1]) = result.value();
            }
            return syscall::Response{.value = result ? 0 : -1, .status = result ? Status::success() : result.status()};
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::lstat, "lstat", [](void *context, const syscall::Request &request) {
            auto result = static_cast<posix::PosixService *>(context)->stat(bounded_c_string(request.args[0]));
            if (result && request.args[1] != 0)
            {
                *reinterpret_cast<posix::FileStatus *>(request.args[1]) = result.value();
            }
            return syscall::Response{.value = result ? 0 : -1, .status = result ? Status::success() : result.status()};
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::fstat, "fstat", [](void *context, const syscall::Request &request) {
            auto result =
                static_cast<posix::PosixService *>(context)->fstat(static_cast<posix::Fd>(request.args[0]));
            if (result && request.args[1] != 0)
            {
                *reinterpret_cast<posix::FileStatus *>(request.args[1]) = result.value();
            }
            return syscall::Response{.value = result ? 0 : -1, .status = result ? Status::success() : result.status()};
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::newfstatat, "newfstatat", [](void *context, const syscall::Request &request) {
            auto result = static_cast<posix::PosixService *>(context)->statat(
                static_cast<posix::Fd>(request.args[0]), bounded_c_string(request.args[1]),
                static_cast<u32>(request.args[3]));
            if (result && request.args[2] != 0)
            {
                *reinterpret_cast<posix::FileStatus *>(request.args[2]) = result.value();
            }
            return syscall::Response{.value = result ? 0 : -1, .status = result ? Status::success() : result.status()};
        });
        !status.ok())
    {
        return status;
    }

    if (auto status = add(syscall::Number::mkdir, "mkdir", [](void *context, const syscall::Request &request) {
            return status_response(static_cast<posix::PosixService *>(context)->mkdir(
                bounded_c_string(request.args[0]), static_cast<u32>(request.args[1])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::mkdirat, "mkdirat", [](void *context, const syscall::Request &request) {
            return status_response(static_cast<posix::PosixService *>(context)->mkdirat(
                static_cast<posix::Fd>(request.args[0]), bounded_c_string(request.args[1]),
                static_cast<u32>(request.args[2])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::unlink, "unlink", [](void *context, const syscall::Request &request) {
            return status_response(
                static_cast<posix::PosixService *>(context)->unlink(bounded_c_string(request.args[0])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::unlinkat, "unlinkat", [](void *context, const syscall::Request &request) {
            return status_response(static_cast<posix::PosixService *>(context)->unlinkat(
                static_cast<posix::Fd>(request.args[0]), bounded_c_string(request.args[1]),
                static_cast<u32>(request.args[2])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::rmdir, "rmdir", [](void *context, const syscall::Request &request) {
            return status_response(
                static_cast<posix::PosixService *>(context)->rmdir(bounded_c_string(request.args[0])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::chdir, "chdir", [](void *context, const syscall::Request &request) {
            return status_response(
                static_cast<posix::PosixService *>(context)->chdir(bounded_c_string(request.args[0])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::fchdir, "fchdir", [](void *context, const syscall::Request &request) {
            return status_response(
                static_cast<posix::PosixService *>(context)->fchdir(static_cast<posix::Fd>(request.args[0])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::getcwd, "getcwd", [](void *context, const syscall::Request &request) {
            auto cwd = static_cast<posix::PosixService *>(context)->getcwd();
            auto *out = reinterpret_cast<char *>(request.args[0]);
            const auto capacity = static_cast<usize>(request.args[1]);
            if (out == nullptr || capacity <= cwd.size())
            {
                return syscall::Response{.value = -1, .status = Status::overflow("getcwd buffer too small")};
            }
            for (usize i = 0; i < cwd.size(); ++i)
            {
                out[i] = cwd[i];
            }
            out[cwd.size()] = '\0';
            return syscall::Response{.value = static_cast<i64>(cwd.size()), .status = Status::success()};
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::getdents, "getdents", [](void *context, const syscall::Request &request) {
            return size_response(static_cast<posix::PosixService *>(context)->getdents64(
                static_cast<posix::Fd>(request.args[0]),
                std::span<std::byte>{reinterpret_cast<std::byte *>(request.args[1]), static_cast<usize>(request.args[2])}));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::getdents64, "getdents64", [](void *context, const syscall::Request &request) {
            return size_response(static_cast<posix::PosixService *>(context)->getdents64(
                static_cast<posix::Fd>(request.args[0]),
                std::span<std::byte>{reinterpret_cast<std::byte *>(request.args[1]), static_cast<usize>(request.args[2])}));
        });
        !status.ok())
    {
        return status;
    }

    if (auto status = add(syscall::Number::access, "access", [](void *context, const syscall::Request &request) {
            return status_response(static_cast<posix::PosixService *>(context)->access(
                bounded_c_string(request.args[0]), static_cast<u32>(request.args[1])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::faccessat, "faccessat", [](void *context, const syscall::Request &request) {
            return status_response(static_cast<posix::PosixService *>(context)->faccessat(
                static_cast<posix::Fd>(request.args[0]), bounded_c_string(request.args[1]),
                static_cast<u32>(request.args[2]), static_cast<u32>(request.args[3])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::faccessat2, "faccessat2", [](void *context, const syscall::Request &request) {
            return status_response(static_cast<posix::PosixService *>(context)->faccessat(
                static_cast<posix::Fd>(request.args[0]), bounded_c_string(request.args[1]),
                static_cast<u32>(request.args[2]), static_cast<u32>(request.args[3])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::fcntl, "fcntl", [](void *context, const syscall::Request &request) {
            return signed_response(static_cast<posix::PosixService *>(context)->fcntl(
                static_cast<posix::Fd>(request.args[0]), static_cast<u32>(request.args[1]), request.args[2]));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::ioctl, "ioctl", [](void *context, const syscall::Request &request) {
            return signed_response(static_cast<posix::PosixService *>(context)->ioctl(
                static_cast<posix::Fd>(request.args[0]), request.args[1], request.args[2]));
        });
        !status.ok())
    {
        return status;
    }

    if (auto status = add(syscall::Number::brk, "brk", [](void *context, const syscall::Request &request) {
            return pointer_response(static_cast<posix::PosixService *>(context)->brk(request.args[0]));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::mmap, "mmap", [](void *context, const syscall::Request &request) {
            return pointer_response(static_cast<posix::PosixService *>(context)->mmap(
                request.args[0], static_cast<usize>(request.args[1]), static_cast<u32>(request.args[2]),
                static_cast<u32>(request.args[3]), static_cast<posix::Fd>(request.args[4]),
                static_cast<usize>(request.args[5])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::mprotect, "mprotect", [](void *context, const syscall::Request &request) {
            return status_response(static_cast<posix::PosixService *>(context)->mprotect(
                request.args[0], static_cast<usize>(request.args[1]), static_cast<u32>(request.args[2])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::munmap, "munmap", [](void *context, const syscall::Request &request) {
            return status_response(static_cast<posix::PosixService *>(context)->munmap(
                request.args[0], static_cast<usize>(request.args[1])));
        });
        !status.ok())
    {
        return status;
    }

    if (auto status = add(syscall::Number::clock_gettime, "clock_gettime",
                          [](void *context, const syscall::Request &request) {
                              if (request.args[1] == 0)
                              {
                                  return syscall::Response{.value = -1,
                                                           .status = Status::invalid_argument("clock buffer is null")};
                              }
                              *reinterpret_cast<posix::ClockTime *>(request.args[1]) =
                                  static_cast<posix::PosixService *>(context)->clock_gettime();
                              return syscall::Response{.value = 0, .status = Status::success()};
                          });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::clock_getres, "clock_getres",
                          [](void *context, const syscall::Request &request) {
                              if (request.args[1] != 0)
                              {
                                  *reinterpret_cast<posix::ClockTime *>(request.args[1]) =
                                      static_cast<posix::PosixService *>(context)->clock_getres();
                              }
                              return syscall::Response{.value = 0, .status = Status::success()};
                          });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::nanosleep, "nanosleep", [](void *context, const syscall::Request &request) {
            if (request.args[0] == 0)
            {
                return syscall::Response{.value = -1, .status = Status::invalid_argument("nanosleep request is null")};
            }
            const auto requested = *reinterpret_cast<const posix::ClockTime *>(request.args[0]);
            auto *remaining = reinterpret_cast<posix::ClockTime *>(request.args[1]);
            return status_response(static_cast<posix::PosixService *>(context)->nanosleep(requested, remaining));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::clock_nanosleep, "clock_nanosleep",
                          [](void *context, const syscall::Request &request) {
                              if (request.args[2] == 0)
                              {
                                  return syscall::Response{
                                      .value = -1, .status = Status::invalid_argument("clock_nanosleep request is null")};
                              }
                              const auto requested = *reinterpret_cast<const posix::ClockTime *>(request.args[2]);
                              auto *remaining = reinterpret_cast<posix::ClockTime *>(request.args[3]);
                              return status_response(
                                  static_cast<posix::PosixService *>(context)->nanosleep(requested, remaining));
                          });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::time, "time", [](void *context, const syscall::Request &request) {
            const auto now = static_cast<posix::PosixService *>(context)->clock_gettime().seconds;
            if (request.args[0] != 0)
            {
                *reinterpret_cast<i64 *>(request.args[0]) = now;
            }
            return syscall::Response{.value = now, .status = Status::success()};
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::gettimeofday, "gettimeofday",
                          [](void *context, const syscall::Request &request) {
                              struct TimeVal
                              {
                                  i64 seconds;
                                  i64 microseconds;
                              };
                              if (request.args[0] != 0)
                              {
                                  const auto now = static_cast<posix::PosixService *>(context)->clock_gettime();
                                  *reinterpret_cast<TimeVal *>(request.args[0]) =
                                      TimeVal{.seconds = now.seconds, .microseconds = now.nanoseconds / 1000};
                              }
                              return syscall::Response{.value = 0, .status = Status::success()};
                          });
        !status.ok())
    {
        return status;
    }

    if (auto status = add(syscall::Number::uname, "uname", [](void *context, const syscall::Request &request) {
            if (request.args[0] == 0)
            {
                return syscall::Response{.value = -1, .status = Status::invalid_argument("uname buffer is null")};
            }
            *reinterpret_cast<posix::UnameInfo *>(request.args[0]) =
                static_cast<posix::PosixService *>(context)->uname();
            return syscall::Response{.value = 0, .status = Status::success()};
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::getrlimit, "getrlimit", [](void *context, const syscall::Request &request) {
            if (request.args[1] == 0)
            {
                return syscall::Response{.value = -1, .status = Status::invalid_argument("rlimit buffer is null")};
            }
            *reinterpret_cast<posix::ResourceLimit *>(request.args[1]) =
                static_cast<posix::PosixService *>(context)->resource_limit(static_cast<u32>(request.args[0]));
            return syscall::Response{.value = 0, .status = Status::success()};
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::prlimit64, "prlimit64", [](void *context, const syscall::Request &request) {
            if (request.args[2] != 0)
            {
                return syscall::Response{.value = -1, .status = Status::unsupported("setting rlimit is not supported")};
            }
            if (request.args[3] != 0)
            {
                *reinterpret_cast<posix::ResourceLimit *>(request.args[3]) =
                    static_cast<posix::PosixService *>(context)->resource_limit(static_cast<u32>(request.args[1]));
            }
            return syscall::Response{.value = 0, .status = Status::success()};
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::sysinfo, "sysinfo", [](void *context, const syscall::Request &request) {
            if (request.args[0] == 0)
            {
                return syscall::Response{.value = -1, .status = Status::invalid_argument("sysinfo buffer is null")};
            }
            *reinterpret_cast<posix::SystemInfo *>(request.args[0]) =
                static_cast<posix::PosixService *>(context)->system_info();
            return syscall::Response{.value = 0, .status = Status::success()};
        });
        !status.ok())
    {
        return status;
    }

    if (auto status = add(syscall::Number::arch_prctl, "arch_prctl", [](void *context, const syscall::Request &request) {
            return signed_response(static_cast<posix::PosixService *>(context)->arch_prctl(
                static_cast<u32>(request.args[0]), request.args[1]));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::futex, "futex", [](void *context, const syscall::Request &request) {
            return signed_response(static_cast<posix::PosixService *>(context)->futex(
                request.args[0], static_cast<u32>(request.args[1]), static_cast<u32>(request.args[2])));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::getrandom, "getrandom", [](void *context, const syscall::Request &request) {
            return size_response(static_cast<posix::PosixService *>(context)->getrandom(
                std::span<std::byte>{reinterpret_cast<std::byte *>(request.args[0]), static_cast<usize>(request.args[1])}));
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::set_tid_address, "set_tid_address",
                          [](void *context, const syscall::Request &request) {
                              auto *service = static_cast<posix::PosixService *>(context);
                              auto status = service->set_tid_address(request.args[0]);
                              return syscall::Response{.value = status.ok() ? static_cast<i64>(service->getpid()) : -1,
                                                       .status = status};
                          });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::set_robust_list, "set_robust_list",
                          [](void *context, const syscall::Request &request) {
                              return status_response(static_cast<posix::PosixService *>(context)->set_robust_list(
                                  request.args[0], static_cast<usize>(request.args[1])));
                          });
        !status.ok())
    {
        return status;
    }
    if (auto status = add(syscall::Number::rseq, "rseq", [](void *context, const syscall::Request &request) {
            return status_response(static_cast<posix::PosixService *>(context)->rseq(
                request.args[0], static_cast<u32>(request.args[1]), static_cast<i32>(request.args[2]),
                static_cast<u32>(request.args[3])));
        });
        !status.ok())
    {
        return status;
    }

    for (auto number : {syscall::Number::rt_sigaction, syscall::Number::rt_sigprocmask})
    {
        if (auto status = add(number, "signal-noop", [](void *context, const syscall::Request &) {
                return status_response(static_cast<posix::PosixService *>(context)->signal_noop());
            });
            !status.ok())
        {
            return status;
        }
    }
    if (auto status = add(syscall::Number::sched_yield, "sched_yield", [](void *context, const syscall::Request &) {
            return status_response(static_cast<posix::PosixService *>(context)->sched_yield());
        });
        !status.ok())
    {
        return status;
    }
    for (auto number : {syscall::Number::exit, syscall::Number::exit_group})
    {
        if (auto status = add(number, "exit", [](void *context, const syscall::Request &request) {
                return status_response(static_cast<posix::PosixService *>(context)->exit(static_cast<i32>(request.args[0])));
            });
            !status.ok())
        {
            return status;
        }
    }
    if (auto status = add(syscall::Number::umask, "umask", [](void *context, const syscall::Request &request) {
            return syscall::Response{
                .value = static_cast<i64>(static_cast<posix::PosixService *>(context)->umask(static_cast<u32>(request.args[0]))),
                .status = Status::success()};
        });
        !status.ok())
    {
        return status;
    }

    for (auto number : {syscall::Number::clone, syscall::Number::fork, syscall::Number::execve, syscall::Number::wait4,
                        syscall::Number::kill, syscall::Number::pipe, syscall::Number::select, syscall::Number::poll})
    {
        if (auto status = add(number, "unsupported-process-ipc", [](void *, const syscall::Request &) {
                return unsupported("process or IPC syscall is not implemented in the single-process profile");
            });
            !status.ok())
        {
            return status;
        }
    }

    return syscalls_.register_callback(
        syscall::Number::ok_debug, "ok_debug", nullptr, [](void *, const syscall::Request &request) {
            return syscall::Response{.value = static_cast<i64>(request.args[0]), .status = Status::success()};
        });
}

} // namespace ok
