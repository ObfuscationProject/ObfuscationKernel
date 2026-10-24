#pragma once

#include "ok/core/types.hpp"
#include <type_traits>

namespace ok::block
{
// I/O history is independent of pointer and storage-capacity width.
using SessionCounter = u64;

template <typename Counter>
concept SessionCounterPolicy = std::is_unsigned_v<Counter> &&
    !std::is_same_v<std::remove_cv_t<Counter>, bool> && sizeof(Counter) <= sizeof(SessionCounter);

template <SessionCounterPolicy Counter = SessionCounter>
inline constexpr SessionCounter session_counter_limit = SessionCounter(Counter(~Counter{0}));

// Pure finite-width arithmetic. This grants no Session or transport authority.
template <SessionCounterPolicy Counter = SessionCounter>
[[nodiscard]] constexpr bool session_counter_fits(SessionCounter used, SessionCounter capacity)
{
    constexpr SessionCounter limit = session_counter_limit<Counter>;
    return used <= limit && capacity <= limit - used;
}
} // namespace ok::block
