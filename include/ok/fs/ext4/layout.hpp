#pragma once

// ext4 layout module: ext4 superblock and inode layout, timestamps, block accounting, the
// checked read-only view and the VFS filesystem.

#include "ok/fs/ext4/journal.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/driver/driver.hpp"
#include "ok/fs/attributes.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/time/realtime_clock.hpp"
#include <array>
#include <cstddef>
#include <span>
#include <bit>
#include <type_traits>

namespace ok::fs
{

inline constexpr usize ext4_superblock_offset = 1024;
inline constexpr u16 ext4_superblock_magic = 0xef53;
inline constexpr usize ext4_volume_name_capacity = 17;

struct Ext4MountInfo
{
    u32 inode_count{0};
    u32 block_count_low{0};
    u32 free_block_count_low{0};
    u32 block_size{1024};
    u16 inode_size{128};
    bool has_extents{false};
    FixedString<ext4_volume_name_capacity> volume_name{};
};

class Ext4Volume final
{
  public:
    Status mount(std::span<const std::byte> image);
    Status mount(driver::BlockDevice &device);
    [[nodiscard]] Result<Ext4MountInfo> info() const;
    Status read_block(u64 block, std::span<std::byte> out) const;
    [[nodiscard]] bool mounted() const
    {
        return mounted_;
    }

  private:
    Status parse_superblock(std::span<const std::byte> superblock);

    bool mounted_{false};
    std::span<const std::byte> image_{};
    driver::BlockDevice *device_{nullptr};
    Ext4MountInfo info_{};
    std::array<std::byte, 1024> superblock_{};
};


// Decode only stored epoch fields. The caller must establish read-only inode
// provenance; this helper does not turn a writable provider into a clock source.
inline Status decode_ext4_times(std::span<const std::byte> inode, FileAttributes &output)
{
    if (inode.size() != 128 && inode.size() != 256)
        return Status::invalid_argument("unsupported ext4 timestamp inode size");
    auto le16 = [&](usize at) -> u16 {
        return std::to_integer<u16>(inode[at]) | (std::to_integer<u16>(inode[at + 1]) << 8);
    };
    auto le32 = [&](usize at) -> u32 { return le16(at) | (static_cast<u32>(le16(at + 2)) << 16); };
    const usize extra_size = inode.size() == 256 ? le16(128) : 0;
    if (extra_size > inode.size() - 128 || (extra_size & 3))
        return Status::io_error("invalid ext4 extra inode extent");
    auto decode = [&](usize base, usize extra) -> Result<EpochTimestamp> {
        const i64 seconds = std::bit_cast<i32>(le32(base));
        const u32 extension = extra + 4 <= 128 + extra_size ? le32(extra) : 0;
        const u32 nanos = extension >> 2;
        if (nanos >= 1000000000) return Status::io_error("invalid ext4 timestamp nanoseconds");
        return EpochTimestamp{seconds + static_cast<i64>(u64{extension & 3} << 32),
                              nanos, TimestampSource::readonly_ext4};
    };
    auto access = decode(8, 140), modification = decode(16, 136), change = decode(12, 132);
    if (!access) return access.status();
    if (!modification) return modification.status();
    if (!change) return change.status();
    auto result = output;
    result.access = access.value(); result.modification = modification.value(); result.change = change.value();
    result.mark(Attribute::access_time); result.mark(Attribute::modification_time); result.mark(Attribute::change_time);
    output = result;
    return {};
}
enum class Ext4TimestampFields : u32
{
    access = 1,
    modification = 2,
    change = 4,
    all = 7,
};

[[nodiscard]] constexpr Ext4TimestampFields operator|(Ext4TimestampFields one, Ext4TimestampFields two)
{
    return static_cast<Ext4TimestampFields>(static_cast<u32>(one) | static_cast<u32>(two));
}

// Format-only, selected-field update. The caller owns event policy, source
// qualification and any explicit initialization of a newly allocated inode.
// All checks precede writes; unknown, unselected and undeclared bytes survive.
inline Status encode_ext4_times(std::span<std::byte> inode, Ext4TimestampFields fields,
                                i64 seconds, u32 nanoseconds)
{
    if (inode.size() != 128 && inode.size() != 256)
        return Status::invalid_argument("unsupported ext4 timestamp inode size");
    const auto mask = static_cast<u32>(fields);
    if (!mask || (mask & ~u32{7}))
        return Status::invalid_argument("invalid ext4 timestamp field mask");
    if (nanoseconds >= 1000000000)
        return Status::invalid_argument("invalid ext4 timestamp nanoseconds");
    const usize extra_size = inode.size() == 256
        ? std::to_integer<u16>(inode[128]) | (std::to_integer<u16>(inode[129]) << 8) : 0;
    if (extra_size > inode.size() - 128 || (extra_size & 3))
        return Status::io_error("invalid ext4 extra inode extent");
    if (seconds < -2147483648LL || seconds > 15032385535LL)
        return Status::overflow("ext4 timestamp seconds exceed extended range");

    struct Field { u32 bit; usize base, extra; };
    constexpr std::array<Field, 3> layout{{{1, 8, 140}, {2, 16, 136}, {4, 12, 132}}};
    for (const auto &field : layout)
        if ((mask & field.bit) && field.extra + 4 > 128 + extra_size &&
            (seconds < -2147483648LL || seconds > 2147483647LL))
            return Status::overflow("ext4 timestamp seconds require a declared extra word");

    const u32 base = static_cast<u32>(seconds);
    const i64 signed_base = std::bit_cast<i32>(base);
    // The range check bounds this subtraction and its nonnegative result.
    const u64 difference = static_cast<u64>(seconds - signed_base);
    if ((difference & 0xffffffffULL) || (difference >> 32) > 3)
        return Status::overflow("ext4 timestamp epoch cannot be represented");
    const u32 extension = (nanoseconds << 2) | static_cast<u32>(difference >> 32);
    const auto store = [&](usize at, u32 value) {
        for (usize byte = 0; byte < 4; ++byte)
            inode[at + byte] = std::byte(static_cast<u8>(value >> (byte * 8)));
    };
    for (const auto &field : layout)
        if (mask & field.bit)
        {
            store(field.base, base);
            if (field.extra + 4 <= 128 + extra_size)
                store(field.extra, extension);
        }
    return {};
}

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
    ModeChangeResult change_owner(u32 uid, u32 gid, user::Credentials caller) override;
    Status set_times(TimestampChange access, TimestampChange modification) override;
    Result<DirectoryListing> list(DirectoryReadIntent = DirectoryReadIntent::access) const override;
    Result<DirectoryListing> list_from(usize start, DirectoryReadIntent = DirectoryReadIntent::access) const override;

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
    Status chown_active(u32 uid, u32 gid, bool clear_set_id = false);
    Status set_times_active(TimestampChange access, TimestampChange modification);
    Result<DirectoryListing> list_active(DirectoryReadIntent intent, usize start = 0, bool paged = false) const;
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

// Pure candidate qualification for the existing plain, nonjournalled subset.
// The caller has admitted superblock features and supplies the same snapshot's
// geometry and metadata-exclusion predicate. No I/O, time sample or write occurs.
template <typename DataRange>
    requires requires(DataRange range, u32 start, u32 count) { { range(start, count) } -> std::same_as<bool>; }
[[nodiscard]] Result<u64> ext4_allocated_blocks512(std::span<const std::byte> inode,
                                                  u32 block_size, u32 block_count,
                                                  DataRange data_range)
{
    if ((inode.size() != 128 && inode.size() != 256) ||
        (block_size != 1024 && block_size != 2048 && block_size != 4096) || !block_count)
        return Status::invalid_argument("ext4 allocation qualification geometry");
    const auto r16 = [&](usize at) -> u16 {
        return static_cast<u16>(std::to_integer<u8>(inode[at])) |
            (static_cast<u16>(std::to_integer<u8>(inode[at + 1])) << 8);
    };
    const auto r32 = [&](usize at) -> u32 { return r16(at) | (static_cast<u32>(r16(at + 2)) << 16); };
    const auto corrupt = [] { return Status::io_error("invalid ext4 allocated block metadata"); };
    const auto unsupported = [] { return Status::unsupported("ext4 allocated block layout unavailable"); };
    constexpr u32 extents = 0x80000;
    const u32 type = r16(0) & mode_type_mask, size = r32(4), sectors = r32(28), flags = r32(32);
    if (type != mode_regular && type != mode_directory && type != mode_symlink)
        return unsupported();
    // EA, large size, high sector/address representation and huge-file units
    // need separate accounting. Neither a low-word count nor zero is a fallback.
    if (r32(104) || r32(108) || r16(116) || r16(118)) return unsupported();
    const auto range_ok = [&](u32 start, u32 count) {
        return count && start < block_count && count <= block_count - start && data_range(start, count);
    };
    const auto inline_extents = [&]() -> Status {
        const u16 entries = r16(42), maximum = r16(44), depth = r16(46);
        // Reduced root capacity and nonzero depth are outside this canonical
        // inline subset, not sufficient evidence of filesystem corruption.
        if (r16(40) != 0xf30a || !maximum || maximum > 4 || entries > maximum ||
            depth > 32 || (depth && !entries)) return corrupt();
        if (maximum != 4 || depth) return unsupported();
        return Status::success();
    };
    if (type == mode_symlink)
    {
        if (flags != 0 && flags != extents) return unsupported();
        if (!size || size >= block_size) return corrupt();
        if (flags == extents)
            if (auto status = inline_extents(); !status) return status;
        if (!sectors)
        {
            if (flags || size >= 60) return corrupt();
            return u64{0};
        }
        if (sectors != block_size / 512) return corrupt();
        u32 physical;
        if (flags == extents)
        {
            if (r16(42) != 1 || r32(52) || r16(56) != 1 || r16(58)) return corrupt();
            physical = r32(60);
        }
        else
        {
            physical = r32(40);
            for (usize at = 44; at < 100; at += 4) if (r32(at)) return corrupt();
        }
        if (!range_ok(physical, 1)) return corrupt();
        return static_cast<u64>(sectors);
    }
    if (flags != extents) return unsupported();
    if (auto status = inline_extents(); !status) return status;
    const u64 logical_limit = (static_cast<u64>(size) + block_size - 1) / block_size;
    u64 previous_end = 0, allocated = 0;
    for (u16 index = 0; index < r16(42); ++index)
    {
        const usize at = 52 + index * 12;
        const u32 logical = r32(at), physical = r32(at + 8), length = r16(at + 4);
        if (length > 32768 || r16(at + 6)) return unsupported();
        if (!length || logical < previous_end || !range_ok(physical, length)) return corrupt();
        previous_end = static_cast<u64>(logical) + length;
        if (previous_end > (u64{1} << 32)) return corrupt();
        if (previous_end > logical_limit) return unsupported();
        for (u16 earlier = 0; earlier < index; ++earlier)
        {
            const usize other = 52 + earlier * 12;
            const u32 begin = r32(other + 8), count = r16(other + 4);
            if (physical < static_cast<u64>(begin) + count && begin < static_cast<u64>(physical) + length)
                return corrupt();
        }
        allocated += length;
    }
    if (allocated * (block_size / 512) != sectors) return corrupt();
    return static_cast<u64>(sectors);
}

namespace ext4_checked
{
[[nodiscard]] constexpr u16 le16(std::span<const u8> bytes, usize at)
{
    return u16{bytes[at]} | (u16{bytes[at + 1]} << 8);
}
[[nodiscard]] constexpr u32 le32(std::span<const u8> bytes, usize at)
{
    return u32{le16(bytes, at)} | (u32{le16(bytes, at + 2)} << 16);
}
[[nodiscard]] constexpr std::array<u8, 4> encoded(u32 value)
{
    return {u8(value), u8(value >> 8), u8(value >> 16), u8(value >> 24)};
}
[[nodiscard]] inline u32 inode_seed(u32 seed, u32 number, std::span<const u8> inode)
{
    return jbd2::detail::crc32c(jbd2::detail::crc32c(seed, encoded(number)), inode.subspan(100, 4));
}
[[nodiscard]] inline Status inode_checksum(u32 seed, u32 number, std::span<const u8> inode)
{
    if (inode.size() != 256 || le16(inode, 128) != 32)
        return Status::unsupported("ext4 checked inode size or extra fields");
    const u32 expected = u32{le16(inode, 124)} | (u32{le16(inode, 130)} << 16);
    auto crc = inode_seed(seed, number, inode);
    for (usize offset = 0; offset < inode.size(); ++offset)
        crc = jbd2::detail::crc_byte(crc, offset == 124 || offset == 125 || offset == 130 || offset == 131
                                        ? 0 : inode[offset]);
    return crc == expected ? Status::success() : Status::io_error("ext4 checked inode checksum");
}
} // namespace ext4_checked

struct Ext4ReadOnlyInode
{
    u32 number{}, uid{}, gid{}, size{}, generation{};
    u16 mode{}, links{};
};
struct Ext4ReadOnlyDirent
{
    u32 inode{};
    u8 type{};
    std::string_view name{}; // Borrowed only until the visitor returns.
};
struct Ext4NextDataOwnership
{
    std::array<u32, 15> old_physical{};
    u32 old_blocks{}, new_physical{};
};
// Exclusive caller-owned storage for opening and directory iteration.
// Visitors must not reenter using this same scratch while an entry is borrowed.
struct Ext4ReadOnlyOpenScratch
{
    std::array<u8, 4096> block{};
    std::array<u8, 256> inode{};
};

struct Ext4GlobalOwnershipScratch
{
    std::array<u8, 4096> block{};
    std::array<u8, 1024> inode_bitmap{};
};

// Checked decoding over an immutable filesystem-block reader. This is neither a
// VFS mount nor a permission check. The owner supplies an exclusive, already
// bound device/replay view and keeps it alive. File data itself has no checksum.
// This first reader admits inline extents and fast/one-block symlinks; extent-tree nodes,
// directory indexing, xattrs and other inode representations remain rejected.
template <jbd2::BlockReader Reader>
class Ext4ReadOnlyView final
{
    template <usize, usize> friend class Ext4RecoveryCompletion;
    template <usize> friend class Ext4MountedMetadata;
    template <usize ActionCapacity, usize RevokeCapacity>
        requires (ActionCapacity > 0 && RevokeCapacity > 0)
    friend class Ext4ReplayPreview;
    friend class Ext4MetadataTransaction;
    friend class Ext4RenameTransaction;
    friend class Ext4CreateTransaction;
    friend class Ext4FirstDataTransaction;
    friend class Ext4NextDataTransaction;
    friend class Ext4ZeroTruncateOwnershipValidator;
    friend class Ext4AllocatedEmptyDataOwnershipValidator;
  public:
    Ext4ReadOnlyView() = default;
    Ext4ReadOnlyView(const Ext4ReadOnlyView &) = delete;
    Ext4ReadOnlyView &operator=(const Ext4ReadOnlyView &) = delete;
    Ext4ReadOnlyView(Ext4ReadOnlyView &&) = delete;
    Ext4ReadOnlyView &operator=(Ext4ReadOnlyView &&) = delete;

    [[nodiscard]] bool valid() const { return ready_ && !failed_; }
    [[nodiscard]] Status open(Reader &reader, const Ext4RecoveryInfo &binding)
    {
        Ext4ReadOnlyOpenScratch scratch{};
        return open(reader, binding, scratch);
    }
    [[nodiscard]] Status open(Reader &reader, const Ext4RecoveryInfo &binding,
                              Ext4ReadOnlyOpenScratch &scratch)
    {
        if (attempted_)
            return Status::already_exists("ext4 checked view is one-shot");
        const auto overlaps = [](const void *a, usize a_size, const void *b, usize b_size)
        {
            const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
            return one <= two ? two - one < a_size : one - two < b_size;
        };
        if (overlaps(this, sizeof(*this), &scratch, sizeof(scratch)) ||
            overlaps(&binding, sizeof(binding), &scratch, sizeof(scratch)) ||
            overlaps(&reader, sizeof(reader), &scratch, sizeof(scratch)))
            return Status::invalid_argument("ext4 checked open request/scratch overlap");
        attempted_ = true;
        reader_ = &reader;
        layout_ = binding.layout;
        if ((layout_.block_size != 1024 && layout_.block_size != 2048 && layout_.block_size != 4096) ||
            !binding.structural_metadata().valid() || layout_.filesystem_blocks > 0xffffffffULL ||
            binding.journal_inode != 8 || layout_.journal_blocks < 1024 ||
            layout_.journal_first_block >= layout_.filesystem_blocks ||
            layout_.journal_blocks > layout_.filesystem_blocks - layout_.journal_first_block)
            return fail(Status::invalid_argument("ext4 checked view binding bounds"));
        for (usize index = 0; index < binding.metadata_count; ++index)
        {
            const auto range = binding.metadata[index];
            if (range.first < layout_.journal_first_block + layout_.journal_blocks &&
                layout_.journal_first_block < range.first + range.count)
                return fail(Status::invalid_argument("ext4 checked binding domain overlaps journal"));
            for (usize previous = 0; previous < index; ++previous)
                if (range.first < binding.metadata[previous].first + binding.metadata[previous].count &&
                    binding.metadata[previous].first < range.first + range.count)
                    return fail(Status::invalid_argument("ext4 checked binding metadata overlap"));
        }
        for (usize index = 0; index < binding.uuid.size(); ++index)
            uuid_[index] = binding.uuid[index];
        if (const auto status = decode_super(binding, scratch.block); !status)
            return fail(status);
        if (const auto status = decode_groups(binding, scratch.block); !status)
            return fail(status);
        auto &root = scratch.inode;
        if (const auto status = decode_inode(2, root, scratch.block); !status)
            return fail(status);
        if ((ext4_checked::le16(root, 0) & 0170000) != 0040000)
            return fail(Status::io_error("ext4 checked root is not a directory"));
        ready_ = true;
        u32 dots = 0, parents = 0;
        if (const auto status = directory(2, [&](const Ext4ReadOnlyDirent &entry) {
                if (entry.name == "." || entry.name == "..")
                {
                    if (entry.inode != 2 || (entry.type && entry.type != 2))
                        return Status::io_error("ext4 checked root dot-entry identity");
                    auto &count = entry.name == "." ? dots : parents;
                    if (++count != 1)
                        return Status::io_error("ext4 checked duplicate root dot entry");
                }
                return Status::success();
            }, scratch); !status)
            return fail(status);
        if (dots != 1 || parents != 1)
            return fail(Status::io_error("ext4 checked missing root dot entry"));
        return Status::success();
    }

    [[nodiscard]] Result<Ext4ReadOnlyInode> inode(u32 number)
    {
        if (!valid())
            return Status::not_initialized("ext4 checked view is unavailable");
        std::array<u8, 256> bytes{};
        if (const auto status = decode_inode(number, bytes); !status)
            return failure(status);
        return information(number, bytes);
    }

    // A bounded writer admission proof, never a generic extent allocator.
    // The target has exactly the initialized blocks required by its size and
    // no other admitted inode may name the physical block being overwritten.
    [[nodiscard]] Result<u32> unique_initialized_data_block(u32 number, u32 logical)
    {
        if (!valid()) return Status::not_initialized("ext4 checked view is unavailable");
        std::array<u8, 256> target{};
        if (auto status = decode_inode(number, target); !status) return failure(status);
        const u32 size = ext4_checked::le32(target, 4);
        const u32 blocks = u32((u64{size} + layout_.block_size - 1) / layout_.block_size);
        if ((ext4_checked::le16(target, 0) & 0170000) != 0100000 || !blocks || logical >= blocks ||
            (ext4_checked::le16(target, 0) & 06000) || ext4_checked::le32(target, 32) != 0x80000)
            return Status::unsupported("ext4 ordered-data target inode profile");
        u32 next = 0;
        for (usize index = 0; index < ext4_checked::le16(target, 42); ++index)
        {
            const usize at = 52 + index * 12;
            const u32 first = ext4_checked::le32(target, at);
            const u32 length = ext4_checked::le16(target, at + 4);
            if (!length || length > 32768 || first != next || length > blocks - next)
                return Status::unsupported("ext4 ordered-data requires exact initialized extents");
            next += length;
        }
        if (next != blocks) return Status::unsupported("ext4 ordered-data extent coverage");
        const u32 physical = mapped(target, logical);
        if (!physical) return Status::unsupported("ext4 ordered-data hole");
        return unique_physical_block(number, physical);
    }

    // Only the explicit recovery profile uses this admission fact. A directory
    // target is derived from its checked inode rather than a journal tag.
    [[nodiscard]] Result<u32> unique_initialized_directory_block(u32 number)
    {
        if (!valid()) return Status::not_initialized("ext4 checked view is unavailable");
        std::array<u8, 256> inode{};
        if (auto status = decode_inode(number, inode); !status) return failure(status);
        using namespace ext4_checked;
        if ((le16(inode, 0) & 0170000) != 0040000 || le32(inode, 4) != layout_.block_size ||
            le32(inode, 28) != layout_.block_size / 512 || le32(inode, 32) != 0x80000 ||
            le16(inode, 40) != 0xf30a || le16(inode, 42) != 1 || le16(inode, 44) != 4 ||
            le16(inode, 46) || le32(inode, 52) || le16(inode, 56) != 1 ||
            le16(inode, 58) || !le32(inode, 60))
            return Status::unsupported("ext4 replay requires one initialized linear directory block");
        const u32 physical = le32(inode, 60);
        std::array<u8, 4096> bytes{};
        if (auto status = block(physical, bytes); !status) return failure(status);
        if (auto status = directory_block(number, inode, std::span<const u8>{bytes}.first(layout_.block_size)); !status)
            return failure(status);
        return unique_physical_block(number, physical);
    }

    // Admission facts for the bounded first-block writer, not allocation
    // authority. A clear bit alone never permits overwriting file data.
    [[nodiscard]] Result<u32> unreferenced_free_data_block(u32 physical)
    {
        if (!valid()) return Status::not_initialized("ext4 first-data view is unavailable");
        if (auto status = first_data_ownership(physical, 0); !status) return status;
        return physical;
    }

    // The final journal overlay may exclude only this checked new mapping.
    [[nodiscard]] Result<u32> unique_first_allocated_data_block(u32 number)
    {
        using namespace ext4_checked;
        if (!valid()) return Status::not_initialized("ext4 first-data view is unavailable");
        std::array<u8, 256> inode{};
        if (auto status = decode_inode(number, inode); !status) return failure(status);
        if (layout_.block_size != 1024 || number < first_inode_ ||
            (le16(inode, 0) & 0170000) != 0100000 || (le16(inode, 0) & 07000) ||
            le16(inode, 26) != 1 || !le32(inode, 4) || le32(inode, 4) > 1024 ||
            le32(inode, 28) != 2 || le32(inode, 32) != 0x80000 ||
            le16(inode, 40) != 0xf30a || le16(inode, 42) != 1 || le16(inode, 44) != 4 ||
            le16(inode, 46) || le32(inode, 52) || le16(inode, 56) != 1 || le16(inode, 58))
            return Status::unsupported("ext4 first-data final mapping profile");
        const u32 physical = le32(inode, 60);
        if (auto status = first_data_ownership(physical, number); !status) return status;
        return physical;
    }

    // Proves the complete old prefix and the candidate in one bounded scan.
    // A final overlay includes the candidate as the last mapped logical block.
    [[nodiscard]] Result<Ext4NextDataOwnership> prove_next_data_blocks(
        u32 number, u32 candidate, bool candidate_allocated)
    {
        using namespace ext4_checked;
        if (!valid()) return Status::not_initialized("ext4 next-data view is unavailable");
        if (layout_.block_size != 1024 || number < first_inode_)
            return Status::unsupported("ext4 next-data inode or block geometry");
        if (auto status = first_data_ordinary_block(candidate); !status) return failure(status);
        std::array<u8, 256> target{};
        if (auto status = decode_inode(number, target); !status) return failure(status);
        const u32 size = le32(target, 4);
        const u32 mapped_blocks = u32((u64{size} + 1023) / 1024);
        const u32 old_blocks = candidate_allocated ? mapped_blocks - (mapped_blocks != 0) : mapped_blocks;
        if ((le16(target, 0) & 0170000) != 0100000 || (le16(target, 0) & 07000) ||
            le16(target, 26) != 1 || !old_blocks || old_blocks > 15 ||
            (!candidate_allocated && size != old_blocks * 1024) ||
            (candidate_allocated && (size <= old_blocks * 1024 ||
                                     size > (old_blocks + 1) * 1024)) ||
            le32(target, 28) != mapped_blocks * 2 || le32(target, 32) != 0x80000 ||
            le16(target, 40) != 0xf30a || !le16(target, 42) || le16(target, 42) > 4 ||
            le16(target, 44) != 4 || le16(target, 46))
            return Status::unsupported("ext4 next-data target inode profile");
        Ext4NextDataOwnership result{};
        result.old_blocks = old_blocks;
        result.new_physical = candidate;
        u32 logical = 0;
        for (usize index = 0; index < le16(target, 42); ++index)
        {
            const usize at = 52 + index * 12;
            const u32 start = le32(target, at);
            const u32 length = le16(target, at + 4);
            const u32 physical = le32(target, at + 8);
            if (!length || length > 32768 || start != logical ||
                length > mapped_blocks - logical || le16(target, at + 6))
                return Status::unsupported("ext4 next-data hole or unwritten extent");
            for (u32 offset = 0; offset < length; ++offset)
            {
                const u32 mapped_physical = physical + offset;
                if (auto status = first_data_ordinary_block(mapped_physical); !status)
                    return failure(status);
                if (logical < old_blocks)
                {
                    if (mapped_physical == candidate)
                        return failure(Status::io_error("ext4 next-data candidate aliases old prefix"));
                    for (u32 prior = 0; prior < logical; ++prior)
                        if (result.old_physical[prior] == mapped_physical)
                            return failure(Status::io_error("ext4 next-data old prefix aliases itself"));
                    result.old_physical[logical] = mapped_physical;
                }
                else if (!candidate_allocated || mapped_physical != candidate)
                    return failure(Status::io_error("ext4 next-data final mapping differs from candidate"));
                ++logical;
            }
        }
        if (logical != mapped_blocks)
            return Status::unsupported("ext4 next-data extent coverage");
        const u32 selected_group = (candidate - first_data_) / blocks_per_group_;
        const auto &selected = groups_[selected_group];
        if (selected.flags & 2)
            return Status::unsupported("ext4 next-data uninitialized candidate bitmap");
        std::array<u8, 4096> bitmap{}, table{};
        if (auto status = block(selected.block_bitmap, bitmap); !status) return status;
        if (u16(jbd2::detail::crc32c(seed_, std::span<const u8>{bitmap}.first(blocks_per_group_ / 8))) !=
            selected.block_checksum)
            return failure(Status::io_error("ext4 next-data candidate bitmap checksum"));
        const u32 candidate_local = (candidate - first_data_) % blocks_per_group_;
        if (bool(bitmap[candidate_local / 8] & (1U << (candidate_local % 8))) != candidate_allocated)
            return failure(Status::io_error("ext4 next-data candidate allocation bit"));
        usize inspected = 0;
        bool journal_seen = false, target_seen = false;
        for (u32 group = 0; group < group_count_; ++group)
        {
            const auto &g = groups_[group];
            if (g.flags & 1) continue;
            if (auto status = block(g.inode_bitmap, bitmap); !status) return status;
            if (u16(jbd2::detail::crc32c(seed_, std::span<const u8>{bitmap}.first(inodes_per_group_ / 8))) !=
                g.inode_checksum)
                return failure(Status::io_error("ext4 next-data inode bitmap checksum"));
            for (u32 local = 0; local < inodes_per_group_; ++local)
            {
                if (!(bitmap[local / 8] & (1U << (local % 8)))) continue;
                if (++inspected > 128) return Status::no_memory("ext4 next-data ownership scan budget");
                const u32 other_number = group * inodes_per_group_ + local + 1;
                const u32 offset = local * 256;
                if (auto status = block(g.table + offset / layout_.block_size, table); !status) return status;
                std::array<u8, 256> raw{};
                for (usize at = 0; at < raw.size(); ++at)
                    raw[at] = table[offset % layout_.block_size + at];
                if (other_number == 8)
                {
                    if (auto status = first_data_journal(raw); !status) return failure(status);
                    journal_seen = true;
                    continue;
                }
                if (other_number != 2 && other_number < first_inode_)
                {
                    if (auto status = first_data_reserved_empty(other_number, raw); !status)
                        return failure(status);
                    continue;
                }
                std::array<u8, 256> checked{};
                if (auto status = decode_inode(other_number, checked); !status)
                    return status.code() == StatusCode::not_found ?
                        fail(Status::io_error("ext4 next-data allocated inode vanished during scan")) : failure(status);
                if (raw != checked)
                    return failure(Status::io_error("ext4 next-data inode changed during ownership scan"));
                if (other_number == number)
                {
                    if (checked != target)
                        return failure(Status::io_error("ext4 next-data target changed during ownership scan"));
                    target_seen = true;
                    continue;
                }
                if (!le32(checked, 28)) continue;
                if (!le32(checked, 32))
                {
                    const u32 physical = le32(checked, 40);
                    if (physical == candidate)
                        return failure(Status::io_error("ext4 next-data candidate referenced by direct block"));
                    for (u32 old = 0; old < old_blocks; ++old)
                        if (physical == result.old_physical[old])
                            return failure(Status::io_error("ext4 next-data old block shared by direct block"));
                    continue;
                }
                for (usize index = 0; index < le16(checked, 42); ++index)
                {
                    const usize at = 52 + index * 12;
                    const u32 encoded = le16(checked, at + 4);
                    const u32 length = encoded > 32768 ? encoded - 32768 : encoded;
                    const u32 first = le32(checked, at + 8);
                    if (candidate >= first && u64{candidate} - first < length)
                        return failure(Status::io_error("ext4 next-data candidate referenced by extent"));
                    for (u32 old = 0; old < old_blocks; ++old)
                        if (result.old_physical[old] >= first &&
                            u64{result.old_physical[old]} - first < length)
                            return failure(Status::io_error("ext4 next-data old block shared by extent"));
                }
            }
        }
        if (!journal_seen || !target_seen)
            return failure(Status::io_error("ext4 next-data journal or target inode is absent"));
        return result;
    }

    // Final-state evidence only: all admitted allocated inodes have checked
    // mappings and no two different inodes own an overlapping physical range.
    // An all-after journal cannot recover which inode changed historically.
    [[nodiscard]] Status prove_global_data_ownership()
    {
        Ext4GlobalOwnershipScratch scratch{};
        return prove_global_data_ownership(scratch);
    }
    // New mounted operations supply this bounded workspace off stack.
    [[nodiscard]] Status prove_global_data_ownership(Ext4GlobalOwnershipScratch &workspace)
    {
        using namespace ext4_checked;
        if (!valid()) return Status::not_initialized("ext4 final ownership view is unavailable");
        if (layout_.block_size != 1024 || inodes_per_group_ > 8192)
            return Status::unsupported("ext4 final ownership geometry");
        ownership_range_count_ = 0;
        auto &scratch = workspace.block;
        auto &inode_bitmap = workspace.inode_bitmap;
        usize inspected = 0;
        bool journal_seen = false;
        for (u32 group = 0; group < group_count_; ++group)
        {
            const auto &g = groups_[group];
            if (g.flags & 1) continue;
            if (auto status = block(g.inode_bitmap, scratch); !status) return status;
            const usize bitmap_bytes = inodes_per_group_ / 8;
            if (u16(jbd2::detail::crc32c(seed_, std::span<const u8>{scratch}.first(bitmap_bytes))) !=
                g.inode_checksum)
                return failure(Status::io_error("ext4 final ownership inode bitmap checksum"));
            for (usize at = 0; at < bitmap_bytes; ++at) inode_bitmap[at] = scratch[at];
            for (u32 local = 0; local < inodes_per_group_; ++local)
            {
                if (!(inode_bitmap[local / 8] & (1U << (local % 8)))) continue;
                if (++inspected > 128)
                    return failure(Status::no_memory("ext4 final ownership scan budget"));
                const u32 number = group * inodes_per_group_ + local + 1;
                if (number == 8 || (number != 2 && number < first_inode_))
                {
                    const u32 offset = local * 256;
                    if (auto status = block(g.table + offset / layout_.block_size, scratch); !status)
                        return status;
                    const auto raw = std::span<const u8, 256>{
                        scratch.data() + offset % layout_.block_size, 256};
                    const auto status = number == 8 ? first_data_journal(raw) :
                        first_data_reserved_empty(number, raw);
                    if (!status) return failure(status);
                    if (number == 8) journal_seen = true;
                    continue;
                }
                std::array<u8, 256> checked{};
                if (auto status = decode_inode(number, checked, scratch); !status)
                    return failure(status.code() == StatusCode::not_found && valid() ?
                        Status::io_error("ext4 final ownership allocated inode vanished") : status);
                const u32 offset = local * 256;
                if (auto status = block(g.table + offset / layout_.block_size, scratch); !status)
                    return status;
                for (usize at = 0; at < checked.size(); ++at)
                    if (checked[at] != scratch[offset % layout_.block_size + at])
                        return failure(Status::io_error("ext4 final ownership inode changed during scan"));
                if (!le32(checked, 28)) continue;
                std::array<OwnershipRange, 4> current{};
                usize count = 0;
                if (!le32(checked, 32))
                {
                    current[count++] = {le32(checked, 40), 1};
                }
                else
                {
                    const usize records = le16(checked, 42);
                    if (records > current.size())
                        return failure(Status::unsupported("ext4 final ownership extent capacity"));
                    for (usize index = 0; index < records; ++index)
                    {
                        const usize at = 52 + index * 12;
                        const u32 encoded = le16(checked, at + 4);
                        current[count++] = {le32(checked, at + 8),
                            encoded > 32768 ? encoded - 32768 : encoded};
                    }
                }
                for (usize index = 0; index < count; ++index)
                {
                    const auto range = current[index];
                    for (usize prior = 0; prior < ownership_range_count_; ++prior)
                    {
                        const auto previous = ownership_ranges_[prior];
                        if (range.first < u64{previous.first} + previous.count &&
                            previous.first < u64{range.first} + range.count)
                            return failure(Status::io_error("ext4 final ownership physical blocks overlap"));
                    }
                }
                if (count > ownership_ranges_.size() - ownership_range_count_)
                    return failure(Status::no_memory("ext4 final ownership range budget"));
                for (usize index = 0; index < count; ++index)
                    ownership_ranges_[ownership_range_count_++] = current[index];
            }
        }
        if (!journal_seen)
            return failure(Status::io_error("ext4 final ownership journal inode is absent"));
        return {};
    }

  private:
    // Only the actual mounted Owner's durable namespace transition admits
    // this one checked orphan. Ordinary open continues to require head zero.
    [[nodiscard]] Status open_retained_orphan(Reader &reader, const Ext4RecoveryInfo &binding,
                                              u32 inode, u32 generation, u32 next)
    {
        if (attempted_ || !binding.needs_recovery || !binding.empty_log ||
            binding.layout.block_size != 1024 || inode < 11 || inode > 8192 || next)
            return Status::unsupported("checked retained orphan requires one live bounded head");
        retained_orphan_ = inode; retained_orphan_generation_ = generation; retained_orphan_next_ = next;
        return open(reader, binding);
    }
    struct OwnershipRange { u32 first{}, count{}; };
    [[nodiscard]] Status first_data_ordinary_block(u32 physical) const
    {
        if (layout_.block_size != 1024 || physical < first_data_ || physical >= layout_.filesystem_blocks)
            return Status::unsupported("ext4 first-data block geometry");
        if (physical >= layout_.journal_first_block &&
            u64{physical} - layout_.journal_first_block < layout_.journal_blocks)
            return Status::io_error("ext4 first-data block aliases journal");
        for (u32 group = 0; group < group_count_; ++group)
        {
            const auto &g = groups_[group];
            const u32 begin = first_data_ + group * blocks_per_group_;
            if ((physical >= begin && physical - begin < 2) || physical == g.block_bitmap ||
                physical == g.inode_bitmap ||
                (physical >= g.table && physical - g.table < table_blocks_))
                return Status::io_error("ext4 first-data block aliases structural metadata");
        }
        return {};
    }

    [[nodiscard]] Status first_data_reserved_empty(u32 number, std::span<const u8, 256> inode) const
    {
        if (jbd2::detail::all_zero(inode)) return {};
        // e2fsprogs reserves unused system records with a low checksum and,
        // for the bad-block inode, plain timestamps. No pointer field is
        // exempt from the zero requirement; extra-size zero omits checksum_hi.
        for (usize at = 0; at < inode.size(); ++at)
            if (!(at >= 8 && at < 20) && at != 124 && at != 125 && inode[at])
                return Status::unsupported("ext4 first-data opaque reserved inode");
        auto crc = ext4_checked::inode_seed(seed_, number, inode);
        for (usize at = 0; at < inode.size(); ++at)
            crc = jbd2::detail::crc_byte(crc, at == 124 || at == 125 ? 0 : inode[at]);
        return u16(crc) == ext4_checked::le16(inode, 124) ? Status{} :
            Status::io_error("ext4 first-data reserved inode checksum");
    }

    [[nodiscard]] Status first_data_journal(std::span<const u8, 256> inode) const
    {
        using namespace ext4_checked;
        if (auto status = inode_checksum(seed_, 8, inode); !status) return status;
        if (layout_.journal_blocks > 32768 || u64{layout_.journal_blocks} * 1024 > 0xffffffffULL ||
            (le16(inode, 0) & 0170000) != 0100000 || le16(inode, 26) != 1 ||
            le32(inode, 4) != layout_.journal_blocks * 1024 ||
            le32(inode, 28) != layout_.journal_blocks * 2 || le32(inode, 32) != 0x80000 ||
            le16(inode, 40) != 0xf30a || le16(inode, 42) != 1 || le16(inode, 44) != 4 ||
            le16(inode, 46) || le32(inode, 52) || le16(inode, 56) != layout_.journal_blocks ||
            le16(inode, 58) || le32(inode, 60) != layout_.journal_first_block ||
            le32(inode, 20) || le32(inode, 104) || le32(inode, 108) || le32(inode, 112) ||
            le16(inode, 116) || le16(inode, 118) ||
            !jbd2::detail::all_zero(inode.subspan(64, 36)))
            return Status::unsupported("ext4 first-data journal inode binding changed");
        return {};
    }

    [[nodiscard]] Status first_data_ownership(u32 physical, u32 exclusive_inode)
    {
        using namespace ext4_checked;
        if (auto status = first_data_ordinary_block(physical); !status) return failure(status);
        const u32 selected_group = (physical - first_data_) / blocks_per_group_;
        const auto &selected = groups_[selected_group];
        if (selected.flags & 2)
            return Status::unsupported("ext4 first-data uninitialized block bitmap");
        std::array<u8, 4096> bitmap{}, table{};
        if (auto status = block(selected.block_bitmap, bitmap); !status) return status;
        if (u16(jbd2::detail::crc32c(seed_, std::span<const u8>{bitmap}.first(blocks_per_group_ / 8))) !=
            selected.block_checksum)
            return failure(Status::io_error("ext4 first-data block bitmap checksum"));
        const u32 local_block = (physical - first_data_) % blocks_per_group_;
        if (bool(bitmap[local_block / 8] & (1U << (local_block % 8))) != bool(exclusive_inode))
            return failure(Status::io_error("ext4 first-data candidate allocation bit"));
        usize inspected = 0;
        bool journal_seen = false, target_seen = !exclusive_inode;
        for (u32 group = 0; group < group_count_; ++group)
        {
            const auto &g = groups_[group];
            // decode_groups proved this flag means an entirely empty group.
            if (g.flags & 1) continue;
            if (auto status = block(g.inode_bitmap, bitmap); !status) return status;
            if (u16(jbd2::detail::crc32c(seed_, std::span<const u8>{bitmap}.first(inodes_per_group_ / 8))) !=
                g.inode_checksum)
                return failure(Status::io_error("ext4 first-data inode bitmap checksum"));
            for (u32 local = 0; local < inodes_per_group_; ++local)
            {
                if (!(bitmap[local / 8] & (1U << (local % 8)))) continue;
                if (++inspected > 128) return Status::no_memory("ext4 first-data ownership scan budget");
                const u32 number = group * inodes_per_group_ + local + 1;
                const u32 offset = local * 256;
                if (auto status = block(g.table + offset / layout_.block_size, table); !status) return status;
                std::array<u8, 256> raw{};
                for (usize at = 0; at < raw.size(); ++at) raw[at] = table[offset % layout_.block_size + at];
                if (number == 8)
                {
                    if (auto status = first_data_journal(raw); !status) return failure(status);
                    journal_seen = true;
                    continue;
                }
                if (number != 2 && number < first_inode_)
                {
                    if (auto status = first_data_reserved_empty(number, raw); !status) return failure(status);
                    continue;
                }
                std::array<u8, 256> checked{};
                if (auto status = decode_inode(number, checked); !status) return failure(status);
                if (raw != checked)
                    return failure(Status::io_error("ext4 first-data inode changed during ownership scan"));
                if (number == exclusive_inode)
                {
                    if (mapped(checked, 0) != physical)
                        return failure(Status::io_error("ext4 first-data exclusive mapping changed"));
                    target_seen = true;
                    continue;
                }
                if (!le32(checked, 28)) continue;
                if (!le32(checked, 32))
                {
                    if (le32(checked, 40) == physical)
                        return failure(Status::io_error("ext4 first-data referenced direct block"));
                    continue;
                }
                for (usize index = 0; index < le16(checked, 42); ++index)
                {
                    const usize at = 52 + index * 12;
                    const u32 encoded = le16(checked, at + 4);
                    const u32 count = encoded > 32768 ? encoded - 32768 : encoded;
                    const u32 first = le32(checked, at + 8);
                    if (physical >= first && u64{physical} - first < count)
                        return failure(Status::io_error("ext4 first-data referenced extent block"));
                }
            }
        }
        if (!journal_seen || !target_seen)
            return failure(Status::io_error("ext4 first-data journal or exclusive inode is absent"));
        return {};
    }

    [[nodiscard]] Result<u32> unique_physical_block(u32 number, u32 physical)
    {
        std::array<u8, 4096> bitmap{};
        usize inspected = 0;
        for (u32 group = 0; group < group_count_; ++group)
        {
            const auto &g = groups_[group];
            if (g.flags & 1) continue;
            if (auto status = block(g.inode_bitmap, bitmap); !status) return failure(status);
            if (u16(jbd2::detail::crc32c(seed_, std::span<const u8>{bitmap}.first(inodes_per_group_ / 8))) !=
                g.inode_checksum)
                return failure(Status::io_error("ext4 ordered-data inode bitmap checksum"));
            for (u32 local = 0; local < inodes_per_group_; ++local)
            {
                const u32 candidate = group * inodes_per_group_ + local + 1;
                if (candidate != 2 && candidate < first_inode_) continue;
                if (!(bitmap[local / 8] & (1U << (local % 8)))) continue;
                if (++inspected > 128) return Status::no_memory("ext4 ordered-data ownership scan budget");
                if (candidate == number) continue;
                std::array<u8, 256> other{};
                if (auto status = decode_inode(candidate, other); !status) return failure(status);
                if (!ext4_checked::le32(other, 28)) continue;
                if (ext4_checked::le32(other, 32) == 0)
                {
                    if (ext4_checked::le32(other, 40) == physical)
                        return Status::io_error("ext4 ordered-data shared direct block");
                    continue;
                }
                for (usize index = 0; index < ext4_checked::le16(other, 42); ++index)
                {
                    const usize at = 52 + index * 12;
                    const u32 encoded = ext4_checked::le16(other, at + 4);
                    const u32 count = encoded > 32768 ? encoded - 32768 : encoded;
                    const u32 first = ext4_checked::le32(other, at + 8);
                    if (physical >= first && u64{physical} < u64{first} + count)
                        return Status::io_error("ext4 ordered-data shared physical block");
                }
            }
        }
        return physical;
    }

  public:

    // Checked projection of the same validated inode image; no epoch provenance
    // is inferred for a mutable provider that uses this read-only decoder.
    [[nodiscard]] Result<Metadata> query_metadata(u32 number)
    {
        if (!valid()) return Status::not_initialized("ext4 checked view is unavailable");
        std::array<u8, 256> bytes{};
        if (const auto status = decode_inode(number, bytes); !status) return failure(status);
        return project_metadata(number, bytes);
    }

    // Stored values from one checked inode image. The owner, not this mutable
    // reader, must establish timestamp provenance before a Linux stat succeeds.
    [[nodiscard]] Result<Metadata> query_stored_metadata(u32 number)
    {
        if (!valid()) return Status::not_initialized("ext4 checked view is unavailable");
        std::array<u8, 256> bytes{};
        if (const auto status = decode_inode(number, bytes); !status) return failure(status);
        auto result = project_metadata(number, bytes);
        // decode_inode checked allocated extents/symlinks against i_blocks.
        result.attributes.mark(Attribute::blocks512);
        if (auto status = decode_ext4_times(std::as_bytes(std::span{bytes}), result.attributes); !status)
            return failure(status);
        result.attributes.access.source = result.attributes.modification.source =
            result.attributes.change.source = TimestampSource::unavailable;
        return result;
    }

    // Fixed scratch space; at most group_capacity * block_size * 8 ordinary
    // inode candidates. The 1 KiB mounted profile admits at most 262,144 inodes.
    // Reserved system/journal inodes are not exposed as ordinary VFS nodes.
    [[nodiscard]] Status validate_stored_metadata()
    {
        auto root = query_stored_metadata(2);
        if (!root) return root.status();
        std::array<u8, 4096> bitmap{};
        for (u32 group = 0; group < group_count_; ++group)
        {
            const auto &g = groups_[group];
            if (g.flags & 1) continue;
            if (auto status = block(g.inode_bitmap, bitmap); !status) return status;
            if (u16(jbd2::detail::crc32c(seed_, std::span<const u8>{bitmap}.first(inodes_per_group_ / 8))) != g.inode_checksum)
                return fail(Status::io_error("ext4 checked scan inode bitmap checksum"));
            for (u32 local = 0; local < inodes_per_group_; ++local)
            {
                const u32 number = group * inodes_per_group_ + local + 1;
                if (number < first_inode_ || !(bitmap[local / 8] & (1U << (local % 8)))) continue;
                auto metadata = query_stored_metadata(number);
                if (!metadata) return metadata.status();
            }
        }
        return {};
    }

    [[nodiscard]] Result<usize> read(u32 number, u64 offset, std::span<u8> out)
    {
        if (!valid())
            return Status::not_initialized("ext4 checked view is unavailable");
        std::array<u8, 256> node{};
        if (const auto status = decode_inode(number, node); !status)
            return failure(status);
        const auto info = information(number, node);
        const auto type = info.mode & 0170000;
        if (type != 0100000 && type != 0120000)
            return Status::is_directory("ext4 checked data read requires a regular file or symlink");
        if (offset >= info.size || out.empty())
            return usize{0};
        const usize count = info.size - offset < out.size() ? usize(info.size - offset) : out.size();
        if (type == 0120000 && !ext4_checked::le32(node, 28))
        {
            for (usize index = 0; index < count; ++index)
                out[index] = node[40 + usize(offset) + index];
            return count;
        }
        std::array<u8, 4096> data{};
        usize done = 0;
        while (done < count)
        {
            const auto logical = u32((offset + done) / layout_.block_size);
            const auto physical = mapped(node, logical);
            const auto begin = usize((offset + done) % layout_.block_size);
            const auto amount = count - done < layout_.block_size - begin ? count - done : layout_.block_size - begin;
            if (physical)
            {
                if (const auto status = block(physical, data); !status)
                    return failure(status);
                for (usize index = 0; index < amount; ++index)
                    out[done + index] = data[begin + index];
            }
            else
                for (usize index = 0; index < amount; ++index)
                    out[done + index] = 0;
            done += amount;
        }
        return done;
    }

    template <typename Visitor>
        requires requires(Visitor visitor, const Ext4ReadOnlyDirent &entry) {
            { visitor(entry) } -> std::same_as<Status>;
        }
    [[nodiscard]] Status directory(u32 number, Visitor &&visitor)
    {
        Ext4ReadOnlyOpenScratch scratch{};
        return directory(number, static_cast<Visitor &&>(visitor), scratch);
    }

    template <typename Visitor>
        requires requires(Visitor visitor, const Ext4ReadOnlyDirent &entry) {
            { visitor(entry) } -> std::same_as<Status>;
        }
    [[nodiscard]] Status directory(u32 number, Visitor &&visitor, Ext4ReadOnlyOpenScratch &scratch)
    {
        if (!valid())
            return Status::not_initialized("ext4 checked view is unavailable");
        auto &node = scratch.inode;
        if (const auto status = decode_inode(number, node, scratch.block); !status)
            return failure(status);
        if ((ext4_checked::le16(node, 0) & 0170000) != 0040000)
            return Status::not_directory("ext4 checked inode is not a directory");
        const u32 size = ext4_checked::le32(node, 4);
        if (size / layout_.block_size > 1024)
            return Status::unsupported("ext4 checked directory scan capacity");
        if (!size || size % layout_.block_size)
            return fail(Status::io_error("ext4 checked directory size"));
        auto &data = scratch.block;
        for (u32 logical = 0; logical < size / layout_.block_size; ++logical)
        {
            const auto physical = mapped(node, logical);
            if (!physical)
                return fail(Status::io_error("ext4 checked directory hole or unwritten block"));
            if (const auto status = block(physical, data); !status)
                return failure(status);
            auto bytes = std::span<const u8>{data}.first(layout_.block_size);
            if (const auto status = directory_block(number, node, bytes); !status)
                return failure(status);
            for (usize offset = 0; offset < layout_.block_size - 12; offset += ext4_checked::le16(bytes, offset + 4))
            {
                const auto child = ext4_checked::le32(bytes, offset);
                if (!child)
                    continue;
                const Ext4ReadOnlyDirent entry{child, bytes[offset + 7],
                    {reinterpret_cast<const char *>(data.data() + offset + 8), bytes[offset + 6]}};
                if (const auto status = visitor(entry); !status)
                    return status;
            }
        }
        return Status::success();
    }

    [[nodiscard]] Result<u32> lookup(u32 directory_number, std::string_view name)
    {
        if (name.empty() || name.size() > 255 || name.find('/') != std::string_view::npos ||
            name.find('\0') != std::string_view::npos)
            return Status::invalid_argument("ext4 checked component name");
        u32 found = 0;
        u8 found_type = 0;
        const auto status = directory(directory_number, [&](const Ext4ReadOnlyDirent &entry) {
            if (entry.name == name)
            {
                if (found)
                    return Status::io_error("ext4 checked duplicate directory name");
                found = entry.inode;
                found_type = entry.type;
            }
            return Status::success();
        });
        if (!status)
            return failure(status);
        if (!found)
            return Status::not_found("ext4 checked component not found");
        const auto child = inode(found);
        if (!child)
        {
            if (child.status().code() == StatusCode::not_found)
                return fail(Status::io_error("ext4 checked directory references an unallocated inode"));
            return child.status();
        }
        const u16 type = child.value().mode & 0170000;
        const u8 expected_type = type == 0100000 ? 1 : type == 0040000 ? 2 : 7;
        if (found_type && found_type != expected_type)
            return fail(Status::io_error("ext4 checked directory type differs from inode"));
        return found;
    }

    // Helps a bounded preview policy identify inode slots without accepting
    // caller-supplied inode-table locations or reinterpreting directory data.
    [[nodiscard]] Result<u32> inode_at(u32 table_block, usize offset) const
    {
        if (!valid() || offset >= layout_.block_size || offset % 256)
            return Status::invalid_argument("ext4 checked inode-table slot");
        for (u32 group = 0; group < group_count_; ++group)
            if (table_block >= groups_[group].table && table_block - groups_[group].table < table_blocks_)
                return group * inodes_per_group_ + (table_block - groups_[group].table) * (layout_.block_size / 256) +
                       u32(offset / 256) + 1;
        return Status::not_found("ext4 checked target is not an inode table");
    }
    [[nodiscard]] u32 checksum_seed() const { return seed_; }
    [[nodiscard]] u32 first_ordinary_inode() const { return first_inode_; }

  private:
    struct Group
    {
        u32 block_bitmap{}, inode_bitmap{}, table{};
        u16 block_checksum{}, inode_checksum{}, flags{};
    };
    [[nodiscard]] Status fail(Status status) { failed_ = true; return status; }
    [[nodiscard]] Metadata project_metadata(u32 number, std::span<const u8> bytes) const
    {
        const auto value = information(number, bytes);
        Metadata result{};
        result.type = (value.mode & mode_type_mask) == mode_directory ? NodeType::directory :
                      (value.mode & mode_type_mask) == mode_symlink ? NodeType::symlink : NodeType::regular;
        result.size = value.size; result.mode = value.mode; result.uid = value.uid; result.gid = value.gid;
        result.link_count = value.links; result.block_size = layout_.block_size;
        result.blocks = ext4_checked::le32(bytes, 28);
        result.attributes.inode = number; result.attributes.mark(Attribute::inode);
        return result;
    }
    [[nodiscard]] Status failure(Status status)
    {
        if (status.code() != StatusCode::not_found && status.code() != StatusCode::unsupported &&
            status.code() != StatusCode::not_directory)
            failed_ = true;
        return status;
    }
    [[nodiscard]] Status block(u32 index, std::span<u8> out)
    {
        if (index >= layout_.filesystem_blocks || out.size() < layout_.block_size)
            return fail(Status::io_error("ext4 checked block bounds"));
        const auto status = reader_->read_block(index, out.first(layout_.block_size));
        // Reader failures always retire the view, even if a device uses a
        // status that is nonfatal for ordinary inode/namespace operations.
        if (!status)
            return fail(status);
        return status;
    }
    [[nodiscard]] Status decode_super(const Ext4RecoveryInfo &binding)
    {
        std::array<u8, 4096> data{};
        return decode_super(binding, data);
    }
    [[nodiscard]] Status decode_super(const Ext4RecoveryInfo &binding, std::array<u8, 4096> &data)
    {
        using namespace ext4_checked;
        if (const auto status = block(1024 / layout_.block_size, data); !status)
            return status;
        const auto s = std::span<const u8>{data}.subspan(1024 % layout_.block_size, 1024);
        if (jbd2::detail::crc32c(~u32{0}, s.first(1020)) != le32(s, 1020))
            return Status::io_error("ext4 checked superblock checksum");
        if (le16(s, 56) != 0xef53 || le32(s, 24) > 2 || (1024U << le32(s, 24)) != layout_.block_size ||
            le32(s, 4) != layout_.filesystem_blocks || le32(s, 76) != 1 || le32(s, 72) ||
            le32(s, 92) != 4 || le32(s, 96) != (binding.needs_recovery ? 0x46U : 0x42U) ||
            le32(s, 100) != 0x400 || le16(s, 88) != 256 || le16(s, 90) || le16(s, 58) != 1 ||
            le16(s, 206) || le32(s, 224) != 8 || le32(s, 228) || le32(s, 232) != retained_orphan_ || s[373] != 1 ||
            (le16(s, 254) != 0 && le16(s, 254) != 32) || le32(s, 336) || le32(s, 340) || le32(s, 344) ||
            !jbd2::detail::all_zero(s.subspan(208, 16)))
            return Status::unsupported("ext4 checked superblock differs from bounded binding");
        for (usize index = 0; index < uuid_.size(); ++index)
            if (uuid_[index] != s[104 + index])
                return Status::io_error("ext4 checked filesystem UUID changed");
        first_data_ = le32(s, 20);
        blocks_per_group_ = le32(s, 32);
        inodes_per_group_ = le32(s, 40);
        inodes_ = le32(s, 0);
        first_inode_ = le32(s, 84);
        if (first_data_ != (layout_.block_size == 1024 ? 1U : 0U) || layout_.filesystem_blocks <= first_data_ ||
            !blocks_per_group_ || blocks_per_group_ > layout_.block_size * 8 || blocks_per_group_ % 8 ||
            !inodes_per_group_ || inodes_per_group_ > layout_.block_size * 8 || inodes_per_group_ % 8 ||
            inodes_per_group_ % (layout_.block_size / 256) || le32(s, 28) != le32(s, 24) ||
            le32(s, 36) != blocks_per_group_ || first_inode_ < 11 || first_inode_ > inodes_ ||
            le32(s, 12) > layout_.filesystem_blocks - first_data_ || le32(s, 16) > inodes_)
            return Status::io_error("ext4 checked group geometry");
        group_count_ = u32((layout_.filesystem_blocks - first_data_ - 1) / blocks_per_group_ + 1);
        if (group_count_ > groups_.size() || u64{group_count_} * inodes_per_group_ != inodes_ ||
            binding.metadata_count != group_count_ * 4)
            return Status::unsupported("ext4 checked group count or capacity");
        table_blocks_ = inodes_per_group_ * 256 / layout_.block_size;
        seed_ = jbd2::detail::crc32c(~u32{0}, uuid_);
        return Status::success();
    }
    [[nodiscard]] Status decode_groups(const Ext4RecoveryInfo &binding)
    {
        std::array<u8, 4096> data{};
        return decode_groups(binding, data);
    }
    [[nodiscard]] Status decode_groups(const Ext4RecoveryInfo &binding, std::array<u8, 4096> &data)
    {
        using namespace ext4_checked;
        if (const auto status = block(first_data_ + 1, data); !status)
            return status;
        for (u32 group = 0; group < group_count_; ++group)
        {
            auto d = std::span<u8>{data}.subspan(group * 32, 32);
            const auto expected = le16(d, 30);
            d[30] = d[31] = 0;
            const auto sum = jbd2::detail::crc32c(jbd2::detail::crc32c(seed_, encoded(group)), d);
            d[30] = u8(expected); d[31] = u8(expected >> 8);
            if (u16(sum) != expected)
                return Status::io_error("ext4 checked group descriptor checksum");
            auto &g = groups_[group];
            g = {le32(d, 0), le32(d, 4), le32(d, 8), le16(d, 24), le16(d, 26), le16(d, 18)};
            const u32 begin = first_data_ + group * blocks_per_group_;
            const u32 count = u32(layout_.filesystem_blocks - begin < blocks_per_group_
                                   ? layout_.filesystem_blocks - begin : blocks_per_group_);
            const std::array<jbd2::BlockRange, 4> ranges{{{begin, 2}, {g.block_bitmap, 1},
                                                       {g.inode_bitmap, 1}, {g.table, table_blocks_}}};
            for (usize index = 0; index < ranges.size(); ++index)
                if (ranges[index].first != binding.metadata[group * 4 + index].first ||
                    ranges[index].count != binding.metadata[group * 4 + index].count)
                    return Status::unsupported("ext4 checked group placement changed");
            if ((g.flags & ~7U) || le16(d, 12) > count || le16(d, 14) > inodes_per_group_ ||
                le16(d, 16) > inodes_per_group_ - le16(d, 14) || le16(d, 28) > inodes_per_group_ ||
                ((g.flags & 1) && (le16(d, 14) != inodes_per_group_ || le16(d, 16) ||
                                   le16(d, 28) != inodes_per_group_ || g.inode_checksum)) ||
                ((g.flags & 2) && (count < table_blocks_ + 4 || le16(d, 12) != count - table_blocks_ - 4 ||
                                   g.block_checksum)))
                return Status::io_error("ext4 checked group counts or initialization");
        }
        return Status::success();
    }
    [[nodiscard]] Status allocation(u32 first, u32 count)
    {
        std::array<u8, 4096> data{};
        return allocation(first, count, data);
    }
    [[nodiscard]] Status allocation(u32 first, u32 count, std::array<u8, 4096> &data)
    {
        using namespace ext4_checked;
        if (!count || first < first_data_ || first >= layout_.filesystem_blocks || count > layout_.filesystem_blocks - first)
            return Status::io_error("ext4 checked data extent bounds");
        const u64 end = u64{first} + count;
        if (first < layout_.journal_first_block + layout_.journal_blocks && layout_.journal_first_block < end)
            return Status::io_error("ext4 checked data aliases journal");
        for (u32 group = (first - first_data_) / blocks_per_group_; group < group_count_; ++group)
        {
            const auto &g = groups_[group];
            const u32 begin = first_data_ + group * blocks_per_group_;
            if (begin >= end)
                break;
            const u64 stop = end - begin < blocks_per_group_ ? end : u64{begin} + blocks_per_group_;
            const u32 start = first > begin ? first : begin;
            if (start < begin + 2 || (g.block_bitmap >= start && g.block_bitmap < stop) ||
                (g.inode_bitmap >= start && g.inode_bitmap < stop) ||
                (start < u64{g.table} + table_blocks_ && g.table < stop) || (g.flags & 2))
                return Status::io_error("ext4 checked data aliases metadata or implicit free blocks");
            if (const auto status = block(g.block_bitmap, data); !status)
                return status;
            if (u16(jbd2::detail::crc32c(seed_, std::span<const u8>{data}.first(blocks_per_group_ / 8))) != g.block_checksum)
                return Status::io_error("ext4 checked block bitmap checksum");
            for (u64 physical = start; physical < stop; ++physical)
                if (!(data[(physical - begin) / 8] & (1U << ((physical - begin) % 8))))
                    return Status::io_error("ext4 checked data block is not allocated");
        }
        return Status::success();
    }
    [[nodiscard]] Status decode_inode(u32 number, std::span<u8, 256> out)
    {
        std::array<u8, 4096> data{};
        return decode_inode(number, out, data);
    }
    [[nodiscard]] Status decode_inode(u32 number, std::span<u8, 256> out, std::array<u8, 4096> &data)
    {
        using namespace ext4_checked;
        if (!number || number > inodes_)
            return Status::not_found("ext4 checked inode number");
        const auto &g = groups_[(number - 1) / inodes_per_group_];
        if (g.flags & 1)
            return Status::not_found("ext4 checked inode group is empty");
        if (const auto status = block(g.inode_bitmap, data); !status)
            return status;
        if (u16(jbd2::detail::crc32c(seed_, std::span<const u8>{data}.first(inodes_per_group_ / 8))) != g.inode_checksum)
            return Status::io_error("ext4 checked inode bitmap checksum");
        const u32 local = (number - 1) % inodes_per_group_;
        if (!(data[local / 8] & (1U << (local % 8))))
            return Status::not_found("ext4 checked inode is not allocated");
        const u32 offset = local * 256;
        if (const auto status = block(g.table + offset / layout_.block_size, data); !status)
            return status;
        for (usize index = 0; index < out.size(); ++index)
            out[index] = data[offset % layout_.block_size + index];
        if (const auto status = inode_checksum(seed_, number, out); !status)
            return status;
        const u32 type = le16(out, 0) & 0170000;
        const bool linked = le16(out, 26) && !le32(out, 20);
        const bool retained_orphan = number == retained_orphan_ && type == 0100000 &&
            !le16(out, 26) && le32(out, 20) == retained_orphan_next_ &&
            le32(out, 100) == retained_orphan_generation_;
        if ((type != 0100000 && type != 0040000 && type != 0120000) || (!linked && !retained_orphan) ||
            le32(out, 104) || le32(out, 108) || le32(out, 112) || le16(out, 116) || le16(out, 118))
            return Status::unsupported("ext4 checked inode type, size or external metadata");
        if (type == 0120000 && !le32(out, 28))
            return !le32(out, 32) && le32(out, 4) > 0 && le32(out, 4) < 60
                       ? Status::success() : Status::unsupported("ext4 checked fast symlink representation");
        if (type == 0120000 && (le32(out, 4) < 60 || le32(out, 4) >= layout_.block_size ||
                                  le32(out, 28) != layout_.block_size / 512))
            return Status::unsupported("ext4 checked one-block symlink size");
        if (type == 0120000 && !le32(out, 32))
        {
            if (le32(out, 4) < 60 || le32(out, 4) >= layout_.block_size ||
                le32(out, 28) != layout_.block_size / 512)
                return Status::unsupported("ext4 checked direct-block symlink size");
            for (usize offset = 44; offset < 100; ++offset)
                if (out[offset])
                    return Status::unsupported("ext4 checked direct-block symlink pointers");
            return allocation(le32(out, 40), 1, data);
        }
        if (le32(out, 32) != 0x80000 || le16(out, 40) != 0xf30a || le16(out, 42) > 4 ||
            le16(out, 44) != 4 || le16(out, 46))
            return Status::unsupported("ext4 checked reader requires inline extents");
        if (type == 0120000 && (le16(out, 42) != 1 || le32(out, 52) || le16(out, 56) != 1))
            return Status::unsupported("ext4 checked symlink requires one initialized extent");
        u64 logical_end = 0;
        u32 allocated_blocks = 0;
        for (usize index = 0; index < le16(out, 42); ++index)
        {
            const usize at = 52 + index * 12;
            const u32 logical = le32(out, at), encoded_length = le16(out, at + 4);
            const u32 length = encoded_length > 32768 ? encoded_length - 32768 : encoded_length;
            const u32 physical = le32(out, at + 8);
            if (!length || logical < logical_end || u64{logical} + length > (u64{1} << 32) || le16(out, at + 6))
                return Status::io_error("ext4 checked extent encoding or order");
            logical_end = u64{logical} + length;
            for (usize previous = 0; previous < index; ++previous)
            {
                const usize prior = 52 + previous * 12;
                const u32 prior_first = le32(out, prior + 8), prior_count = le16(out, prior + 4);
                const u32 prior_length = prior_count > 32768 ? prior_count - 32768 : prior_count;
                if (physical < u64{prior_first} + prior_length && prior_first < u64{physical} + length)
                    return Status::io_error("ext4 checked overlapping physical extents");
            }
            if (const auto status = allocation(physical, length, data); !status)
                return status;
            allocated_blocks += length;
        }
        if (le32(out, 28) != allocated_blocks * (layout_.block_size / 512))
            return Status::io_error("ext4 checked inode sector accounting");
        return Status::success();
    }
    [[nodiscard]] static Ext4ReadOnlyInode information(u32 number, std::span<const u8> inode)
    {
        using namespace ext4_checked;
        return {number, u32{le16(inode, 2)} | (u32{le16(inode, 120)} << 16),
                u32{le16(inode, 24)} | (u32{le16(inode, 122)} << 16), le32(inode, 4), le32(inode, 100),
                le16(inode, 0), le16(inode, 26)};
    }
    [[nodiscard]] static u32 mapped(std::span<const u8> inode, u32 logical)
    {
        using namespace ext4_checked;
        if ((le16(inode, 0) & 0170000) == 0120000 && !le32(inode, 32))
            return logical == 0 ? le32(inode, 40) : 0;
        for (usize index = 0; index < le16(inode, 42); ++index)
        {
            const usize at = 52 + index * 12;
            const u32 start = le32(inode, at), count = le16(inode, at + 4);
            const u32 length = count > 32768 ? count - 32768 : count;
            if (logical >= start && logical - start < length)
                return count > 32768 ? 0 : le32(inode, at + 8) + logical - start;
        }
        return 0;
    }
    [[nodiscard]] Status directory_block(u32 number, std::span<const u8> inode, std::span<const u8> bytes)
    {
        using namespace ext4_checked;
        const usize tail = bytes.size() - 12;
        if (le32(bytes, tail) || le16(bytes, tail + 4) != 12 || bytes[tail + 6] || bytes[tail + 7] != 0xde ||
            jbd2::detail::crc32c(inode_seed(seed_, number, inode), bytes.first(tail)) != le32(bytes, tail + 8))
            return Status::io_error("ext4 checked directory tail or checksum");
        for (usize offset = 0; offset < tail;)
        {
            if (tail - offset < 8)
                return Status::io_error("ext4 checked short directory record");
            const u32 length = le16(bytes, offset + 4), name_length = bytes[offset + 6];
            const u32 child = le32(bytes, offset), type = bytes[offset + 7];
            if (length < 8 || length % 4 || length > tail - offset || name_length > length - 8 || child > inodes_ ||
                (child && (!name_length || type > 7)))
                return Status::io_error("ext4 checked directory entry bounds");
            if (child)
                for (usize index = 0; index < name_length; ++index)
                    if (!bytes[offset + 8 + index] || bytes[offset + 8 + index] == '/')
                        return Status::io_error("ext4 checked invalid directory name");
            offset += length;
        }
        return Status::success();
    }

    Reader *reader_{};
    jbd2::RecoveryLayout layout_{};
    std::array<u8, 16> uuid_{};
    std::array<Group, Ext4RecoveryInfo::group_capacity> groups_{};
    u32 seed_{}, first_data_{}, blocks_per_group_{}, inodes_per_group_{}, group_count_{}, inodes_{}, first_inode_{}, table_blocks_{};
    std::array<OwnershipRange, 128 * 4> ownership_ranges_{};
    usize ownership_range_count_{};
    u32 retained_orphan_{}, retained_orphan_generation_{}, retained_orphan_next_{};
    bool attempted_{}, ready_{}, failed_{};
};
} // namespace ok::fs
