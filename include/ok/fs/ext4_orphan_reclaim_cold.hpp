#pragma once

#include "ok/fs/ext4_orphan_reclaim_journal.hpp"
#include "ok/fs/ext4_orphan_directory_qualified_reclaim.hpp"
#include <new>

namespace ok::fs
{
struct Ext4OrphanReclaimColdRecords
{
    const Ext4OrphanBlock &head, &descriptor, &commit, &end;
    std::span<const Ext4OrphanBlock> payloads{};
};
// Complete, immutable supplied observations. This borrowing contract does not
// establish device provenance; no caller-supplied orphan chain is trusted.
struct Ext4OrphanReclaimColdObserved
{
    const Ext4OrphanBlock &superblock, &descriptors;
    const std::array<Ext4OrphanBlock, 4> &inode_bitmaps, &block_bitmaps;
    std::array<std::span<const Ext4OrphanBlock>, 4> inode_tables{};
    std::span<const Ext4OrphanDirectoryBlockImage> directories{};
};

// Large caller-owned storage, disjoint from all input and never on kernel stack.
struct Ext4OrphanReclaimColdScratch
{
    std::array<std::array<Ext4OrphanBlock, 512>, 4> tables{};
    Ext4OrphanBlock superblock{}, descriptors{}, normalized{}, old_head{}, record{}, payload{};
    std::array<Ext4OrphanBlock, 4> inode_bitmaps{}, block_bitmaps{};
    std::array<Ext4OrphanInodeImage, Ext4OrphanChainProof::capacity> chain{};
    std::array<Ext4OrphanBlock, 5> before{}, after{}, current{};
    std::array<jbd2::ReplayAction, 5> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    std::array<std::span<const Ext4OrphanBlock>, 4> table_spans{};
    Ext4OrphanDirectoryQualifiedReclaimScratch qualified{};
    Ext4OrphanDirectoryQualifiedReclaimPlan reclaim{};
    Ext4OrphanReclaimJournalScratch formatter{};
    Ext4OrphanReclaimJournalPlan canonical{};
    Ext4RecoveryBinding bootstrap{};
    usize chain_count{};
};

struct Ext4OrphanReclaimColdResult
{
    std::array<u32, 5> targets{};
    std::array<bool, 5> home_after{};
    u32 inode{}, old_generation{}, retained_generation{}, next_head{};
    u32 active_sequence{}, parsed_next_sequence{}, cleanup_sequence{};
    usize count{}, old_homes{}, new_homes{}, witness_runs{}, witness_bytes{};
    bool valid{}, all_before{}, all_after{}, complete_global_before_proof{};
};

// Pure semantic proof of a declared preimage and an allowed observed mixture.
// CRC witnesses do not authenticate overwritten history. Live owners must bind
// original publication, and cold execution must seal actual whole device bytes.
class Ext4OrphanReclaimColdVerifier final
{
    using Block = Ext4OrphanBlock;
    struct Reader
    {
        const Ext4OrphanReclaimColdRecords &records;
        Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024) return Status::invalid_argument("reclaim cold journal read size");
            const Block *image{};
            if (!number) image = &records.head;
            else if (number == 1) image = &records.descriptor;
            else if (number >= 2 && number < records.payloads.size() + 2) image = &records.payloads[number - 2];
            else if (number == records.payloads.size() + 2) image = &records.commit;
            else if (number == records.payloads.size() + 3) image = &records.end;
            if (!image) return Status::io_error("reclaim cold journal read bounds");
            for (usize at = 0; at < 1024; ++at) bytes[at] = (*image)[at];
            return {};
        }
    };
    // Strict ordinary bootstrap sees only declared-before images and a private
    // zero-head superblock. The actual orphan bytes remain separately proved.
    class BeforeDevice final : public driver::BlockDevice
    {
      public:
        BeforeDevice(const Ext4RecoveryInfo &binding, const Ext4OrphanReclaimColdScratch &scratch)
            : binding_(binding), scratch_(scratch) {}
        driver::BlockGeometry geometry() const override { return {65536, 512, false}; }
        Status read_blocks(u64 first, std::span<std::byte> out) override
        {
            if (first % 2 || out.size() != 1024 || first / 2 >= 32768)
                return Status::invalid_argument("reclaim cold before bootstrap bounds");
            const u32 physical = u32(first / 2);
            const Block *image{};
            if (physical == 1) image = &scratch_.normalized;
            else if (physical == 2) image = &scratch_.descriptors;
            else if (physical == binding_.layout.journal_first_block) image = &scratch_.old_head;
            else for (usize group = 0; group < 4; ++group)
            {
                const auto &metadata = binding_.metadata;
                if (physical == metadata[group * 4 + 1].first) image = &scratch_.block_bitmaps[group];
                if (physical == metadata[group * 4 + 2].first) image = &scratch_.inode_bitmaps[group];
                const auto table = metadata[group * 4 + 3];
                if (physical >= table.first && physical - table.first < scratch_.table_spans[group].size())
                    image = &scratch_.table_spans[group][physical - table.first];
            }
            if (!image) return Status::io_error("reclaim cold before bootstrap image absent");
            for (usize at = 0; at < 1024; ++at) out[at] = std::byte((*image)[at]);
            return {};
        }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("reclaim cold before bootstrap is readonly"); }
        Status flush() override { return Status::denied("reclaim cold before bootstrap is readonly"); }
      private:
        const Ext4RecoveryInfo &binding_;
        const Ext4OrphanReclaimColdScratch &scratch_;
    };
    static void little32(Block &block, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) block[at + index] = u8(value >> (8 * index)); }
    static void big32(Block &block, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) block[at + index] = u8(value >> (24 - 8 * index)); }
    [[nodiscard]] static Status chain(Ext4OrphanReclaimColdScratch &scratch)
    {
        u32 number = ext4_checked::le32(scratch.superblock, 232);
        scratch.chain_count = 0;
        while (number)
        {
            if (number < 11 || number > 8192 || scratch.chain_count == scratch.chain.size())
                return Status::unsupported("reclaim cold declared chain bounds");
            for (usize prior = 0; prior < scratch.chain_count; ++prior)
                if (scratch.chain[prior].number == number)
                    return Status::io_error("reclaim cold declared chain cycle");
            const u32 group = (number - 1) / 2048, local = (number - 1) % 2048;
            if (scratch.table_spans[group].size() != 512)
                return Status::invalid_argument("reclaim cold declared chain table absent");
            auto &image = scratch.chain[scratch.chain_count++];
            image.number = number; image.table_block = ext4_checked::le32(scratch.descriptors, group * 32 + 8) + local / 4;
            image.table = scratch.tables[group][local / 4];
            const auto inode = std::span<const u8>{image.table}.subspan((local % 4) * 256, 256);
            image.expected_generation = ext4_checked::le32(inode, 100);
            number = ext4_checked::le32(inode, 20);
        }
        return {};
    }
    [[nodiscard]] static const Block *observed_home(const Ext4RecoveryInfo &binding,
        const Ext4OrphanReclaimColdObserved &observed, u32 physical)
    {
        if (physical == 1) return &observed.superblock;
        if (physical == 2) return &observed.descriptors;
        for (usize group = 0; group < 4; ++group)
        {
            if (physical == binding.metadata[group * 4 + 1].first) return &observed.block_bitmaps[group];
            if (physical == binding.metadata[group * 4 + 2].first) return &observed.inode_bitmaps[group];
            const auto table = binding.metadata[group * 4 + 3];
            if (physical >= table.first && physical - table.first < observed.inode_tables[group].size())
                return &observed.inode_tables[group][physical - table.first];
        }
        return nullptr;
    }
    [[nodiscard]] static Block *before_home(const Ext4RecoveryInfo &binding,
        Ext4OrphanReclaimColdScratch &scratch, u32 physical)
    {
        if (physical == 1) return &scratch.superblock;
        if (physical == 2) return &scratch.descriptors;
        for (usize group = 0; group < 4; ++group)
        {
            if (physical == binding.metadata[group * 4 + 1].first) return &scratch.block_bitmaps[group];
            if (physical == binding.metadata[group * 4 + 2].first) return &scratch.inode_bitmaps[group];
            const auto table = binding.metadata[group * 4 + 3];
            if (physical >= table.first && physical - table.first < scratch.table_spans[group].size())
                return &scratch.tables[group][physical - table.first];
        }
        return nullptr;
    }

  public:
    [[nodiscard]] static Status verify(const Ext4RecoveryInfo &binding,
        const Ext4OrphanReclaimColdRecords &records, const Ext4OrphanReclaimColdObserved &observed,
        Ext4OrphanReclaimColdScratch &scratch, Ext4OrphanReclaimColdResult &output)
    {
        output = {};
        scratch.chain_count = 0; scratch.table_spans = {}; scratch.reclaim = {}; scratch.canonical = {};
        const usize count = records.payloads.size();
        const auto &layout = binding.layout;
        if (!binding.needs_recovery || binding.empty_log || count < 4 || count > 5 ||
            layout.first_sector != 0 || layout.block_size != 1024 || layout.filesystem_blocks != 32768 || binding.journal_inode != 8 ||
            binding.metadata_count != 16 || !binding.structural_metadata().valid() ||
            layout.journal_first_block < 3 || layout.journal_first_block >= 32768 ||
            layout.journal_blocks < 1024 || layout.journal_blocks > 32768 - layout.journal_first_block ||
            observed.directories.size() > Ext4OrphanDirectoryReferenceScratch::block_capacity)
            return Status::invalid_argument("reclaim cold input profile");
        for (usize group = 0; group < 4; ++group)
            if (!observed.inode_tables[group].empty() && observed.inode_tables[group].size() != 512)
                return Status::invalid_argument("reclaim cold incomplete observed table");
        const auto codec = jbd2::Codec::open(records.head, u32(layout.journal_blocks), binding.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &head = codec.value().superblock();
        if (head.block_size != 1024 || head.first_block != 1 || head.start_block != 1 ||
            head.sequence < 2 || head.sequence > 0xfffffffdU || head.sequence != binding.journal_sequence)
            return Status::unsupported("reclaim cold active journal sequence or ring");
        const u64 journal_end = layout.journal_first_block + layout.journal_blocks;
        const std::array<jbd2::BlockRange, 2> domain{{{0, layout.journal_first_block},
            {journal_end, layout.filesystem_blocks - journal_end}}};
        Reader reader{records};
        const auto parsed = jbd2::build_replay_plan(codec.value(), reader,
            {32768, std::span<const jbd2::BlockRange>{domain}.first(journal_end < 32768 ? 2 : 1)},
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return parsed.status();
        const auto &replay = parsed.value();
        if (replay.empty_log || replay.incomplete_tail || replay.committed_transactions != 1 || replay.revoked_actions || replay.revoke_records ||
            replay.actions.size() != count || replay.next_sequence != head.sequence + 1)
            return Status::io_error("reclaim cold journal grammar");
        for (usize index = 0; index < count; ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block > 0xffffffffULL || action.journal_block != index + 2 ||
                action.sequence != head.sequence || action.transaction_order)
                return Status::io_error("reclaim cold action identity");
            scratch.after[index] = records.payloads[index];
            if (action.restore_magic) big32(scratch.after[index], 0, jbd2::magic);
        }
        Ext4OrphanReclaimWitness witness{};
        if (auto status = Ext4OrphanReclaimWitnessCodec::decode(records.descriptor, binding.uuid,
                std::span<const Block>{scratch.after}.first(count), std::span<Block>{scratch.before}.first(count), witness); !status)
            return status;
        if (witness.count != count || witness.sequence != head.sequence)
            return Status::io_error("reclaim cold witnessed sequence or count");

        scratch.superblock = observed.superblock; scratch.descriptors = observed.descriptors;
        scratch.inode_bitmaps = observed.inode_bitmaps; scratch.block_bitmaps = observed.block_bitmaps;
        for (usize group = 0; group < 4; ++group)
            if (!observed.inode_tables[group].empty())
            {
                for (usize index = 0; index < 512; ++index) scratch.tables[group][index] = observed.inode_tables[group][index];
                scratch.table_spans[group] = scratch.tables[group];
            }
        Ext4OrphanReclaimColdResult result{};
        result.count = count;
        for (usize index = 0; index < count; ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block != witness.targets[index]) return Status::io_error("reclaim cold witnessed target differs");
            const auto *current = observed_home(binding, observed, witness.targets[index]);
            auto *old = before_home(binding, scratch, witness.targets[index]);
            if (!current || !old) return Status::io_error("reclaim cold observed home absent");
            scratch.current[index] = *current;
            if (*current == scratch.after[index]) { result.home_after[index] = true; ++result.new_homes; }
            else if (*current == scratch.before[index]) ++result.old_homes;
            else return Status::io_error("reclaim cold foreign whole home");
            result.targets[index] = witness.targets[index]; *old = scratch.before[index];
        }
        // A tombstone must be durable before either allocation bitmap may free
        // its identity. The superblock head advances only after allocation homes.
        for (usize index = 1; index < count - 1; ++index)
            if (result.home_after[index] && !result.home_after[0])
                return Status::io_error("reclaim cold allocation precedes tombstone barrier");
        if (result.home_after[count - 1])
            for (usize index = 0; index < count - 1; ++index)
                if (!result.home_after[index]) return Status::io_error("reclaim cold head precedes allocation barrier");

        Ext4RecoveryInfo old_binding = binding;
        old_binding.empty_log = true; old_binding.journal_sequence = head.sequence - 1;
        scratch.old_head = records.head;
        big32(scratch.old_head, 24, old_binding.journal_sequence); big32(scratch.old_head, 28, 0);
        big32(scratch.old_head, 252, 0);
        big32(scratch.old_head, 252, jbd2::detail::crc32c(~u32{0}, scratch.old_head));
        scratch.normalized = scratch.superblock;
        little32(scratch.normalized, 232, 0);
        little32(scratch.normalized, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{scratch.normalized}.first(1020)));
        scratch.bootstrap.~Ext4RecoveryBinding();
        ::new (static_cast<void *>(&scratch.bootstrap)) Ext4RecoveryBinding;
        BeforeDevice before_device(old_binding, scratch);
        if (auto status = scratch.bootstrap.inspect(before_device, {0, 65536}); !status) return status;
        const auto *strict = scratch.bootstrap.info();
        if (!strict || strict->uuid != old_binding.uuid || strict->journal_inode != 8 || !strict->needs_recovery || !strict->empty_log ||
            strict->journal_sequence != old_binding.journal_sequence || strict->metadata_count != 16 ||
            strict->layout.journal_first_block != layout.journal_first_block || strict->layout.journal_blocks != layout.journal_blocks)
            return Status::io_error("reclaim cold strict before binding differs");
        for (usize index = 0; index < 16; ++index)
            if (strict->metadata[index].first != old_binding.metadata[index].first ||
                strict->metadata[index].count != old_binding.metadata[index].count)
                return Status::io_error("reclaim cold strict before metadata differs");
        if (auto status = chain(scratch); !status) return status;
        const Ext4OrphanDirectoryReferenceSource old_source{{{scratch.superblock, scratch.descriptors,
            scratch.inode_bitmaps, scratch.block_bitmaps,
            std::span<const Ext4OrphanInodeImage>{scratch.chain}.first(scratch.chain_count)}, scratch.table_spans}, observed.directories};
        if (auto status = Ext4OrphanDirectoryQualifiedReclaimPlanner::plan(old_binding, old_source, scratch.qualified,
                witness.inode, witness.old_generation, scratch.reclaim); !status) return status;
        if (!scratch.reclaim.valid || scratch.reclaim.reclaim.count != count)
            return Status::fault("reclaim cold complete before plan absent");
        const auto &homes = scratch.reclaim.reclaim;
        if (auto status = Ext4OrphanReclaimJournalPlanner::plan(old_binding, homes, scratch.old_head,
                jbd2::detail::be32(records.commit, 52), jbd2::detail::be32(records.commit, 56), scratch.formatter, scratch.canonical); !status)
            return status;
        const auto &canonical = scratch.canonical;
        if (!canonical.ready || canonical.count != count || canonical.sequence != head.sequence ||
            canonical.journal_head != records.head || canonical.descriptor != records.descriptor ||
            canonical.commit != records.commit || canonical.end != records.end)
            return Status::io_error("reclaim cold canonical journal differs");
        for (usize index = 0; index < count; ++index)
            if (homes.targets[index] != witness.targets[index] || canonical.targets[index] != witness.targets[index] ||
                homes.before[index] != scratch.before[index] || homes.after[index] != scratch.after[index] ||
                canonical.payloads[index] != records.payloads[index])
                return Status::io_error("reclaim cold canonical whole images differ");
        if (homes.released_blocks != witness.released_blocks || homes.released_mapping != witness.released_mapping ||
            homes.retained_generation != witness.retained_generation || homes.next_head != witness.next_head)
            return Status::io_error("reclaim cold canonical released identity differs");
        result.inode = witness.inode; result.old_generation = witness.old_generation;
        result.retained_generation = witness.retained_generation; result.next_head = witness.next_head;
        result.active_sequence = head.sequence; result.parsed_next_sequence = replay.next_sequence;
        result.cleanup_sequence = replay.next_sequence + 1;
        result.witness_runs = witness.runs; result.witness_bytes = witness.bytes;
        result.all_before = result.old_homes == count; result.all_after = result.new_homes == count;
        result.complete_global_before_proof = true; result.valid = true;
        output = result;
        return {};
    }
};
} // namespace ok::fs
