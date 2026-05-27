#pragma once

#include "ok/core/types.hpp"

#include <span>
#include <string_view>

namespace ok::shell_detail
{

inline std::string_view trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    {
        value.remove_suffix(1);
    }
    return value;
}

inline std::string_view first_word(std::string_view value)
{
    value = trim(value);
    usize size = 0;
    while (size < value.size() && value[size] != ' ' && value[size] != '\t')
    {
        ++size;
    }
    return value.substr(0, size);
}

inline std::string_view after_first_word(std::string_view value)
{
    value = trim(value);
    auto word = first_word(value);
    value.remove_prefix(word.size());
    return trim(value);
}

inline std::span<const std::byte> as_bytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

inline Result<u64> parse_unsigned(std::string_view text)
{
    text = trim(text);
    if (text.empty())
    {
        return Status::invalid_argument("expected unsigned integer");
    }
    u64 value = 0;
    for (const auto ch : text)
    {
        if (ch < '0' || ch > '9')
        {
            return Status::invalid_argument("invalid unsigned integer");
        }
        value = value * 10 + static_cast<u64>(ch - '0');
    }
    return value;
}

} // namespace ok::shell_detail
