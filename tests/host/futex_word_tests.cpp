// Hosted checks of the futex user-word operations (src/user/futex_word.hpp)
// against a mock resolver. Host threads stand in for user code on other CPUs;
// this is a model of the operations, not guest or hardware execution.
#include "../../src/user/futex_word.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace ok;
using namespace ok::user::native;
using FutexWakeOperation = abi::linux_user::FutexWakeOperation;

namespace
{
std::atomic<usize> checks{};
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                                            \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

constexpr uptr base = 0x40001000;
constexpr uptr unmapped = base + 8;
constexpr uptr read_only = base + 12;

// A page of words; one word is unmapped and one is mapped read-only.
struct Memory
{
    alignas(16) std::array<u32, 64> words{};
    usize resolves{}, writes{};

    Result<u32 *> operator()(uptr address, bool write)
    {
        ++resolves;
        writes += write;
        CHECK(address % sizeof(u32) == 0);
        if (address < base || address - base >= sizeof(words) || address == unmapped ||
            (write && address == read_only))
            return Status::fault();
        return &words[(address - base) / sizeof(u32)];
    }
    // The word as user code of a profile with this byte order stores it.
    void put(uptr address, u32 value, bool big_endian)
    {
        auto *bytes = reinterpret_cast<std::byte *>(&words[(address - base) / sizeof(u32)]);
        for (usize i = 0; i < 4; ++i) bytes[i] = static_cast<std::byte>(value >> (8 * (big_endian ? 3 - i : i)));
    }
    u32 get(uptr address, bool big_endian) const
    {
        const auto *bytes = reinterpret_cast<const std::byte *>(&words[(address - base) / sizeof(u32)]);
        u32 value{};
        for (usize i = 0; i < 4; ++i) value |= static_cast<u32>(bytes[i]) << (8 * (big_endian ? 3 - i : i));
        return value;
    }
};

constexpr u32 encode(u32 operation, u32 comparison, u32 operand, u32 compare_operand, bool shifted = false)
{
    return (shifted ? 0x80000000U : 0U) | ((operation & 7U) << 28) | ((comparison & 15U) << 24) |
        ((operand & 0xfffU) << 12) | (compare_operand & 0xfffU);
}

// Linux futex_atomic_op_inuser arithmetic, written out independently.
u32 expected_update(u32 operation, u32 previous, u32 operand, bool shifted)
{
    const i32 signed_operand = (operand & 0x800U) ? static_cast<i32>(operand | 0xfffff000U) : static_cast<i32>(operand);
    const u32 argument = shifted ? (u32{1} << (static_cast<u32>(signed_operand) & 31U)) : static_cast<u32>(signed_operand);
    switch (operation)
    {
    case 0: return argument;
    case 1: return previous + argument;
    case 2: return previous | argument;
    case 3: return previous & ~argument;
    default: return previous ^ argument;
    }
}
bool expected_compare(u32 comparison, u32 previous, u32 compare_operand)
{
    const i32 left = static_cast<i32>(previous);
    const i32 right = (compare_operand & 0x800U) ? static_cast<i32>(compare_operand | 0xfffff000U)
                                                 : static_cast<i32>(compare_operand);
    switch (comparison)
    {
    case 0: return left == right;
    case 1: return left != right;
    case 2: return left < right;
    case 3: return left <= right;
    case 4: return left > right;
    default: return left >= right;
    }
}

constexpr std::array<u32, 8> previous_values{0, 1, 2, 0x7ffU, 0x7fffffffU, 0x80000000U, 0xfffff800U, 0xffffffffU};
constexpr std::array<u32, 9> operands{0, 1, 31, 32, 33, 0x7ffU, 0x800U, 0xfffU, 0x801U};

template <bool HardwareAtomic> void arithmetic(bool big_endian)
{
    for (u32 operation = 0; operation < 5; ++operation)
        for (const bool shifted : {false, true})
            for (const auto operand : operands)
                for (const auto previous : previous_values)
                {
                    Memory memory;
                    const uptr address = base + 16;
                    memory.put(address, previous, big_endian);
                    memory.put(address + 4, 0x5a5a5a5aU, big_endian);
                    const auto decoded = FutexWakeOperation::decode(encode(operation, 0, operand, 0, shifted));
                    const auto old = futex_word_wake_operation<HardwareAtomic>(memory, address, decoded, big_endian);
                    CHECK(old && old.value() == previous);
                    CHECK(memory.get(address, big_endian) == expected_update(operation, previous, operand, shifted));
                    CHECK(memory.get(address + 4, big_endian) == 0x5a5a5a5aU);
                    CHECK(memory.resolves == 1 && memory.writes == 1);
                }
    // Undefined arithmetic is rejected before the word is resolved or touched.
    for (u32 operation = 5; operation < 8; ++operation)
    {
        Memory memory;
        memory.put(base, 77, big_endian);
        const auto decoded = FutexWakeOperation::decode(encode(operation, 0, 1, 0));
        const auto old = futex_word_wake_operation<HardwareAtomic>(memory, base, decoded, big_endian);
        CHECK(!old && old.status().code() == StatusCode::unsupported);
        CHECK(memory.resolves == 0 && memory.get(base, big_endian) == 77);
    }
}

void comparisons()
{
    // The comparison runs on the old value the atomic update returned.
    for (u32 comparison = 0; comparison < 6; ++comparison)
        for (const auto compare_operand : operands)
            for (const auto previous : previous_values)
            {
                Memory memory;
                memory.put(base, previous, false);
                const auto decoded = FutexWakeOperation::decode(encode(1, comparison, 1, compare_operand));
                const auto old = futex_word_wake_operation(memory, base, decoded, false);
                CHECK(old && old.value() == previous);
                const auto result = decoded.compare(old.value());
                CHECK(result && result.value() == expected_compare(comparison, previous, compare_operand));
            }
    for (u32 comparison = 6; comparison < 16; ++comparison)
    {
        Memory memory;
        const auto decoded = FutexWakeOperation::decode(encode(0, comparison, 3, 0));
        const auto old = futex_word_wake_operation(memory, base, decoded, false);
        // Linux performs the update before it rejects the comparison.
        CHECK(old && memory.get(base, false) == 3);
        CHECK(!decoded.compare(old.value()) && decoded.compare(old.value()).status().code() == StatusCode::unsupported);
    }
    // Twelve-bit operands are signed; a shift uses only the low five bits.
    CHECK(FutexWakeOperation::decode(encode(0, 0, 0xfffU, 0)).argument() == 0xffffffffU);
    CHECK(FutexWakeOperation::decode(encode(0, 0, 0x800U, 0)).argument() == 0xfffff800U);
    CHECK(FutexWakeOperation::decode(encode(0, 0, 0x7ffU, 0)).argument() == 0x7ffU);
    CHECK(FutexWakeOperation::decode(encode(0, 0, 33, 0, true)).argument() == 2);
    CHECK(FutexWakeOperation::decode(encode(0, 0, 0xfffU, 0, true)).argument() == 0x80000000U);
}

template <bool HardwareAtomic> void access(bool big_endian)
{
    const auto set = FutexWakeOperation::decode(encode(0, 0, 9, 0));
    // Misaligned words fail before resolution, for every operation.
    for (uptr offset = 1; offset < 4; ++offset)
    {
        Memory memory;
        const uptr address = base + offset;
        const auto load = futex_word_load<HardwareAtomic>(memory, address, big_endian);
        const auto store = futex_word_store<HardwareAtomic>(memory, address, 0, big_endian);
        const auto exchange = futex_word_compare_exchange<HardwareAtomic>(memory, address, 0, 1, big_endian);
        const auto update = futex_word_wake_operation<HardwareAtomic>(memory, address, set, big_endian);
        CHECK(!load && load.status().code() == StatusCode::invalid_argument);
        CHECK(store.code() == StatusCode::invalid_argument);
        CHECK(!exchange && exchange.status().code() == StatusCode::invalid_argument);
        CHECK(!update && update.status().code() == StatusCode::invalid_argument);
        CHECK(memory.resolves == 0);
    }
    // Unmapped words fault; a read-only word loads but faults every write.
    {
        Memory memory;
        CHECK(futex_word_load<HardwareAtomic>(memory, unmapped, big_endian).status().code() == StatusCode::fault);
        CHECK(futex_word_store<HardwareAtomic>(memory, unmapped, 0, big_endian).code() == StatusCode::fault);
        CHECK(futex_word_compare_exchange<HardwareAtomic>(memory, unmapped, 0, 1, big_endian).status().code() ==
              StatusCode::fault);
        CHECK(futex_word_wake_operation<HardwareAtomic>(memory, unmapped, set, big_endian).status().code() ==
              StatusCode::fault);
        CHECK(futex_word_load<HardwareAtomic>(memory, base + 4096, big_endian).status().code() == StatusCode::fault);
        memory.put(read_only, 0x01020304U, big_endian);
        const auto load = futex_word_load<HardwareAtomic>(memory, read_only, big_endian);
        CHECK(load && load.value() == 0x01020304U);
        CHECK(futex_word_store<HardwareAtomic>(memory, read_only, 0, big_endian).code() == StatusCode::fault);
        CHECK(futex_word_compare_exchange<HardwareAtomic>(memory, read_only, 0x01020304U, 1, big_endian)
                  .status().code() == StatusCode::fault);
        CHECK(futex_word_wake_operation<HardwareAtomic>(memory, read_only, set, big_endian).status().code() ==
              StatusCode::fault);
        CHECK(memory.get(read_only, big_endian) == 0x01020304U);
        // Loads resolve for reading; every update resolves for writing (COW).
        CHECK(memory.resolves == 9 && memory.writes == 6);
    }
    // Compare-exchange stores only on a match and returns the value found.
    {
        Memory memory;
        memory.put(base, 0x80000123U, big_endian);
        const auto miss = futex_word_compare_exchange<HardwareAtomic>(memory, base, 0x123U, 0x40000000U, big_endian);
        CHECK(miss && miss.value() == 0x80000123U && memory.get(base, big_endian) == 0x80000123U);
        const auto hit = futex_word_compare_exchange<HardwareAtomic>(memory, base, 0x80000123U, 0xc0000000U, big_endian);
        CHECK(hit && hit.value() == 0x80000123U && memory.get(base, big_endian) == 0xc0000000U);
        CHECK(futex_word_store<HardwareAtomic>(memory, base, 0x00a1b2c3U, big_endian));
        CHECK(memory.get(base, big_endian) == 0x00a1b2c3U);
        const auto load = futex_word_load<HardwareAtomic>(memory, base, big_endian);
        CHECK(load && load.value() == 0x00a1b2c3U);
        CHECK(memory.writes == 3 && memory.resolves == 4);
    }
}

// Kernel-side updates race "user" atomics on other host threads; no update of
// either side may be lost or torn.
void concurrency(bool big_endian)
{
    constexpr usize rounds = 200000;
    Memory memory;
    memory.put(base, 0, big_endian);
    std::atomic<bool> go{false};
    auto *word = &memory.words[0];
    const auto add = FutexWakeOperation::decode(encode(1, 0, 1, 0));
    std::vector<std::thread> threads;
    for (usize kernel = 0; kernel < 2; ++kernel)
        threads.emplace_back([&] {
            while (!go.load()) {}
            for (usize i = 0; i < rounds; ++i)
            {
                // A stateless resolver: Memory counts resolves unsynchronized.
                auto resolver = [word](uptr, bool) -> Result<u32 *> { return word; };
                CHECK(futex_word_wake_operation(resolver, base, add, big_endian));
            }
        });
    for (usize user = 0; user < 2; ++user)
        threads.emplace_back([&] {
            while (!go.load()) {}
            for (usize i = 0; i < rounds; ++i)
            {
                if (futex_word_native_order(big_endian))
                {
                    __atomic_fetch_add(word, 1U, __ATOMIC_SEQ_CST);
                    continue;
                }
                u32 raw = __atomic_load_n(word, __ATOMIC_RELAXED);
                while (!__atomic_compare_exchange_n(word, &raw, futex_word_order(futex_word_order(raw, true) + 1, true),
                                                    true, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {}
            }
        });
    go.store(true);
    for (auto &thread : threads) thread.join();
    CHECK(memory.get(base, big_endian) == 4 * rounds);

    // OR/ANDN/XOR of disjoint bits: each side owns its bits, so every bit
    // ends exactly where its owner left it.
    memory.put(base, 0, big_endian);
    threads.clear();
    go.store(false);
    for (u32 bit = 0; bit < 4; ++bit)
        threads.emplace_back([&, bit] {
            while (!go.load()) {}
            auto resolver = [word](uptr, bool) -> Result<u32 *> { return word; };
            const auto set = FutexWakeOperation::decode(encode(2, 0, bit, 0, true));
            const auto clear = FutexWakeOperation::decode(encode(3, 0, bit, 0, true));
            const auto flip = FutexWakeOperation::decode(encode(4, 0, bit + 4, 0, true));
            for (usize i = 0; i < rounds / 4; ++i)
            {
                auto old = futex_word_wake_operation(resolver, base, set, big_endian);
                CHECK(old && !(old.value() & (1U << bit)));
                old = futex_word_wake_operation(resolver, base, clear, big_endian);
                CHECK(old && (old.value() & (1U << bit)));
                CHECK(futex_word_wake_operation(resolver, base, flip, big_endian));
            }
        });
    for (u32 bit = 8; bit < 12; ++bit)
        threads.emplace_back([&, bit] {
            while (!go.load()) {}
            const u32 mask = futex_word_order(1U << bit, big_endian);
            for (usize i = 0; i < rounds / 4 + 1; ++i) __atomic_fetch_xor(word, mask, __ATOMIC_SEQ_CST);
        });
    go.store(true);
    for (auto &thread : threads) thread.join();
    // rounds / 4 flips of bits 4..7 (even: clear); rounds / 4 + 1 of 8..11 (odd: set).
    static_assert((rounds / 4) % 2 == 0);
    CHECK(memory.get(base, big_endian) == 0x00000f00U);
}
} // namespace

int main()
{
    for (const bool big_endian : {false, true})
    {
        arithmetic<true>(big_endian);
        arithmetic<false>(big_endian);
        access<true>(big_endian);
        access<false>(big_endian);
        concurrency(big_endian);
    }
    comparisons();
    std::printf("PASS futex-word checks=%zu scope=hosted-model guest=0 smp=0\n", checks.load());
    return 0;
}
