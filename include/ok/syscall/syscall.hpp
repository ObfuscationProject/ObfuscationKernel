#pragma once

#include "ok/core/types.hpp"
#include "ok/sched/scheduler.hpp"

#include <array>
#include <concepts>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace ok::syscall {

enum class Number : u64 {
    read = 0,
    write = 1,
    open = 2,
    close = 3,
    stat = 4,
    mmap = 9,
    mprotect = 10,
    munmap = 11,
    brk = 12,
    ioctl = 16,
    pread64 = 17,
    pwrite64 = 18,
    readv = 19,
    writev = 20,
    sched_yield = 24,
    nanosleep = 35,
    getpid = 39,
    clone = 56,
    fork = 57,
    execve = 59,
    exit = 60,
    wait4 = 61,
    kill = 62,
    uname = 63,
    getcwd = 79,
    chdir = 80,
    mkdir = 83,
    unlink = 87,
    clock_gettime = 228,
    ok_debug = 1024,
};

struct Request {
    Number number {Number::ok_debug};
    sched::ProcessId caller {0};
    std::array<u64, 6> args {};
};

struct Response {
    i64 value {0};
    Status status {Status::success()};
};

class Handler {
public:
    virtual ~Handler() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual Response invoke(const Request& request) = 0;
};

template <typename F>
concept SyscallCallable = requires(F function, const Request& request) {
    { function(request) } -> std::same_as<Response>;
};

template <SyscallCallable F>
class FunctionHandler final : public Handler {
public:
    FunctionHandler(std::string_view handler_name, F function) : name_(handler_name), function_(std::move(function)) {}

    [[nodiscard]] std::string_view name() const override { return name_; }
    Response invoke(const Request& request) override { return function_(request); }

private:
    std::string_view name_;
    F function_;
};

class Table final {
public:
    template <SyscallCallable F>
    Status register_function(Number number, std::string_view name, F function)
    {
        return register_handler(number, std::make_unique<FunctionHandler<F>>(name, std::move(function)));
    }

    Status register_handler(Number number, std::unique_ptr<Handler> handler);
    Response dispatch(const Request& request);
    [[nodiscard]] bool has_handler(Number number) const;
    [[nodiscard]] usize handler_count() const { return handlers_.size(); }

private:
    std::unordered_map<u64, std::unique_ptr<Handler>> handlers_;
};

} // namespace ok::syscall

