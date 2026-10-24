#include "ok/user/native_signal_mask.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace ok;
using namespace ok::user::native;
namespace lu = ok::abi::linux_user;
static usize checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::abort(); } } while (0)
template <lu::Profile Profile> void test()
{
    using Codec = SignalMaskCodec<Profile>;
    constexpr bool mips = Profile == lu::Profile::mips || Profile == lu::Profile::mips64;
    constexpr bool big = mips || Profile == lu::Profile::ppc;
    constexpr bool big64 = Profile == lu::Profile::mips64;
    constexpr usize size = mips ? 16 : 8;
    constexpr i32 block = mips ? 1 : 0, unblock = block + 1, replace = block + 2;
    CHECK(Codec::bytes == size && Codec::block == block && Codec::unblock == unblock && Codec::replace == replace);
    SignalMask mask{{0x80000001c0000001ULL, mips ? 0xc000000000000001ULL : 0}};
    constexpr u8 little[] = {1,0,0,0xc0,1,0,0,0x80,1,0,0,0,0,0,0,0xc0};
    constexpr u8 be32[] = {0xc0,0,0,1,0x80,0,0,1,0,0,0,1,0xc0,0,0,0};
    constexpr u8 be64[] = {0x80,0,0,1,0xc0,0,0,1,0xc0,0,0,0,0,0,0,1};
    const auto *golden = big64 ? be64 : big ? be32 : little;
    const auto encoded = Codec::encode(mask);
    for (usize i = 0; i < size; ++i) CHECK(encoded[i] == static_cast<std::byte>(golden[i]));
    CHECK(Codec::decode(encoded) == mask);
    for (u32 signal : {1,31,32,33,64}) CHECK(mask.contains(signal));
    for (u32 signal : {65,127,128}) CHECK(mask.contains(signal) == mips);
    SignalMask every{{~u64{0}, ~u64{0}}}; const auto sanitized = Codec::sanitize(every);
    for (u32 signal = 1; signal <= 128; ++signal)
        CHECK(sanitized.contains(signal) == (signal <= size * 8 && signal != 9 && signal != (mips ? 23 : 19)));
    CHECK(!mask.contains(0) && !mask.contains(129));
    auto unchanged = mask; mask.set(0); mask.set(129); CHECK(mask == unchanged);
    std::array<std::byte, 64> memory{};
    for (usize i = 0; i < size; ++i) memory[8 + i] = encoded[i];
    usize reads = 0, writes = 0; bool read_fault = false, write_fault = false;
    SignalMask current{};
    auto read = [&](u64 address, std::span<std::byte> out) {
        ++reads; if (read_fault) return Status::fault();
        CHECK(address <= memory.size() - out.size()); std::memcpy(out.data(), memory.data() + address, out.size()); return Status::success();
    };
    auto write = [&](u64 address, std::span<const std::byte> in) {
        ++writes; if (write_fault) return Status::fault();
        CHECK(address <= memory.size() - in.size()); std::memcpy(memory.data() + address, in.data(), in.size()); return Status::success();
    };
    auto call = [&](i32 how, u64 input, u64 output, u64 bytes = Codec::bytes) {
        return update_signal_mask<Profile>(current, how, input, output, bytes, read, write);
    };
    CHECK(call(999, 8, 32, size + 1) == -22 && !reads && !writes);
    read_fault = true; CHECK(call(999, 8, 32) == -14 && reads == 1 && !writes);
    read_fault = false; CHECK(call(999, 8, 32) == -22 && reads == 2 && !writes && current == SignalMask{});
    CHECK(call(replace, 8, 8) == 0 && current == mask);
    for (usize i = 0; i < size; ++i) CHECK(memory[8 + i] == std::byte{});
    CHECK(call(unblock, 8, 0) == 0 && current == mask);
    std::memcpy(memory.data() + 8, encoded.data(), size);
    write_fault = true; CHECK(call(unblock, 8, 32) == -14 && current == SignalMask{});
    write_fault = false; CHECK(call(block, 8, 9) == 0 && current == mask);
    for (usize i = 0; i < size; ++i) CHECK(memory[9 + i] == std::byte{});
    const auto before = reads; CHECK(call(999, 0, 32) == 0 && reads == before && current == mask);
    for (usize i = 0; i < size; ++i) CHECK(memory[32 + i] == encoded[i]);
    CHECK(call(999, 0, 0) == 0 && current == mask);
    std::memset(memory.data() + 8, 0xff, size);
    CHECK(call(replace, 8, 0) == 0 && current == sanitized);
}
int main()
{
    test<lu::Profile::i386>(); test<lu::Profile::x86_64>(); test<lu::Profile::aarch64>(); test<lu::Profile::arm32>();
    test<lu::Profile::rv64>(); test<lu::Profile::rv32>(); test<lu::Profile::loongarch64>();
    test<lu::Profile::mips>(); test<lu::Profile::mips64>(); test<lu::Profile::ppc>();
    std::printf("PASS signal mask model profiles=10 checks=%zu guest=0 delivery=0 smp=0\n", checks);
}
