// Actual Linux adapter/common native copy boundary with hosted mappings only.
#include "native_runtime_units.hpp"
#include "unavailable_realtime.hpp"
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

template <lu::Profile P>
void mask(Kernel &kernel, sched::ProcessId parent, usize number_slot, usize argument_slot)
{
    auto pid = parent;
    auto raw = [&](u64 value) {
        auto number = lu::Codec<P>::syscall_number(lu::Operation::umask); CHECK(number);
        hw::UserFrame frame{};
        frame.registers.fill(~uptr{0});
        frame.registers[number_slot] = *number;
        frame.registers[argument_slot] = value;
        const auto reads = mock::copies, writes = mock::outputs;
        bool done = true;
        const auto result = user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
            [&](hw::Syscall request, bool &complete) { return native::dispatch(kernel,pid,request,complete); });
        CHECK(done && result >= 0 && result <= 0777 && user::linux_bootstrap::complete<P>(frame,result));
        auto returned = lu::NativeBridge<P>::decode_return(frame);
        CHECK(returned && returned.value().error == 0 && returned.value().value == static_cast<u64>(result));
        CHECK(reads == mock::copies && writes == mock::outputs);
        return result;
    };
    CHECK(kernel.posix().umask(0022) == 0022);
    CHECK(raw(0) == 0022 && raw(01077) == 0 && raw(~u64{0}) == 0077);
    CHECK(raw((~u64{0} << 32) | 0022) == 0777 && raw(0077) == 0022);
    char directory[48], file[64];
    std::snprintf(directory,sizeof directory,"/mask-%s",lu::Codec<P>::profile.name);
    std::snprintf(file,sizeof file,"%s/data",directory);
    CHECK(kernel.posix().mkdir(directory,0777));
    CHECK((kernel.posix().stat(directory).value().mode & 0777) == 0700);
    auto opened = kernel.posix().open(file,posix::o_CREAT|posix::o_RDWR,0666); CHECK(opened);
    CHECK(kernel.posix().close(opened.value()));
    CHECK((kernel.posix().stat(file).value().mode & 0777) == 0600);
    CHECK(raw(0) == 0077);
    opened = kernel.posix().open(file,posix::o_CREAT|posix::o_TRUNC|posix::o_RDWR,0666); CHECK(opened);
    CHECK(kernel.posix().close(opened.value()));
    CHECK((kernel.posix().stat(file).value().mode & 0777) == 0600);
    CHECK(raw(0027) == 0);
    auto child = kernel.posix().fork(parent); CHECK(child && kernel.posix().activate(child.value())); pid = child.value();
    CHECK(raw(0007) == 0027);
    CHECK(kernel.posix().exit(pid,17) && kernel.posix().activate(parent)); pid = parent;
    CHECK(kernel.posix().wait(parent,child.value()).value() == 17);
    CHECK(raw(0027) == 0027 && kernel.posix().umask(0022) == 0027);
    CHECK(kernel.posix().unlink(file) && kernel.posix().rmdir(directory));
    std::printf("PASS Linux umask %s: scalars, return values, common creation modes, existing inode, fork isolation, native sharing\n",lu::Codec<P>::profile.name);
}

int main()
{
    static Kernel kernel; CHECK(kernel.boot({}));
    auto made = kernel.scheduler().create_user_process("umask-adapter",kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096}));
    CHECK(made && kernel.posix().activate(made.value()));
    mask<lu::Profile::i386>(kernel,made.value(),7,4);
    mask<lu::Profile::x86_64>(kernel,made.value(),14,9);
    mask<lu::Profile::aarch64>(kernel,made.value(),8,0);
    mask<lu::Profile::arm32>(kernel,made.value(),7,0);
    mask<lu::Profile::rv64>(kernel,made.value(),17,10);
    mask<lu::Profile::rv32>(kernel,made.value(),17,10);
    mask<lu::Profile::loongarch64>(kernel,made.value(),11,4);
    mask<lu::Profile::mips>(kernel,made.value(),2,4);
    mask<lu::Profile::mips64>(kernel,made.value(),2,4);
    mask<lu::Profile::ppc>(kernel,made.value(),0,3);
}
