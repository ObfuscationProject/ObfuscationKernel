#pragma once

#include "ok/core/types.hpp"

#include <span>

namespace ok::driver
{

// A runtime device boundary. The implementation must not modify `out` on
// failure and must provide all requested bytes on success. It has static boot
// lifetime while installed in PosixService.
class EntropySource
{
  public:
    virtual ~EntropySource() = default;
    virtual Status fill(std::span<std::byte> out) = 0;
};

} // namespace ok::driver
