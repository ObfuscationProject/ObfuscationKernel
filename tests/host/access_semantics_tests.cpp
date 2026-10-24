// Hosted real service/resolver/dispatcher with checked-copy model mappings.
#include "native_clock_model.hpp"
#include <bit>

namespace
{
usize checks{}, calls{}, profiles{}, oracle_cases{};
#undef CHECK
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); std::abort(); } } while (false)

bool same(user::Credentials a, user::Credentials b)
{
    return a.uid == b.uid && a.gid == b.gid && a.euid == b.euid && a.egid == b.egid &&
           a.suid == b.suid && a.sgid == b.sgid && a.kernel_space == b.kernel_space;
}

void service(Kernel &kernel)
{
    auto &p = kernel.posix();
    CHECK(p.set_identity(0, 0));
    const auto create = [&](std::string_view name, u32 mode, u32 uid, u32 gid) {
        auto fd = p.open(name, posix::o_CREAT | posix::o_RDWR, mode);
        CHECK(fd && p.close(fd.value()));
        CHECK(p.chmod(name, mode) && p.chown(name, uid, gid));
    };
    create("/tmp/access-root", 0600, 0, 0);
    create("/tmp/access-owner", 0400, 1000, 2000);
    create("/tmp/access-group", 0040, 3000, 2000);
    create("/tmp/access-other", 0004, 3000, 4000);
    create("/tmp/access-exec", 0000, 0, 0);
    CHECK(p.mkdir("/tmp/access-search", 0700));
    create("/tmp/access-search/child", 0644, 1000, 2000);
    CHECK(p.chown("/tmp/access-search", 1000, 2000));
    CHECK(p.symlinkat("absent-target", posix::at_FDCWD, "/tmp/access-link"));

    CHECK(p.set_credentials({.uid = 1000, .gid = 2000, .euid = 0, .egid = 0}));
    const auto elevated = p.user_credentials();
    CHECK(p.access("/tmp/access-root", posix::r_OK).code() == StatusCode::denied);
    CHECK(p.faccessat(-999, "/tmp/access-root", posix::r_OK, posix::at_EACCESS));
    CHECK(p.access("/tmp/access-owner", posix::r_OK));
    CHECK(same(elevated, p.user_credentials()));
    CHECK(p.set_credentials({.uid = 0, .gid = 0, .euid = 1000, .egid = 2000}));
    CHECK(p.access("/tmp/access-root", posix::r_OK));
    CHECK(p.faccessat(posix::at_FDCWD, "/tmp/access-root", posix::r_OK,
                     posix::at_EACCESS).code() == StatusCode::denied);
    CHECK(p.access("/tmp/access-exec", posix::x_OK).code() == StatusCode::denied);
    CHECK(p.access("/tmp/access-exec", posix::r_OK | posix::w_OK));
    CHECK(p.set_identity(0, 0));
    CHECK(p.chmod("/tmp/access-exec", 0010));
    CHECK(p.access("/tmp/access-exec", posix::x_OK));
    CHECK(p.chmod("/tmp/access-search", 0000));
    CHECK(p.access("/tmp/access-search", posix::x_OK));
    CHECK(p.chmod("/tmp/access-search", 0700));

    CHECK(p.set_credentials({.uid = 5000, .gid = 2000, .euid = 5000, .egid = 4000}));
    CHECK(p.access("/tmp/access-group", posix::r_OK));
    CHECK(p.faccessat(posix::at_FDCWD, "/tmp/access-group", posix::r_OK,
                     posix::at_EACCESS).code() == StatusCode::denied);
    CHECK(p.access("/tmp/access-other", posix::r_OK));
    CHECK(p.set_credentials({.uid = 3000, .gid = 2000, .euid = 3000, .egid = 2000}));
    CHECK(p.access("/tmp/access-group", posix::r_OK).code() == StatusCode::denied);
    CHECK(p.set_credentials({.uid = 5000, .gid = 4000, .euid = 1000, .egid = 2000}));
    CHECK(p.access("/tmp/access-search/child", posix::f_OK).code() == StatusCode::denied);
    CHECK(p.faccessat(posix::at_FDCWD, "/tmp/access-search/child", posix::f_OK, posix::at_EACCESS));

    CHECK(p.set_identity(1000, 2000));
    auto directory = p.open("/tmp/access-search", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(directory);
    CHECK(p.faccessat(directory.value(), "child", posix::r_OK));
    CHECK(p.faccessat(-999, "child", posix::f_OK).code() == StatusCode::bad_descriptor);
    auto file = p.open("/tmp/access-owner", posix::o_RDONLY);
    CHECK(file);
    CHECK(p.faccessat(file.value(), "child", posix::f_OK).code() == StatusCode::not_directory);
    CHECK(p.faccessat(file.value(), "", posix::r_OK, posix::at_EMPTY_PATH));
    CHECK(p.faccessat(file.value(), "", posix::w_OK, posix::at_EMPTY_PATH).code() == StatusCode::denied);
    CHECK(p.faccessat(-999, "", posix::f_OK, posix::at_EMPTY_PATH).code() == StatusCode::bad_descriptor);
    CHECK(p.faccessat(file.value(), "", posix::f_OK).code() == StatusCode::not_found);
    auto pair = p.pipe();
    CHECK(pair);
    for (auto fd : pair.value())
    {
        CHECK(p.faccessat(fd, "", posix::f_OK, posix::at_EMPTY_PATH));
        for (u32 mode : {posix::r_OK, posix::w_OK, posix::x_OK, 7u})
            CHECK(p.faccessat(fd, "", mode, posix::at_EMPTY_PATH).code() == StatusCode::unsupported);
    }
    CHECK(p.faccessat(1, "", posix::f_OK, posix::at_EMPTY_PATH));
    CHECK(p.faccessat(1, "", posix::w_OK, posix::at_EMPTY_PATH).code() == StatusCode::unsupported);
    CHECK(p.close(pair.value()[0]) && p.close(pair.value()[1]));
    CHECK(p.faccessat(pair.value()[0], "", posix::f_OK, posix::at_EMPTY_PATH).code() == StatusCode::bad_descriptor);
    CHECK(p.access("/tmp/access-owner/", posix::f_OK).code() == StatusCode::not_directory);
    CHECK(p.access("/tmp/access-link", posix::f_OK).code() == StatusCode::not_found);
    CHECK(p.faccessat(posix::at_FDCWD, "/tmp/access-link", 7, posix::at_SYMLINK_NOFOLLOW));
    CHECK(p.faccessat(-999, "absent", 0x80000000u).code() == StatusCode::invalid_argument);
    CHECK(p.faccessat(-999, "absent", 0, 0x80000000u).code() == StatusCode::invalid_argument);

    CHECK(p.chdir("/tmp/access-search"));
    CHECK(p.set_identity(0, 0));
    CHECK(p.rename("/tmp/access-search", "/tmp/access-moved"));
    CHECK(p.set_identity(1000, 2000));
    CHECK(p.access("child", posix::r_OK));
    CHECK(p.faccessat(directory.value(), "child", posix::r_OK));
    CHECK(p.unlink("/tmp/access-owner"));
    CHECK(p.access("/tmp/access-owner", posix::f_OK).code() == StatusCode::not_found);
    CHECK(p.faccessat(file.value(), "", posix::r_OK, posix::at_EMPTY_PATH));
    CHECK(p.close(file.value()));
    CHECK(p.faccessat(file.value(), "", 0, posix::at_EMPTY_PATH).code() == StatusCode::bad_descriptor);
    CHECK(p.set_identity(0, 0));
    CHECK(p.unlink("/tmp/access-moved/child") && p.rmdir("/tmp/access-moved"));
    CHECK(p.chmod(".", 0000));
    CHECK(p.set_identity(1000, 2000));
    CHECK(p.faccessat(posix::at_FDCWD, "", posix::f_OK, posix::at_EMPTY_PATH));
    CHECK(p.faccessat(posix::at_FDCWD, "", posix::x_OK, posix::at_EMPTY_PATH).code() == StatusCode::denied);
    CHECK(p.access(".", posix::f_OK).code() == StatusCode::denied);
    CHECK(p.close(directory.value()) && p.chdir("/"));

    // Exhaustive mode queries against an independent bit-by-bit DAC oracle.
    CHECK(p.set_identity(0, 0));
    create("/tmp/access-oracle", 0, 71, 72);
    for (u32 mode = 0; mode < 512; ++mode)
    {
        CHECK(p.set_identity(0, 0) && p.chmod("/tmp/access-oracle", mode));
        const user::Credentials identities[] = {{71, 99, 71, 99, false}, {73, 72, 73, 72, false},
                                                 {73, 99, 73, 99, false}, {0, 99, 0, 99, false}};
        for (usize identity = 0; identity < 4; ++identity)
        {
            CHECK(p.set_credentials(identities[identity]));
            for (u32 query = 0; query < 8; ++query)
            {
                bool expected = true;
                for (u32 bit = 1; bit <= 4; bit *= 2)
                    if (query & bit)
                        expected = expected && (identity == 3
                            ? (bit != 1 || (mode & 0111u))
                            : (mode & (bit << (identity == 0 ? 6 : identity == 1 ? 3 : 0))));
                CHECK(bool(p.access("/tmp/access-oracle", query)) == expected);
                ++oracle_cases;
            }
        }
    }
    CHECK(p.set_identity(0, 0));
}

template <lu::Profile P> void adapter(Kernel &kernel, sched::ProcessId pid, sched::ProcessId other,
                                    usize number_slot, std::array<usize, 4> argument_slots,
                                    std::array<u32, 3> numbers)
{
    using C = lu::Codec<P>; using O = lu::Operation;
    auto *process = kernel.scheduler().find(pid);
    const O operations[]{O::access, O::faccessat, O::faccessat2};
    for (usize i = 0; i < 3; ++i)
    {
        CHECK(C::syscall_number(operations[i]).value_or(0) == numbers[i]);
        if (!numbers[i]) continue;
        const bool legacy = i == 0;
        auto invoke = [&](std::array<u64, 4> values, sched::ProcessId caller = 0) {
            hw::UserFrame frame{};
            frame.registers[number_slot] = numbers[i];
            for (usize slot = 0; slot < 4; ++slot)
                frame.registers[argument_slots[slot]] = values[slot] & C::word_mask;
            bool done = true;
            const auto result = user::linux_bootstrap::dispatch<P>(kernel, caller ? caller : pid, frame, done,
                [&](hw::Syscall call, bool &complete) { return native::dispatch(kernel, pid, call, complete); });
            CHECK(done && user::linux_bootstrap::complete<P>(frame, result));
            ++calls;
            return result;
        };
        const uptr pointer = hw::user_begin + 128;
        const char path[] = "/tmp/access-root";
        std::memcpy(mock::memory.data() + 128, path, sizeof path);
        process->set_credentials({.uid = 1000, .gid = 1000, .euid = 0, .egid = 0});
        auto args = legacy ? std::array<u64, 4>{pointer, posix::r_OK, ~u64{}, ~u64{}}
                           : std::array<u64, 4>{0xfeed0000ffffff9cULL, pointer, posix::r_OK, ~u64{}};
        if (i == 2) args[3] = 0;
        CHECK(invoke(args) == -13);
        const auto old = process->credentials();
        if (i == 2) { args[3] = 0xfeed000000000200ULL; CHECK(invoke(args) == 0); }
        CHECK(same(old, process->credentials()));
        CHECK(invoke(args, other) == -22);
        const usize mode_arg = legacy ? 1 : 2, path_arg = legacy ? 0 : 1;
        args[mode_arg] = 8; args[path_arg] = 1; mock::copies = 0;
        CHECK(invoke(args) == -22 && mock::copies == 0);
        args[mode_arg] = 0;
        if (i == 2)
        {
            args[3] = 0x800; CHECK(invoke(args) == -22 && mock::copies == 0);
            args[3] = posix::at_EMPTY_PATH;
        }
        args[path_arg] = 0; CHECK(invoke(args) == -14 && mock::copies == 0);
        args[path_arg] = hw::user_begin + mock::memory.size() - 1;
        mock::memory.back() = std::byte{0};
        const auto empty = i == 2 ? 0 : -2;
        CHECK(invoke(args) == empty && mock::last_input == args[path_arg]);
        mock::memory.back() = std::byte{'x'};
        CHECK(invoke(args) == -14);
        mock::memory.fill(std::byte{'x'});
        args[path_arg] = pointer; CHECK(invoke(args) == -36);
    }
    ++profiles;
}
}

int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    const auto context = kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096});
    auto process = kernel.scheduler().create_user_process("access-model", context);
    auto other = kernel.scheduler().create_user_process("access-other", context);
    CHECK(process && other && kernel.posix().activate(process.value()));
    service(kernel);
    adapter<lu::Profile::i386>(kernel, process.value(), other.value(), 7, {4, 6, 5, 1}, {33, 307, 439});
    adapter<lu::Profile::x86_64>(kernel, process.value(), other.value(), 14, {9, 10, 11, 5}, {21, 269, 439});
    adapter<lu::Profile::aarch64>(kernel, process.value(), other.value(), 8, {0, 1, 2, 3}, {0, 48, 439});
    adapter<lu::Profile::arm32>(kernel, process.value(), other.value(), 7, {0, 1, 2, 3}, {33, 334, 439});
    adapter<lu::Profile::rv64>(kernel, process.value(), other.value(), 17, {10, 11, 12, 13}, {0, 48, 439});
    adapter<lu::Profile::rv32>(kernel, process.value(), other.value(), 17, {10, 11, 12, 13}, {0, 48, 439});
    adapter<lu::Profile::loongarch64>(kernel, process.value(), other.value(), 11, {4, 5, 6, 7}, {0, 48, 439});
    adapter<lu::Profile::mips>(kernel, process.value(), other.value(), 2, {4, 5, 6, 7}, {4033, 4300, 4439});
    adapter<lu::Profile::mips64>(kernel, process.value(), other.value(), 2, {4, 5, 6, 7}, {5020, 5259, 5439});
    adapter<lu::Profile::ppc>(kernel, process.value(), other.value(), 0, {3, 4, 5, 6}, {33, 298, 439});
    CHECK(profiles == 10 && oracle_cases == 16384);
    std::printf("ACCESS_SEMANTICS_MODEL_PASS checks=%zu profiles=%zu calls=%zu oracle=%zu checked_copy=1 guest=0\n",
                checks, profiles, calls, oracle_cases);
}
