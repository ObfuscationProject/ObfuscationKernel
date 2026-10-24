#pragma once
#include <ok/core/types.hpp>
namespace ok::net
{
struct TcpStreamHandle
{
    u64 owner{}, generation{}; u32 slot{};
    [[nodiscard]] bool operator==(const TcpStreamHandle &) const = default;
    [[nodiscard]] explicit operator bool() const { return owner && generation; }
};
}
