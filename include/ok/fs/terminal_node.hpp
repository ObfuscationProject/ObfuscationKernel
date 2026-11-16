#pragma once

#include "ok/core/types.hpp"
#include "ok/fs/attributes.hpp"

#include <array>

namespace ok::fs
{
// Terminal device nodes (docs/modules/tty.md section 2). The device filesystem
// only names them; every open converts through the scheduler, which owns the
// terminals and re-validates whatever a node reports.
inline constexpr usize pts_capacity = 8; // scheduler.hpp asserts it equals the PTY pair capacity.
inline constexpr u32 tty_major = 5, tty_minor = 0, console_minor = 1, ptmx_minor = 2;
inline constexpr u32 pts_major = 136;
inline constexpr u16 pts_default_mode = 0620;

enum class TerminalNodeKind : u8
{
    none,
    controlling, // /dev/tty, c5,0
    console,     // /dev/console, c5,1
    multiplexer, // /dev/ptmx, c5,2
    pts_slave,   // /dev/pts/N, c136,N
};

struct TerminalNode
{
    TerminalNodeKind kind{};
    u16 index{};
    u64 generation{};
    constexpr bool operator==(const TerminalNode &) const = default;
};

[[nodiscard]] constexpr FilesystemDevice terminal_device(TerminalNodeKind kind, u16 index = 0)
{
    switch (kind)
    {
    case TerminalNodeKind::controlling:
        return {tty_major, tty_minor};
    case TerminalNodeKind::console:
        return {tty_major, console_minor};
    case TerminalNodeKind::multiplexer:
        return {tty_major, ptmx_minor};
    case TerminalNodeKind::pts_slave:
        return {pts_major, index};
    case TerminalNodeKind::none:
        break;
    }
    return {};
}

// Inodes are major * 256 + minor; /dev/console cannot use 1, the root's.
[[nodiscard]] constexpr u64 terminal_inode(TerminalNodeKind kind, u16 index = 0)
{
    const auto device = terminal_device(kind, index);
    return u64{device.major} * 256 + device.minor;
}

// The published state of one /dev/pts entry. A locked entry is listed; an
// entry is removed (present false) when its master's last open closes.
struct PtsEntryView
{
    u64 generation{};
    u32 uid{}, gid{};
    u16 mode{pts_default_mode};
    bool present{}, locked{};
    constexpr bool operator==(const PtsEntryView &) const = default;
};

// Owned by the scheduler and constant-initialized. The scheduler alone
// publishes entries; the device filesystem reads them, and bind_provider
// records the device filesystem whose identity terminal descriptors report.
class TerminalNamespace final
{
  public:
    constexpr TerminalNamespace() = default;
    TerminalNamespace(const TerminalNamespace &) = delete;
    TerminalNamespace &operator=(const TerminalNamespace &) = delete;

    [[nodiscard]] PtsEntryView entry(usize index) const { return index < entries_.size() ? entries_[index] : PtsEntryView{}; }
    [[nodiscard]] u64 revision() const { return revision_; }
    [[nodiscard]] const void *provider() const { return provider_; }
    Status bind_provider(const void *identity)
    {
        if (!identity) return Status::invalid_argument("terminal namespace provider has no identity");
        if (provider_ && provider_ != identity) return Status::busy("terminal namespace has another provider");
        provider_ = identity;
        return {};
    }
    void publish(usize index, const PtsEntryView &view)
    {
        if (index >= entries_.size()) return;
        entries_[index] = view;
        ++revision_;
    }
    // Both change the entry only while it still names `generation`.
    void set_locked(usize index, u64 generation, bool locked)
    {
        auto view = entry(index);
        if (view.generation != generation || !view.present) return;
        view.locked = locked;
        publish(index, view);
    }
    void remove(usize index, u64 generation)
    {
        auto view = entry(index);
        if (view.generation != generation || !view.present) return;
        view.present = view.locked = false;
        publish(index, view);
    }

  private:
    std::array<PtsEntryView, pts_capacity> entries_{};
    u64 revision_{};
    const void *provider_{};
};
} // namespace ok::fs
