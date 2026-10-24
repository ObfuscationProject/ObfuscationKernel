#pragma once

#include "ok/fs/jbd2_replay.hpp"

namespace ok::fs
{

struct Ext4RecoveryBounds
{
    u64 first_sector{};
    u64 sector_count{};
};

// Admission facts are neither a mount capability nor a full allocation fsck.
// The metadata domain excludes directory data and external extent-tree blocks.
struct Ext4RecoveryInfo
{
    static constexpr usize group_capacity = 32;
    jbd2::RecoveryLayout layout{};
    std::array<u8, 16> uuid{};
    std::array<jbd2::BlockRange, group_capacity * 4> metadata{};
    usize metadata_count{};
    u32 journal_inode{};
    u32 journal_sequence{};
    bool needs_recovery{};
    bool empty_log{};

    [[nodiscard]] jbd2::TargetDomain structural_metadata() const
    {
        if (metadata_count > metadata.size())
            return {};
        return {layout.filesystem_blocks, std::span<const jbd2::BlockRange>{metadata}.first(metadata_count)};
    }
};

// One serialized, read-only admission attempt. The owner must exclude device
// replacement, filesystem caches and other writers until these facts retire.
// No writes/flushes, allocation, locks, journal replay or mount publication occur.
// Supported: Linux ext4, 1/2/4 KiB blocks, 32-bit addresses, 256-byte inodes,
// metadata_csum, internal contiguous extent journal, synchronous JBD2 CRC32C v3.
class Ext4RecoveryBinding final
{
    template <usize ActionCapacity, usize RevokeCapacity>
        requires (ActionCapacity > 0 && RevokeCapacity > 0)
    friend class Ext4ReplayPreview;
  public:
    Ext4RecoveryBinding() = default;
    Ext4RecoveryBinding(const Ext4RecoveryBinding &) = delete;
    Ext4RecoveryBinding &operator=(const Ext4RecoveryBinding &) = delete;
    Ext4RecoveryBinding(Ext4RecoveryBinding &&) = delete;
    Ext4RecoveryBinding &operator=(Ext4RecoveryBinding &&) = delete;

    [[nodiscard]] Status inspect(driver::BlockDevice &device, Ext4RecoveryBounds bounds);
    [[nodiscard]] const Ext4RecoveryInfo *info() const { return admitted_ ? &info_ : nullptr; }

  private:
    [[nodiscard]] Status inspect_impl(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                      bool create_candidate, bool first_data_candidate);
    [[nodiscard]] Status inspect_create_candidate(driver::BlockDevice &device,
                                                   Ext4RecoveryBounds bounds)
    { return inspect_impl(device, bounds, true, false); }
    [[nodiscard]] Status inspect_first_data_candidate(driver::BlockDevice &device,
                                                       Ext4RecoveryBounds bounds)
    { return inspect_impl(device, bounds, false, true); }
    // Both provisional bitmap exceptions remain private to the complete
    // directory-create witness proof; they never grant replay authority alone.
    [[nodiscard]] Status inspect_directory_create_candidate(driver::BlockDevice &device,
                                                             Ext4RecoveryBounds bounds)
    { return inspect_impl(device, bounds, true, true); }
    struct Group
    {
        u32 begin{}, end{}, block_bitmap{}, inode_bitmap{}, table{}, table_blocks{};
        u16 block_checksum{}, inode_checksum{}, flags{};
    };
    [[nodiscard]] Status admit();
    [[nodiscard]] Status stable() const;
    [[nodiscard]] Status read_sectors(u64 relative, std::span<u8> out);
    [[nodiscard]] Status read_block(u32 block);
    [[nodiscard]] Status add_range(u64 first, u64 count);
    [[nodiscard]] Status groups();
    [[nodiscard]] Status journal();
    [[nodiscard]] Status allocation();

    bool attempted_{}, admitted_{};
    bool create_candidate_{};
    usize create_bitmap_mismatches_{};
    u32 create_bitmap_mismatch_group_{};
    bool first_data_candidate_{};
    usize first_data_bitmap_mismatches_{};
    u32 first_data_bitmap_mismatch_group_{};
    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    Ext4RecoveryBounds bounds_{};
    Ext4RecoveryInfo info_{};
    u32 seed_{}, first_data_{}, blocks_per_group_{}, inodes_per_group_{}, inode_count_{}, group_count_{};
    std::array<Group, Ext4RecoveryInfo::group_capacity> groups_{};
    std::array<u8, 1024> super_{};
    std::array<u8, 1024> descriptors_{};
    std::array<u8, 256> journal_inode_{};
    std::array<u8, 4096> block_{};
};

} // namespace ok::fs
