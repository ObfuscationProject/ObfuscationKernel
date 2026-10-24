#pragma once

#include "ok/fs/ext4_allocated_empty_data_journal_verify.hpp"

namespace ok::fs
{
struct Ext4AllocatedEmptyDataJournalFormat
{
    Ext4FirstDataBlock head{}, descriptor{}, commit{}, end{}, empty_head{};
    std::array<Ext4FirstDataBlock, 4> payloads{};
    u32 active_sequence{}, next_sequence{};
    bool ready{};
};
struct Ext4AllocatedEmptyDataJournalFormatScratch
{
    Ext4AllocatedEmptyDataJournalScratch verification{};
    Ext4AllocatedEmptyDataJournalResult result{};
    Ext4RecoveryInfo active{};
};

// Pure formatting and complete independent regeneration. Public images do not
// admit publication, replay, lifetime transitions, WRITE or FLUSH. A future
// canonical owner must reserve and retain its exact transport before dispatch.
class Ext4AllocatedEmptyDataJournalFormatter final
{
    using Block = Ext4FirstDataBlock;
    static void be32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    static void header(Block &bytes, jbd2::RecordType type, u32 sequence)
    { be32(bytes, 0, jbd2::magic); be32(bytes, 4, u32(type)); be32(bytes, 8, sequence); }
    [[nodiscard]] static bool overlaps(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
    template <typename A, typename B>
    [[nodiscard]] static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
    [[nodiscard]] static bool same_plan(const Ext4AllocatedEmptyDataPlan &a, const Ext4AllocatedEmptyDataPlan &b)
    {
        return a.update.inode == b.update.inode && a.update.generation == b.update.generation &&
            a.update.size == b.update.size && a.update.seconds == b.update.seconds &&
            a.update.nanoseconds == b.update.nanoseconds && a.group == b.group && a.physical == b.physical &&
            a.old_version == b.old_version && a.new_version == b.new_version && a.targets == b.targets &&
            a.before == b.before && a.after == b.after && a.count == b.count;
    }
  public:
    [[nodiscard]] static Status format(const Ext4RecoveryInfo &binding,
        const Ext4AllocatedEmptyDataPlan &plan, const Block &old_head, const Block &inode_bitmap,
        Ext4AllocatedEmptyDataJournalFormatScratch &scratch, Ext4AllocatedEmptyDataJournalFormat &output)
    {
        if (overlap(scratch, output) || overlap(scratch, binding) || overlap(scratch, plan) ||
            overlap(scratch, old_head) || overlap(scratch, inode_bitmap) || overlap(output, binding) ||
            overlap(output, plan) || overlap(output, old_head) || overlap(output, inode_bitmap))
            return Status::invalid_argument("allocated empty journal format workspace overlap");
        scratch = {}; output = {};
        const auto reject = [&](Status status) { output = {}; return status; };
        if (!binding.needs_recovery || !binding.empty_log || binding.layout.block_size != 1024 ||
            binding.layout.filesystem_blocks > 0xffffffffULL || binding.layout.journal_blocks < 1024 ||
            !binding.structural_metadata().valid() || plan.count != 4 || !plan.update.size || plan.update.size > 1024)
            return Status::unsupported("allocated empty journal format bounded dirty empty profile");
        const auto codec = jbd2::Codec::open(old_head, u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &head = codec.value().superblock();
        if (head.block_size != 1024 || head.first_block != 1 || head.start_block || head.max_blocks <= 7 ||
            head.sequence != binding.journal_sequence || !head.sequence || head.sequence > 0xfffffffcU ||
            (head.features != jbd2::feature_checksum_v3 &&
             head.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)))
            return Status::unsupported("allocated empty journal format fixed ring or sequence capacity");
        output.active_sequence = head.sequence + 1; output.next_sequence = output.active_sequence + 1;
        header(output.descriptor, jbd2::RecordType::descriptor, output.active_sequence);
        header(output.commit, jbd2::RecordType::commit, output.active_sequence);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const auto sequence = std::span<const u8>{output.descriptor}.subspan(8, 4);
        for (usize home = 0; home < 4; ++home)
        {
            output.payloads[home] = plan.after[home];
            u32 flags = (home ? jbd2::flag_same_uuid : 0) | (home == 3 ? jbd2::flag_last_tag : 0);
            if (jbd2::detail::be32(output.payloads[home], 0) == jbd2::magic)
            { be32(output.payloads[home], 0, 0); flags |= jbd2::flag_escape; }
            const usize at = home ? 44 + (home - 1) * 16 : 12;
            be32(output.descriptor, at, plan.targets[home]); be32(output.descriptor, at + 4, flags);
            be32(output.descriptor, at + 12,
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), output.payloads[home]));
        }
        if (auto status = Ext4AllocatedEmptyDataWitnessCodec::encode(binding, plan, inode_bitmap, output.active_sequence,
                scratch.verification.witness, output.descriptor); !status) return reject(status);
        be32(output.descriptor, 1020, jbd2::detail::crc32c(seed, output.descriptor));
        // The witness carries the exact checked update regenerated from the old inode.
        be32(output.commit, 52, ext4_checked::le32(output.descriptor, 184));
        be32(output.commit, 56, ext4_checked::le32(output.descriptor, 188));
        be32(output.commit, 16, jbd2::detail::crc32c(seed, output.commit));
        output.head = old_head; be32(output.head, 24, output.active_sequence); be32(output.head, 28, 1);
        be32(output.head, 252, 0); be32(output.head, 252, jbd2::detail::crc32c(~u32{0}, output.head));
        output.empty_head = old_head; be32(output.empty_head, 24, output.next_sequence); be32(output.empty_head, 28, 0);
        be32(output.empty_head, 252, 0); be32(output.empty_head, 252, jbd2::detail::crc32c(~u32{0}, output.empty_head));
        scratch.active = binding; scratch.active.empty_log = false; scratch.active.journal_sequence = output.active_sequence;
        const Ext4AllocatedEmptyDataJournalRecords records{output.head, output.descriptor, output.commit, output.end, output.payloads};
        if (auto status = Ext4AllocatedEmptyDataJournalVerifier::verify(scratch.active, records, inode_bitmap,
                plan.before, scratch.verification, scratch.result); !status) return reject(status);
        if (!scratch.result.complete || scratch.result.after_mask || !same_plan(scratch.verification.decoded, plan) ||
            scratch.result.active_sequence != output.active_sequence || scratch.result.next_sequence != output.next_sequence)
            return reject(Status::fault("allocated empty generated journal roundtrip differs"));
        const auto empty = jbd2::Codec::open(output.empty_head, u32(binding.layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!empty) return reject(empty.status());
        if (empty.value().superblock().start_block || empty.value().superblock().sequence != output.next_sequence)
            return reject(Status::fault("allocated empty generated empty tail differs"));
        output.ready = true; return {};
    }
};
} // namespace ok::fs
