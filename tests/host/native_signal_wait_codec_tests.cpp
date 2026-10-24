#include "../../src/user/signals/wait_codec.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
namespace lu = abi::linux_user;
static unsigned checks;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL line=%d check=%s\n", __LINE__, #expression); std::abort(); } } while (0)

struct Layout
{
    const char *name;
    u32 suspend, legacy, modern;
    usize word;
    bool big, mips;
    usize payload;
};

static void integer(std::span<std::byte> output, u64 value, bool big)
{
    for (usize i = 0; i < output.size(); ++i)
    {
        const auto at = big ? output.size() - 1 - i : i;
        output[at] = static_cast<std::byte>(value & 255);
        value /= 256;
    }
}

template <lu::Profile Profile>
static void exercise(Layout layout)
{
    using Codec = user::native::SignalWaitCodec<Profile>;
    using Kind = user::native::SignalWaitKind;
    constexpr u64 input_address = 0x1000, time_address = 0x2000;
    constexpr u32 absent = 0xffffffffu;
    const usize mask_size = layout.mips ? 16 : 8;
    const u32 maximum = static_cast<u32>(mask_size * 8), stop = layout.mips ? 23 : 19;
    std::array<std::byte, 16> mask{}, time{};
    mask.fill(std::byte{0xff});
    usize copies{}, copied_time{};
    auto read = [&](u64 address, std::span<std::byte> destination) {
        ++copies;
        if (address == input_address)
        {
            CHECK(destination.size() == mask_size);
            for (usize i = 0; i < destination.size(); ++i) destination[i] = mask[i];
            return true;
        }
        if (address == time_address)
        {
            copied_time = destination.size();
            CHECK(copied_time == 8 || copied_time == 16);
            for (usize i = 0; i < copied_time; ++i) destination[i] = time[i];
            return true;
        }
        return false;
    };
    auto sentinel = [] {
        typename Codec::Request r{};
        r.wanted.set(3); r.info = 123; r.has_timeout = true; r.timeout = {4, 5};
        return r;
    };
    auto unchanged = [&](const typename Codec::Request &r) {
        CHECK(r.kind == Kind::none && r.wanted == sentinel().wanted && r.info == 123 &&
              r.has_timeout && r.timeout.seconds == 4 && r.timeout.nanoseconds == 5);
    };
    CHECK(Codec::kind(layout.suspend) == Kind::suspend);
    CHECK(Codec::kind(absent) == Kind::none);
    if (layout.legacy != absent) CHECK(Codec::kind(layout.legacy) == Kind::timed);
    if (layout.modern != absent) CHECK(Codec::kind(layout.modern) == Kind::timed);
    if constexpr (Profile == lu::Profile::rv32) CHECK(Codec::kind(137) == Kind::none);

    lu::Call call{layout.suspend, {input_address, mask_size}, 2};
    auto result = sentinel();
    CHECK(Codec::decode(call, read, result) == 0 && copies == 1);
    CHECK(result.kind == Kind::suspend && !result.has_timeout && result.info == 0);
    for (u32 signal = 1; signal <= 128; ++signal)
        CHECK(result.wanted.contains(signal) == (signal <= maximum && signal != 9 && signal != stop));
    const auto selection = Codec::selection_mask(result.wanted);
    for (u32 signal = 1; signal <= 128; ++signal)
        CHECK(selection.contains(signal) == !result.wanted.contains(signal));
    for (u64 size : {u64{0}, u64{mask_size + 1}})
    {
        call.arguments[1] = size; call.arguments[0] = 1; copies = 0; result = sentinel();
        CHECK(Codec::decode(call, read, result) == -22 && copies == 0); unchanged(result);
    }
    call.arguments[1] = mask_size; copies = 0;
    CHECK(Codec::decode(call, read, result) == -14 && copies == 1); unchanged(result);
    call.arguments[0] = input_address; call.argument_count = 1; copies = 0;
    CHECK(Codec::decode(call, read, result) == -22 && copies == 0); unchanged(result);

    for (auto number : {layout.legacy, layout.modern})
    {
        if (number == absent) continue;
        const usize width = number == layout.modern || layout.word == 8 ? 8 : 4;
        call = {number, {input_address, 1, time_address, mask_size}, 4};
        time = {}; copies = copied_time = 0;
        CHECK(Codec::decode(call, read, result) == 0 && copies == 2 && copied_time == 2 * width);
        CHECK(result.kind == Kind::timed && result.info == 1 && result.has_timeout &&
              result.timeout.seconds == 0 && result.timeout.nanoseconds == 0);
        auto rejected = [&](i64 error, usize reads) {
            result = sentinel(); copies = 0;
            CHECK(Codec::decode(call, read, result) == error && copies == reads); unchanged(result);
        };
        call.arguments[3] = 0; call.arguments[0] = 1; call.arguments[2] = 1; rejected(-22, 0);
        call.arguments[3] = mask_size; rejected(-14, 1);
        call.arguments[0] = input_address; rejected(-14, 2);
        call.arguments[2] = time_address;
        integer(std::span<std::byte>{time}.first(width), ~u64{0}, layout.big); rejected(-22, 2);
        time = {}; integer(std::span<std::byte>{time}.subspan(width, width), 1000000000, layout.big); rejected(-22, 2);
        integer(std::span<std::byte>{time}.subspan(width, width), ~u64{0}, layout.big); rejected(-22, 2);
        time = {};
        integer(std::span<std::byte>{time}.first(width), 23, layout.big);
        integer(std::span<std::byte>{time}.subspan(width, width), 999999999, layout.big);
        const auto original_time = time;
        CHECK(Codec::decode(call, read, result) == 0 && result.timeout.seconds == 23 &&
              result.timeout.nanoseconds == 999999999 && time == original_time);
        if (width == 8)
        {
            integer(std::span<std::byte>{time}.first(8), u64{1} << 40, layout.big);
            CHECK(Codec::decode(call, read, result) == 0 && result.timeout.seconds == (i64{1} << 40));
            integer(std::span<std::byte>{time}.subspan(8, 8), 0xffffffff00000007ULL, layout.big);
            if (layout.word == 4)
                CHECK(Codec::decode(call, read, result) == 0 && result.timeout.nanoseconds == 7);
            else rejected(-22, 2);
        }
        call.arguments[2] = 0; copies = 0;
        CHECK(Codec::decode(call, read, result) == 0 && copies == 1 && !result.has_timeout && result.info == 1);
        call.arguments[1] = 0;
        CHECK(Codec::decode(call, read, result) == 0 && result.info == 0);
    }

    for (i32 code : {i32{0}, i32{-6}, i32{128}})
    {
        const sched::PendingSignalInfo info{maximum, code, code == 128 ? 0u : 0x89abcdefu,
                                            code == 128 ? 0u : 0x76543210u};
        std::array<std::byte, 128> actual{}, expected{};
        actual.fill(std::byte{0x55});
        integer(std::span<std::byte>{expected}.first(4), maximum, layout.big);
        integer(std::span<std::byte>{expected}.subspan(layout.mips ? 4 : 8, 4), static_cast<u32>(code), layout.big);
        integer(std::span<std::byte>{expected}.subspan(layout.payload, 4), info.sender_pid, layout.big);
        integer(std::span<std::byte>{expected}.subspan(layout.payload + 4, 4), info.sender_uid, layout.big);
        CHECK(Codec::encode_info(info, actual) && actual == expected);
        for (sched::PendingSignalInfo bad : {sched::PendingSignalInfo{0, 0, 0, 0},
             {maximum + 1, 0, 0, 0}, {10, -1, 1, 2}, {8, 128, 1, 0}, {8, 128, 0, 1}})
            CHECK(!Codec::encode_info(bad, actual) && actual == expected);
    }
    std::printf("SIGNAL_WAIT_CODEC_PROFILE_PASS arch=%s\n", layout.name);
}

int main()
{
    constexpr u32 absent = 0xffffffffu;
    exercise<lu::Profile::x86_64>({"x86_64", 130, 128, absent, 8, false, false, 16});
    exercise<lu::Profile::i386>({"i386", 179, 177, 421, 4, false, false, 12});
    exercise<lu::Profile::aarch64>({"aarch64", 133, 137, absent, 8, false, false, 16});
    exercise<lu::Profile::arm32>({"arm32", 179, 177, 421, 4, false, false, 12});
    exercise<lu::Profile::rv64>({"rv64", 133, 137, absent, 8, false, false, 16});
    exercise<lu::Profile::rv32>({"rv32", 133, absent, 421, 4, false, false, 12});
    exercise<lu::Profile::loongarch64>({"loongarch64", 133, 137, absent, 8, false, false, 16});
    exercise<lu::Profile::mips>({"mips", 4199, 4197, 4421, 4, true, true, 12});
    exercise<lu::Profile::mips64>({"mips64", 5128, 5126, absent, 8, true, true, 16});
    exercise<lu::Profile::ppc>({"ppc", 178, 176, 421, 4, true, false, 12});
    std::printf("SIGNAL_WAIT_CODEC_PASS profiles=10 checks=%u hosted=1 native=0\n", checks);
}
