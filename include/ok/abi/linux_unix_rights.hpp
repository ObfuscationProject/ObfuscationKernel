#pragma once
#include "ok/abi/linux_unix_rights_data.hpp"
#include "ok/sched/resources.hpp"

namespace ok::abi::linux_user
{
struct UnixMessageHeader
{
    u64 name{}, name_length{}, vectors{}, vector_count{}, control{}, control_length{};
};
struct UnixMessageVector { u64 address{}, length{}; };
struct UnixRightsList
{
    usize count{};
    std::array<i32, sched::max_unix_rights> descriptors{};
};
template <Profile Selected, bool Native = false> requires SupportedProfile<Selected>
class UnixMessageCodec final
{
  public:
    static constexpr auto profile = Codec<Selected>::profile;
    static constexpr usize word_bytes = profile.word_bytes;
    static constexpr usize header_bytes = word_bytes == 8 ? 56 : 28;
    static constexpr usize cmsg_header_bytes = word_bytes + 8;
    static constexpr usize max_control_bytes = cmsg_header_bytes + sched::max_unix_rights * 4;
    static constexpr usize max_vectors = 8;
    static constexpr u32 socket_level = !Native && (Selected == Profile::mips || Selected == Profile::mips64) ? 65535u : 1u;
    static constexpr u32 rights_type = 1u, peek = 2u, control_truncated = 8u,
        dontwait = 0x40u, nosignal = 0x4000u, cmsg_cloexec = 0x40000000u;
    static constexpr auto numbers = unix_message_numbers[static_cast<usize>(Selected)];
    static constexpr usize name_length_offset = word_bytes;
    static constexpr usize vectors_offset = word_bytes == 8 ? 16 : 8;
    static constexpr usize vector_count_offset = vectors_offset + word_bytes;
    static constexpr usize control_offset = vectors_offset + word_bytes * 2;
    static constexpr usize control_length_offset = control_offset + word_bytes;
    static constexpr usize flags_offset = control_length_offset + word_bytes;
    static constexpr u64 maximum_address = word_bytes == 8 ? ~u64{0} : 0xffffffffu;
    static bool range(u64 address, u64 length)
    { return address <= maximum_address && length <= maximum_address && (!length || (address && length - 1 <= maximum_address - address)); }
    static u64 load(std::span<const u8> bytes)
    {
        u64 value{};
        for (usize index = 0; index < bytes.size(); ++index)
            value |= u64{bytes[index]} << (8 * (profile.big_endian ? bytes.size() - 1 - index : index));
        return value;
    }
    static void store(std::span<u8> bytes, u64 value)
    {
        for (usize index = 0; index < bytes.size(); ++index)
            bytes[index] = static_cast<u8>(value >> (8 * (profile.big_endian ? bytes.size() - 1 - index : index)));
    }
    static Result<UnixMessageHeader> decode_header(std::span<const u8> bytes)
    {
        if (bytes.size() != header_bytes) return Status::invalid_argument("UNIX message header size mismatch");
        UnixMessageHeader header{load(bytes.subspan(0, word_bytes)), load(bytes.subspan(name_length_offset, 4)),
            load(bytes.subspan(vectors_offset, word_bytes)), load(bytes.subspan(vector_count_offset, word_bytes)),
            load(bytes.subspan(control_offset, word_bytes)), load(bytes.subspan(control_length_offset, word_bytes))};
        if (header.name || header.name_length) return Status::unsupported("UNIX socketpair messages have no destination name");
        if (header.vector_count > max_vectors) return Status::message_size("UNIX message vector bound exceeded");
        if (!range(header.vectors, header.vector_count * word_bytes * 2) || !range(header.control, header.control_length))
            return Status::fault("UNIX message address range wraps");
        return header;
    }
    static Result<UnixMessageVector> decode_vector(std::span<const u8> bytes)
    {
        if (bytes.size() != word_bytes * 2) return Status::invalid_argument("UNIX vector size mismatch");
        UnixMessageVector vector{load(bytes.first(word_bytes)), load(bytes.subspan(word_bytes, word_bytes))};
        if (!range(vector.address, vector.length)) return Status::fault("UNIX vector address range wraps");
        return vector;
    }
    static Result<UnixRightsList> decode_rights(std::span<const u8> bytes)
    {
        UnixRightsList result{};
        if (bytes.empty()) return result;
        if (bytes.size() < cmsg_header_bytes || bytes.size() > max_control_bytes)
            return Status::invalid_argument("UNIX control storage size is unsupported");
        const auto length = load(bytes.first(word_bytes));
        if (length < cmsg_header_bytes + 4 || length > bytes.size() || (length - cmsg_header_bytes) % 4)
            return Status::invalid_argument("UNIX rights control length is malformed");
        const auto aligned = (length + word_bytes - 1) & ~(u64{word_bytes} - 1);
        if (bytes.size() > aligned || load(bytes.subspan(word_bytes, 4)) != socket_level ||
            load(bytes.subspan(word_bytes + 4, 4)) != rights_type)
            return Status::unsupported("only one bounded SCM_RIGHTS control is supported");
        result.count = static_cast<usize>((length - cmsg_header_bytes) / 4);
        if (result.count > sched::max_unix_rights) return Status::message_size("UNIX rights bound exceeded");
        for (usize index = 0; index < result.count; ++index)
            result.descriptors[index] = static_cast<i32>(static_cast<u32>(load(bytes.subspan(cmsg_header_bytes + index * 4, 4))));
        return result;
    }
    static usize descriptor_capacity(u64 storage)
    {
        if (storage < cmsg_header_bytes + 4) return 0;
        const auto capacity = (storage - cmsg_header_bytes) / 4;
        return static_cast<usize>(capacity < sched::max_unix_rights ? capacity : sched::max_unix_rights);
    }
    static Result<usize> encode_rights(std::span<u8> bytes, std::span<const i32> descriptors, u64 storage)
    {
        if (descriptors.empty()) return usize{0};
        if (descriptors.size() > sched::max_unix_rights || descriptor_capacity(storage) < descriptors.size() ||
            bytes.size() < max_control_bytes || storage > maximum_address)
            return Status::invalid_argument("UNIX control output capacity is invalid");
        const auto length = cmsg_header_bytes + descriptors.size() * 4;
        const auto aligned = (length + word_bytes - 1) & ~(word_bytes - 1);
        const auto count = static_cast<usize>(storage < aligned ? storage : aligned);
        for (usize index = 0; index < count; ++index) bytes[index] = 0;
        store(bytes.first(word_bytes), length);
        store(bytes.subspan(word_bytes, 4), socket_level);
        store(bytes.subspan(word_bytes + 4, 4), rights_type);
        for (usize index = 0; index < descriptors.size(); ++index)
            store(bytes.subspan(cmsg_header_bytes + index * 4, 4), static_cast<u32>(descriptors[index]));
        return count;
    }
    static void encode_result(std::span<u8> header, usize control_length, bool truncated, bool close_on_exec)
    {
        store(header.subspan(name_length_offset, 4), 0);
        store(header.subspan(control_length_offset, word_bytes), control_length);
        store(header.subspan(flags_offset, 4), (truncated ? control_truncated : 0) |
            (close_on_exec ? cmsg_cloexec : 0));
    }
};
} // namespace ok::abi::linux_user
