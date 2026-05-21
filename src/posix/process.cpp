#include "ok/posix/posix.hpp"

namespace ok::posix
{
namespace
{

inline constexpr usize page_size = 4096;

usize align_up(usize value, usize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

ClockTime PosixService::clock_gettime() const
{
    const auto ticks = monotonic_ticks_ + (scheduler_ == nullptr ? 0 : scheduler_->process_count());
    return ClockTime{.seconds = static_cast<i64>(ticks / 1000), .nanoseconds = static_cast<i64>((ticks % 1000) * 1000000)};
}

ClockTime PosixService::clock_getres() const
{
    return ClockTime{.seconds = 0, .nanoseconds = 1000000};
}

Status PosixService::nanosleep(ClockTime requested, ClockTime *remaining)
{
    if (requested.seconds < 0 || requested.nanoseconds < 0 || requested.nanoseconds >= 1000000000)
    {
        return Status::invalid_argument("invalid nanosleep interval");
    }
    if (remaining != nullptr)
    {
        *remaining = {};
    }
    monotonic_ticks_ += static_cast<u64>(requested.seconds) * 1000u +
                        static_cast<u64>((requested.nanoseconds + 999999) / 1000000);
    return Status::success();
}

UnameInfo PosixService::uname() const
{
    UnameInfo info{};
    static_cast<void>(info.machine.assign("kernel-profile"));
    return info;
}

ResourceLimit PosixService::resource_limit(u32 resource) const
{
    switch (resource)
    {
    case 3:
        return ResourceLimit{.current = 8 * 1024 * 1024, .maximum = 8 * 1024 * 1024};
    case 7:
        return ResourceLimit{.current = max_open_files, .maximum = max_open_files};
    default:
        return ResourceLimit{.current = ~u64{0}, .maximum = ~u64{0}};
    }
}

SystemInfo PosixService::system_info() const
{
    return SystemInfo{
        .uptime = clock_gettime().seconds,
        .total_ram = 64 * 1024 * 1024,
        .free_ram = 32 * 1024 * 1024,
        .process_count = static_cast<u16>(scheduler_ == nullptr ? 0 : scheduler_->process_count()),
    };
}

Result<uptr> PosixService::brk(uptr address)
{
    if (address == 0)
    {
        return program_break_;
    }
    if (address < program_break_base_ || address > program_break_limit_)
    {
        return Status::no_memory("program break outside user heap window");
    }
    program_break_ = address;
    return program_break_;
}

Result<uptr> PosixService::mmap(uptr address, usize length, u32 protection, u32 flags, Fd fd, usize offset)
{
    if (length == 0)
    {
        return Status::invalid_argument("mmap length must be non-zero");
    }
    if ((flags & map_ANONYMOUS) == 0)
    {
        auto *entry = descriptor(fd);
        if (entry == nullptr || !entry->used)
        {
            return Status::invalid_argument("mmap file descriptor is invalid");
        }
    }
    const usize mapped_length = align_up(length, page_size);
    uptr mapped_address = address == 0 ? next_mapping_address_ : address;
    for (auto &mapping : mappings_)
    {
        if (!mapping.used)
        {
            mapping = MemoryMapping{
                .used = true,
                .address = mapped_address,
                .length = mapped_length,
                .protection = protection,
                .flags = flags,
                .fd = fd,
                .offset = offset,
            };
            if (address == 0)
            {
                next_mapping_address_ = mapped_address + mapped_length + page_size;
            }
            return mapped_address;
        }
    }
    return Status::no_memory("memory mapping table full");
}

Status PosixService::mprotect(uptr address, usize length, u32 protection)
{
    const usize mapped_length = align_up(length, page_size);
    for (auto &mapping : mappings_)
    {
        if (mapping.used && mapping.address == address && mapping.length >= mapped_length)
        {
            mapping.protection = protection;
            return Status::success();
        }
    }
    return Status::not_found("memory mapping not found");
}

Status PosixService::munmap(uptr address, usize length)
{
    const usize mapped_length = align_up(length, page_size);
    for (auto &mapping : mappings_)
    {
        if (mapping.used && mapping.address == address && mapping.length >= mapped_length)
        {
            mapping = {};
            return Status::success();
        }
    }
    return Status::not_found("memory mapping not found");
}

Result<i64> PosixService::arch_prctl(u32 code, uptr address)
{
    switch (code)
    {
    case arch_SET_FS:
        fs_base_ = address;
        return 0;
    case arch_SET_GS:
        gs_base_ = address;
        return 0;
    case arch_GET_FS:
        if (address == 0)
        {
            return Status::invalid_argument("arch_prctl output pointer is null");
        }
        *reinterpret_cast<uptr *>(address) = fs_base_;
        return 0;
    case arch_GET_GS:
        if (address == 0)
        {
            return Status::invalid_argument("arch_prctl output pointer is null");
        }
        *reinterpret_cast<uptr *>(address) = gs_base_;
        return 0;
    default:
        return Status::unsupported("arch_prctl code is not implemented");
    }
}

Result<i64> PosixService::futex(uptr user_address, u32 operation, u32)
{
    if (user_address == 0)
    {
        return Status::invalid_argument("futex address is null");
    }
    switch (operation & 0x7fu)
    {
    case futex_WAKE:
        return 0;
    case futex_WAIT:
        return Status::would_block("single-process futex wait would block");
    default:
        return Status::unsupported("futex operation is not implemented");
    }
}

Result<usize> PosixService::getrandom(std::span<std::byte> out)
{
    u64 state = 0x9e3779b97f4a7c15ull ^ monotonic_ticks_;
    for (usize i = 0; i < out.size(); ++i)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        out[i] = static_cast<std::byte>(state & 0xffu);
    }
    return out.size();
}

Status PosixService::set_tid_address(uptr address)
{
    clear_tid_address_ = address;
    return Status::success();
}

Status PosixService::set_robust_list(uptr, usize)
{
    return Status::success();
}

Status PosixService::rseq(uptr, u32, i32, u32)
{
    return Status::success();
}

Status PosixService::signal_noop()
{
    return Status::success();
}

Status PosixService::sched_yield()
{
    return Status::success();
}

Status PosixService::exit(i32 code)
{
    last_exit_code_ = code;
    return Status::success();
}

u32 PosixService::umask(u32 mask)
{
    const auto previous = file_mode_mask_;
    file_mode_mask_ = mask & 0777u;
    return previous;
}

} // namespace ok::posix
