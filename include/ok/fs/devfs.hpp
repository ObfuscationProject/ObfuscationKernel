#pragma once

#include "ok/fs/vfs.hpp"

namespace ok::fs
{
// Character devices whose behavior is a kernel mechanism. The directory has a
// fixed set of nodes that live as long as the object; where it is mounted
// (normally /dev) is distribution policy.
//
//   null  reads end of file, writes are discarded
//   zero  reads zero bytes, writes are discarded
//   full  reads zero bytes, writes fail with ENOSPC
class DeviceFileSystem final
{
  public:
    enum class Kind : u8 { null, zero, full };
    static constexpr usize device_count = 3;

    // Constant-initialized: kernel images run no dynamic initializers, so a
    // global instance must not need one.
    constexpr DeviceFileSystem()
        : root_(*this), devices_{Device{*this, Kind::null}, Device{*this, Kind::zero}, Device{*this, Kind::full}}
    {
    }
    DeviceFileSystem(const DeviceFileSystem &) = delete;
    DeviceFileSystem &operator=(const DeviceFileSystem &) = delete;
    DeviceFileSystem(DeviceFileSystem &&) = delete;
    DeviceFileSystem &operator=(DeviceFileSystem &&) = delete;

    [[nodiscard]] Node &root() { return root_; }

  private:
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

    Directory root_;
    std::array<Device, device_count> devices_;
};
} // namespace ok::fs
