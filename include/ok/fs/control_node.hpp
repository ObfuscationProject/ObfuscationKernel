#pragma once

#include "ok/core/types.hpp"

namespace ok::fs
{
// Kernel control attributes (kernfs-style files, docs/modules/sysfs.md). A
// path open records the node's kind in its open file description; the write
// route compares that byte and never calls into the node for a control write.
enum class ControlNodeKind : u8
{
    none,
    attribute,  // A read-only attribute: opening it for writing is EACCES, even for root.
    cpu_online, // /sys/devices/system/cpu/cpuN/online; `index` is N.
};

struct ControlNode
{
    ControlNodeKind kind{};
    u16 index{};
    constexpr bool operator==(const ControlNode &) const = default;
};
} // namespace ok::fs
