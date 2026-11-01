#pragma once

#include "ok/tty/termios.hpp"

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <span>

// The N_TTY line discipline as a value: input editing, echo, output post
// processing and read planning over fixed buffers. It owns no clock, sends no
// signal and wakes nobody. receive() reports signals, flushes and readiness
// changes; plan_read() reports what a read may copy now or how it must wait
// (including the VTIME timer length); the owner arms waits and timers and moves
// echo and output bytes to their device.
namespace ok::tty
{

// Destination for pending echo: put() takes a prefix and returns how much it took.
template <class Sink>
concept EchoSink = requires(Sink &sink, std::span<const std::byte> bytes) {
    { sink.put(bytes) } -> std::convertible_to<usize>;
};

template <usize ReadCapacity = 4096, usize EchoCapacity = 512>
    requires(std::has_single_bit(ReadCapacity) && ReadCapacity >= 64 && EchoCapacity >= 16)
class NTty final
{
  public:
    static constexpr usize read_capacity = ReadCapacity;
    static constexpr usize echo_capacity = EchoCapacity;
    static constexpr usize max_signals = 8;

    enum class ReadWait : u8
    {
        none,        // copy `copy` bytes now (possibly 0: EOF or an expired/zero timer)
        input,       // wait for input; no timer
        minimum,     // some input, fewer than VMIN bytes; no timer
        interbyte,   // some input, fewer than VMIN; arm (and on new input re-arm) the VTIME timer
        timeout,     // VMIN 0: arm the VTIME timer once from the start of the read
        would_block, // nonblocking and nothing to return: EAGAIN
    };
    struct ReadPlan
    {
        usize copy{}, consume{};
        ReadWait wait{ReadWait::none};
        u8 timer_deciseconds{};
        u64 revision{};
        [[nodiscard]] constexpr u64 timer_nanoseconds() const { return u64{timer_deciseconds} * 100'000'000ULL; }
    };
    struct Input
    {
        usize consumed{}, stored{};
        std::array<TerminalSignal, max_signals> signals{};
        u8 signal_count{};
        bool flush_peer_output{}, became_readable{}, echoed{}, stop_output{}, start_output{}, dropped{};
    };
    struct Output
    {
        usize consumed{}, produced{};
    };

    constexpr NTty() = default;

    // Feeds terminal input. Bytes that need buffer room and find none stop the
    // call (backpressure) unless `drop_when_full`, which drops them and counts an
    // overrun. A signal character beyond `signal_budget` also stops the call.
    constexpr Input receive(const Termios &termios, std::span<const std::byte> input, u8 signal_budget,
                            bool drop_when_full)
    {
        Input result{};
        const bool readable = poll_readable(termios);
        const usize budget = signal_budget < max_signals ? signal_budget : max_signals;
        const u64 echoed = echo_written_;
        for (const std::byte value : input)
        {
            if (!receive_byte(termios, static_cast<u8>(value), budget, drop_when_full, result))
                break;
            ++result.consumed;
        }
        if (result.consumed)
            ++revision_;
        result.echoed = echo_written_ != echoed;
        result.became_readable = !readable && poll_readable(termios);
        return result;
    }

    // OPOST output of program data into `out`. A byte is consumed only when its
    // whole expansion fits, so CR LF is never split.
    constexpr Output process_output(const Termios &termios, std::span<const std::byte> in, std::span<std::byte> out)
    {
        Output result{};
        while (result.consumed < in.size())
        {
            const int produced = output_char(termios, static_cast<u8>(in[result.consumed]),
                                             out.size() - result.produced,
                                             [&](u8 byte) { out[result.produced++] = std::byte{byte}; });
            if (produced < 0)
                break;
            ++result.consumed;
        }
        return result;
    }

    constexpr usize take_echo(std::span<std::byte> out)
    {
        usize count = 0;
        for (; count < out.size() && echo_size_; ++count)
        {
            out[count] = echo_[echo_head_];
            erase(echo_[echo_head_]);
            echo_head_ = (echo_head_ + 1) % EchoCapacity;
            --echo_size_;
        }
        if (!echo_size_)
            echo_head_ = 0;
        return count;
    }

    template <EchoSink Sink>
    constexpr usize drain_echo(Sink &sink)
    {
        usize total = 0;
        while (echo_size_)
        {
            const usize run = echo_head_ + echo_size_ <= EchoCapacity ? echo_size_ : EchoCapacity - echo_head_;
            usize taken = sink.put(std::span<const std::byte>{echo_.data() + echo_head_, run});
            taken = taken < run ? taken : run;
            for (usize index = 0; index < taken; ++index)
                erase(echo_[echo_head_ + index]);
            echo_head_ = (echo_head_ + taken) % EchoCapacity;
            echo_size_ -= taken;
            total += taken;
            if (taken < run)
                break;
        }
        if (!echo_size_)
            echo_head_ = 0;
        return total;
    }

    [[nodiscard]] constexpr usize echo_pending() const { return echo_size_; }

    [[nodiscard]] constexpr ReadPlan plan_read(const Termios &termios, usize count, bool nonblocking,
                                               bool timer_expired) const
    {
        ReadPlan plan{};
        plan.revision = revision_;
        if (!count)
            return plan;
        const usize available = read_head_ - read_tail_;
        if (termios.lflag & lflag::icanon)
        {
            if (canon_head_ == read_tail_)
            {
                plan.wait = nonblocking ? ReadWait::would_block : ReadWait::input;
                return plan;
            }
            const usize span = canon_head_ - read_tail_ < count ? canon_head_ - read_tail_ : count;
            for (usize offset = 0; offset < span; ++offset)
                if (line_end(read_tail_ + offset))
                {
                    // A terminator stored as 0 is the EOF mark: consumed, never copied.
                    plan.consume = offset + 1;
                    plan.copy = at(read_tail_ + offset) ? offset + 1 : offset;
                    return plan;
                }
            plan.copy = plan.consume = span;
            // Filling the buffer exactly also skips an EOF mark that follows (canon_skip_eof).
            const usize next = read_tail_ + span;
            if (span == count && next != canon_head_ && line_end(next) && !at(next))
                ++plan.consume;
            return plan;
        }
        const usize vmin = termios[Cc::min], vtime = termios[Cc::time];
        const usize take = available < count ? available : count;
        if (nonblocking)
        {
            if (!available && (vmin || vtime))
                plan.wait = ReadWait::would_block;
            plan.copy = plan.consume = take;
            return plan;
        }
        if (!vmin && !vtime)
        {
            plan.copy = plan.consume = take;
            return plan;
        }
        if (!vmin)
        {
            if (available || timer_expired)
                plan.copy = plan.consume = take;
            else
            {
                plan.wait = ReadWait::timeout;
                plan.timer_deciseconds = static_cast<u8>(vtime);
            }
            return plan;
        }
        const usize needed = vmin < count ? vmin : count;
        if (available >= needed || (available && vtime && timer_expired))
        {
            plan.copy = plan.consume = take;
            return plan;
        }
        if (!available)
            plan.wait = ReadWait::input;
        else if (!vtime)
            plan.wait = ReadWait::minimum;
        else
        {
            plan.wait = ReadWait::interbyte;
            plan.timer_deciseconds = static_cast<u8>(vtime);
        }
        return plan;
    }

    // Copies the planned bytes without consuming them.
    constexpr usize preview(const ReadPlan &plan, std::span<std::byte> out) const
    {
        const usize count = plan.copy < out.size() ? plan.copy : out.size();
        for (usize index = 0; index < count; ++index)
            out[index] = std::byte{at(read_tail_ + index)};
        return count;
    }

    // Consumes a plan; refused when the state changed since the plan was made.
    constexpr bool consume(const ReadPlan &plan)
    {
        if (plan.revision != revision_ || plan.consume > read_head_ - read_tail_)
            return false;
        for (usize index = 0; index < plan.consume; ++index)
        {
            set_line_end(read_tail_, false);
            erase(read_[mask(read_tail_)]);
            ++read_tail_;
        }
        if (canon_head_ - read_tail_ > read_head_ - read_tail_)
            canon_head_ = read_tail_; // Non-canonical reads may pass a stale canonical head.
        if (plan.consume)
            ++revision_;
        return true;
    }

    // n_tty_set_termios: an ICANON change pushes pending input as one line or
    // makes it all readable. Returns whether the terminal became readable.
    constexpr bool set_termios(const Termios &old, const Termios &next)
    {
        const bool readable = poll_readable(old);
        if ((old.lflag ^ next.lflag) & lflag::icanon)
        {
            line_end_ = {};
            if (!(next.lflag & lflag::icanon) || read_head_ == read_tail_)
                canon_head_ = read_tail_;
            else
            {
                set_line_end(read_head_ - 1, true);
                canon_head_ = read_head_;
            }
            lnext_ = false;
        }
        if ((old.iflag & iflag::ixon) && !(next.iflag & iflag::ixon))
            output_stopped_ = false;
        ++revision_;
        return !readable && poll_readable(next);
    }

    // TCIFLUSH / TCSETSF / ISIG flush: discards (and clears) unread input.
    constexpr void flush_input()
    {
        for (; read_tail_ != read_head_; ++read_tail_)
            erase(read_[mask(read_tail_)]);
        line_end_ = {};
        read_head_ = read_tail_ = canon_head_ = 0;
        lnext_ = false;
        ++revision_;
    }

    // Hangup: input and echo are discarded and cleared, columns and flow reset.
    constexpr void hangup()
    {
        flush_input();
        clear_echo();
        column_ = canon_column_ = 0;
        output_stopped_ = false;
    }

    [[nodiscard]] constexpr bool poll_readable(const Termios &termios) const
    {
        if (termios.lflag & lflag::icanon)
            return canon_head_ != read_tail_;
        const usize vmin = termios[Cc::min];
        const usize amount = !termios[Cc::time] && vmin ? vmin : 1;
        return read_head_ - read_tail_ >= amount;
    }

    // FIONREAD: canonical counts complete lines without their EOF marks.
    [[nodiscard]] constexpr usize inq(const Termios &termios) const
    {
        if (!(termios.lflag & lflag::icanon))
            return read_head_ - read_tail_;
        usize count = canon_head_ - read_tail_;
        for (usize index = read_tail_; index != canon_head_; ++index)
            if (line_end(index) && !at(index))
                --count;
        return count;
    }

    // Bytes a writer may push now (master writability). Canonical input keeps
    // one slot for the terminator but always accepts editing of a partial line.
    [[nodiscard]] constexpr usize room(const Termios &termios) const
    {
        const usize used = read_head_ - read_tail_;
        if (!(termios.lflag & lflag::icanon))
            return ReadCapacity - used;
        if (canon_head_ == read_tail_)
            return used + 1 < ReadCapacity ? ReadCapacity - 1 - used : 1;
        return used + 1 < ReadCapacity ? ReadCapacity - 1 - used : 0;
    }

    [[nodiscard]] constexpr usize buffered() const { return read_head_ - read_tail_; }
    [[nodiscard]] constexpr bool output_stopped() const { return output_stopped_; }
    [[nodiscard]] constexpr u32 column() const { return column_; }
    [[nodiscard]] constexpr u64 revision() const { return revision_; }
    [[nodiscard]] constexpr u64 received() const { return received_; }
    [[nodiscard]] constexpr u32 echo_discarded() const { return echo_discarded_; }
    [[nodiscard]] constexpr u32 overruns() const { return overruns_; }

    // Diagnostic: every byte outside the unread input and pending echo is zero.
    [[nodiscard]] constexpr bool cleared_outside_live_bytes() const
    {
        for (usize index = 0; index < ReadCapacity; ++index)
            if (((index - read_tail_) & (ReadCapacity - 1)) >= read_head_ - read_tail_ && read_[index])
                return false;
        for (usize index = 0; index < EchoCapacity; ++index)
            if ((index + EchoCapacity - echo_head_) % EchoCapacity >= echo_size_ && echo_[index] != std::byte{})
                return false;
        return true;
    }

  private:
    enum class Store : u8 { stored, dropped, full };

    [[nodiscard]] static constexpr usize mask(usize index) { return index & (ReadCapacity - 1); }
    [[nodiscard]] static constexpr bool control(u8 value) { return value < 0x20 || value == 0x7f; }
    [[nodiscard]] static constexpr bool continuation(const Termios &termios, u8 value)
    {
        return (termios.iflag & iflag::iutf8) && (value & 0xc0) == 0x80;
    }
    // Linux ctype isalnum over Latin-1, used by WERASE.
    [[nodiscard]] static constexpr bool word(u8 value)
    {
        return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
               value == '_' || (value >= 0xc0 && value != 0xd7 && value != 0xf7);
    }
    // _POSIX_VDISABLE is 0: an unset slot never matches input.
    [[nodiscard]] static constexpr bool is(const Termios &termios, Cc slot, u8 value)
    {
        return termios[slot] && termios[slot] == value;
    }
    static constexpr void erase(u8 &byte)
    {
        if consteval
        {
            byte = 0;
        }
        else
        {
            *reinterpret_cast<volatile u8 *>(&byte) = 0;
        }
    }
    static constexpr void erase(std::byte &byte)
    {
        if consteval
        {
            byte = std::byte{};
        }
        else
        {
            *reinterpret_cast<volatile u8 *>(&byte) = 0;
        }
    }

    [[nodiscard]] constexpr u8 at(usize index) const { return read_[mask(index)]; }
    [[nodiscard]] constexpr bool line_end(usize index) const
    {
        return (line_end_[mask(index) / 8] >> (mask(index) % 8)) & 1U;
    }
    constexpr void set_line_end(usize index, bool value)
    {
        const u8 bit = static_cast<u8>(1U << (mask(index) % 8));
        auto &cell = line_end_[mask(index) / 8];
        cell = value ? static_cast<u8>(cell | bit) : static_cast<u8>(cell & ~bit);
    }

    // The n_tty char_map: which bytes take the special path under `termios`.
    [[nodiscard]] static constexpr bool special(const Termios &termios, u8 value)
    {
        if (!value)
            return false;
        const u32 input = termios.iflag, local = termios.lflag;
        if (value == '\r' && (input & (iflag::igncr | iflag::icrnl)))
            return true;
        if (value == '\n' && (input & iflag::inlcr))
            return true;
        if (local & lflag::icanon)
        {
            if (value == '\n' || is(termios, Cc::erase, value) || is(termios, Cc::kill, value) ||
                is(termios, Cc::eof, value) || is(termios, Cc::eol, value))
                return true;
            if ((local & lflag::iexten) && (is(termios, Cc::werase, value) || is(termios, Cc::lnext, value) ||
                                            is(termios, Cc::eol2, value) ||
                                            ((local & lflag::echo) && is(termios, Cc::reprint, value))))
                return true;
        }
        if ((input & iflag::ixon) && (is(termios, Cc::start, value) || is(termios, Cc::stop, value)))
            return true;
        return (local & lflag::isig) &&
               (is(termios, Cc::intr, value) || is(termios, Cc::quit, value) || is(termios, Cc::susp, value));
    }

    // Room for one byte. An ordinary canonical byte keeps a slot for the
    // terminator; without a complete line it is dropped rather than blocking,
    // so erase, kill and newline still reach a full line.
    [[nodiscard]] constexpr Store room_for(const Termios &termios, bool terminator) const
    {
        const usize used = read_head_ - read_tail_;
        if (!(termios.lflag & lflag::icanon))
            return used < ReadCapacity ? Store::stored : Store::full;
        if (terminator ? used < ReadCapacity : used + 1 < ReadCapacity)
            return Store::stored;
        return canon_head_ == read_tail_ ? Store::dropped : Store::full;
    }

    constexpr void put(u8 value, bool terminator)
    {
        read_[mask(read_head_)] = value;
        set_line_end(read_head_, terminator);
        ++read_head_;
        ++received_;
        if (terminator)
            canon_head_ = read_head_;
    }

    // Echo is generated through the output path at once, so columns are exact.
    constexpr void push_echo(u8 value)
    {
        if (echo_size_ == EchoCapacity)
        {
            echo_[echo_head_] = std::byte{};
            echo_head_ = (echo_head_ + 1) % EchoCapacity;
            --echo_size_;
            ++echo_discarded_;
        }
        echo_[(echo_head_ + echo_size_) % EchoCapacity] = std::byte{value};
        ++echo_size_;
        ++echo_written_;
    }
    constexpr void clear_echo()
    {
        for (auto &byte : echo_)
            erase(byte);
        echo_head_ = echo_size_ = 0;
    }
    constexpr void echo_raw(const Termios &termios, u8 value)
    {
        output_char(termios, value, EchoCapacity, [&](u8 byte) { push_echo(byte); });
    }
    constexpr void echo_char(const Termios &termios, u8 value)
    {
        if ((termios.lflag & lflag::echoctl) && control(value) && value != '\t')
        {
            push_echo('^');
            push_echo(static_cast<u8>(value ^ 0x40));
            column_ += 2;
            return;
        }
        echo_raw(termios, value);
    }
    constexpr void echo_visible(const Termios &termios, u8 value)
    {
        if (canon_head_ == read_head_)
            canon_column_ = column_;
        echo_char(termios, value);
    }

    // do_output_char: the produced byte count, or -1 when `space` is too small.
    template <class Put>
    constexpr int output_char(const Termios &termios, u8 value, usize space, Put &&emit)
    {
        if (!space)
            return -1;
        if (!(termios.oflag & oflag::opost))
        {
            emit(value);
            return 1;
        }
        const u32 out = termios.oflag;
        switch (value)
        {
        case '\n':
            if (out & oflag::onlret)
                column_ = 0;
            if (out & oflag::onlcr)
            {
                if (space < 2)
                    return -1;
                canon_column_ = column_ = 0;
                emit('\r');
                emit('\n');
                return 2;
            }
            canon_column_ = column_;
            break;
        case '\r':
            if ((out & oflag::onocr) && !column_)
                return 0;
            if (out & oflag::ocrnl)
            {
                value = '\n';
                if (out & oflag::onlret)
                    canon_column_ = column_ = 0;
                break;
            }
            canon_column_ = column_ = 0;
            break;
        case '\t': {
            const u32 spaces = 8 - (column_ & 7);
            if ((out & oflag::tabdly) == oflag::xtabs)
            {
                if (space < spaces)
                    return -1;
                column_ += spaces;
                for (u32 index = 0; index < spaces; ++index)
                    emit(' ');
                return static_cast<int>(spaces);
            }
            column_ += spaces;
            break;
        }
        case '\b':
            if (column_)
                --column_;
            break;
        default:
            if (!control(value) && !continuation(termios, value))
                ++column_;
            break;
        }
        emit(value);
        return 1;
    }

    constexpr void echo_erase_tab(u32 columns, bool after_tab)
    {
        columns &= 7;
        if (!after_tab)
            columns += canon_column_;
        for (u32 count = 8 - (columns & 7); count; --count)
        {
            push_echo('\b');
            if (column_)
                --column_;
        }
    }

    // n_tty eraser() for VERASE, VWERASE and VKILL (ECHOPRT is not acted on).
    constexpr void eraser(const Termios &termios, u8 value)
    {
        if (read_head_ == canon_head_)
            return;
        enum class Kind : u8 { erase, word, kill } kind = Kind::kill;
        const u32 local = termios.lflag;
        const bool echo = local & lflag::echo;
        if (is(termios, Cc::erase, value))
            kind = Kind::erase;
        else if (is(termios, Cc::werase, value))
            kind = Kind::word;
        else if (!echo || !(local & lflag::echok) || !(local & lflag::echoke) || !(local & lflag::echoe))
        {
            while (read_head_ != canon_head_)
                erase(read_[mask(--read_head_)]);
            if (echo)
            {
                echo_char(termios, termios[Cc::kill]);
                if (local & lflag::echok)
                    echo_raw(termios, '\n');
            }
            return;
        }
        usize alnums = 0;
        while (read_head_ != canon_head_)
        {
            usize head = read_head_;
            u8 last{};
            do
                last = at(--head);
            while (continuation(termios, last) && head != canon_head_);
            if (continuation(termios, last))
                break; // Never erase part of a character.
            if (kind == Kind::word)
            {
                if (word(last))
                    ++alnums;
                else if (alnums)
                    break;
            }
            while (read_head_ != head)
                erase(read_[mask(--read_head_)]);
            if (echo)
            {
                if (kind == Kind::erase && !(local & lflag::echoe))
                    echo_char(termios, termios[Cc::erase]);
                else if (last == '\t')
                {
                    u32 columns = 0;
                    bool after_tab = false;
                    for (usize tail = read_head_; tail != canon_head_;)
                    {
                        const u8 before = at(--tail);
                        if (before == '\t')
                        {
                            after_tab = true;
                            break;
                        }
                        if (control(before))
                            columns += (local & lflag::echoctl) ? 2 : 0;
                        else if (!continuation(termios, before))
                            ++columns;
                    }
                    echo_erase_tab(columns, after_tab);
                }
                else
                {
                    const usize rubouts = control(last) ? ((local & lflag::echoctl) ? 2 : 0) : 1;
                    for (usize index = 0; index < rubouts; ++index)
                    {
                        echo_raw(termios, '\b');
                        echo_raw(termios, ' ');
                        echo_raw(termios, '\b');
                    }
                }
            }
            if (kind == Kind::erase)
                break;
        }
    }

    // How a stored byte is echoed under ECHO: as typed (ECHOCTL applies), as a
    // plain newline (non-canonical special path), as a line end (NL also under
    // ECHONL) or not at all (the EOF mark).
    enum class Echo : u8 { visible, newline_raw, line, none };

    // Stores one byte with its echo; false means backpressure.
    constexpr bool store(const Termios &termios, u8 value, bool terminator, bool drop_when_full, Input &result,
                         Echo mode)
    {
        const Store room = room_for(termios, terminator);
        if (room == Store::full && !drop_when_full)
            return false;
        if (room != Store::stored)
        {
            if (room == Store::full)
                ++overruns_;
            else if (termios.iflag & iflag::imaxbel)
                echo_raw(termios, '\a');
            result.dropped = true;
            return true;
        }
        const u32 local = termios.lflag;
        if (mode == Echo::line ? (local & (lflag::echo | lflag::echonl)) : mode != Echo::none && (local & lflag::echo))
        {
            if (mode == Echo::line || (mode == Echo::newline_raw && value == '\n'))
                echo_raw(termios, '\n');
            else
                echo_visible(termios, value);
        }
        put(value, terminator);
        ++result.stored;
        return true;
    }

    constexpr bool receive_byte(const Termios &termios, u8 value, usize budget, bool drop_when_full, Input &result)
    {
        const u32 input = termios.iflag, local = termios.lflag;
        if (input & iflag::istrip)
            value &= 0x7f;
        if (lnext_)
        {
            if (!store(termios, value, false, drop_when_full, result, Echo::visible))
                return false;
            lnext_ = false;
            return true;
        }
        if (!special(termios, value))
            return store(termios, value, false, drop_when_full, result, Echo::visible);
        if ((input & iflag::ixon) && (is(termios, Cc::start, value) || is(termios, Cc::stop, value)))
        {
            output_stopped_ = !is(termios, Cc::start, value);
            (output_stopped_ ? result.stop_output : result.start_output) = true;
            return true;
        }
        if (local & lflag::isig)
        {
            const TerminalSignal signal = is(termios, Cc::intr, value)   ? TerminalSignal::interrupt
                                          : is(termios, Cc::quit, value) ? TerminalSignal::quit
                                          : is(termios, Cc::susp, value) ? TerminalSignal::suspend
                                                                         : TerminalSignal::hup;
            if (signal != TerminalSignal::hup)
            {
                if (result.signal_count >= budget)
                    return false;
                result.signals[result.signal_count++] = signal;
                if (!(local & lflag::noflsh))
                {
                    flush_input();
                    clear_echo();
                    result.flush_peer_output = true;
                }
                if ((input & iflag::ixon) && output_stopped_)
                {
                    output_stopped_ = false;
                    result.start_output = true;
                }
                if (local & lflag::echo)
                    echo_char(termios, value);
                return true;
            }
        }
        if (value == '\r')
        {
            if (input & iflag::igncr)
                return true;
            if (input & iflag::icrnl)
                value = '\n';
        }
        else if (value == '\n' && (input & iflag::inlcr))
            value = '\r';
        if (local & lflag::icanon)
        {
            const bool echo = local & lflag::echo, extended = local & lflag::iexten;
            if (is(termios, Cc::erase, value) || is(termios, Cc::kill, value) ||
                (extended && is(termios, Cc::werase, value)))
            {
                eraser(termios, value);
                return true;
            }
            if (extended && is(termios, Cc::lnext, value))
            {
                lnext_ = true;
                if (echo && (local & lflag::echoctl))
                {
                    echo_raw(termios, '^');
                    echo_raw(termios, '\b');
                }
                return true;
            }
            if (extended && echo && is(termios, Cc::reprint, value))
            {
                echo_char(termios, value);
                echo_raw(termios, '\n');
                for (usize index = canon_head_; index != read_head_; ++index)
                    echo_char(termios, at(index));
                return true;
            }
            if (value == '\n')
                return store(termios, '\n', true, drop_when_full, result, Echo::line);
            if (is(termios, Cc::eof, value))
                return store(termios, 0, true, drop_when_full, result, Echo::none); // EOF mark, never copied.
            if (is(termios, Cc::eol, value) || (extended && is(termios, Cc::eol2, value)))
                return store(termios, value, true, drop_when_full, result, Echo::visible);
        }
        return store(termios, value, false, drop_when_full, result, Echo::newline_raw);
    }

    std::array<u8, ReadCapacity> read_{};
    std::array<u8, ReadCapacity / 8> line_end_{};
    std::array<std::byte, EchoCapacity> echo_{};
    usize read_head_{}, read_tail_{}, canon_head_{};
    usize echo_head_{}, echo_size_{};
    u32 column_{}, canon_column_{};
    bool lnext_{}, output_stopped_{};
    u64 revision_{}, received_{}, echo_written_{};
    u32 echo_discarded_{}, overruns_{};
};

} // namespace ok::tty
