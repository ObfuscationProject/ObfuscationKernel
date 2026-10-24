#pragma once

#include "ok/driver/driver.hpp"
#include "ok/fs/jbd2.hpp"

namespace ok::fs::jbd2
{

// Explicit contiguous internal-journal subset. All block indices except
// first_sector are filesystem blocks; first_sector uses device sectors.
struct RecoveryLayout
{
    u64 first_sector{};
    u64 filesystem_blocks{};
    u64 journal_first_block{};
    u32 journal_blocks{};
    u32 block_size{};
};

// The owner must quiesce filesystem users, caches and other device writers for
// this adapter's entire lifetime. It does not acquire that external ownership.
class RecoveryIo final
{
  public:
    [[nodiscard]] static Result<RecoveryIo> open(driver::BlockDevice &device, RecoveryLayout layout)
    {
        const auto geometry = device.geometry();
        if (!geometry.writable)
            return Status::denied("JBD2 recovery requires writable media");
        if ((layout.block_size != 1024 && layout.block_size != 2048 && layout.block_size != 4096) ||
            geometry.block_size == 0 || (geometry.block_size & (geometry.block_size - 1)) != 0 ||
            geometry.block_size > layout.block_size || layout.block_size % geometry.block_size != 0)
            return Status::unsupported("JBD2 recovery block geometry");
        const u64 sectors = layout.block_size / geometry.block_size;
        if (layout.filesystem_blocks == 0 || layout.first_sector >= geometry.block_count ||
            layout.filesystem_blocks > (geometry.block_count - layout.first_sector) / sectors ||
            layout.journal_blocks < 1024 || layout.journal_first_block >= layout.filesystem_blocks ||
            layout.journal_blocks > layout.filesystem_blocks - layout.journal_first_block)
            return Status::invalid_argument("JBD2 recovery filesystem or journal bounds");
        return RecoveryIo(device, layout, geometry);
    }

    [[nodiscard]] const RecoveryLayout &layout() const { return layout_; }

    [[nodiscard]] bool permits_range(BlockRange range) const
    {
        if (range.count == 0 || range.first >= layout_.filesystem_blocks ||
            range.count > layout_.filesystem_blocks - range.first)
            return false;
        const u64 end = range.first + range.count;
        const u64 journal_end = layout_.journal_first_block + layout_.journal_blocks;
        return end <= layout_.journal_first_block || range.first >= journal_end;
    }

    [[nodiscard]] Status read_block(u32 journal_block, std::span<u8> buffer)
    {
        if (buffer.size() != layout_.block_size || journal_block >= layout_.journal_blocks)
            return Status::invalid_argument("JBD2 recovery journal read bounds");
        if (const auto stable = stable_geometry(); !stable)
            return stable;
        if (const auto status = device_->read_blocks(sector(layout_.journal_first_block + journal_block),
                                                     std::as_writable_bytes(buffer)); !status)
            return status;
        return stable_geometry();
    }

    [[nodiscard]] Status write_block(u64 target, std::span<const u8> buffer)
    {
        if (buffer.size() != layout_.block_size || !permits_range({target, 1}))
            return Status::denied("JBD2 recovery write outside destination or inside journal");
        if (const auto stable = stable_geometry(); !stable)
            return stable;
        if (const auto status = device_->write_blocks(sector(target), std::as_bytes(buffer)); !status)
            return status;
        return stable_geometry();
    }

    [[nodiscard]] Status flush()
    {
        if (const auto stable = stable_geometry(); !stable)
            return stable;
        if (const auto status = device_->flush(); !status)
            return status;
        return stable_geometry();
    }

  private:
    RecoveryIo(driver::BlockDevice &device, RecoveryLayout layout, driver::BlockGeometry geometry)
        : device_(&device), layout_(layout), geometry_(geometry)
    {
    }

    [[nodiscard]] u64 sector(u64 filesystem_block) const
    {
        return layout_.first_sector + filesystem_block * (layout_.block_size / geometry_.block_size);
    }

    [[nodiscard]] Status stable_geometry() const
    {
        const auto now = device_->geometry();
        if (now.block_size != geometry_.block_size || now.block_count != geometry_.block_count || !now.writable)
            return Status::io_error("JBD2 recovery device geometry changed");
        return Status::success();
    }

    driver::BlockDevice *device_;
    RecoveryLayout layout_;
    driver::BlockGeometry geometry_;
};

enum class ReplayPhase : u8
{
    idle,
    validating,
    planning,
    preflight,
    replaying,
    flushing,
    complete,
    failed,
};

struct ReplayReport
{
    ReplayPhase phase{ReplayPhase::idle};
    ReplayPhase failed_phase{ReplayPhase::idle};
    Status status{};
    usize planned_actions{};
    usize revoked_actions{};
    u32 committed_transactions{};
    u32 next_sequence{};
    usize write_attempts{};
    usize completed_writes{};
    usize flush_attempts{};
    usize completed_flushes{};
    bool empty_log{};
    bool incomplete_tail{};
    bool preflight_flush_succeeded{};
    bool final_flush_succeeded{};

    // A failing block write may have changed some or all bytes.
    [[nodiscard]] bool destination_may_have_changed() const { return write_attempts != 0; }
    [[nodiscard]] bool replay_durable() const
    {
        return phase == ReplayPhase::complete && final_flush_succeeded;
    }
};

// Owns all plan, revoke and I/O scratch. A caller cannot execute a forged or
// expired ReplayPlan. One instance admits exactly one serialized run attempt.
template <usize ActionCapacity = 64, usize RevokeCapacity = 64, usize DomainCapacity = 32>
    requires (ActionCapacity > 0 && RevokeCapacity > 0 && DomainCapacity > 0)
class ReplayExecutor final
{
  public:
    ReplayExecutor() = default;
    ReplayExecutor(const ReplayExecutor &) = delete;
    ReplayExecutor &operator=(const ReplayExecutor &) = delete;
    ReplayExecutor(ReplayExecutor &&) = delete;
    ReplayExecutor &operator=(ReplayExecutor &&) = delete;

    [[nodiscard]] const ReplayReport &report() const { return report_; }

    // The device, journal mapping, UUID and metadata domain must refer to one
    // exclusively owned filesystem. The journal remains immutable and dirty;
    // success is durable action replay, never permission to publish a mount.
    // linux_internal_zero additionally requires caller-qualified internal-journal
    // inode ownership and mapping; do not infer it from observed zero tag bytes.
    [[nodiscard]] Status run(RecoveryIo &io, std::span<const u8, 16> expected_uuid, const TargetDomain &domain,
                             DescriptorUuidPolicy tag_policy = DescriptorUuidPolicy::journal_superblock)
    {
        if (report_.phase != ReplayPhase::idle)
            return Status::already_exists("JBD2 replay executor is one-shot");
        report_.phase = ReplayPhase::validating;
        if (!domain.valid() || domain.filesystem_blocks != io.layout().filesystem_blocks)
            return fail(Status::invalid_argument("JBD2 replay target domain differs from device layout"));
        if (domain.metadata.size() > ranges_.size())
            return fail(Status::no_memory("JBD2 replay domain capacity"));
        for (usize index = 0; index < domain.metadata.size(); ++index)
        {
            if (!io.permits_range(domain.metadata[index]))
                return fail(Status::denied("JBD2 replay domain overlaps journal or exceeds destination"));
            ranges_[index] = domain.metadata[index];
        }
        const TargetDomain owned_domain{domain.filesystem_blocks,
                                        std::span<const BlockRange>{ranges_}.first(domain.metadata.size())};
        std::array<u8, 16> owned_uuid{};
        for (usize index = 0; index < owned_uuid.size(); ++index)
            owned_uuid[index] = expected_uuid[index];

        // Prove the actual device implements flush before any recovery write.
        // Quiescing the device and excluding other outstanding I/O is external.
        ++report_.flush_attempts;
        if (const auto flushed = io.flush(); !flushed)
            return fail(flushed);
        ++report_.completed_flushes;
        report_.preflight_flush_succeeded = true;

        report_.phase = ReplayPhase::planning;
        auto record = std::span<u8>{record_}.first(io.layout().block_size);
        auto payload = std::span<u8>{payload_}.first(io.layout().block_size);
        if (const auto read = io.read_block(0, record); !read)
            return fail(read);
        auto codec = Codec::open(record, io.layout().journal_blocks, owned_uuid, tag_policy);
        if (!codec)
            return fail(codec.status());
        if (codec.value().superblock().block_size != io.layout().block_size)
            return fail(Status::invalid_argument("JBD2 superblock and recovery transfer size differ"));
        const auto planned = build_replay_plan(codec.value(), io, owned_domain,
                                               PlanStorage{record, payload, actions_, revocations_});
        if (!planned)
            return fail(planned.status());
        const auto &plan = planned.value();
        report_.planned_actions = plan.actions.size();
        report_.revoked_actions = plan.revoked_actions;
        report_.committed_transactions = plan.committed_transactions;
        report_.next_sequence = plan.next_sequence;
        report_.empty_log = plan.empty_log;
        report_.incomplete_tail = plan.incomplete_tail;

        report_.phase = ReplayPhase::preflight;
        const auto &sb = codec.value().superblock();
        u32 previous_order = 0;
        if (plan.next_sequence != sb.sequence + plan.committed_transactions)
            return fail(Status::invalid_argument("JBD2 replay plan sequence invariant"));
        for (const auto &action : plan.actions)
        {
            if (!owned_domain.permits(action.target_block) || !io.permits_range({action.target_block, 1}) ||
                action.journal_block < sb.first_block || action.journal_block >= sb.max_blocks ||
                action.transaction_order >= plan.committed_transactions || action.transaction_order < previous_order ||
                action.sequence != sb.sequence + action.transaction_order)
                return fail(Status::invalid_argument("JBD2 replay action bounds or transaction order"));
            previous_order = action.transaction_order;
            if (const auto checked = read_payload(io, codec.value(), action, payload); !checked)
                return fail(checked);
        }

        report_.phase = ReplayPhase::replaying;
        for (const auto &action : plan.actions)
        {
            // Revalidate the bytes actually submitted, not only a past read.
            if (const auto checked = read_payload(io, codec.value(), action, payload); !checked)
                return fail(checked);
            if (action.restore_magic)
            {
                payload[0] = u8(magic >> 24);
                payload[1] = u8(magic >> 16);
                payload[2] = u8(magic >> 8);
                payload[3] = u8(magic);
            }
            ++report_.write_attempts;
            if (const auto written = io.write_block(action.target_block, payload); !written)
                return fail(written);
            ++report_.completed_writes;
        }

        report_.phase = ReplayPhase::flushing;
        ++report_.flush_attempts;
        if (const auto flushed = io.flush(); !flushed)
            return fail(flushed);
        ++report_.completed_flushes;
        report_.final_flush_succeeded = true;
        report_.phase = ReplayPhase::complete;
        report_.status = Status::success();
        return Status::success();
    }

  private:
    [[nodiscard]] Status fail(Status status)
    {
        report_.failed_phase = report_.phase;
        report_.phase = ReplayPhase::failed;
        report_.status = status;
        return status;
    }

    [[nodiscard]] static Status read_payload(RecoveryIo &io, const Codec &codec, const ReplayAction &action,
                                             std::span<u8> buffer)
    {
        if (const auto read = io.read_block(action.journal_block, buffer); !read)
            return read;
        return codec.payload(buffer, action.sequence, Tag{action.target_block, action.checksum, action.restore_magic});
    }

    ReplayReport report_{};
    std::array<u8, 4096> record_{};
    std::array<u8, 4096> payload_{};
    std::array<ReplayAction, ActionCapacity> actions_{};
    std::array<Revocation, RevokeCapacity> revocations_{};
    std::array<BlockRange, DomainCapacity> ranges_{};
};

} // namespace ok::fs::jbd2
