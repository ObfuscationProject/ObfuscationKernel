#pragma once

#include "ok/core/types.hpp"

namespace ok::fs
{

// These provider outcomes must not change the global Status/errno mapping.
class ModeChangeResult final
{
  public:
    enum class Kind : u8 { applied, read_only, unavailable, failed };

    [[nodiscard]] static constexpr ModeChangeResult applied() { return {Kind::applied, {}}; }
    [[nodiscard]] static constexpr ModeChangeResult read_only() { return {Kind::read_only, {}}; }
    [[nodiscard]] static constexpr ModeChangeResult unavailable() { return {Kind::unavailable, {}}; }
    [[nodiscard]] static constexpr ModeChangeResult failure(Status status)
    {
        return status ? applied() : ModeChangeResult{Kind::failed, status};
    }
    [[nodiscard]] constexpr Kind kind() const { return kind_; }
    [[nodiscard]] constexpr Status status() const { return status_; }
    [[nodiscard]] constexpr bool ok() const { return kind_ == Kind::applied; }

  private:
    constexpr ModeChangeResult(Kind kind, Status status) : kind_(kind), status_(status) {}
    Kind kind_;
    Status status_;
};

} // namespace ok::fs
