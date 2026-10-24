#include "ok/abi/linux_native.hpp"
#include <cstdio>
#include <cstdlib>
using namespace ok;
using namespace ok::abi::linux_user;
using Frame = arch::execution::UserFrame;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #condition);                                               \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

bool same(const Frame &left, const Frame &right)
{
    return left.registers == right.registers && left.pc == right.pc && left.sp == right.sp &&
           left.flags == right.flags && left.fs_base == right.fs_base && left.gs_base == right.gs_base &&
           left.fp_state == right.fp_state;
}
void condition(Frame &frame, u32 value)
{
    // PowerPC execution.S uses absolute offset 440 = fp_state + 280, big endian.
    frame.fp_state[280] = static_cast<u8>(value >> 24);
    frame.fp_state[281] = static_cast<u8>(value >> 16);
    frame.fp_state[282] = static_cast<u8>(value >> 8);
    frame.fp_state[283] = static_cast<u8>(value);
}
Frame seed()
{
    Frame frame{};
    for (usize i = 0; i < frame.registers.size(); ++i)
        frame.registers[i] = static_cast<uptr>(0xdeadbeef00000100ULL + i);
    frame.pc = 0x40010000;
    frame.sp = 0x70012340;
    frame.flags = 0xabcdefff;
    frame.fs_base = 0x40023000;
    frame.gs_base = 0x40034000;
    for (usize i = 0; i < frame.fp_state.size(); ++i)
        frame.fp_state[i] = static_cast<u8>(i ^ 0x5a);
    condition(frame, 0x8b35a79f);
    return frame;
}

template <Profile Selected> void profile(usize number_slot, std::array<usize, 6> slots, usize return_slot)
{
    using Bridge = NativeBridge<Selected>;
    using C = Codec<Selected>;
    auto frame = seed();
    if constexpr (C::profile.word_bytes == 8)
        for (usize i = 0; i < 32; ++i)
            frame.registers[i] = 0x100 + i;
    frame.registers[number_slot] = C::profile.word_bytes == 4 ? 0xbeef000000001234ULL : 0x1234;
    const auto original = frame;
    constexpr std::array<u8, 24> stack{0, 0, 0, 0, 0,    0,    0,    0,    0,    0,    0,    0,
                                       0, 0, 0, 0, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67};
    for (usize count = 0; count <= 6; ++count)
    {
        auto call = Bridge::decode_call(frame, count, stack);
        CHECK(call && call.value().number == 0x1234 && call.value().argument_count == count);
        for (usize i = 0; i < count; ++i)
        {
            const u64 expected =
                Selected == Profile::mips && i >= 4 ? (i == 4 ? 0x89abcdefULL : 0x01234567ULL) : 0x100 + slots[i];
            CHECK(call.value().arguments[i] == expected);
        }
        CHECK(same(frame, original));
    }
    CHECK(!Bridge::decode_call(frame, 7, stack));
    if constexpr (Selected == Profile::mips)
    {
        CHECK(!Bridge::decode_call(frame, 5, std::span(stack).first(19)));
        CHECK(!Bridge::decode_call(frame, 6, std::span(stack).first(23)));
    }
    const auto snapshot = Bridge::snapshot(frame);
    if constexpr (Selected == Profile::i386)
    {
        CHECK(snapshot.integer[4] == frame.sp && snapshot.integer[5] == 0x102);
        CHECK(snapshot.integer[8] == 0 && snapshot.integer[12] == 0);
    }
    else if constexpr (Selected == Profile::x86_64)
    {
        CHECK(snapshot.integer[7] == frame.sp && snapshot.integer[15] == 0x100);
        CHECK(snapshot.integer[16] == 0);
    }
    else if constexpr (Selected == Profile::aarch64)
        CHECK(snapshot.integer[30] == 0x11e && snapshot.integer[31] == 0);
    else if constexpr (Selected == Profile::arm32)
        CHECK(snapshot.integer[13] == frame.sp && snapshot.integer[15] == frame.pc && snapshot.integer[16] == 0);
    else if constexpr (Selected == Profile::ppc)
    {
        CHECK(snapshot.condition == 0x8b35a79f && snapshot.condition != frame.flags);
        CHECK(snapshot.integer[1] == frame.sp && snapshot.integer[2] == frame.fs_base &&
              snapshot.integer[13] == frame.gs_base);
    }
    else
    {
        CHECK(snapshot.integer[0] == 0);
        if constexpr (Selected == Profile::rv32 || Selected == Profile::rv64)
            CHECK(snapshot.integer[2] == frame.sp);
        else if constexpr (Selected == Profile::loongarch64)
            CHECK(snapshot.integer[3] == frame.sp);
        else
            CHECK(snapshot.integer[29] == frame.sp && snapshot.integer[26] == 0 && snapshot.integer[27] == 0);
    }
    auto expected = original;
    CHECK(Bridge::encode_return(frame, Return{0, 5}));
    expected.registers[return_slot] = C::profile.error_convention == ErrorConvention::negative ? C::word_mask - 4 : 5;
    if constexpr (Selected == Profile::mips || Selected == Profile::mips64)
        expected.registers[7] = 1;
    if constexpr (Selected == Profile::ppc)
        condition(expected, 0x9b35a79f);
    CHECK(same(frame, expected));
    auto result = Bridge::decode_return(frame);
    CHECK(result && result.value().error == 5 && result.value().value == 0);
    CHECK(Bridge::encode_return(frame, Return{0x5678, 0}));
    expected.registers[return_slot] = 0x5678;
    if constexpr (Selected == Profile::mips || Selected == Profile::mips64)
        expected.registers[7] = 0;
    if constexpr (Selected == Profile::ppc)
        condition(expected, 0x8b35a79f);
    CHECK(same(frame, expected));
    result = Bridge::decode_return(frame);
    CHECK(result && result.value().value == 0x5678 && !result.value().error);
    CHECK(!Bridge::encode_return(frame, Return{1, 5}) && same(frame, expected));
    CHECK(!Bridge::encode_return(frame, Return{0, 4096}) && same(frame, expected));
    // Literal timeout goldens also preserve every non-result saved-frame field.
    constexpr u32 timeout = Selected == Profile::mips || Selected == Profile::mips64 ? 145 : 110;
    CHECK(C::errno_number(Error::e_timedout) == timeout);
    auto timeout_frame = original;
    auto timeout_golden = original;
    if constexpr (Selected == Profile::mips || Selected == Profile::mips64)
    {
        timeout_golden.registers[2] = 145;
        timeout_golden.registers[7] = 1;
    }
    else if constexpr (Selected == Profile::ppc)
    {
        timeout_golden.registers[3] = 110;
        condition(timeout_golden, 0x9b35a79f);
    }
    else
        timeout_golden.registers[return_slot] = C::profile.word_bytes == 4
            ? 0xffffff92ULL : 0xffffffffffffff92ULL;
    CHECK(Bridge::encode_return(timeout_frame, Return{0, *C::errno_number(Error::e_timedout)}));
    CHECK(same(timeout_frame, timeout_golden));
    const auto timeout_result = Bridge::decode_return(timeout_frame);
    CHECK(timeout_result && timeout_result.value().error == timeout && timeout_result.value().value == 0);
    if constexpr (C::profile.word_bytes == 4)
        CHECK(!Bridge::encode_return(frame, Return{0x100000000ULL, 0}) && same(frame, expected));
    else
    {
        frame = original;
        frame.registers[slots[0]] = 0x0123456789abcdefULL;
        auto call = Bridge::decode_call(frame, 1);
        CHECK(call && call.value().arguments[0] == 0x0123456789abcdefULL);
        frame.registers[number_slot] = 0x100001234ULL;
        CHECK(Bridge::decode_call(frame, 0).status().code() == StatusCode::overflow);
    }
    frame = original;
    auto tls_expected = original;
    const auto wrong_route = Bridge::thread_pointer_route == ThreadPointerRoute::restored_register
                                 ? ThreadPointerRoute::mips_rdhwr_emulation
                                 : ThreadPointerRoute::restored_register;
    CHECK(Bridge::set_thread_pointer(frame, {0x40012340}, wrong_route).code() == StatusCode::unsupported);
    CHECK(same(frame, original));
    if constexpr (Selected == Profile::mips || Selected == Profile::mips64)
        CHECK(!Bridge::set_thread_pointer(frame, {0x40012340})); // No implicit hardware UserLocal claim.
    CHECK(Bridge::set_thread_pointer(frame, {0x40012340}, Bridge::thread_pointer_route));
    if constexpr (Selected == Profile::i386)
    {
        tls_expected.gs_base = 0x40012340;
        tls_expected.registers[8] = 0x3b;
    }
    else if constexpr (Selected == Profile::arm32)
        tls_expected.gs_base = 0x40012340;
    else if constexpr (Selected == Profile::rv32 || Selected == Profile::rv64)
        tls_expected.registers[4] = 0x40012340;
    else if constexpr (Selected == Profile::loongarch64 || Selected == Profile::ppc)
    {
        tls_expected.registers[2] = 0x40012340;
        tls_expected.fs_base = 0x40012340;
    }
    else
        tls_expected.fs_base = 0x40012340;
    CHECK(same(frame, tls_expected));
    if constexpr (C::profile.word_bytes == 4)
    {
        CHECK(!Bridge::set_thread_pointer(frame, {0x100000000ULL}, Bridge::thread_pointer_route));
        CHECK(same(frame, tls_expected));
    }
    std::printf("PASS %s native frame bridge model: registers, widths, returns, flags and TLS route\n",
                C::profile.name);
}
void stack_copy_boundary()
{
    using Bridge = NativeBridge<Profile::mips>;
    auto frame = seed();
    frame.registers[2] = 4288;
    const auto original = frame;
    unsigned copies = 0;
    usize expected_size = 8;
    auto copy = [&](uptr source, std::span<u8> destination) {
        ++copies;
        CHECK(source == (frame.sp & 0xffffffffULL) + 16 && destination.size() == expected_size);
        constexpr u8 data[]{0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67};
        for (usize i = 0; i < destination.size(); ++i)
            destination[i] = data[i];
        return Status::success();
    };
    auto six = Bridge::decode_call_copying_stack(frame, 6, copy);
    CHECK(six && copies == 1 && six.value().arguments[4] == 0x89abcdef && six.value().arguments[5] == 0x01234567);
    CHECK(Codec<Profile::mips>::decode_argument64(six.value(), 4).value() == 0x89abcdef01234567ULL);
    CHECK(!Codec<Profile::mips>::decode_argument64(six.value(), 3));
    expected_size = 4;
    auto five = Bridge::decode_call_copying_stack(frame, 5, copy);
    CHECK(five && copies == 2 && five.value().arguments[4] == 0x89abcdef);
    CHECK(Bridge::decode_call_copying_stack(frame, 4, copy) && copies == 2);
    CHECK(!Bridge::decode_call_copying_stack(frame, 7, copy) && copies == 2);
    auto fault = [](uptr, std::span<u8> out) {
        out[0] = 0xff;
        return Status::fault("copy failed");
    };
    CHECK(Bridge::decode_call_copying_stack(frame, 6, fault).status().code() == StatusCode::fault);
    CHECK(same(frame, original));
    frame.sp = 0xffffffe8;
    expected_size = 8;
    CHECK(Bridge::decode_call_copying_stack(frame, 6, copy) && copies == 3);
    frame.sp = 0xffffffe9;
    CHECK(!Bridge::decode_call_copying_stack(frame, 6, copy) && copies == 3);
    frame.sp = 0xfffffff0;
    CHECK(!Bridge::decode_call_copying_stack(frame, 5, copy) && copies == 3);
    CHECK(Bridge::decode_call_copying_stack(frame, 4, copy) && copies == 3);
    Frame other{};
    other.registers[14] = 1;
    auto no_copy = [&](uptr, std::span<u8>) {
        ++copies;
        return Status::fault();
    };
    CHECK(NativeBridge<Profile::x86_64>::decode_call_copying_stack(other, 6, no_copy) && copies == 3);
    std::puts("PASS explicit O32 copy boundary: exact slots, overflow, failure, no unnecessary user reads");
}
int main()
{
    profile<Profile::i386>(7, {4, 6, 5, 1, 0, 2}, 7);
    profile<Profile::x86_64>(14, {9, 10, 11, 5, 7, 6}, 14);
    profile<Profile::aarch64>(8, {0, 1, 2, 3, 4, 5}, 0);
    profile<Profile::arm32>(7, {0, 1, 2, 3, 4, 5}, 0);
    profile<Profile::rv64>(17, {10, 11, 12, 13, 14, 15}, 10);
    profile<Profile::rv32>(17, {10, 11, 12, 13, 14, 15}, 10);
    profile<Profile::loongarch64>(11, {4, 5, 6, 7, 8, 9}, 4);
    profile<Profile::mips>(2, {4, 5, 6, 7, 8, 9}, 2);
    profile<Profile::mips64>(2, {4, 5, 6, 7, 8, 9}, 2);
    profile<Profile::ppc>(0, {3, 4, 5, 6, 7, 8}, 3);
    stack_copy_boundary();
}
