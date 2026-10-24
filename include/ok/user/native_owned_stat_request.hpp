#pragma once

#include "ok/abi/linux_native.hpp"
#include "ok/abi/linux_stat_call.hpp"

#include <concepts>
#include <optional>
#include <utility>

namespace ok::user::native
{
// One captured Linux stat-family call. This type owns scalar arguments and the
// checked pathname bytes, but does not retain cwd, dirfd, process or credentials.
template <abi::linux_user::Profile Selected>
    requires abi::linux_user::SupportedProfile<Selected>
class OwnedStatRequest final
{
    using Bridge = abi::linux_user::NativeBridge<Selected>;
    using Codec = abi::linux_user::Codec<Selected>;
    using Adapter = abi::linux_user::StatCallAdapter<Selected>;
    using StatCodec = abi::linux_user::StatCodec<Selected>;
    using StatCall = abi::linux_user::StatCall;
    using StatRequest = abi::linux_user::StatRequest;
    using Operation = abi::linux_user::Operation;
  public:
    using Frame = typename Bridge::Frame;
    class Payload final
    {
      public:
        Payload(const Payload &) = delete;
        Payload &operator=(const Payload &) = delete;
        Payload(Payload &&other) noexcept
            : operation_(other.operation_), path_(std::move(other.path_)), call_(other.call_),
              valid_(std::exchange(other.valid_, false)),
              completion_(std::exchange(other.completion_, Completion::finished)) {}
        Payload &operator=(Payload &&) = delete;

        [[nodiscard]] explicit operator bool() const { return valid_; }
        [[nodiscard]] const StatRequest *operation() const { return valid_ ? &operation_ : nullptr; }
        [[nodiscard]] std::string_view pathname() const { return valid_ ? path_.view() : std::string_view{}; }
        [[nodiscard]] StatCall call() const { return call_; }
        // Only absolute nonempty pathname operations can be considered for a
        // later continuation without retaining cwd/dirfd context. Even those
        // need separately retained process, credentials and mount identities.
        [[nodiscard]] bool absolute_path_candidate() const
        { return valid_ && !operation_.direct_fd && !path_.view().empty() && path_.view().front() == '/' &&
                 !(operation_.flags & posix::at_EMPTY_PATH); }

        template <typename Query, typename Admit, typename Copy>
            requires requires(Query query, Admit admit, Copy copy, StatRequest request,
                              std::string_view path, u64 address, usize length,
                              std::span<const u8> bytes) {
                { query(request, path) } -> std::same_as<Result<posix::FileStatus>>;
                { admit(address, length) } -> std::same_as<bool>;
                { copy(address, bytes) } -> std::same_as<Status>;
            }
        i64 execute_synchronous(Query &&query, Admit &&admit, Copy &&copy)
        {
            if (!valid_ || completion_ != Completion::ready) return -125;
            completion_ = Completion::executing;
            return finish_claimed(query(operation_, path_.view()),
                                  std::forward<Admit>(admit), std::forward<Copy>(copy));
        }

        // A future continuation may obtain a query result under separately
        // retained context, then finish exactly once without user path/stack I/O.
        template <typename Admit, typename Copy>
            requires requires(Admit admit, Copy copy, u64 address, usize length,
                              std::span<const u8> bytes) {
                { admit(address, length) } -> std::same_as<bool>;
                { copy(address, bytes) } -> std::same_as<Status>;
            }
        i64 finish_result(Result<posix::FileStatus> value, Admit &&admit, Copy &&copy)
        {
            if (!valid_ || completion_ != Completion::ready) return -125;
            completion_ = Completion::executing;
            return finish_claimed(std::move(value), std::forward<Admit>(admit), std::forward<Copy>(copy));
        }

      private:
        template <typename Admit, typename Copy>
        i64 finish_claimed(Result<posix::FileStatus> value, Admit &&admit, Copy &&copy)
        {
            completion_ = Completion::finished;
            if (!value) return value.status().code() == StatusCode::unsupported
                              ? -95 : -syscall::ErrnoMapper::errno_for(value.status());
            std::array<u8, 256> bytes{};
            const usize length = StatCodec::size(operation_.layout);
            const auto encoded = StatCodec::encode(value.value(), operation_.layout, {bytes.data(), length});
            if (!encoded) return encoded.status().code() == StatusCode::unsupported
                                   ? -95 : -syscall::ErrnoMapper::errno_for(encoded.status());
            if (!operation_.output || length - 1 > Codec::word_mask - operation_.output ||
                !admit(operation_.output, length)) return -14;
            const auto copied = copy(operation_.output, {bytes.data(), length});
            return copied ? 0 : -syscall::ErrnoMapper::errno_for(copied);
        }

        friend class OwnedStatRequest;
        enum class Completion : u8 { ready, executing, finished };
        Payload(StatCall call, StatRequest operation, FixedString<256> path)
            : operation_(operation), path_(std::move(path)), call_(call) {}
        StatRequest operation_{};
        FixedString<256> path_{};
        StatCall call_{};
        bool valid_{true};
        Completion completion_{Completion::ready};
    };

    OwnedStatRequest(const OwnedStatRequest &) = delete;
    OwnedStatRequest &operator=(const OwnedStatRequest &) = delete;
    OwnedStatRequest(OwnedStatRequest &&other) noexcept
        : operation_(other.operation_), path_(std::move(other.path_)), call_(other.call_),
          state_(std::exchange(other.state_, State::moved)) {}
    OwnedStatRequest &operator=(OwnedStatRequest &&) = delete;

    template <abi::linux_user::StackArgumentCopy CopyStack, typename ReadPath>
        requires requires(ReadPath path, u64 address, FixedString<256> &out) {
            { path(address, out) } -> std::same_as<Status>;
        }
    [[nodiscard]] static Result<OwnedStatRequest> capture(const Frame &frame, CopyStack &&copy_stack,
                                                           ReadPath &&read_path)
    {
        // Freeze the saved frame before either checked user copy callback runs.
        const Frame saved = frame;
        const auto registers = Bridge::snapshot(saved);
        const u64 number = registers.integer[Codec::profile.number_register];
        const auto call = select(number);
        if (!call) return Status::unsupported("Linux stat syscall number is not supported");
        const usize count = Adapter::argument_count(*call);
        const auto captured = Bridge::decode_call_copying_stack(saved, count,
            std::forward<CopyStack>(copy_stack));
        if (!captured) return captured.status();
        if (captured.value().number != number || captured.value().argument_count != count)
            return Status::fault("Linux stat saved argument frame changed");
        const auto decoded = Adapter::decode(*call, std::span{captured.value().arguments}.first(count));
        if (!decoded) return decoded.status();
        const StatRequest operation = decoded.value();
        FixedString<256> path;
        if (!operation.direct_fd)
        {
            if (!operation.path)
            {
                if (!(operation.flags & posix::at_EMPTY_PATH))
                    return Status::fault("Linux stat pathname is null");
            }
            else if (auto status = read_path(operation.path, path); !status)
                return status;
            if (path.view().empty() && !(operation.flags & posix::at_EMPTY_PATH))
                return Status::not_found("Linux stat pathname is empty");
        }
        return OwnedStatRequest(*call, operation, std::move(path));
    }

    [[nodiscard]] Result<Payload> take()
    {
        if (state_ == State::moved) return Status::not_initialized("Linux stat request was moved");
        if (state_ == State::consumed) return Status::already_exists("Linux stat request already consumed");
        state_ = State::consumed;
        return Payload(call_, operation_, std::move(path_));
    }

  private:
    enum class State : u8 { ready, consumed, moved };
    OwnedStatRequest(StatCall call, StatRequest operation, FixedString<256> path)
        : operation_(operation), path_(std::move(path)), call_(call) {}
    [[nodiscard]] static std::optional<StatCall> select(u64 number)
    {
        auto is = [number](Operation operation) {
            const auto expected = Codec::syscall_number(operation);
            return expected && number == *expected;
        };
        if (is(Operation::statx)) return StatCall::statx;
        if constexpr (StatCodec::has_kernel_stat)
        {
            if (is(Operation::fstat)) return StatCall::fstat;
            if (is(Operation::newfstatat)) return StatCall::newfstatat;
            if constexpr (Selected == abi::linux_user::Profile::x86_64)
            {
                if (is(Operation::stat)) return StatCall::stat;
                if (is(Operation::lstat)) return StatCall::lstat;
            }
        }
        return std::nullopt;
    }
    StatRequest operation_{};
    FixedString<256> path_{};
    StatCall call_{};
    State state_{State::ready};
};
} // namespace ok::user::native
