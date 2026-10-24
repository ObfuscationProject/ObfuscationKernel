// Actual Linux adapter and native rename service over hosted user mappings.
#include "unavailable_realtime.hpp"
#include "../../src/user/runtime.cpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ok;
namespace hw = ok::arch::execution;
namespace lu = ok::abi::linux_user;
namespace native = ok::user::native;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (false)

namespace mock
{
std::array<std::byte, 8192> memory{};
uptr forbidden{};
usize copies{};
} // namespace mock

namespace ok::arch
{
Status initialize_clock() { return Status::success(); }
Result<u64> monotonic_nanoseconds() { return u64{3000000042}; }
u64 clock_resolution_ns() { return 1; }
} // namespace ok::arch

namespace ok::arch::execution
{
bool supported() noexcept { return false; }
Status initialize() { return Status::unsupported(); }
Result<AddressSpace> create_space(MappingView) { return Status::unsupported(); }
Status destroy_space(AddressSpace &space) { space = {}; return Status::success(); }
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &view)
{
    if (!view.valid()) return Status::invalid_argument("hosted entry requires a live lease");
    auto result = enter(space, frame);
    if (result) result.value().kernel_stack_pointer = view.lower();
    return result;
}
Result<Trap> enter(AddressSpace &, UserFrame &) { return Status::unsupported(); }
UserFrame initial_frame(uptr pc, uptr sp) { UserFrame f{}; f.pc = pc; f.sp = sp; return f; }
Syscall syscall(const UserFrame &) { return {}; }
void set_return_value(UserFrame &f, i64 v) { f.registers[0] = static_cast<uptr>(v); }
bool user_range(const AddressSpace &, uptr address, usize size, bool)
{
    if (!size) return true;
    if (address < user_begin || address - user_begin >= mock::memory.size() ||
        size > mock::memory.size() - (address - user_begin)) return false;
    return !mock::forbidden || address >= mock::forbidden + 1 ||
           size <= mock::forbidden - address;
}
Status copy_from_user(const AddressSpace &space, void *out, uptr address, usize size)
{
    ++mock::copies;
    if (!user_range(space, address, size, false)) return Status::fault();
    if (size) std::memcpy(out, mock::memory.data() + address - user_begin, size);
    return Status::success();
}
Status copy_to_user(const AddressSpace &space, uptr address, const void *in, usize size)
{
    if (!user_range(space, address, size, true)) return Status::fault();
    if (size) std::memcpy(mock::memory.data() + address - user_begin, in, size);
    return Status::success();
}
} // namespace ok::arch::execution

template <lu::Profile P>
constexpr std::array<i32, 5> argument_slots()
{
    if constexpr (P == lu::Profile::i386) return {4, 6, 5, 1, 0};
    else if constexpr (P == lu::Profile::x86_64) return {9, 10, 11, 5, 7};
    else if constexpr (P == lu::Profile::aarch64 || P == lu::Profile::arm32) return {0, 1, 2, 3, 4};
    else if constexpr (P == lu::Profile::rv64 || P == lu::Profile::rv32) return {10, 11, 12, 13, 14};
    else if constexpr (P == lu::Profile::loongarch64) return {4, 5, 6, 7, 8};
    else if constexpr (P == lu::Profile::mips) return {4, 5, 6, 7, -1};
    else if constexpr (P == lu::Profile::mips64) return {4, 5, 6, 7, 8};
    else return {3, 4, 5, 6, 7};
}

template <lu::Profile P>
constexpr usize number_slot()
{
    if constexpr (P == lu::Profile::i386) return 7;
    else if constexpr (P == lu::Profile::x86_64) return 14;
    else return lu::Codec<P>::profile.number_register;
}

template <lu::Profile P>
struct Calls
{
    Kernel &kernel;
    sched::ProcessId pid;
    usize forwarded{};
    syscall::Number last_native{};
    constexpr static uptr old_address = hw::user_begin + 128;
    constexpr static uptr new_address = hw::user_begin + 512;
    constexpr static uptr stack_address = hw::user_begin + 1024;

    static uptr path(uptr address, const char *value)
    {
        CHECK(std::strlen(value) < 256);
        std::memcpy(mock::memory.data() + address - hw::user_begin, value, std::strlen(value) + 1);
        return address;
    }

    i64 invoke(lu::Operation operation, u64 oldfd, u64 oldpath, u64 newfd, u64 newpath,
               u64 flags = 0)
    {
        const auto number = lu::Codec<P>::syscall_number(operation);
        CHECK(number);
        hw::UserFrame frame{};
        frame.registers.fill(~uptr{0});
        frame.registers[number_slot<P>()] = *number;
        frame.sp = stack_address;
        const std::array<u64, 5> arguments = operation == lu::Operation::rename
            ? std::array<u64, 5>{oldpath, newpath, 0, 0, 0}
            : std::array<u64, 5>{oldfd, oldpath, newfd, newpath, flags};
        constexpr auto slots = argument_slots<P>();
        const usize count = operation == lu::Operation::rename ? 2 :
                            operation == lu::Operation::renameat ? 4 : 5;
        for (usize i = 0; i < count; ++i)
            if (slots[i] >= 0) frame.registers[static_cast<usize>(slots[i])] = arguments[i];
        if constexpr (P == lu::Profile::mips)
        {
            const u32 word = static_cast<u32>(flags);
            const usize offset = stack_address + 16 - hw::user_begin;
            for (usize i = 0; i < 4; ++i)
                mock::memory[offset + i] = static_cast<std::byte>(word >> (24 - i * 8));
        }
        bool complete = true;
        const auto result = user::linux_bootstrap::dispatch<P>(kernel, pid, frame, complete,
            [&](hw::Syscall request, bool &done) -> i64 {
                ++forwarded;
                last_native = static_cast<syscall::Number>(request.number);
                return native::dispatch(kernel, pid, request, done);
            });
        CHECK(complete && user::linux_bootstrap::complete<P>(frame, result));
        auto returned = lu::NativeBridge<P>::decode_return(frame);
        CHECK(returned);
        u32 error = result < 0 ? static_cast<u32>(-result) : 0;
        if (error == 95) error = *lu::Codec<P>::errno_number(lu::Error::e_opnotsupp);
        if (error) CHECK(returned.value().error == error);
        else CHECK(returned.value().error == 0 && returned.value().value == static_cast<u64>(result));
        return result;
    }

    i64 renameat(u64 oldfd, const char *oldpath, u64 newfd, const char *newpath,
                 u64 flags = 0, bool force_at2 = false)
    {
        const auto operation = !force_at2 && lu::Codec<P>::syscall_number(lu::Operation::renameat)
            ? lu::Operation::renameat : lu::Operation::renameat2;
        return invoke(operation, oldfd, path(old_address, oldpath), newfd,
                      path(new_address, newpath), flags);
    }
};

static void create_file(Kernel &kernel, const char *name, const char *contents)
{
    auto fd = kernel.posix().open(name, posix::o_CREAT | posix::o_RDWR, 0644);
    CHECK(fd);
    const auto length = std::strlen(contents);
    CHECK(kernel.posix().write(fd.value(), {reinterpret_cast<const std::byte *>(contents), length}).value() == length);
    CHECK(kernel.posix().close(fd.value()));
}

static void expect_contents(Kernel &kernel, const char *name, const char *contents)
{
    auto fd = kernel.posix().open(name, posix::o_RDONLY);
    CHECK(fd);
    char buffer[32]{};
    const auto expected = std::strlen(contents);
    CHECK(kernel.posix().read(fd.value(), {reinterpret_cast<std::byte *>(buffer), expected}).value() == expected);
    CHECK(std::memcmp(buffer, contents, expected) == 0);
    CHECK(kernel.posix().close(fd.value()));
}

template <lu::Profile P>
void rename_profile(Kernel &kernel, sched::ProcessId pid)
{
    char base[48];
    std::snprintf(base, sizeof base, "/rename-%s", lu::Codec<P>::profile.name);
    CHECK(kernel.posix().mkdir(base, 0777) && kernel.posix().chdir(base));
    Calls<P> call{kernel, pid};
    const u64 cwd = static_cast<u32>(posix::at_FDCWD);
    create_file(kernel, "source", "new-data");
    create_file(kernel, "target", "old-data");
    auto retained = kernel.posix().open("target", posix::o_RDONLY);
    CHECK(retained);
    if (lu::Codec<P>::syscall_number(lu::Operation::rename))
    {
        CHECK(call.invoke(lu::Operation::rename, 0,
                          Calls<P>::path(call.old_address, "source"), 0,
                          Calls<P>::path(call.new_address, "target")) == 0);
        CHECK(call.last_native == syscall::Number::rename);
    }
    else CHECK(call.renameat(cwd, "source", cwd, "target") == 0);
    CHECK(!kernel.posix().stat("source"));
    expect_contents(kernel, "target", "new-data");
    char old_buffer[8]{};
    CHECK(kernel.posix().read(retained.value(), {reinterpret_cast<std::byte *>(old_buffer), 8}).value() == 8);
    CHECK(std::memcmp(old_buffer, "old-data", 8) == 0);
    CHECK(kernel.posix().close(retained.value()));

    CHECK(kernel.posix().mkdir("other", 0755));
    auto current = kernel.posix().open(".", posix::o_RDONLY | posix::o_DIRECTORY);
    auto other = kernel.posix().open("other", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(current && other);
    CHECK(call.renameat(current.value(), "target", other.value(), "moved") == 0);
    CHECK(call.last_native == syscall::Number::renameat);
    CHECK(!kernel.posix().stat("target"));
    expect_contents(kernel, "other/moved", "new-data");
    CHECK(call.renameat(other.value(), "moved", other.value(), "final", 0, true) == 0);
    CHECK(call.last_native == syscall::Number::renameat);
    CHECK(!kernel.posix().stat("other/moved"));
    expect_contents(kernel, "other/final", "new-data");

    // Invalid pointers and relative dirfds fail without moving the source.
    CHECK(call.invoke(lu::Operation::renameat2, other.value(), 0,
                      other.value(), Calls<P>::path(call.new_address, "bad")) == -14);
    CHECK(call.invoke(lu::Operation::renameat2, other.value(),
                      Calls<P>::path(call.old_address, "final"), other.value(), 0) == -14);
    CHECK(call.renameat(0x1234ffffU, "final", other.value(), "bad", 0, true) == -9);
    CHECK(call.renameat(other.value(), "final", 0x1234ffffU, "bad", 0, true) == -9);
    expect_contents(kernel, "other/final", "new-data");

    create_file(kernel, "other/flag-source", "flag-src");
    create_file(kernel, "other/flag-target", "flag-dst");
    for (u64 flags : {1u, 2u, 4u})
    {
        const auto before = call.forwarded;
        const auto copies = mock::copies;
        CHECK(call.renameat(other.value(), "flag-source", other.value(), "flag-target", flags, true) == -95);
        CHECK(call.forwarded == before);
        CHECK(mock::copies - copies == (P == lu::Profile::mips ? 1 : 0));
        expect_contents(kernel, "other/flag-source", "flag-src");
        expect_contents(kernel, "other/flag-target", "flag-dst");
    }
    for (u64 flags : {3u, 6u, 8u, 0x80000000u})
    {
        const auto before = call.forwarded;
        const auto copies = mock::copies;
        CHECK(call.renameat(other.value(), "flag-source", other.value(), "flag-target", flags, true) == -22);
        CHECK(call.forwarded == before);
        CHECK(mock::copies - copies == (P == lu::Profile::mips ? 1 : 0));
    }
    const auto before = call.forwarded;
    CHECK(call.invoke(lu::Operation::renameat2, other.value(), 0, other.value(), 0, 1) == -95);
    CHECK(call.invoke(lu::Operation::renameat2, other.value(), 0, other.value(), 0, 8) == -22);
    CHECK(call.forwarded == before);
    if constexpr (P == lu::Profile::mips)
    {
        mock::forbidden = call.stack_address + 16;
        CHECK(call.renameat(other.value(), "flag-source", other.value(), "flag-target", 0, true) == -14);
        CHECK(call.forwarded == before);
        mock::forbidden = 0;
    }
    expect_contents(kernel, "other/flag-source", "flag-src");
    expect_contents(kernel, "other/flag-target", "flag-dst");

    CHECK(kernel.posix().chmod("other", 0555));
    CHECK(kernel.posix().set_identity(1000, 1000));
    CHECK(call.renameat(other.value(), "final", other.value(), "denied", 0, true) == -13);
    CHECK(kernel.posix().set_identity(0, 0));
    expect_contents(kernel, "other/final", "new-data");
    CHECK(kernel.posix().chmod("other", 0755));
    CHECK(kernel.posix().close(other.value()) && kernel.posix().close(current.value()));
    CHECK(kernel.posix().unlink("other/final") && kernel.posix().unlink("other/flag-source") &&
          kernel.posix().unlink("other/flag-target") && kernel.posix().rmdir("other"));
    CHECK(kernel.posix().chdir("/") && kernel.posix().rmdir(base));
    std::printf("PASS Linux rename %s: replacement/retained fd, dirfd, flags, faults, permissions\n",
                lu::Codec<P>::profile.name);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    static Kernel kernel;
    CHECK(kernel.boot({}));
    auto made = kernel.scheduler().create_user_process("rename-adapter",
        kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096}));
    CHECK(made && kernel.posix().activate(made.value()));
    const auto pid = made.value();
    rename_profile<lu::Profile::i386>(kernel, pid);
    rename_profile<lu::Profile::x86_64>(kernel, pid);
    rename_profile<lu::Profile::aarch64>(kernel, pid);
    rename_profile<lu::Profile::arm32>(kernel, pid);
    rename_profile<lu::Profile::rv64>(kernel, pid);
    rename_profile<lu::Profile::rv32>(kernel, pid);
    rename_profile<lu::Profile::loongarch64>(kernel, pid);
    rename_profile<lu::Profile::mips>(kernel, pid);
    rename_profile<lu::Profile::mips64>(kernel, pid);
    rename_profile<lu::Profile::ppc>(kernel, pid);
}
