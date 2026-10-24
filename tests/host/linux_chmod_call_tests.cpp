#include "ok/abi/linux_chmod_call.hpp"

#include <array>
#include <cstdio>

using namespace ok;
using namespace ok::abi::linux_user;
namespace
{
usize checks{};
#define CHECK(expression) do { ++checks; if (!(expression)) { std::fprintf(stderr, "chmod call check failed at %d\n", __LINE__); return false; } } while (false)

template <Profile P> bool profile()
{
    using Adapter = ChmodCallAdapter<P>;
    using Result = fs::ModeChangeResult;
    usize reads = 0, changes = 0;
    u64 address{};
    ChmodRequest seen{};
    std::string_view text = "target";
    Status read_status;
    auto result = Result::applied();
    auto read = [&](u64 pointer, FixedString<256> &path) {
        ++reads; address = pointer;
        return read_status ? path.assign(text) : read_status;
    };
    auto change = [&](ChmodRequest request, std::string_view name) {
        ++changes; seen = request;
        if (!request.direct_fd && name != text)
            return Result::failure(Status::io_error("path callback mismatch"));
        return result;
    };
    auto run = [&](ChmodCall call, std::span<const u64> words) {
        reads = changes = 0;
        return Adapter::execute(call, words, read, change);
    };
    std::array<u64, 4> words{0x12345678ffffff9cULL, 0x123400005678ULL, 0xabcdef00ffffffffULL, 0};
    CHECK(run(ChmodCall::fchmodat2, words) == 0);
    CHECK(reads == 1 && changes == 1 && address == (words[1] & Codec<P>::word_mask));
    CHECK(seen.fd == -100 && seen.permissions == 07777 && !seen.direct_fd);
    words[3] = 0xface000000001100ULL;
    CHECK(run(ChmodCall::fchmodat2, words) == 0 && seen.flags == 0x1100);
    words[3] = 0x800;
    words[1] = 0;
    CHECK(run(ChmodCall::fchmodat2, words) == -22 && reads == 0 && changes == 0);
    words[3] = 0x1000;
    CHECK(run(ChmodCall::fchmodat2, words) == -14 && reads == 0 && changes == 0);
    words[1] = 0x8000;
    text = "";
    CHECK(run(ChmodCall::fchmodat2, words) == 0 && reads == 1 && changes == 1);
    words[3] = 0;
    CHECK(run(ChmodCall::fchmodat2, words) == -2 && reads == 1 && changes == 0);
    text = "target";
    read_status = Status::name_too_long("bounded path");
    CHECK(run(ChmodCall::fchmodat2, words) == -36 && reads == 1 && changes == 0);
    read_status = Status::success();
    // Old fchmodat captures exactly three words and never consumes fourth flags.
    words[3] = ~u64{};
    CHECK(run(ChmodCall::fchmodat, {words.data(), 3}) == 0 && seen.flags == 0);
    CHECK(run(ChmodCall::fchmodat, words) == -14 && reads == 0 && changes == 0);
    CHECK(run(static_cast<ChmodCall>(255), {}) == -38 && reads == 0 && changes == 0);
    std::array<u64, 2> descriptor{0x12340000ffffffffULL, 040000u | 0750u};
    CHECK(run(ChmodCall::fchmod, descriptor) == 0 && reads == 0 && changes == 1);
    CHECK(seen.fd == -1 && seen.direct_fd && seen.permissions == 0750);
    result = Result::read_only();
    CHECK(run(ChmodCall::fchmod, descriptor) == -30 && reads == 0 && changes == 1);
    result = Result::unavailable();
    CHECK(run(ChmodCall::fchmod, descriptor) == -95 && reads == 0 && changes == 1);
    result = Result::failure(Status::permission("owner check"));
    CHECK(run(ChmodCall::fchmod, descriptor) == -1);
    result = Result::failure(Status::denied("component search"));
    CHECK(run(ChmodCall::fchmodat, {words.data(), 3}) == -13);
    result = Result::failure(Status::bad_descriptor());
    CHECK(run(ChmodCall::fchmod, descriptor) == -9);
    result = Result::applied();
    std::array<u64, 2> path{0x8000, 07777};
    constexpr bool legacy = P == Profile::i386 || P == Profile::x86_64 || P == Profile::arm32 ||
                            P == Profile::mips || P == Profile::mips64 || P == Profile::ppc;
    CHECK(run(ChmodCall::chmod, path) == (legacy ? 0 : -38));
    CHECK(reads == (legacy ? 1u : 0u) && changes == (legacy ? 1u : 0u));
    for (usize count = 0; count < 4; ++count)
        CHECK(run(ChmodCall::fchmodat2, {words.data(), count}) == -14 && reads == 0 && changes == 0);
    return true;
}
}

int main()
{
    if (!(profile<Profile::i386>() && profile<Profile::x86_64>() && profile<Profile::aarch64>() &&
          profile<Profile::arm32>() && profile<Profile::rv64>() && profile<Profile::rv32>() &&
          profile<Profile::loongarch64>() && profile<Profile::mips>() && profile<Profile::mips64>() && profile<Profile::ppc>()))
        return 1;
    std::printf("PASS CHMOD_CALL_MODEL checks=%zu profiles=10 user-execution=0 guest=0\n", checks);
}
