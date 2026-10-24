#pragma once

#include "ok/fs/ext4_directory_create_plan.hpp"

namespace ok::fs
{
struct Ext4DirectoryCreateWitness
{
    Ext4CreateUpdate update{};
    u32 inode{}, physical{};
    usize count{}, runs{}, bytes{};
};

// Private reversible descriptor padding, protected by the JBD2 descriptor CRC.
// Exact preimages survive destruction of free slots, parent times and counters.
// These image bytes grant no device/session provenance or recovery authority.
class Ext4DirectoryCreateWitnessCodec final
{
    using Block = Ext4CreateBlock;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'D', 'R', 'W', '0', '0', '1'};
    static void store16(Block &block, usize at, u16 value)
    { block[at] = u8(value); block[at + 1] = u8(value >> 8); }
    static void store32(Block &block, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) block[at + i] = u8(value >> (8 * i)); }
  public:
    static constexpr usize begin = 160, patch_begin = 280, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize i = 0; i < magic.size(); ++i)
            if (descriptor[begin + i] != magic[i]) return false;
        return true;
    }
    // Leaves descriptor unchanged on any error; source images must not alias it.
    [[nodiscard]] static Status encode(const Ext4CreateUpdate &update,
                                      const Ext4CreatePlan &plan, Block &descriptor)
    {
        if ((plan.count != 7 && plan.count != 8) || update.parent != plan.parent ||
            update.attributes.type != NodeType::directory || update.name.empty() || update.name.size() > 31)
            return Status::invalid_argument("mkdir witness plan or name");
        Block proposed = descriptor;
        const usize tag_end = 44 + (plan.count - 1) * 16;
        for (usize at = tag_end; at < begin; ++at) proposed[at] = 0;
        for (usize at = begin; at < end; ++at) proposed[at] = 0;
        for (usize i = 0; i < magic.size(); ++i) proposed[begin + i] = magic[i];
        store16(proposed, 168, 1);
        proposed[172] = u8(plan.count); proposed[173] = u8(update.name.size());
        store32(proposed, 176, update.parent); store32(proposed, 180, plan.inode);
        store32(proposed, 184, update.seconds); store32(proposed, 188, update.nanoseconds);
        store32(proposed, 192, update.attributes.mode); store32(proposed, 196, update.attributes.uid);
        store32(proposed, 200, update.attributes.gid);
        const u32 physical = plan.targets[plan.count - 2];
        store32(proposed, 272, physical);
        for (usize i = 0; i < update.name.size(); ++i) proposed[240 + i] = u8(update.name[i]);
        usize cursor = patch_begin, runs = 0;
        for (usize home = 0; home < plan.count; ++home)
        {
            store32(proposed, 204 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            for (usize at = 0; at < 1024;)
            {
                if (plan.before[home][at] == plan.after[home][at]) { ++at; continue; }
                const usize first = at;
                while (at < 1024 && plan.before[home][at] != plan.after[home][at]) ++at;
                const usize length = at - first;
                if (cursor > end || end - cursor < 5 || length > end - cursor - 5)
                    return Status::unsupported("mkdir complete before witness exceeds descriptor capacity");
                proposed[cursor] = u8(home);
                store16(proposed, cursor + 1, u16(first)); store16(proposed, cursor + 3, u16(length));
                cursor += 5;
                for (usize i = 0; i < length; ++i) proposed[cursor++] = plan.before[home][first + i];
                ++runs;
            }
        }
        store16(proposed, 170, u16(cursor)); store16(proposed, 174, u16(runs));
        descriptor = proposed;
        return {};
    }
    // Output name borrows descriptor storage. All input/output images must be
    // disjoint and descriptor must remain immutable while the name is used.
    [[nodiscard]] static Status decode(const Block &descriptor, std::span<const Block> after,
                                      std::span<Block> before, Ext4DirectoryCreateWitness &output)
    {
        using namespace ext4_checked;
        output = {};
        if (!present(descriptor) || le16(descriptor, 168) != 1)
            return Status::unsupported("mkdir durable witness absent or version unknown");
        const usize count = descriptor[172], length = descriptor[173];
        const usize limit = le16(descriptor, 170), runs = le16(descriptor, 174);
        if ((count != 7 && count != 8) || after.size() != count || before.size() != count ||
            !length || length > 31 || limit < patch_begin || limit > end)
            return Status::invalid_argument("mkdir witness header geometry");
        for (usize at = 44 + (count - 1) * 16; at < begin; ++at)
            if (descriptor[at]) return Status::io_error("mkdir witness tag padding");
        for (usize at = limit; at < end; ++at)
            if (descriptor[at]) return Status::io_error("mkdir witness trailing bytes");
        for (usize at = 204 + count * 4; at < 240; ++at)
            if (descriptor[at]) return Status::io_error("mkdir witness unused preimage CRC");
        for (usize at = 240 + length; at < 272; ++at)
            if (descriptor[at]) return Status::io_error("mkdir witness name padding");
        for (usize at = 276; at < patch_begin; ++at)
            if (descriptor[at]) return Status::io_error("mkdir witness reserved bytes");
        for (usize home = 0; home < count; ++home) before[home] = after[home];
        usize cursor = patch_begin, prior_home = 0, prior_end = 0;
        for (usize run = 0; run < runs; ++run)
        {
            if (cursor > limit || limit - cursor < 5)
                return Status::io_error("mkdir witness truncated patch");
            const usize home = descriptor[cursor], offset = le16(descriptor, cursor + 1);
            const usize size = le16(descriptor, cursor + 3);
            cursor += 5;
            if (home >= count || offset >= 1024 || !size || size > 1024 - offset ||
                size > limit - cursor ||
                (run && (home < prior_home || (home == prior_home && offset <= prior_end))))
                return Status::io_error("mkdir witness patch bounds or canonical order");
            for (usize i = 0; i < size; ++i)
            {
                if (descriptor[cursor + i] == after[home][offset + i])
                    return Status::io_error("mkdir witness patch includes unchanged byte");
                before[home][offset + i] = descriptor[cursor + i];
            }
            cursor += size; prior_home = home; prior_end = offset + size;
        }
        if (cursor != limit) return Status::io_error("mkdir witness patch length mismatch");
        for (usize home = 0; home < count; ++home)
            if (le32(descriptor, 204 + home * 4) != jbd2::detail::crc32c(~u32{0}, before[home]))
                return Status::io_error("mkdir witnessed preimage checksum");
        output.update = {le32(descriptor, 176), le32(descriptor, 184), le32(descriptor, 188),
            {reinterpret_cast<const char *>(descriptor.data() + 240), length},
            {NodeType::directory, le32(descriptor, 192), le32(descriptor, 196), le32(descriptor, 200)}};
        output.inode = le32(descriptor, 180); output.physical = le32(descriptor, 272); output.count = count;
        output.runs = runs; output.bytes = limit - begin;
        return {};
    }
};
} // namespace ok::fs
