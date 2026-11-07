// Terminal ioctl dispatch (docs/modules/tty.md section 5): the error order of
// the shared dispatch under injected seams for the native wire and all ten
// Linux encodings, the native request table, and termios crossing both
// personalities on the actual console terminal. Hosted evidence only.
#include "native_clock_model.hpp"

namespace
{
using N = syscall::Number;
using Pid = sched::ProcessId;
using Op = sched::ConsoleControlOperation;
using Kind = lu::TerminalArgument;
namespace cc = user::console_control;
constexpr uptr output = hw::user_begin + 128;
usize native_requests{}, linux_profiles{}, order_cases{};

// Every refusal stops before the next stage; copies have exactly the codec sizes.
template <class Codec> void error_order()
{
    for (usize index = 0; index < static_cast<usize>(Op::count); ++index)
    {
        const auto operation = static_cast<Op>(index);
        const auto kind = cc::argument(operation);
        const usize in_bytes = kind == Kind::int_in ? 4 : kind == Kind::termios_in ? Codec::termios_bytes
                             : kind == Kind::window_in ? Codec::window_bytes : 0;
        const usize out_bytes = kind == Kind::int_out ? 4 : kind == Kind::termios_out ? Codec::termios_bytes
                              : kind == Kind::window_out ? Codec::window_bytes : 0;
        struct
        {
            Status describe{}, check{}, control{};
            i64 value{};
            bool copy_in = true, writable = true, copy_out = true;
        } seam;
        usize checks{}, inputs{}, controls{}, ranges{}, outputs{}, input_size{}, output_size{};
        sched::TerminalRequest seen{};
        const auto run = [&](std::optional<Op> selected, u64 address = 0x1000) {
            checks = inputs = controls = ranges = outputs = input_size = output_size = 0;
            ++order_cases;
            return cc::dispatch<Codec>(3, selected, address,
                [&](i32 fd) -> Result<sched::TerminalSubject> {
                    CHECK(fd == 3);
                    if (!seam.describe) return seam.describe;
                    return sched::TerminalSubject{7, sched::console_terminal_ref, sched::TerminalSide::console};
                },
                [&](const sched::TerminalSubject &subject, Op requested) {
                    CHECK(subject.description == 7 && requested == operation);
                    ++checks;
                    return seam.check;
                },
                [&](u64, std::span<u8> bytes) {
                    ++inputs;
                    input_size = bytes.size();
                    for (auto &byte : bytes) byte = 0;
                    return seam.copy_in ? Status{} : Status::fault();
                },
                [&](const sched::TerminalSubject &, const sched::TerminalRequest &request) -> Result<sched::TerminalReply> {
                    ++controls;
                    seen = request;
                    if (!seam.control) return seam.control;
                    sched::TerminalReply reply{};
                    reply.value = seam.value;
                    return reply;
                },
                [&](u64, usize bytes) { ++ranges; output_size = bytes; return seam.writable; },
                [&](u64, std::span<const u8> bytes) {
                    ++outputs;
                    CHECK(bytes.size() == output_size);
                    return seam.copy_out ? Status{} : Status::fault();
                });
        };
        const auto untouched = [&] { return !inputs && !controls && !ranges && !outputs; };
        seam.describe = Status::bad_descriptor();
        CHECK(run(operation) == -9 && !checks && untouched());
        seam.describe = Status::not_terminal();
        CHECK(run(operation) == -25 && !checks && untouched());
        seam.describe = {};
        CHECK(run(std::nullopt) == -25 && !checks && untouched());
        CHECK(run(Op::count) == -25 && !checks && untouched());
        seam.check = Status::not_terminal();
        CHECK(run(operation) == -25 && checks == 1 && untouched());
        seam.check = {};
        if (in_bytes)
        {
            seam.copy_in = false;
            CHECK(run(operation) == -14 && inputs == 1 && input_size == in_bytes && !controls && !outputs);
            seam.copy_in = true;
        }
        for (const auto &[status, result] : std::array<std::pair<Status, i64>, 4>{{
                 {Status::invalid_argument(), -22}, {Status::no_process(), -3}, {Status::permission(), -1},
                 {Status::not_terminal(), -25}}})
        {
            seam.control = status;
            CHECK(run(operation) == result && controls == 1 && !ranges && !outputs && inputs == (in_bytes ? 1u : 0u));
        }
        seam.control = {};
        if (kind == Kind::scalar)
        {
            // Only TCFLSH keeps the whole word, so its high bits reach control as invalid.
            CHECK(run(operation, 0xffffffff00000002ULL) == 0 && seen.scalar == (operation == Op::flush ? -1 : 2) &&
                  untouched() == false);
            CHECK(run(operation, 0xffffffffULL) == 0 && seen.scalar == -1);
        }
        if (kind == Kind::int_out)
        {
            for (const i64 value : {i64{-1}, i64{0x80000000}})
            {
                seam.value = value;
                CHECK(run(operation) == -75 && controls == 1 && !ranges && !outputs);
            }
            seam.value = 0x7fffffff;
        }
        if (out_bytes)
        {
            seam.writable = false;
            CHECK(run(operation) == -14 && ranges == 1 && output_size == out_bytes && !outputs);
            seam.writable = true;
            seam.copy_out = false;
            CHECK(run(operation) == -14 && outputs == 1);
            seam.copy_out = true;
            CHECK(run(operation) == 0 && outputs == 1 && output_size == out_bytes);
        }
        else
            CHECK(run(operation) == seam.value && !ranges && !outputs);
        CHECK(seen.operation == operation);
    }
}

// Native requests are uniform on all ten profiles (ok/uapi/syscall.h).
void native_table()
{
    const std::array<std::pair<u64, Op>, 16> mapped{{
        {OK_TIOCSCTTY, Op::acquire}, {OK_TIOCGSID, Op::get_session}, {OK_TIOCGPGRP, Op::get_foreground},
        {OK_TIOCSPGRP, Op::set_foreground}, {OK_TCGETS, Op::get_attributes}, {OK_TCSETS, Op::set_now},
        {OK_TCSETSW, Op::set_drain}, {OK_TCSETSF, Op::set_flush}, {OK_TCSBRK, Op::drain_break},
        {OK_TCFLSH, Op::flush}, {OK_TIOCGWINSZ, Op::get_window}, {OK_TIOCSWINSZ, Op::set_window},
        {OK_FIONREAD, Op::input_pending}, {OK_TIOCGPTN, Op::get_pty_number}, {OK_TIOCSPTLCK, Op::set_pty_lock},
        {OK_TIOCNOTTY, Op::detach}}};
    for (const auto &[request, operation] : mapped)
    {
        CHECK(cc::native_operation(request) == operation);
        // The native number is the asm-generic Linux number of the same request.
        CHECK(lu::TtyIoctlCodec<lu::Profile::x86_64>::operation(request) ==
              static_cast<lu::TerminalIoctl>(operation));
        ++native_requests;
    }
    // These never exist natively.
    for (const u64 refused : {u64{0x540a}, u64{0x5411},
                              u64{0x5412}, u64{0x802c542a}, u64{0x5420}, u64{0x80045440}, u64{0}})
        CHECK(!cc::native_operation(refused));
    CHECK(!cc::native_operation((u64{1} << 32) | OK_TCGETS)); // The native request word is not truncated.
}

void activate(Kernel &kernel, Pid pid)
{
    auto *process = kernel.scheduler().find(pid); CHECK(process);
    if (process->state() == sched::ProcessState::created) CHECK(kernel.scheduler().set_runnable(pid));
    process->threads()[0].native_active = true;
    CHECK(kernel.posix().activate(pid));
}

i64 call(Kernel &kernel, Pid pid, N number, std::array<uptr, 6> args = {})
{
    activate(kernel, pid);
    bool done = true;
    const auto result = native::dispatch(kernel, pid, {static_cast<uptr>(number), args}, done);
    CHECK(done);
    return result;
}

i64 ioctl(Kernel &kernel, Pid pid, u64 command, uptr argument = 0)
{
    return call(kernel, pid, N::ioctl, {0, static_cast<uptr>(command), argument});
}

Pid fork(Kernel &kernel, Pid parent)
{
    activate(kernel, parent);
    const auto child = kernel.scheduler().fork_process(parent); CHECK(child);
    return child.value();
}

template <lu::Profile Profile>
i64 linux_ioctl(Kernel &kernel, Pid pid, lu::TerminalIoctl operation, u64 argument)
{
    using Codec = lu::Codec<Profile>;
    activate(kernel, pid);
    auto *process = kernel.scheduler().find(pid);
    process->resources().personality = sched::UserPersonality::linux;
    hw::UserFrame frame{};
    frame.registers.fill(~uptr{0});
    constexpr usize number = Profile == lu::Profile::i386 ? 7 :
                             Profile == lu::Profile::x86_64 ? 14 : Codec::profile.number_register;
    constexpr std::array<usize, 3> slots = Profile == lu::Profile::i386 ? std::array<usize, 3>{4, 6, 5} :
        Profile == lu::Profile::x86_64 ? std::array<usize, 3>{9, 10, 11} :
        std::array<usize, 3>{static_cast<usize>(Codec::profile.argument_registers[0]),
                             static_cast<usize>(Codec::profile.argument_registers[1]),
                             static_cast<usize>(Codec::profile.argument_registers[2])};
    frame.registers[number] = *Codec::syscall_number(lu::Operation::ioctl);
    frame.registers[slots[0]] = 0;
    frame.registers[slots[1]] = *lu::TtyIoctlCodec<Profile>::number(operation);
    frame.registers[slots[2]] = argument;
    bool done = true;
    const auto result = user::linux_bootstrap::dispatch<Profile>(kernel, pid, frame, done,
        [&](hw::Syscall, bool &) -> i64 { CHECK(false); return -5; });
    CHECK(done);
    process->resources().personality = sched::UserPersonality::native;
    return result;
}

std::span<u8> user_bytes(usize count)
{
    return {reinterpret_cast<u8 *>(mock::memory.data()) + (output - hw::user_begin), count};
}

// A Linux TCSETS is seen by a native TCGETS in the generic encoding and comes
// back byte for byte through the Linux TCGETS (flags, slots and residue).
template <lu::Profile Profile> void console_profile(Kernel &kernel, Pid login)
{
    using Tty = lu::TtyIoctlCodec<Profile>;
    using T = lu::TerminalIoctl;
    using Wire = tty::NativeTermiosWire<std::endian::native == std::endian::big>;
    auto &console = kernel.scheduler().console_terminal();
    mock::memory.fill(std::byte{0x5a});
    CHECK(linux_ioctl<Profile>(kernel, login, T::get_attributes, output) == 0);
    CHECK(mock::memory[output - hw::user_begin + Tty::termios_bytes] == std::byte{0x5a});
    tty::Termios termios{};
    tty::ForeignResidue residue{};
    CHECK(Tty::decode_termios(user_bytes(Tty::termios_bytes), termios, residue));
    CHECK(termios == tty::console_compat_termios());
    auto cooked = tty::linux_std_termios();
    cooked[tty::Cc::erase] = 0x08;
    std::array<u8, Tty::termios_bytes> sent{};
    CHECK(Tty::encode_termios(cooked, {}, sent));
    if constexpr (Profile == lu::Profile::mips || Profile == lu::Profile::mips64)
        sent[17 + 11] = 0x19; // VDSUSP has no generic slot: it rides in the residue.
    std::memcpy(mock::memory.data() + (output - hw::user_begin), sent.data(), sent.size());
    CHECK(linux_ioctl<Profile>(kernel, login, T::set_now, output) == 0 && console.termios == cooked);
    mock::memory.fill(std::byte{});
    CHECK(ioctl(kernel, login, OK_TCGETS, output) == 0);
    CHECK(Wire::decode(user_bytes(36), termios, residue) && termios == cooked);
    mock::memory.fill(std::byte{});
    CHECK(linux_ioctl<Profile>(kernel, login, T::get_attributes, output) == 0);
    CHECK(std::memcmp(user_bytes(sent.size()).data(), sent.data(), sent.size()) == 0);
    // Back to the compat default through the native wire, which clears the residue.
    std::array<u8, 36> compat{};
    CHECK(Wire::encode(tty::console_compat_termios(), {}, compat));
    std::memcpy(mock::memory.data() + (output - hw::user_begin), compat.data(), compat.size());
    CHECK(ioctl(kernel, login, OK_TCSETS, output) == 0 && console.termios == tty::console_compat_termios());
    CHECK(console.residue == tty::ForeignResidue{});
    // Window and FIONREAD are 8 and 4 bytes on every profile.
    mock::memory.fill(std::byte{0x5a});
    CHECK(linux_ioctl<Profile>(kernel, login, T::get_window, output) == 0);
    CHECK(mock::memory[output - hw::user_begin + 8] == std::byte{0x5a});
    tty::WinSize window{};
    CHECK(Tty::decode_window(user_bytes(8), window) && window == tty::console_window());
    native::input('z');
    mock::memory.fill(std::byte{0x5a});
    CHECK(linux_ioctl<Profile>(kernel, login, T::input_pending, output) == 0);
    CHECK(Tty::decode_int(user_bytes(4)).value() == 1 && mock::memory[output - hw::user_begin + 4] == std::byte{0x5a});
    CHECK(linux_ioctl<Profile>(kernel, login, T::flush, 0) == 0 && !console.ldisc.buffered());
    CHECK(linux_ioctl<Profile>(kernel, login, T::flush, 3) == -22);
    ++linux_profiles;
}

void console(Kernel &kernel)
{
    auto &scheduler = kernel.scheduler();
    const auto context = kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096});
    const auto init = scheduler.create_init_process(context); CHECK(init && init.value() == 1);
    native::runtime_kernel = &kernel;
    // Before any lease: anyone reads the compat default; nothing else is open.
    CHECK(ioctl(kernel, 1, OK_TCGETS, output) == 0 && ioctl(kernel, 1, OK_TCSETS, output) == -25);
    const auto login = fork(kernel, 1);
    CHECK(call(kernel, 1, N::console_claim, {login}) > 0);
    CHECK(call(kernel, login, N::setsid) == static_cast<i64>(login));
    CHECK(ioctl(kernel, login, OK_TIOCSCTTY) == 0);
    console_profile<lu::Profile::i386>(kernel, login);
    console_profile<lu::Profile::x86_64>(kernel, login);
    console_profile<lu::Profile::aarch64>(kernel, login);
    console_profile<lu::Profile::arm32>(kernel, login);
    console_profile<lu::Profile::rv64>(kernel, login);
    console_profile<lu::Profile::rv32>(kernel, login);
    console_profile<lu::Profile::loongarch64>(kernel, login);
    console_profile<lu::Profile::mips>(kernel, login);
    console_profile<lu::Profile::mips64>(kernel, login);
    console_profile<lu::Profile::ppc>(kernel, login);
    // Live error order on the console: association (ENOTTY) before copy-in (EFAULT).
    CHECK(ioctl(kernel, 1, OK_TCSETS, hw::user_end) == -25 && ioctl(kernel, login, OK_TCSETS, hw::user_end) == -14);
    CHECK(ioctl(kernel, 1, OK_TIOCSPGRP, hw::user_end) == -25 && ioctl(kernel, login, OK_TIOCSPGRP, hw::user_end) == -14);
    CHECK(ioctl(kernel, login, OK_TCGETS, hw::user_end) == -14);
    CHECK(call(kernel, 1, N::console_revoke, {static_cast<uptr>(scheduler.find(login)->console_epoch())}) == 0);
    // Revocation resets the whole terminal.
    CHECK(scheduler.console_terminal().termios == tty::console_compat_termios() &&
          !scheduler.console_terminal().session && !scheduler.console_terminal().ldisc.buffered());
    CHECK(scheduler.exit_process(login, 0));
    native::runtime_kernel = nullptr;
}
} // namespace

int main()
{
    error_order<cc::NativeTerminalCodec<false>>();
    error_order<cc::NativeTerminalCodec<true>>();
    error_order<lu::TtyIoctlCodec<lu::Profile::i386>>();
    error_order<lu::TtyIoctlCodec<lu::Profile::x86_64>>();
    error_order<lu::TtyIoctlCodec<lu::Profile::aarch64>>();
    error_order<lu::TtyIoctlCodec<lu::Profile::arm32>>();
    error_order<lu::TtyIoctlCodec<lu::Profile::rv64>>();
    error_order<lu::TtyIoctlCodec<lu::Profile::rv32>>();
    error_order<lu::TtyIoctlCodec<lu::Profile::loongarch64>>();
    error_order<lu::TtyIoctlCodec<lu::Profile::mips>>();
    error_order<lu::TtyIoctlCodec<lu::Profile::mips64>>();
    error_order<lu::TtyIoctlCodec<lu::Profile::ppc>>();
    native_table();
    static Kernel kernel; CHECK(kernel.boot({}));
    console(kernel);
    std::printf("TERMINAL_CONTROL_PASS native_requests=%zu linux_profiles=%zu console=1 error_order=%d hosted=1 guest=0\n",
                native_requests, linux_profiles, order_cases > 0 ? 1 : 0);
}
