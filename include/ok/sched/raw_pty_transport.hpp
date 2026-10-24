#pragma once

#include "ok/core/types.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace ok::sched::private_pty
{
// Private serialized boot-CPU owner model. This is not an exported Kernel ABI.
enum class Side : std::uint8_t { master, slave };
inline constexpr std::uint16_t invalid_slot = std::numeric_limits<std::uint16_t>::max();

struct Handle
{
    std::uint16_t slot{invalid_slot};
    std::uint64_t generation{};
    Side side{Side::master};
    constexpr bool operator==(const Handle &) const = default;
    [[nodiscard]] explicit operator bool() const { return slot != invalid_slot || generation != 0; }
};
struct PairHandles
{
    Handle master{}, slave{};
};
struct Stamp
{
    std::uint16_t slot{invalid_slot};
    std::uint64_t generation{}, revision{};
    // A saturated revision is diagnostic only: equality cannot suppress rechecks.
    bool revision_saturated{};
    constexpr bool operator==(const Stamp &) const = default;
};
struct Change
{
    Stamp stamp{};
    bool changed{}, master_retired{}, slave_retired{}, pair_freed{};
};
struct Transfer
{
    std::size_t bytes{};
    Change change{};
};
struct Readiness
{
    Stamp stamp{};
    std::size_t buffered{}, outgoing_available{};
    bool readable{}, writable{}, hangup{}, error{};
};
struct Audit
{
    std::size_t live_pairs{}, live_endpoints{}, buffered_bytes{};
    bool cleared_outside_live_bytes{true};
};

template <std::size_t PairCapacity = 8, std::size_t QueueCapacity = 4096,
          std::uint64_t GenerationLimit = std::numeric_limits<std::uint64_t>::max(),
          std::uint64_t RevisionLimit = std::numeric_limits<std::uint64_t>::max(),
          std::uint64_t InitialGeneration = 0>
class RawPtyTable final
{
    static_assert(PairCapacity > 0 && PairCapacity < invalid_slot);
    static_assert(QueueCapacity > 0 && GenerationLimit > 0 && RevisionLimit > 0);
    static_assert(InitialGeneration < GenerationLimit);
    struct Queue
    {
        std::array<std::byte, QueueCapacity> bytes{};
        std::size_t head{}, size{};

        // Volatile stores also clear bytes after a final read/retirement.
        static void erase(std::byte &byte)
        {
            auto *target = reinterpret_cast<volatile unsigned char *>(&byte);
            *target = 0;
        }
        void clear()
        {
            for (auto &byte : bytes) erase(byte);
            head = size = 0;
        }
        [[nodiscard]] std::size_t read(std::span<std::byte> output)
        {
            const auto count = output.size() < size ? output.size() : size;
            for (std::size_t index = 0; index < count; ++index)
            {
                output[index] = bytes[head];
                erase(bytes[head]);
                if (++head == QueueCapacity) head = 0;
            }
            size -= count;
            if (!size) head = 0;
            return count;
        }
        [[nodiscard]] std::size_t write(std::span<const std::byte> input)
        {
            const auto available = QueueCapacity - size;
            const auto count = input.size() < available ? input.size() : available;
            auto tail = head >= QueueCapacity - size ? head - (QueueCapacity - size) : head + size;
            for (std::size_t index = 0; index < count; ++index)
            {
                bytes[tail] = input[index];
                if (++tail == QueueCapacity) tail = 0;
            }
            size += count;
            return count;
        }
        [[nodiscard]] bool cleared_outside_live_bytes() const
        {
            for (std::size_t index = 0; index < QueueCapacity; ++index)
            {
                const auto offset = index >= head ? index - head : QueueCapacity - head + index;
                if (offset >= size && bytes[index] != std::byte{}) return false;
            }
            return true;
        }
    };
    struct Pair
    {
        Queue master_input{}, slave_input{};
        // Generation survives freeing a slot. Endpoint owner counts live in the OFD layer.
        std::uint64_t generation{}, revision{};
        bool used{}, master_alive{}, slave_alive{};
    };
    std::array<Pair, PairCapacity> pairs_{};

    [[nodiscard]] static bool known_side(Side side)
    { return side == Side::master || side == Side::slave; }
    [[nodiscard]] static bool local_alive(const Pair &pair, Side side)
    { return side == Side::master ? pair.master_alive : pair.slave_alive; }
    [[nodiscard]] static bool peer_alive(const Pair &pair, Side side)
    { return side == Side::master ? pair.slave_alive : pair.master_alive; }
    [[nodiscard]] static Queue &incoming(Pair &pair, Side side)
    { return side == Side::master ? pair.master_input : pair.slave_input; }
    [[nodiscard]] static Queue &outgoing(Pair &pair, Side side)
    { return side == Side::master ? pair.slave_input : pair.master_input; }
    [[nodiscard]] const Pair *resolve(Handle handle) const
    {
        if (handle.slot >= pairs_.size() || !handle.generation || !known_side(handle.side)) return nullptr;
        const auto &pair = pairs_[handle.slot];
        return pair.used && pair.generation == handle.generation && local_alive(pair, handle.side) ? &pair : nullptr;
    }
    [[nodiscard]] Pair *resolve(Handle handle)
    { return const_cast<Pair *>(static_cast<const RawPtyTable *>(this)->resolve(handle)); }
    [[nodiscard]] static Stamp stamp(std::uint16_t slot, const Pair &pair)
    { return {slot, pair.generation, pair.revision, pair.revision == RevisionLimit}; }
    [[nodiscard]] static Change observe(std::uint16_t slot, const Pair &pair, bool changed)
    {
        return {stamp(slot, pair), changed, !pair.master_alive, !pair.slave_alive, !pair.used};
    }
    static void advance_revision(Pair &pair)
    {
        // Closure must never fail because a diagnostic revision is exhausted.
        if (pair.revision != RevisionLimit) ++pair.revision;
    }

  public:
    static constexpr std::size_t pair_capacity = PairCapacity, queue_capacity = QueueCapacity;
    static constexpr bool serialized_boot_cpu = true;
    class PreparedRead final
    {
      public:
        PreparedRead(const PreparedRead &) = delete;
        PreparedRead &operator=(const PreparedRead &) = delete;
        PreparedRead &operator=(PreparedRead &&) = delete;
        PreparedRead(PreparedRead &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), handle_(other.handle_),
              revision_(other.revision_), count_(other.count_), bytes_(other.bytes_) {}
        [[nodiscard]] Handle endpoint() const { return handle_; }
        [[nodiscard]] bool owned_by(const RawPtyTable &owner) const { return owner_ == &owner; }
        [[nodiscard]] std::span<const std::byte> bytes() const { return {bytes_.data(), count_}; }
        void cancel() { owner_ = nullptr; }
        [[nodiscard]] Result<Transfer> commit()
        {
            if (!owner_) return Status::busy("PTY read preview retired");
            auto *owner = std::exchange(owner_, nullptr);
            const auto *pair = owner->resolve(handle_);
            if (!pair) return Status::bad_descriptor("PTY preview endpoint retired");
            if (count_ && (pair->revision != revision_ || pair->revision == RevisionLimit))
                return Status::busy("PTY read preview revision changed or saturated");
            return owner->read(handle_, {bytes_.data(), count_});
        }
      private:
        friend class RawPtyTable;
        PreparedRead(RawPtyTable &owner, Handle handle, std::uint64_t revision, std::size_t count)
            : owner_(&owner), handle_(handle), revision_(revision), count_(count) {}
        RawPtyTable *owner_{};
        Handle handle_{};
        std::uint64_t revision_{};
        std::size_t count_{};
        std::array<std::byte, QueueCapacity> bytes_{};
    };
    RawPtyTable()
    {
        for (auto &pair : pairs_) pair.generation = InitialGeneration;
    }
    RawPtyTable(const RawPtyTable &) = delete;
    RawPtyTable &operator=(const RawPtyTable &) = delete;
    RawPtyTable(RawPtyTable &&) = delete;
    RawPtyTable &operator=(RawPtyTable &&) = delete;

    // A retained canonical OFD must outlive this preview. Copy-out precedes
    // commit; a failed/partial copy can cancel without consuming any input.
    [[nodiscard]] Result<PreparedRead> prepare_read(Handle handle, std::size_t count)
    {
        auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        auto &queue = incoming(*pair, handle.side);
        if (!count) return PreparedRead{*this, handle, pair->revision, 0};
        if (!queue.size)
        {
            if (peer_alive(*pair, handle.side)) return Status::would_block("raw PTY queue is empty");
            if (handle.side == Side::master) return Status::io_error("raw PTY slave retired");
            return PreparedRead{*this, handle, pair->revision, 0};
        }
        // Diagnostic saturated revisions cannot authorize a deferred consume.
        if (pair->revision == RevisionLimit) return Status::overflow("PTY preview revision saturated");
        if (count > queue.size) count = queue.size;
        PreparedRead preview{*this, handle, pair->revision, count};
        auto cursor = queue.head;
        for (std::size_t index = 0; index < count; ++index)
        {
            preview.bytes_[index] = queue.bytes[cursor];
            if (++cursor == QueueCapacity) cursor = 0;
        }
        return preview;
    }

    // Handles are local to this single nonmovable owner table. No retain API:
    // the future OFD layer calls retire only when its final endpoint hold dies.
    [[nodiscard]] Result<PairHandles> create()
    {
        bool exhausted_free_slot = false;
        for (std::size_t slot = 0; slot < pairs_.size(); ++slot)
        {
            auto &pair = pairs_[slot];
            if (pair.used) continue;
            if (pair.generation == GenerationLimit) { exhausted_free_slot = true; continue; }
            pair.master_input.clear(); pair.slave_input.clear();
            ++pair.generation;
            pair.revision = 1;
            pair.used = pair.master_alive = pair.slave_alive = true;
            const auto index = static_cast<std::uint16_t>(slot);
            return PairHandles{{index, pair.generation, Side::master}, {index, pair.generation, Side::slave}};
        }
        return exhausted_free_slot ? Status::overflow("PTY generation exhausted") : Status::system_file_limit("PTY pair table full");
    }
    [[nodiscard]] Result<Transfer> read(Handle handle, std::span<std::byte> output)
    {
        auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        if (output.empty()) return Transfer{0, observe(handle.slot, *pair, false)};
        auto &queue = incoming(*pair, handle.side);
        if (!queue.size)
        {
            if (peer_alive(*pair, handle.side)) return Status::would_block("raw PTY queue is empty or full");
            if (handle.side == Side::master) return Status::io_error("raw PTY peer retired");
            return Transfer{0, observe(handle.slot, *pair, false)};
        }
        const auto count = queue.read(output);
        advance_revision(*pair);
        return Transfer{count, observe(handle.slot, *pair, true)};
    }
    [[nodiscard]] Result<Transfer> write(Handle handle, std::span<const std::byte> input)
    {
        auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        if (input.empty()) return Transfer{0, observe(handle.slot, *pair, false)};
        if (!peer_alive(*pair, handle.side)) return Status::io_error("raw PTY peer retired");
        auto &queue = outgoing(*pair, handle.side);
        if (queue.size == QueueCapacity) return Status::would_block("raw PTY queue is empty or full");
        const auto count = queue.write(input);
        advance_revision(*pair);
        return Transfer{count, observe(handle.slot, *pair, true)};
    }
    [[nodiscard]] Result<Readiness> readiness(Handle handle) const
    {
        const auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        const auto &input = handle.side == Side::master ? pair->master_input : pair->slave_input;
        const auto &output = handle.side == Side::master ? pair->slave_input : pair->master_input;
        const bool peer = peer_alive(*pair, handle.side);
        return Readiness{stamp(handle.slot, *pair), input.size, QueueCapacity - output.size,
            input.size != 0 || !peer, peer && output.size != QueueCapacity, !peer,
            handle.side == Side::slave && !peer};
    }
    [[nodiscard]] Result<Change> retire(Handle handle)
    {
        auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        if (handle.side == Side::master) pair->master_alive = false;
        else pair->slave_alive = false;
        // Destroy unread input to this retired side; accepted peer input drains.
        incoming(*pair, handle.side).clear();
        advance_revision(*pair);
        if (!pair->master_alive && !pair->slave_alive)
        {
            pair->master_input.clear(); pair->slave_input.clear();
            pair->used = false;
        }
        return observe(handle.slot, *pair, true);
    }
    // Read-only diagnostics. No byte contents or mutable provider state escape.
    [[nodiscard]] Audit audit() const
    {
        Audit result{};
        for (const auto &pair : pairs_)
        {
            result.live_pairs += pair.used ? 1 : 0;
            result.live_endpoints += pair.master_alive ? 1 : 0;
            result.live_endpoints += pair.slave_alive ? 1 : 0;
            result.buffered_bytes += pair.master_input.size + pair.slave_input.size;
            result.cleared_outside_live_bytes = result.cleared_outside_live_bytes &&
                pair.master_input.cleared_outside_live_bytes() && pair.slave_input.cleared_outside_live_bytes() &&
                (pair.master_alive || !pair.master_input.size) && (pair.slave_alive || !pair.slave_input.size) &&
                (pair.used || (!pair.master_alive && !pair.slave_alive));
        }
        return result;
    }
};
} // namespace ok::sched::private_pty
