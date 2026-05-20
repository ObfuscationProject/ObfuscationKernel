#pragma once

#include "ok/core/fixed.hpp"
#include "ok/driver/driver.hpp"
#include "ok/fs/vfs.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace ok::fs
{

inline constexpr u32 simplefs_magic = 0x53464b4fu;
inline constexpr u16 simplefs_version = 1;
inline constexpr usize simplefs_label_capacity = 17;
inline constexpr usize simplefs_name_capacity = max_path_segment;
inline constexpr usize simplefs_max_entries = 32;
inline constexpr usize simplefs_entry_size = 64;
inline constexpr usize simplefs_table_blocks = 4;
inline constexpr usize simplefs_data_start_block = 1 + simplefs_table_blocks;

struct SimpleFsInfo
{
    FixedString<simplefs_label_capacity> label{};
    u32 block_size{driver::block_sector_size};
    u64 block_count{0};
    u32 file_count{0};
    u32 data_start_block{simplefs_data_start_block};
    bool mounted{false};
};

struct SimpleFsEntry
{
    FixedString<simplefs_name_capacity> name{};
    Metadata metadata{};
};

struct SimpleFsDirectoryListing
{
    std::array<SimpleFsEntry, simplefs_max_entries> entries{};
    usize count{0};
};

class SimpleDiskFileSystem final
{
  public:
    Status format(driver::BlockDevice &device, std::string_view label);
    Status mount(driver::BlockDevice &device);
    [[nodiscard]] Result<SimpleFsInfo> info() const;
    [[nodiscard]] Result<SimpleFsDirectoryListing> list_root();
    Status create(std::string_view path, NodeType type);
    Status unlink(std::string_view path);
    Status write_file(std::string_view path, std::span<const std::byte> data);
    [[nodiscard]] Result<FileBuffer> read_file(std::string_view path);
    [[nodiscard]] Result<Metadata> stat(std::string_view path);
    [[nodiscard]] bool mounted() const
    {
        return mounted_;
    }

  private:
    struct DiskEntry
    {
        bool used{false};
        NodeType type{NodeType::regular};
        u32 size{0};
        u32 start_block{0};
        u32 block_count{0};
        FixedString<simplefs_name_capacity> name{};
    };

    [[nodiscard]] Status ensure_mounted() const;
    [[nodiscard]] Result<DiskEntry> read_entry(usize index);
    Status write_entry(usize index, const DiskEntry &entry);
    [[nodiscard]] Result<usize> find_entry(std::string_view path);
    [[nodiscard]] Result<usize> find_free_entry();
    [[nodiscard]] Result<u32> allocate_extent(u32 required_blocks, usize ignored_entry);
    [[nodiscard]] Result<FixedString<simplefs_name_capacity>> normalize_name(std::string_view path) const;
    Status flush_info();
    Status load_info();

    driver::BlockDevice *device_{nullptr};
    SimpleFsInfo info_{};
    bool mounted_{false};
};

} // namespace ok::fs
