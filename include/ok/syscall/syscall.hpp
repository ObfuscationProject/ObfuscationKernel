#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/sched/scheduler.hpp"

#include <array>
#include <concepts>
#include <string_view>

namespace ok::syscall
{

enum class Number : u64
{
    read = 0,
    write = 1,
    open = 2,
    close = 3,
    stat = 4,
    fstat = 5,
    lstat = 6,
    poll = 7,
    lseek = 8,
    mmap = 9,
    mprotect = 10,
    munmap = 11,
    brk = 12,
    rt_sigaction = 13,
    rt_sigprocmask = 14,
    ioctl = 16,
    pread64 = 17,
    pwrite64 = 18,
    readv = 19,
    writev = 20,
    access = 21,
    pipe = 22,
    select = 23,
    sched_yield = 24,
    dup = 32,
    dup2 = 33,
    nanosleep = 35,
    getpid = 39,
    clone = 56,
    fork = 57,
    execve = 59,
    exit = 60,
    wait4 = 61,
    kill = 62,
    uname = 63,
    fcntl = 72,
    getdents = 78,
    getcwd = 79,
    chdir = 80,
    fchdir = 81,
    mkdir = 83,
    rmdir = 84,
    creat = 85,
    unlink = 87,
    umask = 95,
    gettimeofday = 96,
    getrlimit = 97,
    sysinfo = 99,
    getuid = 102,
    getgid = 104,
    geteuid = 107,
    getegid = 108,
    getppid = 110,
    arch_prctl = 158,
    gettid = 186,
    time = 201,
    futex = 202,
    getdents64 = 217,
    set_tid_address = 218,
    clock_gettime = 228,
    clock_getres = 229,
    clock_nanosleep = 230,
    exit_group = 231,
    openat = 257,
    mkdirat = 258,
    newfstatat = 262,
    unlinkat = 263,
    faccessat = 269,
    set_robust_list = 273,
    dup3 = 292,
    prlimit64 = 302,
    getrandom = 318,
    rseq = 334,
    close_range = 436,
    faccessat2 = 439,
    ok_debug = 1024,
};

enum class DispatchMode : u8
{
    trap,
    fast_path,
    vdso_assisted,
};

struct Request
{
    Number number{Number::ok_debug};
    sched::ProcessId caller{0};
    std::array<u64, 6> args{};
};

struct Response
{
    i64 value{0};
    Status status{Status::success()};
};

using Callback = Response (*)(void *context, const Request &request);

inline constexpr usize direct_syscall_limit = 512;

class Handler
{
  public:
    virtual ~Handler() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual Response invoke(const Request &request) = 0;
};

template <typename F>
concept SyscallCallable = requires(F function, const Request &request) {
    { function(request) } -> std::same_as<Response>;
};

class Table final
{
  public:
    void set_mode(DispatchMode mode)
    {
        mode_ = mode;
    }
    [[nodiscard]] DispatchMode mode() const
    {
        return mode_;
    }
    Status register_handler(Number number, Handler &handler);
    Status register_callback(Number number, std::string_view name, void *context, Callback callback);
    Response dispatch(const Request &request);
    [[nodiscard]] bool has_handler(Number number) const;
    [[nodiscard]] usize handler_count() const
    {
        return handlers_.size();
    }

  private:
    struct Entry
    {
        u64 number{0};
        std::string_view name{};
        Handler *handler{nullptr};
        void *context{nullptr};
        Callback callback{nullptr};
    };

    DispatchMode mode_{DispatchMode::trap};
    StaticVector<Entry, 128> handlers_;
    std::array<Entry *, direct_syscall_limit> direct_handlers_{};
};

} // namespace ok::syscall
