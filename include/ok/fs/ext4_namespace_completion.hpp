#pragma once

#include "ok/fs/ext4_namespace_recovery_binding.hpp"
#include "ok/fs/ext4_completion.hpp"

namespace ok::fs
{
// Only a canonical owner may execute this checked, same-session proof. An empty
// journal with a published orphan is a cleanup state, not ordinary mount access.
class Ext4NamespaceCompletion final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = Ext4NamespaceBlock;
  public:
    Ext4NamespaceCompletion() = default;
    Ext4NamespaceCompletion(const Ext4NamespaceCompletion &) = delete;
    Ext4NamespaceCompletion &operator=(const Ext4NamespaceCompletion &) = delete;
    [[nodiscard]] const Ext4CompletionReport &report() const { return report_; }
    [[nodiscard]] bool orphan_cleanup_pending() const { return orphan_; }

  private:
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    {
        if (report_.phase != Ext4CompletionPhase::idle)
            return Status::already_exists("namespace completion is one-shot");
        report_.phase = Ext4CompletionPhase::validating;
        device_ = &device; geometry_ = device.geometry();
        if (!geometry_.writable || geometry_.block_size != 512)
            return fail(Status::unsupported("namespace completion requires writable 512-byte sectors"));
        if (auto status = admission_.inspect(device, bounds); !status) return fail(status);
        const auto *info = admission_.info();
        const auto &proof = *admission_.result();
        if (!info || !proof.durable_old_witness || !proof.homes.allocation_unchanged ||
            proof.homes.count < 2 || proof.homes.count > 5 || info->journal_sequence > 0xfffffffeU)
            return fail(Status::fault("namespace completion admitted proof profile"));
        layout_ = info->layout; count_ = proof.homes.count;
        sequence_ = info->journal_sequence; orphan_ = proof.homes.orphan_published;
        report_.validated_actions = count_;
        // Keep the existing completion convention: reserve the parser's next
        // sequence and store the following empty-head sequence. Offline Linux
        // recovery also maps active N to empty N+2 in the qualified profile.
        report_.journal_sequence = sequence_ + 2;
        journal_ = admission_.journal_head();
        big32(journal_, 24, report_.journal_sequence); big32(journal_, 28, 0);
        big32(journal_, 252, 0);
        big32(journal_, 252, jbd2::detail::crc32c(~u32{0}, journal_));
        report_.phase = Ext4CompletionPhase::preflight;
        return {};
    }
    template <usize Capacity> requires (Capacity >= 6)
    [[nodiscard]] Status logical_images(std::array<u64, Capacity> &blocks,
                                        std::array<Block, Capacity> &images, usize &count) const
    {
        if (report_.phase != Ext4CompletionPhase::preflight)
            return Status::bad_descriptor("namespace completion has no admitted images");
        blocks[0] = layout_.journal_first_block; images[0] = admission_.journal_head();
        for (usize i = 0; i < count_; ++i)
        { blocks[i + 1] = admission_.result()->homes.targets[i]; images[i + 1] = admission_.scratch()->after[i]; }
        count = count_ + 1;
        return {};
    }
    template <usize Capacity>
    [[nodiscard]] Status matches(const std::array<u64, Capacity> &blocks,
                                 const std::array<Block, Capacity> &images, usize count,
                                 u32 sequence) const
    {
        if (report_.phase != Ext4CompletionPhase::preflight || sequence != sequence_ ||
            count != count_ + 1 || count > Capacity || blocks[0] != layout_.journal_first_block ||
            images[0] != admission_.journal_head())
            return Status::fault("namespace completion publication identity");
        for (usize i = 0; i < count_; ++i)
            if (blocks[i + 1] != admission_.result()->homes.targets[i] ||
                images[i + 1] != admission_.scratch()->after[i])
                return Status::fault("namespace completion canonical publication bytes differ");
        return {};
    }
    [[nodiscard]] Status execute()
    {
        if (report_.phase != Ext4CompletionPhase::preflight)
            return Status::bad_descriptor("namespace completion is not prepared");
        // Cache staging may have performed reads since admission. Recheck the
        // complete transaction and whole observed homes before any transport
        // ordering change. This does not refresh or weaken the sealed proof.
        if (auto status = current(); !status) return fail(status);
        if (auto status = flush(); !status) return fail(status);
        const auto &homes = admission_.result()->homes;
        if (!homes.all_after)
        {
            report_.phase = Ext4CompletionPhase::replaying;
            for (usize i = 0; i < count_; ++i)
                if (!homes.home_after[i])
                    if (auto status = write(homes.targets[i], admission_.scratch()->after[i]); !status)
                        return fail(status);
            report_.phase = Ext4CompletionPhase::home_flush;
            if (auto status = flush(); !status) return fail(status);
        }
        report_.home_durable = true;
        report_.phase = Ext4CompletionPhase::journal_clean;
        if (auto status = write(u32(layout_.journal_first_block), journal_); !status) return fail(status);
        report_.phase = Ext4CompletionPhase::journal_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.journal_durable = true;
        // The recovery bit and allocated orphan are deliberately retained.
        // Compare physical bytes; a successful transport status is insufficient.
        for (usize i = 0; i < count_; ++i)
            if (auto status = equal(homes.targets[i], admission_.scratch()->after[i]); !status)
                return fail(status);
        if (auto status = equal(u32(layout_.journal_first_block), journal_); !status) return fail(status);
        report_.filesystem_durable = true;
        report_.phase = Ext4CompletionPhase::complete;
        return {};
    }
    [[nodiscard]] Status matches_published(const Ext4NamespaceJournalPlan &journal,
                                           const Ext4NamespacePlan &homes) const
    {
        const auto &canonical = admission_.scratch()->canonical;
        if (report_.phase != Ext4CompletionPhase::preflight || !journal.ready || journal.no_op ||
            homes.no_op || journal.sequence != sequence_ || journal.count != count_ || homes.count != count_ ||
            journal.journal_head != admission_.journal_head() || journal.descriptor != canonical.descriptor ||
            journal.commit != canonical.commit || journal.end != canonical.end)
            return Status::fault("namespace completion differs from owned durable journal");
        for (usize i = 0; i < count_; ++i)
            if (journal.targets[i] != canonical.targets[i] || homes.targets[i] != canonical.targets[i] ||
                journal.payloads[i] != canonical.payloads[i] || homes.before[i] != admission_.scratch()->before[i] ||
                homes.after[i] != admission_.scratch()->after[i])
                return Status::fault("namespace completion differs from owned historical images");
        return {};
    }
    [[nodiscard]] Status current()
    {
        const auto &canonical = admission_.scratch()->canonical;
        const u32 first = u32(layout_.journal_first_block);
        if (auto status = equal(first, admission_.journal_head()); !status) return status;
        if (auto status = equal(first + 1, canonical.descriptor); !status) return status;
        for (usize i = 0; i < count_; ++i)
        {
            if (auto status = equal(first + u32(i) + 2, canonical.payloads[i]); !status) return status;
            const auto &observed = admission_.result()->homes.home_after[i] ?
                admission_.scratch()->after[i] : admission_.scratch()->before[i];
            if (auto status = equal(admission_.result()->homes.targets[i], observed); !status) return status;
        }
        if (auto status = equal(first + u32(count_) + 2, canonical.commit); !status) return status;
        return equal(first + u32(count_) + 3, canonical.end);
    }
    [[nodiscard]] Status equal(u32 block, const Block &expected)
    {
        if (auto status = stable(); !status) return status;
        if (block >= layout_.filesystem_blocks) return Status::invalid_argument("namespace completion read bounds");
        if (auto status = device_->read_blocks(layout_.first_sector + u64{block} * 2,
            std::as_writable_bytes(std::span{payload_})); !status) return status;
        if (auto status = stable(); !status) return status;
        return payload_ == expected ? Status{} : Status::io_error("namespace completion physical image differs");
    }
    [[nodiscard]] Status stable() const
    {
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
            now.writable == geometry_.writable ? Status{} : Status::io_error("namespace completion device changed");
    }
    [[nodiscard]] Status write(u32 block, const Block &image)
    {
        if (block >= layout_.filesystem_blocks) return Status::invalid_argument("namespace completion write bounds");
        if (auto status = stable(); !status) return status;
        ++report_.write_attempts;
        if (auto status = device_->write_blocks(layout_.first_sector + u64{block} * 2,
            std::as_bytes(std::span{image})); !status) return status;
        ++report_.completed_writes;
        return stable();
    }
    [[nodiscard]] Status flush()
    {
        if (auto status = stable(); !status) return status;
        ++report_.flush_attempts;
        if (auto status = device_->flush(); !status) return status;
        ++report_.completed_flushes;
        return stable();
    }
    [[nodiscard]] Status fail(Status status)
    {
        report_.failed_phase = report_.phase; report_.phase = Ext4CompletionPhase::failed;
        report_.status = status; return status;
    }
    static void big32(Block &block, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) block[at + i] = u8(value >> (24 - i * 8)); }
    Ext4NamespaceRecoveryAdmission admission_{};
    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    jbd2::RecoveryLayout layout_{};
    Ext4CompletionReport report_{};
    Block journal_{}, payload_{};
    usize count_{};
    u32 sequence_{};
    bool orphan_{};
};
} // namespace ok::fs
