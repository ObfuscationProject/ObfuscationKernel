#include "ok/user/native_syscall_restart.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
using Profile = abi::linux_user::Profile;
using Frame = arch::execution::UserFrame;
using Trap = arch::execution::Trap;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::exit(1); } } while (0)

static bool equal(const Frame &a, const Frame &b)
{
    return a.registers == b.registers && a.pc == b.pc && a.sp == b.sp &&
        a.flags == b.flags && a.fs_base == b.fs_base && a.gs_base == b.gs_base && a.fp_state == b.fp_state;
}

template <Profile P> static void exercise_once(std::span<const u8> code, usize number_slot,
    uptr number, Trap trap, bool thumb, bool writing)
{
    using A = user::native::RestartAdapter<P>;
    constexpr uptr begin = 0x10000000, end = 0x20000000, instruction = 0x10002000;
    Frame original;
    for (usize i = 0; i < original.registers.size(); ++i) original.registers[i] = 0x4100 + i * 31;
    for (usize i = 0; i < original.fp_state.size(); ++i) original.fp_state[i] = static_cast<u8>(i * 11 + 0x29);
    original.registers[number_slot] = number;
    original.pc = instruction + code.size(); original.sp = 0x18004000;
    original.flags = P == Profile::arm32 ? (thumb ? 0x8400f830U : 0x84000010U) : 0x1234;
    original.fs_base = 0x18000000; original.gs_base = 0x18001000;
    if constexpr (P == Profile::arm32) original.registers[15] = original.pc;
    auto expected = original;
    expected.pc = instruction;
    if constexpr (P == Profile::arm32) expected.registers[15] = instruction;
    if constexpr (P == Profile::ppc) expected.fp_state[280] |= 0x10;
    CHECK(A::family(original) == (writing ? A::Family::pipe_write : A::Family::pipe_read));
    const auto prepared = A::prepare(original, trap, code, begin, end);
    CHECK(prepared);
    CHECK(equal(prepared.value().frame, expected));
    CHECK(prepared.value().site.instruction == instruction);
    CHECK(prepared.value().site.bytes == code.size());
    CHECK(prepared.value().site.thumb == thumb);
    CHECK(prepared.value().ppc_result == 0);
    for (usize i = 0; i < code.size(); ++i) CHECK(prepared.value().opcode[i] == code[i]);
    // Register sentinels include nonboolean a3. Full comparison also covers all
    // FP/SPE, TLS, status and x86 syscall-clobbered state, not just arguments.
    if constexpr (P == Profile::mips || P == Profile::mips64)
        CHECK(prepared.value().frame.registers[7] == 0x41d9);
    if constexpr (P == Profile::ppc)
    {
        auto so = original; so.fp_state[280] |= 0x10;
        auto again = A::prepare(so, trap, code, begin, end);
        CHECK(again && equal(again.value().frame, expected));
        CHECK(again.value().frame.registers[3] == 0x415d);
    }
    auto bad_trap = trap; bad_trap.kind = arch::execution::TrapKind::timer;
    CHECK(!A::prepare(original, bad_trap, code, begin, end));
    bad_trap = trap; bad_trap.vector ^= 1;
    CHECK(!A::prepare(original, bad_trap, code, begin, end));
    if constexpr (P == Profile::aarch64)
    {
        bad_trap = trap; bad_trap.vector = 8;
        CHECK(!A::prepare(original, bad_trap, code, begin, end));
        bad_trap = trap; bad_trap.error_code = 0;
        CHECK(!A::prepare(original, bad_trap, code, begin, end));
    }
    if constexpr (P == Profile::mips || P == Profile::mips64)
    {
        bad_trap = trap; bad_trap.error_code |= 0x80000000ULL;
        CHECK(!A::prepare(original, bad_trap, code, begin, end));
        // A normalized taken branch can point far past its delay instruction.
        auto branch = original; branch.pc += 8;
        CHECK(!A::prepare(branch, bad_trap, code, begin, end));
    }
    auto bad = original; bad.registers[number_slot] = 123456;
    CHECK(!A::prepare(bad, trap, code, begin, end));
    CHECK(!A::prepare(original, trap, code.first(code.size() - 1), begin, end));
    const std::array<u8, 4> wrong{};
    CHECK(!A::prepare(original, trap, std::span{wrong}.first(code.size()), begin, end));
    typename A::Site unsafe{instruction, 255, thumb};
    std::array<u8, 255> oversized{};
    CHECK(!A::instruction(unsafe, oversized));
    auto with_pc = [&](uptr pc) {
        auto f = original; f.pc = pc;
        if constexpr (P == Profile::arm32) f.registers[15] = pc;
        return f;
    };
    CHECK(!A::prepare(with_pc(0), trap, code, begin, end));
    CHECK(!A::prepare(with_pc(begin + code.size() - 1), trap, code, begin, end));
    CHECK(!A::prepare(with_pc(end + code.size()), trap, code, begin, end));
    CHECK(!A::prepare(original, trap, code, end, begin));
    CHECK(A::prepare(with_pc(begin + code.size()), trap, code, begin, end));
    CHECK(A::prepare(with_pc(end), trap, code, begin, end));
    if constexpr (P != Profile::i386 && P != Profile::x86_64)
        CHECK(!A::prepare(with_pc(original.pc + 1), trap, code, begin, end));
    if constexpr (P == Profile::rv32 || P == Profile::rv64)
    {
        const auto compressed_neighbor = A::prepare(with_pc(instruction + 6), trap, code, begin, end);
        CHECK(compressed_neighbor && compressed_neighbor.value().frame.pc == instruction + 2);
    }
    if constexpr (abi::linux_user::Codec<P>::profile.word_bytes == 4)
    {
        bad = original; bad.pc |= (uptr{1} << 32);
        CHECK(!A::prepare(bad, trap, code, begin, end));
        bad = original; bad.registers[number_slot] |= (uptr{1} << 32);
        CHECK(!A::prepare(bad, trap, code, begin, end));
        bad = original; bad.registers[21] |= (uptr{1} << 32);
        CHECK(!A::prepare(bad, trap, code, begin, end));
    }
    unsigned copies = 0;
    auto copy = [&](uptr address, std::span<u8> destination) -> Status {
        ++copies; CHECK(address == instruction); CHECK(destination.size() == code.size());
        for (usize i = 0; i < code.size(); ++i) destination[i] = code[i];
        return Status::success();
    };
    auto copied = A::prepare_copy(original, trap, begin, end, copy);
    CHECK(copied && equal(copied.value().frame, expected) && copies == 1);
    CHECK(!A::prepare_copy(original, bad_trap, begin, end, copy) && copies == 1);
    auto denied = [&](uptr, std::span<u8>) -> Status { ++copies; return Status::fault("no executable mapping"); };
    CHECK(!A::prepare_copy(original, trap, begin, end, denied) && copies == 2);
    CHECK(equal(original, with_pc(original.pc)));
    if constexpr (P == Profile::x86_64 || P == Profile::i386 || P == Profile::rv32 || P == Profile::rv64)
    {
        // A real callback must admit both executable pages. The helper requests
        // the complete crossing instruction once and propagates access failure.
        const auto crossing = with_pc(begin + 4096 + (code.size() == 2 ? 1 : 2));
        unsigned crossing_copies = 0;
        auto cross = [&](uptr address, std::span<u8> bytes) -> Status {
            ++crossing_copies; CHECK(address < begin + 4096 && address + bytes.size() > begin + 4096);
            return Status::fault("second page is nonexecutable");
        };
        CHECK(!A::prepare_copy(crossing, trap, begin, end, cross) && crossing_copies == 1);
    }
}

template <Profile P> static void exercise(std::span<const u8> code, usize number_slot,
    uptr read_number, Trap trap, bool thumb = false)
{
    using Codec = abi::linux_user::Codec<P>;
    const auto write = Codec::syscall_number(abi::linux_user::Operation::write);
    CHECK(write && *write == read_number + 1);
    exercise_once<P>(code, number_slot, read_number, trap, thumb, false);
    exercise_once<P>(code, number_slot, *write, trap, thumb, true);
    const auto readv = Codec::syscall_number(abi::linux_user::Operation::readv);
    const auto writev = Codec::syscall_number(abi::linux_user::Operation::writev);
    CHECK(readv && writev);
    exercise_once<P>(code, number_slot, *readv, trap, thumb, false);
    exercise_once<P>(code, number_slot, *writev, trap, thumb, true);
}

int main()
{
    using K = arch::execution::TrapKind;
    constexpr u8 x64[]{0x0f, 0x05}, x32[]{0xcd, 0x80}, a64[]{1, 0, 0, 0xd4};
    constexpr u8 arm[]{0, 0, 0, 0xef}, thumb[]{0, 0xdf}, rv[]{0x73, 0, 0, 0};
    constexpr u8 la[]{0, 0, 0x2b, 0}, mips[]{0, 0, 0, 0x0c}, ppc[]{0x44, 0, 0, 2};
    exercise<Profile::x86_64>(x64, 14, 0, {K::syscall, 256, 0});
    exercise<Profile::i386>(x32, 7, 3, {K::syscall, 128, 0});
    exercise<Profile::aarch64>(a64, 8, 63, {K::syscall, 0x15, 0x54000000});
    exercise<Profile::arm32>(arm, 7, 3, {K::syscall, 2, 0});
    exercise<Profile::arm32>(thumb, 7, 3, {K::syscall, 2, 0}, true);
    exercise<Profile::rv64>(rv, 17, 63, {K::syscall, 8, 8});
    exercise<Profile::rv32>(rv, 17, 63, {K::syscall, 8, 8});
    exercise<Profile::loongarch64>(la, 11, 63, {K::syscall, 11, 0});
    exercise<Profile::mips>(mips, 2, 4003, {K::syscall, 8, 32});
    exercise<Profile::mips64>(mips, 2, 5000, {K::syscall, 8, 32});
    exercise<Profile::ppc>(ppc, 0, 3, {K::syscall, 8, 0});
    std::printf("RESTART_ADAPTER_PASS profiles=10 variants=44 checks=%u hosted=1 guest=0\n", checks);
}
