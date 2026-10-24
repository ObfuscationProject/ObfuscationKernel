#pragma once
#include "ok/fs/fat.hpp"
#include "ok/fs/vfs.hpp"
namespace ok::fs
{
class FatFileSystem;
// Borrowed identities remain stable; unexposed retired nodes can be recycled.
class FatNode final : public Node
{
  public:
    std::string_view name() const override
    {
        return name_.view();
    }
    Metadata metadata() const override;
    Result<Metadata> query_metadata() const override;
    Result<FileBuffer> read(usize offset, usize count) const override;
    Status write(usize offset, std::span<const std::byte> bytes) override;
    Status truncate(usize size) override;
    Status sync() override;
    Status retain() override;
    Status release() override;
    Status expose_borrowed_identity() override;
    const void *filesystem_identity() const override
    {
        return fs_;
    }
    Result<DirectoryListing> list(DirectoryReadIntent = DirectoryReadIntent::access) const override;
    Node *lookup(std::string_view name) override;
    Result<Node *> lookup_child(std::string_view name) override;
    Result<Node *> lookup_child_retained(std::string_view name) override;
    Status create(std::string_view name, NodeType type) override;
    Result<Node *> create_child(std::string_view name, const InitialNodeAttributes &attributes) override;
    Result<Node *> create_child_retained(std::string_view name, const InitialNodeAttributes &attributes) override;
    Status remove(std::string_view name, bool directory) override;
    Status chmod(u32) override
    {
        return Status::unsupported("FAT uses fixed mount permissions");
    }
    Status chown(u32, u32) override
    {
        return Status::unsupported("FAT has no POSIX ownership fields");
    }
    Result<FixedString<96>> directory_path(const Node &root) const override;

  private:
    friend class FatFileSystem;
    FatFileSystem *fs_{};
    FixedString<max_path_segment> name_{};
    FixedString<13> alias_{};
    u32 first_{}, parent_{}, slot_{}, slots_{}, references_{};
    u64 size_{}, valid_{};
    bool directory_{}, contiguous_{}, live_{}, readonly_{}, exposed_{};
};
// Serialized, bounded FAT32/exFAT provider. 512-byte sectors, ASCII names up to
// the VFS component limit, 128 live/retained/borrowed identities and 262144 data clusters.
// Nonjournalled media: I/O failure leaves the volume dirty and disables writes.
class FatFileSystem final
{
  public:
    static constexpr usize max_nodes = 128, max_clusters = 262144, max_chain = 1024;
    Status mount(driver::BlockDevice &device, bool writable = true);
    Node *root()
    {
        return mounted_ ? &nodes_[0] : nullptr;
    }
    Status sync();
    const FatMountInfo &info() const
    {
        return info_;
    }

  private:
    friend class FatNode;
    using Sector = std::array<std::byte, 512>;
    using Entry = std::array<std::byte, 32>;
    struct Chain
    {
        std::array<u32, max_chain> values{};
        usize count{};
    };
    struct Guard
    {
        FatFileSystem &fs;
        Status status;
        explicit Guard(FatFileSystem &owner);
        ~Guard();
    };
    Status read_sector(u64 sector, Sector &out) const;
    Status write_sector(u64 sector, const Sector &data);
    Status start_write();
    Status set_dirty(bool dirty);
    Result<u32> fat(u32 cluster) const;
    Status set_fat(u32 cluster, u32 value);
    Result<Chain> chain(u32 first, bool contiguous = false, u64 bytes = 0) const;
    Status claim(const Chain &chain);
    Result<bool> allocated(u32 cluster) const;
    Status set_allocated(u32 cluster, bool allocated);
    Result<u32> allocate_cluster();
    Status free_chain(u32 first, bool contiguous, u64 bytes);
    Result<u64> cluster_sector(u32 cluster) const;
    Status entry(const FatNode &directory, u32 slot, Entry &out) const;
    Status put_entry(const FatNode &directory, u32 slot, const Entry &entry);
    Status scan(FatNode &directory);
    Status scan_fat(FatNode &directory);
    Status scan_exfat(FatNode &directory);
    Status initialize_exfat();
    Result<u32> free_slots(FatNode &directory, u32 count);
    Status extend(FatNode &node, usize clusters);
    Status publish(FatNode &node);
    Result<Node *> create(FatNode &directory, std::string_view name, const InitialNodeAttributes &attributes);
    Status remove(FatNode &directory, std::string_view name, bool is_directory);
    Status resize(FatNode &node, u64 size);
    Status write(FatNode &node, u64 offset, std::span<const std::byte> bytes);
    Status zero(FatNode &node, u64 begin, u64 end);
    Result<FileBuffer> read(const FatNode &node, u64 offset, usize count) const;
    Result<u32> lookup(const FatNode &directory, std::string_view name) const;
    Result<u32> add(FatNode &directory, std::string_view name, u32 slot, u32 slots, u32 first, u64 size, u64 valid,
                    bool is_directory, bool contiguous, bool readonly);
    Status persist_fat_hints();
    bool bit(u32 cluster) const;
    void bit(u32 cluster, bool value);
    FatVolume volume_;
    FatMountInfo info_{};
    driver::BlockDevice *device_{};
    std::array<FatNode, max_nodes> nodes_{};
    std::array<u8, (max_clusters + 7) / 8> owners_{};
    usize used_{1};
    u64 write_sequence_{};
    u32 bitmap_{}, upcase_{}, bitmap_bytes_{}, upcase_bytes_{}, upcase_checksum_{};
    u16 fsinfo_{0xffff}, backup_boot_{0xffff};
    bool mounted_{}, writable_{}, dirty_{}, failed_{}, busy_{};
    mutable smp::SpinLock lock_{};
};
} // namespace ok::fs
