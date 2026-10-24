#pragma once
#include "ok/fs/archive_directory_profile.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/user/user.hpp"

namespace ok::fs
{
// A bounded, little-endian boot archive. The linked blob must outlive the VFS.
// Header: "OKAR", u32 version, u32 count, u32 reserved.
// Each record: u32 path_bytes, u32 data_bytes, u32 mode, u32 reserved;
// absolute path bytes, data bytes, zero padding to the next 8-byte boundary.
// Ownership is guest root. Directories retain sticky/SGID bits; regular files
// have no special bits. Import failures do not roll back earlier valid records.
Status mount_archive(VirtualFileSystem &vfs, std::span<const std::byte> archive);
struct BootArchive
{
    const std::byte *data{};
    usize size{};
    const char *initial_program{};
    user::Credentials initial_credentials{user::root_credentials()};
};
} // namespace ok::fs
