// Hosted copied-byte model of the terminal codecs: the native 36-byte wire on
// both endiannesses and the generated Linux TermiosCodec/TtyIoctlCodec on all
// ten profiles. No user pointer, terminal object, guest or hardware is involved.
#include "ok/abi/linux_termios.hpp"
#include "ok/uapi/syscall.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
#include <utility>

using namespace ok;
using namespace ok::abi::linux_user;

namespace
{
usize checks{};
#define CHECK(expression)                                                                                    \
    do                                                                                                       \
    {                                                                                                        \
        ++checks;                                                                                            \
        if (!(expression))                                                                                   \
        {                                                                                                    \
            std::fprintf(stderr, "terminal codec check failed at line %d: %s\n", __LINE__, #expression);     \
            return false;                                                                                    \
        }                                                                                                    \
    } while (false)

using T = TerminalIoctl;
constexpr usize ops = static_cast<usize>(T::count);

// docs/modules/tty.md §5, column by column.
constexpr std::array<u32, ops> generic_requests{0x540e, 0x5429, 0x540f, 0x5410, 0x5401, 0x5402, 0x5403, 0x5404,
                                               0x5409, 0x540b, 0x5413, 0x5414, 0x5422, 0x541b, 0x80045430,
                                               0x40045431};
constexpr std::array<u32, ops> mips_requests{0x5480, 0x7416, 0x40047477, 0x80047476, 0x540d, 0x540e, 0x540f,
                                            0x5410, 0x5405, 0x5407, 0x40087468, 0x80087467, 0x5471, 0x467f,
                                            0x40045430, 0x80045431};
constexpr std::array<u32, ops> ppc_requests{0x540e, 0x5429, 0x40047477, 0x80047476, 0x402c7413, 0x802c7414,
                                           0x802c7415, 0x802c7416, 0x2000741d, 0x2000741f, 0x40087468,
                                           0x80087467, 0x5422, 0x4004667f, 0x40045430, 0x80045431};
// Native requests are the generic column on every profile.
constexpr std::array<u32, 12> native_requests{OK_TCGETS,    OK_TCSETS,     OK_TCSETSW,  OK_TCSETSF,
                                              OK_TCSBRK,    OK_TCFLSH,     OK_TIOCGWINSZ, OK_TIOCSWINSZ,
                                              OK_TIOCNOTTY, OK_FIONREAD,   OK_TIOCSCTTY, OK_TIOCGSID};

template <Profile P> constexpr bool mips_family = P == Profile::mips || P == Profile::mips64;

template <Profile P> std::array<u8, 64> encoded(const tty::Termios &termios, const tty::ForeignResidue &residue = {})
{
    std::array<u8, 64> out{};
    (void)TermiosCodec<P>::encode(termios, residue, {out.data(), TermiosCodec<P>::termios_bytes});
    return out;
}

template <bool Big> u32 load(const std::array<u8, 64> &bytes, usize offset)
{
    return tty::wire::load<Big, 4>(bytes, offset);
}

// A termios whose every field uses named values only.
tty::Termios random_named(std::mt19937_64 &random)
{
    using namespace tty;
    tty::Termios t{};
    t.iflag = static_cast<u32>(random()) & 0x7fff;
    t.oflag = static_cast<u32>(random()) & 0xffff;
    const u32 baud = static_cast<u32>(random()) & cflag::cbaud;
    const u32 input_baud = static_cast<u32>(random() % 3) == 0 ? 0 : static_cast<u32>(random()) & cflag::cbaud;
    t.cflag = baud | (input_baud << cflag::ibshift) | (static_cast<u32>(random()) & 0xff0) |
              (static_cast<u32>(random()) & (cflag::addrb | cflag::cmspar | cflag::crtscts));
    t.lflag = static_cast<u32>(random()) & 0x1dfff & ~0x2000U;
    t.line = static_cast<u8>(random());
    for (auto &c : t.cc)
        c = static_cast<u8>(random());
    t.cc[17] = t.cc[18] = 0; // generic slots without a name live in the residue
    return t;
}

template <Profile P> bool profile(std::set<std::array<u8, 5>> &layouts)
{
    using Wire = TermiosCodec<P>;
    using Tty = TtyIoctlCodec<P>;
    constexpr bool big = Wire::big_endian;
    constexpr bool mips = mips_family<P>;
    constexpr bool ppc = P == Profile::ppc;
    const auto &requests = mips ? mips_requests : ppc ? ppc_requests : generic_requests;
    // Layout: exact kernel struct sizes.
    constexpr TermiosLayout layout = Wire::layout;
    layouts.insert({layout.bytes, layout.nccs, layout.line_offset, layout.cc_offset, layout.speed_offset});
    CHECK(layout.bytes == (ppc ? 44 : mips ? 40 : 36) && layout.nccs == (mips ? 23 : 19));
    CHECK(layout.line_offset == (ppc ? 35 : 16) && layout.cc_offset == (ppc ? 16 : 17));
    CHECK(layout.speed_offset == (ppc ? 36 : 0) && Tty::termios_bytes == layout.bytes);
    CHECK(Wire::unnamed.count == (mips ? 6 : 2));
    // Requests.
    for (usize index = 0; index < ops; ++index)
    {
        const auto op = static_cast<T>(index);
        CHECK(Tty::number(op) == requests[index]);
        CHECK(Tty::operation(requests[index]) == op);
        CHECK(Tty::operation(0xdead000000000000ULL | requests[index]) == op);
        const std::array<u64, 3> words{0x00000000fffffffeULL, 0xffffffff00000000ULL | requests[index],
                                       0x8765432112345678ULL};
        const auto decoded = Tty::decode(words);
        CHECK(decoded && decoded.value().fd == -2 && decoded.value().operation == op);
        CHECK(decoded.value().argument == (words[2] & Codec<P>::word_mask));
    }
    CHECK(!Tty::number(T::count));
    // Console controls keep their ConsoleIoctlCodec numbers.
    for (usize index = 0; index < 4; ++index)
        CHECK(Tty::number(static_cast<T>(index)) == Codec<P>::profile.console_ioctls[index]);
    // Requests outside the table (TCXONC, TCGETS2, TIOCPKT, TIOCSTI, TIOCGPTPEER, TIOCOUTQ ...) are not terminal requests.
    for (const u32 unknown : {0x540aU, 0x802c542aU, 0x5420U, 0x5412U, 0x5441U, 0x5411U, 0x7472U, 0x40047473U,
                              0x2000741eU, 0x80047420U, 0U, 0xffffffffU})
    {
        if (std::find(requests.begin(), requests.end(), unknown) != requests.end())
            continue;
        const std::array<u64, 3> words{0, unknown, 0};
        const auto decoded = Tty::decode(words);
        CHECK(!Tty::operation(unknown) && !decoded && decoded.status().code() == StatusCode::not_terminal);
    }
    const std::array<u64, 3> frame{0, requests[4], 0};
    for (usize count = 0; count < 3; ++count)
        CHECK(!Tty::decode({frame.data(), count}));
    // Argument kinds and copy sizes.
    CHECK(Tty::argument_bytes(T::get_attributes) == layout.bytes && Tty::argument_bytes(T::set_flush) == layout.bytes);
    CHECK(Tty::argument_bytes(T::get_window) == 8 && Tty::argument_bytes(T::set_window) == 8);
    CHECK(Tty::argument_bytes(T::input_pending) == 4 && Tty::argument_bytes(T::get_pty_number) == 4 &&
          Tty::argument_bytes(T::set_pty_lock) == 4 && Tty::argument_bytes(T::set_foreground) == 4);
    CHECK(Tty::argument_bytes(T::flush) == 0 && Tty::argument_bytes(T::drain_break) == 0 &&
          Tty::argument_bytes(T::detach) == 0 && Tty::argument_bytes(T::acquire) == 0);
    CHECK(terminal_argument(T::flush) == TerminalArgument::scalar && Tty::scalar_argument(0xffffffff00000002ULL) == 2);
    CHECK(terminal_argument(T::detach) == TerminalArgument::none);
    // Standard termios bytes per profile.
    const auto standard = encoded<P>(tty::linux_std_termios());
    const u32 iflag = load<big>(standard, 0), oflag = load<big>(standard, 4), cflag = load<big>(standard, 8),
              lflag = load<big>(standard, 12);
    CHECK(iflag == (ppc ? 0x300U : mips ? 0x500U : 0x500U) && oflag == (ppc ? 0x3U : 0x5U));
    CHECK(cflag == (ppc ? 0x4b0fU : 0x4bfU));
    CHECK(lflag == (ppc ? 0x5cfU : mips ? 0xb3bU : 0x8a3bU));
    const usize cc = layout.cc_offset;
    CHECK(standard[cc + 0] == 3 && standard[cc + 2] == 0x7f && standard[layout.line_offset] == 0);
    CHECK(standard[cc + (ppc ? 4 : mips ? 16 : 4)] == 4);   // VEOF
    CHECK(standard[cc + (ppc ? 5 : mips ? 4 : 6)] == 1);    // VMIN
    CHECK(standard[cc + (ppc ? 7 : mips ? 5 : 5)] == 0);    // VTIME
    CHECK(standard[cc + (ppc ? 12 : 10)] == 0x1a);          // VSUSP
    if constexpr (ppc)
        CHECK(load<big>(standard, 36) == 38400 && load<big>(standard, 40) == 38400);
    // A fresh Linux pty reports the pts init_termios: HUPCL clear.
    const auto pty = encoded<P>(tty::linux_pty_termios());
    CHECK(load<big>(pty, 8) == (ppc ? 0xb0fU : 0xbfU) && load<big>(pty, 0) == iflag && load<big>(pty, 12) == lflag);
    if constexpr (ppc)
        CHECK(load<big>(pty, 36) == 38400 && load<big>(pty, 40) == 38400);
    // TOSTOP and NOFLSH travel to their profile bits.
    tty::Termios stopped{};
    stopped.lflag = tty::lflag::tostop | tty::lflag::noflsh;
    CHECK(load<big>(encoded<P>(stopped), 12) == (ppc ? 0x80400000U : mips ? 0x8080U : 0x180U));
    // Round trip of named values.
    std::mt19937_64 random(0x7465726dULL + static_cast<u64>(P));
    for (usize round = 0; round < 2000; ++round)
    {
        const tty::Termios original = random_named(random);
        std::array<u8, 64> bytes{};
        CHECK(Wire::encode(original, {}, {bytes.data(), layout.bytes}));
        tty::Termios back{};
        tty::ForeignResidue residue{};
        CHECK(Wire::decode({bytes.data(), layout.bytes}, back, residue));
        CHECK(back == original && residue.flags == (std::array<u32, 4>{}) && residue.cc_count == Wire::unnamed.count);
    }
    // Residue: arbitrary kernel bytes survive decode/encode, except the PPC speed
    // words, which are recomputed from CBAUD and ignored on input.
    for (usize round = 0; round < 2000; ++round)
    {
        std::array<u8, 64> bytes{};
        for (usize index = 0; index < layout.bytes; ++index)
            bytes[index] = static_cast<u8>(random());
        tty::Termios value{};
        tty::ForeignResidue residue{};
        CHECK(Wire::decode({bytes.data(), layout.bytes}, value, residue));
        std::array<u8, 64> again{};
        CHECK(Wire::encode(value, residue, {again.data(), layout.bytes}));
        const usize compared = ppc ? 36 : layout.bytes;
        CHECK(std::equal(bytes.begin(), bytes.begin() + static_cast<long>(compared), again.begin()));
        if constexpr (ppc)
        {
            CHECK(load<big>(again, 36) == tty::input_baud(value) && load<big>(again, 40) == tty::output_baud(value));
            std::array<u8, 64> speeds = bytes;
            speeds[36] ^= 0xff;
            speeds[43] ^= 0x55;
            tty::Termios other{};
            tty::ForeignResidue other_residue{};
            CHECK(Wire::decode({speeds.data(), layout.bytes}, other, other_residue));
            CHECK(other == value && other_residue == residue);
        }
    }
    if constexpr (mips)
    {
        // VDSUSP (slot 11) has no generic name and rides in the residue.
        std::array<u8, 64> bytes = encoded<P>(tty::linux_std_termios());
        bytes[cc + 11] = 0x19;
        tty::Termios value{};
        tty::ForeignResidue residue{};
        CHECK(Wire::decode({bytes.data(), layout.bytes}, value, residue));
        CHECK(value == tty::linux_std_termios() && residue.cc_count == 6 && residue.cc[0] == 0x19);
        CHECK(encoded<P>(value, residue)[cc + 11] == 0x19 && encoded<P>(value)[cc + 11] == 0);
    }
    // Exact copy sizes: other sizes are refused and leave everything untouched.
    std::array<u8, 64> guard{};
    guard.fill(0xa5);
    tty::Termios untouched = tty::console_compat_termios();
    tty::ForeignResidue kept{};
    kept.cc_count = 3;
    for (const usize size : {usize{0}, usize{layout.bytes - 1}, usize{layout.bytes + 1}, usize{60}})
    {
        CHECK(!Wire::decode({guard.data(), size}, untouched, kept));
        CHECK(!Wire::encode(untouched, kept, {guard.data(), size}));
        CHECK(untouched == tty::console_compat_termios() && kept.cc_count == 3);
        CHECK(std::all_of(guard.begin(), guard.end(), [](u8 byte) { return byte == 0xa5; }));
    }
    // Window and int copies are target-endian and exact.
    std::array<u8, 8> window{};
    CHECK(Tty::encode_window({0x0102, 0x0304, 0x0506, 0x0708}, window));
    CHECK(window == (big ? std::array<u8, 8>{1, 2, 3, 4, 5, 6, 7, 8} : std::array<u8, 8>{2, 1, 4, 3, 6, 5, 8, 7}));
    tty::WinSize size_back{};
    CHECK(Tty::decode_window(window, size_back) && size_back == (tty::WinSize{0x0102, 0x0304, 0x0506, 0x0708}));
    CHECK(!Tty::decode_window({window.data(), 7}, size_back));
    std::array<u8, 4> word{};
    CHECK(Tty::encode_int(-2, word) && Tty::decode_int(word) && Tty::decode_int(word).value() == -2);
    CHECK(word == (big ? std::array<u8, 4>{0xff, 0xff, 0xff, 0xfe} : std::array<u8, 4>{0xfe, 0xff, 0xff, 0xff}));
    CHECK(!Tty::decode_int({word.data(), 3}));
    return true;
}

template <bool Big> bool native_wire()
{
    using Wire = tty::NativeTermiosWire<Big>;
    static_assert(Wire::termios_bytes == 36);
    std::array<u8, 36> bytes{};
    const tty::Termios standard = tty::linux_std_termios();
    tty::ForeignResidue residue{};
    residue.flags[0] = 0x80000000;
    CHECK(Wire::encode(standard, residue, bytes)); // the native wire ignores any residue
    const std::array<u8, 4> lflag = Big ? std::array<u8, 4>{0, 0, 0x8a, 0x3b} : std::array<u8, 4>{0x3b, 0x8a, 0, 0};
    CHECK(std::equal(lflag.begin(), lflag.end(), bytes.begin() + 12));
    CHECK(bytes[16] == 0 && bytes[17] == 3 && bytes[17 + 4] == 4 && bytes[17 + 6] == 1); // VINTR, VEOF, VMIN
    CHECK(bytes[Big ? 3 : 0] == 0x00 && bytes[Big ? 2 : 1] == 0x05); // ICRNL|IXON = 0x500
    tty::Termios back{};
    tty::ForeignResidue cleared{};
    cleared.cc_count = 4;
    CHECK(Wire::decode(bytes, back, cleared) && back == standard && cleared == tty::ForeignResidue{});
    for (const usize size : {usize{35}, usize{37}, usize{44}})
    {
        std::array<u8, 44> other{};
        CHECK(!Wire::decode({other.data(), size}, back, cleared) && !Wire::encode(back, {}, {other.data(), size}));
    }
    // On little-endian generic profiles the native wire equals the Linux bytes.
    if constexpr (!Big)
    {
        std::mt19937_64 random(0x6e6174ULL);
        for (usize round = 0; round < 500; ++round)
        {
            const tty::Termios value = random_named(random);
            std::array<u8, 36> native{};
            CHECK(Wire::encode(value, {}, native));
            const auto linux_bytes = encoded<Profile::x86_64>(value);
            CHECK(std::equal(native.begin(), native.end(), linux_bytes.begin()));
        }
    }
    return true;
}

template <usize... Index> bool all_profiles(std::index_sequence<Index...>, std::set<std::array<u8, 5>> &layouts)
{
    return (profile<static_cast<Profile>(Index)>(layouts) && ...);
}
} // namespace

int main()
{
    static_assert(sizeof(tty::Termios) >= 36 && tty::nccs == 19);
    for (usize index = 0; index < native_requests.size(); ++index)
        if (native_requests[index] != generic_requests[index < 10 ? index + 4 : (index == 10 ? 0 : 1)])
            return 1;
    if (!(native_wire<false>() && native_wire<true>()))
        return 1;
    std::set<std::array<u8, 5>> layouts;
    constexpr usize count = static_cast<usize>(Profile::count);
    if (!all_profiles(std::make_index_sequence<count>{}, layouts))
        return 1;
    if (layouts.size() != 3)
        return 1;
    std::printf("terminal codec checks=%zu\n", checks);
    std::printf("TERMINAL_CODEC_PASS linux_profiles=%zu termios_layouts=%zu roundtrip=1 residue=1 "
                "native_wire_endians=2 hosted=1 guest=0\n",
                count, layouts.size());
    return 0;
}
