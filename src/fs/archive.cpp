#include "ok/fs/archive.hpp"

namespace ok::fs
{
namespace
{
u32 read32(std::span<const std::byte> bytes, usize offset)
{
    u32 result = 0;
    for (usize i = 0; i < 4; ++i)
        result |= static_cast<u32>(bytes[offset + i]) << (i * 8);
    return result;
}
bool valid_path(std::string_view path)
{
    if (path.size() < 2 || path.size() >= 128 || path.front() != '/' || path.back() == '/')
        return false;
    usize start = 1;
    for (usize i = 1; i <= path.size(); ++i)
    {
        if (i < path.size() && path[i] == '\0')
            return false;
        if (i == path.size() || path[i] == '/')
        {
            auto component = path.substr(start, i - start);
            if (component.empty() || component == "." || component == ".." || component.size() >= max_path_segment)
                return false;
            start = i + 1;
        }
    }
    return true;
}
} // namespace
Status mount_archive(VirtualFileSystem &vfs, std::span<const std::byte> archive)
{
    if (archive.size() < 16 || read32(archive, 0) != 0x52414b4fu || read32(archive, 4) != 1 ||
        read32(archive, 12) != 0 || read32(archive, 8) > max_ram_nodes - 3)
        return Status::invalid_argument("invalid boot archive header");
    const auto count = read32(archive, 8);
    // Validate the entire input before exposing any of its files.
    for (unsigned pass = 0; pass < 2; ++pass)
    {
        usize offset = 16;
        for (u32 record = 0; record < count; ++record)
        {
            if (offset > archive.size() || archive.size() - offset < 16)
                return Status::invalid_argument("truncated archive record");
            const usize path_size = read32(archive, offset);
            const usize data_size = read32(archive, offset + 4);
            const u32 mode = read32(archive, offset + 8);
            const auto kind = mode & mode_type_mask;
            const u32 permissions = kind == mode_directory ? boot_archive_directory_mode_mask
                                                           : boot_archive_regular_mode_mask;
            if (read32(archive, offset + 12) != 0 || (kind != mode_directory && kind != mode_regular) ||
                (mode & ~(mode_type_mask | permissions)) || (kind == mode_directory && data_size != 0))
                return Status::invalid_argument("invalid archive record type");
            offset += 16;
            if (path_size > archive.size() - offset)
                return Status::invalid_argument("truncated archive path");
            std::string_view path(reinterpret_cast<const char *>(archive.data() + offset), path_size);
            if (!valid_path(path))
                return Status::invalid_argument("invalid archive path");
            offset += path_size;
            if (data_size > archive.size() - offset)
                return Status::invalid_argument("truncated archive contents");
            if (pass == 1)
            {
                if (kind == mode_directory)
                {
                    if (auto status = vfs.import_archive_directory(path, mode & permissions); !status.ok())
                        return status;
                }
                else if (auto status = vfs.create_readonly_file(path, archive.subspan(offset, data_size), mode, true);
                         !status.ok())
                    return status;
            }
            offset += data_size;
            const usize padding = (8 - (offset & 7)) & 7;
            if (padding > archive.size() - offset)
                return Status::invalid_argument("truncated archive padding");
            for (usize i = 0; i < padding; ++i)
                if (archive[offset + i] != std::byte{})
                    return Status::invalid_argument("nonzero archive padding");
            offset += padding;
        }
        if (offset != archive.size())
            return Status::invalid_argument("trailing archive contents");
    }
    return Status::success();
}
} // namespace ok::fs
