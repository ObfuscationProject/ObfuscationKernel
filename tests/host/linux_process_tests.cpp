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
void profile(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize, 4> slots)
{
    auto call = [&](lu::Operation operation, std::array<uptr, 4> args, i64 forced = 0, bool forward = false) {
        auto number = lu::Codec<P>::syscall_number(operation);
        CHECK(number);
        hw::UserFrame frame{};
        frame.registers[number_slot] = *number;
        for (usize i = 0; i < args.size(); ++i)
            frame.registers[slots[i]] = args[i];
        bool complete = true;
        usize callbacks = 0;
        hw::Syscall received{};
        auto result = user::linux_bootstrap::dispatch<P>(kernel, pid, frame, complete,
            [&](hw::Syscall request, bool &done) {
                ++callbacks;
                received = request;
                return forward ? native::dispatch(kernel, pid, request, done) : forced;
            });
        CHECK(complete);
        CHECK(user::linux_bootstrap::complete<P>(frame, result));
        auto returned = lu::NativeBridge<P>::decode_return(frame);
        CHECK(returned);
        if (result >= 0)
            CHECK(returned.value().error == 0 && returned.value().value == static_cast<u64>(result));
        else
            CHECK(returned.value().error == static_cast<u32>(
                (P == lu::Profile::mips || P == lu::Profile::mips64) && result == -75 ? 79 :
                (P == lu::Profile::mips || P == lu::Profile::mips64) && result == -38 ? 89 : -result));
        return std::tuple{i64(result), callbacks, received};
    };
    CHECK(std::get<0>(call(lu::Operation::getpid, {}, 0, true)) == static_cast<i64>(pid));
    CHECK(std::get<0>(call(lu::Operation::getppid, {}, 0, true)) == 0);
    CHECK(std::get<0>(call(lu::Operation::gettid, {}, 0, true)) == static_cast<i64>(pid));
    CHECK(std::get<0>(call(lu::Operation::set_tid_address, {hw::user_begin, 0, 0, 0}, 0, true)) == static_cast<i64>(pid));
    CHECK(kernel.scheduler().find(pid)->threads()[0].tls.clear_tid_address == hw::user_begin);
    CHECK(kernel.posix().gettid() == pid); // One task-ID domain; a leader TID equals TGID.
    bool native_done = true;
    CHECK(native::dispatch(kernel, pid, {OK_SYS_GETTID, {}}, native_done) == static_cast<i64>(kernel.posix().gettid()));
    const auto leader = kernel.scheduler().find(pid)->threads()[0].tid;
    auto peer = kernel.scheduler().create_thread(pid, kernel.scheduler().find(pid)->threads()[0].context);
    CHECK(peer && peer.value() != pid && kernel.scheduler().activate_thread(pid, peer.value()));
    const auto copied = mock::copies;
    CHECK(std::get<0>(call(lu::Operation::getpid, {}, 0, true)) == static_cast<i64>(pid));
    CHECK(std::get<0>(call(lu::Operation::gettid, {}, 0, true)) == static_cast<i64>(peer.value()));
    CHECK(std::get<0>(call(lu::Operation::set_tid_address, {1, 0, 0, 0}, 0, true)) == static_cast<i64>(peer.value()));
    CHECK(mock::copies == copied);
    CHECK(kernel.scheduler().find_thread(pid, peer.value())->tls.clear_tid_address == 1);
    CHECK(kernel.scheduler().find_thread(pid, leader)->tls.clear_tid_address == hw::user_begin);
    CHECK(kernel.scheduler().exit_thread(pid, peer.value()).value() == 1);
    CHECK(kernel.scheduler().activate_thread(pid, leader));
    const auto identity = [&](lu::Operation legacy, lu::Operation wide, uptr expected) {
        const auto selected = lu::Codec<P>::syscall_number(wide) ? wide : legacy;
        CHECK(std::get<0>(call(selected, {}, 0, true)) == static_cast<i64>(expected));
        if (lu::Codec<P>::syscall_number(wide))
            CHECK(std::get<0>(call(legacy, {}, 0, true)) == static_cast<i64>(expected > 65535 ? 65534 : expected));
    };
    identity(lu::Operation::getuid, lu::Operation::getuid32, 0x12345678);
    identity(lu::Operation::geteuid, lu::Operation::geteuid32, 0x12345678);
    identity(lu::Operation::getgid, lu::Operation::getgid32, 0x76543210);
    identity(lu::Operation::getegid, lu::Operation::getegid32, 0x76543210);
    if (lu::Codec<P>::syscall_number(lu::Operation::fork))
    {
        auto [result, callbacks, request] = call(lu::Operation::fork, {}, -11);
        CHECK(result == -11 && callbacks == 1 && request.number == OK_SYS_FORK);
        CHECK(std::get<0>(call(lu::Operation::fork, {}, -75)) == -75);
    }
    constexpr uptr sigchld = P == lu::Profile::mips || P == lu::Profile::mips64 ? 18 : 17;
    auto [result, callbacks, request] = call(lu::Operation::clone, {sigchld, 0, 0, 0}, -11);
    CHECK(result == -11 && callbacks == 1 && request.number == OK_SYS_FORK);
    auto rejected = call(lu::Operation::clone, {sigchld | 0x100, 0, 0, 0});
    CHECK(std::get<0>(rejected) == -38 && std::get<1>(rejected) == 0);
    rejected = call(lu::Operation::clone, {sigchld, hw::user_begin, 0, 0});
    CHECK(std::get<0>(rejected) == -22 && std::get<1>(rejected) == 0);
    auto exec = call(lu::Operation::execve, {hw::user_begin, hw::user_begin+8, hw::user_begin+16, 0}, -14);
    CHECK(std::get<0>(exec) == -14 && std::get<1>(exec) == 1 && std::get<2>(exec).number == OK_SYS_EXECVE);
    if (lu::Codec<P>::syscall_number(lu::Operation::wait4))
    {
        auto wait = call(lu::Operation::wait4, {static_cast<uptr>(-1), hw::user_begin, 1, 0}, -10);
        CHECK(std::get<0>(wait) == -10 && std::get<1>(wait) == 0);
        if constexpr (lu::Codec<P>::profile.word_bytes == 8)
        {
            const uptr high = uptr{1} << 32;
            auto child = kernel.posix().fork(pid);
            CHECK(child && kernel.posix().exit(child.value(), 19));
            wait = call(lu::Operation::wait4, {high | child.value(), hw::user_begin + 128, high, 0}, 0, true);
            CHECK(std::get<0>(wait) == static_cast<i64>(child.value()));
            CHECK(std::get<1>(wait) == 0);
            const auto offset = hw::user_begin + 128 - hw::user_begin;
            CHECK(mock::memory[offset + (lu::Codec<P>::profile.big_endian ? 2 : 1)] == std::byte{19});
            CHECK(!kernel.scheduler().find(child.value()));
            wait = call(lu::Operation::wait4, {high | 0xffffffffU, hw::user_begin, high | 1U, 0}, -10);
            CHECK(std::get<0>(wait) == -10 && std::get<1>(wait) == 0);
        }
    }
    std::printf("PASS hosted Linux process %s: identity/TID, selected clone/fork routing, EAGAIN vs EOVERFLOW, exec forwarding and typed wait completion\n", lu::Codec<P>::profile.name);
}

void waitid_rv32(Kernel &kernel, sched::ProcessId pid, sched::ProcessId adopted)
{
    constexpr uptr output = hw::user_begin + 256;
    usize callbacks = 0;
    auto call = [&](u32 kind, u32 id, uptr address, u32 options, uptr usage, bool &done) {
        hw::UserFrame frame{};
        frame.registers[17] = *lu::Codec<lu::Profile::rv32>::syscall_number(lu::Operation::waitid);
        frame.registers[10] = kind; frame.registers[11] = id; frame.registers[12] = address;
        frame.registers[13] = options; frame.registers[14] = usage;
        done = true; callbacks = 0;
        return user::linux_bootstrap::dispatch<lu::Profile::rv32>(kernel, pid, frame, done,
            [&](hw::Syscall request, bool &complete) { ++callbacks; return native::dispatch(kernel, pid, request, complete); });
    };
    auto field = [&](usize offset) {
        const auto *bytes = mock::memory.data() + output - hw::user_begin + offset;
        u32 value = 0;
        for (usize i=0;i<4;++i) value |= std::to_integer<u32>(bytes[i]) << (8*i);
        return value;
    };
    bool done = true;
    const auto *before_move = kernel.scheduler().find(pid);
    const auto adopted_uid = kernel.scheduler().find(adopted)->credentials().uid;
    CHECK(kernel.posix().exit(adopted, 27));
    CHECK(call(0, 0, output, 4, 0, done) == 0 && done);
    CHECK(kernel.scheduler().find(pid) != before_move);
    CHECK(field(12) == adopted && field(16) == adopted_uid && field(20) == 27);
    CHECK(!kernel.scheduler().find(adopted));
    auto child = kernel.posix().fork(pid); CHECK(child);
    auto *child_process = kernel.scheduler().find(child.value());
    const u32 uid = child_process->credentials().uid;
    mock::memory.fill(std::byte{0x55});
    CHECK(call(1, child.value(), output, 5, 0, done) == 0 && done && callbacks == 0);
    for (usize i=0;i<128;++i) CHECK(mock::memory[output-hw::user_begin+i] == std::byte{});
    CHECK(call(1, child.value(), hw::user_begin+mock::memory.size()-127, 4, 0, done) == -14 && callbacks == 0);
    CHECK(kernel.scheduler().find(child.value()));
    CHECK(call(2, child.value(), output, 4, 0, done) == -22 && callbacks == 0);
    CHECK(call(1, child.value(), output, 0x1000004, 0, done) == -22 && callbacks == 0);
    CHECK(call(1, child.value(), output, 4, output, done) == -22 && callbacks == 0);
    CHECK(call(1, child.value(), output, 4, 0, done) == -11 && !done && callbacks == 0);
    CHECK(kernel.posix().exit(child.value(), 42));
    CHECK(kernel.posix().activate(pid));
    CHECK(call(1, child.value(), output, 4, 0, done) == 0 && done);
    CHECK(field(0)==17 && field(4)==0 && field(8)==1 && field(12)==child.value() && field(16)==uid && field(20)==42);
    for (usize i=24;i<128;++i) CHECK(mock::memory[output-hw::user_begin+i] == std::byte{});
    CHECK(!kernel.scheduler().find(child.value()));
    CHECK(call(1, child.value(), output, 4, 0, done) == -10 && done);
    child = kernel.posix().fork(pid); CHECK(child);
    CHECK(kernel.scheduler().terminate_process(child.value(), 11));
    CHECK(call(0, 0, output, 4, 0, done) == 0 && done);
    CHECK(field(8)==2 && field(12)==child.value() && field(20)==11);
    std::puts("PASS hosted RV32 waitid: full siginfo, nohang zeroing, copy admission before reap, parent compaction, block/retry, exited/signalled status");
}

int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    const auto context = kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096});
    auto first = kernel.scheduler().create_user_process("advance-tid", context);
    CHECK(first && kernel.scheduler().create_thread(first.value(), context));
    auto earlier_child = kernel.posix().fork(first.value());
    CHECK(earlier_child);
    auto made = kernel.scheduler().create_user_process("process-adapter", context);
    CHECK(made && kernel.posix().activate(made.value()));
    CHECK(kernel.posix().set_identity(0x12345678, 0x76543210));
    auto pid = made.value();
    CHECK(kernel.scheduler().reparent_children(first.value(), pid));
    waitid_rv32(kernel, pid, earlier_child.value());
    profile<lu::Profile::i386>(kernel, pid, 7, {4,6,5,1});
    profile<lu::Profile::x86_64>(kernel, pid, 14, {9,10,11,5});
    profile<lu::Profile::aarch64>(kernel, pid, 8, {0,1,2,3});
    profile<lu::Profile::arm32>(kernel, pid, 7, {0,1,2,3});
    profile<lu::Profile::rv64>(kernel, pid, 17, {10,11,12,13});
    profile<lu::Profile::rv32>(kernel, pid, 17, {10,11,12,13});
    profile<lu::Profile::loongarch64>(kernel, pid, 11, {4,5,6,7});
    profile<lu::Profile::mips>(kernel, pid, 2, {4,5,6,7});
    profile<lu::Profile::mips64>(kernel, pid, 2, {4,5,6,7});
    profile<lu::Profile::ppc>(kernel, pid, 0, {3,4,5,6});
}
