#pragma once

#include "native_syscall_origin.hpp"

namespace ok::user::native
{
// Immediate image authority only. The operation guard owns its original OFD.
template <abi::linux_user::Profile Selected>
class LinuxUdpImmediateAccess final
{
    friend class LinuxUdpReceiveCoordinator<Selected>;
  public:
    using Origins = SyscallOriginTable<Selected>;
    using Capture = typename Origins::LinuxUdpImmediateCapture;
    LinuxUdpImmediateAccess(const LinuxUdpImmediateAccess &) = delete;
    LinuxUdpImmediateAccess &operator=(const LinuxUdpImmediateAccess &) = delete;
    LinuxUdpImmediateAccess &operator=(LinuxUdpImmediateAccess &&) = delete;
    LinuxUdpImmediateAccess(LinuxUdpImmediateAccess &&other) noexcept
        : capture_(std::move(other.capture_)) {}

    [[nodiscard]] static Result<LinuxUdpImmediateAccess> acquire(
        Origins &origins, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread)
    {
        auto capture = origins.linux_udp_immediate_capture(process, thread);
        if (!capture) return capture.status();
        return LinuxUdpImmediateAccess(std::move(capture.value()));
    }
    [[nodiscard]] bool live() const { return capture_.live(); }
    [[nodiscard]] u64 raw_syscall() const { return capture_.raw_syscall(); }
    [[nodiscard]] LinuxUdpImmediateOperation operation() const { return capture_.operation(); }
    void reset() { capture_.reset(); }

    [[nodiscard]] Status range(uptr address, usize bytes, bool write)
    {
        static_cast<void>(write);
        if (!live()) return lost();
        constexpr u64 mask = abi::linux_user::Codec<Selected>::word_mask;
        namespace hw = arch::execution;
        if (address > mask || bytes > mask || address > hw::user_end ||
            bytes > hw::user_end - address || (bytes && address < hw::user_begin))
            return Status::fault("Linux UDP interval exceeds the supported user range");
        return {};
    }
    [[nodiscard]] Status read(uptr address, std::span<std::byte> output)
    {
        if (auto status = range(address, output.size(), false); !status) return status;
        if (output.empty()) return {};
        auto space = capture_.address_space();
        if (!space) return lost();
        const auto status = arch::execution::copy_from_user(space.value(), output.data(), address, output.size());
        if (!live()) return lost();
        return status;
    }
    [[nodiscard]] Status write(uptr address, std::span<const std::byte> input)
    {
        if (auto status = range(address, input.size(), true); !status) return status;
        if (input.empty()) return {};
        auto space = capture_.address_space();
        if (!space) return lost();
        const auto status = arch::execution::copy_to_user(space.value(), address, input.data(), input.size());
        if (!live()) return lost();
        return status;
    }

  private:
    explicit LinuxUdpImmediateAccess(Capture &&capture) : capture_(std::move(capture)) {}
    Status lost()
    {
        capture_.reset();
        return Status::denied("Linux UDP immediate access lost its current captured image");
    }
    Capture capture_;
};
} // namespace ok::user::native
