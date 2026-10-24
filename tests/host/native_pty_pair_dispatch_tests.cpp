#include "ok/user/native_pty_pair_dispatch.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace ok;
namespace pty = ok::user::native_pty;
namespace
{
usize checks{}, cases{};
#define CHECK(condition) do { ++checks; if (!(condition)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #condition); std::abort(); } } while (false)

struct State
{
    std::array<u8, 10> output{};
    std::array<i32, 2> descriptors{3, 4};
    std::vector<std::string> events{};
    bool writable{true}, copy_failure{}, cancel_failure{}, quarantined{};
    usize prefix{}, fail_validation{}, validations{}, visible{}, reserved{}, providers{}, cancels{}, reports{};
    u32 flags{};
    Status prepare_failure{}, reported{};
    State() { output.fill(0xa5); }
};

struct Prepared
{
    State *state{};
    explicit Prepared(State &owner) : state{&owner} { owner.reserved = 2; owner.providers = 1; }
    Prepared(const Prepared &) = delete;
    Prepared &operator=(const Prepared &) = delete;
    Prepared(Prepared &&other) noexcept : state{std::exchange(other.state, nullptr)} {}
    ~Prepared() { if (state) CHECK(cancel()); }
    std::array<i32, 2> descriptors() const { return state->descriptors; }
    Status validate() const
    {
        state->events.emplace_back("validate");
        ++state->validations;
        CHECK(state->visible == 0 && state->reserved == 2 && state->providers == 1);
        return state->validations == state->fail_validation ? Status::busy("changed authority") : Status{};
    }
    Status cancel()
    {
        if (!state) return {};
        auto &owner = *state;
        owner.events.emplace_back("cancel"); ++owner.cancels;
        state = nullptr;
        if (owner.cancel_failure)
        {
            owner.quarantined = true;
            return Status::io_error("retained failed holder");
        }
        owner.reserved = 0; owner.providers = 0;
        return {};
    }
    void publish() noexcept
    {
        CHECK(state && !state->visible && state->reserved == 2 && state->validations == 2);
        CHECK(!state->copy_failure && !state->fail_validation);
        state->events.emplace_back("publish");
        state->visible = 2; state->reserved = 0; state = nullptr;
    }
};

template <typename Word, bool BigEndian>
Result<usize> invoke(State &state, const std::array<Word, 6> &arguments)
{
    return pty::create<BigEndian>(arguments,
        [&](Word address, usize size) {
            state.events.emplace_back("writable");
            CHECK(address == arguments[0] && size == 8 && !state.visible && !state.reserved);
            return state.writable;
        },
        [&](u32 flags) -> Result<Prepared> {
            state.events.emplace_back("prepare"); state.flags = flags;
            if (!state.prepare_failure) return state.prepare_failure;
            return Prepared{state};
        },
        [&](Word address, std::span<const u8> bytes) -> Status {
            state.events.emplace_back("copy");
            CHECK(address == arguments[0] && bytes.size() == 8 && !state.visible && state.reserved == 2);
            const auto copied = state.copy_failure ? state.prefix : bytes.size();
            std::copy_n(bytes.begin(), copied, state.output.begin() + 1);
            return state.copy_failure ? Status::fault("injected checked-copy failure") : Status{};
        },
        [](Prepared &operation) noexcept { operation.publish(); },
        [&](Status failure) { ++state.reports; state.reported = failure; });
}

template <typename Word, bool BigEndian>
std::array<Word, 6> arguments(Word flags = 0)
{ return {Word{128}, flags, 0, 0, 0, 0}; }

template <bool BigEndian>
std::array<u8, 8> expected(std::array<i32, 2> descriptors)
{
    std::array<u8, 8> bytes{};
    // Oracle uses division in wire order, independently of production shifts.
    for (usize side = 0; side < 2; ++side)
    {
        auto value = static_cast<u32>(descriptors[side]);
        for (usize index = 0; index < 4; ++index)
        {
            bytes[side * 4 + (BigEndian ? 3 - index : index)] = static_cast<u8>(value % 256);
            value /= 256;
        }
    }
    return bytes;
}

template <typename Word, bool BigEndian>
void profile(const char *name)
{
    ++cases;
    auto args = arguments<Word, BigEndian>();
    for (const Word flags : {Word{0}, Word{pty::nonblock}, Word{pty::cloexec}, Word{pty::nonblock | pty::cloexec}})
    {
        State state;
        args[1] = flags;
        const auto result = invoke<Word, BigEndian>(state, args);
        CHECK(result && result.value() == 0 && state.visible == 2 && !state.reserved && state.providers == 1);
        CHECK(state.flags == flags && state.cancels == 0 && state.reports == 0);
        CHECK(state.events == (std::vector<std::string>{"writable", "prepare", "validate", "copy", "validate", "publish"}));
        const auto oracle = expected<BigEndian>(state.descriptors);
        CHECK(std::equal(oracle.begin(), oracle.end(), state.output.begin() + 1));
        CHECK(state.output.front() == 0xa5 && state.output.back() == 0xa5);
    }
    args[1] = 0;
    // Every unknown bit, including LP64 high bits, is refused before output/preparation.
    for (usize bit = 0; bit < sizeof(Word) * 8; ++bit)
    {
        const Word flag = Word{1} << bit;
        if (flag & (pty::nonblock | pty::cloexec)) continue;
        State state; args[1] = flag;
        const auto result = invoke<Word, BigEndian>(state, args);
        CHECK(!result && result.status().code() == StatusCode::invalid_argument && state.events.empty());
    }
    args[1] = 0;
    for (usize slot = 2; slot < 6; ++slot)
    {
        State state; args[slot] = std::numeric_limits<Word>::max();
        const auto result = invoke<Word, BigEndian>(state, args);
        CHECK(!result && result.status().code() == StatusCode::invalid_argument && state.events.empty());
        args[slot] = 0;
    }
    for (usize offset = 0; offset < 7; ++offset)
    {
        State state; args[0] = std::numeric_limits<Word>::max() - static_cast<Word>(offset);
        const auto result = invoke<Word, BigEndian>(state, args);
        CHECK(!result && result.status().code() == StatusCode::fault && state.events.empty());
    }
    {
        State state; args[0] = std::numeric_limits<Word>::max() - 7;
        CHECK((invoke<Word, BigEndian>(state, args)));
        CHECK(state.visible == 2);
    }
    {
        State state; args[0] = 0;
        const auto result = invoke<Word, BigEndian>(state, args);
        CHECK(!result && result.status().code() == StatusCode::fault && state.events.empty());
    }
    args[0] = 128;
    {
        State state; state.writable = false;
        const auto result = invoke<Word, BigEndian>(state, args);
        CHECK(!result && result.status().code() == StatusCode::fault);
        CHECK(state.events == std::vector<std::string>{"writable"} && !state.reserved && !state.providers);
    }
    for (auto failure : {Status::process_file_limit(), Status::system_file_limit(), Status::no_space(), Status::overflow()})
    {
        State state; state.prepare_failure = failure;
        const auto result = invoke<Word, BigEndian>(state, args);
        CHECK(!result && result.status().code() == failure.code());
        CHECK(state.events == (std::vector<std::string>{"writable", "prepare"}));
        CHECK(!state.reserved && !state.visible && !state.providers && !state.cancels);
    }
    for (usize validation : {usize{1}, usize{2}})
    {
        State state; state.fail_validation = validation;
        const auto result = invoke<Word, BigEndian>(state, args);
        CHECK(!result && result.status().code() == StatusCode::busy);
        CHECK(state.validations == validation && state.cancels == 1 && !state.reserved && !state.providers && !state.visible);
        CHECK(state.output.front() == 0xa5 && state.output.back() == 0xa5);
        if (validation == 1) CHECK(std::all_of(state.output.begin(), state.output.end(), [](u8 b) { return b == 0xa5; }));
    }
    for (const auto bad : {std::array<i32, 2>{-1, 4}, std::array<i32, 2>{3, -1}, std::array<i32, 2>{3, 3}})
    {
        State state; state.descriptors = bad;
        const auto result = invoke<Word, BigEndian>(state, args);
        CHECK(!result && result.status().code() == StatusCode::invalid_argument);
        CHECK(state.cancels == 1 && !state.reserved && !state.visible && !state.providers);
        CHECK(std::all_of(state.output.begin(), state.output.end(), [](u8 b) { return b == 0xa5; }));
    }
    for (usize prefix = 0; prefix <= 8; ++prefix)
    {
        State state; state.copy_failure = true; state.prefix = prefix;
        const auto result = invoke<Word, BigEndian>(state, args);
        CHECK(!result && result.status().code() == StatusCode::fault);
        CHECK(state.validations == 1 && state.cancels == 1 && !state.reserved && !state.visible && !state.providers);
        const auto oracle = expected<BigEndian>(state.descriptors);
        for (usize index = 0; index < 8; ++index) CHECK(state.output[index + 1] == (index < prefix ? oracle[index] : 0xa5));
        CHECK(state.output.front() == 0xa5 && state.output.back() == 0xa5);
    }
    for (usize mode = 0; mode < 3; ++mode)
    {
        State state; state.cancel_failure = true;
        if (mode < 2) state.fail_validation = mode + 1;
        else state.copy_failure = true;
        const auto result = invoke<Word, BigEndian>(state, args);
        CHECK(!result && result.status().code() == StatusCode::io_error);
        CHECK(state.cancels == 1 && state.reports == 1 && state.reported.code() == StatusCode::io_error);
        CHECK(state.quarantined && state.reserved == 2 && state.providers == 1 && !state.visible);
    }
    u32 random = 0x514f32;
    for (usize index = 0; index < 1000; ++index)
    {
        random = random * 1664525 + 1013904223;
        State state;
        state.descriptors = {static_cast<i32>(random & 0x7fffffffU), std::numeric_limits<i32>::max()};
        if (state.descriptors[0] == state.descriptors[1]) --state.descriptors[0];
        CHECK((invoke<Word, BigEndian>(state, args)));
        const auto oracle = expected<BigEndian>(state.descriptors);
        CHECK(std::equal(oracle.begin(), oracle.end(), state.output.begin() + 1));
    }
    std::printf("PAIR_WIRE_PROFILE %s PASS\n", name);
}
}

int main()
{
    profile<u32, false>("i386"); profile<u64, false>("x86_64");
    profile<u64, false>("aarch64"); profile<u32, false>("arm32");
    profile<u64, false>("rv64"); profile<u32, false>("rv32");
    profile<u64, false>("loongarch64"); profile<u32, true>("mips");
    profile<u64, true>("mips64"); profile<u32, true>("ppc");
    // Choose a fundamental unsigned word distinct from both fixed-width aliases.
    using Alias = std::conditional_t<!std::same_as<unsigned int, u32> &&
        !std::same_as<unsigned int, u64>, unsigned int,
        std::conditional_t<!std::same_as<unsigned long, u32> &&
            !std::same_as<unsigned long, u64>, unsigned long, unsigned long long>>;
    static_assert(!std::same_as<Alias, u32> && !std::same_as<Alias, u64> &&
        (sizeof(Alias) == sizeof(u32) || sizeof(Alias) == sizeof(u64)),
        "boundary regression requires a distinct supported unsigned word");
    profile<Alias, false>("host-distinct-unsigned-le");
    profile<Alias, true>("host-distinct-unsigned-be");
    std::printf("NATIVE_PTY_PAIR_BOUNDARY_PASS profiles=%zu checks=%zu hosted_template_only=1\n", cases, checks);
}
