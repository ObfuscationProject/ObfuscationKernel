#include "unavailable_realtime.hpp"
// Actual Linux adapter/common native copy boundary with hosted mappings only.
#include "native_runtime_units.hpp"
#include <algorithm>
#include <array>
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
usize outputs{};
usize partial_output{};
uptr last_input{};
usize last_input_size{};
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
    mock::last_input=address; mock::last_input_size=size;
    if (!user_range(s, address, size, false))
        return Status::fault();
    if (size)
        std::memcpy(out, mock::memory.data() + address - user_begin, size);
    return Status::success();
}
Status copy_to_user(const AddressSpace &s, uptr address, const void *in, usize size)
{
    ++mock::outputs;
    if (!user_range(s, address, size, true))
        return Status::fault();
    if (mock::partial_output)
    {
        std::memcpy(mock::memory.data() + address - user_begin, in, mock::partial_output);
        return Status::fault();
    }
    if (size)
        std::memcpy(mock::memory.data() + address - user_begin, in, size);
    return Status::success();
}
} // namespace ok::arch::execution

#include "ok/posix/native_stat.hpp"
int main()
{
    static_assert(OK_SYS_FSTAT_V2 == 1026 && OK_SYS_NEWFSTATAT_V2 == 1027);
    static Kernel kernel;
    CHECK(kernel.boot({}));
    auto made = kernel.scheduler().create_user_process("native-stat-copy",
        kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096}));
    CHECK(made && kernel.posix().activate(made.value()));
    const auto pid = made.value();
    auto file = kernel.posix().open("/stat-copy", posix::o_CREAT | posix::o_RDWR, 0640);
    CHECK(file);
    std::byte payload = std::byte{'X'};
    CHECK(kernel.posix().write(file.value(), {&payload, 1}).value() == 1);
    constexpr uptr path = hw::user_begin + 64;
    constexpr uptr old_path = hw::user_begin + 128;
    constexpr uptr legacy_output = hw::user_begin + mock::memory.size() - sizeof(ok_stat);
    constexpr uptr v2_output = hw::user_begin + mock::memory.size() - sizeof(ok_stat_v2);
    std::memcpy(mock::memory.data() + 64, "/stat-copy", sizeof("/stat-copy"));
    std::memcpy(mock::memory.data() + 128, "/stat-copy", sizeof("/stat-copy"));
    const auto queried = kernel.posix().fstat(file.value());
    CHECK(queried && queried.value().attributes.has(fs::Attribute::inode));
    const u64 inode = queried.value().attributes.inode;
    CHECK(inode != 0);
    const auto expected_legacy = posix::native_file_status(queried.value());
    const auto expected_v2 = posix::native_file_status_v2(queried.value());
    auto invoke = [&](syscall::Number operation, std::array<uptr, 6> arguments) {
        hw::Syscall request{static_cast<uptr>(operation), arguments};
        bool complete = true;
        const auto result = native::dispatch(kernel, pid, request, complete);
        CHECK(complete);
        return result;
    };
    auto descriptor_args = [&](uptr output) {
        return std::array<uptr, 6>{static_cast<uptr>(file.value()), output, 0, 0, 0, 0};
    };
    auto pathname_args = [&](uptr pathname, uptr output) {
        return std::array<uptr, 6>{static_cast<uptr>(posix::at_FDCWD), pathname, output, 0, 0, 0};
    };
    auto clear_tail = [&](usize size) {
        std::fill(mock::memory.end() - size - 1, mock::memory.end(), std::byte{0xa5});
    };
    auto unchanged_tail = [&](usize size) {
        for (usize i = mock::memory.size() - size; i < mock::memory.size(); ++i)
            CHECK(mock::memory[i] == std::byte{0xa5});
    };
    auto v2_inode = [&] {
        ok_stat_v2 value{};
        std::memcpy(&value, mock::memory.data() + mock::memory.size() - sizeof(value), sizeof(value));
        return value.inode;
    };
    for (auto operation : {syscall::Number::stat, syscall::Number::lstat,
                           syscall::Number::fstat, syscall::Number::newfstatat})
    {
        clear_tail(sizeof(ok_stat));
        const auto args = operation == syscall::Number::newfstatat ? pathname_args(path, legacy_output) :
            operation == syscall::Number::fstat ? descriptor_args(legacy_output) :
            std::array<uptr, 6>{path, legacy_output, 0, 0, 0, 0};
        CHECK(invoke(operation, args) == 0);
        CHECK(mock::memory[mock::memory.size() - sizeof(ok_stat) - 1] == std::byte{0xa5});
        CHECK(!std::memcmp(mock::memory.data() + mock::memory.size() - sizeof(ok_stat),
                           expected_legacy.data(), expected_legacy.size()));
        mock::forbidden = legacy_output + sizeof(ok_stat) - 1;
        clear_tail(sizeof(ok_stat));
        const usize outputs = mock::outputs;
        CHECK(invoke(operation, args) == -14 && mock::outputs == outputs);
        unchanged_tail(sizeof(ok_stat));
        mock::forbidden = 0;
    }
    for (auto operation : {syscall::Number::fstat_v2, syscall::Number::newfstatat_v2})
    {
        const auto args = operation == syscall::Number::fstat_v2 ?
            descriptor_args(v2_output) : pathname_args(path, v2_output);
        clear_tail(sizeof(ok_stat_v2));
        CHECK(invoke(operation, args) == 0);
        CHECK(mock::memory[mock::memory.size() - sizeof(ok_stat_v2) - 1] == std::byte{0xa5});
        CHECK(!std::memcmp(mock::memory.data() + mock::memory.size() - sizeof(ok_stat_v2),
                           expected_v2.data(), expected_v2.size()));
        CHECK(v2_inode() == inode);
        mock::forbidden = v2_output + sizeof(ok_stat_v2) - 1;
        clear_tail(sizeof(ok_stat_v2));
        const usize outputs = mock::outputs;
        CHECK(invoke(operation, args) == -14 && mock::outputs == outputs);
        unchanged_tail(sizeof(ok_stat_v2));
        mock::forbidden = 0;
        clear_tail(sizeof(ok_stat_v2));
        CHECK(invoke(operation, operation == syscall::Number::fstat_v2 ?
            descriptor_args(legacy_output) : pathname_args(path, legacy_output)) == -14);
        unchanged_tail(sizeof(ok_stat_v2));
    }
    CHECK(kernel.posix().rename("/stat-copy", "/stat-renamed"));
    std::memcpy(mock::memory.data() + 64, "/stat-renamed", sizeof("/stat-renamed"));
    clear_tail(sizeof(ok_stat_v2));
    CHECK(invoke(syscall::Number::fstat_v2, descriptor_args(v2_output)) == 0);
    CHECK(v2_inode() == inode);
    clear_tail(sizeof(ok_stat_v2));
    CHECK(invoke(syscall::Number::newfstatat_v2, pathname_args(path, v2_output)) == 0);
    CHECK(v2_inode() == inode);
    clear_tail(sizeof(ok_stat_v2));
    CHECK(invoke(syscall::Number::newfstatat_v2, pathname_args(old_path, v2_output)) == -2);
    unchanged_tail(sizeof(ok_stat_v2));
    clear_tail(sizeof(ok_stat_v2));
    CHECK(invoke(syscall::Number::fstat_v2, {9999, v2_output, 0, 0, 0, 0}) < 0);
    unchanged_tail(sizeof(ok_stat_v2));
    clear_tail(sizeof(ok_stat_v2));
    CHECK(invoke(syscall::Number::newfstatat_v2, pathname_args(old_path, legacy_output)) == -14);
    unchanged_tail(sizeof(ok_stat_v2));
    clear_tail(sizeof(ok_stat_v2));
    CHECK(invoke(syscall::Number::fstat_v2, {9999, legacy_output, 0, 0, 0, 0}) == -14);
    unchanged_tail(sizeof(ok_stat_v2));
    for (auto descriptor : {posix::Fd{1}})
    {
        clear_tail(sizeof(ok_stat_v2));
        CHECK(invoke(syscall::Number::fstat_v2,
                     {static_cast<uptr>(descriptor), v2_output, 0, 0, 0, 0}) == 0);
        CHECK(v2_inode() == 0);
    }
    auto pipe = kernel.posix().pipe();
    CHECK(pipe);
    for (auto descriptor : pipe.value())
    {
        clear_tail(sizeof(ok_stat_v2));
        CHECK(invoke(syscall::Number::fstat_v2,
                     {static_cast<uptr>(descriptor), v2_output, 0, 0, 0, 0}) == 0);
        CHECK(v2_inode() == 0);
        CHECK(kernel.posix().close(descriptor));
    }
    CHECK(kernel.posix().close(file.value()));
    std::puts("PASS native stat runtime model: legacy/v2 exact output bounds, unknown and retained inode identity");
}
