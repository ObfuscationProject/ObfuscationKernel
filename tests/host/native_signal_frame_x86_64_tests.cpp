#include "ok/arch/x86_64/signal.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <ucontext.h>

using namespace ok;
using Codec = user::native::X86SignalFrame;
static usize checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line=%u\n", __LINE__); std::abort(); } } while (false)
static_assert(offsetof(ucontext_t, uc_mcontext) == 40);
static_assert(offsetof(ucontext_t, uc_sigmask) == 296);
static_assert(offsetof(ucontext_t, uc_stack) == 16);
static_assert(offsetof(ucontext_t, uc_mcontext) + offsetof(mcontext_t, gregs) + REG_RIP * sizeof(greg_t) == 168);
static_assert(sizeof(_libc_fpstate) == 512 && offsetof(_libc_fpstate, mxcsr) == 24);
static_assert(offsetof(mcontext_t, fpregs) == 184);
static_assert(offsetof(mcontext_t, gregs) + REG_OLDMASK * sizeof(greg_t) == 168);

int main()
{
    constexpr u64 begin = 0x40000000, end = 0x80000000;
    Codec::Frame frame{};
    for (usize i = 0; i < frame.registers.size(); ++i) frame.registers[i] = 100 + i;
    frame.pc = begin + 0x1000; frame.sp = begin + 0x8000; frame.flags = 0x202;
    frame.fs_base = begin + 0x2000; frame.gs_base = begin + 0x3000;
    for (usize i = 0; i < frame.fp_state.size(); ++i) frame.fp_state[i] = static_cast<u8>(i * 7);
    frame.fp_state[24] = 0x80; frame.fp_state[25] = 0x1f; frame.fp_state[26] = frame.fp_state[27] = 0;
    Codec::Placement location;
    CHECK(Codec::place(frame.sp, begin, end, location));
    CHECK(location.address % 16 == 8 && location.fp_address % 64 == 0);
    CHECK(location.address + Codec::prefix_bytes <= location.fp_address);
    CHECK(location.fp_address + Codec::fp_bytes <= frame.sp - 128);
    sched::SignalMask mask{}; mask.set(10); mask.set(33); mask.set(64);
    const auto original = Codec::encode(frame, mask, {}, {10, -6, 51, 52}, location, begin + 0x4000, 0xffbf);
    auto image = original;
    CHECK(Codec::get(image.prefix, 0, 8) == begin + 0x4000);
    CHECK(Codec::get(image.prefix, 8, 8) == 6 && Codec::get(image.prefix, 16, 8) == 0);
    CHECK(Codec::get(image.prefix, 32, 4) == 2);
    CHECK(Codec::get(image.prefix, 8 + 168, 8) == frame.pc);
    CHECK(Codec::get(image.prefix, 8 + 296, 8) == mask.words[0]);
    CHECK(Codec::get(image.prefix, 8 + offsetof(ucontext_t, uc_mcontext) + offsetof(mcontext_t, fpregs), 8) == location.fp_address);
    CHECK(Codec::get(image.prefix, 8 + offsetof(ucontext_t, uc_mcontext) + REG_OLDMASK * sizeof(greg_t), 8) == mask.words[0]);
    CHECK(Codec::get(image.prefix, 312, 4) == 10 && Codec::get(image.prefix, 316, 4) == 0);
    CHECK(static_cast<i32>(Codec::get(image.prefix, 320, 4)) == -6);
    CHECK(Codec::get(image.prefix, 328, 4) == 51 && Codec::get(image.prefix, 332, 4) == 52);
    for (usize i = 336; i < image.prefix.size(); ++i) CHECK(image.prefix[i] == std::byte{});
    for (usize i = 416; i < image.fp.size(); ++i) CHECK(image.fp[i] == std::byte{});
    for (usize i = 0; i < 8; ++i)
        for (usize j = 10; j < 16; ++j) CHECK(image.fp[32 + 16 * i + j] == std::byte{});
    bool prefix_fault = false, fp_fault = false;
    u64 fp_source = location.fp_address;
    auto read = [&](u64 address, std::span<std::byte> output) {
        if (address == location.address && output.size() == Codec::restore_bytes && !prefix_fault) {
            for (usize i = 0; i < output.size(); ++i) output[i] = image.prefix[i];
            return true;
        }
        if (address == fp_source && output.size() == Codec::fp_bytes && !fp_fault) {
            for (usize i = 0; i < output.size(); ++i) output[i] = image.fp[i];
            return true;
        }
        return false;
    };
    Codec::Restored restored{};
    auto decode = [&] { return Codec::decode(location.address + 8, frame, begin, end, 0xffbf, read, restored); };
    CHECK(decode());
    CHECK(restored.frame.registers == frame.registers);
    CHECK(restored.frame.sp == frame.sp && restored.frame.pc == frame.pc);
    CHECK(restored.mask.words == mask.words && restored.frame.fs_base == frame.fs_base);
    for (usize i = 0; i < 512; ++i) CHECK(restored.frame.fp_state[i] == static_cast<u8>(image.fp[i]));
    // Real libc handlers edit PC, registers and mask; no private-frame token.
    Codec::put(image.prefix, Codec::context_offset + 128, begin + 0x5000, 8);
    Codec::put(image.prefix, Codec::context_offset + 13 * 8, 0x778899, 8);
    Codec::put(image.prefix, Codec::context_offset + 136, ~u64{}, 8);
    Codec::put(image.prefix, Codec::mask_offset, ~u64{}, 8);
    frame.fs_base += 0x1000;
    CHECK(decode());
    CHECK(restored.frame.pc == begin + 0x5000 && restored.frame.registers[14] == 0x778899);
    CHECK(restored.frame.flags == (Codec::restored_flags_mask | 0x202));
    CHECK((restored.frame.flags & 0x3000) == 0 && restored.frame.fs_base == frame.fs_base);
    CHECK(!restored.mask.contains(9) && !restored.mask.contains(19) && restored.mask.contains(64));
    frame.flags |= 0x200000; Codec::put(image.prefix, Codec::context_offset + 136, 0, 8);
    CHECK(decode() && (restored.frame.flags & 0x200000)); // Preserve current ID, ignore saved ID.
    frame.flags &= ~ok::uptr{0x200000};
    // Failed staging must leave the caller's previous successful result intact.
    auto failure = [&] {
        const auto before = restored;
        CHECK(!decode());
        CHECK(restored.frame.registers == before.frame.registers && restored.frame.pc == before.frame.pc);
        CHECK(restored.frame.fp_state == before.frame.fp_state && restored.mask.words == before.mask.words);
    };
    prefix_fault = true; failure(); prefix_fault = false;
    fp_fault = true; failure(); fp_fault = false;
    for (auto field : {usize{128}, usize{120}}) {
        image = original; Codec::put(image.prefix, Codec::context_offset + field, 0xffff800000000000ULL, 8); failure();
    }
    for (u64 cs : {u64{0}, u64{8}, u64{0x1b}, u64{0x33}}) {
        image = original; Codec::put(image.prefix, Codec::context_offset + 144, cs, 2); failure();
    }
    image = original; Codec::put(image.prefix, Codec::context_offset + 150, 0, 2); failure();
    Codec::put(image.prefix, 8, 0, 8); CHECK(decode()); // Legacy SS fixup.
    for (u64 flags : {u64{1}, u64{7}, u64{8}, ~u64{}}) {
        image = original; Codec::put(image.prefix, 8, flags, 8); failure();
    }
    image = original; Codec::put(image.prefix, 32, 0, 4); failure();
    image = original; Codec::put(image.fp, 24, 0x10000, 4); failure();
    image = original; Codec::put(image.fp, 24, 0x40, 4); failure(); // Unsupported DAZ for selected mask.
    image = original; Codec::put(image.fp, 464, 0x46505853, 4); failure();
    image = original; Codec::put(image.prefix, 232, end - 511, 8); failure();
    image = original; Codec::put(image.prefix, 232, ~u64{16}, 8); failure();
    image = original; Codec::put(image.prefix, 232, 0, 8);
    fp_fault = true; CHECK(decode()); fp_fault = false;
    CHECK(restored.frame.fp_state[0] == 0x7f && restored.frame.fp_state[1] == 3);
    CHECK(restored.frame.fp_state[24] == 0x80 && restored.frame.fp_state[25] == 0x1f);
    image = original; fp_source += 3; Codec::put(image.prefix, 232, fp_source, 8);
    CHECK(decode()); // User pointer alignment is irrelevant to staged FXSAVE storage.
    for (u64 top : {u64{0}, begin, begin + 100, end + 1, ~u64{}}) {
        auto before = location; CHECK(!Codec::place(top, begin, end, location));
        CHECK(location.address == before.address && location.fp_address == before.fp_address);
    }
    for (u64 sp : {u64{0}, u64{7}, begin, end, ~u64{}})
        CHECK(!Codec::decode(sp, frame, begin, end, 0xffbf, read, restored));
    CHECK(Codec::place(begin + 1152, begin, end, location));
    CHECK(location.address == begin + 56 && location.fp_address == begin + 512);
    CHECK(!Codec::place(begin + 1151, begin, end, location));
    for (u64 n = 0; n < 128; ++n) {
        CHECK(Codec::place(frame.sp + n, begin, end, location));
        CHECK(location.address % 16 == 8 && location.fp_address % 64 == 0);
        CHECK(location.fp_address + 512 <= frame.sp + n - 128);
    }
    std::printf("PASS x86_64 signal-frame model checks=%zu guest=0 delivery=0\n", checks);
}
