#include "ok/fs/mode_policy.hpp"
#include "ok/fs/ext4/layout.hpp"
#include "ok/arch/realtime.hpp"

namespace ok::fs
{
namespace
{
u16 r16(const std::byte *p)
{
    return std::to_integer<u16>(p[0]) | (std::to_integer<u16>(p[1]) << 8);
}
u32 r32(const std::byte *p)
{
    return r16(p) | (static_cast<u32>(r16(p + 2)) << 16);
}
void w16(std::byte *p, u16 v)
{
    p[0] = std::byte(v);
    p[1] = std::byte(v >> 8);
}
void w32(std::byte *p, u32 v)
{
    w16(p, static_cast<u16>(v));
    w16(p + 2, static_cast<u16>(v >> 16));
}
u32 file_size(const std::byte *p)
{
    return r32(p + 4);
}
constexpr u32 extents_flag = 0x80000;
constexpr u16 extent_magic = 0xf30a;
constexpr usize fast_symlink_capacity = 60;
u8 directory_type(NodeType type)
{
    return type == NodeType::directory ? 2 : type == NodeType::symlink ? 7 : 1;
}
u16 aligned_entry(usize length)
{
    return static_cast<u16>((8 + length + 3) & ~usize{3});
}
bool valid_name(std::string_view name)
{
    return !name.empty() && name != "." && name != ".." && name.size() < max_path_segment &&
           name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}
Status corrupt()
{
    return Status::io_error("invalid ext4 metadata");
}
bool power_of(u32 value, u32 base)
{
    while (value > 1 && value % base == 0)
        value /= base;
    return value == 1;
}
} // namespace

Status Ext4FileSystem::begin_operation(const Ext4Node *node, bool mounting)
{
    smp::ScopedSpinLock guard(identity_lock_);
    if (operation_active_) return Status::busy("ext4 backend operation is active");
    if (mounting)
    {
        if (mounted_) return Status::busy("ext4 already published; cache reinitialization is forbidden");
    }
    else
    {
        if (!mounted_) return Status::not_initialized("ext4 unmounted");
        if (node && (node->fs_ != this ||
            (node->phase_ != Ext4Node::Phase::linked &&
             !(node->phase_ == Ext4Node::Phase::detached && node->references_))))
            return Status::not_found("ext4 node identity is no longer live");
    }
    operation_active_ = true;
    return {};
}
Status Ext4FileSystem::drain_detached()
{
    for (;;)
    {
        u32 number{};
        {
            smp::ScopedSpinLock guard(identity_lock_);
            if (!operation_active_) return Status::fault("ext4 cleanup lacks operation admission");
            for (const auto &node : nodes_)
                if (node.phase_ == Ext4Node::Phase::detached && !node.references_)
                { number = node.ino_; break; }
        }
        if (!number) return {};
        if (failed_) return Status::io_error("ext4 I/O failed; cleanup is quarantined");
        if (auto status = destroy(number); !status)
        {
            smp::ScopedSpinLock guard(identity_lock_);
            failed_ = true;
            return status;
        }
    }
}
Status Ext4FileSystem::finish_operation()
{
    for (;;)
    {
        const auto status = drain_detached();
        smp::ScopedSpinLock guard(identity_lock_);
        if (!status)
        {
            operation_active_ = false;
            return status;
        }
        bool ready{};
        for (const auto &node : nodes_)
            if (node.phase_ == Ext4Node::Phase::detached && !node.references_) ready = true;
        if (ready) continue;
        // Release cannot add a ready orphan between this check and flag clear.
        operation_active_ = false;
        return {};
    }
}
void Ext4FileSystem::detach(Ext4Node &node)
{
    smp::ScopedSpinLock guard(identity_lock_);
    node.phase_ = Ext4Node::Phase::detached;
}
bool Ext4FileSystem::owns(const Node &target) const
{
    // Node owner is fixed before publication. Identity state is checked under
    // the same memory lock, so a forged filesystem_identity cannot justify a cast.
    smp::ScopedSpinLock guard(identity_lock_);
    for (const auto &node : nodes_)
        if (&node == &target)
            return node.phase_ == Ext4Node::Phase::linked ||
                   (node.phase_ == Ext4Node::Phase::detached && node.references_);
    return false;
}
Node *Ext4FileSystem::root()
{
    smp::ScopedSpinLock guard(identity_lock_);
    if (!mounted_ || nodes_[0].phase_ != Ext4Node::Phase::linked) return nullptr;
    nodes_[0].borrowed_identity_exposed_ = true;
    return &nodes_[0];
}
Status Ext4FileSystem::mount(driver::BlockDevice &device, Ext4MountOptions options)
{ return operation(nullptr, [&] { return mount_active(device, options); }, true); }
Status Ext4FileSystem::sync()
{ return operation(nullptr, [&] { return sync_active(); }); }
Result<Metadata> Ext4Node::query_metadata() const
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return query_metadata_active(); });
}
Result<FileBuffer> Ext4Node::read(usize offset, usize count) const
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return read_active(offset, count); });
}
Status Ext4Node::write(usize offset, std::span<const std::byte> input)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return write_active(offset, input); });
}
Status Ext4Node::truncate(usize size)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return truncate_active(size); });
}
Result<Node *> Ext4Node::lookup_child(std::string_view name)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return lookup_active(name, false, true); });
}
Result<Node *> Ext4Node::lookup_child_retained(std::string_view name)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return lookup_active(name, true, false); }, false, true);
}
Result<Node *> Ext4Node::create_child(std::string_view name, const InitialNodeAttributes &attributes)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return create_active(name, attributes, false); });
}
Result<Node *> Ext4Node::create_child_retained(std::string_view name, const InitialNodeAttributes &attributes)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return create_active(name, attributes, true); }, false, true);
}
Status Ext4Node::create_symlink(std::string_view name, std::string_view target, u32 uid, u32 gid)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return create_symlink_active(name, target, uid, gid); });
}
Status Ext4Node::link(std::string_view name, Node &target)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return link_active(name, target); });
}
Status Ext4Node::rename(std::string_view old_name, Node &target, std::string_view new_name)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return rename_active(old_name, target, new_name); });
}
Result<FixedString<96>> Ext4Node::directory_path(const Node &root) const
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return directory_path_active(root); });
}
Status Ext4Node::remove(std::string_view name, bool directory)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return remove_active(name, directory); });
}
Status Ext4Node::chmod(u32 mode)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return chmod_active(mode); });
}
ModeChangeResult Ext4Node::change_mode(u32 mode, user::Credentials caller)
{
    if (!fs_) return ModeChangeResult::failure(Status::not_found("ext4 node has no owner"));
    return fs_->operation(this, [&] { return change_mode_active(mode, caller); });
}
Status Ext4Node::chown(u32 uid, u32 gid)
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return chown_active(uid, gid); });
}
// As on Linux since 2.2.13, an ownership change clears set-user-ID, and
// set-group-ID on a group-executable file, for root as well.
ModeChangeResult Ext4Node::change_owner(u32 uid, u32 gid, user::Credentials caller)
{
    if (!fs_) return ModeChangeResult::failure(Status::not_found("ext4 node has no owner"));
    if (caller.euid != user::root_uid)
        return ModeChangeResult::failure(Status::permission("ext4 ownership change requires root"));
    return ModeChangeResult::failure(fs_->operation(this, [&] { return chown_active(uid, gid, true); }));
}
Result<DirectoryListing> Ext4Node::list(DirectoryReadIntent intent) const
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return list_active(intent); });
}

Result<DirectoryListing> Ext4Node::list_from(usize start, DirectoryReadIntent intent) const
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return list_active(intent, start, true); });
}

Status Ext4FileSystem::mount(driver::BlockDevice &device, bool writable)
{
    return mount(device, Ext4MountOptions{writable, Ext4TimestampPolicy::unqualified});
}
Status Ext4FileSystem::mount_active(driver::BlockDevice &device, Ext4MountOptions options)
{
    const bool writable = options.writable;
    if ((options.timestamps != Ext4TimestampPolicy::unqualified &&
         options.timestamps != Ext4TimestampPolicy::maintained) ||
        (options.timestamps == Ext4TimestampPolicy::maintained && !writable))
        return Status::invalid_argument("maintained ext4 requires an explicit writable mount");
    if (mounted_)
        return Status::busy("ext4 already mounted");
    device_ = nullptr;
    failed_ = dirty_ = writable_ = false;
    write_sequence_ = 0;
    timestamps_ = options.timestamps;
    const auto geometry = device.geometry();
    if (geometry.block_size != 512 || geometry.block_count < 8)
        return Status::unsupported("ext4 requires 512-byte sectors");
    std::array<std::byte, 1024> super{};
    if (auto status = device.read_blocks(2, super); !status.ok())
        return status;
    const auto *s = super.data();
    if (r16(s + 56) != 0xef53 || r32(s + 24) > 2)
        return Status::invalid_argument("unsupported ext4 superblock");
    // No journal replay, metadata checksums, 64-bit descriptors, bigalloc,
    // inline data, encryption or indexed directories in this first writer.
    if (r32(s + 96) != 0x42u || (r32(s + 100) & ~3u) || r32(s + 92) || r16(s + 206) || r32(s + 72))
        return Status::unsupported("unsupported ext4 feature set");
    if (r16(s + 58) != 1)
        return Status::denied("ext4 requires a clean filesystem; run offline fsck");
    block_size_ = 1024u << r32(s + 24);
    blocks_ = r32(s + 4);
    inodes_ = r32(s);
    first_data_ = r32(s + 20);
    blocks_per_group_ = r32(s + 32);
    inodes_per_group_ = r32(s + 40);
    inode_size_ = r16(s + 88);
    first_inode_ = r32(s + 84);
    if (blocks_ <= first_data_ || !inodes_ || !blocks_per_group_ || !inodes_per_group_ ||
        blocks_per_group_ > block_size_ * 8 || inodes_per_group_ > block_size_ * 8 ||
        (inode_size_ != 128 && inode_size_ != 256) || first_inode_ < 11 || first_inode_ > inodes_ ||
        static_cast<u64>(blocks_) * (block_size_ / 512) > geometry.block_count ||
        first_data_ != (block_size_ == 1024 ? 1u : 0u))
        return io_status(corrupt());
    if (r32(s + 12) > blocks_ - first_data_ || r32(s + 16) > inodes_ || r32(s + 28) != r32(s + 24))
        return io_status(corrupt());
    groups_ = (blocks_ - first_data_ - 1) / blocks_per_group_ + 1;
    if (groups_ > 32 || (inodes_ - 1) / inodes_per_group_ + 1 != groups_)
        return Status::unsupported("ext4 group limit exceeded");
    if (writable && !geometry.writable)
        return Status::denied("block device is read-only");
    if (writable)
        if (auto status = device.flush(); !status.ok())
            return status;
    device_ = &device;
    writable_ = writable;
    for (u32 group = 0; group < groups_; ++group)
    {
        Block descriptors{};
        if (auto status = read_block(first_data_ + 1, descriptors); !status.ok())
            return status;
        const auto *d = descriptors.data() + group * 32;
        auto &layout = group_layout_[group];
        layout.begin = first_data_ + group * blocks_per_group_;
        layout.end = blocks_ - layout.begin < blocks_per_group_ ? blocks_ : layout.begin + blocks_per_group_;
        layout.block_bitmap = r32(d);
        layout.inode_bitmap = r32(d + 4);
        layout.table = r32(d + 8);
        layout.table_blocks =
            static_cast<u32>((static_cast<u64>(inodes_per_group_) * inode_size_ + block_size_ - 1) / block_size_);
        layout.superblock = !(r32(s + 100) & 1) || group == 0 || group == 1 || power_of(group, 3) ||
                            power_of(group, 5) || power_of(group, 7);
        const auto first = layout.begin + (layout.superblock ? 2 : 0);
        if (layout.block_bitmap < first || layout.block_bitmap >= layout.end || layout.inode_bitmap < first ||
            layout.inode_bitmap >= layout.end || layout.block_bitmap == layout.inode_bitmap || layout.table < first ||
            layout.table >= layout.end || layout.table_blocks > layout.end - layout.table ||
            (layout.block_bitmap >= layout.table && layout.block_bitmap - layout.table < layout.table_blocks) ||
            (layout.inode_bitmap >= layout.table && layout.inode_bitmap - layout.table < layout.table_blocks) ||
            r16(d + 12) > layout.end - layout.begin || r16(d + 14) > inodes_per_group_ ||
            r16(d + 16) > inodes_per_group_ - r16(d + 14) || (r16(d + 18) & ~4u))
            return io_status(corrupt());
    }
    auto root_inode = inode(2);
    if (!root_inode || (r16(root_inode.value().data()) & mode_type_mask) != mode_directory)
        return io_status(corrupt());
    if (maintained())
    {
        if (auto status = this->writable(root_inode.value()); !status)
            return status;
        TimestampEvent event;
        const TimestampTarget target{&root_inode.value(), Ext4TimestampFields::all};
        if (auto status = prepare_event(event, {&target, 1}); !status)
            return status;
    }
    {
        smp::ScopedSpinLock guard(identity_lock_);
        for (auto &node : nodes_) node.fs_ = this;
        nodes_[0].ino_ = 2;
        nodes_[0].phase_ = Ext4Node::Phase::linked;
        static_cast<void>(nodes_[0].name_.assign("/"));
        mounted_ = true;
    }
    return Status::success();
}
Status Ext4FileSystem::validate_times(const Inode &record) const
{
    FileAttributes attributes;
    return io_status(decode_ext4_times(std::span<const std::byte>{record}.first(inode_size_), attributes));
}
Status Ext4FileSystem::apply_event(const TimestampEvent &event, Inode &record,
                                  Ext4TimestampFields fields) const
{
    if (!event.active)
        return Status::success();
    return encode_ext4_times(std::span<std::byte>{record}.first(inode_size_), fields,
                             event.sample.value.seconds, event.sample.value.nanoseconds);
}
Status Ext4FileSystem::prepare_event(TimestampEvent &event, std::span<const TimestampTarget> targets)
{
    event = {};
    if (!maintained())
        return Status::success();
    if (failed_)
        return Status::io_error("maintained ext4 is quarantined");
    if (!writable_ || targets.empty() || targets.size() > 4)
        return Status::invalid_argument("invalid maintained ext4 event");
    std::array<Inode, 4> staged{};
    for (usize i = 0; i < targets.size(); ++i)
    {
        if (!targets[i].initialized)
            if (auto status = validate_times(*targets[i].record); !status)
                return status;
        staged[i] = *targets[i].record;
    }
    auto sample = arch::realtime_sample();
    if (!sample)
        return Status::io_error("maintained ext4 requires an active realtime clock");
    const auto &value = sample.value();
    if (value.value.nanoseconds >= time::nanoseconds_per_second || !value.wall_generation ||
        !value.monotonic.generation || (value.origin != time::RealtimeOrigin::boot_rtc &&
                                       value.origin != time::RealtimeOrigin::administrator_software))
        return Status::io_error("invalid active realtime sample");
    event.sample = value;
    event.active = true;
    for (usize i = 0; i < targets.size(); ++i)
        if (auto status = apply_event(event, staged[i], targets[i].fields); !status)
        {
            event.active = false;
            return status;
        }
    for (usize i = 0; i < targets.size(); ++i)
        *targets[i].record = staged[i];
    return Status::success();
}
Status Ext4FileSystem::access_event(u32 ino, Inode &record)
{
    if (!maintained())
        return Status::success();
    if (auto status = writable(record); !status)
        return status;
    TimestampEvent event;
    const TimestampTarget target{&record, Ext4TimestampFields::access};
    if (auto status = prepare_event(event, {&target, 1}); !status)
        return status;
    NamespaceMutation mutation(*this);
    if (auto status = save_inode(ino, record); !status)
        return status;
    mutation.committed = true;
    return Status::success();
}
Result<u32> Ext4FileSystem::available_number(bool is_inode, u32 after) const
{
    for (u32 group = 0; group < groups_; ++group)
    {
        Block buffer{};
        if (auto status = read_block(first_data_ + 1, buffer); !status)
            return status;
        const auto *descriptor = buffer.data() + group * 32;
        if (!r16(descriptor + (is_inode ? 14 : 12)))
            continue;
        const auto bitmap = r32(descriptor + (is_inode ? 4 : 0));
        if (auto status = read_block(bitmap, buffer); !status)
            return status;
        const auto per = is_inode ? inodes_per_group_ : blocks_per_group_;
        const auto base = is_inode ? group * per + 1 : first_data_ + group * per;
        const auto total = is_inode ? inodes_ + 1 : blocks_;
        for (u32 bit = 0; bit < per && base + bit < total; ++bit)
        {
            const auto number = base + bit;
            if (number <= after || (is_inode && number < first_inode_))
                continue;
            if (std::to_integer<u8>(buffer[bit / 8]) & (1u << (bit % 8)))
                continue;
            if (!is_inode && !data_block(number))
                return io_status(corrupt());
            return number;
        }
    }
    return Status::no_memory("ext4 allocation exhausted");
}
Result<u32> Ext4FileSystem::preflight_insert(const Inode &directory, std::string_view name, u32 after)
{
    const auto size = file_size(directory.data());
    if (size % block_size_)
        return io_status(corrupt());
    for (u32 logical = 0; logical < size / block_size_; ++logical)
    {
        auto physical = map(directory, logical);
        if (!physical)
            return physical.status();
        if (!physical.value())
            return io_status(corrupt());
        Block block{};
        if (auto status = read_block(physical.value(), block); !status)
            return status;
        for (u32 offset = 0; offset < block_size_;)
        {
            if (block_size_ - offset < 8)
                return io_status(corrupt());
            const auto *entry = block.data() + offset;
            const auto length = r16(entry + 4);
            const auto name_length = std::to_integer<u8>(entry[6]);
            if (length < 8 || length % 4 || length > block_size_ - offset || name_length > length - 8)
                return io_status(corrupt());
            const auto used = r32(entry) ? aligned_entry(name_length) : 0;
            if (length - used >= aligned_entry(name.size()))
                return u32{0};
            offset += length;
        }
    }
    if (size > 0xffffffffu - block_size_)
        return Status::no_space("ext4 directory size capacity exceeded");
    auto available = available_number(false, after);
    if (!available)
        return available.status();
    auto staged = directory;
    if (auto status = append(staged, size / block_size_, available.value()); !status)
        return status;
    return available.value();
}
Status Ext4FileSystem::preflight_write(const Inode &record, usize offset, usize count)
{
    auto staged = record;
    u32 after = 0;
    for (usize done = 0; done < count;)
    {
        const auto position = offset + done;
        const auto logical = static_cast<u32>(position / block_size_);
        auto physical = map(staged, logical);
        if (!physical)
            return physical.status();
        if (!physical.value())
        {
            auto available = available_number(false, after);
            if (!available)
                return available.status();
            after = available.value();
            if (auto status = append(staged, logical, after); !status)
                return status;
        }
        const auto remaining = block_size_ - position % block_size_;
        done += remaining < count - done ? remaining : count - done;
        if (offset + done > file_size(staged.data()))
            w32(staged.data() + 4, static_cast<u32>(offset + done));
    }
    return Status::success();
}
Status Ext4FileSystem::io_status(Status status) const
{
    if (!status.ok())
    {
        failed_ = true;
        return Status::io_error(status.message());
    }
    return status;
}
bool Ext4FileSystem::data_block(u32 block) const
{
    return data_range(block, 1);
}
bool Ext4FileSystem::data_range(u32 first, u32 count) const
{
    if (!count || first < first_data_ || first >= blocks_ || count > blocks_ - first)
        return false;
    const auto end = first + count;
    for (u32 group = (first - first_data_) / blocks_per_group_; group < groups_ && group_layout_[group].begin < end;
         ++group)
    {
        const auto &layout = group_layout_[group];
        if ((layout.superblock && first < layout.begin + 2) ||
            (layout.block_bitmap >= first && layout.block_bitmap < end) ||
            (layout.inode_bitmap >= first && layout.inode_bitmap < end) ||
            (first < layout.table + layout.table_blocks && layout.table < end))
            return false;
    }
    return true;
}
void Ext4FileSystem::restore_dirty_marker(std::array<std::byte, 1024> &super)
{
    // A failed clean-marker write/flush may have reached the device partially.
    // Quarantine the mount and make a best-effort durable dirty-marker repair.
    // A permanently failed medium cannot promise persistence of this repair.
    failed_ = true;
    dirty_ = true;
    w16(super.data() + 58, 0);
    static_cast<void>(device_->write_blocks(2, super));
    static_cast<void>(device_->flush());
}
Status Ext4FileSystem::sync_active()
{
    if (auto status = drain_detached(); !status) return status;
    if (failed_)
        return Status::io_error("ext4 I/O failed; offline fsck required");
    if (!device_)
        return Status::not_initialized("ext4 unmounted");
    if (auto status = io_status(device_->flush()); !status.ok())
        return status;
    if (!dirty_)
        return Status::success();
    // An unlinked open inode remains allocated. Keep the volume unclean until
    // its last description closes, so an interrupted session requires fsck.
    {
        smp::ScopedSpinLock guard(identity_lock_);
        for (const auto &node : nodes_)
            if (node.phase_ == Ext4Node::Phase::detached) return Status::success();
    }
    std::array<std::byte, 1024> super{};
    if (auto status = io_status(device_->read_blocks(2, super)); !status.ok())
        return status;
    w16(super.data() + 58, 1);
    if (auto status = io_status(device_->write_blocks(2, super)); !status.ok())
    {
        restore_dirty_marker(super);
        return status;
    }
    if (auto status = io_status(device_->flush()); !status.ok())
    {
        restore_dirty_marker(super);
        return status;
    }
    dirty_ = false;
    return Status::success();
}
Status Ext4FileSystem::read_block(u32 block, Block &out) const
{
    if (!device_ || block >= blocks_)
        return io_status(corrupt());
    return io_status(device_->read_blocks(static_cast<u64>(block) * (block_size_ / 512), {out.data(), block_size_}));
}
Status Ext4FileSystem::write_block(u32 block, const Block &data)
{
    if (failed_)
        return Status::io_error("ext4 I/O failed; further writes disabled");
    if (!writable_)
        return Status::denied("ext4 mounted read-only");
    if (!device_ || block >= blocks_)
        return io_status(corrupt());
    if (!dirty_)
    {
        std::array<std::byte, 1024> super{};
        if (auto status = io_status(device_->read_blocks(2, super)); !status.ok())
            return status;
        w16(super.data() + 58, 0);
        if (auto status = io_status(device_->write_blocks(2, super)); !status.ok())
            return status;
        if (auto status = io_status(device_->flush()); !status.ok())
            return status;
        dirty_ = true;
    }
    const u64 sector = static_cast<u64>(block) * (block_size_ / 512);
    if (block == 1024 / block_size_)
    {
        auto updated = data;
        w16(updated.data() + 1024 % block_size_ + 58, 0);
        const auto status = io_status(device_->write_blocks(sector, {updated.data(), block_size_}));
        if (status.ok())
            ++write_sequence_;
        return status;
    }
    const auto status = io_status(device_->write_blocks(sector, {data.data(), block_size_}));
    if (status.ok())
        ++write_sequence_;
    return status;
}
Result<Ext4FileSystem::Inode> Ext4FileSystem::inode(u32 ino) const
{
    if (maintained() && failed_)
        return Status::io_error("maintained ext4 is quarantined");
    if (!ino || ino > inodes_)
        return io_status(corrupt());
    const auto group = (ino - 1) / inodes_per_group_;
    Block buffer{};
    if (auto status = read_block(first_data_ + 1, buffer); !status.ok())
        return status;
    const auto table = r32(buffer.data() + group * 32 + 8);
    const auto offset = ((ino - 1) % inodes_per_group_) * inode_size_;
    if (auto status = read_block(table + offset / block_size_, buffer); !status.ok())
        return status;
    Inode out{};
    for (usize i = 0; i < inode_size_; ++i)
        out[i] = buffer[offset % block_size_ + i];
    if (r32(out.data() + 108))
        return Status::unsupported("ext4 files above 4 GiB are unsupported");
    if (maintained())
        if (auto status = validate_times(out); !status)
            return status;
    return out;
}
Status Ext4FileSystem::save_inode(u32 ino, const Inode &data)
{
    if (!ino || ino > inodes_)
        return io_status(corrupt());
    Block buffer{};
    if (auto status = read_block(first_data_ + 1, buffer); !status.ok())
        return status;
    const auto table = r32(buffer.data() + ((ino - 1) / inodes_per_group_) * 32 + 8);
    const auto offset = ((ino - 1) % inodes_per_group_) * inode_size_;
    const auto block = table + offset / block_size_;
    if (auto status = read_block(block, buffer); !status.ok())
        return status;
    for (usize i = 0; i < inode_size_; ++i)
        buffer[offset % block_size_ + i] = data[i];
    return write_block(block, buffer);
}
Result<u32> Ext4FileSystem::map(const Inode &node, u32 logical) const
{
    if (!(r32(node.data() + 32) & extents_flag))
        return Status::unsupported("ext4 inode is not extent-backed");
    const std::byte *header = node.data() + 40;
    usize available = 60;
    Block tree{};
    int expected_depth = -1;
    for (usize level = 0; level < 5; ++level)
    {
        const auto entries = r16(header + 2), maximum = r16(header + 4), depth = r16(header + 6);
        if (r16(header) != extent_magic || entries > maximum || maximum > (available - 12) / 12 || depth > 3 ||
            (expected_depth >= 0 && depth != expected_depth))
            return io_status(corrupt());
        if (!depth)
        {
            u64 previous_end = 0;
            for (u16 i = 0; i < entries; ++i)
            {
                const auto *e = header + 12 + i * 12;
                const auto start = r32(e);
                const auto count = r16(e + 4);
                const u32 length = count > 32768 ? count - 32768 : count;
                const auto physical = r32(e + 8);
                if (!length || start < previous_end || r16(e + 6) || static_cast<u64>(start) + length > u64{1} << 32 ||
                    physical >= blocks_ || length > blocks_ - physical)
                    return io_status(corrupt());
                previous_end = static_cast<u64>(start) + length;
                if (!data_range(physical, length))
                    return io_status(corrupt());
                if (logical >= start && logical - start < length)
                {
                    if (count > 32768)
                        return u32{0}; // Unwritten extent reads as zeros.
                    return physical + logical - start;
                }
            }
            return u32{0};
        }
        u32 next = 0;
        for (u16 i = 0; i < entries; ++i)
        {
            const auto *e = header + 12 + i * 12;
            if (r32(e) > logical)
                break;
            if (r16(e + 8))
                return Status::unsupported("64-bit extent index");
            next = r32(e + 4);
        }
        if (!next)
            return u32{0};
        if (!data_block(next))
            return io_status(corrupt());
        expected_depth = depth - 1;
        if (auto status = read_block(next, tree); !status.ok())
            return status;
        header = tree.data();
        available = block_size_;
    }
    return io_status(corrupt());
}
Result<u32> Ext4FileSystem::symlink_block(const Inode &node) const
{
    const auto *p = node.data();
    const auto size = file_size(p), flags = r32(p + 32), sectors = r32(p + 28);
    if ((r16(p) & mode_type_mask) != mode_symlink)
        return Status::invalid_argument("not a symbolic link");
    // Inline pathname bytes are never interpreted as block pointers. The
    // supported subset has no EA blocks, encryption or inline-data inodes.
    if ((flags != 0 && flags != extents_flag) || r32(p + 104) || r16(p + 116) || r16(p + 118))
        return Status::unsupported("unsupported ext4 symbolic link layout");
    if (!size || size >= block_size_)
        return io_status(corrupt());
    if (!sectors)
    {
        if (flags || size >= fast_symlink_capacity)
            return io_status(corrupt());
        return u32{0};
    }
    if (sectors != block_size_ / 512)
        return io_status(corrupt());
    u32 physical;
    if (flags == extents_flag)
    {
        // Linux stores a block symlink in one initialized logical block.
        if (r16(p + 40) != extent_magic || r16(p + 42) != 1 || r16(p + 44) != 4 || r16(p + 46) ||
            r32(p + 52) || r16(p + 56) != 1 || r16(p + 58))
            return io_status(corrupt());
        physical = r32(p + 60);
    }
    else
    {
        // Accept the canonical legacy one-direct-block symlink, without
        // introducing indirect block support for ordinary files.
        physical = r32(p + 40);
        for (usize offset = 44; offset < 100; offset += 4)
            if (r32(p + offset))
                return io_status(corrupt());
    }
    if (!data_block(physical))
        return io_status(corrupt());
    return physical;
}
Result<FileBuffer> Ext4FileSystem::read_symlink(const Inode &node, usize offset, usize count) const
{
    auto physical = symlink_block(node);
    if (!physical)
        return physical.status();
    Block block{};
    const std::byte *target = node.data() + 40;
    if (physical.value())
    {
        if (auto status = read_block(physical.value(), block); !status.ok())
            return status;
        target = block.data();
    }
    const auto size = file_size(node.data());
    if (target[size] != std::byte{0})
        return io_status(corrupt());
    for (usize i = 0; i < size; ++i)
        if (target[i] == std::byte{0})
            return io_status(corrupt());
    FileBuffer result{};
    if (offset >= size)
        return result;
    if (count > size - offset)
        count = size - offset;
    if (count > result.data.size())
        count = result.data.size();
    for (usize i = 0; i < count; ++i)
        result.data[i] = target[offset + i];
    result.size = count;
    return result;
}
Status Ext4FileSystem::writable(const Inode &node) const
{
    if (failed_)
        return Status::io_error("ext4 I/O or metadata failure; further writes disabled");
    if (!writable_)
        return Status::denied("ext4 mounted read-only");
    if ((r16(node.data()) & mode_type_mask) == mode_symlink)
        return symlink_block(node).status();
    if (r32(node.data() + 32) != extents_flag || r16(node.data() + 40) != extent_magic || r16(node.data() + 46) != 0 ||
        r16(node.data() + 42) > 4 || r16(node.data() + 44) != 4 || r32(node.data() + 104) || r16(node.data() + 116))
        return Status::unsupported("ext4 write requires inline plain extents");
    for (u16 i = 0; i < r16(node.data() + 42); ++i)
        if (r16(node.data() + 56 + i * 12) > 32768 || r16(node.data() + 58 + i * 12))
            return Status::unsupported("unwritten/64-bit extent mutation unsupported");
    const auto logical_limit = (static_cast<u64>(file_size(node.data())) + block_size_ - 1) / block_size_;
    u64 previous_end = 0, allocated = 0;
    for (u16 i = 0; i < r16(node.data() + 42); ++i)
    {
        const auto *extent = node.data() + 52 + i * 12;
        const auto logical = r32(extent), physical = r32(extent + 8);
        const auto length = r16(extent + 4);
        if (!length || logical < previous_end || physical >= blocks_ || length > blocks_ - physical)
            return io_status(corrupt());
        previous_end = static_cast<u64>(logical) + length;
        if (previous_end > logical_limit)
            return Status::unsupported("initialized extents beyond EOF are not writable");
        if (!data_range(physical, length))
            return io_status(corrupt());
        for (u16 j = 0; j < i; ++j)
        {
            const auto *other = node.data() + 52 + j * 12;
            const auto begin = r32(other + 8);
            if (physical < static_cast<u64>(begin) + r16(other + 4) && begin < static_cast<u64>(physical) + length)
                return io_status(corrupt());
        }
        allocated += length;
    }
    if (allocated * (block_size_ / 512) != r32(node.data() + 28))
        return io_status(corrupt());
    return Status::success();
}
Status Ext4FileSystem::append(Inode &node, u32 logical, u32 physical)
{
    if (auto status = writable(node); !status.ok())
        return status;
    auto *h = node.data() + 40;
    auto count = r16(h + 2);
    u16 position = 0;
    while (position < count && r32(h + 12 + position * 12) < logical)
        ++position;
    if (position < count && r32(h + 12 + position * 12) == logical)
        return io_status(corrupt());
    if (position > 0)
    {
        auto *previous = h + 12 + (position - 1) * 12;
        const auto length = r16(previous + 4);
        if (logical < r32(previous) + length)
            return io_status(corrupt());
        if (logical == r32(previous) + length && physical == r32(previous + 8) + length && length < 32768)
        {
            w16(previous + 4, length + 1);
            w32(node.data() + 28, r32(node.data() + 28) + block_size_ / 512);
            return Status::success();
        }
    }
    if (position < count)
    {
        auto *next = h + 12 + position * 12;
        const auto length = r16(next + 4);
        if (logical + 1 == r32(next) && physical + 1 == r32(next + 8) && length < 32768)
        {
            w32(next, logical);
            w32(next + 8, physical);
            w16(next + 4, length + 1);
            w32(node.data() + 28, r32(node.data() + 28) + block_size_ / 512);
            return Status::success();
        }
    }
    if (count >= 4)
        return Status::overflow("ext4 inline extent capacity exceeded");
    for (u16 i = count; i > position; --i)
        for (usize j = 0; j < 12; ++j)
            h[12 + i * 12 + j] = h[12 + (i - 1) * 12 + j];
    auto *entry = h + 12 + position * 12;
    w32(entry, logical);
    w16(entry + 4, 1);
    w16(entry + 6, 0);
    w32(entry + 8, physical);
    w16(h + 2, count + 1);
    w32(node.data() + 28, r32(node.data() + 28) + block_size_ / 512);
    return Status::success();
}
Status Ext4FileSystem::counts(u32 group, bool is_inode, int delta, bool directory)
{
    if (group >= groups_ || (delta != -1 && delta != 1))
        return io_status(corrupt());
    Block buffer{};
    if (auto status = read_block(first_data_ + 1, buffer); !status.ok())
        return status;
    auto *d = buffer.data() + group * 32;
    const usize field = is_inode ? 14 : 12;
    const auto value = static_cast<int>(r16(d + field)) + delta;
    const auto limit = is_inode ? inodes_per_group_ : group_layout_[group].end - group_layout_[group].begin;
    if (value < 0 || static_cast<u32>(value) > limit)
        return io_status(corrupt());
    w16(d + field, static_cast<u16>(value));
    if (is_inode && directory)
    {
        const auto directories = static_cast<int>(r16(d + 16)) - delta;
        if (directories < 0 || static_cast<u32>(directories) > inodes_per_group_)
            return io_status(corrupt());
        w16(d + 16, static_cast<u16>(directories));
    }
    Block super{};
    if (auto status = read_block(1024 / block_size_, super); !status.ok())
        return status;
    auto *s = super.data() + 1024 % block_size_;
    const usize superfield = is_inode ? 16 : 12;
    const auto old = r32(s + superfield);
    if ((delta < 0 && !old) || (delta > 0 && old == ~u32{0}))
        return io_status(corrupt());
    w32(s + superfield, delta < 0 ? old - 1 : old + 1);
    if (auto status = write_block(first_data_ + 1, buffer); !status.ok())
        return status;
    return write_block(1024 / block_size_, super);
}
Result<u32> Ext4FileSystem::allocate(bool is_inode, bool directory)
{
    for (u32 group = 0; group < groups_; ++group)
    {
        Block buffer{};
        if (auto status = read_block(first_data_ + 1, buffer); !status.ok())
            return status;
        const auto *d = buffer.data() + group * 32;
        if (!r16(d + (is_inode ? 14 : 12)))
            continue;
        const auto bitmap = r32(d + (is_inode ? 4 : 0));
        if (auto status = read_block(bitmap, buffer); !status.ok())
            return status;
        const auto per = is_inode ? inodes_per_group_ : blocks_per_group_;
        const auto base = is_inode ? group * per + 1 : first_data_ + group * per;
        const auto total = is_inode ? inodes_ + 1 : blocks_;
        for (u32 bit = 0; bit < per && base + bit < total; ++bit)
        {
            if (is_inode && base + bit < first_inode_)
                continue;
            auto byte = std::to_integer<u8>(buffer[bit / 8]);
            const u8 mask = static_cast<u8>(1u << (bit % 8));
            if (byte & mask)
                continue;
            if (!is_inode && !data_block(base + bit))
                return io_status(corrupt());
            buffer[bit / 8] = std::byte(byte | mask);
            if (auto status = write_block(bitmap, buffer); !status.ok())
                return status;
            if (auto status = counts(group, is_inode, -1, directory); !status.ok())
                return status;
            return base + bit;
        }
    }
    return Status::no_memory("ext4 allocation exhausted");
}
Status Ext4FileSystem::free_number(u32 number, bool is_inode, bool directory)
{
    if (number < (is_inode ? first_inode_ : first_data_) || number >= (is_inode ? inodes_ + 1 : blocks_))
        return io_status(corrupt());
    if (!is_inode && !data_block(number))
        return io_status(corrupt());
    const auto per = is_inode ? inodes_per_group_ : blocks_per_group_;
    const auto index = number - (is_inode ? 1 : first_data_);
    const auto group = index / per, bit = index % per;
    Block buffer{};
    if (auto status = read_block(first_data_ + 1, buffer); !status.ok())
        return status;
    const auto bitmap = r32(buffer.data() + group * 32 + (is_inode ? 4 : 0));
    if (auto status = read_block(bitmap, buffer); !status.ok())
        return status;
    const auto byte = std::to_integer<u8>(buffer[bit / 8]);
    const auto mask = static_cast<u8>(1u << (bit % 8));
    if (!(byte & mask))
        return io_status(corrupt());
    buffer[bit / 8] = std::byte(byte & ~mask);
    if (auto status = write_block(bitmap, buffer); !status.ok())
        return status;
    return counts(group, is_inode, 1, directory);
}
Result<Ext4FileSystem::Location> Ext4FileSystem::find(const Inode &directory, std::string_view name) const
{
    if ((r16(directory.data()) & mode_type_mask) != mode_directory)
        return Status::invalid_argument("not a directory");
    const auto size = file_size(directory.data());
    if (size % block_size_)
        return io_status(corrupt());
    for (u32 logical = 0; logical < size / block_size_; ++logical)
    {
        auto physical = map(directory, logical);
        if (!physical)
            return physical.status();
        if (!physical.value())
            return io_status(corrupt());
        Block data{};
        if (auto status = read_block(physical.value(), data); !status.ok())
            return status;
        for (u32 offset = 0; offset < block_size_;)
        {
            if (block_size_ - offset < 8)
                return io_status(corrupt());
            const auto *e = data.data() + offset;
            const auto length = r16(e + 4);
            const auto namelen = std::to_integer<u8>(e[6]);
            if (length < 8 || length % 4 || length > block_size_ - offset || namelen > length - 8)
                return io_status(corrupt());
            if (r32(e) && std::string_view(reinterpret_cast<const char *>(e + 8), namelen) == name)
                return Location{physical.value(), static_cast<u16>(offset), length};
            offset += length;
        }
    }
    return Status::not_found("ext4 directory entry not found");
}
Result<u32> Ext4FileSystem::child(const Inode &directory, std::string_view name) const
{
    auto location = find(directory, name);
    if (!location)
        return location.status();
    Block buffer{};
    if (auto status = read_block(location.value().block, buffer); !status.ok())
        return status;
    return r32(buffer.data() + location.value().offset);
}
Status Ext4FileSystem::insert(u32 parent, Inode &directory, std::string_view name, u32 ino, NodeType type)
{
    const auto needed = aligned_entry(name.size());
    const auto count = file_size(directory.data()) / block_size_;
    for (u32 logical = 0; logical <= count; ++logical)
    {
        Block data{};
        u32 physical = 0;
        if (logical == count)
        {
            auto allocation = allocate(false);
            if (!allocation)
                return allocation.status();
            physical = allocation.value();
            if (auto status = append(directory, logical, physical); !status.ok())
            {
                if (!failed_)
                    static_cast<void>(free_number(physical, false));
                return status;
            }
            w16(data.data() + 4, static_cast<u16>(block_size_));
            w32(directory.data() + 4, (logical + 1) * block_size_);
        }
        else
        {
            auto block = map(directory, logical);
            if (!block || !block.value())
                return io_status(corrupt());
            physical = block.value();
            if (auto status = read_block(physical, data); !status.ok())
                return status;
        }
        for (u32 offset = 0; offset < block_size_;)
        {
            if (block_size_ - offset < 8)
                return io_status(corrupt());
            auto *entry = data.data() + offset;
            const auto length = r16(entry + 4);
            const auto namelen = std::to_integer<u8>(entry[6]);
            if (length < 8 || length % 4 || length > block_size_ - offset || namelen > length - 8)
                return io_status(corrupt());
            const auto used = r32(entry) ? aligned_entry(namelen) : 0;
            if (length - used >= needed)
            {
                if (used)
                {
                    w16(entry + 4, used);
                    entry += used;
                }
                w32(entry, ino);
                w16(entry + 4, length - used);
                entry[6] = std::byte(name.size());
                entry[7] = std::byte(directory_type(type));
                for (usize i = 0; i < name.size(); ++i)
                    entry[8 + i] = std::byte(name[i]);
                if (auto status = write_block(physical, data); !status.ok())
                    return status;
                if (type == NodeType::directory)
                    w16(directory.data() + 26, r16(directory.data() + 26) + 1);
                return save_inode(parent, directory);
            }
            offset += length;
        }
    }
    return Status::no_memory("ext4 directory full");
}
Status Ext4FileSystem::zero_tail(const Inode &node)
{
    const auto size = file_size(node.data());
    if (!(size % block_size_))
        return Status::success();
    auto block = map(node, size / block_size_);
    if (!block)
        return block.status();
    if (!block.value())
        return Status::success();
    Block data{};
    if (auto status = read_block(block.value(), data); !status.ok())
        return status;
    for (usize i = size % block_size_; i < block_size_; ++i)
        data[i] = std::byte{0};
    return write_block(block.value(), data);
}

// Storage-only helper: public truncate stages its event; orphan reclamation
// must not acquire another clock or invent a truncation event.
Status Ext4FileSystem::resize(u32 ino, Inode &node, usize size)
{
    if (auto status = writable(node); !status.ok())
        return status;
    if (size > 0xffffffffu)
        return Status::overflow("ext4 file too large");
    const auto old = file_size(node.data());
    if (size > old)
        if (auto status = zero_tail(node); !status.ok())
            return status;
    if (size < old)
    {
        const auto keep = static_cast<u32>((static_cast<u64>(size) + block_size_ - 1) / block_size_);
        auto *header = node.data() + 40;
        auto count = r16(header + 2);
        for (u16 i = count; i > 0; --i)
        {
            auto *e = header + 12 + (i - 1) * 12;
            const auto start = r32(e), physical = r32(e + 8);
            const auto length = r16(e + 4);
            const u32 retain = keep > start ? (keep - start < length ? keep - start : length) : 0;
            for (u32 j = retain; j < length; ++j)
                if (auto status = free_number(physical + j, false); !status.ok())
                    return status;
            w32(node.data() + 28, r32(node.data() + 28) - (length - retain) * (block_size_ / 512));
            if (!retain)
            {
                --count;
                for (usize j = 0; j < 12; ++j)
                    e[j] = std::byte{0};
            }
            else
                w16(e + 4, static_cast<u16>(retain));
        }
        w16(header + 2, count);
        if (size % block_size_)
        {
            auto block = map(node, static_cast<u32>(size / block_size_));
            if (!block)
                return block.status();
            if (block.value())
            {
                Block data{};
                if (auto status = read_block(block.value(), data); !status.ok())
                    return status;
                for (usize i = size % block_size_; i < block_size_; ++i)
                    data[i] = std::byte{0};
                if (auto status = write_block(block.value(), data); !status.ok())
                    return status;
            }
        }
    }
    w32(node.data() + 4, static_cast<u32>(size));
    return save_inode(ino, node);
}
Status Ext4FileSystem::destroy(u32 ino)
{
    Ext4Node *victim{};
    {
        smp::ScopedSpinLock guard(identity_lock_);
        if (!operation_active_ || ino < first_inode_)
            return Status::busy("ext4 destruction requires admission and a nonreserved inode");
        for (auto &node : nodes_)
            if (node.ino_ == ino && node.phase_ == Ext4Node::Phase::detached) { victim = &node; break; }
        if (!victim) return Status::not_found("ext4 detached identity is absent");
        if (victim->references_) return Status::busy("ext4 detached inode remains held");
        victim->phase_ = Ext4Node::Phase::retiring;
    }
    auto data = inode(ino);
    if (!data)
        return data.status();
    if (r16(data.value().data() + 26)) return io_status(corrupt());
    NamespaceMutation mutation(*this, maintained());
    const bool directory = (r16(data.value().data()) & mode_type_mask) == mode_directory;
    if ((r16(data.value().data()) & mode_type_mask) == mode_symlink)
    {
        if (auto status = writable(data.value()); !status.ok())
            return status;
        auto block = symlink_block(data.value());
        if (!block)
            return block.status();
        if (block.value())
            if (auto status = free_number(block.value(), false); !status.ok())
                return status;
    }
    else if (auto status = resize(ino, data.value(), 0); !status.ok())
        return status;
    data.value() = {}; // Clear the released inode, including ext4 orphan linkage.
    if (auto status = save_inode(ino, data.value()); !status.ok())
        return status;
    if (auto status = free_number(ino, true, directory); !status.ok())
        return status;
    {
        smp::ScopedSpinLock guard(identity_lock_);
        if (victim->borrowed_identity_exposed_) victim->phase_ = Ext4Node::Phase::tombstone;
        else
        {
            victim->ino_ = 0;
            victim->name_ = {};
            victim->phase_ = Ext4Node::Phase::vacant;
        }
    }
    mutation.committed = true;
    return Status::success();
}
Ext4Node *Ext4FileSystem::node(u32 ino, std::string_view name)
{
    smp::ScopedSpinLock guard(identity_lock_);
    if (!ino || ino > inodes_)
    {
        failed_ = true;
        return nullptr;
    }
    for (auto &node : nodes_)
        if (node.ino_ == ino && node.phase_ == Ext4Node::Phase::linked)
            return &node;
    for (auto &node : nodes_)
        if (node.phase_ == Ext4Node::Phase::vacant && !node.borrowed_identity_exposed_ && !node.references_)
        {
            if (!node.name_.assign(name).ok())
                return nullptr;
            node.ino_ = ino;
            node.references_ = 0;
            node.phase_ = Ext4Node::Phase::linked;
            return &node;
        }
    return nullptr;
}
Metadata Ext4FileSystem::metadata(const Inode &node) const
{
    const auto *p = node.data();
    const auto mode = r16(p);
    return Metadata{.type = (mode & mode_type_mask) == mode_directory ? NodeType::directory
                          : (mode & mode_type_mask) == mode_symlink ? NodeType::symlink : NodeType::regular,
                    .size = file_size(p),
                    .mode = mode,
                    .uid = r16(p + 2) | (static_cast<u32>(r16(p + 120)) << 16),
                    .gid = r16(p + 24) | (static_cast<u32>(r16(p + 122)) << 16),
                    .link_count = r16(p + 26),
                    .block_size = block_size_,
                    .blocks = r32(p + 28)};
}
Metadata Ext4Node::metadata() const
{
    const auto result = query_metadata();
    return result ? result.value() : Metadata{};
}
Result<Metadata> Ext4Node::query_metadata_active() const
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    auto data = fs_->inode(ino_);
    if (!data)
        return data.status();
    auto metadata = fs_->metadata(data.value());
    metadata.attributes.inode = ino_;
    metadata.attributes.mark(Attribute::inode);
    auto allocation = ext4_allocated_blocks512(std::span<const std::byte>{data.value()}.first(fs_->inode_size_),
                                               fs_->block_size_, fs_->blocks_,
                                               [&](u32 start, u32 count) { return fs_->data_range(start, count); });
    if (allocation)
    {
        metadata.blocks = allocation.value();
        metadata.attributes.mark(Attribute::blocks512);
    }
    else if (allocation.status().code() != StatusCode::unsupported)
        return fs_->io_status(allocation.status());
    if (!fs_->writable_ || fs_->maintained())
        if (auto status = decode_ext4_times(std::span<const std::byte>{data.value()}.first(fs_->inode_size_),
                                            metadata.attributes); !status)
            return fs_->io_status(status);
    if (fs_->maintained())
        metadata.attributes.access.source = metadata.attributes.modification.source =
            metadata.attributes.change.source = TimestampSource::maintained_ext4;
    return metadata;
}
Result<FileBuffer> Ext4Node::read_active(usize offset, usize count) const
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    auto data = fs_->inode(ino_);
    if (!data)
        return data.status();
    if ((r16(data.value().data()) & mode_type_mask) == mode_symlink)
    {
        auto output = fs_->read_symlink(data.value(), offset, count);
        if (!output)
            return output.status();
        if (count)
            if (auto status = fs_->access_event(ino_, data.value()); !status)
                return status;
        return output;
    }
    if ((r16(data.value().data()) & mode_type_mask) != mode_regular)
        return Status::invalid_argument("not a regular file");
    const auto size = file_size(data.value().data());
    FileBuffer out{};
    const bool accessed = count != 0;
    count = offset >= size ? 0 : (count < size - offset ? count : size - offset);
    count = count < out.data.size() ? count : out.data.size();
    while (out.size < count)
    {
        const auto position = offset + out.size;
        auto physical = fs_->map(data.value(), static_cast<u32>(position / fs_->block_size_));
        if (!physical)
            return physical.status();
        Ext4FileSystem::Block buffer{};
        if (physical.value())
            if (auto status = fs_->read_block(physical.value(), buffer); !status.ok())
                return status;
        const auto within = position % fs_->block_size_;
        auto length = fs_->block_size_ - within;
        if (length > count - out.size)
            length = count - out.size;
        for (usize i = 0; i < length; ++i)
            out.data[out.size + i] = buffer[within + i];
        out.size += length;
    }
    if (accessed)
        if (auto status = fs_->access_event(ino_, data.value()); !status)
            return status;
    return out;
}
Status Ext4Node::write_active(usize offset, std::span<const std::byte> input)
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    auto data = fs_->inode(ino_);
    if (!data)
        return data.status();
    auto &node = data.value();
    if ((r16(node.data()) & mode_type_mask) != mode_regular)
        return Status::invalid_argument("not a regular file");
    if (auto status = fs_->writable(node); !status.ok())
        return status;
    if (offset > 0xffffffffu || input.size() > 0xffffffffu - offset)
        return Status::overflow("ext4 file too large");
    if (input.empty())
        return Status::success();
    if (fs_->maintained())
        if (auto status = fs_->preflight_write(node, offset, input.size()); !status)
            return status;
    Ext4FileSystem::TimestampEvent event;
    const Ext4FileSystem::TimestampTarget target{&node,
        Ext4TimestampFields::modification | Ext4TimestampFields::change};
    if (auto status = fs_->prepare_event(event, {&target, 1}); !status)
        return status;
    Ext4FileSystem::NamespaceMutation mutation(*fs_, fs_->maintained());
    if (offset > file_size(node.data()))
        if (auto status = fs_->zero_tail(node); !status.ok())
            return status;
    usize done = 0;
    while (done < input.size())
    {
        const auto position = offset + done;
        const auto logical = static_cast<u32>(position / fs_->block_size_);
        auto mapped = fs_->map(node, logical);
        if (!mapped)
            return mapped.status();
        auto physical = mapped.value();
        Ext4FileSystem::Block buffer{};
        if (!physical)
        {
            auto allocation = fs_->allocate(false);
            if (!allocation)
                return allocation.status();
            physical = allocation.value();
            if (auto status = fs_->append(node, logical, physical); !status.ok())
            {
                if (!fs_->failed_)
                    static_cast<void>(fs_->free_number(physical, false));
                return status;
            }
        }
        else if (auto status = fs_->read_block(physical, buffer); !status.ok())
            return status;
        const auto within = position % fs_->block_size_;
        auto length = fs_->block_size_ - within;
        if (length > input.size() - done)
            length = input.size() - done;
        for (usize i = 0; i < length; ++i)
            buffer[within + i] = input[done + i];
        if (auto status = fs_->write_block(physical, buffer); !status.ok())
            return status;
        done += length;
        if (offset + done > file_size(node.data()))
            w32(node.data() + 4, static_cast<u32>(offset + done));
        if (auto status = fs_->save_inode(ino_, node); !status.ok())
            return status;
    }
    mutation.committed = true;
    return Status::success();
}
Status Ext4Node::truncate_active(usize size)
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    auto data = fs_->inode(ino_);
    if (!data)
        return data.status();
    if ((r16(data.value().data()) & mode_type_mask) != mode_regular)
        return Status::invalid_argument("not a regular file");
    if (auto status = fs_->writable(data.value()); !status)
        return status;
    if (size > 0xffffffffu)
        return Status::overflow("ext4 file too large");
    Ext4FileSystem::TimestampEvent event;
    const Ext4FileSystem::TimestampTarget target{&data.value(),
        Ext4TimestampFields::modification | Ext4TimestampFields::change};
    if (auto status = fs_->prepare_event(event, {&target, 1}); !status)
        return status;
    Ext4FileSystem::NamespaceMutation mutation(*fs_, fs_->maintained());
    if (auto status = fs_->resize(ino_, data.value(), size); !status)
        return status;
    mutation.committed = true;
    return Status::success();
}
Status Ext4Node::sync()
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    return fs_->operation(this, [&] { return fs_->sync_active(); });
}
Status Ext4Node::retain_locked()
{
    if (phase_ != Phase::linked && !(phase_ == Phase::detached && references_))
        return Status::not_found("ext4 node identity is no longer retainable");
    if (references_ == ~u32{0})
        return Status::overflow("ext4 node reference count exceeded");
    ++references_;
    return Status::success();
}
Status Ext4Node::retain()
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    smp::ScopedSpinLock guard(fs_->identity_lock_);
    return retain_locked();
}
Status Ext4Node::expose_locked()
{
    if (phase_ != Phase::linked && !(phase_ == Phase::detached && references_))
        return Status::not_found("ext4 node identity cannot be exposed");
    borrowed_identity_exposed_ = true;
    return {};
}
Status Ext4Node::expose_borrowed_identity()
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    smp::ScopedSpinLock guard(fs_->identity_lock_);
    return expose_locked();
}
Status Ext4Node::release()
{
    if (!fs_) return Status::not_found("ext4 node has no owner");
    {
        smp::ScopedSpinLock guard(fs_->identity_lock_);
        if (!references_) return Status::invalid_argument("ext4 unbalanced release");
        --references_; // Consumed even if later cleanup fails.
        if (references_ || phase_ != Phase::detached || fs_->operation_active_) return {};
        fs_->operation_active_ = true;
    }
    return fs_->finish_operation();
}
Node *Ext4Node::lookup(std::string_view name)
{
    const auto result = lookup_child(name);
    return result ? result.value() : nullptr;
}
Result<Node *> Ext4Node::lookup_active(std::string_view name, bool retained, bool exposed)
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    auto data = fs_->inode(ino_);
    if (!data)
        return data.status();
    auto found = fs_->child(data.value(), name);
    if (!found)
        return found.status();
    if (!found.value() || found.value() > fs_->inodes_)
        return fs_->io_status(corrupt());
    auto *node = fs_->node(found.value(), name);
    if (!node)
        return Status::no_memory("ext4 node cache capacity exceeded");
    {
        smp::ScopedSpinLock guard(fs_->identity_lock_);
        if (retained)
        { if (auto status = node->retain_locked(); !status) return status; }
        if (exposed)
        { if (auto status = node->expose_locked(); !status) return status; }
    }
    return static_cast<Node *>(node);
}
Status Ext4Node::create(std::string_view name, NodeType type)
{
    auto result = create_child_retained(name, {type, type == NodeType::directory ? 0755u : 0644u,
                                             default_uid, default_gid});
    return result ? result.value()->release() : result.status();
}
Result<Node *> Ext4Node::create_active(std::string_view name, const InitialNodeAttributes &attributes, bool retained)
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    const auto type = attributes.type;
    if (!valid_name(name) || (type != NodeType::regular && type != NodeType::directory) ||
        (attributes.mode & ~mode_permission_mask))
        return Status::invalid_argument("unsupported ext4 node");
    auto parent = fs_->inode(ino_);
    if (!parent)
        return parent.status();
    if ((r16(parent.value().data()) & mode_type_mask) != mode_directory)
        return Status::not_directory("ext4 creation parent is not a directory");
    if (auto status = fs_->writable(parent.value()); !status.ok())
        return status;
    if (!r16(parent.value().data() + 26))
        return Status::not_found("ext4 creation parent is unlinked");
    auto exists = fs_->child(parent.value(), name);
    if (exists)
        return Status::already_exists("ext4 node exists");
    if (exists.status().code() != StatusCode::not_found)
        return exists.status();
    const bool directory = type == NodeType::directory;
    if (directory && r16(parent.value().data() + 26) == 65535)
        return Status::link_limit("ext4 directory link count exceeded");

    // Namespace calls are serialized. Reserve the stable result before any
    // allocation; a successful publication must not need another lookup.
    Ext4Node *result = nullptr;
    {
        smp::ScopedSpinLock guard(fs_->identity_lock_);
        for (auto &candidate : fs_->nodes_)
            if (candidate.phase_ == Phase::vacant && !candidate.borrowed_identity_exposed_ && !candidate.references_)
            { result = &candidate; break; }
        if (!result) return Status::no_memory("ext4 node cache capacity exceeded");
        if (auto status = result->name_.assign(name); !status) return status;
    }

    // Read-only insertion planning distinguishes logical capacity failures
    // from uncertain publication. The old insert helper also allocates, and
    // therefore cannot provide that boundary to this operation.
    Ext4FileSystem::Block entry_block{};
    const auto needed = aligned_entry(name.size());
    const auto parent_blocks = file_size(parent.value().data()) / fs_->block_size_;
    u32 entry_physical = 0;
    u16 entry_offset = 0, entry_length = 0, entry_used = 0;
    for (u32 logical = 0; logical < parent_blocks && !entry_physical; ++logical)
    {
        auto physical = fs_->map(parent.value(), logical);
        if (!physical)
            return physical.status();
        if (!physical.value())
            return fs_->io_status(corrupt());
        if (auto status = fs_->read_block(physical.value(), entry_block); !status.ok())
            return status;
        for (u32 offset = 0; offset < fs_->block_size_;)
        {
            if (fs_->block_size_ - offset < 8)
                return fs_->io_status(corrupt());
            const auto *entry = entry_block.data() + offset;
            const auto length = r16(entry + 4);
            const auto namelen = std::to_integer<u8>(entry[6]);
            if (length < 8 || length % 4 || length > fs_->block_size_ - offset || namelen > length - 8)
                return fs_->io_status(corrupt());
            const auto used = r32(entry) ? aligned_entry(namelen) : 0;
            if (length - used >= needed)
            {
                entry_physical = physical.value();
                entry_offset = static_cast<u16>(offset);
                entry_length = length;
                entry_used = used;
                break;
            }
            offset += length;
        }
    }
    const bool grow_parent = !entry_physical;
    if (grow_parent && parent_blocks >= 0xffffffffu / fs_->block_size_)
        return Status::no_space("ext4 directory size capacity exceeded");

    Ext4FileSystem::Inode data{};
    w16(data.data(), static_cast<u16>(mode_for(type, attributes.mode)));
    w16(data.data() + 2, static_cast<u16>(attributes.uid));
    w16(data.data() + 24, static_cast<u16>(attributes.gid));
    w16(data.data() + 120, static_cast<u16>(attributes.uid >> 16));
    w16(data.data() + 122, static_cast<u16>(attributes.gid >> 16));
    w16(data.data() + 26, directory ? 2 : 1);
    w32(data.data() + 32, extents_flag);
    w16(data.data() + 40, extent_magic);
    w16(data.data() + 44, 4);
    if (fs_->maintained())
    {
        if (fs_->inode_size_ == 256)
            w16(data.data() + 128, 16);
        auto available_inode = fs_->available_number(true);
        if (!available_inode)
            return available_inode.status().code() == StatusCode::no_memory
                ? Status::no_space(available_inode.status().message()) : available_inode.status();
        u32 after = 0;
        if (directory)
        {
            auto available = fs_->available_number(false);
            if (!available)
                return available.status().code() == StatusCode::no_memory
                    ? Status::no_space(available.status().message()) : available.status();
            after = available.value();
        }
        auto insertion = fs_->preflight_insert(parent.value(), name, after);
        if (!insertion)
            return insertion.status().code() == StatusCode::no_memory || insertion.status().code() == StatusCode::overflow
                ? Status::no_space(insertion.status().message()) : insertion.status();
    }
    Ext4FileSystem::TimestampEvent event;
    const Ext4FileSystem::TimestampTarget times[]{
        {&parent.value(), Ext4TimestampFields::modification | Ext4TimestampFields::change},
        {&data, Ext4TimestampFields::all, true}};
    if (auto status = fs_->prepare_event(event, times); !status)
        return status;

    Ext4FileSystem::NamespaceMutation mutation(*fs_);
    const auto storage_status = [](Status status) {
        return status.code() == StatusCode::no_memory ? Status::no_space(status.message()) : status;
    };
    auto allocated = fs_->allocate(true, directory);
    if (!allocated)
        return storage_status(allocated.status());
    const auto ino = allocated.value();
    u32 child_block = 0, parent_block = 0;
    auto rollback = [&](Status cause) {
        // No parent entry has been attempted here. Only successful logical
        // rollback may leave this mount writable; failed I/O forbids cleanup.
        if (fs_->failed_)
            return cause;
        if (parent_block)
            if (auto status = fs_->free_number(parent_block, false); !status.ok())
                return status;
        if (child_block)
            if (auto status = fs_->free_number(child_block, false); !status.ok())
                return status;
        if (auto status = fs_->free_number(ino, true, directory); !status.ok())
            return status;
        mutation.committed = true;
        return storage_status(cause);
    };
    if (directory)
    {
        auto allocation = fs_->allocate(false);
        if (!allocation)
            return rollback(allocation.status());
        child_block = allocation.value();
        if (auto status = fs_->append(data, 0, child_block); !status.ok())
            return rollback(status);
        w32(data.data() + 4, fs_->block_size_);
    }
    if (grow_parent)
    {
        auto allocation = fs_->allocate(false);
        if (!allocation)
            return rollback(allocation.status());
        parent_block = allocation.value();
        if (auto status = fs_->append(parent.value(), parent_blocks, parent_block); !status.ok())
            return rollback(status.code() == StatusCode::overflow
                                ? Status::no_space("ext4 directory extent capacity exceeded") : status);
        w32(parent.value().data() + 4, (parent_blocks + 1) * fs_->block_size_);
        entry_physical = parent_block;
        entry_block = {};
        entry_length = static_cast<u16>(fs_->block_size_);
    }
    if (directory)
    {
        Ext4FileSystem::Block block{};
        w32(block.data(), ino);
        w16(block.data() + 4, 12);
        block[6] = std::byte{1};
        block[7] = std::byte{2};
        block[8] = std::byte{'.'};
        w32(block.data() + 12, ino_);
        w16(block.data() + 16, static_cast<u16>(fs_->block_size_ - 12));
        block[18] = std::byte{2};
        block[19] = std::byte{2};
        block[20] = block[21] = std::byte{'.'};
        if (auto status = fs_->write_block(child_block, block); !status.ok())
            return status;
    }
    if (auto status = fs_->save_inode(ino, data); !status.ok())
        return status;

    auto *entry = entry_block.data() + entry_offset;
    if (entry_used)
    {
        w16(entry + 4, entry_used);
        entry += entry_used;
    }
    w32(entry, ino);
    w16(entry + 4, entry_length - entry_used);
    entry[6] = std::byte(name.size());
    entry[7] = std::byte(directory_type(type));
    for (usize i = 0; i < name.size(); ++i)
        entry[8 + i] = std::byte(name[i]);
    // From this point even a failed write may have published the child. Keep
    // its allocations and enter write-failed state rather than freeing them.
    if (auto status = fs_->write_block(entry_physical, entry_block); !status.ok())
        return status;
    if (directory)
        w16(parent.value().data() + 26, r16(parent.value().data() + 26) + 1);
    if (auto status = fs_->save_inode(ino_, parent.value()); !status.ok())
        return status;
    {
        smp::ScopedSpinLock guard(fs_->identity_lock_);
        result->ino_ = ino;
        result->phase_ = Phase::linked;
        if (retained)
        { if (auto status = result->retain_locked(); !status) return fs_->io_status(status); }
        else
        { if (auto status = result->expose_locked(); !status) return fs_->io_status(status); }
    }
    mutation.committed = true;
    return static_cast<Node *>(result);
}
Status Ext4Node::create_symlink_active(std::string_view name, std::string_view target, u32 uid, u32 gid)
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    if (!valid_name(name) || target.empty() || target.find('\0') != std::string_view::npos)
        return Status::invalid_argument("invalid symbolic link");
    if (target.size() >= fs_->block_size_)
        return Status::overflow("ext4 symbolic link target exceeds one block");
    auto parent = fs_->inode(ino_);
    if (!parent)
        return parent.status();
    if ((r16(parent.value().data()) & mode_type_mask) != mode_directory)
        return Status::not_directory("symbolic link parent is not a directory");
    if (auto status = fs_->writable(parent.value()); !status.ok())
        return status;
    if (!r16(parent.value().data() + 26))
        return Status::not_found("symbolic link parent is unlinked");
    auto existing = fs_->child(parent.value(), name);
    if (existing)
        return Status::already_exists("symbolic link name exists");
    if (existing.status().code() != StatusCode::not_found)
        return existing.status();
    Ext4FileSystem::Inode data{};
    w16(data.data(), static_cast<u16>(mode_symlink | 0777));
    w16(data.data() + 2, static_cast<u16>(uid));
    w16(data.data() + 24, static_cast<u16>(gid));
    w16(data.data() + 120, static_cast<u16>(uid >> 16));
    w16(data.data() + 122, static_cast<u16>(gid >> 16));
    w16(data.data() + 26, 1);
    w32(data.data() + 4, static_cast<u32>(target.size()));
    if (fs_->maintained())
    {
        if (fs_->inode_size_ == 256)
            w16(data.data() + 128, 16);
        auto available_inode = fs_->available_number(true);
        if (!available_inode)
            return available_inode.status();
        u32 after = 0;
        if (target.size() >= fast_symlink_capacity)
        {
            auto available = fs_->available_number(false);
            if (!available)
                return available.status();
            after = available.value();
        }
        auto insertion = fs_->preflight_insert(parent.value(), name, after);
        if (!insertion)
            return insertion.status();
    }
    Ext4FileSystem::TimestampEvent event;
    const Ext4FileSystem::TimestampTarget times[]{
        {&parent.value(), Ext4TimestampFields::modification | Ext4TimestampFields::change},
        {&data, Ext4TimestampFields::all, true}};
    if (auto status = fs_->prepare_event(event, times); !status)
        return status;
    Ext4FileSystem::NamespaceMutation mutation(*fs_);
    auto allocation = fs_->allocate(true);
    if (!allocation)
        return allocation.status();
    const auto ino = allocation.value();
    u32 physical = 0;
    auto rollback = [&](Status cause) {
        // Before publication, ordinary capacity failures can return their own
        // allocations. An I/O failure has an uncertain disk state: stop writing.
        if (fs_->failed_)
            return cause;
        if (physical)
            if (auto status = fs_->free_number(physical, false); !status.ok())
                return status;
        if (auto status = fs_->save_inode(ino, {}); !status.ok())
            return status;
        if (auto status = fs_->free_number(ino, true); !status.ok())
            return status;
        mutation.committed = true;
        return cause;
    };
    if (target.size() < fast_symlink_capacity)
    {
        for (usize i = 0; i < target.size(); ++i)
            data[40 + i] = std::byte(target[i]);
    }
    else
    {
        auto block = fs_->allocate(false);
        if (!block)
            return rollback(block.status());
        physical = block.value();
        w32(data.data() + 28, fs_->block_size_ / 512);
        w32(data.data() + 32, extents_flag);
        w16(data.data() + 40, extent_magic);
        w16(data.data() + 42, 1);
        w16(data.data() + 44, 4);
        w16(data.data() + 56, 1);
        w32(data.data() + 60, physical);
        Ext4FileSystem::Block contents{};
        for (usize i = 0; i < target.size(); ++i)
            contents[i] = std::byte(target[i]);
        if (auto status = fs_->write_block(physical, contents); !status.ok())
            return status;
    }
    if (auto status = fs_->save_inode(ino, data); !status.ok())
        return status;
    if (auto status = fs_->insert(ino_, parent.value(), name, ino, NodeType::symlink); !status.ok())
        return rollback(status);
    mutation.committed = true;
    return Status::success();
}
Status Ext4FileSystem::replace_entry(const Inode &directory, std::string_view name, u32 ino, NodeType type)
{
    auto location = find(directory, name);
    if (!location)
        return location.status();
    Block block{};
    if (auto status = read_block(location.value().block, block); !status.ok())
        return status;
    auto *entry = block.data() + location.value().offset;
    w32(entry, ino);
    if (ino)
        entry[7] = std::byte(directory_type(type));
    return write_block(location.value().block, block);
}
Result<bool> Ext4FileSystem::empty_directory(const Inode &directory) const
{
    if ((r16(directory.data()) & mode_type_mask) != mode_directory)
        return Status::not_directory("node is not a directory");
    const auto size = file_size(directory.data());
    if (size % block_size_)
        return io_status(corrupt());
    for (u32 logical = 0; logical < size / block_size_; ++logical)
    {
        auto physical = map(directory, logical);
        if (!physical)
            return physical.status();
        if (!physical.value())
            return io_status(corrupt());
        Block block{};
        if (auto status = read_block(physical.value(), block); !status.ok())
            return status;
        for (u32 offset = 0; offset < block_size_;)
        {
            if (block_size_ - offset < 8)
                return io_status(corrupt());
            const auto *entry = block.data() + offset;
            const auto length = r16(entry + 4);
            const auto name_length = std::to_integer<u8>(entry[6]);
            if (length < 8 || length % 4 || length > block_size_ - offset || name_length > length - 8)
                return io_status(corrupt());
            const std::string_view name(reinterpret_cast<const char *>(entry + 8), name_length);
            if (r32(entry) && name != "." && name != "..")
                return false;
            offset += length;
        }
    }
    return true;
}
Result<FixedString<max_path_segment>> Ext4FileSystem::child_name(const Inode &directory, u32 ino) const
{
    const auto size = file_size(directory.data());
    if ((r16(directory.data()) & mode_type_mask) != mode_directory || size % block_size_)
        return io_status(corrupt());
    for (u32 logical = 0; logical < size / block_size_; ++logical)
    {
        auto physical = map(directory, logical);
        if (!physical)
            return physical.status();
        if (!physical.value())
            return io_status(corrupt());
        Block block{};
        if (auto status = read_block(physical.value(), block); !status.ok())
            return status;
        for (u32 offset = 0; offset < block_size_;)
        {
            if (block_size_ - offset < 8)
                return io_status(corrupt());
            const auto *entry = block.data() + offset;
            const auto length = r16(entry + 4);
            const auto name_length = std::to_integer<u8>(entry[6]);
            if (length < 8 || length % 4 || length > block_size_ - offset || name_length > length - 8)
                return io_status(corrupt());
            const std::string_view name(reinterpret_cast<const char *>(entry + 8), name_length);
            if (r32(entry) == ino && name != "." && name != "..")
            {
                FixedString<max_path_segment> result;
                if (auto status = result.assign(name); !status.ok())
                    return status;
                return result;
            }
            offset += length;
        }
    }
    return Status::not_found("directory has no live parent entry");
}
Result<FixedString<96>> Ext4Node::directory_path_active(const Node &root) const
{
    if (!fs_ || !ino_ || !fs_->owns(root))
        return Status::not_found("directory belongs to another filesystem");
    const auto root_ino = static_cast<const Ext4Node &>(root).ino_;
    FixedString<96> suffix;
    u32 current = ino_;
    // Each non-root component consumes at least two bytes of the bounded path.
    for (usize depth = 0; depth < 48; ++depth)
    {
        auto data = fs_->inode(current);
        if (!data)
            return data.status();
        if ((r16(data.value().data()) & mode_type_mask) != mode_directory)
            return Status::not_directory("node is not a directory");
        if (!r16(data.value().data() + 26))
            return Status::not_found("directory has been removed");
        if (current == root_ino)
            return suffix;
        if (current == 2)
            return Status::not_found("directory is outside this root");
        auto parent_number = fs_->child(data.value(), "..");
        if (!parent_number)
            return parent_number.status();
        if (parent_number.value() == current)
            return fs_->io_status(corrupt());
        auto parent = fs_->inode(parent_number.value());
        if (!parent)
            return parent.status();
        auto name = fs_->child_name(parent.value(), current);
        if (!name)
            return name.status();
        FixedString<96> path;
        if (!path.append('/').ok() || !path.append(name.value().view()).ok() || !path.append(suffix.view()).ok())
            return Status::overflow("directory path exceeds capacity");
        suffix = path;
        current = parent_number.value();
    }
    return Status::overflow("directory ancestry exceeds path capacity");
}
Status Ext4FileSystem::validate_move(u32 source, u32 destination_parent) const
{
    // Follow actual '..' inodes rather than path text. A directory cannot move
    // below itself even when the caller reached the destination through aliases.
    for (u32 steps = 0; steps < inodes_; ++steps)
    {
        if (destination_parent == source)
            return Status::invalid_argument("cannot move a directory into its descendant");
        if (destination_parent == 2)
            return Status::success();
        auto parent = inode(destination_parent);
        if (!parent)
            return parent.status();
        auto next = child(parent.value(), "..");
        if (!next)
            return io_status(next.status());
        if (next.value() == destination_parent)
            return io_status(corrupt());
        destination_parent = next.value();
    }
    return io_status(corrupt());
}
Status Ext4Node::link_active(std::string_view name, Node &target)
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    if (!fs_->owns(target))
        return Status::cross_device("hard link crosses filesystems");
    if (!valid_name(name))
        return Status::invalid_argument("invalid hard link name");
    auto &source = static_cast<Ext4Node &>(target);
    if (!source.ino_)
        return Status::not_found("hard link source was removed");
    auto parent = fs_->inode(ino_);
    if (!parent)
        return parent.status();
    auto data = fs_->inode(source.ino_);
    if (!data)
        return data.status();
    if ((r16(parent.value().data()) & mode_type_mask) != mode_directory)
        return Status::not_directory("hard link destination is not a directory");
    if ((r16(data.value().data()) & mode_type_mask) == mode_directory)
        return Status::permission("directories cannot be hard linked");
    const auto source_type = r16(data.value().data()) & mode_type_mask;
    if (source_type != mode_regular && source_type != mode_symlink)
        return Status::unsupported("hard link source type is unsupported");
    if (auto status = fs_->writable(parent.value()); !status.ok())
        return status;
    if (auto status = fs_->writable(data.value()); !status.ok())
        return status;
    const auto links = r16(data.value().data() + 26);
    if (!links || !r16(parent.value().data() + 26))
        return Status::not_found("hard link references an unlinked node");
    if (links == 65535)
        return Status::link_limit("ext4 link count exceeded");
    auto existing = fs_->child(parent.value(), name);
    if (existing)
        return Status::already_exists("hard link name exists");
    if (existing.status().code() != StatusCode::not_found)
        return existing.status();
    if (fs_->maintained())
    {
        auto insertion = fs_->preflight_insert(parent.value(), name);
        if (!insertion)
            return insertion.status();
    }
    Ext4FileSystem::TimestampEvent event;
    const Ext4FileSystem::TimestampTarget times[]{
        {&parent.value(), Ext4TimestampFields::modification | Ext4TimestampFields::change},
        {&data.value(), Ext4TimestampFields::change}};
    if (auto status = fs_->prepare_event(event, times); !status)
        return status;
    Ext4FileSystem::NamespaceMutation mutation(*fs_);
    if (auto status = fs_->insert(ino_, parent.value(), name, source.ino_, fs_->metadata(data.value()).type); !status.ok())
        return status;
    w16(data.value().data() + 26, links + 1);
    if (auto status = fs_->save_inode(source.ino_, data.value()); !status.ok())
        return status;
    mutation.committed = true;
    return Status::success();
}
Status Ext4Node::rename_active(std::string_view old_name, Node &destination, std::string_view new_name)
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    if (!fs_->owns(destination))
        return Status::cross_device("rename crosses filesystems");
    if (!valid_name(old_name) || !valid_name(new_name))
        return Status::invalid_argument("invalid rename component");
    auto &new_parent = static_cast<Ext4Node &>(destination);
    if (!new_parent.ino_)
        return Status::not_found("rename destination was removed");
    auto from = fs_->inode(ino_);
    if (!from)
        return from.status();
    auto to = fs_->inode(new_parent.ino_);
    if (!to)
        return to.status();
    if ((r16(from.value().data()) & mode_type_mask) != mode_directory ||
        (r16(to.value().data()) & mode_type_mask) != mode_directory)
        return Status::not_directory("rename parent is not a directory");
    if (auto status = fs_->writable(from.value()); !status.ok())
        return status;
    if (auto status = fs_->writable(to.value()); !status.ok())
        return status;
    if (!r16(from.value().data() + 26) || !r16(to.value().data() + 26))
        return Status::not_found("rename parent is unlinked");
    auto source_number = fs_->child(from.value(), old_name);
    if (!source_number)
        return source_number.status();
    if (source_number.value() == 2)
        return Status::busy("cannot rename the root directory");
    auto source = fs_->inode(source_number.value());
    if (!source)
        return source.status();
    if (auto status = fs_->writable(source.value()); !status.ok())
        return status;
    const auto type = r16(source.value().data()) & mode_type_mask;
    const bool directory = type == mode_directory;
    if (!directory && type != mode_regular && type != mode_symlink)
        return Status::unsupported("rename source type unsupported");
    if (!r16(source.value().data() + 26))
        return fs_->io_status(corrupt());
    auto target_number = fs_->child(to.value(), new_name);
    if (!target_number && target_number.status().code() != StatusCode::not_found)
        return target_number.status();
    if (target_number && target_number.value() == source_number.value())
        return Status::success();
    Ext4FileSystem::Inode target{};
    Ext4Node *victim = nullptr;
    if (target_number)
    {
        auto found = fs_->inode(target_number.value());
        if (!found)
            return found.status();
        target = found.value();
        const bool target_directory = (r16(target.data()) & mode_type_mask) == mode_directory;
        if (directory && !target_directory)
            return Status::not_directory("replacement is not a directory");
        if (!directory && target_directory)
            return Status::is_directory("file cannot replace directory");
        if (auto status = fs_->writable(target); !status.ok())
            return status;
        const auto links = r16(target.data() + 26);
        if (!links)
            return fs_->io_status(corrupt());
        if (directory)
        {
            auto empty = fs_->empty_directory(target);
            if (!empty)
                return empty.status();
            if (!empty.value())
                return Status::not_empty("replacement directory is not empty");
            auto dot = fs_->child(target, ".");
            if (!dot)
                return dot.status().code() == StatusCode::not_found ? fs_->io_status(corrupt()) : dot.status();
            if (dot.value() != target_number.value())
                return fs_->io_status(corrupt());
            auto dotdot = fs_->child(target, "..");
            if (!dotdot)
                return dotdot.status().code() == StatusCode::not_found ? fs_->io_status(corrupt()) : dotdot.status();
            if (dotdot.value() != new_parent.ino_ ||
                links != 2 || r16(to.value().data() + 26) < 3)
                return fs_->io_status(corrupt());
        }
        victim = fs_->node(target_number.value(), new_name);
        if (!victim)
            return Status::no_memory("ext4 node cache full");
    }
    if (directory)
    {
        auto dot = fs_->child(source.value(), ".");
        if (!dot)
            return dot.status().code() == StatusCode::not_found ? fs_->io_status(corrupt()) : dot.status();
        if (dot.value() != source_number.value())
            return fs_->io_status(corrupt());
        auto dotdot = fs_->child(source.value(), "..");
        if (!dotdot)
            return dotdot.status().code() == StatusCode::not_found ? fs_->io_status(corrupt()) : dotdot.status();
        if (dotdot.value() != ino_ ||
            r16(source.value().data() + 26) < 2 || r16(from.value().data() + 26) < 3)
            return fs_->io_status(corrupt());
        if (auto status = fs_->validate_move(source_number.value(), new_parent.ino_); !status.ok())
            return status;
        if (!target_number && r16(to.value().data() + 26) == 65535)
            return Status::link_limit("destination directory link count exceeded");
    }
    if (fs_->maintained() && !target_number)
    {
        auto insertion = fs_->preflight_insert(to.value(), new_name);
        if (!insertion)
            return insertion.status();
    }
    Ext4FileSystem::TimestampEvent event;
    std::array<Ext4FileSystem::TimestampTarget, 4> times{};
    usize time_count = 0;
    times[time_count++] = {&from.value(), Ext4TimestampFields::modification | Ext4TimestampFields::change};
    if (ino_ != new_parent.ino_)
        times[time_count++] = {&to.value(), Ext4TimestampFields::modification | Ext4TimestampFields::change};
    times[time_count++] = {&source.value(), Ext4TimestampFields::change};
    if (victim)
        times[time_count++] = {&target, Ext4TimestampFields::change};
    if (auto status = fs_->prepare_event(event, {times.data(), time_count}); !status)
        return status;
    if (ino_ == new_parent.ino_)
        to.value() = from.value();
    Ext4FileSystem::NamespaceMutation mutation(*fs_);
    if (target_number)
    {
        if (auto status = fs_->replace_entry(to.value(), new_name, source_number.value(), fs_->metadata(source.value()).type); !status.ok())
            return status;
    }
    else if (auto status = fs_->insert(new_parent.ino_, to.value(), new_name, source_number.value(), fs_->metadata(source.value()).type);
             !status.ok())
        return status;
    // Inserting into the same directory may split the old source record or
    // grow its extent map. Reload it before erasing the source and link count.
    from = fs_->inode(ino_);
    if (!from)
        return from.status();
    if (auto status = fs_->apply_event(event, from.value(),
            Ext4TimestampFields::modification | Ext4TimestampFields::change); !status)
        return status;
    if (auto status = fs_->replace_entry(from.value(), old_name, 0, NodeType::regular); !status.ok())
        return status;
    if (directory)
    {
        if (ino_ != new_parent.ino_)
            if (auto status = fs_->replace_entry(source.value(), "..", new_parent.ino_, NodeType::directory); !status.ok())
                return status;
        w16(from.value().data() + 26, r16(from.value().data() + 26) - 1);
    }
    if (directory || fs_->maintained())
        if (auto status = fs_->save_inode(ino_, from.value()); !status.ok())
            return status;
    if (fs_->maintained())
    {
        // An insertion already saved the destination; replacement must save its
        // planned M/C explicitly. Distinct parents cannot alias this reload.
        if (ino_ != new_parent.ino_ && target_number)
            if (auto status = fs_->save_inode(new_parent.ino_, to.value()); !status)
                return status;
        if (auto status = fs_->save_inode(source_number.value(), source.value()); !status)
            return status;
    }
    if (victim)
    {
        w16(target.data() + 26, directory ? 0 : r16(target.data() + 26) - 1);
        if (auto status = fs_->save_inode(victim->ino_, target); !status.ok())
            return status;
        if (!r16(target.data() + 26)) fs_->detach(*victim);
    }
    mutation.committed = true;
    return Status::success();
}

Status Ext4Node::remove_active(std::string_view name, bool directory)
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    if (!valid_name(name))
        return Status::invalid_argument("invalid removal name");
    auto parent = fs_->inode(ino_);
    if (!parent)
        return parent.status();
    if (auto status = fs_->writable(parent.value()); !status.ok())
        return status;
    auto location = fs_->find(parent.value(), name);
    if (!location)
        return location.status();
    auto found_victim = lookup_active(name, false, false);
    if (!found_victim)
        return found_victim.status();
    auto *victim = static_cast<Ext4Node *>(found_victim.value());
    auto data = fs_->inode(victim->ino_);
    if (!data)
        return data.status();
    if (auto status = fs_->writable(data.value()); !status.ok())
        return status;
    if (((r16(data.value().data()) & mode_type_mask) == mode_directory) != directory)
        return Status::invalid_argument("removal type mismatch");
    if (directory)
    {
        auto listing = victim->list_active(DirectoryReadIntent::inspection);
        if (!listing)
            return listing.status();
        if (listing.value().count)
            return Status::busy("directory not empty");
    }
    const auto links = r16(data.value().data() + 26);
    if (!links || (directory && (links != 2 || r16(parent.value().data() + 26) < 3)))
        return fs_->io_status(corrupt());
    Ext4FileSystem::Block block{};
    if (auto status = fs_->read_block(location.value().block, block); !status.ok())
        return status;
    Ext4FileSystem::TimestampEvent event;
    const Ext4FileSystem::TimestampTarget times[]{
        {&parent.value(), Ext4TimestampFields::modification | Ext4TimestampFields::change},
        {&data.value(), Ext4TimestampFields::change}};
    if (auto status = fs_->prepare_event(event, times); !status)
        return status;
    Ext4FileSystem::NamespaceMutation mutation(*fs_, fs_->maintained());
    w32(block.data() + location.value().offset, 0);
    if (auto status = fs_->write_block(location.value().block, block); !status.ok())
        return status;
    w16(data.value().data() + 26, directory ? 0 : links - 1);
    if (auto status = fs_->save_inode(victim->ino_, data.value()); !status.ok())
        return status;
    if (directory)
        w16(parent.value().data() + 26, r16(parent.value().data() + 26) - 1);
    if (directory || fs_->maintained())
        if (auto status = fs_->save_inode(ino_, parent.value()); !status.ok())
            return status;
    if (!r16(data.value().data() + 26)) fs_->detach(*victim);
    mutation.committed = true;
    return Status::success();
}
Status Ext4FileSystem::apply_mode(u32 ino, Inode &record, u32 mode)
{
    TimestampEvent event;
    const TimestampTarget target{&record, Ext4TimestampFields::change};
    if (auto status = prepare_event(event, {&target, 1}); !status)
        return status;
    NamespaceMutation mutation(*this, maintained());
    w16(record.data(),
        static_cast<u16>((r16(record.data()) & mode_type_mask) | (mode & mode_permission_mask)));
    if (auto status = save_inode(ino, record); !status)
        return status;
    mutation.committed = true;
    return Status::success();
}
Status Ext4Node::chmod_active(u32 mode)
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    auto data = fs_->inode(ino_);
    if (!data)
        return data.status();
    if (fs_->maintained())
        if (auto status = fs_->writable(data.value()); !status)
            return status;
    return fs_->apply_mode(ino_, data.value(), mode);
}
ModeChangeResult Ext4Node::change_mode_active(u32 mode, user::Credentials caller)
{
    if (!fs_ || !ino_)
        return ModeChangeResult::failure(Status::not_found("ext4 node was removed"));
    if (!fs_->writable_)
        return ModeChangeResult::read_only();
    if (!fs_->maintained())
        return ModeChangeResult::unavailable();
    auto data = fs_->inode(ino_);
    if (!data)
        return data.status().code() == StatusCode::unsupported ? ModeChangeResult::unavailable()
                                                               : ModeChangeResult::failure(data.status());
    const auto type = r16(data.value().data()) & mode_type_mask;
    if (type != mode_regular && type != mode_directory)
        return ModeChangeResult::unavailable();
    if (auto status = fs_->writable(data.value()); !status)
        return status.code() == StatusCode::unsupported ? ModeChangeResult::unavailable()
                                                        : ModeChangeResult::failure(status);
    const auto metadata = fs_->metadata(data.value());
    const auto decision = mode_change_policy(metadata.uid, metadata.gid, mode, caller);
    if (!decision.authorized)
        return ModeChangeResult::failure(Status::permission("only root or the inode owner can change mode"));
    // The admitted boot-CPU context is serialized. Authority and ctime staging
    // use this one inode record; this is not a concurrent inode-lock guarantee.
    auto status = fs_->apply_mode(ino_, data.value(), decision.permissions);
    return status.code() == StatusCode::unsupported ? ModeChangeResult::unavailable()
                                                    : ModeChangeResult::failure(status);
}
Status Ext4Node::chown_active(u32 uid, u32 gid, bool clear_set_id)
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    auto data = fs_->inode(ino_);
    if (!data)
        return data.status();
    if (fs_->maintained())
        if (auto status = fs_->writable(data.value()); !status)
            return status;
    Ext4FileSystem::TimestampEvent event;
    const Ext4FileSystem::TimestampTarget target{&data.value(), Ext4TimestampFields::change};
    if (auto status = fs_->prepare_event(event, {&target, 1}); !status)
        return status;
    Ext4FileSystem::NamespaceMutation mutation(*fs_, fs_->maintained());
    w16(data.value().data() + 2, static_cast<u16>(uid));
    w16(data.value().data() + 24, static_cast<u16>(gid));
    w16(data.value().data() + 120, static_cast<u16>(uid >> 16));
    w16(data.value().data() + 122, static_cast<u16>(gid >> 16));
    if (const u16 mode = r16(data.value().data()); clear_set_id && (mode & mode_type_mask) == mode_regular)
    {
        u16 cleared = static_cast<u16>(mode & ~04000u);
        if (mode & 010u) cleared = static_cast<u16>(cleared & ~02000u);
        w16(data.value().data(), cleared);
    }
    if (auto status = fs_->save_inode(ino_, data.value()); !status)
        return status;
    mutation.committed = true;
    return Status::success();
}
Result<DirectoryListing> Ext4Node::list_active(DirectoryReadIntent intent, usize start, bool paged) const
{
    if (!fs_ || !ino_)
        return Status::not_found("ext4 node was removed");
    auto data = fs_->inode(ino_);
    if (!data)
        return data.status();
    if ((r16(data.value().data()) & mode_type_mask) != mode_directory)
        return Status::invalid_argument("not a directory");
    DirectoryListing out{};
    const auto size = file_size(data.value().data());
    if (size % fs_->block_size_)
        return fs_->io_status(corrupt());
    for (u32 logical = 0; logical < size / fs_->block_size_; ++logical)
    {
        auto physical = fs_->map(data.value(), logical);
        if (!physical || !physical.value())
            return fs_->io_status(corrupt());
        Ext4FileSystem::Block buffer{};
        if (auto status = fs_->read_block(physical.value(), buffer); !status.ok())
            return status;
        for (u32 offset = 0; offset < fs_->block_size_;)
        {
            if (fs_->block_size_ - offset < 8)
                return fs_->io_status(corrupt());
            const auto *entry = buffer.data() + offset;
            const auto length = r16(entry + 4);
            const auto namelen = std::to_integer<u8>(entry[6]);
            if (length < 8 || length % 4 || length > fs_->block_size_ - offset || namelen > length - 8)
                return fs_->io_status(corrupt());
            const auto number = r32(entry);
            const std::string_view name(reinterpret_cast<const char *>(entry + 8), namelen);
            if (number && name != "." && name != ".." && start)
                --start;
            else if (number && name != "." && name != "..")
            {
                if (out.count == out.entries.size())
                {
                    if (paged) return out;
                    return Status::overflow("directory listing capacity exceeded");
                }
                auto child_inode = fs_->inode(number);
                if (!child_inode)
                    return child_inode.status();
                auto &target = out.entries[out.count++];
                if (auto status = target.name.assign(name); !status.ok())
                    return status;
                target.metadata = fs_->metadata(child_inode.value());
                target.metadata.attributes.inode = number;
                target.metadata.attributes.mark(Attribute::inode);
                if (fs_->maintained())
                {
                    if (auto status = decode_ext4_times(
                            std::span<const std::byte>{child_inode.value()}.first(fs_->inode_size_),
                            target.metadata.attributes); !status)
                        return fs_->io_status(status);
                    target.metadata.attributes.access.source = target.metadata.attributes.modification.source =
                        target.metadata.attributes.change.source = TimestampSource::maintained_ext4;
                }
            }
            offset += length;
        }
    }
    if (intent == DirectoryReadIntent::access)
        if (auto status = fs_->access_event(ino_, data.value()); !status)
            return status;
    return out;
}
} // namespace ok::fs
