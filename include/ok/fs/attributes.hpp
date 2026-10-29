#pragma once

#include "ok/core/types.hpp"
#include <array>

namespace ok::fs
{
enum class Attribute : u32
{
    inode = 1, device = 2, access_time = 4, modification_time = 8, change_time = 16, blocks512 = 32,
};
// kernel_fixed: a kernel-defined node (devfs) with constant epoch timestamps.
enum class TimestampSource : u8 { unavailable, readonly_ext4, maintained_ext4, kernel_fixed };
struct EpochTimestamp
{
    i64 seconds{};
    u32 nanoseconds{};
    TimestampSource source{TimestampSource::unavailable};
};
struct FilesystemDevice
{
    u32 major{}, minor{};
    bool operator==(const FilesystemDevice &) const = default;
};
struct FileAttributes
{
    u64 inode{};
    FilesystemDevice device{};
    u32 valid{};
    EpochTimestamp access{}, modification{}, change{};
    [[nodiscard]] bool has(Attribute field) const { return (valid & static_cast<u32>(field)) != 0; }
    void mark(Attribute field) { valid |= static_cast<u32>(field); }
};

// Serialized owner-local allocation. Failed requests cannot wrap or reuse IDs.
template <u64 Maximum = ~u64{0}>
class IdentitySequence final
{
  public:
    [[nodiscard]] Result<u64> next()
    {
        if (last_ == Maximum) return Status::overflow("object identity exhausted");
        return ++last_;
    }
  private:
    u64 last_{};
};

// Registrations are retained for the VFS lifetime, including covered mounts and
// unlinked open paths. Keys must outlive the registry and may not be reconstructed
// at the same address. Numbers identify this namespace's live provider instances.
template <usize Capacity = 9, u32 MaximumMinor = ~u32{0}>
class FilesystemIdentities final
{
  public:
    [[nodiscard]] Result<FilesystemDevice> admit(const void *provider)
    {
        if (!provider) return Status::unsupported("filesystem has no identity provider");
        for (const auto &entry : entries_)
            if (entry.provider == provider) return entry.device;
        for (auto &entry : entries_)
            if (!entry.provider)
            {
                auto identity = sequence_.next();
                if (!identity) return identity.status();
                entry = {provider, {0, static_cast<u32>(identity.value())}};
                return entry.device;
            }
        return Status::no_memory("filesystem identity registry full");
    }
    [[nodiscard]] Result<FilesystemDevice> find(const void *provider) const
    {
        if (provider)
            for (const auto &entry : entries_)
                if (entry.provider == provider) return entry.device;
        return Status::not_found("unregistered filesystem identity");
    }
  private:
    struct Entry { const void *provider{}; FilesystemDevice device{}; };
    std::array<Entry, Capacity> entries_{};
    IdentitySequence<MaximumMinor> sequence_{};
};
} // namespace ok::fs
