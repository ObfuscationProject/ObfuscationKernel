#pragma once

#include "ok/fs/ext4_regular_move_witness.hpp"

namespace ok::fs
{
struct Ext4RegularMoveJournalScratch
{
    Ext4RegularMovePlan regular_move_plan{};
    std::array<jbd2::ReplayAction, 5> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    Ext4RegularMoveBlock record{}, payload{};
    std::array<Ext4RegularMoveBlock, Ext4RegularMovePlan::capacity> witness_before{};
    Ext4RegularMoveWitness decoded_witness{};
};
struct Ext4RegularMoveJournalPlan
{
    Ext4RegularMoveBlock journal_head{}, descriptor{}, commit{}, end{};
    std::array<Ext4RegularMoveBlock, 5> payloads{};
    std::array<u32, 5> targets{};
    usize count{};
    u32 sequence{};
    bool ready{};
};

// Immutable format preparation only. Scratch/output must be disjoint from
// source images and each other. Structural JBD2 admission and witness roundtrip
// are construction checks; no strict replay, I/O, sealing or mount authority.
class Ext4RegularMoveJournalPlanner final
{
    using Block = Ext4RegularMoveBlock;
    static void store(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    static void header(Block &bytes, jbd2::RecordType type, u32 sequence)
    { store(bytes, 0, jbd2::magic); store(bytes, 4, u32(type)); store(bytes, 8, sequence); }
    struct Records
    {
        const Ext4RegularMoveJournalPlan &plan;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024) return Status::invalid_argument("regular move journal model read size");
            const Block *record = nullptr;
            if (number == 0) record = &plan.journal_head;
            else if (number == 1) record = &plan.descriptor;
            else if (number >= 2 && number < plan.count + 2) record = &plan.payloads[number - 2];
            else if (number == plan.count + 2) record = &plan.commit;
            else if (number == plan.count + 3) record = &plan.end;
            if (!record) return Status::io_error("regular move journal model read bounds");
            for (usize index = 0; index < bytes.size(); ++index) bytes[index] = (*record)[index];
            return {};
        }
    };
  public:
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding,
        const Ext4RegularMoveUpdate &update, const Ext4RegularMoveSource &source,
        const Block &old_journal_head, Ext4RegularMoveJournalScratch &scratch,
        Ext4RegularMoveJournalPlan &output)
    {
        // Both resets must preserve every input, including borrowed names.
        const auto overlap = [](const void *one, usize a, const void *two, usize b) {
            if (!a || !b) return false;
            const usize x = reinterpret_cast<usize>(one), y = reinterpret_cast<usize>(two);
            return (x >= y && x - y < b) || (y >= x && y - x < a);
        };
        if (overlap(&scratch, sizeof(scratch), &output, sizeof(output)))
            return Status::invalid_argument("regular move journal scratch/output overlap");
        const auto aliases = [&](const void *data, usize bytes) {
            return overlap(&scratch, sizeof(scratch), data, bytes) ||
                overlap(&output, sizeof(output), data, bytes);
        };
        if (aliases(&binding, sizeof(binding)) || aliases(&update, sizeof(update)) ||
            aliases(&source, sizeof(source)) || aliases(&old_journal_head, sizeof(Block)) ||
            aliases(update.old_name.data(), update.old_name.size()) ||
            aliases(update.new_name.data(), update.new_name.size()) ||
            aliases(&source.superblock, sizeof(Block)) || aliases(&source.descriptors, sizeof(Block)) ||
            aliases(&source.from_parent_table, sizeof(Block)) || aliases(&source.to_parent_table, sizeof(Block)) ||
            aliases(&source.source_table, sizeof(Block)) || aliases(&source.from_directory, sizeof(Block)) ||
            aliases(&source.to_directory, sizeof(Block)) || aliases(&source.inode_bitmaps, sizeof(source.inode_bitmaps)) ||
            aliases(&source.block_bitmaps, sizeof(source.block_bitmaps)) || aliases(&source.orphan, sizeof(source.orphan)))
            return Status::invalid_argument("regular move journal output aliases input");
        output = {}; scratch = {};
        const auto reject = [&](Status status) { output = {}; return status; };
        if (!binding.needs_recovery || !binding.empty_log)
            return Status::unsupported("regular move journal requires mounted dirty empty log");
        const auto codec = jbd2::Codec::open(old_journal_head, u32(binding.layout.journal_blocks),
            binding.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &journal = codec.value().superblock();
        if (journal.block_size != 1024 || journal.max_blocks != binding.layout.journal_blocks ||
            journal.first_block != 1 || journal.start_block ||
            journal.sequence != binding.journal_sequence || journal.sequence > 0xfffffffcU ||
            (journal.features != jbd2::feature_checksum_v3 &&
             journal.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)))
            return Status::unsupported("regular move journal ring, sequence or feature profile");
        auto &canonical = scratch.regular_move_plan;
        if (const auto status = Ext4RegularMovePlanner::plan(binding, update, source, canonical); !status)
            return status;
        if (canonical.count < 3 || canonical.count > output.targets.size() ||
            journal.max_blocks <= canonical.count + 3)
            return Status::unsupported("regular move journal record capacity");
        output.count = canonical.count; output.sequence = journal.sequence + 1;
        output.journal_head = old_journal_head;
        header(output.descriptor, jbd2::RecordType::descriptor, output.sequence);
        header(output.commit, jbd2::RecordType::commit, output.sequence);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const std::array<u8, 4> sequence{u8(output.sequence >> 24), u8(output.sequence >> 16),
                                      u8(output.sequence >> 8), u8(output.sequence)};
        for (usize index = 0; index < output.count; ++index)
        {
            output.targets[index] = canonical.targets[index];
            auto &payload = output.payloads[index];
            payload = canonical.after[index];
            u32 flags = index ? jbd2::flag_same_uuid : 0;
            if (index + 1 == output.count) flags |= jbd2::flag_last_tag;
            if (jbd2::detail::be32(payload, 0) == jbd2::magic)
            { store(payload, 0, 0); flags |= jbd2::flag_escape; }
            const usize at = index == 0 ? 12 : 44 + (index - 1) * 16;
            store(output.descriptor, at, output.targets[index]);
            store(output.descriptor, at + 4, flags);
            store(output.descriptor, at + 12,
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), payload));
        }
        // Only OKMVW001 supplies complete inverse preimages for this family.
        // The marker and every patch byte participate in the descriptor CRC.
        if (const auto status = Ext4RegularMoveWitnessCodec::encode(update, canonical, output.descriptor); !status)
            return reject(status);
        if (const auto status = Ext4RegularMoveWitnessCodec::decode(output.descriptor,
                std::span<const Block>{canonical.after}.first(canonical.count),
                std::span<Block>{scratch.witness_before}.first(canonical.count), scratch.decoded_witness); !status)
            return reject(status);
        if (scratch.decoded_witness.count != canonical.count ||
            scratch.decoded_witness.targets != canonical.targets)
            return reject(Status::fault("regular move journal generated witness identity"));
        for (usize index = 0; index < canonical.count; ++index)
            if (scratch.witness_before[index] != canonical.before[index])
                return reject(Status::fault("regular move journal generated witness preimage"));
        store(output.descriptor, 1020, jbd2::detail::crc32c(seed, output.descriptor));
        store(output.commit, 52, update.seconds); store(output.commit, 56, update.nanoseconds);
        store(output.commit, 16, jbd2::detail::crc32c(seed, output.commit));
        store(output.journal_head, 24, output.sequence); store(output.journal_head, 28, 1);
        store(output.journal_head, 252, 0);
        store(output.journal_head, 252, jbd2::detail::crc32c(~u32{0}, output.journal_head));
        const auto new_codec = jbd2::Codec::open(output.journal_head, u32(binding.layout.journal_blocks),
            binding.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!new_codec) return reject(new_codec.status());
        std::array<jbd2::BlockRange, 5> allowed{};
        for (usize index = 0; index < output.count; ++index) allowed[index] = {output.targets[index], 1};
        Records records{output};
        const auto parsed = jbd2::build_replay_plan(new_codec.value(), records,
            {binding.layout.filesystem_blocks, std::span<const jbd2::BlockRange>{allowed}.first(output.count)},
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return reject(parsed.status());
        const auto &replay = parsed.value();
        if (replay.actions.size() != output.count || replay.committed_transactions != 1 ||
            replay.revoked_actions || replay.revoke_records || replay.incomplete_tail || replay.empty_log ||
            replay.next_sequence != output.sequence + 1)
            return reject(Status::fault("regular move journal generated record shape"));
        for (usize index = 0; index < replay.actions.size(); ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block != output.targets[index] || action.journal_block != index + 2 ||
                action.sequence != output.sequence || action.transaction_order)
                return reject(Status::fault("regular move journal generated target identity"));
            const auto &payload = output.payloads[index];
            for (usize byte = 0; byte < payload.size(); ++byte)
            {
                const u8 restored = action.restore_magic && byte < 4 ? u8(jbd2::magic >> (24 - byte * 8)) : payload[byte];
                if (restored != canonical.after[index][byte])
                    return reject(Status::fault("regular move journal generated payload differs"));
            }
        }
        output.ready = true;
        return {};
    }
};
} // namespace ok::fs
