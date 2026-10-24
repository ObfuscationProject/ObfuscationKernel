#include "ok/user/native_signal_trampoline.hpp"
#include "../fixtures/signals/trampoline_assembly.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::user::native;
using Profile = abi::linux_user::Profile;
using Entry = SignalTrampolineEntry;
static unsigned checks;
#define CHECK(test) do { ++checks; if (!(test)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #test); std::abort(); } } while (0)

template <Profile P> concept Available = requires { typename SignalTrampoline<P>; };
static_assert(static_cast<usize>(Profile::count) == 10);
static_assert(!Available<Profile::i386> && !Available<Profile::x86_64> && !Available<Profile::aarch64>);
static_assert(!Available<Profile::arm32> && !Available<Profile::ppc> && !Available<Profile::count>);
static_assert(Available<Profile::rv32> && Available<Profile::rv64> && Available<Profile::mips>);
static_assert(Available<Profile::mips64> && Available<Profile::loongarch64>);

template <Profile P, usize N> void verify(const std::array<std::byte, N> &assembled, u32 number)
{
    using T = SignalTrampoline<P>;
    CHECK(T::image() == assembled);
    CHECK(T::realtime_number == number);
    CHECK(T::code_bytes == (P == Profile::mips ? 32 : 16));
    CHECK(T::entry_offset(Entry::realtime) == 0);
    CHECK(T::entry_offset(Entry::legacy).has_value() == (P == Profile::mips));
    if constexpr (P == Profile::mips)
    {
        CHECK(T::entry_offset(Entry::legacy) == 16);
        CHECK(T::legacy_number == 4119);
    }
    constexpr u64 begin = 0x40000000, end = 0x80000000;
    for (u64 delta = 0; delta < 2048; ++delta)
    {
        u64 address = 0x12345678;
        const u64 base = end - delta;
        const bool expected = delta >= T::code_bytes && !(base & 3);
        CHECK(T::entry_address(base, Entry::realtime, begin, end, address) == expected);
        CHECK(address == (expected ? base : 0x12345678));
    }
    for (unsigned entry = 0; entry < 256; ++entry)
    {
        u64 address = 0xabcdef;
        const bool expected = entry == 0 || (entry == 1 && T::has_legacy);
        CHECK(T::entry_address(begin, static_cast<Entry>(entry), begin, end, address) == expected);
        CHECK(address == (expected ? begin + (entry == 1 ? 16 : 0) : 0xabcdef));
    }
    for (u64 base : {u64{0}, begin - 4, begin + 1, end, ~u64{}})
    {
        u64 address = 0x55;
        CHECK(!T::entry_address(base, Entry::realtime, begin, end, address));
        CHECK(address == 0x55);
    }
    u64 address = 0x55;
    CHECK(!T::entry_address(begin, Entry::realtime, begin, begin, address));
    CHECK(!T::entry_address(begin, Entry::realtime, end, begin, address));
    CHECK(address == 0x55);
    if constexpr (T::Codec::profile.word_bytes == 4)
    {
        CHECK(!T::entry_address(u64{1} << 32, Entry::realtime, begin, (u64{1} << 32) + 4096, address));
        CHECK(address == 0x55);
    }
    const auto max = T::Codec::word_mask;
    const auto last_base = (max - T::code_bytes) & ~u64{3};
    CHECK(T::entry_address(last_base, Entry::realtime, max - 4096, max, address));
    CHECK(address == last_base);
}

int main()
{
    verify<Profile::rv32>(trampoline_assembly::rv32, 139);
    verify<Profile::rv64>(trampoline_assembly::rv64, 139);
    verify<Profile::mips>(trampoline_assembly::mips, 4193);
    verify<Profile::mips64>(trampoline_assembly::mips64, 5211);
    verify<Profile::loongarch64>(trampoline_assembly::loongarch64, 139);
    std::printf("PASS signal trampoline byte/address model: %u checks; no target execution\n", checks);
}
