#pragma once
#include "native_syscall_origin.hpp"
#include "tcp_original_ofd.hpp"

namespace ok::user::native
{
template <abi::linux_user::Profile Selected>
class TcpImmediateAccess final
{
  public:
    using Origins = SyscallOriginTable<Selected>;
    using Capture = typename Origins::TcpImmediateCapture;
    TcpImmediateAccess(const TcpImmediateAccess &) = delete;
    TcpImmediateAccess &operator=(const TcpImmediateAccess &) = delete;
    TcpImmediateAccess &operator=(TcpImmediateAccess &&) = delete;
    TcpImmediateAccess(TcpImmediateAccess &&other) noexcept
        : capture_(std::move(other.capture_)), original_(std::exchange(other.original_, nullptr)) {}
    [[nodiscard]] static Result<TcpImmediateAccess> acquire(Origins &origins,
        const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread)
    {
        auto capture = origins.tcp_immediate_capture(process, thread);
        return capture ? Result<TcpImmediateAccess>{TcpImmediateAccess{std::move(capture.value())}}
                       : Result<TcpImmediateAccess>{capture.status()};
    }
    [[nodiscard]] bool live() const { return capture_.live() && (!original_ || original_->live()); }
    Status bind_original(TcpOriginalDescription &original, i32 fd)
    {
        if (original_ || !capture_.live() || !original.matches(capture_.pid(), fd))
            return Status::denied("TCP copied access cannot bind a foreign original description");
        original_ = &original; return {};
    }
    [[nodiscard]] bool linux_personality() const { return capture_.linux_personality(); }
    [[nodiscard]] u32 raw_syscall() const { return capture_.raw_syscall(); }
    [[nodiscard]] u64 pid() const { return capture_.pid(); }
    [[nodiscard]] u64 tid() const { return capture_.tid(); }
    [[nodiscard]] abi::linux_user::TcpCall operation() const { return capture_.operation(); }
    Status preflight(u64 address, usize size, bool write)
    {
        namespace hw = arch::execution;
        if (!live()) return lost();
        const u64 mask = linux_personality() ? abi::linux_user::Codec<Selected>::word_mask : ~uptr{0};
        if (address > mask || size > mask || (size && address > mask - (size - 1)) ||
            address > hw::user_end || size > hw::user_end - address || (size && address < hw::user_begin))
            return Status::fault("TCP copied interval exceeds the captured user range");
        auto space = capture_.address_space(); if (!space) return lost();
        const bool admitted = hw::user_range(space.value(), static_cast<uptr>(address), size, write);
        if (!live()) return lost();
        return admitted ? Status{} : Status::fault("TCP copied interval is not fully mapped with required permission");
    }
    Status read(u64 address, std::span<std::byte> output)
    {
        if (auto status = preflight(address, output.size(), false); !status) return status;
        if (output.empty()) return {};
        auto space = capture_.address_space(); if (!space) return lost();
        const auto copied = arch::execution::copy_from_user(space.value(), output.data(), static_cast<uptr>(address), output.size());
        return live() ? copied : lost();
    }
    Status write(u64 address, std::span<const std::byte> input)
    {
        if (auto status = preflight(address, input.size(), true); !status) return status;
        if (input.empty()) return {};
        auto space = capture_.address_space(); if (!space) return lost();
        const auto copied = arch::execution::copy_to_user(space.value(), static_cast<uptr>(address), input.data(), input.size());
        return live() ? copied : lost();
    }
    void reset() { capture_.reset(); original_ = nullptr; }
  private:
    explicit TcpImmediateAccess(Capture &&capture) : capture_(std::move(capture)) {}
    Status lost()
    { reset(); return Status::denied("TCP immediate access lost its original captured image/descriptor"); }
    Capture capture_;
    TcpOriginalDescription *original_{};
};
}
