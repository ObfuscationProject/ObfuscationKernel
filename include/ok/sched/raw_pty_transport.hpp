#pragma once

#include "ok/core/types.hpp"
#include "ok/sched/terminal.hpp"
#include "ok/tty/termios.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace ok::sched::private_pty
{
// Private serialized boot-CPU owner model. This is not an exported Kernel ABI.
// Every slave is a terminal (docs/modules/tty.md sections 1 and 8): master
// writes run through the slave's N_TTY, slave writes through its OPOST into
// the master queue, and the slave's echo goes into the master queue too.
enum class Side : std::uint8_t { master, slave };
inline constexpr std::uint16_t invalid_slot = std::numeric_limits<std::uint16_t>::max();
inline constexpr std::uint16_t max_endpoint_opens = std::numeric_limits<std::uint16_t>::max();
inline constexpr std::size_t pty_echo_capacity = 512;

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
// Wait keys a change may have made ready; the scheduler wakes them.
inline constexpr std::uint8_t wake_master_read = 1, wake_master_write = 2, wake_slave_read = 4,
                              wake_slave_write = 8, wake_all = 15;
struct Change
{
    Stamp stamp{};
    bool changed{}, master_retired{}, slave_retired{}, pair_freed{};
    std::uint8_t wakes{};
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
    // write_error: a write now fails with EIO (no open slave for a master,
    // no master for a slave), whatever `writable` reports for poll.
    bool readable{}, writable{}, hangup{}, error{}, write_error{};
};
struct Audit
{
    std::size_t live_pairs{}, live_endpoints{}, buffered_bytes{};
    std::size_t master_opens{}, slave_opens{}, dormant_slaves{}, locked_pairs{};
    bool cleared_outside_live_bytes{true};
};

// A pair from create() is a raw pair (OK_SYS_PTY_PAIR): both ends open once,
// raw termios, drain-then-EOF on the slave after the master's last close. A
// pair from create_multiplexed() is ptmx-style: the slave starts locked and
// dormant, opens are counted per endpoint, the slave may be reopened while the
// master lives, and the master's last close hangs the slave up (its input is
// discarded). The slot is freed when both ends have no open.
template <std::size_t PairCapacity = 8, std::size_t QueueCapacity = 4096,
          std::uint64_t GenerationLimit = std::numeric_limits<std::uint64_t>::max(),
          std::uint64_t RevisionLimit = std::numeric_limits<std::uint64_t>::max(),
          std::uint64_t InitialGeneration = 0>
class RawPtyTable final
{
    static_assert(PairCapacity > 0 && PairCapacity < invalid_slot);
    static_assert(QueueCapacity > 0 && GenerationLimit > 0 && RevisionLimit > 0);
    static_assert(InitialGeneration < GenerationLimit);

  public:
    using Ldisc = tty::NTty<QueueCapacity, pty_echo_capacity>;
    using SlaveTerminal = BasicTerminal<Ldisc>;

  private:
    using Plan = typename Ldisc::ReadPlan;
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
        [[nodiscard]] std::size_t peek(std::span<std::byte> output, std::size_t offset, std::size_t limit) const
        {
            if (limit > size) limit = size;
            if (offset >= limit) return 0;
            const auto count = output.size() < limit - offset ? output.size() : limit - offset;
            auto cursor = (head + offset) % QueueCapacity;
            for (std::size_t index = 0; index < count; ++index)
            {
                output[index] = bytes[cursor];
                if (++cursor == QueueCapacity) cursor = 0;
            }
            return count;
        }
        void discard(std::size_t count)
        {
            if (count > size) count = size;
            for (std::size_t index = 0; index < count; ++index)
            {
                erase(bytes[head]);
                if (++head == QueueCapacity) head = 0;
            }
            size -= count;
            if (!size) head = 0;
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
    struct EchoSink
    {
        Queue &queue;
        std::size_t put(std::span<const std::byte> bytes) { return queue.write(bytes); }
    };
    struct Pair
    {
        Queue master_input{};
        SlaveTerminal slave{};
        // Generation survives freeing a slot. Every open counts one canonical OFD.
        std::uint64_t generation{}, revision{};
        std::uint16_t master_opens{}, slave_opens{};
        bool used{}, locked{}, slave_seen{}, multiplexed{};
    };
    std::array<Pair, PairCapacity> pairs_{};

    [[nodiscard]] static bool known_side(Side side)
    { return side == Side::master || side == Side::slave; }
    [[nodiscard]] static std::uint16_t opens(const Pair &pair, Side side)
    { return side == Side::master ? pair.master_opens : pair.slave_opens; }
    // Slave states: dormant (never opened), open, closed (seen, no open).
    [[nodiscard]] static bool slave_closed(const Pair &pair) { return pair.slave_seen && !pair.slave_opens; }
    [[nodiscard]] const Pair *lookup(Handle handle) const
    {
        if (handle.slot >= pairs_.size() || !handle.generation || !known_side(handle.side)) return nullptr;
        const auto &pair = pairs_[handle.slot];
        return pair.used && pair.generation == handle.generation ? &pair : nullptr;
    }
    [[nodiscard]] const Pair *resolve(Handle handle) const
    {
        const auto *pair = lookup(handle);
        return pair && opens(*pair, handle.side) ? pair : nullptr;
    }
    [[nodiscard]] Pair *resolve(Handle handle)
    { return const_cast<Pair *>(static_cast<const RawPtyTable *>(this)->resolve(handle)); }
    [[nodiscard]] static Stamp stamp(std::uint16_t slot, const Pair &pair)
    { return {slot, pair.generation, pair.revision, pair.revision == RevisionLimit}; }
    [[nodiscard]] static Change observe(std::uint16_t slot, const Pair &pair, bool changed, std::uint8_t wakes = 0)
    {
        return {stamp(slot, pair), changed, !pair.master_opens, slave_closed(pair), !pair.used, wakes};
    }
    static void advance_revision(Pair &pair)
    {
        // Closure must never fail because a diagnostic revision is exhausted.
        if (pair.revision != RevisionLimit) ++pair.revision;
    }
    // Moves pending slave echo into the master queue; true when bytes moved.
    static bool flush_echo(Pair &pair)
    {
        EchoSink sink{pair.master_input};
        return pair.slave.ldisc.drain_echo(sink) != 0;
    }
    [[nodiscard]] Result<std::uint16_t> claim()
    {
        bool exhausted_free_slot = false;
        for (std::size_t slot = 0; slot < pairs_.size(); ++slot)
        {
            auto &pair = pairs_[slot];
            if (pair.used) continue;
            if (pair.generation == GenerationLimit) { exhausted_free_slot = true; continue; }
            pair.master_input.clear();
            ++pair.generation;
            pair.revision = 1;
            pair.used = true;
            return static_cast<std::uint16_t>(slot);
        }
        return exhausted_free_slot ? Status::overflow("PTY generation exhausted") : Status::system_file_limit("PTY pair table full");
    }
    static void release_slot(Pair &pair)
    {
        pair.master_input.clear();
        pair.slave.ldisc.hangup();
        pair.slave.session = pair.slave.foreground = 0;
        pair.slave.hung_up = false;
        pair.used = pair.locked = false;
    }
    // What a read of `count` bytes may take now; an empty plan is EOF.
    // `timer_expired`: the reader's VTIME timer ran out (a retry, n_tty_read).
    [[nodiscard]] static Result<Plan> plan(const Pair &pair, Side side, std::size_t count, bool nonblocking,
                                           bool timer_expired = false)
    {
        Plan result{};
        if (side == Side::master)
        {
            const auto available = pair.master_input.size;
            if (!available)
            {
                if (slave_closed(pair)) return Status::io_error("PTY slave closed");
                return Status::would_block("PTY master queue is empty");
            }
            result.copy = result.consume = count < available ? count : available;
            return result;
        }
        const auto &terminal = pair.slave;
        result = terminal.ldisc.plan_read(terminal.termios, count, nonblocking, timer_expired);
        if (result.wait == Ldisc::ReadWait::none) return result;
        if (!pair.master_opens)
        {
            // After the master's last close a raw slave drains what it holds below
            // VMIN (Linux n_tty_read copies before its other-side-closed check),
            // then reads EOF; a hung-up ptmx slave's input was discarded.
            const auto drained = terminal.ldisc.plan_read(terminal.termios, count, true, false);
            if (drained.wait == Ldisc::ReadWait::none && drained.consume) return drained;
            return Plan{.revision = result.revision};
        }
        return Status::would_block("PTY slave input is not ready");
    }
    [[nodiscard]] static std::size_t copy(const Pair &pair, Side side, const Plan &planned, std::span<std::byte> output,
                                          std::size_t offset)
    {
        if (side == Side::master) return pair.master_input.peek(output, offset, planned.copy);
        return pair.slave.ldisc.preview(planned, output, offset);
    }
    [[nodiscard]] Result<Transfer> consume(Handle handle, Pair &pair, const Plan &planned)
    {
        if (!planned.consume) return Transfer{0, observe(handle.slot, pair, false)};
        std::uint8_t wakes = 0;
        if (handle.side == Side::master)
        {
            pair.master_input.discard(planned.consume);
            wakes = wake_slave_write;
            // A master read makes room for echo that did not fit.
            if (flush_echo(pair)) wakes |= wake_master_read;
        }
        else
        {
            if (!pair.slave.ldisc.consume(planned)) return Status::busy("PTY terminal input changed during the read");
            wakes = wake_master_write;
        }
        advance_revision(pair);
        return Transfer{planned.copy, observe(handle.slot, pair, true, wakes)};
    }

  public:
    static constexpr std::size_t pair_capacity = PairCapacity, queue_capacity = QueueCapacity;
    static constexpr bool serialized_boot_cpu = true;
    // A read plan: copy() previews planned bytes, commit() consumes them.
    class PreparedRead final
    {
      public:
        PreparedRead(const PreparedRead &) = delete;
        PreparedRead &operator=(const PreparedRead &) = delete;
        PreparedRead &operator=(PreparedRead &&) = delete;
        PreparedRead(PreparedRead &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), handle_(other.handle_),
              revision_(other.revision_), plan_(other.plan_) {}
        [[nodiscard]] Handle endpoint() const { return handle_; }
        [[nodiscard]] bool owned_by(const RawPtyTable &owner) const { return owner_ == &owner; }
        [[nodiscard]] std::size_t size() const { return plan_.copy; }
        // Copies planned bytes from `offset` on; 0 once retired or stale.
        [[nodiscard]] std::size_t copy(std::span<std::byte> output, std::size_t offset = 0) const
        {
            if (!owner_) return 0;
            const auto *pair = owner_->resolve(handle_);
            if (!pair || pair->revision != revision_) return 0;
            return RawPtyTable::copy(*pair, handle_.side, plan_, output, offset);
        }
        void cancel() { owner_ = nullptr; }
        [[nodiscard]] Result<Transfer> commit()
        {
            if (!owner_) return Status::busy("PTY read preview retired");
            auto *owner = std::exchange(owner_, nullptr);
            auto *pair = owner->resolve(handle_);
            if (!pair) return Status::bad_descriptor("PTY preview endpoint retired");
            if (plan_.consume && (pair->revision != revision_ || pair->revision == RevisionLimit))
                return Status::busy("PTY read preview revision changed or saturated");
            return owner->consume(handle_, *pair, plan_);
        }
      private:
        friend class RawPtyTable;
        PreparedRead(RawPtyTable &owner, Handle handle, std::uint64_t revision, const Plan &planned)
            : owner_(&owner), handle_(handle), revision_(revision), plan_(planned) {}
        RawPtyTable *owner_{};
        Handle handle_{};
        std::uint64_t revision_{};
        Plan plan_{};
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
    // A nonblocking slave read takes what is buffered below VMIN (POSIX, n_tty_read).
    [[nodiscard]] Result<PreparedRead> prepare_read(Handle handle, std::size_t count, bool nonblocking = false,
                                                    bool timer_expired = false)
    {
        auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        if (!count) return PreparedRead{*this, handle, pair->revision, Plan{}};
        auto planned = plan(*pair, handle.side, count, nonblocking, timer_expired);
        if (!planned) return planned.status();
        // Diagnostic saturated revisions cannot authorize a deferred consume.
        if (planned.value().consume && pair->revision == RevisionLimit)
            return Status::overflow("PTY preview revision saturated");
        return PreparedRead{*this, handle, pair->revision, planned.value()};
    }

    // Handles are local to this single nonmovable owner table. The OFD layer
    // retires one open when the final reference of a bound description dies.
    [[nodiscard]] Result<PairHandles> create()
    {
        const auto slot = claim();
        if (!slot) return slot.status();
        auto &pair = pairs_[slot.value()];
        pair.slave.reset(tty::raw_pair_termios(), {});
        pair.master_opens = pair.slave_opens = 1;
        pair.slave_seen = true;
        pair.locked = pair.multiplexed = false;
        return PairHandles{{slot.value(), pair.generation, Side::master}, {slot.value(), pair.generation, Side::slave}};
    }
    // A ptmx-style pair: the master is open once, the slave locked and dormant.
    [[nodiscard]] Result<Handle> create_multiplexed()
    {
        const auto slot = claim();
        if (!slot) return slot.status();
        auto &pair = pairs_[slot.value()];
        pair.slave.reset(tty::linux_pty_termios(), {});
        pair.master_opens = 1;
        pair.slave_opens = 0;
        pair.slave_seen = false;
        pair.locked = pair.multiplexed = true;
        return Handle{slot.value(), pair.generation, Side::master};
    }
    // Admission of one more open of an existing endpoint, checked in order.
    // Masters are opened only by creating their pair; a raw slave cannot be
    // reopened once closed.
    [[nodiscard]] Status admit_open(Handle handle) const
    {
        if (handle.slot >= pairs_.size() || !handle.generation || !known_side(handle.side))
            return Status::bad_descriptor("invalid PTY endpoint");
        const auto *pair = lookup(handle);
        if (!pair) return Status::not_found("PTY pair is gone");
        if (handle.side == Side::master) return Status::invalid_argument("a PTY master opens only with its pair");
        if (!pair->master_opens) return Status::io_error("PTY master is gone");
        if (!pair->multiplexed && !pair->slave_opens) return Status::io_error("raw PTY slave was closed");
        if (pair->locked) return Status::io_error("PTY slave is locked");
        if (pair->slave_opens == max_endpoint_opens) return Status::overflow("PTY slave open count exhausted");
        return {};
    }
    // Infallible after admit_open (which it repeats): one more open.
    Change attach(Handle handle)
    {
        if (!admit_open(handle)) return {};
        auto &pair = pairs_[handle.slot];
        ++pair.slave_opens;
        pair.slave_seen = true;
        advance_revision(pair);
        return observe(handle.slot, pair, true, wake_master_read | wake_master_write);
    }
    // TIOCSPTLCK and TIOCGPTN: ptmx masters only. Relocking refuses new slave opens only.
    [[nodiscard]] Status set_locked(Handle master, bool locked)
    {
        auto *pair = resolve(master);
        if (!pair) return Status::bad_descriptor("invalid PTY endpoint");
        if (master.side != Side::master || !pair->multiplexed)
            return Status::not_terminal("PTY lock needs a multiplexed master");
        pair->locked = locked;
        return {};
    }
    [[nodiscard]] Result<std::uint16_t> index(Handle master) const
    {
        const auto *pair = resolve(master);
        if (!pair) return Status::bad_descriptor("invalid PTY endpoint");
        if (master.side != Side::master || !pair->multiplexed)
            return Status::not_terminal("PTY index needs a multiplexed master");
        return master.slot;
    }
    // The slave terminal of a live pair, or null for a stale generation.
    [[nodiscard]] SlaveTerminal *terminal(std::uint16_t slot, std::uint64_t generation)
    {
        if (slot >= pairs_.size() || !generation) return nullptr;
        auto &pair = pairs_[slot];
        return pair.used && pair.generation == generation ? &pair.slave : nullptr;
    }
    [[nodiscard]] const SlaveTerminal *terminal(std::uint16_t slot, std::uint64_t generation) const
    { return const_cast<RawPtyTable *>(this)->terminal(slot, generation); }
    // Storage of a slot's terminal whether or not the slot is live (outbox drains).
    [[nodiscard]] SlaveTerminal &terminal_storage(std::uint16_t slot) { return pairs_[slot].slave; }
    // Generation of a live pair; 0 for a free slot.
    [[nodiscard]] std::uint64_t live_generation(std::uint16_t slot) const
    { return slot < pairs_.size() && pairs_[slot].used ? pairs_[slot].generation : 0; }
    // Opens of one endpoint (0 for a stale handle).
    [[nodiscard]] std::uint16_t open_count(Handle handle) const
    {
        const auto *pair = lookup(handle);
        return pair ? opens(*pair, handle.side) : 0;
    }
    // Current slave handle of a live ptmx-style pair (a /dev/pts/N lookup).
    [[nodiscard]] Handle slave_handle(std::uint16_t slot) const
    {
        if (slot >= pairs_.size() || !pairs_[slot].used || !pairs_[slot].multiplexed) return {};
        return {slot, pairs_[slot].generation, Side::slave};
    }
    [[nodiscard]] Result<Transfer> read(Handle handle, std::span<std::byte> output, bool nonblocking = false)
    {
        auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        if (output.empty()) return Transfer{0, observe(handle.slot, *pair, false)};
        const auto planned = plan(*pair, handle.side, output.size(), nonblocking);
        if (!planned) return planned.status();
        static_cast<void>(copy(*pair, handle.side, planned.value(), output, 0));
        return consume(handle, *pair, planned.value());
    }
    [[nodiscard]] Result<Transfer> write(Handle handle, std::span<const std::byte> input)
    {
        auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        if (input.empty()) return Transfer{0, observe(handle.slot, *pair, false)};
        auto &terminal = pair->slave;
        std::uint8_t wakes = 0;
        if (handle.side == Side::master)
        {
            // A raw pair's master write fails once its slave closed. A ptmx slave's
            // count includes the master's reference (Linux pty_common_install), so
            // its master writes into the slave ldisc while it is dormant or closed.
            if (!pair->multiplexed && !pair->slave_opens) return Status::io_error("raw PTY slave was closed");
            std::size_t consumed = 0;
            bool readable = false, flush_peer_output = false, start_output = false;
            while (consumed < input.size())
            {
                // The budget counts signal characters, but repeats share one outbox
                // entry: a pass stopped at the budget goes on with the room left.
                const auto budget = terminal.outbox_room();
                const auto received = terminal.ldisc.receive(terminal.termios, input.subspan(consumed), budget, false);
                for (std::size_t index = 0; index < received.signal_count; ++index)
                    static_cast<void>(terminal.owe(terminal.foreground, received.signals[index], true));
                consumed += received.consumed;
                readable = readable || received.became_readable;
                flush_peer_output = flush_peer_output || received.flush_peer_output;
                start_output = start_output || received.start_output;
                if (!received.consumed || received.signal_count < budget) break;
            }
            if (!consumed) return Status::would_block("PTY slave input is full");
            // Slave readers wake on every stored byte in non-canonical mode (Linux
            // __receive_buf; their plan may need fewer than VMIN bytes) and on a
            // completed line or EOF in canonical mode.
            if (readable || (!(terminal.termios.lflag & tty::lflag::icanon) && terminal.ldisc.buffered()))
                wakes |= wake_slave_read;
            if (flush_peer_output && pair->master_input.size)
            {
                pair->master_input.clear();
                wakes |= wake_slave_write;
            }
            if (start_output) wakes |= wake_slave_write;
            if (flush_echo(*pair)) wakes |= wake_master_read;
            advance_revision(*pair);
            return Transfer{consumed, observe(handle.slot, *pair, true, wakes)};
        }
        if (!pair->master_opens) return Status::io_error("raw PTY peer retired");
        // n_tty_write: pending echo leaves first; output stopped by IXON waits.
        static_cast<void>(flush_echo(*pair));
        if (terminal.ldisc.echo_pending() || terminal.ldisc.output_stopped() ||
            pair->master_input.size == QueueCapacity)
            return Status::would_block("raw PTY queue is empty or full");
        std::array<std::byte, 256> chunk{};
        std::size_t consumed = 0;
        while (consumed < input.size() && pair->master_input.size < QueueCapacity)
        {
            const auto room = QueueCapacity - pair->master_input.size;
            const auto moved = terminal.ldisc.process_output(terminal.termios, input.subspan(consumed),
                std::span<std::byte>{chunk.data(), room < chunk.size() ? room : chunk.size()});
            if (!moved.consumed) break;
            static_cast<void>(pair->master_input.write({chunk.data(), moved.produced}));
            consumed += moved.consumed;
        }
        if (!consumed) return Status::would_block("raw PTY queue is empty or full");
        advance_revision(*pair);
        return Transfer{consumed, observe(handle.slot, *pair, true, wake_master_read)};
    }
    // TCFLSH: `input` discards what this endpoint reads, `output` what it wrote
    // that its peer has not read yet.
    [[nodiscard]] Result<Change> flush(Handle handle, bool input, bool output)
    {
        auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        const bool master = handle.side == Side::master;
        std::uint8_t wakes = 0;
        if (master ? input : output)
        {
            pair->master_input.clear();
            wakes |= wake_slave_write;
            if (flush_echo(*pair)) wakes |= wake_master_read;
        }
        if (master ? output : input)
        {
            pair->slave.ldisc.flush_input();
            wakes |= wake_master_write;
        }
        if (wakes) advance_revision(*pair);
        return observe(handle.slot, *pair, wakes != 0, wakes);
    }
    // FIONREAD: queued bytes for a master, N_TTY inq for a slave.
    [[nodiscard]] Result<std::size_t> input_pending(Handle handle) const
    {
        const auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        if (handle.side == Side::master) return pair->master_input.size;
        return pair->slave.ldisc.inq(pair->slave.termios);
    }
    [[nodiscard]] Result<Readiness> readiness(Handle handle) const
    {
        const auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        const auto &terminal = pair->slave;
        Readiness result{stamp(handle.slot, *pair)};
        if (handle.side == Side::master)
        {
            const auto room = terminal.ldisc.room(terminal.termios);
            result.buffered = pair->master_input.size;
            result.outgoing_available = room;
            result.readable = pair->master_input.size || slave_closed(*pair);
            result.writable = (pair->multiplexed || pair->slave_opens) && room;
            result.hangup = slave_closed(*pair);
            result.write_error = !pair->multiplexed && !pair->slave_opens;
            return result;
        }
        const bool gone = !pair->master_opens;
        result.buffered = terminal.ldisc.buffered();
        result.outgoing_available = QueueCapacity - pair->master_input.size;
        result.readable = gone || terminal.ldisc.poll_readable(terminal.termios);
        // A hung-up ptmx slave polls IN|OUT|ERR|HUP; a raw one never OUT.
        result.writable = terminal.hung_up || (!gone && result.outgoing_available && !terminal.ldisc.echo_pending() &&
                                               !terminal.ldisc.output_stopped());
        result.hangup = result.error = result.write_error = gone;
        return result;
    }
    // Why a blocking read of `count` bytes would wait now (`none`: it completes),
    // the VTIME length to arm (0: no timer) and the bytes the reader can see,
    // which a waiting reader compares to restart an interbyte timer.
    struct ReadState
    {
        typename Ldisc::ReadWait wait{Ldisc::ReadWait::none};
        std::uint8_t timer_deciseconds{};
        std::size_t buffered{};
    };
    [[nodiscard]] Result<ReadState> read_state(Handle handle, std::size_t count) const
    {
        const auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        if (handle.side == Side::master)
        {
            const bool ready = !count || pair->master_input.size || slave_closed(*pair);
            return ReadState{ready ? Ldisc::ReadWait::none : Ldisc::ReadWait::input, 0, pair->master_input.size};
        }
        const auto &terminal = pair->slave;
        if (!pair->master_opens) return ReadState{Ldisc::ReadWait::none, 0, terminal.ldisc.buffered()};
        const auto planned = terminal.ldisc.plan_read(terminal.termios, count, false, false);
        return ReadState{planned.wait, planned.timer_deciseconds, terminal.ldisc.buffered()};
    }
    // Releases one open. The last open of a side changes its state: a master
    // drops its queue, queues SIGHUP then SIGCONT for the slave session's
    // leader and clears the session (Linux tty_signal_session_leader), and
    // hangs up a ptmx slave (discarding its input); a raw slave destroys its
    // unread input. A ptmx slave keeps input and session.
    [[nodiscard]] Result<Change> retire(Handle handle)
    {
        auto *pair = resolve(handle);
        if (!pair) return Status::bad_descriptor("invalid raw PTY endpoint");
        if (handle.side == Side::master)
        {
            if (!--pair->master_opens)
            {
                pair->master_input.clear();
                if (const auto leader = pair->slave.session)
                {
                    static_cast<void>(pair->slave.owe(leader, tty::TerminalSignal::hup, false));
                    static_cast<void>(pair->slave.owe(leader, tty::TerminalSignal::cont, false));
                }
                pair->slave.session = pair->slave.foreground = 0;
                if (pair->multiplexed)
                {
                    pair->slave.ldisc.hangup();
                    pair->slave.hung_up = true;
                }
            }
        }
        else if (!--pair->slave_opens && !pair->multiplexed)
            pair->slave.ldisc.hangup();
        advance_revision(*pair);
        if (!pair->master_opens && !pair->slave_opens) release_slot(*pair);
        return observe(handle.slot, *pair, true, wake_all);
    }
    // Read-only diagnostics. No byte contents or mutable provider state escape.
    [[nodiscard]] Audit audit() const
    {
        Audit result{};
        for (const auto &pair : pairs_)
        {
            const auto &ldisc = pair.slave.ldisc;
            result.live_pairs += pair.used ? 1 : 0;
            result.live_endpoints += (pair.master_opens ? 1 : 0) + (pair.slave_opens ? 1 : 0);
            result.buffered_bytes += pair.master_input.size + ldisc.buffered();
            result.master_opens += pair.master_opens;
            result.slave_opens += pair.slave_opens;
            result.dormant_slaves += pair.used && !pair.slave_seen ? 1 : 0;
            result.locked_pairs += pair.used && pair.locked ? 1 : 0;
            const bool slave_input_owned = pair.multiplexed ? pair.master_opens : pair.slave_opens;
            result.cleared_outside_live_bytes = result.cleared_outside_live_bytes &&
                pair.master_input.cleared_outside_live_bytes() && ldisc.cleared_outside_live_bytes() &&
                (pair.master_opens || !pair.master_input.size) && (slave_input_owned || !ldisc.buffered()) &&
                (!ldisc.echo_pending() || !pair.master_opens || pair.master_input.size == QueueCapacity) &&
                (pair.slave_seen || !pair.slave_opens) && (pair.multiplexed || !pair.used || pair.slave_seen) &&
                (pair.multiplexed || !pair.locked) &&
                (!pair.slave.hung_up || (pair.used && pair.multiplexed && !pair.master_opens)) &&
                (pair.used || (!pair.master_opens && !pair.slave_opens && !ldisc.echo_pending() && !pair.locked &&
                               !pair.slave.session && !pair.slave.foreground));
        }
        return result;
    }
};
} // namespace ok::sched::private_pty
