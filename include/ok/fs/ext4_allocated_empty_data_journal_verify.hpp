#pragma once

#include "ok/fs/ext4_allocated_empty_data_witness.hpp"

namespace ok::fs
{
struct Ext4AllocatedEmptyDataJournalRecords
{
    const Ext4FirstDataBlock &head, &descriptor, &commit, &end;
    std::span<const Ext4FirstDataBlock> payloads{};
};
struct Ext4AllocatedEmptyDataJournalScratch
{
    Ext4AllocatedEmptyDataWitnessScratch witness{};
    Ext4AllocatedEmptyDataPlan decoded{};
    std::array<Ext4FirstDataBlock, 4> after{};
    std::array<jbd2::ReplayAction, 4> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    Ext4FirstDataBlock record{}, payload{};
};
struct Ext4AllocatedEmptyDataJournalResult
{
    Ext4AllocatedEmptyDataWitness witness{};
    u32 active_sequence{}, next_sequence{};
    u8 after_mask{};
    bool complete{};
};

// Complete bounded journal and exact participant-image facts. These borrowing
// interfaces never authenticate a device snapshot or grant write authority.
class Ext4AllocatedEmptyDataJournalVerifier final
{
    template <jbd2::BlockReader> friend class Ext4AllocatedEmptyDataPreview;
    using Block = Ext4FirstDataBlock;
    struct Reader
    {
        const Ext4AllocatedEmptyDataJournalRecords &records;
        Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024) return Status::invalid_argument("allocated empty journal read size");
            const Block *image{};
            if (!number) image = &records.head;
            else if (number == 1) image = &records.descriptor;
            else if (number >= 2 && number < 6) image = &records.payloads[number - 2];
            else if (number == 6) image = &records.commit;
            else if (number == 7) image = &records.end;
            if (!image) return Status::io_error("allocated empty journal fixed record bounds");
            for (usize byte = 0; byte < 1024; ++byte) bytes[byte] = (*image)[byte];
            return {};
        }
    };
    static void be32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    [[nodiscard]] static bool overlaps(const void *a, usize a_size, const void *b, usize b_size)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < a_size : one - two < b_size;
    }
    template <typename A, typename B>
    [[nodiscard]] static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
    [[nodiscard]] static Status parse(const Ext4RecoveryInfo &binding,
        const Ext4AllocatedEmptyDataJournalRecords &records, Ext4AllocatedEmptyDataJournalScratch &scratch)
    {
        if (!binding.needs_recovery || binding.empty_log || records.payloads.size() != 4 ||
            binding.layout.block_size != 1024 ||
            !binding.structural_metadata().valid())
            return Status::invalid_argument("allocated empty active journal profile");
        if (!jbd2::detail::all_zero(records.end))
            return Status::io_error("allocated empty explicit end record is not zero");
        const auto opened = jbd2::Codec::open(records.head, u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!opened) return opened.status();
        const auto &head = opened.value().superblock();
        if (head.block_size != 1024 || head.first_block != 1 || head.start_block != 1 ||
            head.max_blocks != binding.layout.journal_blocks || head.max_blocks <= 7 ||
            (head.features != jbd2::feature_checksum_v3 &&
             head.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)) ||
            head.sequence < 2 || head.sequence > 0xfffffffdU || head.sequence != binding.journal_sequence)
            return Status::unsupported("allocated empty active sequence or fixed journal ring");
        Reader reader{records};
        const auto parsed = jbd2::build_replay_plan(opened.value(), reader, binding.structural_metadata(),
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return parsed.status();
        const auto &plan = parsed.value();
        if (plan.empty_log || plan.incomplete_tail || plan.committed_transactions != 1 ||
            plan.revoked_actions || plan.revoke_records || plan.actions.size() != 4 || plan.next_sequence != head.sequence + 1)
            return Status::io_error("allocated empty complete committed journal grammar");
        for (usize home = 0; home < 4; ++home)
        {
            const auto &action = plan.actions[home];
            if (action.sequence != head.sequence || action.transaction_order || action.journal_block != home + 2 ||
                action.target_block != ext4_checked::le32(records.descriptor, 192 + home * 4))
                return Status::io_error("allocated empty exact journal action identity");
            scratch.after[home] = records.payloads[home];
            if (action.restore_magic) be32(scratch.after[home], 0, jbd2::magic);
        }
        return {};
    }
  public:
    [[nodiscard]] static Status verify(const Ext4RecoveryInfo &binding,
        const Ext4AllocatedEmptyDataJournalRecords &records, const Block &inode_bitmap,
        std::span<const Block> actual_homes, Ext4AllocatedEmptyDataJournalScratch &scratch,
        Ext4AllocatedEmptyDataJournalResult &output)
    {
        if (overlap(scratch, output) || overlap(scratch, binding) || overlap(output, binding) ||
            overlap(scratch, records) || overlap(output, records) || overlap(scratch, inode_bitmap) ||
            overlap(output, inode_bitmap))
            return Status::invalid_argument("allocated empty journal request/workspace overlap");
        for (const auto *block : {&records.head, &records.descriptor, &records.commit, &records.end})
            if (overlap(scratch, *block) || overlap(output, *block))
                return Status::invalid_argument("allocated empty journal record/workspace overlap");
        for (const auto images : {records.payloads, actual_homes})
            if (overlaps(&scratch, sizeof(scratch), images.data(), images.size_bytes()) ||
                overlaps(&output, sizeof(output), images.data(), images.size_bytes()))
                return Status::invalid_argument("allocated empty journal image/workspace overlap");
        scratch = {}; output = {};
        if (actual_homes.size() != 4)
            return Status::invalid_argument("allocated empty actual whole-home count");
        if (auto status = parse(binding, records, scratch); !status) return status;
        Ext4AllocatedEmptyDataWitness witness{};
        if (auto status = Ext4AllocatedEmptyDataWitnessCodec::decode(binding, records.descriptor, scratch.after,
                inode_bitmap, scratch.witness, scratch.decoded, witness); !status) return status;
        if (witness.sequence != binding.journal_sequence)
            return Status::io_error("allocated empty witness active sequence differs");
        // A committed timestamp must describe this exact transform. Require the
        // private synchronous commit profile, including every unused byte.
        scratch.record = {};
        be32(scratch.record, 0, jbd2::magic); be32(scratch.record, 4, u32(jbd2::RecordType::commit));
        be32(scratch.record, 8, witness.sequence);
        be32(scratch.record, 52, witness.update.seconds); be32(scratch.record, 56, witness.update.nanoseconds);
        be32(scratch.record, 16, jbd2::detail::crc32c(jbd2::detail::crc32c(~u32{0}, binding.uuid), scratch.record));
        if (scratch.record != records.commit)
            return Status::io_error("allocated empty exact commit image differs");
        u8 mask{};
        if (auto status = Ext4AllocatedEmptyDataHomeMatcher::classify(scratch.decoded,
                std::span<const Block, 4>{actual_homes.data(), 4}, mask); !status) return status;
        output.witness = witness; output.active_sequence = witness.sequence;
        output.next_sequence = witness.sequence + 1; output.after_mask = mask; output.complete = true;
        return {};
    }
};

} // namespace ok::fs
