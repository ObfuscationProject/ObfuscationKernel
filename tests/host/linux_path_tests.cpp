#include "unavailable_realtime.hpp"
// Actual Linux adapter/common native copy boundary with hosted mappings only.
#include "native_runtime_units.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
using namespace ok;
namespace native = ok::user::native;
namespace hw = ok::arch::execution;
namespace lu = ok::abi::linux_user;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                                            \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)
namespace mock
{
std::array<std::byte, 8192> memory{};
uptr forbidden{};
usize copies{};
} // namespace mock
namespace ok::arch
{
Status initialize_clock()
{
    return Status::success();
}
Result<u64> monotonic_nanoseconds()
{
    return u64{3000000042};
}
u64 clock_resolution_ns()
{
    return 1;
}
} // namespace ok::arch
namespace ok::arch::execution
{
bool supported() noexcept
{
    return false;
}
OwnershipReport ownership_report() noexcept { return {}; }
Status initialize()
{
    return Status::unsupported();
}
Result<AddressSpace> create_space(MappingView)
{
    return Status::unsupported();
}
Status destroy_space(AddressSpace &space)
{
    space = {};
    return Status::success();
}
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &view)
{
    if (!view.valid()) return Status::invalid_argument("hosted entry requires a live lease");
    auto result = enter(space, frame);
    if (result) result.value().kernel_stack_pointer = view.lower(); // Synthetic hosted capture only.
    return result;
}
Result<Trap> enter(AddressSpace &, UserFrame &)
{
    return Status::unsupported();
}
UserFrame initial_frame(uptr pc, uptr sp)
{
    UserFrame f{};
    f.pc = pc;
    f.sp = sp;
    return f;
}
Syscall syscall(const UserFrame &)
{
    return {};
}
void set_return_value(UserFrame &f, i64 v)
{
    f.registers[0] = static_cast<uptr>(v);
}
bool user_range(const AddressSpace &, uptr address, usize size, bool)
{
    return !size || (address >= user_begin && address - user_begin < mock::memory.size() &&
                     size <= mock::memory.size() - (address - user_begin) &&
                     (!mock::forbidden || address >= mock::forbidden + 1 || size <= mock::forbidden - address));
}
Status copy_from_user(const AddressSpace &s, void *out, uptr address, usize size)
{
    ++mock::copies;
    if (!user_range(s, address, size, false))
        return Status::fault();
    if (size)
        std::memcpy(out, mock::memory.data() + address - user_begin, size);
    return Status::success();
}
Status copy_to_user(const AddressSpace &s, uptr address, const void *in, usize size)
{
    if (!user_range(s, address, size, true))
        return Status::fault();
    if (size)
        std::memcpy(mock::memory.data() + address - user_begin, in, size);
    return Status::success();
}
} // namespace ok::arch::execution

template <lu::Profile P>
void paths(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize, 4> slots)
{
    auto call = [&](lu::Operation operation, std::array<uptr, 4> arguments) {
        auto number = lu::Codec<P>::syscall_number(operation);
        CHECK(number);
        hw::UserFrame frame{};
        frame.registers[number_slot] = *number;
        for (usize i = 0; i < arguments.size(); ++i)
            frame.registers[slots[i]] = arguments[i];
        bool done = true;
        const i64 result = user::linux_bootstrap::dispatch<P>(kernel, pid, frame, done,
            [&](hw::Syscall request, bool &complete) { return native::dispatch(kernel, pid, request, complete); });
        CHECK(done && user::linux_bootstrap::complete<P>(frame, result));
        auto returned = lu::NativeBridge<P>::decode_return(frame);
        CHECK(returned);
        if (result >= 0) CHECK(returned.value().error == 0 && returned.value().value == static_cast<u64>(result));
        else CHECK(returned.value().error == lu::OpenCodec<P>::pathname_errno(static_cast<u32>(-result)));
        return result;
    };
    constexpr uptr name = hw::user_begin + 128, output = hw::user_begin + 512;
    auto path = [&](const char *value) {
        std::memcpy(mock::memory.data() + name - hw::user_begin, value, std::strlen(value) + 1);
        return name;
    };
    auto equals = [&](uptr address, const char *value) {
        CHECK(std::strcmp(reinterpret_cast<const char *>(mock::memory.data() + address - hw::user_begin), value) == 0);
    };
    CHECK(kernel.posix().chdir("/"));
    CHECK(call(lu::Operation::getcwd, {output, 2, 0, 0}) == 2);
    equals(output, "/");
    CHECK(call(lu::Operation::getcwd, {output, 1, 0, 0}) == -34);
    CHECK(call(lu::Operation::getcwd, {output, 0, 0, 0}) == -34);
    CHECK(call(lu::Operation::getcwd, {0, 10, 0, 0}) == -14);
    const uptr edge = hw::user_begin + mock::memory.size() - 2;
    CHECK(call(lu::Operation::getcwd, {edge, 4096, 0, 0}) == 2);
    equals(edge, "/");
    CHECK(call(lu::Operation::chdir, {path("/path-home/sub"), 0, 0, 0}) == 0);
    CHECK(call(lu::Operation::getcwd, {output, 64, 0, 0}) == 15);
    equals(output, "/path-home/sub");
    auto retained = kernel.posix().open(".", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(retained);
    CHECK(kernel.posix().rename("/path-home/sub", "/path-home/renamed"));
    CHECK(call(lu::Operation::getcwd, {output, 64, 0, 0}) == 19);
    equals(output, "/path-home/renamed");
    CHECK(call(lu::Operation::chdir, {path(".."), 0, 0, 0}) == 0);
    CHECK(call(lu::Operation::fchdir, {static_cast<uptr>(retained.value()), 0, 0, 0}) == 0);
    CHECK(call(lu::Operation::getcwd, {output, 64, 0, 0}) == 19);
    CHECK(kernel.posix().rename("/path-home/renamed", "/path-home/sub"));
    CHECK(call(lu::Operation::fchdir, {~uptr{0}, 0, 0, 0}) == -9);
    CHECK(call(lu::Operation::chdir, {0, 0, 0, 0}) == -14);
    CHECK(call(lu::Operation::chdir, {path("/missing-directory"), 0, 0, 0}) == -2);
    CHECK(call(lu::Operation::chdir, {path("/path-home/file"), 0, 0, 0}) == -20);
    CHECK(kernel.posix().close(retained.value()));
    auto directory = kernel.posix().open("/path-home", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(directory);
    mock::memory[output - hw::user_begin + 2] = std::byte{0x55};
    CHECK(call(lu::Operation::readlinkat, {static_cast<uptr>(directory.value()), path("link"), output, 2}) == 2);
    CHECK(mock::memory[output - hw::user_begin] == std::byte{'f'} &&
          mock::memory[output - hw::user_begin + 1] == std::byte{'i'} &&
          mock::memory[output - hw::user_begin + 2] == std::byte{0x55});
    CHECK(call(lu::Operation::readlinkat, {~uptr{0}, path("link"), output, 20}) == -9);
    CHECK(call(lu::Operation::readlinkat, {~uptr{0}, path("/path-home/link"), output, 20}) == 4);
    CHECK(call(lu::Operation::readlinkat, {static_cast<uptr>(directory.value()), path("file"), output, 20}) == -22);
    CHECK(call(lu::Operation::readlinkat, {static_cast<uptr>(directory.value()), path("link"), 0, 20}) == -14);
    CHECK(call(lu::Operation::readlinkat, {static_cast<uptr>(directory.value()), path("link"), output, 0}) == -22);
    CHECK(call(lu::Operation::readlinkat, {static_cast<uptr>(directory.value()), path("link"), output, 0x80000000u}) == -22);
    CHECK(call(lu::Operation::readlinkat, {static_cast<uptr>(directory.value()), path("link"), edge, 2}) == 2);
    if constexpr (lu::Codec<P>::profile.word_bytes == 8)
        CHECK(call(lu::Operation::readlinkat, {static_cast<uptr>(directory.value()), path("link"), output, (uptr{1} << 32) | 2}) == 2);
    if (lu::Codec<P>::syscall_number(lu::Operation::readlink))
        CHECK(call(lu::Operation::readlink, {path("/path-home/link"), output, 20, 0}) == 4);
    CHECK(kernel.posix().close(directory.value()));
    CHECK(kernel.posix().chdir("/"));
    CHECK(kernel.posix().mkdir("/path-home/denied", 0));
    auto denied = kernel.posix().open("/path-home/denied", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(denied && kernel.posix().set_identity(1000, 1000));
    CHECK(call(lu::Operation::chdir, {path("/path-home/denied"), 0, 0, 0}) == -13);
    CHECK(call(lu::Operation::fchdir, {static_cast<uptr>(denied.value()), 0, 0, 0}) == -13);
    CHECK(call(lu::Operation::getcwd, {output, 64, 0, 0}) == 2);
    equals(output, "/");
    CHECK(kernel.posix().set_identity(0, 0));
    CHECK(kernel.posix().close(denied.value()) && kernel.posix().rmdir("/path-home/denied"));
    CHECK(kernel.posix().mkdir("/path-home/gone"));
    CHECK(call(lu::Operation::chdir, {path("/path-home/gone"), 0, 0, 0}) == 0);
    CHECK(kernel.posix().rmdir("/path-home/gone"));
    CHECK(call(lu::Operation::getcwd, {output, 64, 0, 0}) == -2);
    CHECK(call(lu::Operation::chdir, {path("/"), 0, 0, 0}) == 0);
    directory = kernel.posix().open("/path-home", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(directory);
    const uptr dirfd = static_cast<uptr>(directory.value());
    auto removed = kernel.posix().open("/path-home/unlink-leaf", posix::o_CREAT | posix::o_RDWR);
    CHECK(removed);
    const char payload = 'x';
    CHECK(kernel.posix().write(removed.value(), {reinterpret_cast<const std::byte *>(&payload), 1}).value() == 1);
    CHECK(kernel.posix().seek(removed.value(), 0, posix::SeekWhence::set));
    CHECK(call(lu::Operation::unlinkat, {dirfd, path("unlink-leaf"), 1, 0}) == -22);
    CHECK(kernel.posix().stat("/path-home/unlink-leaf"));
    CHECK(call(lu::Operation::unlinkat, {~uptr{0}, path("unlink-leaf"), 0, 0}) == -9);
    CHECK(call(lu::Operation::unlinkat, {dirfd, 0, 0, 0}) == -14);
    CHECK(call(lu::Operation::unlinkat, {dirfd, path(""), 0, 0}) == -2);
    CHECK(call(lu::Operation::unlinkat, {dirfd, path("unlink-leaf"), 0, 0}) == 0);
    CHECK(!kernel.posix().stat("/path-home/unlink-leaf"));
    char retained_byte = 0;
    CHECK(kernel.posix().read(removed.value(), {reinterpret_cast<std::byte *>(&retained_byte), 1}).value() == 1);
    CHECK(retained_byte == payload && kernel.posix().close(removed.value()));
    CHECK(call(lu::Operation::unlinkat, {dirfd, path("unlink-leaf"), 0, 0}) == -2);
    CHECK(kernel.posix().symlinkat("file", posix::at_FDCWD, "/path-home/unlink-link"));
    CHECK(call(lu::Operation::unlinkat, {~uptr{0}, path("/path-home/unlink-link"), 0, 0}) == 0);
    CHECK(kernel.posix().stat("/path-home/file"));
    CHECK(kernel.posix().mkdir("/path-home/unlink-directory"));
    CHECK(call(lu::Operation::unlinkat, {dirfd, path("unlink-directory"), 0, 0}) == -21);
    auto child = kernel.posix().open("/path-home/unlink-directory/child", posix::o_CREAT | posix::o_RDWR);
    CHECK(child && kernel.posix().close(child.value()));
    CHECK(call(lu::Operation::unlinkat, {dirfd, path("unlink-directory"), 0x200, 0}) == -39);
    CHECK(call(lu::Operation::unlinkat, {dirfd, path("unlink-directory/child"), 0, 0}) == 0);
    CHECK(call(lu::Operation::unlinkat, {dirfd, path("unlink-directory"), 0x200, 0}) == 0);
    auto legacy = kernel.posix().open("/path-home/legacy-unlink", posix::o_CREAT | posix::o_RDWR);
    CHECK(legacy && kernel.posix().close(legacy.value()));
    CHECK(kernel.posix().set_identity(1000, 1000));
    CHECK(call(lu::Operation::unlinkat, {dirfd, path("legacy-unlink"), 0, 0}) == -13);
    CHECK(kernel.posix().set_identity(0, 0));
    if (lu::Codec<P>::syscall_number(lu::Operation::unlink))
        CHECK(call(lu::Operation::unlink, {path("/path-home/legacy-unlink"), 0, 0, 0}) == 0);
    else
        CHECK(call(lu::Operation::unlinkat, {dirfd, path("legacy-unlink"), 0, 0}) == 0);
    CHECK(kernel.posix().mkdir("/path-home/legacy-rmdir"));
    if (lu::Codec<P>::syscall_number(lu::Operation::rmdir))
        CHECK(call(lu::Operation::rmdir, {path("/path-home/legacy-rmdir"), 0, 0, 0}) == 0);
    else
        CHECK(call(lu::Operation::unlinkat, {dirfd, path("legacy-rmdir"), 0x200, 0}) == 0);
    CHECK(kernel.posix().close(directory.value()));
    std::printf("PASS Linux paths %s: retained cwd/dirfd, Linux sizes, copy boundaries and symlink bytes\n", lu::Codec<P>::profile.name);
}

int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    const auto context = kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096});
    auto made = kernel.scheduler().create_user_process("path-adapter", context);
    CHECK(made && kernel.posix().activate(made.value()));
    CHECK(kernel.posix().mkdir("/path-home"));
    CHECK(kernel.posix().mkdir("/path-home/sub"));
    auto file = kernel.posix().open("/path-home/file", posix::o_CREAT | posix::o_RDWR);
    CHECK(file && kernel.posix().close(file.value()));
    CHECK(kernel.posix().symlinkat("file", posix::at_FDCWD, "/path-home/link"));
    const auto pid = made.value();
    paths<lu::Profile::i386>(kernel, pid, 7, {4,6,5,1});
    paths<lu::Profile::x86_64>(kernel, pid, 14, {9,10,11,5});
    paths<lu::Profile::aarch64>(kernel, pid, 8, {0,1,2,3});
    paths<lu::Profile::arm32>(kernel, pid, 7, {0,1,2,3});
    paths<lu::Profile::rv64>(kernel, pid, 17, {10,11,12,13});
    paths<lu::Profile::rv32>(kernel, pid, 17, {10,11,12,13});
    paths<lu::Profile::loongarch64>(kernel, pid, 11, {4,5,6,7});
    paths<lu::Profile::mips>(kernel, pid, 2, {4,5,6,7});
    paths<lu::Profile::mips64>(kernel, pid, 2, {4,5,6,7});
    paths<lu::Profile::ppc>(kernel, pid, 0, {3,4,5,6});
}
