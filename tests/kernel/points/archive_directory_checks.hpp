#pragma once

#include "ok/fs/archive.hpp"

namespace ok::tests
{
// Kernel boot model only; native guest authorization is qualified separately.
inline Status check_archive_directory_metadata_model(fs::VirtualFileSystem &vfs)
{
    std::array<std::byte, 128> archive{};
    const auto write32 = [&](usize offset, u32 value) {
        for (usize index = 0; index < 4; ++index)
            archive[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xffu);
    };
    write32(0, 0x52414b4fu);
    write32(4, 1);
    write32(8, 3);
    usize size = 16;
    const auto append = [&](std::string_view path, u32 mode) {
        write32(size, static_cast<u32>(path.size()));
        write32(size + 8, fs::mode_directory | mode);
        size += 16;
        for (const char character : path)
            archive[size++] = static_cast<std::byte>(character);
        size = (size + 7) & ~usize{7};
    };
    append("/tmp", 01777);
    append("/archive-group", 02770);
    append("/archive-group/plain", 0750);
    const auto bytes = std::span<const std::byte>(archive.data(), size);
    if (auto status = fs::mount_archive(vfs, bytes); !status) return status;
    const auto exact = [&](std::string_view path, u32 mode) {
        const auto metadata = vfs.stat(path);
        return metadata && metadata.value().type == fs::NodeType::directory &&
            metadata.value().mode == (fs::mode_directory | mode) &&
            metadata.value().uid == 0 && metadata.value().gid == 0;
    };
    if (!exact("/tmp", 01777) || !exact("/archive-group", 02770) ||
        !exact("/archive-group/plain", 0750))
        return Status::fault("archive directory root metadata or exact SGID mode failed");
    if (auto status = fs::mount_archive(vfs, bytes); !status) return status;

    // Reuse the first record alone to attempt weakening the seeded sticky /tmp.
    write32(8, 1);
    write32(24, fs::mode_directory | 0777);
    if (fs::mount_archive(vfs, std::span<const std::byte>(archive.data(), 40)).code() !=
            StatusCode::already_exists || !exact("/tmp", 01777))
        return Status::fault("archive directory collision weakened sticky metadata");
    return {};
}
} // namespace ok::tests
