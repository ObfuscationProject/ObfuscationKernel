#pragma once

#include "ok/core/types.hpp"

#include <array>
#include <concepts>
#include <span>

namespace ok::fs::jbd2
{

inline constexpr u32 magic = 0xc03b3998;
inline constexpr u32 feature_revoke = 1;
inline constexpr u32 feature_64bit = 2;
inline constexpr u32 feature_checksum_v3 = 16;
inline constexpr u32 flag_escape = 1;
inline constexpr u32 flag_same_uuid = 2;
inline constexpr u32 flag_last_tag = 8;

enum class RecordType : u32
{
    descriptor = 1,
    commit = 2,
    superblock_v2 = 4,
    revoke = 5,
};

namespace detail
{
[[nodiscard]] constexpr u32 be32(std::span<const u8> bytes, usize offset)
{
    return (u32{bytes[offset]} << 24) | (u32{bytes[offset + 1]} << 16) |
           (u32{bytes[offset + 2]} << 8) | bytes[offset + 3];
}

[[nodiscard]] constexpr u64 be64(std::span<const u8> bytes, usize offset)
{
    return (u64{be32(bytes, offset)} << 32) | be32(bytes, offset + 4);
}

[[nodiscard]] constexpr u32 crc_byte(u32 crc, u8 byte)
{
    crc ^= byte;
    for (u32 bit = 0; bit < 8; ++bit)
    {
        crc = (crc >> 1) ^ (0x82f63b78U & (0U - (crc & 1U)));
    }
    return crc;
}

// JBD2 uses the raw CRC32C state, with no final complement.
[[nodiscard]] constexpr u32 crc32c(u32 seed, std::span<const u8> bytes)
{
    for (const auto byte : bytes)
    {
        seed = crc_byte(seed, byte);
    }
    return seed;
}

[[nodiscard]] constexpr u32 checksum_zeroed(u32 seed, std::span<const u8> bytes, usize offset)
{
    for (usize index = 0; index < bytes.size(); ++index)
    {
        seed = crc_byte(seed, index >= offset && index - offset < 4 ? 0 : bytes[index]);
    }
    return seed;
}

[[nodiscard]] constexpr bool all_zero(std::span<const u8> bytes)
{
    for (const auto byte : bytes)
    {
        if (byte != 0)
        {
            return false;
        }
    }
    return true;
}
} // namespace detail

// Descriptor tag bytes are distinct from the journal-superblock checksum UUID.
// Select linux_internal_zero only after qualifying an internally owned journal
// inode and its mapping. Zero descriptor bytes alone do not establish identity;
// this policy never changes the expected superblock UUID or checksum seed.
enum class DescriptorUuidPolicy : u8 { journal_superblock, linux_internal_zero };

struct Superblock
{
    u32 block_size{};
    u32 max_blocks{};
    u32 first_block{};
    u32 sequence{};
    u32 start_block{};
    u32 features{};
    std::array<u8, 16> uuid{};
};

struct Tag
{
    u64 target_block{};
    u32 checksum{};
    bool restore_magic{};
};

struct Commit
{
    u64 seconds{};
    u32 nanoseconds{};
};

// A validated, read-only checksum-v3 codec; no disk mutation or mount operation.
class Codec final
{
  public:
    [[nodiscard]] static Result<Codec> open(std::span<const u8> bytes, u32 available_blocks,
                                            std::span<const u8, 16> expected_uuid,
                                            DescriptorUuidPolicy tag_policy = DescriptorUuidPolicy::journal_superblock)
    {
        if (bytes.size() < 1024)
        {
            return Status::invalid_argument("truncated JBD2 superblock");
        }
        bytes = bytes.first(1024);
        if (detail::be32(bytes, 0) != magic || detail::be32(bytes, 4) != static_cast<u32>(RecordType::superblock_v2))
        {
            return Status::unsupported("JBD2 v2 superblock required");
        }
        const auto features = detail::be32(bytes, 40);
        if (detail::be32(bytes, 36) != 0 || detail::be32(bytes, 44) != 0 ||
            (features & feature_checksum_v3) == 0 ||
            (features & ~(feature_revoke | feature_64bit | feature_checksum_v3)) != 0 || bytes[80] != 4)
        {
            return Status::unsupported("only synchronous JBD2 CRC32C v3 is supported");
        }
        if (detail::be32(bytes, 64) != 1 || detail::be32(bytes, 68) != 0 || detail::be32(bytes, 84) != 0)
        {
            return Status::unsupported("shared, dynamic or fast-commit journal layout");
        }
        if (detail::checksum_zeroed(~u32{0}, bytes, 252) != detail::be32(bytes, 252))
        {
            return Status::io_error("JBD2 superblock checksum");
        }
        if (tag_policy != DescriptorUuidPolicy::journal_superblock &&
            tag_policy != DescriptorUuidPolicy::linux_internal_zero)
            return Status::invalid_argument("JBD2 descriptor UUID policy");
        Codec codec;
        codec.tag_policy_ = tag_policy;
        auto &sb = codec.superblock_;
        sb.block_size = detail::be32(bytes, 12);
        sb.max_blocks = detail::be32(bytes, 16);
        sb.first_block = detail::be32(bytes, 20);
        sb.sequence = detail::be32(bytes, 24);
        sb.start_block = detail::be32(bytes, 28);
        sb.features = features;
        if (sb.block_size != 1024 && sb.block_size != 2048 && sb.block_size != 4096)
        {
            return Status::unsupported("JBD2 block size");
        }
        if (sb.max_blocks < 1024 || sb.max_blocks > available_blocks || sb.first_block == 0 ||
            sb.first_block >= sb.max_blocks || sb.max_blocks - sb.first_block >= 0x80000000U ||
            (sb.start_block != 0 && (sb.start_block < sb.first_block || sb.start_block >= sb.max_blocks)))
        {
            return Status::invalid_argument("JBD2 log bounds");
        }
        if (detail::be32(bytes, 32) != 0)
        {
            return Status::io_error("JBD2 recorded journal error");
        }
        for (usize index = 0; index < sb.uuid.size(); ++index)
        {
            sb.uuid[index] = bytes[48 + index];
            if (sb.uuid[index] != expected_uuid[index])
            {
                return Status::invalid_argument("JBD2 journal UUID mismatch");
            }
        }
        codec.seed_ = detail::crc32c(~u32{0}, sb.uuid);
        return codec;
    }

    [[nodiscard]] const Superblock &superblock() const { return superblock_; }

    // Callback is invoked only after the complete descriptor validates.
    template <typename Visitor>
        requires requires(Visitor visitor, const Tag &tag) { { visitor(tag) } -> std::same_as<Status>; }
    [[nodiscard]] Status descriptor(std::span<const u8> block, u32 sequence, Visitor &&visitor) const
    {
        const auto status = record(block, RecordType::descriptor, sequence, superblock_.block_size - 4);
        if (!status)
        {
            return status;
        }
        const auto ignore = [](const Tag &) { return Status::success(); };
        const auto validated = tags(block, ignore);
        if (!validated)
        {
            return validated;
        }
        return tags(block, visitor);
    }

    template <typename Visitor>
        requires requires(Visitor visitor, u64 target) { { visitor(target) } -> std::same_as<Status>; }
    [[nodiscard]] Status revoke(std::span<const u8> block, u32 sequence, Visitor &&visitor) const
    {
        if ((superblock_.features & feature_revoke) == 0)
        {
            return Status::unsupported("JBD2 revoke feature absent");
        }
        const auto status = record(block, RecordType::revoke, sequence, superblock_.block_size - 4);
        if (!status)
        {
            return status;
        }
        const auto count = detail::be32(block, 12);
        const u32 width = (superblock_.features & feature_64bit) != 0 ? 8 : 4;
        if (count < 16 || count > block.size() - 4 || (count - 16) % width != 0)
        {
            return Status::invalid_argument("JBD2 revoke record length");
        }
        for (usize offset = 16; offset < count; offset += width)
        {
            const auto target = width == 8 ? detail::be64(block, offset) : detail::be32(block, offset);
            const auto accepted = visitor(target);
            if (!accepted)
            {
                return accepted;
            }
        }
        return Status::success();
    }

    [[nodiscard]] Result<Commit> commit(std::span<const u8> block, u32 sequence) const
    {
        const auto status = record(block, RecordType::commit, sequence, 16);
        if (!status)
        {
            return status;
        }
        if (!detail::all_zero(block.subspan(12, 4)) || !detail::all_zero(block.subspan(20, 28)))
        {
            return Status::unsupported("JBD2 v3 commit checksum layout");
        }
        const auto nanoseconds = detail::be32(block, 56);
        if (nanoseconds >= 1000000000)
        {
            return Status::invalid_argument("JBD2 commit timestamp");
        }
        return Commit{detail::be64(block, 48), nanoseconds};
    }

    [[nodiscard]] Status payload(std::span<const u8> block, u32 sequence, const Tag &tag) const
    {
        if (block.size() != superblock_.block_size)
        {
            return Status::invalid_argument("JBD2 payload block size");
        }
        const std::array<u8, 4> sequence_bytes{u8(sequence >> 24), u8(sequence >> 16),
                                               u8(sequence >> 8), u8(sequence)};
        if (detail::crc32c(detail::crc32c(seed_, sequence_bytes), block) != tag.checksum)
        {
            return Status::io_error("JBD2 payload checksum");
        }
        if ((tag.restore_magic && detail::be32(block, 0) != 0) ||
            (!tag.restore_magic && detail::be32(block, 0) == magic))
        {
            return Status::invalid_argument("JBD2 payload escape encoding");
        }
        return Status::success();
    }

  private:
    Codec() = default;
    Superblock superblock_{};
    u32 seed_{};
    DescriptorUuidPolicy tag_policy_{DescriptorUuidPolicy::journal_superblock};

    [[nodiscard]] Status record(std::span<const u8> block, RecordType type, u32 sequence, usize checksum) const
    {
        if (block.size() != superblock_.block_size)
        {
            return Status::invalid_argument("JBD2 record block size");
        }
        if (detail::be32(block, 0) != magic || detail::be32(block, 4) != static_cast<u32>(type) ||
            detail::be32(block, 8) != sequence)
        {
            return Status::invalid_argument("JBD2 record header");
        }
        if (detail::checksum_zeroed(seed_, block, checksum) != detail::be32(block, checksum))
        {
            return Status::io_error("JBD2 record checksum");
        }
        return Status::success();
    }

    template <typename Visitor> [[nodiscard]] Status tags(std::span<const u8> block, Visitor &&visitor) const
    {
        const auto end = block.size() - 4;
        usize offset = 12;
        bool has_uuid = false;
        while (offset <= end && end - offset >= 16)
        {
            const auto low = detail::be32(block, offset);
            const auto flags = detail::be32(block, offset + 4);
            const auto high = detail::be32(block, offset + 8);
            const auto checksum = detail::be32(block, offset + 12);
            offset += 16;
            if ((flags & ~(flag_escape | flag_same_uuid | flag_last_tag)) != 0)
            {
                return Status::unsupported("JBD2 deleted or unknown tag flags");
            }
            if ((superblock_.features & feature_64bit) == 0 && high != 0)
            {
                return Status::unsupported("JBD2 high target without 64bit feature");
            }
            if ((flags & flag_same_uuid) == 0)
            {
                if (end - offset < 16)
                {
                    return Status::invalid_argument("truncated JBD2 tag UUID");
                }
                for (usize index = 0; index < 16; ++index)
                {
                    if (block[offset + index] != (tag_policy_ == DescriptorUuidPolicy::linux_internal_zero ? 0 : superblock_.uuid[index]))
                    {
                        return Status::invalid_argument("foreign JBD2 tag UUID");
                    }
                }
                offset += 16;
                has_uuid = true;
            }
            else if (!has_uuid)
            {
                return Status::invalid_argument("first JBD2 tag omits UUID");
            }
            const auto accepted = visitor(Tag{(u64{high} << 32) | low, checksum, (flags & flag_escape) != 0});
            if (!accepted)
            {
                return accepted;
            }
            if ((flags & flag_last_tag) != 0)
            {
                return Status::success();
            }
        }
        return Status::invalid_argument("JBD2 descriptor has no final tag");
    }
};

struct BlockRange
{
    u64 first{};
    u64 count{};
};

// Caller derives these ranges from a validated filesystem, excluding journal storage.
struct TargetDomain
{
    u64 filesystem_blocks{};
    std::span<const BlockRange> metadata{};

    [[nodiscard]] bool valid() const
    {
        if (filesystem_blocks == 0 || metadata.empty())
        {
            return false;
        }
        for (const auto &range : metadata)
        {
            if (range.count == 0 || range.first >= filesystem_blocks ||
                range.count > filesystem_blocks - range.first)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool permits(u64 target) const
    {
        for (const auto &range : metadata)
        {
            if (target >= range.first && target - range.first < range.count)
            {
                return true;
            }
        }
        return false;
    }
};

struct ReplayAction
{
    u64 target_block{};
    u32 journal_block{};
    u32 sequence{};
    u32 transaction_order{};
    u32 checksum{};
    bool restore_magic{};
};

struct Revocation
{
    u64 target_block{};
    u32 transaction_order{};
};

struct PlanStorage
{
    std::span<u8> record_block{};
    std::span<u8> payload_block{};
    std::span<ReplayAction> actions{};
    std::span<Revocation> revocations{};
};

struct ReplayPlan
{
    std::span<const ReplayAction> actions{};
    u32 committed_transactions{};
    u32 next_sequence{};
    u32 scanned_blocks{};
    usize revoked_actions{};
    bool incomplete_tail{};
    bool empty_log{};
    // Active record blocks, including empty and uncommitted revoke records.
    u32 revoke_records{};
};

template <typename Reader>
concept BlockReader = requires(Reader &reader, u32 index, std::span<u8> buffer) {
    { reader.read_block(index, buffer) } -> std::same_as<Status>;
};

// Reader must expose an immutable journal snapshot and fill each successful read.
// Scratch is caller-owned, disjoint, and unpublished unless the Result succeeds.
// This builds a model plan only: it never executes actions or establishes a mount.
template <BlockReader Reader>
[[nodiscard]] Result<ReplayPlan> build_replay_plan(const Codec &codec, Reader &reader,
                                                 const TargetDomain &domain, PlanStorage storage)
{
    const auto &sb = codec.superblock();
    if (!domain.valid() || storage.record_block.size() < sb.block_size ||
        storage.payload_block.size() < sb.block_size)
    {
        return Status::invalid_argument("JBD2 plan domain or scratch");
    }
    if (sb.start_block == 0)
    {
        return ReplayPlan{{}, 0, sb.sequence, 0, 0, false, true};
    }
    auto record_buffer = storage.record_block.first(sb.block_size);
    auto payload_buffer = storage.payload_block.first(sb.block_size);
    u32 position = sb.start_block;
    u32 scanned = 0;
    u32 sequence = sb.sequence;
    u32 committed = 0;
    usize action_count = 0;
    usize revoke_count = 0;
    usize committed_actions = 0;
    usize committed_revokes = 0;
    u32 revoke_records = 0;
    bool pending = false;
    const auto read_next = [&](std::span<u8> buffer) -> Status {
        if (scanned == sb.max_blocks - sb.first_block)
        {
            return Status::invalid_argument("JBD2 log revisits starting block");
        }
        const auto result = reader.read_block(position, buffer);
        if (!result)
        {
            return result;
        }
        ++scanned;
        ++position;
        if (position == sb.max_blocks)
        {
            position = sb.first_block;
        }
        return Status::success();
    };
    for (;;)
    {
        const auto read = read_next(record_buffer);
        if (!read)
        {
            return read;
        }
        const std::span<const u8> block = record_buffer;
        if (detail::all_zero(block))
        {
            break;
        }
        if (detail::be32(block, 0) != magic)
        {
            return Status::invalid_argument("unrecognized JBD2 log boundary");
        }
        const auto type = detail::be32(block, 4);
        if (type != static_cast<u32>(RecordType::descriptor) && type != static_cast<u32>(RecordType::commit) && type != static_cast<u32>(RecordType::revoke))
        {
            return Status::unsupported("JBD2 log record type");
        }
        const auto record_sequence = detail::be32(block, 8);
        if (record_sequence != sequence)
        {
            const u32 age = sequence - record_sequence;
            if (age >= 0x80000000U)
            {
                return Status::invalid_argument("future or ambiguous JBD2 transaction sequence");
            }
            // Validate a stale boundary before trusting it as the end of the log.
            Status stale;
            if (type == static_cast<u32>(RecordType::descriptor))
            {
                stale = codec.descriptor(block, record_sequence, [](const Tag &) { return Status::success(); });
            }
            else if (type == static_cast<u32>(RecordType::revoke))
            {
                stale = codec.revoke(block, record_sequence, [](u64) { return Status::success(); });
            }
            else
            {
                stale = codec.commit(block, record_sequence).status();
            }
            if (!stale)
            {
                return stale;
            }
            break;
        }
        if (type == static_cast<u32>(RecordType::descriptor))
        {
            pending = true;
            const auto decoded = codec.descriptor(block, sequence, [&](const Tag &tag) -> Status {
                if (!domain.permits(tag.target_block))
                {
                    return Status::denied("JBD2 target outside validated metadata domain");
                }
                if (action_count == storage.actions.size())
                {
                    return Status::no_memory("JBD2 action capacity");
                }
                const auto source = position;
                const auto read_payload = read_next(payload_buffer);
                if (!read_payload)
                {
                    return read_payload;
                }
                const auto checked = codec.payload(payload_buffer, sequence, tag);
                if (!checked)
                {
                    return checked;
                }
                storage.actions[action_count++] = {tag.target_block, source, sequence, committed,
                                                    tag.checksum, tag.restore_magic};
                return Status::success();
            });
            if (!decoded)
            {
                return decoded;
            }
        }
        else if (type == static_cast<u32>(RecordType::revoke))
        {
            pending = true;
            const auto decoded = codec.revoke(block, sequence, [&](u64 target) -> Status {
                if (!domain.permits(target))
                {
                    return Status::denied("JBD2 revoke outside validated metadata domain");
                }
                if (revoke_count == storage.revocations.size())
                {
                    return Status::no_memory("JBD2 revoke capacity");
                }
                storage.revocations[revoke_count++] = {target, committed};
                return Status::success();
            });
            if (!decoded)
            {
                return decoded;
            }
            ++revoke_records;
        }
        else
        {
            const auto decoded = codec.commit(block, sequence);
            if (!decoded)
            {
                return decoded.status();
            }
            ++committed;
            ++sequence;
            committed_actions = action_count;
            committed_revokes = revoke_count;
            pending = false;
        }
    }
    usize output_count = 0;
    usize revoked = 0;
    for (usize index = 0; index < committed_actions; ++index)
    {
        const auto action = storage.actions[index];
        bool suppressed = false;
        for (usize revoke = 0; revoke < committed_revokes; ++revoke)
        {
            const auto &entry = storage.revocations[revoke];
            if (entry.target_block == action.target_block && entry.transaction_order >= action.transaction_order)
            {
                suppressed = true;
                break;
            }
        }
        if (suppressed)
        {
            ++revoked;
        }
        else
        {
            storage.actions[output_count++] = action;
        }
    }
    return ReplayPlan{storage.actions.first(output_count), committed, sequence, scanned, revoked, pending, false, revoke_records};
}

} // namespace ok::fs::jbd2
