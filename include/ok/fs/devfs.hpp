#pragma once

#include "ok/fs/terminal_node.hpp"
#include "ok/fs/vfs.hpp"

#include <utility>

namespace ok::fs
{
// Character devices whose behavior is a kernel mechanism. The directory has a
// fixed set of nodes that live as long as the object; where it is mounted
// (normally /dev) is distribution policy.
//
//   null     reads end of file, writes are discarded
//   zero     reads zero bytes, writes are discarded
//   full     reads zero bytes, writes fail with ENOSPC
//   tty      the caller's controlling terminal
//   console  the serial console terminal
//   ptmx     allocates a PTY pair and opens its master
//   pts/N    the slave of PTY pair N, listed while its master is open
//
// The terminal nodes and pts entries are names only: open() converts them
// through the scheduler (docs/modules/tty.md sections 2 and 3). /dev/pts lists
// nothing until attach_terminals links the scheduler's terminal namespace.
class DeviceFileSystem final
{
  public:
    enum class Kind : u8 { null, zero, full, controlling, console, multiplexer };
    static constexpr usize device_count = 6;
    static constexpr usize root_entry_count = device_count + 1; // The devices and pts.

    // Constant-initialized: kernel images run no dynamic initializers, so a
    // global instance must not need one.
    constexpr DeviceFileSystem() : DeviceFileSystem(std::make_index_sequence<pts_capacity>{}) {}
    DeviceFileSystem(const DeviceFileSystem &) = delete;
    DeviceFileSystem &operator=(const DeviceFileSystem &) = delete;
    DeviceFileSystem(DeviceFileSystem &&) = delete;
    DeviceFileSystem &operator=(DeviceFileSystem &&) = delete;

    [[nodiscard]] Node &root() { return root_; }
    // Links the terminal namespace the VFS carries (PosixService attaches the
    // scheduler's). Call it before mounting; the same namespace again succeeds,
    // another one is busy, and a VFS without one is not_initialized.
    Status attach_terminals(VirtualFileSystem &vfs);
    Status attach_terminals(TerminalNamespace &terminals);
    [[nodiscard]] const TerminalNamespace *terminals() const { return terminals_; }

  private:
    template <usize... Index>
    constexpr explicit DeviceFileSystem(std::index_sequence<Index...>)
        : root_(*this), pts_root_(*this),
          devices_{Device{*this, Kind::null}, Device{*this, Kind::zero}, Device{*this, Kind::full},
                   Device{*this, Kind::controlling}, Device{*this, Kind::console}, Device{*this, Kind::multiplexer}},
          pts_{PtsDevice{*this, static_cast<u16>(Index)}...}
    {
    }

    class Device final : public Node
    {
      public:
        constexpr Device(const DeviceFileSystem &owner, Kind kind) : owner_(owner), kind_(kind) {}
        std::string_view name() const override;
        Metadata metadata() const override;
        Result<FileBuffer> read(usize offset, usize count) const override;
        Status write(usize offset, std::span<const std::byte> data) override;
        Status truncate(usize) override { return Status::success(); }
        const void *filesystem_identity() const override { return &owner_; }
        TerminalNode terminal_node() const override;
        Node *lookup(std::string_view) override { return nullptr; }
        Status create(std::string_view, NodeType) override { return Status::not_directory("device node"); }
        Status chmod(u32) override { return Status::permission("device nodes are fixed"); }
        Status chown(u32, u32) override { return Status::permission("device nodes are fixed"); }
        [[nodiscard]] Kind kind() const { return kind_; }

      private:
        const DeviceFileSystem &owner_;
        Kind kind_;
    };
    class Directory final : public Node
    {
      public:
        constexpr explicit Directory(DeviceFileSystem &owner) : owner_(owner) {}
        std::string_view name() const override { return "/"; }
        Metadata metadata() const override;
        Result<FileBuffer> read(usize, usize) const override { return Status::is_directory("device directory"); }
        Status write(usize, std::span<const std::byte>) override { return Status::is_directory("device directory"); }
        Result<DirectoryListing> list(DirectoryReadIntent = DirectoryReadIntent::access) const override;
        const void *filesystem_identity() const override { return &owner_; }
        Node *lookup(std::string_view child) override;
        Status create(std::string_view, NodeType) override { return Status::permission("device directory is fixed"); }
        Status chmod(u32) override { return Status::permission("device directory is fixed"); }
        Status chown(u32, u32) override { return Status::permission("device directory is fixed"); }

      private:
        DeviceFileSystem &owner_;
    };
    // /dev/pts: one entry per published PTY pair, locked ones included.
    class PtsDirectory final : public Node
    {
      public:
        constexpr explicit PtsDirectory(DeviceFileSystem &owner) : owner_(owner) {}
        std::string_view name() const override { return "pts"; }
        Metadata metadata() const override;
        Result<FileBuffer> read(usize, usize) const override { return Status::is_directory("pts directory"); }
        Status write(usize, std::span<const std::byte>) override { return Status::is_directory("pts directory"); }
        Result<DirectoryListing> list(DirectoryReadIntent = DirectoryReadIntent::access) const override;
        const void *filesystem_identity() const override { return &owner_; }
        Node *lookup(std::string_view child) override;
        Status create(std::string_view, NodeType) override { return Status::permission("pts directory is fixed"); }
        Status chmod(u32) override { return Status::permission("pts directory is fixed"); }
        Status chown(u32, u32) override { return Status::permission("pts directory is fixed"); }

      private:
        DeviceFileSystem &owner_;
    };
    // Mode and ownership live in the scheduler's namespace entry and change
    // through the terminal service (fchmod/fchown and the path routes).
    class PtsDevice final : public Node
    {
      public:
        constexpr PtsDevice(const DeviceFileSystem &owner, u16 index) : owner_(owner), index_(index) {}
        std::string_view name() const override;
        Metadata metadata() const override;
        Result<Metadata> query_metadata() const override;
        Result<FileBuffer> read(usize, usize) const override;
        Status write(usize, std::span<const std::byte>) override;
        Status truncate(usize) override { return Status::success(); }
        const void *filesystem_identity() const override { return &owner_; }
        TerminalNode terminal_node() const override;
        Node *lookup(std::string_view) override { return nullptr; }
        Status create(std::string_view, NodeType) override { return Status::not_directory("device node"); }
        Status chmod(u32) override { return Status::permission("pts nodes change through the terminal service"); }
        Status chown(u32, u32) override { return Status::permission("pts nodes change through the terminal service"); }
        [[nodiscard]] PtsEntryView view() const;

      private:
        const DeviceFileSystem &owner_;
        u16 index_;
    };

    Directory root_;
    PtsDirectory pts_root_;
    std::array<Device, device_count> devices_;
    std::array<PtsDevice, pts_capacity> pts_;
    TerminalNamespace *terminals_{};
};
} // namespace ok::fs
