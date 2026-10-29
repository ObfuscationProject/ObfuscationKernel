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
usize outputs{};
bool fail_output{};
usize fail_output_at{};
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
    ++mock::outputs;
    if (mock::fail_output || (mock::fail_output_at && mock::outputs == mock::fail_output_at) || !user_range(s, address, size, true))
        return Status::fault();
    if (size)
        std::memcpy(mock::memory.data() + address - user_begin, in, size);
    return Status::success();
}
} // namespace ok::arch::execution


namespace
{
usize descriptor_count(const sched::ProcessResources &resources)
{
    usize count = 0;
    for (i32 fd = 0; fd < static_cast<i32>(posix::max_open_files); ++fd) count += resources.files.contains(fd);
    return count;
}
template <lu::Profile P>
void pipes(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize, 4> slots)
{
    using Flags = lu::OpenCodec<P>;
    constexpr uptr output = hw::user_begin + 128;
    auto &resources = kernel.scheduler().find(pid)->resources();
    const usize original_descriptions = kernel.scheduler().open_description_count();
    hw::UserFrame returned_frame{};
    auto call = [&](lu::Operation operation, std::array<uptr, 4> arguments) {
        auto number = lu::Codec<P>::syscall_number(operation);
        CHECK(number);
        hw::UserFrame frame{};
        frame.registers[number_slot] = *number;
        for (usize i = 0; i < arguments.size(); ++i) frame.registers[slots[i]] = arguments[i];
        bool done = true;
        const i64 result = user::linux_bootstrap::dispatch<P>(kernel, pid, frame, done,
            [&](hw::Syscall request, bool &complete) { return native::dispatch(kernel, pid, request, complete); });
        CHECK(done && user::linux_bootstrap::complete<P>(frame, result));
        const auto returned = lu::NativeBridge<P>::decode_return(frame);
        CHECK(returned);
        if (result >= 0) CHECK(!returned.value().error && returned.value().value == static_cast<u64>(result));
        else CHECK(returned.value().error == static_cast<u32>(-result));
        returned_frame = frame;
        return result;
    };
    auto clean = [&] {
        CHECK(descriptor_count(resources) == 3);
        CHECK(kernel.scheduler().open_description_count() == original_descriptions);
    };
    auto close = [&](i32 fd) { CHECK(call(lu::Operation::close, {static_cast<uptr>(fd),0,0,0}) == 0); };
    auto pair = [&] {
        std::array<i32,2> values{};
        for (usize fd=0;fd<2;++fd)
            for (usize i=0;i<4;++i)
            {
                const auto byte = static_cast<u8>(mock::memory[128+4*fd+i]);
                const auto shift = lu::Codec<P>::profile.big_endian ? 3-i : i;
                values[fd] |= static_cast<i32>(static_cast<u32>(byte) << (8*shift));
            }
        CHECK(mock::memory[127] == std::byte{0x55} && mock::memory[136] == std::byte{0x55});
        return values;
    };
    const auto fcntl = lu::Codec<P>::syscall_number(lu::Operation::fcntl64) ? lu::Operation::fcntl64 : lu::Operation::fcntl;
    const auto before_output = mock::outputs;
    CHECK(call(lu::Operation::pipe2,{0,Flags::append,0,0}) == -22);
    CHECK(call(lu::Operation::pipe2,{0,0,0,0}) == -14);
    CHECK(call(lu::Operation::pipe2,{hw::user_begin+mock::memory.size()-4,0,0,0}) == -14);
    mock::forbidden=output+7;
    CHECK(call(lu::Operation::pipe2,{output,0,0,0}) == -14);
    mock::forbidden=0;
    CHECK(mock::outputs==before_output+3);
    clean();
    mock::fail_output=true;
    for (usize i=0;i<80;++i)
    {
        CHECK(call(lu::Operation::pipe2,{output,0,0,0}) == -14);
        clean();
    }
    mock::fail_output=false;
    mock::memory.fill(std::byte{0x55});
    CHECK(call(lu::Operation::pipe2,{output,Flags::nonblock|Flags::cloexec,0,0}) == 0);
    auto fds=pair();CHECK(fds[0]==3 && fds[1]==4);
    CHECK(call(fcntl,{3,1,0,0})==1 && call(fcntl,{4,1,0,0})==1);
    CHECK(call(fcntl,{3,3,0,0})==Flags::nonblock && call(fcntl,{4,3,0,0})==(Flags::nonblock|1));
    constexpr uptr data=hw::user_begin+256;
    CHECK(call(lu::Operation::read,{3,data,1,0})==-11);
    mock::memory[256]=std::byte{'p'};
    CHECK(call(lu::Operation::write,{4,data,1,0})==1);
    mock::memory[256]=std::byte{};
    CHECK(call(lu::Operation::read,{3,data,1,0})==1 && mock::memory[256]==std::byte{'p'});
    CHECK(call(lu::Operation::dup,{4,0,0,0})==5);
    CHECK(call(fcntl,{5,1,0,0})==0);
    close(4);
    CHECK(call(lu::Operation::read,{3,data,1,0})==-11);
    close(5);
    CHECK(call(lu::Operation::read,{3,data,1,0})==0);
    close(3);clean();
    CHECK(call(lu::Operation::pipe2,{output,Flags::nonblock,0,0})==0);
    close(3);
    CHECK(call(lu::Operation::write,{4,data,1,0})==-32); // errno only; no SIGPIPE delivery claim.
    CHECK(call(lu::Operation::write,{4,data,0,0})==0);
    close(4);clean();
    if (lu::Codec<P>::syscall_number(lu::Operation::pipe))
    {
        mock::memory.fill(std::byte{0x55});
        const auto copies=mock::copies, outputs=mock::outputs;
        constexpr bool registers=lu::PipeCodec<P>::register_pair;
        CHECK(call(lu::Operation::pipe,{registers?0:output,~uptr{0},~uptr{0},~uptr{0}})==(registers?3:0));
        if constexpr (registers)
        {
            CHECK(returned_frame.registers[3]==4 && returned_frame.registers[7]==0);
            CHECK(mock::outputs==outputs && mock::copies==copies && mock::memory[128]==std::byte{0x55});
        }
        else CHECK((pair()==std::array<i32,2>{3,4}));
        close(3);close(4);clean();
        if constexpr (!registers)
        {
            mock::fail_output=true;CHECK(call(lu::Operation::pipe,{output,0,0,0})==-14);
            mock::fail_output=false;clean();
        }
    }
    if constexpr (lu::Codec<P>::profile.word_bytes==8)
    {
        CHECK(call(lu::Operation::pipe2,{output,(uptr{1}<<32)|Flags::nonblock,0,0})==0);
        CHECK(call(fcntl,{3,3,0,0})==Flags::nonblock);
        close(3);close(4);clean();
    }
    std::array<i32,28> held{};
    for (auto &fd:held)
    {
        auto result=kernel.posix().duplicate(1);CHECK(result);fd=result.value();
    }
    const auto descriptions=kernel.scheduler().open_description_count();
    CHECK(descriptor_count(resources)==31);
    CHECK(call(lu::Operation::pipe2,{output,0,0,0})==-24);
    const auto failed_outputs = mock::outputs;
    CHECK(call(lu::Operation::pipe2,{0,0,0,0})==-24);
    CHECK(mock::outputs==failed_outputs);
    CHECK(descriptor_count(resources)==31 && kernel.scheduler().open_description_count()==descriptions);
    std::array<u16,sched::max_open_descriptions> global_descriptions{};
    usize held_descriptions=0;
    for (;;) {
        auto made=kernel.scheduler().allocate_description({});
        if (!made) { CHECK(made.status().code()==StatusCode::system_file_limit); break; }
        CHECK(held_descriptions<global_descriptions.size());
        global_descriptions[held_descriptions++]=made.value();
        CHECK(kernel.scheduler().retain_description(made.value()));
    }
    CHECK(call(lu::Operation::pipe2,{0,0,0,0})==-23);
    CHECK(call(lu::Operation::pipe2,{output,0,0,0})==-23);
    CHECK(call(lu::Operation::pipe2,{0,Flags::append,0,0})==-22);
    CHECK(mock::outputs==failed_outputs && descriptor_count(resources)==31);
    while (held_descriptions) CHECK(kernel.scheduler().release_description(global_descriptions[--held_descriptions]));
    static sched::ProcessResources holder;
    holder={};
    for (usize i=0;i<sched::max_process_pipes;++i)
        CHECK(kernel.scheduler().create_pipe(holder));
    CHECK(call(lu::Operation::pipe2,{output,0,0,0})==-23);
    CHECK(call(lu::Operation::pipe2,{0,0,0,0})==-23);
    CHECK(mock::outputs==failed_outputs && descriptor_count(resources)==31);
    for (i32 fd=0;fd<static_cast<i32>(sched::max_process_fds);++fd)
        if (holder.files.contains(fd)) CHECK(kernel.scheduler().close_descriptor(holder,fd));
    CHECK(kernel.scheduler().open_description_count()==descriptions);
    if constexpr (lu::PipeCodec<P>::register_pair)
    {
        CHECK(call(lu::Operation::pipe,{0,0,0,0})==-24);
        CHECK(returned_frame.registers[7]==1);
    }
    for (auto fd:held) close(fd);
    CHECK(call(lu::Operation::pipe2,{output,0,0,0})==0);
    close(3);close(4);clean();
    std::printf("PASS Linux pipes %s: target codec, two-register ABI, flags, EAGAIN/EOF/EPIPE, references, copy rollback and capacity\n",lu::Codec<P>::profile.name);
}
void native_fallback(Kernel &kernel,sched::ProcessId pid)
{
    const auto before=kernel.scheduler().open_description_count();
    bool complete=true;
    hw::Syscall request{static_cast<uptr>(syscall::Number::pipe2),{hw::user_begin+128,posix::o_NONBLOCK|posix::o_CLOEXEC,0,0,0,0}};
    mock::fail_output=true;
    for(usize i=0;i<80;++i)
    {
        CHECK(native::dispatch(kernel,pid,request,complete)==-14 && complete);
        CHECK(descriptor_count(kernel.scheduler().find(pid)->resources())==3);
        CHECK(kernel.scheduler().open_description_count()==before);
    }
    mock::fail_output=false;
    CHECK(native::dispatch(kernel,pid,request,complete)==0);
    std::array<i32,2> fds{};std::memcpy(fds.data(),mock::memory.data()+128,sizeof(fds));
    CHECK((fds==std::array<i32,2>{3,4}));
    CHECK(kernel.posix().fcntl(3,posix::f_GETFD,0).value()==1);
    CHECK(kernel.posix().close(3) && kernel.posix().close(4));
    CHECK(kernel.scheduler().open_description_count()==before);
}

template <lu::Profile P>
void socket_pairs(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize, 4> slots)
{
    using Sockets = lu::SocketPairCodec<P>;
    using Flags = lu::OpenCodec<P>;
    constexpr uptr output = hw::user_begin + 128;
    const auto before = kernel.scheduler().open_description_count();
    auto call = [&](u32 number, std::array<uptr, 4> arguments) {
        hw::UserFrame frame{};
        frame.registers[number_slot] = number;
        for (usize i = 0; i < arguments.size(); ++i) frame.registers[slots[i]] = arguments[i];
        bool complete = true;
        const auto result = user::linux_bootstrap::dispatch<P>(kernel, pid, frame, complete,
            [&](hw::Syscall request, bool &done) { return native::dispatch(kernel, pid, request, done); });
        CHECK(complete && user::linux_bootstrap::complete<P>(frame, result));
        return result;
    };
    hw::UserFrame route{};
    route.registers[number_slot] = Sockets::numbers.direct;
    CHECK(native::linux_udp_immediate_operation_for_frame<P>(route) == native::LinuxUdpImmediateOperation::none);
    if constexpr (Sockets::numbers.legacy_socketcall != 0)
    {
        route.registers[number_slot] = Sockets::numbers.legacy_socketcall;
        route.registers[slots[0]] = 8;
        const auto copies = mock::copies;
        CHECK(native::linux_udp_immediate_operation_for_frame<P>(route) == native::LinuxUdpImmediateOperation::none);
        CHECK(mock::copies == copies);
        route.registers[slots[0]] = 1;
        CHECK(native::linux_udp_immediate_operation_for_frame<P>(route) == native::LinuxUdpImmediateOperation::socketcall);
    }
    auto clean = [&] {
        CHECK(descriptor_count(kernel.scheduler().find(pid)->resources()) == 3);
        CHECK(kernel.scheduler().open_description_count() == before);
    };
    auto fds = [&] {
        std::array<i32, 2> result{};
        for (usize entry = 0; entry < 2; ++entry)
            for (usize byte = 0; byte < 4; ++byte)
            {
                const usize shift = lu::Codec<P>::profile.big_endian ? 3 - byte : byte;
                result[entry] |= static_cast<i32>(static_cast<u32>(mock::memory[128 + entry * 4 + byte]) << (8 * shift));
            }
        return result;
    };
    auto close = [&] { CHECK(kernel.posix().close(3) && kernel.posix().close(4)); clean(); };
    CHECK(call(Sockets::numbers.direct, {2, Sockets::stream, 0, output}) == -97);
    CHECK(call(Sockets::numbers.direct, {1, 0, 0, output}) == -22);
    CHECK(call(Sockets::numbers.direct, {1, Sockets::stream, 1, output}) == -93);
    CHECK(call(Sockets::numbers.direct, {1, Sockets::stream, 0, 0}) == -14);
    clean();
    mock::fail_output = true;
    for (usize i = 0; i < 20; ++i)
    {
        CHECK(call(Sockets::numbers.direct, {1, Sockets::stream, 0, output}) == -14);
        clean();
    }
    mock::fail_output = false;
    mock::memory.fill(std::byte{0x55});
    CHECK(call(Sockets::numbers.direct, {1, Sockets::stream | Flags::nonblock | Flags::cloexec, 0, output}) == 0);
    CHECK((fds() == std::array<i32, 2>{3, 4}));
    CHECK(mock::memory[127] == std::byte{0x55} && mock::memory[136] == std::byte{0x55});
    CHECK(kernel.posix().fcntl(3, posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
    std::array<std::byte, 2> read{};
    const std::array<std::byte, 2> payload{std::byte{'a'}, std::byte{'b'}};
    CHECK(kernel.posix().write(3, payload).value() == 2);
    CHECK(kernel.posix().read(4, read).value() == 2 && read[0] == std::byte{'a'});
    close();
    if constexpr (Sockets::numbers.legacy_socketcall != 0)
    {
        constexpr uptr input = hw::user_begin + 384;
        const std::array<u32, 4> arguments{1, Sockets::stream, 0, static_cast<u32>(output)};
        for (usize entry = 0; entry < arguments.size(); ++entry)
            for (usize byte = 0; byte < 4; ++byte)
            {
                const usize shift = lu::Codec<P>::profile.big_endian ? 3 - byte : byte;
                mock::memory[384 + entry * 4 + byte] = static_cast<std::byte>(arguments[entry] >> (8 * shift));
            }
        CHECK(call(Sockets::numbers.legacy_socketcall, {7, input, 0, 0}) == -38);
        CHECK(call(Sockets::numbers.legacy_socketcall, {8, 0, 0, 0}) == -14);
        CHECK(call(Sockets::numbers.legacy_socketcall, {8, input, 0, 0}) == 0);
        CHECK((fds() == std::array<i32, 2>{3, 4}));
        close();
    }
    std::printf("PASS Linux AF_UNIX socketpair %s: direct/legacy ABI, target endian, flags and rollback\n",
                lu::Codec<P>::profile.name);
}
#include "linux_unix_message_cases.hpp"
}
int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    const auto context=kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096});
    const auto made=kernel.scheduler().create_user_process("pipe-adapter",context);
    CHECK(made && kernel.posix().activate(made.value()));
    const auto pid=made.value();
    pipes<lu::Profile::i386>(kernel,pid,7,{4,6,5,1});
    pipes<lu::Profile::x86_64>(kernel,pid,14,{9,10,11,5});
    pipes<lu::Profile::aarch64>(kernel,pid,8,{0,1,2,3});
    pipes<lu::Profile::arm32>(kernel,pid,7,{0,1,2,3});
    pipes<lu::Profile::rv64>(kernel,pid,17,{10,11,12,13});
    pipes<lu::Profile::rv32>(kernel,pid,17,{10,11,12,13});
    pipes<lu::Profile::loongarch64>(kernel,pid,11,{4,5,6,7});
    pipes<lu::Profile::mips>(kernel,pid,2,{4,5,6,7});
    pipes<lu::Profile::mips64>(kernel,pid,2,{4,5,6,7});
    pipes<lu::Profile::ppc>(kernel,pid,0,{3,4,5,6});
    socket_pairs<lu::Profile::i386>(kernel,pid,7,{4,6,5,1});
    socket_pairs<lu::Profile::x86_64>(kernel,pid,14,{9,10,11,5});
    socket_pairs<lu::Profile::aarch64>(kernel,pid,8,{0,1,2,3});
    socket_pairs<lu::Profile::arm32>(kernel,pid,7,{0,1,2,3});
    socket_pairs<lu::Profile::rv64>(kernel,pid,17,{10,11,12,13});
    socket_pairs<lu::Profile::rv32>(kernel,pid,17,{10,11,12,13});
    socket_pairs<lu::Profile::loongarch64>(kernel,pid,11,{4,5,6,7});
    socket_pairs<lu::Profile::mips>(kernel,pid,2,{4,5,6,7});
    socket_pairs<lu::Profile::mips64>(kernel,pid,2,{4,5,6,7});
    socket_pairs<lu::Profile::ppc>(kernel,pid,0,{3,4,5,6});
    unix_messages<lu::Profile::i386>(kernel,pid,7,{4,6,5,1});
    unix_messages<lu::Profile::x86_64>(kernel,pid,14,{9,10,11,5});
    unix_messages<lu::Profile::aarch64>(kernel,pid,8,{0,1,2,3});
    unix_messages<lu::Profile::arm32>(kernel,pid,7,{0,1,2,3});
    unix_messages<lu::Profile::rv64>(kernel,pid,17,{10,11,12,13});
    unix_messages<lu::Profile::rv32>(kernel,pid,17,{10,11,12,13});
    unix_messages<lu::Profile::loongarch64>(kernel,pid,11,{4,5,6,7});
    unix_messages<lu::Profile::mips>(kernel,pid,2,{4,5,6,7});
    unix_messages<lu::Profile::mips64>(kernel,pid,2,{4,5,6,7});
    unix_messages<lu::Profile::ppc>(kernel,pid,0,{3,4,5,6});
    unix_messages<lu::configured_native_profile(), true>(kernel,pid,0,{0,0,0,0});
    native_fallback(kernel,pid);
}
