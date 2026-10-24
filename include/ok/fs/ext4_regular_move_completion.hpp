#pragma once

#include "ok/fs/ext4_regular_move_recovery.hpp"

namespace ok::fs
{
enum class Ext4RegularMoveCompletionPhase : u8
{
    idle, validating, prepared, rechecking, preflight_flush, homes, home_flush, journal_head,
    journal_flush, verifying, complete, failed,
};

struct Ext4RegularMoveCompletionReport
{
    Ext4RegularMoveCompletionPhase phase{Ext4RegularMoveCompletionPhase::idle};
    Ext4RegularMoveCompletionPhase failed_phase{Ext4RegularMoveCompletionPhase::idle};
    Status status{};
    usize validated_actions{}, sealed_snapshots{}, missing_homes{};
    usize write_attempts{}, completed_writes{}, flush_attempts{}, completed_flushes{};
    u32 active_sequence{}, empty_sequence{};
    bool preflight_durable{}, homes_durable{}, journal_durable{}, physical_verified{}, filesystem_dirty_retained{};
    [[nodiscard]] bool destination_may_have_changed() const { return write_attempts != 0; }
    [[nodiscard]] bool complete() const
    {
        return phase == Ext4RegularMoveCompletionPhase::complete && preflight_durable && homes_durable &&
            journal_durable && physical_verified && filesystem_dirty_retained;
    }
};

// One-shot completion of this distinct active regular-move witness profile.
// The public source-prototype API does not acquire exclusive/quiesced ownership,
// issue a mount capability or admit a mounted transaction. The caller retains
// its exclusive session across preparation, execution and failure retirement.
// Whole-home old/new recovery is supported; torn/foreign images fail closed.
class Ext4RegularMoveCompletion final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = Ext4RegularMoveBlock;

  public:
    Ext4RegularMoveCompletion() = default;
    Ext4RegularMoveCompletion(const Ext4RegularMoveCompletion &) = delete;
    Ext4RegularMoveCompletion &operator=(const Ext4RegularMoveCompletion &) = delete;
    Ext4RegularMoveCompletion(Ext4RegularMoveCompletion &&) = delete;
    Ext4RegularMoveCompletion &operator=(Ext4RegularMoveCompletion &&) = delete;
    [[nodiscard]] const Ext4RegularMoveCompletionReport &report() const { return report_; }
    [[nodiscard]] const Ext4RegularMoveRecoveryBinding &binding() const { return binding_; }

    // Preparation performs only actual device reads and semantic admission.
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    {
        if (report_.phase != Ext4RegularMoveCompletionPhase::idle)
            return Status::already_exists("regular move completion is one-shot");
        report_.phase = Ext4RegularMoveCompletionPhase::validating;
        if (!device.geometry().writable)
            return fail(Status::denied("regular move completion requires writable media"));
        if (auto status = binding_.inspect(device, bounds); !status) return fail(status);
        if (!binding_.geometry_.writable)
            return fail(Status::io_error("regular move completion writable geometry changed"));
        report_.validated_actions = binding_.validation_.count;
        report_.sealed_snapshots = binding_.snapshot_count_;
        report_.active_sequence = binding_.generic_.info()->journal_sequence;
        report_.empty_sequence = report_.active_sequence + 2;
        for (usize index = 0; index < binding_.validation_.count; ++index)
            if (!binding_.validation_.home_after[index]) ++report_.missing_homes;
        report_.phase = Ext4RegularMoveCompletionPhase::prepared;
        return {};
    }

    [[nodiscard]] Status execute()
    {
        if (report_.phase != Ext4RegularMoveCompletionPhase::prepared)
            return Status::bad_descriptor("regular move completion is not prepared");
        // Retire readiness before any observation. Even a read/preflight error
        // consumes this attempt, so a stale report cannot be retried in place.
        report_.phase = Ext4RegularMoveCompletionPhase::rechecking;
        if (auto status = binding_.recheck(); !status) return fail(status);
        // Every exact snapshot has been compared before any write or FLUSH.
        // No broad structural/rename profile can substitute for this binding.
        // Establish that the observed active log is durable before any home is
        // written: a successful read may otherwise observe volatile live I/O.
        report_.phase = Ext4RegularMoveCompletionPhase::preflight_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.preflight_durable = true;
        report_.phase = Ext4RegularMoveCompletionPhase::homes;
        for (usize index = 0; index < binding_.validation_.count; ++index)
            if (!binding_.validation_.home_after[index])
                if (auto status = write(binding_.scratch_.canonical.targets[index],
                                        binding_.scratch_.canonical.after[index]); !status) return fail(status);
        report_.phase = Ext4RegularMoveCompletionPhase::home_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.homes_durable = true;
        report_.phase = Ext4RegularMoveCompletionPhase::journal_head;
        if (auto status = write(binding_.journal_first_, binding_.empty_head_); !status) return fail(status);
        report_.phase = Ext4RegularMoveCompletionPhase::journal_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.journal_durable = true;
        report_.phase = Ext4RegularMoveCompletionPhase::verifying;
        if (auto status = verify_physical(); !status) return fail(status);
        report_.physical_verified = true;
        report_.filesystem_dirty_retained = true; // Original dirty superblock is exact and unwritten.
        report_.phase = Ext4RegularMoveCompletionPhase::complete;
        return {};
    }

  private:
    // Only the canonical Owner can bind these complete retained images to its
    // exact cache participants. Public reports remain observations, not seals.
    template <usize Capacity> requires (Capacity >= Ext4RegularMovePlan::capacity + 1)
    [[nodiscard]] Status logical_images(std::array<u64, Capacity> &blocks,
        std::array<Block, Capacity> &images, usize &count) const
    {
        if (report_.phase != Ext4RegularMoveCompletionPhase::prepared || !binding_.admitted_)
            return Status::bad_descriptor("regular move logical images lack prepared admission");
        count = binding_.validation_.count + 1;
        blocks[0] = binding_.journal_first_; images[0] = binding_.journal_[0];
        for (usize index = 0; index < binding_.validation_.count; ++index)
        { blocks[index + 1] = binding_.scratch_.canonical.targets[index];
          images[index + 1] = binding_.scratch_.canonical.after[index]; }
        return {};
    }
    [[nodiscard]] const Block *observed_preimage(u64 block) const
    {
        if (report_.phase != Ext4RegularMoveCompletionPhase::prepared || !binding_.admitted_) return nullptr;
        for (usize index = 0; index < binding_.snapshot_count_; ++index)
            if (binding_.snapshots_[index].block == block) return binding_.snapshots_[index].image;
        return nullptr;
    }
    template <usize Capacity>
    [[nodiscard]] Status matches(const std::array<u64, Capacity> &blocks,
        const std::array<Block, Capacity> &images, usize count, u32 sequence) const
    {
        if (report_.phase != Ext4RegularMoveCompletionPhase::prepared || !binding_.admitted_ ||
            count != binding_.validation_.count + 1 || count > Capacity ||
            sequence != report_.active_sequence || blocks[0] != binding_.journal_first_ ||
            images[0] != binding_.journal_[0])
            return Status::fault("regular move completion original participant identity differs");
        for (usize index = 1; index < count; ++index)
            if (blocks[index] != binding_.scratch_.canonical.targets[index - 1] ||
                images[index] != binding_.scratch_.canonical.after[index - 1])
                return Status::fault("regular move completion original participant image differs");
        return {};
    }
    [[nodiscard]] Status matches_published(const Ext4RegularMoveJournalPlan &journal,
                                            const Ext4RegularMovePlan &plan) const
    {
        const usize count = binding_.validation_.count;
        if (report_.phase != Ext4RegularMoveCompletionPhase::prepared || !binding_.admitted_ ||
            !journal.ready || !plan.count || count != journal.count || count != plan.count ||
            report_.active_sequence != journal.sequence || binding_.journal_count_ != count + 4 ||
            binding_.journal_[0] != journal.journal_head || binding_.journal_[1] != journal.descriptor ||
            binding_.journal_[count + 2] != journal.commit || binding_.journal_[count + 3] != journal.end)
            return Status::fault("regular move completion differs from original published journal");
        for (usize index = 0; index < count; ++index)
            if (binding_.journal_[index + 2] != journal.payloads[index] ||
                binding_.scratch_.canonical.targets[index] != plan.targets[index] ||
                binding_.scratch_.canonical.before[index] != plan.before[index] ||
                binding_.scratch_.canonical.after[index] != plan.after[index])
                return Status::fault("regular move completion differs from original writer homes");
        return {};
    }
    [[nodiscard]] Status verify_terminal()
    {
        if (!report_.complete()) return Status::bad_descriptor("regular move terminal proof is incomplete");
        return verify_physical();
    }
    [[nodiscard]] Status write(u32 block, const Block &image)
    {
        if (auto status = binding_.bounded(block); !status) return status;
        ++report_.write_attempts;
        if (auto status = binding_.device_->write_blocks(binding_.bounds_.first_sector + u64{block} * 2,
                std::as_bytes(std::span{image})); !status) return status;
        ++report_.completed_writes;
        return binding_.stable();
    }
    [[nodiscard]] Status flush()
    {
        if (auto status = binding_.stable(); !status) return status;
        ++report_.flush_attempts;
        if (auto status = binding_.device_->flush(); !status) return status;
        ++report_.completed_flushes;
        return binding_.stable();
    }
    [[nodiscard]] Status verify_physical()
    {
        if (auto status = binding_.stable(); !status) return status;
        for (usize snapshot = 0; snapshot < binding_.snapshot_count_; ++snapshot)
        {
            const auto &observed = binding_.snapshots_[snapshot];
            const Block *expected = observed.image;
            if (observed.block == binding_.journal_first_) expected = &binding_.empty_head_;
            else
                for (usize home = 0; home < binding_.validation_.count; ++home)
                    if (observed.block == binding_.scratch_.canonical.targets[home])
                    { expected = &binding_.scratch_.canonical.after[home]; break; }
            // A journal-inode ancillary table may also be a selected home.
            // That one physical block must verify against its whole after image.
            if (auto status = binding_.read(observed.block, binding_.probe_); !status) return status;
            if (binding_.probe_ != *expected)
                return Status::io_error("regular move completion physical image differs");
        }
        return binding_.stable();
    }
    [[nodiscard]] Status fail(Status status)
    {
        report_.failed_phase = report_.phase;
        report_.phase = Ext4RegularMoveCompletionPhase::failed;
        report_.status = status;
        return status;
    }

    Ext4RegularMoveRecoveryBinding binding_{};
    Ext4RegularMoveCompletionReport report_{};
};
} // namespace ok::fs
