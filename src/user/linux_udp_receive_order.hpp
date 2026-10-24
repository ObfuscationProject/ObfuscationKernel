#pragma once

#include "ok/abi/linux_udp_address.hpp"
#include "ok/net/native_udp.hpp"
#include <bit>
#include <concepts>

namespace ok::user::native::linux_udp
{

struct ReceiveOutput
{
    u64 payload{}, count{}, source{}, source_length{};
    u32 flags{};
};

template <class Access>
concept ReceiveAccess = requires(Access &access, u64 pointer,
                                 std::span<std::byte> input,
                                 std::span<const std::byte> output) {
    { access.live() } -> std::same_as<bool>;
    { access.read(pointer, input) } -> std::same_as<Status>;
    { access.write(pointer, output) } -> std::same_as<Status>;
};

// Owned operation bytes only. The caller supplies the retained original
// endpoint and current-image access; scratch exclusion creates neither.
class ReceiveScratch final
{
  public:
    static constexpr u32 peek = 2, trunc = 32, dontwait = 64;
    ReceiveScratch() = default;
    ReceiveScratch(const ReceiveScratch &) = delete;
    ReceiveScratch &operator=(const ReceiveScratch &) = delete;
    ReceiveScratch(ReceiveScratch &&) = delete;
    ReceiveScratch &operator=(ReceiveScratch &&) = delete;

    template <abi::linux_user::Profile Target, ReceiveAccess Access>
    Result<usize> receive(net::NativeUdpService &service, u32 original_endpoint,
                          Access &access, ReceiveOutput output)
    {
        if (busy_) return Status::busy("recursive Linux UDP receive scratch entry");
        Scope scope{*this};
        if (output.flags & ~(peek | trunc | dontwait))
            return Status::unsupported("Linux UDP receive flag outside this profile");
        if (!access.live()) return Status::fault("Linux UDP receive image access expired");
        const auto selected = service.peek(original_endpoint);
        if (!selected) return selected.status();
        const auto &packet = selected.value();
        const usize actual = packet.payload.size();
        if (actual > payload_.size())
            return Status::io_error("Linux UDP service packet exceeds owned scratch");
        // Finish all borrowed reads before consume, including the source fields.
        for (usize at = 0; at < actual; ++at) payload_[at] = packet.payload[at];
        if (auto status = abi::linux_user::Ipv4SocketAddressCodec<Target>::encode(
                source_, {2, packet.source_port, packet.source}); !status) return status;
        if (!(output.flags & peek))
            if (auto status = service.consume(original_endpoint); !status) return status;
        const usize copied = output.count < actual ? static_cast<usize>(output.count) : actual;
        if (copied)
        {
            if (!access.live()) return Status::fault("Linux UDP receive image access expired");
            if (auto status = access.write(output.payload, {payload_.data(), copied}); !status) return status;
        }
        // A capability can be revoked by a copy callback even on success.
        if (!access.live()) return Status::fault("Linux UDP receive image access expired");
        if (output.source)
        {
            if (auto status = access.read(output.source_length, length_); !status) return status;
            if (!access.live()) return Status::fault("Linux UDP receive image access expired");
            const i32 capacity = std::bit_cast<i32>(read_length<Target>());
            if (capacity < 0) return Status::invalid_argument("negative Linux UDP source capacity");
            const usize prefix = static_cast<u32>(capacity) < source_.size()
                ? static_cast<u32>(capacity) : source_.size();
            if (prefix)
            {
                const auto *bytes = reinterpret_cast<const std::byte *>(source_.data());
                if (auto status = access.write(output.source, {bytes, prefix}); !status) return status;
                if (!access.live()) return Status::fault("Linux UDP receive image access expired");
            }
            write_length<Target>(16);
            if (auto status = access.write(output.source_length, length_); !status) return status;
            if (!access.live()) return Status::fault("Linux UDP receive image access expired");
        }
        return output.flags & trunc ? actual : copied;
    }

  private:
    class Scope final
    {
      public:
        explicit Scope(ReceiveScratch &scratch) : scratch_{scratch} { scratch_.busy_ = true; }
        ~Scope() { scratch_.busy_ = false; }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        Scope(Scope &&) = delete;
        Scope &operator=(Scope &&) = delete;
      private:
        ReceiveScratch &scratch_;
    };
    template <abi::linux_user::Profile Target> u32 read_length() const
    {
        u32 value{};
        for (usize at = 0; at < length_.size(); ++at)
        {
            const usize shift = abi::linux_user::Codec<Target>::profile.big_endian ? 3 - at : at;
            value |= u32{std::to_integer<u8>(length_[at])} << (8 * shift);
        }
        return value;
    }
    template <abi::linux_user::Profile Target> void write_length(u32 value)
    {
        for (usize at = 0; at < length_.size(); ++at)
        {
            const usize shift = abi::linux_user::Codec<Target>::profile.big_endian ? 3 - at : at;
            length_[at] = std::byte{static_cast<u8>(value >> (8 * shift))};
        }
    }
    bool busy_{};
    std::array<std::byte, net::NativeUdpService::datagram_capacity> payload_{};
    std::array<u8, 16> source_{};
    std::array<std::byte, 4> length_{};
};

} // namespace ok::user::native::linux_udp
