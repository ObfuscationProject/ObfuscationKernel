#pragma once
#include "ok/abi/linux_unix_rights.hpp"
#include "ok/posix/posix.hpp"
#include "ok/syscall/linux.hpp"

namespace ok::user::native
{
// Immediate bounded message operation. All user-copy happens while a move-only
// receive owns the stream preview and exact descriptor reservations.
template <abi::linux_user::Profile Profile, bool Native = false, typename CopyIn, typename CopyOut, typename Writable>
i64 unix_message(posix::PosixService &posix, bool receive, u64 fd, u64 address, u64 flags,
    CopyIn &&copy_in, CopyOut &&copy_out, Writable &&writable)
{
    using C = abi::linux_user::UnixMessageCodec<Profile, Native>;
    auto error = [](Status status) -> i64 { return -syscall::ErrnoMapper::errno_for(status); };
    if (fd > static_cast<u64>(__INT32_MAX__)) return -9;
    const u64 allowed = receive ? C::dontwait | C::peek | C::cmsg_cloexec : C::dontwait | C::nosignal;
    if (flags & ~allowed) return -95;
    if (!C::range(address, C::header_bytes)) return -14;
    std::array<u8, C::header_bytes> header_bytes{};
    if (auto status = copy_in(address, std::span<u8>{header_bytes}); !status) return error(status);
    auto decoded = C::decode_header(header_bytes);
    if (!decoded) return error(decoded.status());
    const auto header = decoded.value();
    std::array<abi::linux_user::UnixMessageVector, C::max_vectors> vectors{};
    std::array<u8, C::max_vectors * C::word_bytes * 2> vector_bytes{};
    const auto vector_length = static_cast<usize>(header.vector_count * C::word_bytes * 2);
    if (vector_length)
        if (auto status = copy_in(header.vectors, std::span<u8>{vector_bytes.data(), vector_length}); !status) return error(status);
    usize payload_length{};
    for (usize index = 0; index < header.vector_count; ++index)
    {
        auto vector = C::decode_vector(std::span<const u8>{vector_bytes}.subspan(index * C::word_bytes * 2, C::word_bytes * 2));
        if (!vector) return error(vector.status());
        vectors[index] = vector.value();
        const auto room = sched::pipe_capacity - payload_length;
        payload_length += static_cast<usize>(vector.value().length < room ? vector.value().length : room);
    }
    std::array<std::byte, sched::pipe_capacity> payload{};
    std::array<u8, C::max_control_bytes> control{};
    if (!receive)
    {
        if (header.control_length > control.size()) return -90;
        if (header.control_length)
            if (auto status = copy_in(header.control, std::span<u8>{control.data(), static_cast<usize>(header.control_length)}); !status)
                return error(status);
        auto rights = C::decode_rights({control.data(), static_cast<usize>(header.control_length)});
        if (!rights) return error(rights.status());
        usize copied{};
        for (usize index = 0; index < header.vector_count && copied < payload_length; ++index)
        {
            const auto remaining = payload_length - copied;
            const auto count = static_cast<usize>(vectors[index].length < remaining ? vectors[index].length : remaining);
            if (count)
                if (auto status = copy_in(vectors[index].address,
                    std::span<u8>{reinterpret_cast<u8 *>(payload.data() + copied), count}); !status) return error(status);
            copied += count;
        }
        const auto result = posix.send_unix_rights(static_cast<i32>(fd), {payload.data(), payload_length},
            {rights.value().descriptors.data(), rights.value().count}, (flags & C::dontwait) != 0);
        return result ? static_cast<i64>(result.value()) : error(result.status());
    }
    if (!writable(address, C::header_bytes)) return -14;
    auto prepared = posix.prepare_unix_receive(static_cast<i32>(fd), {payload.data(), payload_length},
        C::descriptor_capacity(header.control_length), (flags & C::peek) != 0, (flags & C::cmsg_cloexec) != 0,
        (flags & C::dontwait) != 0);
    if (!prepared) return error(prepared.status());
    auto &operation = prepared.value();
    auto encoded = C::encode_rights(control, operation.descriptors(), header.control_length);
    if (!encoded) return error(encoded.status());
    // Unused trailing iovecs and absent ancillary output need no writable
    // mapping. A refusal here cancels every temporary hold and reservation.
    if (encoded.value() && !writable(header.control, encoded.value())) return -14;
    usize checked{};
    for (usize index = 0; index < header.vector_count && checked < operation.bytes(); ++index)
    {
        const auto remaining = operation.bytes() - checked;
        const auto count = static_cast<usize>(vectors[index].length < remaining ? vectors[index].length : remaining);
        if (count && !writable(vectors[index].address, count)) return -14;
        checked += count;
    }
    usize copied{};
    for (usize index = 0; index < header.vector_count && copied < operation.bytes(); ++index)
    {
        const auto remaining = operation.bytes() - copied;
        const auto count = static_cast<usize>(vectors[index].length < remaining ? vectors[index].length : remaining);
        if (count)
            if (auto status = copy_out(vectors[index].address,
                std::span<const u8>{reinterpret_cast<const u8 *>(payload.data() + copied), count}); !status) return error(status);
        copied += count;
    }
    if (encoded.value())
        if (auto status = copy_out(header.control, std::span<const u8>{control.data(), encoded.value()}); !status) return error(status);
    C::encode_result(header_bytes, encoded.value(), operation.control_truncated(), (flags & C::cmsg_cloexec) != 0);
    if (auto status = copy_out(address, std::span<const u8>{header_bytes}); !status) return error(status);
    const auto result = operation.commit();
    return result ? static_cast<i64>(result.value()) : error(result.status());
}
} // namespace ok::user::native
