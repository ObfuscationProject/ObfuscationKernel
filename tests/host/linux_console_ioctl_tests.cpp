#include "ok/abi/linux_console_ioctl.hpp"

#include <array>
#include <cstdio>

using namespace ok;
using namespace ok::abi::linux_user;
namespace
{
usize checks{};
#define CHECK(expression) do { ++checks; if (!(expression)) { std::fprintf(stderr, "console ioctl codec check failed at %d\n", __LINE__); return false; } } while (false)

template <Profile P> bool profile()
{
    using Console = ConsoleIoctlCodec<P>;
    constexpr bool mips = P == Profile::mips || P == Profile::mips64;
    constexpr bool encoded = mips || P == Profile::ppc;
    constexpr std::array<u32, 4> expected{mips ? 0x5480u : 0x540eu,
        mips ? 0x7416u : 0x5429u, encoded ? 0x40047477u : 0x540fu,
        encoded ? 0x80047476u : 0x5410u};
    CHECK(Codec<P>::errno_number(Error::e_notty) == 25);
    CHECK(Console::pid_bytes == 4);
    for (usize index = 0; index < expected.size(); ++index)
    {
        const auto selected = static_cast<ConsoleIoctlOperation>(index);
        CHECK(Console::number(selected) == expected[index]);
        CHECK(Console::operation(expected[index]) == selected);
        CHECK(Console::operation(0xffffffff00000000ULL | expected[index]) == selected);
        std::array<u64, 3> words{0xabcdef00fffffffdULL,
            0x1234567800000000ULL | expected[index], 0x87654321fedcba98ULL};
        auto decoded = Console::decode(words);
        CHECK(decoded && decoded.value().fd == -3 && decoded.value().operation == selected);
        CHECK(decoded.value().argument == (words[2] & Codec<P>::word_mask));
        for (usize count = 0; count < words.size(); ++count)
            CHECK(!Console::decode({words.data(), count}));
    }
    CHECK(!Console::number(ConsoleIoctlOperation::count));
    CHECK(!Console::number(static_cast<ConsoleIoctlOperation>(255)));
    for (u32 unknown : {0u, 0x5401u, 0x40087477u, 0x80087476u, 0xffffffffu})
    {
        CHECK(!Console::operation(unknown));
        const std::array<u64, 3> words{0, unknown, 0};
        auto decoded = Console::decode(words);
        CHECK(!decoded && decoded.status().code() == StatusCode::not_terminal);
    }
    const std::array<u64, 4> too_many{0, expected[0], 0, 0};
    CHECK(!Console::decode(too_many));
    CHECK(Console::scalar_argument(0x12345678ffffffffULL) == -1);
    CHECK(Console::scalar_argument(0x1234567880000000ULL) == (-2147483647 - 1));
    CHECK(Console::scalar_argument(0x1234567800000000ULL) == 0);
    std::array<u8, 4> bytes{};
    CHECK(Console::encode_pid(bytes, 0x12345678));
    const std::array<u8, 4> literal = Codec<P>::profile.big_endian ?
        std::array<u8, 4>{0x12, 0x34, 0x56, 0x78} : std::array<u8, 4>{0x78, 0x56, 0x34, 0x12};
    CHECK(bytes == literal);
    CHECK(Console::decode_pid(literal) && Console::decode_pid(literal).value() == 0x12345678);
    for (i32 pid : {0, 1, -1, 2147483647, -2147483647 - 1})
    {
        CHECK(Console::encode_pid(bytes, pid));
        auto decoded = Console::decode_pid(bytes);
        CHECK(decoded && decoded.value() == pid);
    }
    std::array<u8, 8> guarded{1, 2, 3, 4, 5, 6, 7, 8};
    const auto original = guarded;
    for (usize size = 0; size <= guarded.size(); ++size)
    {
        if (size == 4)
            continue;
        CHECK(!Console::decode_pid({guarded.data(), size}));
        CHECK(!Console::encode_pid({guarded.data(), size}, 42));
        CHECK(guarded == original);
    }
    CHECK(Console::encode_pid({guarded.data() + 2, 4}, -1));
    CHECK(guarded[0] == 1 && guarded[1] == 2 && guarded[6] == 7 && guarded[7] == 8);
    return true;
}
}

int main()
{
    if (!(profile<Profile::i386>() && profile<Profile::x86_64>() && profile<Profile::aarch64>() &&
          profile<Profile::arm32>() && profile<Profile::rv64>() && profile<Profile::rv32>() &&
          profile<Profile::loongarch64>() && profile<Profile::mips>() && profile<Profile::mips64>() &&
          profile<Profile::ppc>()))
        return 1;
    std::printf("PASS CONSOLE_IOCTL_CODEC_MODEL checks=%zu profiles=10 user-execution=0 guest=0\n", checks);
}
