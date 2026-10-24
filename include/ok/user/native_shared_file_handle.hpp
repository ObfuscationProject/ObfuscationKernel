#pragma once
#include "ok/core/types.hpp"
namespace ok::user::native
{
struct SharedFileHandle
{
    u64 owner{}, generation{};
    u16 slot{};
    [[nodiscard]] explicit operator bool() const { return owner && generation; }
    [[nodiscard]] bool operator==(const SharedFileHandle &) const = default;
};
} // namespace ok::user::native
