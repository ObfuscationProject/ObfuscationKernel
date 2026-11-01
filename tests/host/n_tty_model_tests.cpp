// Hosted model of the N_TTY value template (include/ok/tty/n_tty.hpp): directed
// Linux n_tty cases plus a seeded differential run against an independent
// string-based reference. No terminal object, runtime, guest or hardware runs.
#include "ok/tty/n_tty.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <random>
#include <string>
#include <vector>

using namespace ok;
using namespace ok::tty;

namespace
{
usize checks{};
#define CHECK(expression)                                                                                    \
    do                                                                                                       \
    {                                                                                                        \
        ++checks;                                                                                            \
        if (!(expression))                                                                                   \
        {                                                                                                    \
            std::fprintf(stderr, "n_tty check failed at line %d: %s\n", __LINE__, #expression);              \
            return false;                                                                                    \
        }                                                                                                    \
    } while (false)

using Tty = NTty<>;
static_assert(Tty::read_capacity == 4096 && Tty::echo_capacity == 512);

// Constant evaluation: construction, input, planning and consumption.
constexpr bool constant_model()
{
    NTty<64, 16> tty{};
    const Termios termios = linux_std_termios();
    const std::byte input[]{std::byte{'h'}, std::byte{'i'}, std::byte{'\r'}, std::byte{0x04}};
    const auto received = tty.receive(termios, input, 1, false);
    const auto plan = tty.plan_read(termios, 16, false, false);
    std::array<std::byte, 16> out{};
    const usize copied = tty.preview(plan, out);
    const bool consumed = tty.consume(plan);
    const auto eof = tty.plan_read(termios, 16, false, false);
    return received.consumed == 4 && received.became_readable && plan.copy == 3 && copied == 3 &&
           out[2] == std::byte{'\n'} && consumed && eof.wait == NTty<64, 16>::ReadWait::none && eof.copy == 0 &&
           eof.consume == 1 && tty.echo_pending() == 4; // "hi\r\n"
}
static_assert(constant_model());
static_assert(NTty<64, 16>{}.plan_read(raw_pair_termios(), 1, true, false).wait ==
              NTty<64, 16>::ReadWait::would_block);
constinit Tty constant_initialized{};

std::span<const std::byte> bytes(const std::string &text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

template <class T> std::string echo_of(T &tty)
{
    std::string result(tty.echo_pending(), '\0');
    tty.take_echo({reinterpret_cast<std::byte *>(result.data()), result.size()});
    return result;
}

template <class T> std::string read_now(T &tty, const Termios &termios, usize count)
{
    const auto plan = tty.plan_read(termios, count, true, false);
    std::string result(plan.copy, '\0');
    tty.preview(plan, {reinterpret_cast<std::byte *>(result.data()), result.size()});
    if (!tty.consume(plan))
        return "<stale>";
    return result;
}

Termios cooked()
{
    Termios termios = linux_std_termios();
    termios.oflag = oflag::opost | oflag::onlcr;
    return termios;
}

bool defaults()
{
    const Termios standard = linux_std_termios();
    CHECK(standard.iflag == (iflag::icrnl | iflag::ixon) && standard.oflag == (oflag::opost | oflag::onlcr));
    CHECK(standard.cflag == (cflag::b38400 | cflag::cs8 | cflag::cread | cflag::hupcl));
    CHECK(standard.lflag == (lflag::isig | lflag::icanon | lflag::echo | lflag::echoe | lflag::echok |
                             lflag::echoctl | lflag::echoke | lflag::iexten));
    CHECK(standard[Cc::intr] == 3 && standard[Cc::quit] == 0x1c && standard[Cc::erase] == 0x7f &&
          standard[Cc::kill] == 0x15 && standard[Cc::eof] == 4 && standard[Cc::time] == 0 &&
          standard[Cc::min] == 1 && standard[Cc::susp] == 0x1a && standard[Cc::werase] == 0x17 &&
          standard[Cc::lnext] == 0x16 && standard[Cc::reprint] == 0x12 && standard[Cc::eol] == 0);
    // /dev/ptmx pairs: tty_std_termios without HUPCL, as Linux unix98_pty_init.
    const Termios pty = linux_pty_termios();
    CHECK(pty.cflag == (cflag::b38400 | cflag::cs8 | cflag::cread) && output_baud(pty) == 38400);
    CHECK(pty.iflag == standard.iflag && pty.oflag == standard.oflag && pty.lflag == standard.lflag &&
          pty.cc == standard.cc && pty.line == 0);
    const Termios raw = raw_pair_termios();
    CHECK(raw.iflag == 0 && raw.oflag == oflag::onlcr && raw.cflag == standard.cflag);
    CHECK(raw.lflag == (lflag::echoe | lflag::echok | lflag::echoctl | lflag::echoke));
    CHECK(raw[Cc::min] == 1 && raw[Cc::time] == 0 && raw.cc == standard.cc);
    const Termios console = console_compat_termios();
    CHECK(console.iflag == iflag::icrnl && console.oflag == 0 && console.lflag == 0);
    CHECK(console.cflag == (cflag::b115200 | cflag::cs8 | cflag::cread | cflag::clocal));
    CHECK(output_baud(console) == 115200 && input_baud(console) == 115200 && output_baud(standard) == 38400);
    CHECK(console_window() == (WinSize{24, 80, 0, 0}));
    // The console default reproduces the old path: CR becomes LF, no echo, byte at a time.
    Tty tty{};
    const auto received = tty.receive(console, bytes("a\rb"), 0, true);
    CHECK(received.consumed == 3 && tty.echo_pending() == 0 && read_now(tty, console, 1) == "a");
    CHECK(read_now(tty, console, 8) == "\nb");
    std::array<std::byte, 8> out{};
    const auto written = tty.process_output(console, bytes("x\ny"), out);
    CHECK(written.produced == 3 && out[1] == std::byte{'\n'});
    return true;
}

bool canonical_lines_and_eof()
{
    const Termios termios = cooked();
    Tty tty{};
    CHECK(tty.plan_read(termios, 8, false, false).wait == Tty::ReadWait::input);
    CHECK(tty.plan_read(termios, 8, true, false).wait == Tty::ReadWait::would_block);
    auto received = tty.receive(termios, bytes("ab"), 1, false);
    CHECK(!received.became_readable && !tty.poll_readable(termios) && tty.inq(termios) == 0);
    CHECK(echo_of(tty) == "ab");
    received = tty.receive(termios, bytes("c\rde\n"), 1, false);
    CHECK(received.became_readable && tty.poll_readable(termios) && tty.inq(termios) == 7);
    CHECK(echo_of(tty) == "c\r\nde\r\n");
    // One line per read, partial lines continue.
    CHECK(read_now(tty, termios, 2) == "ab" && read_now(tty, termios, 8) == "c\n");
    CHECK(read_now(tty, termios, 8) == "de\n" && !tty.poll_readable(termios));
    // VEOF: at line start reads 0, otherwise pushes the line without a terminator.
    tty.receive(termios, bytes("xy\x04\x04"), 1, false);
    CHECK(echo_of(tty) == "xy" && tty.inq(termios) == 2);
    CHECK(read_now(tty, termios, 8) == "xy");
    auto plan = tty.plan_read(termios, 8, false, false);
    CHECK(plan.wait == Tty::ReadWait::none && plan.copy == 0 && plan.consume == 1 && tty.consume(plan));
    // Filling the buffer exactly before an EOF mark also eats the mark.
    tty.receive(termios, bytes("pq\x04z\n"), 1, false);
    CHECK(read_now(tty, termios, 2) == "pq" && read_now(tty, termios, 8) == "z\n");
    // VEOL and VEOL2 terminate and are copied.
    Termios eol = termios;
    eol[Cc::eol] = ';';
    eol[Cc::eol2] = '|';
    tty.receive(eol, bytes("a;b|"), 1, false);
    CHECK(read_now(tty, eol, 8) == "a;" && read_now(tty, eol, 8) == "b|");
    // A stale plan is refused.
    tty.receive(termios, bytes("k\n"), 1, false);
    plan = tty.plan_read(termios, 8, false, false);
    tty.receive(termios, bytes("m"), 1, false);
    CHECK(!tty.consume(plan));
    CHECK(read_now(tty, termios, 8) == "k\n");
    return true;
}

bool erase_kill_werase_echo()
{
    Termios termios = cooked();
    Tty tty{};
    tty.receive(termios, bytes("abc\x7f"), 1, false);
    CHECK(echo_of(tty) == "abc\b \b");
    tty.receive(termios, bytes("\x01\x7f"), 1, false); // ^A takes two columns under ECHOCTL
    CHECK(echo_of(tty) == "^A\b \b\b \b");
    tty.receive(termios, bytes("\x15"), 1, false); // ECHOKE: visual line erase
    CHECK(echo_of(tty) == "\b \b\b \b" && tty.buffered() == 0);
    tty.receive(termios, bytes("\x7f\x15"), 1, false); // nothing to erase: no echo
    CHECK(echo_of(tty) == "");
    tty.receive(termios, bytes("one two  \x17"), 1, false);
    CHECK(echo_of(tty) == "one two  \b \b\b \b\b \b\b \b\b \b");
    tty.receive(termios, bytes("\n"), 1, false);
    CHECK(read_now(tty, termios, 16) == "one \n");
    echo_of(tty);
    // Without ECHOKE the kill character is echoed, then NL under ECHOK.
    termios.lflag &= ~lflag::echoke;
    tty.receive(termios, bytes("xy\x15"), 1, false);
    CHECK(echo_of(tty) == "xy^U\r\n" && tty.buffered() == 0);
    // Without ECHOE an erase echoes the erase character.
    termios.lflag &= ~lflag::echoe;
    tty.receive(termios, bytes("z\x7f"), 1, false);
    CHECK(echo_of(tty) == "z^?" && tty.buffered() == 0);
    // Tab erase backs up to the previous tab stop measured from the line start,
    // whose column is where the cursor was when the line began.
    termios = cooked();
    Tty fresh{};
    fresh.receive(termios, bytes("ab\t\x7f"), 1, false);
    CHECK(echo_of(fresh) == "ab\t\b\b\b\b\b\b" && fresh.buffered() == 2);
    fresh.receive(termios, bytes("\tc\t\x7f"), 1, false); // after a tab: normal tab spacing
    CHECK(echo_of(fresh) == "\tc\t\b\b\b\b\b\b\b");
    tty.receive(termios, bytes("ab\t\x7f"), 1, false); // line starts at column 3 after "z^?"
    CHECK(echo_of(tty) == "ab\t\b\b\b");
    tty.receive(termios, bytes("\x15"), 1, false);
    echo_of(tty);
    // ECHONL echoes NL without ECHO.
    termios.lflag = (termios.lflag & ~lflag::echo) | lflag::echonl;
    tty.receive(termios, bytes("s\n"), 1, false);
    CHECK(echo_of(tty) == "\r\n" && read_now(tty, termios, 8) == "s\n");
    return true;
}

bool lnext_and_reprint()
{
    const Termios termios = cooked();
    Tty tty{};
    tty.receive(termios, bytes("a\x16\x03" "b\x16\x7f"), 1, false);
    CHECK(echo_of(tty) == "a^\b^Cb^\b^?" && tty.buffered() == 4);
    tty.receive(termios, bytes("\x12"), 1, false);
    CHECK(echo_of(tty) == "^R\r\na^Cb^?");
    tty.receive(termios, bytes("\n"), 1, false);
    CHECK(read_now(tty, termios, 8) == std::string("a\x03" "b\x7f\n"));
    echo_of(tty);
    // An input flush (TCIFLUSH, TCSETSF) keeps a pending LNEXT, as Linux
    // reset_buffer_flags does; a hangup clears it.
    tty.receive(termios, bytes("\x16"), 1, false);
    tty.flush_input();
    tty.receive(termios, bytes("\x7f\n"), 1, false);
    CHECK(read_now(tty, termios, 8) == "\x7f\n");
    echo_of(tty);
    tty.receive(termios, bytes("\x16"), 1, false);
    tty.hangup();
    tty.receive(termios, bytes("\x7f\n"), 1, false);
    CHECK(read_now(tty, termios, 8) == "\n");
    return true;
}

bool utf8_erase()
{
    Termios termios = cooked();
    termios.iflag |= iflag::iutf8;
    Tty tty{};
    tty.receive(termios, bytes("a\xc3\xa9\x7f"), 1, false); // "aé" then erase
    CHECK(echo_of(tty) == "a\xc3\xa9\b \b" && tty.buffered() == 1);
    // Word erase: Latin-1 letters (here the UTF-8 lead bytes) count as word
    // characters, as with the kernel's isalnum, so "a€€" is one word.
    tty.receive(termios, bytes("\xe2\x82\xac\xe2\x82\xac\x17"), 1, false);
    CHECK(echo_of(tty) == "\xe2\x82\xac\xe2\x82\xac\b \b\b \b\b \b" && tty.buffered() == 0);
    CHECK(tty.column() == 0);
    // Without IUTF8 one erase removes one byte.
    termios.iflag &= ~iflag::iutf8;
    tty.receive(termios, bytes("\xc3\xa9\x7f\n"), 1, false);
    echo_of(tty);
    CHECK(read_now(tty, termios, 8) == "\xc3\n");
    return true;
}

bool isig_and_flow()
{
    Termios termios = cooked();
    Tty tty{};
    auto received = tty.receive(termios, bytes("partial\x03"), 4, false);
    CHECK(received.signal_count == 1 && received.signals[0] == TerminalSignal::interrupt);
    CHECK(received.flush_peer_output && tty.buffered() == 0 && echo_of(tty) == "^C");
    received = tty.receive(termios, bytes("\x1c\x1a"), 4, false);
    CHECK(received.signal_count == 2 && received.signals[0] == TerminalSignal::quit &&
          received.signals[1] == TerminalSignal::suspend);
    echo_of(tty);
    // NOFLSH keeps the input; no budget stops before the signal character.
    termios.lflag |= lflag::noflsh;
    received = tty.receive(termios, bytes("ab\x03" "cd"), 0, false);
    CHECK(received.consumed == 2 && received.signal_count == 0 && tty.buffered() == 2);
    received = tty.receive(termios, bytes("\x03" "cd"), 1, false);
    CHECK(received.consumed == 3 && received.signal_count == 1 && !received.flush_peer_output);
    CHECK(tty.buffered() == 4);
    // Without ISIG the characters are data.
    termios.lflag &= ~lflag::isig;
    received = tty.receive(termios, bytes("\x03"), 0, false);
    CHECK(received.consumed == 1 && received.signal_count == 0 && tty.buffered() == 5);
    // IXON: VSTOP/VSTART are consumed and toggle output; a signal restarts output.
    termios = cooked();
    tty.flush_input();
    received = tty.receive(termios, bytes("\x13"), 1, false);
    CHECK(received.stop_output && tty.output_stopped() && tty.buffered() == 0);
    received = tty.receive(termios, bytes("\x11"), 1, false);
    CHECK(received.start_output && !tty.output_stopped());
    tty.receive(termios, bytes("\x13"), 1, false);
    received = tty.receive(termios, bytes("\x03"), 1, false);
    CHECK(received.start_output && !tty.output_stopped());
    tty.receive(termios, bytes("\x13"), 1, false);
    Termios no_ixon = termios;
    no_ixon.iflag &= ~iflag::ixon;
    tty.set_termios(termios, no_ixon);
    CHECK(!tty.output_stopped());
    return true;
}

bool input_mapping()
{
    Termios termios = cooked();
    termios.lflag &= ~lflag::echo;
    Tty tty{};
    termios.iflag = iflag::istrip;
    tty.receive(termios, bytes("\xe1\n"), 1, false);
    CHECK(read_now(tty, termios, 8) == "a\n");
    termios.iflag = iflag::igncr;
    tty.receive(termios, bytes("a\rb\n"), 1, false);
    CHECK(read_now(tty, termios, 8) == "ab\n");
    termios.iflag = iflag::inlcr;
    tty.receive(termios, bytes("c\n"), 1, false); // NL becomes CR, which does not end the line
    CHECK(!tty.poll_readable(termios));
    termios.iflag = iflag::icrnl;
    tty.receive(termios, bytes("\r"), 1, false);
    CHECK(read_now(tty, termios, 8) == "c\r\n");
    termios.iflag = 0;
    tty.receive(termios, bytes("d\r\n"), 1, false);
    CHECK(read_now(tty, termios, 8) == "d\r\n");
    return true;
}

bool output_processing()
{
    Termios termios = cooked();
    Tty tty{};
    std::array<std::byte, 64> out{};
    const auto text = [&](usize count) { return std::string(reinterpret_cast<const char *>(out.data()), count); };
    auto result = tty.process_output(termios, bytes("a\nb"), out);
    CHECK(result.consumed == 3 && text(result.produced) == "a\r\nb");
    // A byte is consumed only with its whole expansion.
    result = tty.process_output(termios, bytes("\n"), std::span<std::byte>{out.data(), 1});
    CHECK(result.consumed == 0 && result.produced == 0);
    termios.oflag = oflag::opost | oflag::ocrnl;
    result = tty.process_output(termios, bytes("x\r"), out);
    CHECK(text(result.produced) == "x\n");
    termios.oflag = oflag::opost | oflag::onocr;
    result = tty.process_output(termios, bytes("\r\rq\r"), out);
    CHECK(text(result.produced) == "\rq\r" && result.consumed == 4);
    termios.oflag = oflag::opost | oflag::xtabs;
    result = tty.process_output(termios, bytes("ab\t|"), out);
    CHECK(text(result.produced) == "ab      |" && tty.column() == 9);
    result = tty.process_output(termios, bytes("\t"), std::span<std::byte>{out.data(), 6});
    CHECK(result.consumed == 0);
    termios.oflag = oflag::opost | oflag::onlret;
    tty.process_output(termios, bytes("zz\n"), out);
    CHECK(tty.column() == 0);
    // IUTF8 continuation bytes do not advance the column.
    termios.oflag = oflag::opost;
    termios.iflag = iflag::iutf8;
    tty.process_output(termios, bytes("\r\xc3\xa9"), out);
    CHECK(tty.column() == 1);
    // Without OPOST bytes pass unchanged.
    termios.oflag = oflag::onlcr;
    result = tty.process_output(termios, bytes("\n\r\t"), out);
    CHECK(text(result.produced) == "\n\r\t");
    return true;
}

bool vmin_vtime()
{
    Termios termios = raw_pair_termios();
    Tty tty{};
    using W = Tty::ReadWait;
    const auto set = [&](u8 min, u8 time) {
        termios[Cc::min] = min;
        termios[Cc::time] = time;
    };
    set(0, 0); // polling read: returns what is there, possibly 0, even when blocking
    auto plan = tty.plan_read(termios, 8, false, false);
    CHECK(plan.wait == W::none && plan.copy == 0);
    CHECK(tty.plan_read(termios, 8, true, false).wait == W::none);
    set(0, 5); // read with timeout from the start
    plan = tty.plan_read(termios, 8, false, false);
    CHECK(plan.wait == W::timeout && plan.timer_deciseconds == 5 && plan.timer_nanoseconds() == 500'000'000ULL);
    plan = tty.plan_read(termios, 8, false, true);
    CHECK(plan.wait == W::none && plan.copy == 0);
    CHECK(tty.plan_read(termios, 8, true, false).wait == W::would_block);
    tty.receive(termios, bytes("abc"), 0, false); // with data buffered it returns at once, no timer
    plan = tty.plan_read(termios, 8, false, false);
    CHECK(plan.wait == W::none && plan.copy == 3 && plan.consume == 3 && plan.timer_deciseconds == 0);
    CHECK(tty.plan_read(termios, 2, false, false).copy == 2); // capped by the request
    tty.flush_input();
    set(3, 0); // counted read without timer
    plan = tty.plan_read(termios, 8, false, false);
    CHECK(plan.wait == W::input && !tty.poll_readable(termios));
    tty.receive(termios, bytes("ab"), 0, false);
    CHECK(tty.plan_read(termios, 8, false, false).wait == W::minimum && !tty.poll_readable(termios));
    CHECK(tty.plan_read(termios, 2, false, false).copy == 2); // VMIN is capped by the request
    CHECK(tty.plan_read(termios, 8, true, false).copy == 2);  // nonblocking takes what is there
    tty.receive(termios, bytes("c"), 0, false);
    CHECK(tty.poll_readable(termios) && tty.plan_read(termios, 8, false, false).copy == 3);
    tty.flush_input();
    set(3, 2); // interbyte timer only after the first byte
    CHECK(tty.plan_read(termios, 8, false, false).wait == W::input);
    tty.receive(termios, bytes("a"), 0, false);
    CHECK(tty.poll_readable(termios)); // Linux readiness: one byte once VTIME is set
    plan = tty.plan_read(termios, 8, false, false);
    CHECK(plan.wait == W::interbyte && plan.timer_deciseconds == 2);
    plan = tty.plan_read(termios, 8, false, true);
    CHECK(plan.wait == W::none && plan.copy == 1);
    // A zero-length read never waits.
    CHECK(tty.plan_read(termios, 0, false, false).wait == W::none);
    return true;
}

bool switching_and_capacity()
{
    Termios canonical = cooked();
    canonical.lflag &= ~lflag::echo;
    const Termios raw = raw_pair_termios();
    Tty tty{};
    tty.receive(canonical, bytes("abc"), 1, false);
    CHECK(!tty.poll_readable(canonical));
    CHECK(tty.set_termios(canonical, raw) && tty.inq(raw) == 3 && read_now(tty, raw, 2) == "ab");
    tty.receive(raw, bytes("xy"), 0, false);
    CHECK(!tty.set_termios(raw, canonical) && tty.poll_readable(canonical));
    CHECK(read_now(tty, canonical, 8) == "cxy");
    // Linux n_tty_receive_buf_common: without a complete line, canonical input
    // past capacity - 1 keeps overwriting the last slot. IMAXBEL has no effect
    // in Linux, and with ECHO clear nothing reaches the terminal.
    Termios bell = canonical;
    bell.iflag |= iflag::imaxbel;
    auto received = tty.receive(bell, bytes(std::string(5000, 'a')), 1, false);
    CHECK(received.consumed == 5000 && !received.dropped && tty.buffered() == 4096);
    CHECK(tty.room(bell) == 1 && tty.echo_pending() == 0);
    // The first erase takes back the newest byte and then erases one more.
    tty.receive(bell, bytes("\x7f\x7f" "bb\n"), 1, false);
    CHECK(tty.buffered() == 4096 && tty.inq(bell) == 4096 && tty.room(bell) == 0);
    // With a complete line in a full buffer further input applies backpressure.
    received = tty.receive(bell, bytes("z"), 1, false);
    CHECK(received.consumed == 0);
    received = tty.receive(bell, bytes("z"), 1, true); // console: dropped and counted
    CHECK(received.consumed == 1 && received.dropped && tty.overruns() == 1);
    const std::string line = read_now(tty, bell, 5000);
    CHECK(line.size() == 4096 && line.substr(4092) == "abb\n");
    // With a complete line queued, input stops one slot short: neither a
    // terminator nor an erase is taken until a read makes room.
    tty.receive(canonical, bytes("a\n"), 1, false);
    received = tty.receive(canonical, bytes(std::string(5000, 'x')), 1, false);
    CHECK(received.consumed == 4093 && tty.buffered() == 4095 && tty.room(canonical) == 0);
    CHECK(tty.receive(canonical, bytes("\n"), 1, false).consumed == 0);
    CHECK(tty.receive(canonical, bytes("\x7f"), 1, false).consumed == 0);
    CHECK(tty.inq(canonical) == 2 && tty.buffered() == 4095 && read_now(tty, canonical, 8) == "a\n");
    CHECK(tty.receive(canonical, bytes("\n"), 1, false).consumed == 1);
    CHECK(read_now(tty, canonical, 5000) == std::string(4093, 'x') + "\n");
    // Under ECHO every byte past the capacity is echoed; an erase at the full
    // buffer silently drops the newest byte, then rubs out the one before.
    NTty<64, 512> small{};
    Termios echoing = cooked();
    echoing.iflag |= iflag::imaxbel;
    const auto overflowed = small.receive(echoing, bytes(std::string(70, 'e')), 1, false);
    CHECK(overflowed.consumed == 70 && small.buffered() == 64 && echo_of(small) == std::string(70, 'e'));
    small.receive(echoing, bytes("\x7f"), 1, false);
    CHECK(small.buffered() == 62 && echo_of(small) == "\b \b");
    small.receive(echoing, bytes("\n"), 1, false);
    CHECK(read_now(small, echoing, 100) == std::string(62, 'e') + "\n");
    // Non-canonical: backpressure at the full buffer, or an overrun.
    received = tty.receive(raw, bytes(std::string(5000, 'r')), 0, false);
    CHECK(received.consumed == 4096 && tty.room(raw) == 0 && tty.inq(raw) == 4096);
    received = tty.receive(raw, bytes("s"), 0, true);
    CHECK(received.consumed == 1 && tty.overruns() == 2 && tty.buffered() == 4096);
    tty.hangup();
    CHECK(tty.buffered() == 0 && tty.echo_pending() == 0 && tty.cleared_outside_live_bytes());
    return true;
}

bool echo_ring_and_sink()
{
    const Termios termios = cooked();
    Tty tty{};
    tty.receive(termios, bytes(std::string(600, 'e')), 1, false);
    CHECK(tty.echo_pending() == 512 && tty.echo_discarded() == 88);
    struct Sink
    {
        std::string data;
        usize limit;
        usize put(std::span<const std::byte> part)
        {
            const usize take = part.size() < limit - data.size() ? part.size() : limit - data.size();
            data.append(reinterpret_cast<const char *>(part.data()), take);
            return take;
        }
    } sink{{}, 100};
    static_assert(EchoSink<Sink>);
    CHECK(tty.drain_echo(sink) == 100 && tty.echo_pending() == 412);
    sink.limit = 10000;
    CHECK(tty.drain_echo(sink) == 412 && tty.echo_pending() == 0 && sink.data == std::string(512, 'e'));
    tty.flush_input();
    CHECK(tty.cleared_outside_live_bytes());
    return true;
}

// Linux moves its column only for echo it writes: echo discarded by an ISIG
// flush or dropped from a full ring never moves the cursor.
bool echo_columns()
{
    const Termios termios = linux_std_termios();
    Tty tty{};
    tty.receive(termios, bytes("abc\x03"), 1, false);
    CHECK(echo_of(tty) == "^C" && tty.column() == 2);
    tty.receive(termios, bytes("\t\x7f"), 1, false); // the line starts at column 2
    CHECK(echo_of(tty) == "\t\b\b\b\b\b\b" && tty.column() == 2);
    // Only the bytes the owner took count; a UTF-8 continuation byte has no width.
    Termios utf8 = termios;
    utf8.iflag |= iflag::iutf8;
    Tty partial{};
    partial.receive(utf8, bytes("a\xc3\xa9x"), 1, false);
    std::array<std::byte, 3> three{};
    CHECK(partial.take_echo(three) == 3);
    partial.receive(utf8, bytes("\x03"), 1, false);
    CHECK(echo_of(partial) == "^C" && partial.column() == 4);
    // Without OPOST echo does not move the column at all.
    Termios raw_echo = termios;
    raw_echo.oflag &= ~oflag::opost;
    Tty plain{};
    plain.receive(raw_echo, bytes("\tab"), 1, false);
    std::array<std::byte, 1> one{};
    CHECK(plain.take_echo(one) == 1);
    plain.receive(raw_echo, bytes("\x03"), 1, false);
    CHECK(echo_of(plain) == "^C" && plain.column() == 2);
    // Program output written while no echo is pending moves the cursor too.
    Tty output{};
    std::array<std::byte, 8> out{};
    output.process_output(termios, bytes("abc"), out);
    output.receive(termios, bytes("x\x03"), 1, false);
    CHECK(echo_of(output) == "^C" && output.column() == 5);
    // Echo dropped from the full ring: the column counts the 512 bytes kept.
    Tty flood{};
    flood.receive(termios, bytes(std::string(600, 'e')), 1, false);
    CHECK(flood.echo_pending() == 512 && flood.column() == 512);
    CHECK(echo_of(flood) == std::string(512, 'e') && flood.column() == 512);
    return true;
}

// Independent reference for canonical input with ASCII (no tab) data under
// OPOST|ONLCR|ICRNL, with randomly chosen echo, ISIG and IEXTEN flags.
struct Reference
{
    struct Line
    {
        std::string content;
        bool eof{};
    };
    std::deque<Line> lines;
    std::string partial, echo;
    bool lnext{};
    unsigned signals{};

    static bool control(unsigned char c) { return c < 0x20 || c == 0x7f; }
    static bool word(unsigned char c) { return std::isalnum(c) || c == '_'; }
    usize used() const
    {
        usize total = partial.size();
        for (const auto &line : lines)
            total += line.content.size() + (line.eof ? 1 : 0);
        return total;
    }
    void echo_raw(const Termios &t, char c)
    {
        if (c == '\n' && (t.oflag & oflag::opost) && (t.oflag & oflag::onlcr))
            echo += "\r\n";
        else
            echo += c;
    }
    void echo_char(const Termios &t, unsigned char c)
    {
        if ((t.lflag & lflag::echoctl) && control(c) && c != '\t')
        {
            echo += '^';
            echo += static_cast<char>(c ^ 0x40);
        }
        else
            echo_raw(t, static_cast<char>(c));
    }
    void rubout(const Termios &t, unsigned char c)
    {
        const int count = control(c) ? ((t.lflag & lflag::echoctl) ? 2 : 0) : 1;
        for (int i = 0; i < count; ++i)
            echo += "\b \b";
    }
    bool store(const Termios &t, unsigned char c, bool terminator, bool eof, int echo_mode)
    {
        if (echo_mode == 1 && (t.lflag & lflag::echo))
            echo_char(t, c);
        if (echo_mode == 2 && (t.lflag & (lflag::echo | lflag::echonl)))
            echo_raw(t, '\n');
        if (!eof)
            partial += static_cast<char>(c);
        if (terminator)
        {
            lines.push_back({partial, eof});
            partial.clear();
        }
        return true;
    }
    // Returns false for backpressure. At 4095 bytes input waits while a line is
    // queued; otherwise a byte at 4096 first takes back the newest one (Linux
    // n_tty_receive_buf_common). IMAXBEL has no effect.
    bool receive(const Termios &t, unsigned char c)
    {
        const bool echo_on = t.lflag & lflag::echo, ext = t.lflag & lflag::iexten;
        if (used() + 1 >= 4096)
        {
            if (!lines.empty())
                return false;
            if (used() == 4096)
                partial.pop_back();
        }
        if (lnext)
        {
            if (!store(t, c, false, false, 1))
                return false;
            lnext = false;
            return true;
        }
        if ((t.lflag & lflag::isig) && (c == 3 || c == 0x1c || c == 0x1a))
        {
            ++signals;
            if (!(t.lflag & lflag::noflsh))
            {
                lines.clear();
                partial.clear();
                echo.clear();
            }
            if (echo_on)
                echo_char(t, c);
            return true;
        }
        if (c == '\r')
            c = '\n';
        if (c == 0x7f || c == 0x15 || (ext && c == 0x17))
        {
            if (partial.empty())
                return true;
            if (c == 0x15 && (!echo_on || !(t.lflag & lflag::echok) || !(t.lflag & lflag::echoke) ||
                              !(t.lflag & lflag::echoe)))
            {
                partial.clear();
                if (echo_on)
                {
                    echo_char(t, 0x15);
                    if (t.lflag & lflag::echok)
                        echo_raw(t, '\n');
                }
                return true;
            }
            unsigned alnums = 0;
            while (!partial.empty())
            {
                const unsigned char last = static_cast<unsigned char>(partial.back());
                if (c == 0x17)
                {
                    if (word(last))
                        ++alnums;
                    else if (alnums)
                        break;
                }
                partial.pop_back();
                if (echo_on)
                {
                    if (c == 0x7f && !(t.lflag & lflag::echoe))
                        echo_char(t, 0x7f);
                    else
                        rubout(t, last);
                }
                if (c == 0x7f)
                    break;
            }
            return true;
        }
        if (ext && c == 0x16)
        {
            lnext = true;
            if (echo_on && (t.lflag & lflag::echoctl))
                echo += "^\b";
            return true;
        }
        if (ext && echo_on && c == 0x12)
        {
            echo_char(t, c);
            echo_raw(t, '\n');
            for (const char p : partial)
                echo_char(t, static_cast<unsigned char>(p));
            return true;
        }
        if (c == '\n')
            return store(t, '\n', true, false, 2);
        if (c == 4)
            return store(t, 0, true, true, 0);
        return store(t, c, false, false, 1);
    }
    // Nonblocking canonical read; nullopt when no complete line.
    bool read(usize count, std::string &out)
    {
        if (lines.empty())
            return false;
        Line &line = lines.front();
        const usize n = count < line.content.size() ? count : line.content.size();
        out = line.content.substr(0, n);
        line.content.erase(0, n);
        if (line.content.empty())
            lines.pop_front();
        return true;
    }
};

bool differential(usize steps)
{
    std::mt19937_64 random(0x6e5f747479ULL);
    const std::string alphabet = std::string("abcdefg _-.:019XY") + "\r\n\x7f\x15\x17\x16\x12\x04\x03\x1c\x01\x1b";
    const std::string long_line = "abcdefg _-.:019XY\x7f\x01";
    Tty tty{};
    Reference reference{};
    Termios termios = cooked();
    termios.iflag = iflag::icrnl;
    usize peak = 0;
    for (usize step = 0; step < steps; ++step)
    {
        if (step % 1000 == 0)
        {
            const u64 bits = random();
            termios.lflag = lflag::icanon;
            const std::array<u32, 9> optional{lflag::echo, lflag::echoe, lflag::echok, lflag::echoke, lflag::echoctl,
                                              lflag::noflsh, lflag::iexten, lflag::isig, lflag::echonl};
            for (usize index = 0; index < optional.size(); ++index)
                if (bits >> index & 1)
                    termios.lflag |= optional[index];
            termios.iflag = iflag::icrnl | ((bits >> 12 & 1) ? iflag::imaxbel : 0);
        }
        if (step % 21000 == 0)
        {
            tty.hangup();
            reference = {};
        }
        const u64 action = random() % 16;
        // Long stretches without reads drive the buffer to its limits.
        const bool starving = (step / 7000) % 3 == 2;
        if (action < 10 || starving)
        {
            // Some starving stretches type one endless line (no terminator,
            // signal or kill) to reach the canonical overflow drop.
            const std::string &chars = starving && (step / 7000) % 6 == 5 ? long_line : alphabet;
            std::string input;
            for (usize count = 1 + random() % 6; count; --count)
                input += chars[random() % chars.size()];
            const auto received = tty.receive(termios, bytes(input), 8, false);
            usize accepted = 0;
            while (accepted < input.size() && reference.receive(termios, static_cast<unsigned char>(input[accepted])))
                ++accepted;
            CHECK(received.consumed == accepted);
        }
        else
        {
            const usize count = 1 + random() % 24;
            std::string expected;
            const bool ready = reference.read(count, expected);
            const auto plan = tty.plan_read(termios, count, true, false);
            CHECK((plan.wait == Tty::ReadWait::none) == ready);
            if (ready)
            {
                std::string got(plan.copy, '\0');
                tty.preview(plan, {reinterpret_cast<std::byte *>(got.data()), got.size()});
                CHECK(got == expected && tty.consume(plan));
            }
        }
        CHECK(echo_of(tty) == reference.echo);
        reference.echo.clear();
        CHECK(tty.buffered() == reference.used());
        CHECK(tty.poll_readable(termios) == !reference.lines.empty());
        usize pending = 0;
        for (const auto &line : reference.lines)
            pending += line.content.size();
        CHECK(tty.inq(termios) == pending);
        peak = std::max(peak, tty.buffered());
    }
    CHECK(peak == 4096); // the run reaches the full buffer
    return true;
}

// raw_pair_termios: N_TTY is a 4096-byte FIFO, byte for byte, with no echo.
bool raw_equivalence(usize steps)
{
    std::mt19937_64 random(0x726177ULL);
    const Termios termios = raw_pair_termios();
    Tty tty{};
    std::deque<unsigned char> fifo;
    for (usize step = 0; step < steps; ++step)
    {
        if (random() % 2)
        {
            std::string input(random() % 700, '\0');
            for (auto &c : input)
                c = static_cast<char>(random());
            const auto received = tty.receive(termios, bytes(input), 0, false);
            const usize accepted = input.size() < 4096 - fifo.size() ? input.size() : 4096 - fifo.size();
            CHECK(received.consumed == accepted && received.signal_count == 0 && !received.echoed);
            fifo.insert(fifo.end(), input.begin(), input.begin() + static_cast<long>(accepted));
        }
        else
        {
            const usize count = 1 + random() % 900;
            const bool nonblocking = random() % 2;
            const auto plan = tty.plan_read(termios, count, nonblocking, false);
            if (fifo.empty())
                CHECK(plan.wait == (nonblocking ? Tty::ReadWait::would_block : Tty::ReadWait::input));
            else
            {
                const usize take = count < fifo.size() ? count : fifo.size();
                std::string got(plan.copy, '\0');
                tty.preview(plan, {reinterpret_cast<std::byte *>(got.data()), got.size()});
                CHECK(plan.wait == Tty::ReadWait::none && got.size() == take);
                CHECK(std::equal(got.begin(), got.end(), fifo.begin(),
                                 [](char a, unsigned char b) { return static_cast<unsigned char>(a) == b; }));
                CHECK(tty.consume(plan));
                fifo.erase(fifo.begin(), fifo.begin() + static_cast<long>(take));
            }
        }
        CHECK(tty.buffered() == fifo.size() && tty.room(termios) == 4096 - fifo.size());
        CHECK(tty.poll_readable(termios) == !fifo.empty() && tty.inq(termios) == fifo.size());
        CHECK(tty.echo_pending() == 0);
    }
    // Output is untouched with OPOST clear.
    std::string all(256, '\0');
    for (usize index = 0; index < all.size(); ++index)
        all[index] = static_cast<char>(index);
    std::string out(256, '\0');
    const auto written = tty.process_output(termios, bytes(all), {reinterpret_cast<std::byte *>(out.data()), out.size()});
    CHECK(written.consumed == 256 && written.produced == 256 && out == all);
    return true;
}
} // namespace

int main()
{
    constexpr usize steps = 100000;
    if (!defaults() || !canonical_lines_and_eof() || !erase_kill_werase_echo() || !lnext_and_reprint())
        return 1;
    if (!utf8_erase() || !isig_and_flow() || !input_mapping() || !output_processing() || !vmin_vtime())
        return 1;
    if (!switching_and_capacity() || !echo_ring_and_sink() || !echo_columns() || !differential(steps) || !raw_equivalence(steps))
        return 1;
    if (constant_initialized.buffered() != 0)
        return 1;
    std::printf("n_tty checks=%zu\n", checks);
    std::printf("N_TTY_MODEL_PASS canonical=%zu echo=%zu model_steps=%zu utf8_erase=1 isig=1 vmin_vtime=1 opost=1 "
                "raw_equivalence=1 constexpr=1 guest=0\n",
                Tty::read_capacity, Tty::echo_capacity, steps);
    return 0;
}
