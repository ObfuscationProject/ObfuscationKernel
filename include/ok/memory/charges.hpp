#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"

namespace ok::memory
{

// Per-owner accounting. A charge account is owned by a process, a kernel
// subsystem or a cache; resources are charged before they are handed out and
// uncharged when released. The owner fixes the unit: frame accounts count
// pages, object accounts count bytes. `limit == 0` means unlimited.
class ChargeAccount final : private NonCopyable
{
  public:
    constexpr ChargeAccount() = default;
    explicit constexpr ChargeAccount(usize limit) : limit_(limit)
    {
    }

    Status charge(usize units)
    {
        if (units > ~usize{0} - used_)
            return Status{StatusCode::overflow, "charge counter overflow"};
        if (limit_ != 0 && used_ + units > limit_)
            return Status{StatusCode::no_memory, "charge account quota exhausted"};
        used_ += units;
        if (used_ > peak_)
            peak_ = used_;
        return Status::success();
    }
    bool uncharge(usize units)
    {
        if (units > used_)
            return false;
        used_ -= units;
        return true;
    }
    Status set_limit(usize limit)
    {
        if (limit != 0 && limit < used_)
            return Status{StatusCode::busy, "quota below current usage"};
        limit_ = limit;
        return Status::success();
    }

    [[nodiscard]] usize used() const
    {
        return used_;
    }
    [[nodiscard]] usize limit() const
    {
        return limit_;
    }
    [[nodiscard]] usize peak() const
    {
        return peak_;
    }

  private:
    usize limit_{0};
    usize used_{0};
    usize peak_{0};
};

} // namespace ok::memory
