#pragma once

#include "ok/fs/ext4_regular_move_plan.hpp"

namespace ok::fs
{
struct Ext4RegularMoveWitness
{
    Ext4RegularMoveUpdate update{};
    std::array<u32, Ext4RegularMovePlan::capacity> targets{};
    usize count{}, runs{}, bytes{};
};

// Reversible descriptor padding. The caller must validate the JBD2 descriptor
// CRC, recompute the planner and compare canonical images before recovery I/O.
// Neither this record nor a checksum grants device/session authority.
class Ext4RegularMoveWitnessCodec final
{
    using Block = Ext4RegularMoveBlock;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'M', 'V', 'W', '0', '0', '1'};

    static constexpr void store16(Block &block, usize at, u16 value)
    { block[at] = u8(value); block[at + 1] = u8(value >> 8); }
    static constexpr void store32(Block &block, usize at, u32 value)
    { for (usize byte = 0; byte < 4; ++byte) block[at + byte] = u8(value >> (8 * byte)); }
    [[nodiscard]] static bool name_valid(std::string_view name)
    {
        if (name.empty() || name.size() > 31 || name == "." || name == "..") return false;
        for (char byte : name) if (!byte || byte == '/') return false;
        return true;
    }
    [[nodiscard]] static bool update_valid(const Ext4RegularMoveUpdate &update)
    {
        return update.from_parent >= 2 && update.from_parent <= 8192 &&
            update.to_parent >= 2 && update.to_parent <= 8192 && update.source > 10 && update.source <= 8192 &&
            update.from_parent != update.to_parent && update.from_parent != update.source &&
            update.to_parent != update.source && update.seconds <= 0x7fffffffU &&
            update.nanoseconds < 1000000000U && name_valid(update.old_name) && name_valid(update.new_name);
    }
    [[nodiscard]] static bool overlap(const void *left, usize left_bytes, const void *right, usize right_bytes)
    {
        if (!left_bytes || !right_bytes) return false;
        const usize a = reinterpret_cast<usize>(left), b = reinterpret_cast<usize>(right);
        return (a >= b && a - b < right_bytes) || (b >= a && b - a < left_bytes);
    }
    [[nodiscard]] static bool zero(const Block &block, usize first, usize last)
    {
        for (usize at = first; at < last; ++at) if (block[at]) return false;
        return true;
    }

  public:
    static constexpr usize begin = 128, patch_begin = 320, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize at = 0; at < magic.size(); ++at)
            if (descriptor[begin + at] != magic[at]) return false;
        return true;
    }

    // Encoded names are copied. Every refusal preserves descriptor and inputs.
    [[nodiscard]] static Status encode(const Ext4RegularMoveUpdate &update,
                                       const Ext4RegularMovePlan &plan, Block &descriptor)
    {
        if (overlap(&descriptor, sizeof(descriptor), &plan, sizeof(plan)) ||
            overlap(&descriptor, sizeof(descriptor), &update, sizeof(update)) ||
            overlap(&descriptor, sizeof(descriptor), update.old_name.data(), update.old_name.size()) ||
            overlap(&descriptor, sizeof(descriptor), update.new_name.data(), update.new_name.size()))
            return Status::invalid_argument("regular move witness output aliases input");
        if (!update_valid(update) || plan.count < 3 || plan.count > Ext4RegularMovePlan::capacity ||
            update.from_parent != plan.from_parent || update.to_parent != plan.to_parent ||
            update.source != plan.source || update.source_generation != plan.source_generation ||
            !plan.allocation_unchanged || !plan.file_identity_retained || !plan.source_link_count_unchanged ||
            plan.old_name_length != update.old_name.size() || plan.new_name_length != update.new_name.size() ||
            std::string_view{plan.old_name.data(), plan.old_name_length} != update.old_name ||
            std::string_view{plan.new_name.data(), plan.new_name_length} != update.new_name)
            return Status::invalid_argument("regular move witness plan identity or names");
        for (usize home = 0; home < plan.count; ++home)
        {
            if (!plan.targets[home]) return Status::invalid_argument("regular move witness zero home");
            for (usize previous = 0; previous < home; ++previous)
                if (plan.targets[previous] == plan.targets[home])
                    return Status::invalid_argument("regular move witness duplicate home");
        }
        Block proposed = descriptor;
        for (usize at = 44 + (plan.count - 1) * 16; at < end; ++at) proposed[at] = 0;
        for (usize at = 0; at < magic.size(); ++at) proposed[begin + at] = magic[at];
        store16(proposed, 136, 1);
        proposed[140] = u8(plan.count); proposed[141] = u8(update.old_name.size()); proposed[142] = u8(update.new_name.size());
        store32(proposed, 144, update.from_parent); store32(proposed, 148, update.to_parent);
        store32(proposed, 152, update.source); store32(proposed, 156, update.from_generation);
        store32(proposed, 160, update.to_generation); store32(proposed, 164, update.source_generation);
        store32(proposed, 168, update.seconds); store32(proposed, 172, update.nanoseconds);
        for (usize at = 0; at < update.old_name.size(); ++at) proposed[240 + at] = u8(update.old_name[at]);
        for (usize at = 0; at < update.new_name.size(); ++at) proposed[272 + at] = u8(update.new_name[at]);
        usize cursor = patch_begin, runs = 0;
        for (usize home = 0; home < plan.count; ++home)
        {
            store32(proposed, 176 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            store32(proposed, 200 + home * 4, plan.targets[home]);
            for (usize at = 0; at < 1024;)
            {
                if (plan.before[home][at] == plan.after[home][at]) { ++at; continue; }
                const usize first = at;
                while (at < 1024 && plan.before[home][at] != plan.after[home][at]) ++at;
                const usize length = at - first;
                if (cursor > end || end - cursor < 5 || length > end - cursor - 5)
                    return Status::unsupported("regular move complete preimage witness exceeds descriptor capacity");
                proposed[cursor] = u8(home); store16(proposed, cursor + 1, u16(first)); store16(proposed, cursor + 3, u16(length));
                cursor += 5;
                for (usize byte = 0; byte < length; ++byte) proposed[cursor++] = plan.before[home][first + byte];
                ++runs;
            }
        }
        store16(proposed, 138, u16(cursor)); store16(proposed, 304, u16(runs));
        descriptor = proposed;
        return {};
    }

    // Names borrow immutable descriptor storage. All before blocks must be
    // disjoint from every input and from the witness output. Aliasing refusals
    // and oversized-span refusals happen before either output is reset. Later
    // failure clears the witness; partial blocks grant no recovery authority.
    [[nodiscard]] static Status decode(const Block &descriptor, std::span<const Block> after,
                                       std::span<Block> before, Ext4RegularMoveWitness &output)
    {
        using namespace ext4_checked;
        if (after.size() > Ext4RegularMovePlan::capacity || before.size() > Ext4RegularMovePlan::capacity)
            return Status::invalid_argument("regular move witness span capacity");
        if (overlap(before.data(), before.size_bytes(), after.data(), after.size_bytes()) ||
            overlap(before.data(), before.size_bytes(), &descriptor, sizeof(descriptor)) ||
            overlap(before.data(), before.size_bytes(), &output, sizeof(output)) ||
            overlap(&output, sizeof(output), after.data(), after.size_bytes()) ||
            overlap(&output, sizeof(output), &descriptor, sizeof(descriptor)))
            return Status::invalid_argument("regular move witness decode outputs alias input");
        output = {};
        if (!present(descriptor) || le16(descriptor, 136) != 1)
            return Status::unsupported("regular move durable witness absent or unknown");
        const usize count = descriptor[140], old_length = descriptor[141], new_length = descriptor[142];
        const usize limit = le16(descriptor, 138), runs = le16(descriptor, 304);
        if (count < 3 || count > Ext4RegularMovePlan::capacity || after.size() != count || before.size() != count ||
            !old_length || old_length > 31 || !new_length || new_length > 31 || limit < patch_begin || limit > end)
            return Status::invalid_argument("regular move witness header geometry");
        if (!zero(descriptor, 44 + (count - 1) * 16, begin) || descriptor[143] ||
            !zero(descriptor, 176 + count * 4, 200) || !zero(descriptor, 200 + count * 4, 240) ||
            !zero(descriptor, 240 + old_length, 272) || !zero(descriptor, 272 + new_length, 304) ||
            !zero(descriptor, 306, patch_begin) || !zero(descriptor, limit, end))
            return Status::io_error("regular move witness padding differs");
        Ext4RegularMoveWitness decoded{};
        decoded.update = {le32(descriptor, 144), le32(descriptor, 148), le32(descriptor, 152),
            le32(descriptor, 156), le32(descriptor, 160), le32(descriptor, 164),
            {reinterpret_cast<const char *>(descriptor.data() + 240), old_length},
            {reinterpret_cast<const char *>(descriptor.data() + 272), new_length},
            le32(descriptor, 168), le32(descriptor, 172)};
        if (!update_valid(decoded.update)) return Status::invalid_argument("regular move witnessed identity or names");
        for (usize home = 0; home < count; ++home)
        {
            decoded.targets[home] = le32(descriptor, 200 + home * 4);
            if (!decoded.targets[home]) return Status::io_error("regular move witnessed zero home");
            for (usize previous = 0; previous < home; ++previous)
                if (decoded.targets[previous] == decoded.targets[home])
                    return Status::io_error("regular move witnessed duplicate home");
        }
        for (usize home = 0; home < count; ++home) before[home] = after[home];
        usize cursor = patch_begin, previous_home = 0, previous_end = 0;
        for (usize run = 0; run < runs; ++run)
        {
            if (cursor > limit || limit - cursor < 5) return Status::io_error("regular move witness truncated patch");
            const usize home = descriptor[cursor], offset = le16(descriptor, cursor + 1), length = le16(descriptor, cursor + 3);
            cursor += 5;
            if (home >= count || offset >= 1024 || !length || length > 1024 - offset || length > limit - cursor ||
                (run && (home < previous_home || (home == previous_home && offset <= previous_end))))
                return Status::io_error("regular move witness patch bounds or order");
            for (usize byte = 0; byte < length; ++byte)
            {
                if (descriptor[cursor + byte] == after[home][offset + byte])
                    return Status::io_error("regular move witness patch includes unchanged byte");
                before[home][offset + byte] = descriptor[cursor + byte];
            }
            cursor += length; previous_home = home; previous_end = offset + length;
        }
        if (cursor != limit) return Status::io_error("regular move witness patch length differs");
        for (usize home = 0; home < count; ++home)
            if (le32(descriptor, 176 + home * 4) != jbd2::detail::crc32c(~u32{0}, before[home]))
                return Status::io_error("regular move complete preimage checksum differs");
        decoded.count = count; decoded.runs = runs; decoded.bytes = limit - begin;
        output = decoded;
        return {};
    }
};
} // namespace ok::fs
