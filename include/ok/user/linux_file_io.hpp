#pragma once

#include "ok/abi/linux_native.hpp"
#include "ok/abi/linux_seek.hpp"
#include "ok/posix/posix.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/syscall/linux.hpp"
#include "ok/time/public_clock.hpp"

#include <array>
#include <concepts>

// Regular-file I/O and clock records of the Linux personality, shared by the
// kernel-lock syscall path and the kernel sections that run the same calls
// without the lock (src/user/runtime/section.hpp). Both paths call these
// helpers, so their results are equal by construction. A helper reaches
// kernel state only through its Access: the caller's PosixService, the
// caller's address-space snapshot and the caller's descriptors.
namespace ok::user::linux_file_io
{
namespace lu = abi::linux_user;
namespace hw = arch::execution;

template <typename A>
concept Access = requires(A &access, const A &view, i32 fd) {
    { access.posix() } -> std::same_as<posix::PosixService &>;
    { view.space() } -> std::same_as<const hw::AddressSpace &>;
    { view.description(fd) } -> std::same_as<const sched::OpenFileDescription *>;
};

// The access of a caller process: its descriptors resolve in the scheduler's
// description table (a lookup that touches no scheduler table lock).
class ProcessAccess final
{
  public:
    ProcessAccess(posix::PosixService &posix, const sched::SchedulerState &scheduler,
                  const sched::ProcessResources &resources)
        : posix_(posix), scheduler_(scheduler), resources_(resources)
    {
    }
    [[nodiscard]] posix::PosixService &posix() { return posix_; }
    [[nodiscard]] const hw::AddressSpace &space() const { return resources_.native_space; }
    [[nodiscard]] const sched::OpenFileDescription *description(i32 fd) const
    {
        const auto *slot = resources_.files.entry(fd);
        return slot && slot->used ? scheduler_.open_description(slot->description) : nullptr;
    }

  private:
    posix::PosixService &posix_;
    const sched::SchedulerState &scheduler_;
    const sched::ProcessResources &resources_;
};

[[nodiscard]] inline i64 failure(const Status &status)
{
    return -syscall::ErrnoMapper::errno_for(status);
}

// read(2) and write(2) of a node-backed description, as the native generic
// marshalling does them: one staging buffer of pipe_capacity bytes, the
// syscall table handlers' null-buffer check, and for a read further chunks
// until one comes back short. A write takes at most pipe_capacity bytes in
// one transfer, under PosixService::write's position hold. A read holds the
// description's position lock across every chunk and the copy-outs between
// them, so it returns one contiguous range of the file while other CPUs read
// the same description (Linux holds f_pos_lock the same way). The copy-outs
// resolve no copy-on-write page that waits for a section: a section's output
// range was resolved at admission, and the kernel-lock caller's process was
// drained before the call.
// `would_block` reports a provider that asked to wait (the caller decides
// whether its thread blocked).
template <Access A>
i64 regular_rw(A &access, bool writing, i32 fd, uptr address, usize requested, bool &would_block)
{
    would_block = false;
    const auto &space = access.space();
    alignas(16) std::array<std::byte, sched::pipe_capacity + 1> buffer{};
    usize size = requested < sched::pipe_capacity ? requested : sched::pipe_capacity;
    auto *const position_lock = writing ? nullptr : access.posix().node_position_lock(fd);
    auto transfer = [&](usize count) -> Result<usize> {
        if (buffer.data() == nullptr && count != 0)
            return Status::fault(writing ? "write buffer is null" : "read buffer is null");
        if (writing) return access.posix().write(fd, std::span<const std::byte>{buffer.data(), count});
        if (position_lock) return access.posix().read_held(fd, std::span<std::byte>{buffer.data(), count});
        return access.posix().read(fd, std::span<std::byte>{buffer.data(), count});
    };
    if (writing)
    {
        if (!hw::copy_from_user(space, buffer.data(), address, size))
            return -14;
    }
    else if (!hw::user_range(space, address, size, true))
        return -14;
    const sched::DescriptionPositionGuard position(position_lock);
    auto result = transfer(size);
    if (!result)
    {
        would_block = result.status().code() == StatusCode::would_block;
        return failure(result.status());
    }
    if (writing)
        return static_cast<i64>(result.value());
    size = result.value();
    if (size > buffer.size() || !hw::copy_to_user(space, address, buffer.data(), size))
        return -14;
    if (size != sched::pipe_capacity)
        return static_cast<i64>(size);
    usize total = size;
    while (total < requested)
    {
        const usize wanted = requested - total < sched::pipe_capacity ? requested - total : sched::pipe_capacity;
        auto more = transfer(wanted);
        if (!more || more.value() == 0) break;
        const auto chunk = more.value();
        if (chunk > wanted || !hw::copy_to_user(space, address + total, buffer.data(), chunk)) break;
        total += chunk;
        if (chunk < wanted) break;
    }
    return static_cast<i64>(total);
}

// pread64 and pwrite64 (linux_bootstrap::dispatch): the 64-bit offset, the
// descriptor and seekability checks, then one positioned transfer of at most
// pipe_capacity bytes; a positioned write honours O_APPEND.
template <lu::Profile Profile, Access A>
i64 linux_positioned_io(A &access, bool writing, const lu::Call &call)
{
    using Codec = lu::Codec<Profile>;
    const auto &a = call.arguments;
    const auto &space = access.space();
    constexpr usize first = Codec::profile.aligned_64bit_pairs ? 4 : 3;
    const auto offset = Codec::decode_argument64(call, first);
    if (!offset || static_cast<i64>(offset.value()) < 0) return -22;
    const auto fd = static_cast<i32>(static_cast<u32>(a[0]));
    const auto *description = access.description(fd);
    if (!description || !description->used) return -9;
    if ((!description->node && !description->native_shared_file) || description->pipe != sched::invalid_description ||
        description->console || description->input_console) return -29;
    if (writing ? !description->writable : !description->readable) return -9;
    if (a[2] > (Codec::word_mask >> 1)) return -22;
    constexpr u64 limit = Codec::word_mask >> 1;
    if (offset.value() > limit) return -75;
    const auto size = a[2] < sched::pipe_capacity ? static_cast<usize>(a[2]) : sched::pipe_capacity;
    // Bound the admitted endpoint even when hosted models have wider usize.
    if (size > limit - offset.value()) return -75;
    std::array<std::byte, sched::pipe_capacity> buffer{};
    if (writing)
    {
        if (!hw::copy_from_user(space, buffer.data(), static_cast<uptr>(a[1]), size)) return -14;
        auto result = access.posix().template pwrite_with_policy<posix::PositionedWritePolicy::linux_append>(
            fd, {buffer.data(), size}, static_cast<usize>(offset.value()), limit);
        return result ? static_cast<i64>(result.value()) : failure(result.status());
    }
    if (!hw::user_range(space, static_cast<uptr>(a[1]), size, true)) return -14;
    auto result = access.posix().pread(fd, {buffer.data(), size}, static_cast<usize>(offset.value()));
    if (!result) return failure(result.status());
    if (!hw::copy_to_user(space, static_cast<uptr>(a[1]), buffer.data(), result.value())) return -14;
    return static_cast<i64>(result.value());
}

// lseek and _llseek (linux_bootstrap::dispatch): Linux checks the descriptor,
// then SEEK_MAX, then whether the endpoint seeks; SEEK_DATA/SEEK_HOLE are
// refused. _llseek copies the new position out after committing it.
template <lu::Profile Profile, Access A>
i64 linux_lseek(A &access, bool wide, const lu::Call &call)
{
    using Codec = lu::Codec<Profile>;
    using Seek = lu::SeekCodec<Profile>;
    const auto &a = call.arguments;
    const auto fd = static_cast<i32>(static_cast<u32>(a[0]));
    const auto whence = static_cast<u32>(a[wide ? 4 : 2]);
    const auto *description = access.description(fd);
    if (!description) return -9;
    // Linux checks fd, then SEEK_MAX, then the endpoint's seek capability.
    if (whence > 4) return -22;
    if (description->console || description->input_console ||
        (!description->node && !description->native_shared_file) ||
        description->pipe != sched::invalid_description) return -29;
    // SEEK_DATA/HOLE require filesystem extent semantics outside this slice.
    if (whence > 2) return -22;
    if (whence == 2 && description->node)
    {
        auto metadata = description->node->query_metadata();
        if (!metadata) return failure(metadata.status());
        // Directory cookies have no byte-size-relative end in this profile.
        if (metadata.value().type == fs::NodeType::directory) return -22;
    }
    const i64 offset = wide ? Seek::wide_offset(a[1], a[2]) : Seek::word_offset(a[1]);
    auto result = access.posix().seek(fd, offset, static_cast<posix::SeekWhence>(whence));
    if (!result) return failure(result.status());
    if (!wide)
    {
        // Raw lseek returns native-word off_t. Linux applies this conversion
        // after vfs_llseek; the common service may impose a smaller capacity.
        if constexpr (Codec::profile.word_bytes == 4)
            if (result.value() > 0x7fffffffu) return -75;
        return static_cast<i64>(result.value());
    }
    // Linux commits the shared position before copying the result. EFAULT
    // must retain that position; do not preflight or undo the successful seek.
    const auto bytes = Seek::encode(result.value());
    return hw::copy_to_user(access.space(), static_cast<uptr>(a[3]), bytes.data(), bytes.size()) ? 0 : -14;
}

// fsync and fdatasync: the descriptor's provider synchronizes; both calls
// map its status alike.
template <Access A>
i64 file_sync(A &access, i32 fd)
{
    const auto status = access.posix().fsync(fd);
    return status ? 0 : failure(status);
}

// clock_gettime and clock_getres in all their time32/time64 variants: clock
// identities 0 (realtime) and 1 (monotonic); a NULL getres record validates
// the identity only.
template <lu::Profile Profile>
i64 linux_clock_record(const hw::AddressSpace &space, const lu::Call &call, bool resolution, bool time64)
{
    using Codec = lu::Codec<Profile>;
    const auto &a = call.arguments;
    const auto identity = static_cast<u32>(a[0]);
    if (identity > 1)
        return -22;
    if (resolution && a[1] == 0) return 0;
    // NULL getres validates identity only; output requires a live provider.
    auto now = time::public_clock(identity, resolution);
    if (!now) return -5;
    const auto format = time64 ? lu::TimeFormat::kernel_timespec : lu::TimeFormat::legacy_timespec;
    std::array<u8, 16> record{};
    const usize size = Codec::timespec_size(format);
    auto status = Codec::encode_timespec(
        {record.data(), size}, format,
        {now.value().seconds, now.value().nanoseconds});
    if (!status) return -75;
    return hw::copy_to_user(space, static_cast<uptr>(a[1]), record.data(), size) ? 0 : -14;
}

} // namespace ok::user::linux_file_io
