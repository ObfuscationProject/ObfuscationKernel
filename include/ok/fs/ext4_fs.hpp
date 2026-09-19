#pragma once

#include "ok/driver/driver.hpp"
#include "ok/fs/vfs.hpp"

namespace ok::fs
{
class Ext4FileSystem;
class Ext4Node final : public Node
{
  public:
    std::string_view name() const override
    {
        return name_.view();
    }
    Metadata metadata() const override;
    Result<FileBuffer> read(usize offset, usize count) const override;
    Status write(usize offset, std::span<const std::byte> data) override;
    Status truncate(usize size) override;
    Status sync() override;
    Status retain() override;
    Status release() override;
    const void *filesystem_identity() const override
    {
        return fs_;
    }
    Status link(std::string_view name, Node &target) override;
    Status rename(std::string_view old_name, Node &new_parent, std::string_view new_name) override;
    Result<FixedString<96>> directory_path(const Node &root) const override;
    Node *lookup(std::string_view child) override;
    Status create(std::string_view name, NodeType type) override;
    Status remove(std::string_view name, bool directory) override;
    Status chmod(u32 mode) override;
    Status chown(u32 uid, u32 gid) override;
    Result<DirectoryListing> list() const override;

  private:
    friend class Ext4FileSystem;
    Ext4FileSystem *fs_{nullptr};
    u32 ino_{0};
    u32 references_{0};
    FixedString<max_path_segment> name_{};
};

// Deliberately bounded, non-journalled ext4 writer. Unsupported on-disk features
// are rejected before any write. Reads traverse extent trees; mutations require
// inline extents (at most four runs), 1/2/4 KiB blocks, and 32-bit block numbers.
class Ext4FileSystem final
{
  public:
    Status mount(driver::BlockDevice &device, bool writable = true);
    [[nodiscard]] Node *root()
    {
        return mounted_ ? &nodes_[0] : nullptr;
    }
    Status sync();

  private:
    friend class Ext4Node;
    using Block = std::array<std::byte, 4096>;
    using Inode = std::array<std::byte, 256>;
    struct Location
    {
        u32 block;
        u16 offset;
        u16 length;
    };
    Status io_status(Status status) const;
    [[nodiscard]] bool data_block(u32 block) const;
    [[nodiscard]] bool data_range(u32 first, u32 count) const;
    void restore_dirty_marker(std::array<std::byte, 1024> &super);
    Status read_block(u32 block, Block &out) const;
    Status write_block(u32 block, const Block &data);
    Result<Inode> inode(u32 ino) const;
    Status save_inode(u32 ino, const Inode &data);
    Result<u32> map(const Inode &inode, u32 logical) const;
    Status append(Inode &inode, u32 logical, u32 physical);
    Result<u32> allocate(bool inode, bool directory = false);
    Status free_number(u32 number, bool inode, bool directory = false);
    Status counts(u32 group, bool inode, int delta, bool directory);
    Result<Location> find(const Inode &directory, std::string_view name) const;
    Result<u32> child(const Inode &directory, std::string_view name) const;
    Status insert(u32 parent, Inode &directory, std::string_view name, u32 ino, bool dir);
    Status replace_entry(const Inode &directory, std::string_view name, u32 ino, bool is_directory);
    Result<bool> empty_directory(const Inode &directory) const;
    Result<FixedString<max_path_segment>> child_name(const Inode &directory, u32 ino) const;
    Status validate_move(u32 source, u32 destination_parent) const;
    struct NamespaceMutation
    {
        explicit NamespaceMutation(Ext4FileSystem &value) : fs(value), writes(value.write_sequence_)
        {
        }
        ~NamespaceMutation()
        {
            if (!committed && fs.write_sequence_ != writes)
                fs.failed_ = true;
        }
        Ext4FileSystem &fs;
        u64 writes;
        bool committed{false};
    };
    Status zero_tail(const Inode &inode);
    Status resize(u32 ino, Inode &inode, usize size);
    Status destroy(u32 ino);
    Ext4Node *node(u32 ino, std::string_view name);
    Metadata metadata(const Inode &inode) const;
    Status writable(const Inode &inode) const;
    driver::BlockDevice *device_{nullptr};
    u64 write_sequence_{0};
    bool mounted_{false};
    bool writable_{false};
    bool dirty_{false};
    mutable bool failed_{false};
    u32 block_size_{0}, blocks_{0}, inodes_{0}, first_data_{0};
    u32 blocks_per_group_{0}, inodes_per_group_{0}, groups_{0}, first_inode_{0};
    u16 inode_size_{0};
    struct Group
    {
        u32 begin{}, end{}, block_bitmap{}, inode_bitmap{}, table{}, table_blocks{};
        bool superblock{};
    };
    std::array<Group, 32> group_layout_{};
    std::array<Ext4Node, 128> nodes_{};
};
} // namespace ok::fs
