#pragma once

#include "ok/fs/ext4_namespace_journal.hpp"

namespace ok::fs
{
struct Ext4NamespaceWitness
{
    Ext4NamespaceUpdate update{};
    usize count{}, runs{}, bytes{};
};

// Private reversible descriptor extension. Neither decoded bytes nor a CRC
// attest device/session provenance or authorize recovery I/O.
class Ext4NamespaceWitnessCodec final
{
    using Block = Ext4NamespaceBlock;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'N', 'S', 'W', '0', '0', '1'};
    static void store16(Block &block, usize at, u16 value)
    { block[at] = u8(value); block[at + 1] = u8(value >> 8); }
    static void store32(Block &block, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) block[at + i] = u8(value >> (8 * i)); }
  public:
    static constexpr usize begin = 128, patch_begin = 264, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize i = 0; i < magic.size(); ++i)
            if (descriptor[begin + i] != magic[i]) return false;
        return true;
    }
    // Leaves descriptor unchanged if the full witness does not fit.
    [[nodiscard]] static Status encode(const Ext4NamespaceUpdate &update,
        const Ext4NamespacePlan &plan, Block &descriptor)
    {
        if (plan.no_op || plan.count < 2 || plan.count > 5 ||
            update.source_name.empty() || update.source_name.size() > 31 ||
            update.destination_name.size() > 31 ||
            (update.operation != Ext4NamespaceOperation::replace &&
             update.operation != Ext4NamespaceOperation::unlink))
            return Status::invalid_argument("namespace witness plan or names");
        Block proposed = descriptor;
        for (usize at = begin; at < end; ++at) proposed[at] = 0;
        for (usize i = 0; i < magic.size(); ++i) proposed[begin + i] = magic[i];
        store16(proposed, 136, 1);
        proposed[140] = u8(plan.count); proposed[141] = u8(update.operation);
        store32(proposed, 144, update.parent); store32(proposed, 148, update.source);
        store32(proposed, 152, update.destination);
        store32(proposed, 156, update.parent_generation);
        store32(proposed, 160, update.source_generation);
        store32(proposed, 164, update.destination_generation);
        store32(proposed, 168, update.seconds); store32(proposed, 172, update.nanoseconds);
        proposed[176] = u8(update.source_name.size());
        proposed[177] = u8(update.destination_name.size());
        for (usize i = 0; i < update.source_name.size(); ++i)
            proposed[200 + i] = u8(update.source_name[i]);
        for (usize i = 0; i < update.destination_name.size(); ++i)
            proposed[232 + i] = u8(update.destination_name[i]);
        usize cursor = patch_begin, runs = 0;
        for (usize home = 0; home < plan.count; ++home)
        {
            store32(proposed, 180 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            for (usize at = 0; at < 1024;)
            {
                if (plan.before[home][at] == plan.after[home][at]) { ++at; continue; }
                const usize first = at;
                while (at < 1024 && plan.before[home][at] != plan.after[home][at]) ++at;
                const usize length = at - first;
                if (cursor > end || end - cursor < 5 || length > end - cursor - 5)
                    return Status::unsupported("namespace complete before witness exceeds descriptor capacity");
                proposed[cursor] = u8(home);
                store16(proposed, cursor + 1, u16(first));
                store16(proposed, cursor + 3, u16(length));
                cursor += 5;
                for (usize i = 0; i < length; ++i) proposed[cursor++] = plan.before[home][first + i];
                ++runs;
            }
        }
        store16(proposed, 138, u16(cursor)); store16(proposed, 142, u16(runs));
        descriptor = proposed;
        return {};
    }
    // Names borrow descriptor storage. before, after and descriptor must not
    // alias; the caller must keep descriptor immutable while using the result.
    [[nodiscard]] static Status decode(const Block &descriptor,
        std::span<const Block> after, std::span<Block> before, Ext4NamespaceWitness &output)
    {
        using namespace ext4_checked;
        output = {};
        if (!present(descriptor) || le16(descriptor, 136) != 1)
            return Status::unsupported("namespace durable witness absent or version unknown");
        const usize count = descriptor[140], limit = le16(descriptor, 138), runs = le16(descriptor, 142);
        if (count < 2 || count > 5 || after.size() != count || before.size() != count ||
            limit < patch_begin || limit > end || descriptor[141] > u8(Ext4NamespaceOperation::unlink) ||
            !descriptor[176] || descriptor[176] > 31 || descriptor[177] > 31 ||
            descriptor[178] || descriptor[179])
            return Status::invalid_argument("namespace witness header geometry");
        for (usize at = limit; at < end; ++at)
            if (descriptor[at]) return Status::io_error("namespace witness trailing bytes");
        for (usize at = 180 + count * 4; at < 200; ++at)
            if (descriptor[at]) return Status::io_error("namespace witness unused preimage CRC");
        for (usize at = 200 + descriptor[176]; at < 232; ++at)
            if (descriptor[at]) return Status::io_error("namespace witness source name padding");
        for (usize at = 232 + descriptor[177]; at < patch_begin; ++at)
            if (descriptor[at]) return Status::io_error("namespace witness destination name padding");
        for (usize home = 0; home < count; ++home) before[home] = after[home];
        usize cursor = patch_begin, prior_home = 0, prior_end = 0;
        for (usize run = 0; run < runs; ++run)
        {
            if (cursor > limit || limit - cursor < 5)
                return Status::io_error("namespace witness truncated patch");
            const usize home = descriptor[cursor], offset = le16(descriptor, cursor + 1);
            const usize length = le16(descriptor, cursor + 3);
            cursor += 5;
            if (home >= count || offset >= 1024 || !length || length > 1024 - offset ||
                length > limit - cursor ||
                (run && (home < prior_home || (home == prior_home && offset <= prior_end))))
                return Status::io_error("namespace witness patch bounds or canonical order");
            for (usize i = 0; i < length; ++i)
            {
                if (descriptor[cursor + i] == after[home][offset + i])
                    return Status::io_error("namespace witness patch includes unchanged byte");
                before[home][offset + i] = descriptor[cursor + i];
            }
            cursor += length; prior_home = home; prior_end = offset + length;
        }
        if (cursor != limit) return Status::io_error("namespace witness patch length mismatch");
        for (usize home = 0; home < count; ++home)
            if (le32(descriptor, 180 + home * 4) != jbd2::detail::crc32c(~u32{0}, before[home]))
                return Status::io_error("namespace witnessed preimage checksum");
        output.update = {Ext4NamespaceOperation(descriptor[141]), le32(descriptor, 144),
            le32(descriptor, 148), le32(descriptor, 152), le32(descriptor, 156),
            le32(descriptor, 160), le32(descriptor, 164),
            {reinterpret_cast<const char *>(descriptor.data() + 200), descriptor[176]},
            {reinterpret_cast<const char *>(descriptor.data() + 232), descriptor[177]},
            le32(descriptor, 168), le32(descriptor, 172)};
        output.count = count; output.runs = runs; output.bytes = limit - begin;
        return {};
    }
};

// The witness is flushed with the existing descriptor before commit. This
// class prepares bytes only; legacy witness-free planning remains separate.
class Ext4NamespaceDurableJournalPlanner final
{
  public:
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding,
        const Ext4NamespaceUpdate &update, const Ext4NamespaceSource &source,
        const Ext4NamespaceBlock &old_head, Ext4NamespaceJournalScratch &scratch,
        Ext4NamespaceJournalPlan &output)
    {
        if (auto status = Ext4NamespaceJournalPlanner::plan(binding, update, source, old_head, scratch, output);
            !status) return status;
        if (output.no_op) return {};
        if (auto status = Ext4NamespaceWitnessCodec::encode(update, scratch.namespace_plan, output.descriptor);
            !status) { output = {}; return status; }
        for (usize at = 1020; at < 1024; ++at) output.descriptor[at] = 0;
        const u32 crc = jbd2::detail::crc32c(jbd2::detail::crc32c(~u32{0}, binding.uuid), output.descriptor);
        for (usize i = 0; i < 4; ++i) output.descriptor[1020 + i] = u8(crc >> (24 - i * 8));
        return {};
    }
};
} // namespace ok::fs
