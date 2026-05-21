#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/syscall/syscall.hpp"

#include <span>

namespace ok::syscall
{

inline constexpr i64 linux_EPERM = 1;
inline constexpr i64 linux_ENOENT = 2;
inline constexpr i64 linux_EIO = 5;
inline constexpr i64 linux_ENOMEM = 12;
inline constexpr i64 linux_EACCES = 13;
inline constexpr i64 linux_EFAULT = 14;
inline constexpr i64 linux_EEXIST = 17;
inline constexpr i64 linux_EINVAL = 22;
inline constexpr i64 linux_ENOSYS = 38;
inline constexpr i64 linux_EAGAIN = 11;
inline constexpr i64 linux_EOVERFLOW = 75;

struct LinuxSyscallFrame
{
    u64 syscall_number{0};
    u64 rdi{0};
    u64 rsi{0};
    u64 rdx{0};
    u64 r10{0};
    u64 r8{0};
    u64 r9{0};
    i64 return_value{0};
};

class LinuxSyscallAbi final
{
  public:
    [[nodiscard]] Request decode_x86_64(const LinuxSyscallFrame &frame, sched::ProcessId caller) const;
    void encode_return(LinuxSyscallFrame &frame, i64 value) const
    {
        frame.return_value = value;
    }
};

class ErrnoMapper final
{
  public:
    [[nodiscard]] static i64 errno_for(Status status);
    [[nodiscard]] static i64 result_for(Response response);
};

class LinuxSyscallDispatcher final
{
  public:
    explicit LinuxSyscallDispatcher(Table *table = nullptr) : table_(table)
    {
    }
    void attach(Table &table)
    {
        table_ = &table;
    }
    [[nodiscard]] i64 dispatch_x86_64(LinuxSyscallFrame &frame, sched::ProcessId caller) const;

  private:
    Table *table_{nullptr};
    LinuxSyscallAbi abi_{};
};

struct LinuxAuxvEntry
{
    u64 type{0};
    u64 value{0};
};

class LinuxAuxvBuilder final
{
  public:
    Status add(u64 type, u64 value);
    [[nodiscard]] std::span<const LinuxAuxvEntry> entries() const
    {
        return {entries_.begin(), entries_.size()};
    }

  private:
    StaticVector<LinuxAuxvEntry, 16> entries_;
};

class LinuxCompatProcess final
{
  public:
    void set_tls_base(uptr address)
    {
        tls_base_ = address;
    }
    [[nodiscard]] uptr tls_base() const
    {
        return tls_base_;
    }
    [[nodiscard]] LinuxAuxvBuilder &auxv()
    {
        return auxv_;
    }

  private:
    uptr tls_base_{0};
    LinuxAuxvBuilder auxv_;
};

class LinuxVdsoPlaceholder final
{
  public:
    [[nodiscard]] uptr base() const
    {
        return 0;
    }
    [[nodiscard]] bool enabled() const
    {
        return false;
    }
};

} // namespace ok::syscall
