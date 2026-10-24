#pragma once

#include "ok/core/types.hpp"
#include <array>
#include <concepts>
#include <span>
#include <type_traits>

namespace ok::user::native_pty
{
inline constexpr u32 nonblock = 0x0800U;
inline constexpr u32 cloexec = 0x080000U;
inline constexpr usize pair_output_bytes = 8;

// Preparation owns both unpublished fds, both OFDs and the provider. Only the
// serialized caller may mutate those owners between validation and publication.
template <bool BigEndian, typename Word, typename Writable, typename Prepare,
          typename CopyOut, typename Publish, typename RecordFailure>
requires (std::unsigned_integral<Word> && (sizeof(Word) == sizeof(u32) || sizeof(Word) == sizeof(u64)))
Result<usize> create(const std::array<Word, 6> &arguments, Writable &&writable,
                     Prepare &&prepare, CopyOut &&copy_out, Publish &&publish,
                     RecordFailure &&record_failure)
{
    if ((arguments[1] & ~static_cast<Word>(nonblock | cloexec)) ||
        arguments[2] || arguments[3] || arguments[4] || arguments[5])
        return Status::invalid_argument("invalid native PTY pair arguments");
    const auto output = arguments[0];
    if (!output || output > static_cast<Word>(~Word{0} - (pair_output_bytes - 1)) ||
        !writable(output, pair_output_bytes))
        return Status::fault("native PTY pair output is not writable");

    auto prepared = prepare(static_cast<u32>(arguments[1]));
    if (!prepared) return prepared.status();
    auto &operation = prepared.value();
    using Operation = std::remove_reference_t<decltype(operation)>;
    static_assert(std::same_as<std::invoke_result_t<Publish &, Operation &>, void> &&
                  std::is_nothrow_invocable_v<Publish &, Operation &>,
                  "native PTY pair publication must be nonfallible");
    auto refuse = [&](Status failure) -> Result<usize> {
        if (const auto cleanup = operation.cancel(); !cleanup)
        {
            record_failure(cleanup);
            return cleanup;
        }
        return failure;
    };
    if (auto status = operation.validate(); !status) return refuse(status);
    const auto descriptors = operation.descriptors();
    static_assert(std::same_as<std::remove_cvref_t<decltype(descriptors)>, std::array<i32, 2>>,
                  "native PTY descriptor wire values are signed 32 bit");
    if (descriptors[0] < 0 || descriptors[1] < 0 || descriptors[0] == descriptors[1])
        return refuse(Status::invalid_argument("invalid prepared native PTY descriptors"));
    std::array<u8, pair_output_bytes> bytes{};
    for (usize side = 0; side < 2; ++side)
        for (usize index = 0; index < 4; ++index)
            bytes[4 * side + index] = static_cast<u8>(static_cast<u32>(descriptors[side]) >>
                (8 * (BigEndian ? 3 - index : index)));
    // Output may partially change on copy failure. No fd is published then.
    if (auto status = copy_out(output, std::span<const u8>{bytes}); !status)
        return refuse(status);
    if (auto status = operation.validate(); !status) return refuse(status);
    publish(operation);
    return usize{0};
}
} // namespace ok::user::native_pty
