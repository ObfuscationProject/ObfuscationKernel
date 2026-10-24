#pragma once

#include "ok/driver/driver.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/fs/ext4_times.hpp"
#include "ok/time/realtime_clock.hpp"

#include <type_traits>

namespace ok::fs
{
enum class Ext4TimestampPolicy : u8 { unqualified, maintained };
struct Ext4MountOptions
{
    bool writable{true};
    Ext4TimestampPolicy timestamps{Ext4TimestampPolicy::unqualified};
};

class Ext4FileSystem;
class Ext4Node final : public Node
{
  public:
    Ext4Node() = default;
    Ext4Node(const Ext4Node &) = delete;
    Ext4Node &operator=(const Ext4Node &) = delete;
    Ext4Node(Ext4Node &&) = delete;
    Ext4Node &operator=(Ext4Node &&) = delete;
    std::string_view name() const override
    {
        return name_.view();
    }
    Metadata metadata() const override;
    Result<Metadata> query_metadata() const override;
    Result<Node *> lookup_child(std::string_view child) override;
    Result<Node *> lookup_child_retained(std::string_view child) override;
    Result<FileBuffer> read(usize offset, usize count) const override;
    Status write(usize offset, std::span<const std::byte> data) override;
    Status truncate(usize size) override;
    Status sync() override;
    Status retain() override;
    Status release() override;
    Status expose_borrowed_identity() override;
    const void *filesystem_identity() const override
    {
        return fs_;
    }
    Status link(std::string_view name, Node &target) override;
    Status rename(std::string_view old_name, Node &new_parent, std::string_view new_name) override;
    Result<FixedString<96>> directory_path(const Node &root) const override;
    Node *lookup(std::string_view child) override;
    Status create(std::string_view name, NodeType type) override;
    Result<Node *> create_child(std::string_view name, const InitialNodeAttributes &attributes) override;
    Result<Node *> create_child_retained(std::string_view name, const InitialNodeAttributes &attributes) override;
    Status create_symlink(std::string_view name, std::string_view target,
                          u32 uid = default_uid, u32 gid = default_gid) override;
    Status remove(std::string_view name, bool directory) override;
    Status chmod(u32 mode) override;
    ModeChangeResult change_mode(u32 mode, user::Credentials caller) override;
    Status chown(u32 uid, u32 gid) override;
    Result<DirectoryListing> list(DirectoryReadIntent = DirectoryReadIntent::access) const override;

  private:
    friend class Ext4FileSystem;
    enum class Phase : u8 { vacant, linked, detached, retiring, tombstone };
    Result<Metadata> query_metadata_active() const;
    Result<FileBuffer> read_active(usize offset, usize count) const;
    Status write_active(usize offset, std::span<const std::byte> data);
    Status truncate_active(usize size);
    Result<Node *> lookup_active(std::string_view child, bool retained, bool exposed);
    Result<Node *> create_active(std::string_view name, const InitialNodeAttributes &attributes, bool retained);
    Status create_symlink_active(std::string_view name, std::string_view target, u32 uid, u32 gid);
    Status link_active(std::string_view name, Node &target);
    Status rename_active(std::string_view old_name, Node &new_parent, std::string_view new_name);
    Result<FixedString<96>> directory_path_active(const Node &root) const;
    Status remove_active(std::string_view name, bool directory);
    Status chmod_active(u32 mode);
    ModeChangeResult change_mode_active(u32 mode, user::Credentials caller);
    Status chown_active(u32 uid, u32 gid);
    Result<DirectoryListing> list_active(DirectoryReadIntent intent) const;
    Status retain_locked();
    Status expose_locked();
    Ext4FileSystem *fs_{nullptr};
    u32 ino_{0};
    u32 references_{0};
    Phase phase_{Phase::vacant};
    bool borrowed_identity_exposed_{false};
    FixedString<max_path_segment> name_{};
};

// Deliberately bounded, non-journalled ext4 writer. Unsupported on-disk features
// are rejected before any write. Ordinary file mutations require inline extents
// (at most four runs); symlinks use inline bytes or one block. Filesystem blocks
// are 1/2/4 KiB with 32-bit block numbers.
class Ext4FileSystem final
{
  public:
    Status mount(driver::BlockDevice &device, bool writable = true);
    Status mount(driver::BlockDevice &device, Ext4MountOptions options);
    [[nodiscard]] Node *root();
    Status sync();

  private:
    friend class Ext4Node;
    Status begin_operation(const Ext4Node *node, bool mounting = false);
    Status finish_operation();
    Status drain_detached();
    void detach(Ext4Node &node);
    [[nodiscard]] bool owns(const Node &node) const;
    template <typename Action>
    auto operation(const Ext4Node *node, Action &&action, bool mounting = false,
                   bool retained_result = false) -> decltype(action())
    {
        using Value = decltype(action());
        const auto failure = [](Status status) -> Value {
            if constexpr (std::is_same_v<Value, ModeChangeResult>) return ModeChangeResult::failure(status);
            else return Value{status};
        };
        if (auto status = begin_operation(node, mounting); !status) return failure(status);
        auto value = action();
        const auto status = finish_operation();
        const bool succeeded = [&]() {
            if constexpr (std::is_same_v<Value, ModeChangeResult>)
                return value.kind() != ModeChangeResult::Kind::failed;
            else return static_cast<bool>(value);
        }();
        if (!status && succeeded)
        {
            if constexpr (std::is_same_v<Value, Result<Node *>>)
                if (retained_result && value.value()) static_cast<void>(value.value()->release());
            return failure(status);
        }
        return value;
    }
    Status mount_active(driver::BlockDevice &device, Ext4MountOptions options);
    Status sync_active();
    using Block = std::array<std::byte, 4096>;
    using Inode = std::array<std::byte, 256>;
    struct TimestampEvent
    {
        time::RealtimeSample sample{};
        bool active{false};
    };
    struct TimestampTarget
    {
        Inode *record;
        Ext4TimestampFields fields;
        bool initialized{false};
    };
    [[nodiscard]] bool maintained() const { return timestamps_ == Ext4TimestampPolicy::maintained; }
    Status validate_times(const Inode &record) const;
    Status prepare_event(TimestampEvent &event, std::span<const TimestampTarget> targets);
    Status apply_event(const TimestampEvent &event, Inode &record, Ext4TimestampFields fields) const;
    Status apply_mode(u32 ino, Inode &record, u32 mode);
    Status access_event(u32 ino, Inode &record);
    Result<u32> available_number(bool inode, u32 after = 0) const;
    Result<u32> preflight_insert(const Inode &directory, std::string_view name, u32 after = 0);
    Status preflight_write(const Inode &record, usize offset, usize count);
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
    Status insert(u32 parent, Inode &directory, std::string_view name, u32 ino, NodeType type);
    Status replace_entry(const Inode &directory, std::string_view name, u32 ino, NodeType type);
    Result<bool> empty_directory(const Inode &directory) const;
    Result<FixedString<max_path_segment>> child_name(const Inode &directory, u32 ino) const;
    Status validate_move(u32 source, u32 destination_parent) const;
    struct NamespaceMutation
    {
        explicit NamespaceMutation(Ext4FileSystem &value, bool enabled = true)
            : fs(value), writes(value.write_sequence_), active(enabled)
        {
        }
        ~NamespaceMutation()
        {
            if (active && !committed && fs.write_sequence_ != writes)
                fs.failed_ = true;
        }
        Ext4FileSystem &fs;
        u64 writes;
        bool active;
        bool committed{false};
    };
    Result<u32> symlink_block(const Inode &inode) const;
    Result<FileBuffer> read_symlink(const Inode &inode, usize offset, usize count) const;
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
    Ext4TimestampPolicy timestamps_{Ext4TimestampPolicy::unqualified};
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
    mutable smp::SpinLock identity_lock_{};
    bool operation_active_{false};
};
} // namespace ok::fs
