#pragma once

#include "ok/fs/ext4_completion.hpp"

namespace ok::fs
{
struct Ext4InodeMetadataUpdate
{
    u32 inode{}, uid{}, gid{}, ctime_seconds{}, ctime_nanoseconds{};
    u16 permissions{};
    bool ordered_data{};
    u32 size{}, mtime_seconds{}, mtime_nanoseconds{};
};
enum class Ext4TransactionPhase : u8
{
    idle, validating, preflight, data_initialization, data_initialization_flush, data_initialization_readback, payloads, payload_flush, commit_record,
    commit_flush, recovery_bit, recovery_flush, publication, publication_flush,
    published, failed,
};
struct Ext4TransactionReport
{
    Ext4TransactionPhase phase{Ext4TransactionPhase::idle}, failed_phase{Ext4TransactionPhase::idle};
    Status status{};
    usize write_attempts{}, completed_writes{}, flush_attempts{}, completed_flushes{};
    u32 sequence{}, inode{}, target_block{};
    bool payloads_durable{}, commit_record_durable{}, recovery_bit_durable{}, publication_durable{};
    [[nodiscard]] bool published() const { return phase == Ext4TransactionPhase::published && publication_durable; }
};

// One private transaction on exclusively owned, quiesced media. The mounted
// mode requires a durable recovery bit and journals one inode-table action.
// Success publishes a recoverable journal; a separate owner checkpoints it.
// No VFS mount, policy/permission decision, allocation or file-data write API.
class Ext4MetadataTransaction final
{
    using Block = std::array<u8, 1024>;
    using Completion = Ext4RecoveryCompletion<2, 2>;
    class ProposedDevice final : public driver::BlockDevice
    {
      public:
        explicit ProposedDevice(Ext4MetadataTransaction &owner) : owner_(owner) {}
        driver::BlockGeometry geometry() const override { return owner_.device_->geometry(); }
        Status read_blocks(u64 sector, std::span<std::byte> out) override
        {
            if (const auto status = owner_.device_->read_blocks(sector, out); !status)
                return status;
            if (sector < owner_.bounds_.first_sector || out.size() % 512)
                return Status::invalid_argument("ext4 transaction proposed read bounds");
            // Binding may read sub-block sectors; overlay each sector exactly.
            for (usize offset = 0; offset < out.size(); offset += 512)
            {
                const u64 relative = sector - owner_.bounds_.first_sector + offset / 512;
                const auto *block = owner_.proposed(relative / 2);
                if (block)
                    for (usize byte = 0; byte < 512; ++byte)
                        out[offset + byte] = std::byte((*block)[(relative % 2) * 512 + byte]);
            }
            return {};
        }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("ext4 transaction proposed view is readonly"); }
        Status flush() override { return Status::denied("ext4 transaction proposed view cannot flush"); }
      private:
        Ext4MetadataTransaction &owner_;
    };

  public:
    explicit Ext4MetadataTransaction(bool mounted_dirty = false)
        : clean_(mounted_dirty, mounted_dirty), proposed_(mounted_dirty, mounted_dirty),
          mounted_dirty_(mounted_dirty) {}
    Ext4MetadataTransaction(const Ext4MetadataTransaction &) = delete;
    Ext4MetadataTransaction &operator=(const Ext4MetadataTransaction &) = delete;
    Ext4MetadataTransaction(Ext4MetadataTransaction &&) = delete;
    Ext4MetadataTransaction &operator=(Ext4MetadataTransaction &&) = delete;
    [[nodiscard]] const Ext4TransactionReport &report() const { return report_; }

    [[nodiscard]] Status commit(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                Ext4InodeMetadataUpdate update)
    {
        if (const auto status = prepare(device, bounds, update); !status) return status;
        return publish_prepared();
    }

  private:
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <bool AllowDirectory = true, bool CheckedOwnership = false, bool CheckedMode = false>
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                 Ext4InodeMetadataUpdate update)
    {
        if (report_.phase != Ext4TransactionPhase::idle)
            return Status::already_exists("ext4 transaction is one-shot");
        report_.phase = Ext4TransactionPhase::validating;
        device_ = &device; bounds_ = bounds; geometry_ = device.geometry();
        if (update.permissions & ~u16{07777} || update.ctime_seconds > 0x7fffffffU ||
            update.ctime_nanoseconds >= 1000000000 ||
            (update.ordered_data && (update.mtime_seconds > 0x7fffffffU || update.mtime_nanoseconds >= 1000000000)))
            return fail(Status::invalid_argument("ext4 transaction mode or timestamp"));
        if (const auto status = clean_.prepare(device, bounds); !status)
            return fail(status);
        if (clean_.report().already_clean == mounted_dirty_ || !clean_.report().empty_log)
            return fail(Status::unsupported("ext4 transaction requires the selected empty journal state"));
        if (mounted_dirty_ && !update.ordered_data && !CheckedOwnership && !CheckedMode)
            return fail(Status::unsupported("mounted-dirty transaction requires ordered regular data"));
        auto &preview = clean_.preview_;
        const auto &super = preview.codec_->superblock();
        if (super.first_block != 1 || super.max_blocks < 6 || super.sequence > 0xfffffffcU)
            return fail(Status::unsupported("ext4 transaction ring or sequence capacity"));
        const auto inode = preview.inode(update.inode);
        if (!inode)
            return fail(inode.status());
        auto &view = preview.view_;
        const u16 type = inode.value().mode & 0170000;
        if (update.inode < view.first_ordinary_inode() ||
            (type != 0100000 && (!AllowDirectory || type != 0040000)))
            return fail(Status::unsupported("ext4 transaction requires ordinary regular inode or directory"));
        if (!CheckedOwnership && type == 0040000 && (update.uid != inode.value().uid || update.gid != inode.value().gid))
            return fail(Status::unsupported("ext4 directory transaction preserves ownership"));
        if constexpr (CheckedOwnership)
        {
            const bool changed = update.uid != inode.value().uid || update.gid != inode.value().gid;
            const u16 permissions = (inode.value().mode & 07777u) & (changed ? ~06000u : ~0u);
            if (update.ordered_data || update.permissions != permissions)
                return fail(Status::unsupported("checked ownership preserves data and nonprivileged permissions"));
            proposed_.policy_ = Ext4ReplayPolicy::checked_ownership;
            proposed_.preview_.policy_ = Ext4ReplayPolicy::checked_ownership;
        }
        if constexpr (CheckedMode)
        {
            static_assert(!CheckedOwnership);
            if (update.ordered_data || update.uid != inode.value().uid || update.gid != inode.value().gid)
                return fail(Status::unsupported("checked mode preserves ownership and data"));
            proposed_.policy_ = Ext4ReplayPolicy::checked_mode;
            proposed_.preview_.policy_ = Ext4ReplayPolicy::checked_mode;
        }
        if (update.ordered_data && (type != 0100000 || !update.size || !inode.value().size ||
            (u64{update.size} + 1023) / 1024 != (u64{inode.value().size} + 1023) / 1024 ||
            update.uid != inode.value().uid || update.gid != inode.value().gid ||
            update.permissions != (inode.value().mode & 07777)))
            return fail(Status::unsupported("ext4 ordered-data inode identity or block count"));
        const auto local = (update.inode - 1) % view.inodes_per_group_;
        report_.target_block = view.groups_[(update.inode - 1) / view.inodes_per_group_].table + local / 4;
        report_.inode = update.inode; report_.sequence = super.sequence + 1;
        journal_first_ = u32(preview.layout().journal_first_block);
        if (const auto status = preview.raw_block(report_.target_block, inode_); !status)
            return fail(status);
        filesystem_ = clean_.filesystem_; journal_ = clean_.journal_;
        if (!mounted_dirty_)
        {
            Completion::le32(filesystem_, 96, ext4_checked::le32(filesystem_, 96) | 4);
            Completion::le32(filesystem_, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{filesystem_}.first(1020)));
        }
        auto bytes = std::span<u8>{inode_}.subspan((local % 4) * 256, 256);
        const u64 version = u64{ext4_checked::le32(bytes, 36)} | (u64{ext4_checked::le32(bytes, 152)} << 32);
        if (version == ~u64{0})
            return fail(Status::unsupported("ext4 transaction inode version exhausted"));
        le16(bytes, 0, u16(type | update.permissions));
        le16(bytes, 2, u16(update.uid)); le16(bytes, 120, u16(update.uid >> 16));
        le16(bytes, 24, u16(update.gid)); le16(bytes, 122, u16(update.gid >> 16));
        Completion::le32(bytes, 12, update.ctime_seconds);
        Completion::le32(bytes, 132, update.ctime_nanoseconds << 2);
        if (update.ordered_data)
        {
            Completion::le32(bytes, 4, update.size);
            Completion::le32(bytes, 16, update.mtime_seconds);
            Completion::le32(bytes, 136, update.mtime_nanoseconds << 2);
        }
        Completion::le32(bytes, 36, u32(version + 1)); Completion::le32(bytes, 152, u32((version + 1) >> 32));
        le16(bytes, 124, 0); le16(bytes, 130, 0);
        const auto crc = jbd2::detail::crc32c(ext4_checked::inode_seed(view.checksum_seed(), update.inode, bytes), bytes);
        le16(bytes, 124, u16(crc)); le16(bytes, 130, u16(crc >> 16));

        header(descriptor_, 1); header(commit_, 2);
        const auto seed = view.checksum_seed();
        if (mounted_dirty_)
            tag(12, report_.target_block, jbd2::flag_last_tag, inode_, seed);
        else
        {
            tag(12, 1, 0, filesystem_, seed); // Explicit zero internal-journal tag UUID follows.
            tag(44, report_.target_block, jbd2::flag_same_uuid | jbd2::flag_last_tag, inode_, seed);
        }
        Completion::be32(descriptor_, 1020, jbd2::detail::crc32c(seed, descriptor_));
        Completion::be32(commit_, 52, update.ctime_seconds);
        Completion::be32(commit_, 56, update.ctime_nanoseconds);
        Completion::be32(commit_, 16, jbd2::detail::crc32c(seed, commit_));
        Completion::be32(journal_, 24, report_.sequence); Completion::be32(journal_, 28, 1);
        Completion::be32(journal_, 252, 0);
        Completion::be32(journal_, 252, jbd2::detail::crc32c(~u32{0}, journal_));
        // Admit every generated record/action and final allocated-inode view
        // through the same completion gate, without synthetic writes/flushes.
        proposed_.preview_.allow_ordered_data_ = update.ordered_data;
        if (const auto status = proposed_.prepare(proposed_device_, bounds); !status)
            return fail(status);
        if (proposed_.report().validated_actions != (mounted_dirty_ ? 1U : 2U) || proposed_.report().empty_log)
            return fail(Status::fault("ext4 transaction proposed journal invariant"));

        report_.phase = Ext4TransactionPhase::preflight;
        return {};
    }
    [[nodiscard]] Status logical_images(std::array<Block, 3> &images)
    {
        if (const auto status = proposed_.preview_.final_block(1, images[0]); !status) return status;
        if (const auto status = proposed_.preview_.final_block(report_.target_block, images[1]); !status) return status;
        images[2] = journal_;
        return {};
    }
    [[nodiscard]] Status publish_prepared()
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("ext4 transaction is not prepared");
        if (const auto status = flush(); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payloads;
        if (const auto status = write(journal_first_ + 1, descriptor_); !status) return fail(status);
        if (!mounted_dirty_)
            if (const auto status = write(journal_first_ + 2, filesystem_); !status) return fail(status);
        if (const auto status = write(journal_first_ + (mounted_dirty_ ? 2 : 3), inode_); !status) return fail(status);
        if (const auto status = write(journal_first_ + (mounted_dirty_ ? 4 : 5), end_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payload_flush;
        if (const auto status = flush(); !status) return fail(status);
        report_.payloads_durable = true;
        report_.phase = Ext4TransactionPhase::commit_record;
        if (const auto status = write(journal_first_ + (mounted_dirty_ ? 3 : 4), commit_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::commit_flush;
        if (const auto status = flush(); !status) return fail(status);
        report_.commit_record_durable = true;
        if (!mounted_dirty_)
        {
            report_.phase = Ext4TransactionPhase::recovery_bit;
            if (const auto status = write(1, filesystem_); !status) return fail(status);
            report_.phase = Ext4TransactionPhase::recovery_flush;
            if (const auto status = flush(); !status) return fail(status);
        }
        report_.recovery_bit_durable = true;
        report_.phase = Ext4TransactionPhase::publication;
        if (const auto status = write(journal_first_, journal_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::publication_flush;
        if (const auto status = flush(); !status) return fail(status);
        report_.publication_durable = true; report_.phase = Ext4TransactionPhase::published;
        return {};
    }

  private:
    [[nodiscard]] const Block *proposed(u64 block) const
    {
        if (block == 1 || (!mounted_dirty_ && block == u64{journal_first_} + 2)) return &filesystem_;
        if (block == journal_first_) return &journal_;
        if (block == u64{journal_first_} + 1) return &descriptor_;
        if (block == u64{journal_first_} + (mounted_dirty_ ? 2 : 3)) return &inode_;
        if (block == u64{journal_first_} + (mounted_dirty_ ? 3 : 4)) return &commit_;
        if (block == u64{journal_first_} + (mounted_dirty_ ? 4 : 5)) return &end_;
        return nullptr;
    }
    void header(Block &block, u32 type)
    {
        Completion::be32(block, 0, jbd2::magic); Completion::be32(block, 4, type);
        Completion::be32(block, 8, report_.sequence);
    }
    void tag(usize at, u32 target, u32 flags, Block &payload, u32 seed)
    {
        if (jbd2::detail::be32(payload, 0) == jbd2::magic)
        {
            Completion::be32(payload, 0, 0); flags |= jbd2::flag_escape;
        }
        Completion::be32(descriptor_, at, target); Completion::be32(descriptor_, at + 4, flags);
        const auto seq = std::span<const u8>{descriptor_}.subspan(8, 4);
        Completion::be32(descriptor_, at + 12, jbd2::detail::crc32c(jbd2::detail::crc32c(seed, seq), payload));
    }
    static void le16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    [[nodiscard]] Status stable() const
    {
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
               now.writable == geometry_.writable ? Status{} : Status::io_error("ext4 transaction device changed");
    }
    [[nodiscard]] Status write(u32 block, const Block &bytes)
    {
        if (const auto status = stable(); !status) return status;
        ++report_.write_attempts;
        if (const auto status = device_->write_blocks(bounds_.first_sector + u64{block} * 2, std::as_bytes(std::span{bytes})); !status)
            return status;
        ++report_.completed_writes; return stable();
    }
    [[nodiscard]] Status flush()
    {
        if (const auto status = stable(); !status) return status;
        ++report_.flush_attempts;
        if (const auto status = device_->flush(); !status) return status;
        ++report_.completed_flushes; return stable();
    }
    [[nodiscard]] Status fail(Status status)
    {
        report_.failed_phase = report_.phase; report_.phase = Ext4TransactionPhase::failed;
        report_.status = status; return status;
    }
    driver::BlockDevice *device_{};
    Ext4RecoveryBounds bounds_{};
    driver::BlockGeometry geometry_{};
    Completion clean_{}, proposed_{};
    ProposedDevice proposed_device_{*this};
    Ext4TransactionReport report_{};
    u32 journal_first_{};
    Block journal_{}, filesystem_{}, inode_{}, descriptor_{}, commit_{}, end_{};
    bool mounted_dirty_{};
};
} // namespace ok::fs
