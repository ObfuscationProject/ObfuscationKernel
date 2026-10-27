#pragma once

// ext4 journal module: JBD2 on-disk records, the bounded journal replay executor and the ext4
// journal recovery binding.

#include "ok/core/types.hpp"
#include "ok/driver/driver.hpp"
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

namespace ok::fs
{

struct Ext4RecoveryBounds
{
    u64 first_sector{};
    u64 sector_count{};
};

// Admission facts are neither a mount capability nor a full allocation fsck.
// The metadata domain excludes directory data and external extent-tree blocks.
struct Ext4RecoveryInfo
{
    static constexpr usize group_capacity = 32;
    jbd2::RecoveryLayout layout{};
    std::array<u8, 16> uuid{};
    std::array<jbd2::BlockRange, group_capacity * 4> metadata{};
    usize metadata_count{};
    u32 journal_inode{};
    u32 journal_sequence{};
    bool needs_recovery{};
    bool empty_log{};

    [[nodiscard]] jbd2::TargetDomain structural_metadata() const
    {
        if (metadata_count > metadata.size())
            return {};
        return {layout.filesystem_blocks, std::span<const jbd2::BlockRange>{metadata}.first(metadata_count)};
    }
};

// One serialized, read-only admission attempt. The owner must exclude device
// replacement, filesystem caches and other writers until these facts retire.
// No writes/flushes, allocation, locks, journal replay or mount publication occur.
// Supported: Linux ext4, 1/2/4 KiB blocks, 32-bit addresses, 256-byte inodes,
// metadata_csum, internal contiguous extent journal, synchronous JBD2 CRC32C v3.
class Ext4RecoveryBinding final
{
    template <usize ActionCapacity, usize RevokeCapacity>
        requires (ActionCapacity > 0 && RevokeCapacity > 0)
    friend class Ext4ReplayPreview;
  public:
    Ext4RecoveryBinding() = default;
    Ext4RecoveryBinding(const Ext4RecoveryBinding &) = delete;
    Ext4RecoveryBinding &operator=(const Ext4RecoveryBinding &) = delete;
    Ext4RecoveryBinding(Ext4RecoveryBinding &&) = delete;
    Ext4RecoveryBinding &operator=(Ext4RecoveryBinding &&) = delete;

    [[nodiscard]] Status inspect(driver::BlockDevice &device, Ext4RecoveryBounds bounds);
    [[nodiscard]] const Ext4RecoveryInfo *info() const { return admitted_ ? &info_ : nullptr; }

  private:
    [[nodiscard]] Status inspect_impl(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                      bool create_candidate, bool first_data_candidate);
    [[nodiscard]] Status inspect_create_candidate(driver::BlockDevice &device,
                                                   Ext4RecoveryBounds bounds)
    { return inspect_impl(device, bounds, true, false); }
    [[nodiscard]] Status inspect_first_data_candidate(driver::BlockDevice &device,
                                                       Ext4RecoveryBounds bounds)
    { return inspect_impl(device, bounds, false, true); }
    // Both provisional bitmap exceptions remain private to the complete
    // directory-create witness proof; they never grant replay authority alone.
    [[nodiscard]] Status inspect_directory_create_candidate(driver::BlockDevice &device,
                                                             Ext4RecoveryBounds bounds)
    { return inspect_impl(device, bounds, true, true); }
    struct Group
    {
        u32 begin{}, end{}, block_bitmap{}, inode_bitmap{}, table{}, table_blocks{};
        u16 block_checksum{}, inode_checksum{}, flags{};
    };
    [[nodiscard]] Status admit();
    [[nodiscard]] Status stable() const;
    [[nodiscard]] Status read_sectors(u64 relative, std::span<u8> out);
    [[nodiscard]] Status read_block(u32 block);
    [[nodiscard]] Status add_range(u64 first, u64 count);
    [[nodiscard]] Status groups();
    [[nodiscard]] Status journal();
    [[nodiscard]] Status allocation();

    bool attempted_{}, admitted_{};
    bool create_candidate_{};
    usize create_bitmap_mismatches_{};
    u32 create_bitmap_mismatch_group_{};
    bool first_data_candidate_{};
    usize first_data_bitmap_mismatches_{};
    u32 first_data_bitmap_mismatch_group_{};
    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    Ext4RecoveryBounds bounds_{};
    Ext4RecoveryInfo info_{};
    u32 seed_{}, first_data_{}, blocks_per_group_{}, inodes_per_group_{}, inode_count_{}, group_count_{};
    std::array<Group, Ext4RecoveryInfo::group_capacity> groups_{};
    std::array<u8, 1024> super_{};
    std::array<u8, 1024> descriptors_{};
    std::array<u8, 256> journal_inode_{};
    std::array<u8, 4096> block_{};
};

} // namespace ok::fs
