#include "ok/user/native_metadata.hpp"
#include "ok/user/native_metadata_profile.hpp"
#include <cstdio>

using namespace ok;
using namespace ok::user::native_metadata;
namespace
{
usize checks{};
#define CHECK(expression) do { ++checks; if (!(expression)) { std::fprintf(stderr, "native metadata check failed at %d\n", __LINE__); return false; } } while (false)

template <typename Word> bool words()
{
    FixedString<256> copied;
    std::array<char, 257> memory{};
    memory.fill('x');
    usize copied_bytes{};
    const auto base = Word{0x1000};
    auto copy = [&](Word pointer, char &value) {
        ++copied_bytes;
        if (pointer < base || pointer - base >= memory.size()) return false;
        value = memory[static_cast<usize>(pointer - base)];
        return true;
    };
    memory[255] = 0;
    CHECK(copy_path(base, copied, copy).ok() && copied.size() == 255 && copied_bytes == 256);
    memory[255] = 'x'; memory[256] = 0; copied_bytes = 0;
    CHECK(copy_path(base, copied, copy).code() == StatusCode::name_too_long && copied_bytes == 256);
    copied_bytes = 0;
    CHECK(copy_path(static_cast<Word>(base - 1), copied, copy).code() == StatusCode::fault && copied_bytes == 1);
    usize wrapping_bytes{};
    CHECK(copy_path(static_cast<Word>(~Word{}), copied, [&](Word, char &value) {
        ++wrapping_bytes; value = 'x'; return true;
    }).code() == StatusCode::fault && wrapping_bytes == 1);
    using Result = fs::ModeChangeResult;
    usize reads{}, changes{};
    Word address{};
    Request seen{};
    std::string_view text = "target";
    Status read_status;
    auto result = Result::applied();
    auto read = [&](Word pointer, FixedString<256> &path) {
        ++reads; address = pointer;
        return read_status ? path.assign(text) : read_status;
    };
    auto change = [&](Request request, std::string_view name) {
        ++changes; seen = request;
        if (name != (request.direct_fd ? std::string_view{} : text))
            return Result::failure(Status::io_error("path callback mismatch"));
        return result;
    };
    auto run = [&](Operation call, std::array<Word, 6> arguments, bool native = true) {
        reads = changes = 0;
        return dispatch(native, call, arguments, read, change);
    };
    constexpr Word cwd = static_cast<Word>(-100);
    constexpr Word pointer = 0x8000;
    CHECK(run(Operation::chmod, {pointer, 04750}) == 0);
    CHECK(reads == 1 && changes == 1 && address == pointer && seen.fd == -100 && seen.mode == 04750);
    CHECK(!seen.ownership && !seen.direct_fd && seen.flags == 0);
    CHECK(run(Operation::fchmod, {7, 07777}) == 0);
    CHECK(reads == 0 && changes == 1 && seen.fd == 7 && seen.mode == 07777 && seen.direct_fd);
    CHECK(run(Operation::fchmodat, {cwd, pointer, 0600, OK_AT_SYMLINK_NOFOLLOW}) == 0);
    CHECK(reads == 1 && changes == 1 && seen.fd == -100 && seen.flags == OK_AT_SYMLINK_NOFOLLOW);
    CHECK(run(Operation::chown, {pointer, 0xffffffffU, 0x80000001U}) == 0);
    CHECK(seen.ownership && !seen.direct_fd && seen.uid == 0xffffffffU && seen.gid == 0x80000001U);
    CHECK(run(Operation::fchown, {8, 0xfffffffeU, 0xffffffffU}) == 0);
    CHECK(reads == 0 && seen.fd == 8 && seen.direct_fd && seen.uid == 0xfffffffeU && seen.gid == 0xffffffffU);
    CHECK(run(Operation::fchownat, {cwd, pointer, 1, 2, OK_AT_EMPTY_PATH | OK_AT_SYMLINK_NOFOLLOW}) == 0);
    CHECK(seen.fd == -100 && seen.uid == 1 && seen.gid == 2 && seen.flags == 0x1100U);
    for (auto call : {Operation::chmod, Operation::fchmod, Operation::fchmodat, Operation::chown, Operation::fchown, Operation::fchownat})
    {
        std::array<Word, 6> args{};
        args[5] = 1;
        CHECK(run(call, args) == -22 && reads == 0 && changes == 0);
        CHECK(run(call, args, false) == -38 && reads == 0 && changes == 0);
    }
    CHECK(run(Operation::fchmodat, {cwd, 0, 0600, 0x800}) == -22 && reads == 0 && changes == 0);
    CHECK(run(Operation::fchownat, {cwd, 0, 1, 2, 0x800}) == -22 && reads == 0 && changes == 0);
    CHECK(run(Operation::chmod, {0, 0600}) == -14 && reads == 0 && changes == 0);
    read_status = Status::fault("user path");
    CHECK(run(Operation::chown, {pointer, 1, 2}) == -14 && reads == 1 && changes == 0);
    read_status = Status::name_too_long("bounded path");
    CHECK(run(Operation::chmod, {pointer, 0600}) == -36 && changes == 0);
    read_status = Status::success();
    text = "";
    CHECK(run(Operation::chown, {pointer, 1, 2}) == -2 && reads == 1 && changes == 0);
    CHECK(run(Operation::fchmodat, {cwd, pointer, 0600, OK_AT_EMPTY_PATH}) == 0 && changes == 1);
    CHECK(run(Operation::fchownat, {cwd, pointer, 0xffffffffU, 0xffffffffU, OK_AT_EMPTY_PATH}) == 0);
    CHECK(seen.uid == 0xffffffffU && seen.gid == 0xffffffffU);
    text = "target";
    CHECK(run(Operation::fchmodat, {static_cast<Word>(u32{0xffffff9c}), pointer, 0600, 0}) == 0 && seen.fd == -100);
    if constexpr (sizeof(Word) > sizeof(u32))
    {
        constexpr Word high = Word{1} << 32;
        CHECK(run(Operation::chmod, {pointer, high | 0600}) == -22 && reads == 0 && changes == 0);
        CHECK(run(Operation::chown, {pointer, high | 1, 2}) == -22 && reads == 0 && changes == 0);
        CHECK(run(Operation::chown, {pointer, 1, high | 2}) == -22 && reads == 0 && changes == 0);
        CHECK(run(Operation::fchmodat, {cwd, pointer, 0600, high}) == -22 && reads == 0 && changes == 0);
        CHECK(run(Operation::fchownat, {cwd, pointer, 1, 2, high}) == -22 && reads == 0 && changes == 0);
        CHECK(run(Operation::fchmod, {high | 7, 0600}) == -9 && reads == 0 && changes == 0);
        CHECK(run(Operation::fchownat, {high | 0xffffff9cU, pointer, 1, 2}) == -9 && reads == 0 && changes == 0);
    }
    result = Result::read_only();
    CHECK(run(Operation::fchmod, {7, 0600}) == -30 && changes == 1);
    result = Result::unavailable();
    CHECK(run(Operation::fchown, {7, 1, 2}) == -95 && changes == 1);
    result = Result::failure(Status::permission("root owner check"));
    CHECK(run(Operation::fchown, {7, 0xffffffffU, 0xffffffffU}) == -1);
    result = Result::failure(Status::denied("component search"));
    CHECK(run(Operation::chmod, {pointer, 0600}) == -13);
    result = Result::failure(Status::bad_descriptor());
    CHECK(run(Operation::fchmod, {7, 0600}) == -9);
    result = Result::failure(Status::io_error("journal failure"));
    CHECK(run(Operation::chown, {pointer, 1, 2}) == -5);
    return true;
}
}

int main()
{
    static_assert(OK_UAPI_VERSION_MINOR == 16 && OK_NATIVE_METADATA_VERSION == 1);
    static_assert(ok::user::native_metadata_module_abi == 74);
    static_assert(OK_SYS_CHMOD == 90 && OK_SYS_FCHMOD == 91 && OK_SYS_CHOWN == 92 && OK_SYS_FCHOWN == 93);
    static_assert(OK_SYS_FCHOWNAT == 260 && OK_SYS_FCHMODAT == 268);
    static_assert(static_cast<u64>(syscall::Number::chmod) == OK_SYS_CHMOD && static_cast<u64>(syscall::Number::fchmod) == OK_SYS_FCHMOD);
    static_assert(static_cast<u64>(syscall::Number::chown) == OK_SYS_CHOWN && static_cast<u64>(syscall::Number::fchown) == OK_SYS_FCHOWN);
    static_assert(static_cast<u64>(syscall::Number::fchmodat) == OK_SYS_FCHMODAT && static_cast<u64>(syscall::Number::fchownat) == OK_SYS_FCHOWNAT);
    static_assert(select(syscall::Number::chmod) == Operation::chmod);
    static_assert(select(syscall::Number::fchmod) == Operation::fchmod);
    static_assert(select(syscall::Number::fchmodat) == Operation::fchmodat);
    static_assert(select(syscall::Number::chown) == Operation::chown);
    static_assert(select(syscall::Number::fchown) == Operation::fchown);
    static_assert(select(syscall::Number::fchownat) == Operation::fchownat);
    static_assert(!select(syscall::Number::read));
    if (!words<u32>() || !words<u64>()) return 1;
    std::printf("PASS NATIVE_METADATA_DISPATCH checks=%zu word-models=2 user-execution=0 guest=0\n", checks);
}
