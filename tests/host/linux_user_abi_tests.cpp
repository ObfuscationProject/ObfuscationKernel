#include "ok/abi/linux_user.hpp"

#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::abi::linux_user;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!static_cast<bool>(condition))                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                         \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

// Literal vectors are independent of the production byte codecs/generator.
constexpr std::array<u8, 16> time64_le{5, 4, 3, 2, 1, 0, 0, 0, 0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0};
constexpr std::array<u8, 16> time64_be{0, 0, 0, 1, 2, 3, 4, 5, 0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44};
constexpr std::array<u8, 8> time32_le{4, 3, 2, 1, 0x44, 0x33, 0x22, 0x11};
constexpr std::array<u8, 8> time32_be{1, 2, 3, 4, 0x11, 0x22, 0x33, 0x44};
constexpr std::array<u8, 16> negative64_le{0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 5, 0, 0, 0, 0, 0, 0, 0};
constexpr std::array<u8, 16> negative64_be{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0, 0, 0, 0, 0, 0, 0, 5};

struct Expected
{
    u32 write_number;
    u32 exit_group;
    u8 number_register;
    std::array<i8, 6> argument_registers;
    u32 enosys;
};
// Checked against Linux 6.13.4 architecture tables and musl 1.2.5 wrappers.
constexpr std::array<Expected, 10> expected{{
    {4, 252, 0, {3, 1, 2, 6, 7, 5}, 38},
    {1, 231, 0, {5, 4, 1, 10, 8, 9}, 38},
    {64, 94, 8, {0, 1, 2, 3, 4, 5}, 38},
    {4, 248, 7, {0, 1, 2, 3, 4, 5}, 38},
    {64, 94, 17, {10, 11, 12, 13, 14, 15}, 38},
    {64, 94, 17, {10, 11, 12, 13, 14, 15}, 38},
    {64, 94, 11, {4, 5, 6, 7, 8, 9}, 38},
    {4004, 4246, 2, {4, 5, 6, 7, -1, -1}, 89},
    {5001, 5205, 2, {4, 5, 6, 7, 8, 9}, 89},
    {4, 234, 0, {3, 4, 5, 6, 7, 8}, 38},
}};

// Linux 6.13.4 UAPI pairs, in Profile order; PowerPC reverses get/set order.
constexpr std::array<std::array<u32, 2>, 10> robust_numbers{{
    {311, 312}, {273, 274}, {99, 100}, {338, 339}, {99, 100},
    {99, 100}, {99, 100}, {4309, 4310}, {5268, 5269}, {300, 299},
}};

static_assert(Codec<Profile::arm32>::timespec_size(TimeFormat::legacy_timespec) == 8);
static_assert(Codec<Profile::rv32>::timespec_size(TimeFormat::kernel_timespec) == 16);
static_assert(!SupportedProfile<static_cast<Profile>(255)>);
static_assert(Codec<Profile::x86_64>::syscall_number(Operation::write) == 1);
static_assert(Codec<Profile::mips>::errno_number(Error::e_overflow) == 79);
static_assert(!Codec<Profile::x86_64>::syscall_number(Operation::mmap2));
static_assert(Codec<Profile::arm32>::syscall_number(Operation::arm_set_tls) == 0x0f0005);
static_assert(Codec<Profile::rv32>::syscall_number(Operation::clock_gettime64) == 403);
static_assert(!Codec<Profile::rv32>::syscall_number(Operation::clock_gettime32));
static_assert(Codec<Profile::loongarch64>::syscall_number(Operation::statx) == 291);


template <Profile P> void timespec_tests()
{
    using C = Codec<P>;
    const auto &wire64 = C::profile.big_endian ? time64_be : time64_le;
    auto parsed = C::decode_timespec(wire64, TimeFormat::kernel_timespec);
    CHECK(parsed && parsed.value().seconds == 0x0102030405LL && parsed.value().nanoseconds == 0x11223344);
    std::array<u8, 16> output{};
    CHECK(C::encode_timespec(output, TimeFormat::kernel_timespec, {0x0102030405LL, 0x11223344}));
    CHECK(output == wire64);
    parsed = C::decode_timespec(C::profile.big_endian ? negative64_be : negative64_le, TimeFormat::kernel_timespec);
    CHECK(parsed && parsed.value().seconds == -2 && parsed.value().nanoseconds == 5);
    CHECK(C::encode_timespec(output, TimeFormat::kernel_timespec, {-2, 5}));
    CHECK(output == (C::profile.big_endian ? negative64_be : negative64_le));
    CHECK(!C::decode_timespec(std::span<const u8>{wire64}.first(15), TimeFormat::kernel_timespec));
    CHECK(!C::decode_timespec(wire64, static_cast<TimeFormat>(99)));
    output.fill(0xa5);
    const auto unchanged = output;
    CHECK(!C::encode_timespec(output, TimeFormat::kernel_timespec, {0, -1}));
    CHECK(!C::encode_timespec(output, TimeFormat::kernel_timespec, {0, 1000000000}));
    CHECK(!C::encode_timespec(std::span<u8>{output}.first(15), TimeFormat::kernel_timespec, {0, 0}));
    CHECK(output == unchanged);
    // Linux 32-bit get_timespec64 ignores the high tv_nsec padding; 64-bit does not.
    std::array<u8, 16> invalid_ns{};
    invalid_ns[C::profile.big_endian ? 11 : 12] = 1;
    const auto padded = C::decode_timespec(invalid_ns, TimeFormat::kernel_timespec);
    if constexpr (C::profile.word_bytes == 4)
        CHECK(padded && padded.value().nanoseconds == 0);
    else
        CHECK(!padded);
    if constexpr (C::profile.word_bytes == 4)
    {
        const auto &wire32 = C::profile.big_endian ? time32_be : time32_le;
        parsed = C::decode_timespec(wire32, TimeFormat::legacy_timespec);
        CHECK(parsed && parsed.value().seconds == 0x01020304 && parsed.value().nanoseconds == 0x11223344);
        std::array<u8, 8> small{};
        CHECK(C::encode_timespec(small, TimeFormat::legacy_timespec, {0x01020304, 0x11223344}));
        CHECK(small == wire32);
        const auto saved = small;
        CHECK(C::encode_timespec(small, TimeFormat::legacy_timespec, {2147483648LL, 0}).code() == StatusCode::overflow);
        CHECK(small == saved);
        CHECK(C::encode_timespec(small, TimeFormat::legacy_timespec, {-2147483649LL, 0}).code() == StatusCode::overflow);
        CHECK(small == saved);
        CHECK(C::encode_timespec(small, TimeFormat::legacy_timespec, {-2147483648LL, 999999999}));
        parsed = C::decode_timespec(small, TimeFormat::legacy_timespec);
        CHECK(parsed && parsed.value().seconds == -2147483648LL);
    }
    else
    {
        parsed = C::decode_timespec(wire64, TimeFormat::legacy_timespec);
        CHECK(parsed && parsed.value().seconds == 0x0102030405LL);
    }
}

template <Profile P> void call_tests()
{
    using C = Codec<P>;
    const auto &golden = expected[static_cast<usize>(P)];
    CHECK(C::syscall_number(Operation::write) == golden.write_number);
    CHECK(C::syscall_number(Operation::exit_group) == golden.exit_group);
    CHECK(C::syscall_number(Operation::set_robust_list) == robust_numbers[static_cast<usize>(P)][0]);
    CHECK(C::syscall_number(Operation::get_robust_list) == robust_numbers[static_cast<usize>(P)][1]);
    CHECK(C::errno_number(Error::e_nosys) == golden.enosys);
    CHECK(!C::syscall_number(static_cast<Operation>(255)));
    CHECK(!C::errno_number(static_cast<Error>(255)));
    RegisterFile frame;
    for (usize index = 0; index < frame.integer.size(); ++index)
        frame.integer[index] = 0xa000 + index;
    frame.integer[golden.number_register] = golden.write_number;
    constexpr std::array<u8, 24> stack{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       1, 2, 3, 4, 5, 6, 7, 8};
    auto parsed = C::decode_call(frame, 6, stack);
    CHECK(parsed && parsed.value().number == golden.write_number && parsed.value().argument_count == 6);
    for (usize index = 0; index < 6; ++index)
    {
        const i8 slot = golden.argument_registers[index];
        CHECK(parsed.value().arguments[index] == (slot >= 0 ? 0xa000U + static_cast<u64>(slot) : index == 4 ? 0x01020304U : 0x05060708U));
    }
    CHECK(!C::decode_call(frame, 7, stack));
    auto no_args = C::decode_call(frame, 0);
    CHECK(no_args && no_args.value().argument_count == 0);
    if constexpr (P == Profile::mips)
    {
        CHECK(C::decode_call(frame, 4));
        CHECK(C::decode_call(frame, 5, std::span<const u8>{stack}.first(20)));
        CHECK(!C::decode_call(frame, 5, std::span<const u8>{stack}.first(19)));
        CHECK(!C::decode_call(frame, 6, std::span<const u8>{stack}.first(23)));
    }
    if constexpr (C::profile.word_bytes == 4)
    {
        frame.integer[static_cast<usize>(golden.argument_registers[0])] = 0x100000000ULL;
        CHECK(C::decode_call(frame, 1).status().code() == StatusCode::overflow);
    }
    frame.integer[golden.number_register] = 0x100000000ULL;
    CHECK(C::decode_call(frame, 0).status().code() == StatusCode::overflow);
    Call pairs{0, {0x11223344, 0x55667788, 0x11223344, 0x55667788, 0, 0}, 4};
    auto wide = C::decode_argument64(pairs, 0);
    if constexpr (C::profile.word_bytes == 4)
    {
        CHECK(wide && wide.value() == (C::profile.big_endian ? 0x1122334455667788ULL : 0x5566778811223344ULL));
        if constexpr (C::profile.aligned_64bit_pairs)
            CHECK(!C::decode_argument64(pairs, 1));
        else
            CHECK(C::decode_argument64(pairs, 1));
        pairs.argument_count = 1;
        CHECK(!C::decode_argument64(pairs, 0));
        pairs.argument_count = 4;
        pairs.arguments[0] = 0x100000000ULL;
        CHECK(C::decode_argument64(pairs, 0).status().code() == StatusCode::overflow);
    }
    else
        CHECK(wide && wide.value() == 0x11223344);
    pairs.argument_count = 7;
    CHECK(!C::decode_argument64(pairs, 0));
}

template <Profile P> void return_tests()
{
    using C = Codec<P>;
    const u32 enosys = expected[static_cast<usize>(P)].enosys;
    RegisterFile frame;
    frame.integer.fill(0x1234);
    frame.condition = 0xa5a5a5a5;
    CHECK(C::encode_return(frame, {0, enosys}));
    if constexpr (P == Profile::mips || P == Profile::mips64)
    {
        CHECK(frame.integer[2] == 89 && frame.integer[7] == 1);
    }
    else if constexpr (P == Profile::ppc)
    {
        CHECK(frame.integer[3] == 38 && frame.condition == 0xb5a5a5a5);
    }
    else
        CHECK(frame.integer[C::profile.return_register] == (C::profile.word_bytes == 4 ? 0xffffffdaULL : 0xffffffffffffffdaULL));
    auto parsed = C::decode_return(frame);
    CHECK(parsed && parsed.value().error == enosys && parsed.value().value == 0);
    CHECK(C::encode_return(frame, {42, 0}));
    parsed = C::decode_return(frame);
    CHECK(parsed && parsed.value().value == 42 && parsed.value().error == 0);
    if constexpr (P == Profile::mips || P == Profile::mips64)
        CHECK(frame.integer[7] == 0);
    if constexpr (P == Profile::ppc)
        CHECK(frame.condition == 0xa5a5a5a5);
    const auto saved = frame;
    CHECK(!C::encode_return(frame, {1, enosys}));
    CHECK(!C::encode_return(frame, {0, 4096}));
    CHECK(frame.integer == saved.integer && frame.condition == saved.condition);
    if constexpr (C::profile.word_bytes == 4)
    {
        CHECK(C::encode_return(frame, {0x100000000ULL, 0}).code() == StatusCode::overflow);
        CHECK(frame.integer == saved.integer && frame.condition == saved.condition);
        frame.integer[C::profile.return_register] = 0x100000000ULL;
        CHECK(C::decode_return(frame).status().code() == StatusCode::overflow);
        frame = saved;
    }
    if constexpr (C::profile.error_convention == ErrorConvention::negative)
    {
        CHECK(!C::encode_return(frame, {C::word_mask, 0}));
        CHECK(C::encode_return(frame, {C::word_mask - 4095, 0}));
        parsed = C::decode_return(frame);
        CHECK(parsed && parsed.value().error == 0 && parsed.value().value == C::word_mask - 4095);
        CHECK(C::encode_return(frame, {0, 4095}));
        CHECK(C::decode_return(frame).value().error == 4095);
    }
    else
    {
        CHECK(C::encode_return(frame, {C::word_mask, 0}));
        CHECK(C::decode_return(frame).value().value == C::word_mask);
        CHECK(C::encode_return(frame, {0, enosys}));
        frame.integer[C::profile.return_register] = 0;
        CHECK(!C::decode_return(frame));
        if constexpr (P == Profile::mips || P == Profile::mips64)
        {
            frame.integer[7] = 2;
            CHECK(!C::decode_return(frame));
        }
    }
}

template <Profile P> void timeout_return_tests()
{
    using C = Codec<P>;
    constexpr u32 timeout = P == Profile::mips || P == Profile::mips64 ? 145 : 110;
    CHECK(C::errno_number(Error::e_timedout) == timeout);
    RegisterFile frame;
    frame.integer.fill(0x1234);
    frame.condition = 0xa5a5a5a5;
    auto golden = frame;
    if constexpr (P == Profile::mips || P == Profile::mips64)
    {
        golden.integer[2] = 145;
        golden.integer[7] = 1;
    }
    else if constexpr (P == Profile::ppc)
    {
        golden.integer[3] = 110;
        golden.condition = 0xb5a5a5a5;
    }
    else
        golden.integer[C::profile.return_register] = C::profile.word_bytes == 4
            ? 0xffffff92ULL : 0xffffffffffffff92ULL;
    CHECK(C::encode_return(frame, {0, *C::errno_number(Error::e_timedout)}));
    CHECK(frame.integer == golden.integer && frame.condition == golden.condition);
    auto parsed = C::decode_return(frame);
    CHECK(parsed && parsed.value().error == timeout && parsed.value().value == 0);
}

template <Profile P> void run_profile()
{
    timespec_tests<P>();
    call_tests<P>();
    return_tests<P>();
    timeout_return_tests<P>();
    std::printf("PASS %s: Linux ABI byte/register codec model; no target execution\n", Codec<P>::profile.name);
}

int main()
{
    run_profile<Profile::i386>();
    run_profile<Profile::x86_64>();
    run_profile<Profile::aarch64>();
    run_profile<Profile::arm32>();
    run_profile<Profile::rv64>();
    run_profile<Profile::rv32>();
    run_profile<Profile::loongarch64>();
    run_profile<Profile::mips>();
    run_profile<Profile::mips64>();
    run_profile<Profile::ppc>();
}
