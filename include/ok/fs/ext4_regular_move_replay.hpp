#pragma once

#include "ok/fs/ext4_regular_move_witness.hpp"

namespace ok::fs
{
struct Ext4RegularMoveReplayAncillary
{
    const Ext4RegularMoveBlock &superblock;
    const Ext4RegularMoveBlock &descriptors;
    const std::array<Ext4RegularMoveBlock, 4> &inode_bitmaps;
    const std::array<Ext4RegularMoveBlock, 4> &block_bitmaps;
    const Ext4RegularMoveEmptyOrphanAuthorization &orphan;
};

struct Ext4RegularMoveReplayScratch
{
    std::array<Ext4RegularMoveBlock, Ext4RegularMovePlan::capacity> after{}, before{};
    Ext4RegularMovePlan canonical{};
};

struct Ext4RegularMoveReplayValidation
{
    std::array<bool, Ext4RegularMovePlan::capacity> home_after{};
    usize count{};
    u32 from_parent{}, to_parent{}, source{};
    u32 from_generation{}, to_generation{}, source_generation{};
    bool all_homes_after{};
};

// Pure validation of one already-parsed immutable committed journal snapshot.
// No device reads, writes, cache exclusions, session ownership or classifier.
// The caller supplies raw escaped payloads and homes in replay-action order.
// A successful report is semantic evidence, never recovery I/O authority.
class Ext4RegularMoveReplayValidator final
{
    using Block = Ext4RegularMoveBlock;
    [[nodiscard]] static bool overlap(const void *left, usize left_bytes, const void *right, usize right_bytes)
    {
        if (!left_bytes || !right_bytes) return false;
        const usize a = reinterpret_cast<usize>(left), b = reinterpret_cast<usize>(right);
        return (a >= b && a - b < right_bytes) || (b >= a && b - a < left_bytes);
    }
    [[nodiscard]] static u32 table_for(const Block &descriptors, u32 number)
    {
        return ext4_checked::le32(descriptors, ((number - 1) / 2048) * 32 + 8) +
               ((number - 1) % 2048) / 4;
    }
    [[nodiscard]] static usize home_for(const Ext4RegularMoveWitness &witness, u32 target)
    {
        for (usize index = 0; index < witness.count; ++index)
            if (witness.targets[index] == target) return index;
        return Ext4RegularMovePlan::capacity;
    }

  public:
    // Capacity/alias refusals preserve both outputs. Later failure leaves a
    // zero report; scratch may contain partial unadmitted reconstructed bytes.
    [[nodiscard]] static Status validate(const Ext4RecoveryInfo &binding, const jbd2::Codec &codec,
        const jbd2::ReplayPlan &replay, const Block &descriptor,
        std::span<const Block> journal_payloads, std::span<const Block> current_homes,
        const Ext4RegularMoveReplayAncillary &ancillary, Ext4RegularMoveReplayScratch &scratch,
        Ext4RegularMoveReplayValidation &output)
    {
        constexpr usize capacity = Ext4RegularMovePlan::capacity;
        if (journal_payloads.size() > capacity || current_homes.size() > capacity || replay.actions.size() > capacity)
            return Status::invalid_argument("regular move replay span capacity");
        const auto aliases = [&](const void *data, usize bytes) {
            return overlap(&scratch, sizeof(scratch), data, bytes) ||
                   overlap(&output, sizeof(output), data, bytes);
        };
        if (overlap(&scratch, sizeof(scratch), &output, sizeof(output)) ||
            aliases(&binding, sizeof(binding)) || aliases(&codec, sizeof(codec)) ||
            aliases(&replay, sizeof(replay)) || aliases(replay.actions.data(), replay.actions.size_bytes()) ||
            aliases(&descriptor, sizeof(descriptor)) ||
            aliases(journal_payloads.data(), journal_payloads.size_bytes()) ||
            aliases(current_homes.data(), current_homes.size_bytes()) ||
            aliases(&ancillary, sizeof(ancillary)) || aliases(&ancillary.superblock, sizeof(Block)) ||
            aliases(&ancillary.descriptors, sizeof(Block)) ||
            aliases(&ancillary.inode_bitmaps, sizeof(ancillary.inode_bitmaps)) ||
            aliases(&ancillary.block_bitmaps, sizeof(ancillary.block_bitmaps)) ||
            aliases(&ancillary.orphan, sizeof(ancillary.orphan)))
            return Status::invalid_argument("regular move replay output aliases input");
        output = {}; scratch = {};
        const usize count = replay.actions.size();
        const auto &head = codec.superblock();
        if (count < 3 || journal_payloads.size() != count || current_homes.size() != count ||
            replay.empty_log || replay.committed_transactions != 1 || replay.incomplete_tail ||
            replay.revoked_actions || replay.revoke_records || head.block_size != 1024 ||
            binding.layout.block_size != 1024 || head.uuid != binding.uuid ||
            head.max_blocks != binding.layout.journal_blocks || head.first_block != 1 || head.start_block != 1 ||
            (head.features != jbd2::feature_checksum_v3 &&
             head.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)) ||
            count + 3 >= head.max_blocks || head.sequence > 0xfffffffdU ||
            replay.next_sequence != head.sequence + 1 || replay.scanned_blocks != count + 3)
            return Status::unsupported("regular move replay transaction shape");
        if (!Ext4RegularMoveWitnessCodec::present(descriptor))
            return Status::unsupported("regular move replay requires OKMVW001 witness");
        for (usize index = 0; index < count; ++index)
        {
            const auto &action = replay.actions[index];
            if (!action.target_block || action.target_block >= binding.layout.filesystem_blocks ||
                action.journal_block != index + 2 || action.transaction_order || action.sequence != head.sequence)
                return Status::unsupported("regular move replay action placement");
            for (usize previous = 0; previous < index; ++previous)
                if (replay.actions[previous].target_block == action.target_block)
                    return Status::unsupported("regular move replay duplicate home");
            const usize at = index ? 44 + (index - 1) * 16 : 12;
            const u32 flags = (action.restore_magic ? jbd2::flag_escape : 0) |
                (index ? jbd2::flag_same_uuid : 0) | (index + 1 == count ? jbd2::flag_last_tag : 0);
            if (jbd2::detail::be32(descriptor, at + 4) != flags ||
                jbd2::detail::be32(descriptor, at + 8))
                return Status::unsupported("regular move replay noncanonical descriptor tags");
        }
        if (!jbd2::detail::all_zero(std::span<const u8>{descriptor}.subspan(28, 16)))
            return Status::unsupported("regular move replay internal descriptor UUID");
        usize tags{};
        if (auto status = codec.descriptor(descriptor, head.sequence, [&](const jbd2::Tag &tag) -> Status {
                if (tags >= count || tag.target_block != replay.actions[tags].target_block ||
                    tag.checksum != replay.actions[tags].checksum ||
                    tag.restore_magic != replay.actions[tags].restore_magic)
                    return Status::io_error("regular move replay descriptor differs from parsed actions");
                ++tags; return {};
            }); !status) return status;
        if (tags != count) return Status::io_error("regular move replay descriptor tag count");
        for (usize index = 0; index < count; ++index)
        {
            const auto &action = replay.actions[index];
            if (auto status = codec.payload(journal_payloads[index], head.sequence,
                {action.target_block, action.checksum, action.restore_magic}); !status) return status;
            scratch.after[index] = journal_payloads[index];
            if (action.restore_magic)
            {
                scratch.after[index][0] = 0xc0; scratch.after[index][1] = 0x3b;
                scratch.after[index][2] = 0x39; scratch.after[index][3] = 0x98;
            }
        }
        Ext4RegularMoveWitness witness{};
        if (auto status = Ext4RegularMoveWitnessCodec::decode(descriptor,
            std::span<const Block>{scratch.after}.first(count), std::span<Block>{scratch.before}.first(count), witness);
            !status) return status;
        for (usize index = 0; index < count; ++index)
            if (witness.targets[index] != replay.actions[index].target_block)
                return Status::unsupported("regular move replay witness target differs");
        const auto &update = witness.update;
        const u32 from_table = table_for(ancillary.descriptors, update.from_parent);
        const u32 to_table = table_for(ancillary.descriptors, update.to_parent);
        const u32 source_table = table_for(ancillary.descriptors, update.source);
        const usize from_home = home_for(witness, from_table), to_home = home_for(witness, to_table);
        const usize source_home = home_for(witness, source_table);
        if (from_home == capacity || to_home == capacity || source_home == capacity)
            return Status::unsupported("regular move replay missing witnessed inode table");
        const Ext4RegularMoveSource source{from_table, to_table, source_table, witness.targets[0], witness.targets[1],
            ancillary.superblock, ancillary.descriptors, scratch.before[from_home], scratch.before[to_home],
            scratch.before[source_home], scratch.before[0], scratch.before[1],
            ancillary.inode_bitmaps, ancillary.block_bitmaps, ancillary.orphan};
        if (auto status = Ext4RegularMovePlanner::plan(binding, update, source, scratch.canonical); !status)
            return status;
        if (scratch.canonical.count != count)
            return Status::unsupported("regular move replay canonical home count");
        Ext4RegularMoveReplayValidation result{};
        result.all_homes_after = true;
        for (usize index = 0; index < count; ++index)
        {
            if (scratch.canonical.targets[index] != witness.targets[index] ||
                scratch.canonical.before[index] != scratch.before[index] ||
                scratch.canonical.after[index] != scratch.after[index])
                return Status::unsupported("regular move replay image differs from canonical plan");
            result.home_after[index] = current_homes[index] == scratch.after[index];
            if (!result.home_after[index] && current_homes[index] != scratch.before[index])
                return Status::unsupported("regular move replay home differs from old or new image");
            result.all_homes_after &= result.home_after[index];
        }
        auto canonical_descriptor = descriptor;
        if (auto status = Ext4RegularMoveWitnessCodec::encode(update, scratch.canonical, canonical_descriptor); !status)
            return status;
        if (canonical_descriptor != descriptor)
            return Status::unsupported("regular move replay witness differs from canonical encoding");
        result.count = count; result.from_parent = update.from_parent; result.to_parent = update.to_parent;
        result.source = update.source; result.from_generation = update.from_generation;
        result.to_generation = update.to_generation; result.source_generation = update.source_generation;
        output = result;
        return {};
    }
};
} // namespace ok::fs
