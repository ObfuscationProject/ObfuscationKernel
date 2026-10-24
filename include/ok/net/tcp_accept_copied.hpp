#pragma once
#include <ok/net/tcp_stream_checked.hpp>
#include <ok/uapi/syscall.h>

namespace ok::net
{
template <class Original>
concept TcpAcceptOriginal = requires(const Original &original, u64 pid, i32 fd)
{
    { original.matches(pid, fd) } -> std::same_as<bool>;
    { original.flags() } -> std::same_as<u32>;
    { original.handle() } -> std::same_as<TcpStreamHandle>;
};
template <class Copy>
concept TcpAcceptCopy = TcpUserCopy<Copy> && requires(const Copy &copy)
{ { copy.live() } -> std::same_as<bool>; };

// The caller captures its original listener before this copied ABI adapter.
// The returned operation keeps all holds until captured result publication.
template <abi::linux_user::Profile Selected> requires abi::linux_user::SupportedProfile<Selected>
class TcpAcceptCopied final
{
    using Codec = abi::linux_user::Codec<Selected>;
    using Constants = abi::linux_user::UdpConstants<Selected>;
    using Wire = abi::linux_user::UdpWire<Selected>;
  public:
    explicit TcpAcceptCopied(TcpStreamOperations &operations) : operations_{operations} {}
    template <TcpAcceptCopy Copy, TcpAcceptOriginal Original>
    Result<PreparedTcpAccept> prepare(u64 pid, i32 fd, u64 address, u64 length_address,
        u32 flags, bool accept4, bool linux, Copy &copy, const Original &original)
    {
        auto authority = [&] { return copy.live() && original.matches(pid, fd); };
        if (!authority()) return Status::denied("copied TCP accept has no original listener authority");
        const u32 nonblock = linux ? Constants::sock_nonblock : OK_SOCK_NONBLOCK;
        const u32 cloexec = linux ? Constants::sock_cloexec : OK_SOCK_CLOEXEC;
        if ((!accept4 && flags) || (flags & ~(nonblock | cloexec)))
            return Status::invalid_argument("TCP accept4 flags are outside the target profile");
        // Admission pins the original OFD and process lifetime before any copy
        // callback, but has not selected a listener child or reserved a stream.
        auto admitted = operations_.begin_accept(pid, fd, flags & nonblock, flags & cloexec);
        if (!admitted) return admitted.status();
        auto &operation = admitted.value();
        usize prefix{};
        if (address)
        {
            if (auto status = interval(length_address, 4); !status) return status;
            if (auto status = copy.preflight(length_address, 4, false); !status) return status;
            if (!authority() || !operation.live()) return lost();
            std::array<std::byte, 4> length{};
            if (auto status = copy.read(length_address, length); !status) return status;
            if (!authority() || !operation.live()) return lost();
            std::array<u8, 4> copied{};
            for (usize at = 0; at < copied.size(); ++at) copied[at] = std::to_integer<u8>(length[at]);
            auto supplied = Wire::decode_socklen(copied);
            if (!supplied || supplied.value() < 0) return Status::invalid_argument("TCP accept socklen capacity is negative or malformed");
            prefix = static_cast<usize>(supplied.value()) < 16 ? static_cast<usize>(supplied.value()) : 16;
            if (prefix)
            {
                if (auto status = interval(address, prefix); !status) return status;
                if (overlap(address, prefix, length_address, 4))
                    return Status::invalid_argument("TCP accept address and length controls overlap");
                if (auto status = copy.preflight(address, prefix, true); !status) return status;
                if (!authority() || !operation.live()) return lost();
            }
            if (auto status = copy.preflight(length_address, 4, true); !status) return status;
            if (!authority() || !operation.live()) return lost();
        }
        const auto prepared = operation.prepare_child();
        if (!prepared)
            return prepared.code() == StatusCode::would_block && !(original.flags() & TcpStreamOperations::nonblock)
                ? Result<PreparedTcpAccept>{Status::unsupported("blocking TCP accept requires a captured continuation")}
                : Result<PreparedTcpAccept>{prepared};
        if (address)
        {
            std::array<u8, 16> record{};
            const auto peer = operation.peer();
            if (auto status = Wire::encode_address(record, {2, peer.port, peer.address}); !status) return status;
            std::array<std::byte, 16> bytes{};
            for (usize at = 0; at < bytes.size(); ++at) bytes[at] = std::byte{record[at]};
            if (prefix)
            {
                if (auto status = copy.write(address, std::span{bytes}.first(prefix)); !status) return status;
                if (!authority() || !operation.live()) return lost();
            }
            std::array<u8, 4> length{};
            if (auto status = Wire::encode_socklen(length, 16); !status) return status;
            std::array<std::byte, 4> output{};
            for (usize at = 0; at < output.size(); ++at) output[at] = std::byte{length[at]};
            if (auto status = copy.write(length_address, output); !status) return status;
            if (!authority() || !operation.live()) return lost();
        }
        if (!authority() || !operation.live()) return lost();
        return std::move(operation);
    }
  private:
    static Status lost() { return Status::denied("TCP accept output lost captured image or original listener"); }
    static Status interval(u64 address, usize size)
    {
        return !address || address > Codec::word_mask || (size && address > Codec::word_mask - (size - 1))
            ? Status::fault("TCP accept copied interval exceeds target word") : Status{};
    }
    static bool overlap(u64 first, usize first_size, u64 second, usize second_size)
    { return first <= second ? second - first < first_size : first - second < second_size; }
    TcpStreamOperations &operations_;
};
}
