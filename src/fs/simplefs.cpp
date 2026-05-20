#include "ok/fs/simplefs.hpp"

namespace ok::fs
{
namespace
{

using Block = std::array<std::byte, driver::block_sector_size>;

u16 read_le16(std::span<const std::byte> data, usize offset)
{
    return static_cast<u16>(std::to_integer<u16>(data[offset]) | (std::to_integer<u16>(data[offset + 1]) << 8));
}

u32 read_le32(std::span<const std::byte> data, usize offset)
{
    return static_cast<u32>(read_le16(data, offset)) | (static_cast<u32>(read_le16(data, offset + 2)) << 16);
}

u64 read_le64(std::span<const std::byte> data, usize offset)
{
    return static_cast<u64>(read_le32(data, offset)) | (static_cast<u64>(read_le32(data, offset + 4)) << 32);
}

void write_le16(std::span<std::byte> data, usize offset, u16 value)
{
    data[offset] = static_cast<std::byte>(value & 0xffu);
    data[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void write_le32(std::span<std::byte> data, usize offset, u32 value)
{
    write_le16(data, offset, static_cast<u16>(value & 0xffffu));
    write_le16(data, offset + 2, static_cast<u16>((value >> 16) & 0xffffu));
}

void write_le64(std::span<std::byte> data, usize offset, u64 value)
{
    write_le32(data, offset, static_cast<u32>(value & 0xffff'ffffull));
    write_le32(data, offset + 4, static_cast<u32>((value >> 32) & 0xffff'ffffull));
}

void clear_block(Block &block)
{
    for (auto &byte : block)
    {
        byte = std::byte{0};
    }
}

bool ranges_overlap(u32 left_start, u32 left_count, u32 right_start, u32 right_count)
{
    if (left_count == 0 || right_count == 0)
    {
        return false;
    }
    const auto left_end = left_start + left_count;
    const auto right_end = right_start + right_count;
    return left_start < right_end && right_start < left_end;
}

} // namespace

Status SimpleDiskFileSystem::format(driver::BlockDevice &device, std::string_view label)
{
    const auto geometry = device.geometry();
    if (geometry.block_size != driver::block_sector_size)
    {
        return Status::unsupported("SimpleFS requires 512 byte blocks");
    }
    if (!geometry.writable)
    {
        return Status::denied("block device is not writable");
    }
    if (geometry.block_count <= simplefs_data_start_block)
    {
        return Status::invalid_argument("block device is too small for SimpleFS");
    }
    if (geometry.block_count > 0xffff'ffffull)
    {
        return Status::unsupported("SimpleFS block count exceeds 32 bit on-disk fields");
    }

    device_ = &device;
    mounted_ = true;
    info_ = SimpleFsInfo{};
    info_.block_count = geometry.block_count;
    info_.block_size = geometry.block_size;
    info_.file_count = 0;
    info_.data_start_block = simplefs_data_start_block;
    info_.mounted = true;
    if (auto status = info_.label.assign(label); !status.ok())
    {
        mounted_ = false;
        return status;
    }

    Block block{};
    clear_block(block);
    if (auto status = flush_info(); !status.ok())
    {
        mounted_ = false;
        return status;
    }
    for (u64 table_block = 1; table_block < simplefs_data_start_block; ++table_block)
    {
        if (auto status = device.write_blocks(table_block, std::span<const std::byte>(block.data(), block.size()));
            !status.ok())
        {
            mounted_ = false;
            return status;
        }
    }
    return Status::success();
}

Status SimpleDiskFileSystem::mount(driver::BlockDevice &device)
{
    device_ = &device;
    mounted_ = true;
    if (auto status = load_info(); !status.ok())
    {
        mounted_ = false;
        device_ = nullptr;
        return status;
    }
    info_.mounted = true;
    return Status::success();
}

Result<SimpleFsInfo> SimpleDiskFileSystem::info() const
{
    if (!mounted_)
    {
        return Status::not_initialized("SimpleFS is not mounted");
    }
    return info_;
}

Result<SimpleFsDirectoryListing> SimpleDiskFileSystem::list_root()
{
    if (auto status = ensure_mounted(); !status.ok())
    {
        return status;
    }

    SimpleFsDirectoryListing listing{};
    for (usize i = 0; i < simplefs_max_entries; ++i)
    {
        auto entry = read_entry(i);
        if (!entry)
        {
            return entry.status();
        }
        if (!entry.value().used)
        {
            continue;
        }
        auto &out = listing.entries[listing.count++];
        if (auto status = out.name.assign(entry.value().name.view()); !status.ok())
        {
            return status;
        }
        out.metadata = Metadata{
            .type = entry.value().type,
            .size = entry.value().size,
            .mode = entry.value().type == NodeType::directory ? 0755u : 0644u,
        };
    }
    return listing;
}

Status SimpleDiskFileSystem::create(std::string_view path, NodeType type)
{
    if (auto status = ensure_mounted(); !status.ok())
    {
        return status;
    }
    auto name = normalize_name(path);
    if (!name)
    {
        return name.status();
    }
    auto existing = find_entry(name.value().view());
    if (existing)
    {
        return Status::already_exists("SimpleFS entry already exists");
    }
    if (existing.status().code() != StatusCode::not_found)
    {
        return existing.status();
    }
    auto free_entry = find_free_entry();
    if (!free_entry)
    {
        return free_entry.status();
    }

    DiskEntry entry{};
    entry.used = true;
    entry.type = type;
    if (auto status = entry.name.assign(name.value().view()); !status.ok())
    {
        return status;
    }
    if (auto status = write_entry(free_entry.value(), entry); !status.ok())
    {
        return status;
    }
    ++info_.file_count;
    return flush_info();
}

Status SimpleDiskFileSystem::unlink(std::string_view path)
{
    if (auto status = ensure_mounted(); !status.ok())
    {
        return status;
    }
    auto entry_index = find_entry(path);
    if (!entry_index)
    {
        return entry_index.status();
    }
    DiskEntry empty{};
    if (auto status = write_entry(entry_index.value(), empty); !status.ok())
    {
        return status;
    }
    if (info_.file_count > 0)
    {
        --info_.file_count;
    }
    return flush_info();
}

Status SimpleDiskFileSystem::write_file(std::string_view path, std::span<const std::byte> data)
{
    if (auto status = ensure_mounted(); !status.ok())
    {
        return status;
    }
    if (data.size() > max_file_data)
    {
        return Status::overflow("SimpleFS file exceeds fixed kernel buffer size");
    }
    auto entry_index = find_entry(path);
    if (!entry_index)
    {
        return entry_index.status();
    }
    auto entry = read_entry(entry_index.value());
    if (!entry)
    {
        return entry.status();
    }
    if (entry.value().type != NodeType::regular && entry.value().type != NodeType::device)
    {
        return Status::invalid_argument("SimpleFS entry is not writable");
    }

    const auto required_blocks =
        static_cast<u32>((data.size() + driver::block_sector_size - 1) / driver::block_sector_size);
    u32 start_block = 0;
    if (required_blocks != 0)
    {
        auto allocation = allocate_extent(required_blocks, entry_index.value());
        if (!allocation)
        {
            return allocation.status();
        }
        start_block = allocation.value();

        Block block{};
        usize written = 0;
        for (u32 block_index = 0; block_index < required_blocks; ++block_index)
        {
            clear_block(block);
            const auto remaining = data.size() - written;
            const auto count = remaining < block.size() ? remaining : block.size();
            for (usize i = 0; i < count; ++i)
            {
                block[i] = data[written + i];
            }
            if (auto status = device_->write_blocks(start_block + block_index,
                                                    std::span<const std::byte>(block.data(), block.size()));
                !status.ok())
            {
                return status;
            }
            written += count;
        }
    }

    auto updated = entry.value();
    updated.size = static_cast<u32>(data.size());
    updated.start_block = start_block;
    updated.block_count = required_blocks;
    return write_entry(entry_index.value(), updated);
}

Result<FileBuffer> SimpleDiskFileSystem::read_file(std::string_view path)
{
    if (auto status = ensure_mounted(); !status.ok())
    {
        return status;
    }
    auto entry_index = find_entry(path);
    if (!entry_index)
    {
        return entry_index.status();
    }
    auto entry = read_entry(entry_index.value());
    if (!entry)
    {
        return entry.status();
    }
    if (entry.value().type != NodeType::regular && entry.value().type != NodeType::device)
    {
        return Status::invalid_argument("SimpleFS entry is not readable");
    }
    if (entry.value().size > max_file_data)
    {
        return Status::overflow("SimpleFS file exceeds fixed kernel buffer size");
    }

    FileBuffer out{};
    out.size = entry.value().size;
    Block block{};
    usize copied = 0;
    for (u32 block_index = 0; block_index < entry.value().block_count && copied < out.size; ++block_index)
    {
        if (auto status = device_->read_blocks(entry.value().start_block + block_index,
                                               std::span<std::byte>(block.data(), block.size()));
            !status.ok())
        {
            return status;
        }
        const auto remaining = out.size - copied;
        const auto count = remaining < block.size() ? remaining : block.size();
        for (usize i = 0; i < count; ++i)
        {
            out.data[copied + i] = block[i];
        }
        copied += count;
    }
    return out;
}

Result<Metadata> SimpleDiskFileSystem::stat(std::string_view path)
{
    if (auto status = ensure_mounted(); !status.ok())
    {
        return status;
    }
    auto entry_index = find_entry(path);
    if (!entry_index)
    {
        return entry_index.status();
    }
    auto entry = read_entry(entry_index.value());
    if (!entry)
    {
        return entry.status();
    }
    return Metadata{
        .type = entry.value().type,
        .size = entry.value().size,
        .mode = entry.value().type == NodeType::directory ? 0755u : 0644u,
    };
}

Status SimpleDiskFileSystem::ensure_mounted() const
{
    if (!mounted_ || device_ == nullptr)
    {
        return Status::not_initialized("SimpleFS is not mounted");
    }
    return Status::success();
}

Result<SimpleDiskFileSystem::DiskEntry> SimpleDiskFileSystem::read_entry(usize index)
{
    if (index >= simplefs_max_entries)
    {
        return Status::invalid_argument("SimpleFS entry index out of range");
    }
    Block block{};
    const auto byte_offset = index * simplefs_entry_size;
    const auto block_number = 1 + byte_offset / driver::block_sector_size;
    const auto block_offset = byte_offset % driver::block_sector_size;
    if (auto status =
            device_->read_blocks(block_number, std::span<std::byte>(block.data(), block.size()));
        !status.ok())
    {
        return status;
    }

    auto bytes = std::span<const std::byte>(block.data(), block.size());
    DiskEntry entry{};
    entry.used = std::to_integer<u8>(bytes[block_offset]) != 0;
    entry.type = static_cast<NodeType>(std::to_integer<u8>(bytes[block_offset + 1]));
    entry.size = read_le32(bytes, block_offset + 4);
    entry.start_block = read_le32(bytes, block_offset + 8);
    entry.block_count = read_le32(bytes, block_offset + 12);

    const auto *name = reinterpret_cast<const char *>(block.data() + block_offset + 16);
    usize name_size = 0;
    while (name_size + 1 < simplefs_name_capacity && name[name_size] != '\0')
    {
        ++name_size;
    }
    if (auto status = entry.name.assign(std::string_view{name, name_size}); !status.ok())
    {
        return status;
    }
    return entry;
}

Status SimpleDiskFileSystem::write_entry(usize index, const DiskEntry &entry)
{
    if (index >= simplefs_max_entries)
    {
        return Status::invalid_argument("SimpleFS entry index out of range");
    }
    Block block{};
    const auto byte_offset = index * simplefs_entry_size;
    const auto block_number = 1 + byte_offset / driver::block_sector_size;
    const auto block_offset = byte_offset % driver::block_sector_size;
    if (auto status = device_->read_blocks(block_number, std::span<std::byte>(block.data(), block.size()));
        !status.ok())
    {
        return status;
    }

    for (usize i = 0; i < simplefs_entry_size; ++i)
    {
        block[block_offset + i] = std::byte{0};
    }
    block[block_offset] = entry.used ? std::byte{1} : std::byte{0};
    block[block_offset + 1] = static_cast<std::byte>(static_cast<u8>(entry.type));
    write_le32(std::span<std::byte>(block.data(), block.size()), block_offset + 4, entry.size);
    write_le32(std::span<std::byte>(block.data(), block.size()), block_offset + 8, entry.start_block);
    write_le32(std::span<std::byte>(block.data(), block.size()), block_offset + 12, entry.block_count);
    const auto name = entry.name.view();
    for (usize i = 0; i < name.size(); ++i)
    {
        block[block_offset + 16 + i] = static_cast<std::byte>(name[i]);
    }
    return device_->write_blocks(block_number, std::span<const std::byte>(block.data(), block.size()));
}

Result<usize> SimpleDiskFileSystem::find_entry(std::string_view path)
{
    auto name = normalize_name(path);
    if (!name)
    {
        return name.status();
    }
    for (usize i = 0; i < simplefs_max_entries; ++i)
    {
        auto entry = read_entry(i);
        if (!entry)
        {
            return entry.status();
        }
        if (entry.value().used && entry.value().name.view() == name.value().view())
        {
            return i;
        }
    }
    return Status::not_found("SimpleFS entry not found");
}

Result<usize> SimpleDiskFileSystem::find_free_entry()
{
    for (usize i = 0; i < simplefs_max_entries; ++i)
    {
        auto entry = read_entry(i);
        if (!entry)
        {
            return entry.status();
        }
        if (!entry.value().used)
        {
            return i;
        }
    }
    return Status::overflow("SimpleFS directory table is full");
}

Result<u32> SimpleDiskFileSystem::allocate_extent(u32 required_blocks, usize ignored_entry)
{
    if (required_blocks == 0)
    {
        return 0u;
    }
    for (u32 candidate = simplefs_data_start_block; candidate + required_blocks <= info_.block_count; ++candidate)
    {
        bool collision = false;
        for (usize i = 0; i < simplefs_max_entries; ++i)
        {
            if (i == ignored_entry)
            {
                continue;
            }
            auto entry = read_entry(i);
            if (!entry)
            {
                return entry.status();
            }
            if (entry.value().used &&
                ranges_overlap(candidate, required_blocks, entry.value().start_block, entry.value().block_count))
            {
                collision = true;
                break;
            }
        }
        if (!collision)
        {
            return candidate;
        }
    }
    return Status::overflow("SimpleFS has no contiguous free extent");
}

Result<FixedString<simplefs_name_capacity>> SimpleDiskFileSystem::normalize_name(std::string_view path) const
{
    if (path.empty() || path == "/")
    {
        return Status::invalid_argument("SimpleFS path must name a root entry");
    }
    if (path.front() == '/')
    {
        path.remove_prefix(1);
    }
    if (path.empty())
    {
        return Status::invalid_argument("SimpleFS path must name a root entry");
    }
    for (const auto value : path)
    {
        if (value == '/')
        {
            return Status::unsupported("SimpleFS currently supports a flat root directory");
        }
    }
    FixedString<simplefs_name_capacity> name{};
    if (auto status = name.assign(path); !status.ok())
    {
        return status;
    }
    return name;
}

Status SimpleDiskFileSystem::flush_info()
{
    if (auto status = ensure_mounted(); !status.ok())
    {
        return status;
    }
    Block block{};
    clear_block(block);
    auto bytes = std::span<std::byte>(block.data(), block.size());
    write_le32(bytes, 0, simplefs_magic);
    write_le16(bytes, 4, simplefs_version);
    write_le16(bytes, 6, static_cast<u16>(info_.block_size));
    write_le64(bytes, 8, info_.block_count);
    write_le32(bytes, 16, info_.file_count);
    write_le32(bytes, 20, info_.data_start_block);
    const auto label = info_.label.view();
    for (usize i = 0; i < label.size(); ++i)
    {
        block[24 + i] = static_cast<std::byte>(label[i]);
    }
    return device_->write_blocks(0, std::span<const std::byte>(block.data(), block.size()));
}

Status SimpleDiskFileSystem::load_info()
{
    if (auto status = ensure_mounted(); !status.ok())
    {
        return status;
    }
    Block block{};
    if (auto status = device_->read_blocks(0, std::span<std::byte>(block.data(), block.size())); !status.ok())
    {
        return status;
    }
    auto bytes = std::span<const std::byte>(block.data(), block.size());
    if (read_le32(bytes, 0) != simplefs_magic)
    {
        return Status::invalid_argument("SimpleFS magic mismatch");
    }
    if (read_le16(bytes, 4) != simplefs_version)
    {
        return Status::unsupported("SimpleFS version mismatch");
    }
    if (read_le16(bytes, 6) != driver::block_sector_size)
    {
        return Status::unsupported("SimpleFS block size mismatch");
    }

    const auto geometry = device_->geometry();
    info_ = SimpleFsInfo{};
    info_.block_size = read_le16(bytes, 6);
    info_.block_count = read_le64(bytes, 8);
    info_.file_count = read_le32(bytes, 16);
    info_.data_start_block = read_le32(bytes, 20);
    if (info_.block_count > geometry.block_count || info_.data_start_block != simplefs_data_start_block)
    {
        return Status::invalid_argument("SimpleFS superblock does not match block device");
    }

    const auto *label = reinterpret_cast<const char *>(block.data() + 24);
    usize label_size = 0;
    while (label_size + 1 < simplefs_label_capacity && label[label_size] != '\0')
    {
        ++label_size;
    }
    if (auto status = info_.label.assign(std::string_view{label, label_size}); !status.ok())
    {
        return status;
    }
    info_.mounted = true;
    return Status::success();
}

} // namespace ok::fs
