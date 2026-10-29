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
static usize checks{};
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
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


namespace
{
template <lu::Profile P>
void seeks(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize, 5> slots)
{
    constexpr uptr output = hw::user_begin + 128;
    constexpr uptr stack = hw::user_begin + 1024;
    auto fd_result = kernel.posix().open("/seek-model", posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC, 0600);
    CHECK(fd_result);const i32 fd = fd_result.value();
    auto duplicated=kernel.posix().duplicate(fd);CHECK(duplicated);const i32 duplicate=duplicated.value();
    const std::array<std::byte,6> payload{std::byte{'a'},std::byte{'b'},std::byte{'c'},std::byte{'d'},std::byte{'e'},std::byte{'f'}};
    CHECK(kernel.posix().write(fd,payload).value()==6);
    auto position=[&] { return kernel.posix().seek(duplicate,0,posix::SeekWhence::current).value(); };
    auto call = [&](lu::Operation operation, std::array<u64, 5> arguments, uptr sp=hw::user_begin+1024) {
        auto number=lu::Codec<P>::syscall_number(operation);CHECK(number);
        hw::UserFrame frame{};frame.sp=sp;frame.registers[number_slot]=*number;
        for(usize i=0;i<arguments.size();++i)
        {
            if constexpr(P==lu::Profile::mips)
                if(i==4)
                {
                    for(usize byte=0;byte<4;++byte)
                        mock::memory[1024+16+byte]=static_cast<std::byte>(arguments[4]>>(8*(3-byte)));
                    continue;
                }
            frame.registers[slots[i]]=static_cast<uptr>(arguments[i]);
        }
        bool done=true;
        i64 result=user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
            [&](hw::Syscall request,bool &complete){ return native::dispatch(kernel,pid,request,complete); });
        CHECK(done && user::linux_bootstrap::complete<P>(frame,result));
        auto returned=lu::NativeBridge<P>::decode_return(frame);CHECK(returned);
        const u32 expected=result==-75 ? *lu::Codec<P>::errno_number(lu::Error::e_overflow) : static_cast<u32>(-result);
        if(result<0) CHECK(returned.value().error==expected);
        else CHECK(!returned.value().error && returned.value().value==static_cast<u64>(result));
        return result;
    };
    auto read_output=[&] {
        u64 value=0;
        for(usize byte=0;byte<8;++byte)
        {
            const auto shift=lu::Codec<P>::profile.big_endian?7-byte:byte;
            value|=static_cast<u64>(mock::memory[128+byte])<<(8*shift);
        }
        CHECK(mock::memory[127]==std::byte{0x55} && mock::memory[136]==std::byte{0x55});
        return value;
    };
    auto seek=[&](i32 file,i64 offset,u32 origin) {
        if(lu::Codec<P>::syscall_number(lu::Operation::_llseek))
        {
            mock::memory.fill(std::byte{0x55});
            auto rc=call(lu::Operation::_llseek,{static_cast<u32>(file),static_cast<u64>(offset)>>32,static_cast<u32>(offset),output,origin});
            return rc<0?rc:static_cast<i64>(read_output());
        }
        return call(lu::Operation::lseek,{static_cast<u32>(file),static_cast<u64>(offset),origin,0,0});
    };
    CHECK(seek(fd,2,0)==2 && position()==2);
    std::byte byte{};CHECK(kernel.posix().read(duplicate,{&byte,1}).value()==1 && byte==std::byte{'c'});
    CHECK(seek(fd,-2,1)==1 && position()==1);
    CHECK(seek(fd,-1,2)==5 && position()==5);
    CHECK(seek(fd,9,0)==9);
    CHECK(kernel.posix().read(duplicate,{&byte,1}).value()==0);
    CHECK(seek(fd,-10,1)==-22 && position()==9);
    CHECK(seek(fd,0,3)==-22 && position()==9); // Extent seeking is outside this slice.
    CHECK(seek(fd,0,4)==-22 && seek(fd,0,5)==-22);
    CHECK(seek(-1,-1,99)==-9 && seek(-1,0,0)==-9);
    CHECK(seek(1,-1,99)==-22 && seek(1,0,0)==-29 && seek(0,0,4)==-29);
    auto pipe=kernel.posix().pipe();CHECK(pipe);
    CHECK(seek(pipe.value()[0],0,1)==-29 && seek(pipe.value()[1],0,99)==-22);
    CHECK(kernel.posix().close(pipe.value()[0]) && kernel.posix().close(pipe.value()[1]));
    CHECK(seek(fd,__INT64_MAX__,0)==__INT64_MAX__);
    CHECK(seek(fd,1,1)==-75 && position()==static_cast<u64>(__INT64_MAX__));
    CHECK(seek(fd,(-__INT64_MAX__-1),1)==-22 && position()==static_cast<u64>(__INT64_MAX__));
    CHECK(seek(fd,0,0)==0);
    if(lu::Codec<P>::syscall_number(lu::Operation::lseek))
    {
        CHECK(call(lu::Operation::lseek,{static_cast<u32>(fd),~u64{0},0,0,0})==-22);
        if constexpr(lu::Codec<P>::profile.word_bytes==4)
        {
            CHECK(call(lu::Operation::lseek,{static_cast<u32>(fd),(u64{1}<<32)|3,0,0,0})==3);
            CHECK(call(lu::Operation::lseek,{static_cast<u32>(fd),0x80000000u,0,0,0})==-22 && position()==3);
            CHECK(kernel.posix().seek(fd,0x7fffffff,posix::SeekWhence::set));
            CHECK(call(lu::Operation::lseek,{static_cast<u32>(fd),1,1,0,0})==-75 && position()==0x80000000u);
            // This hosted common service is LP64; real 32-bit capacity is tested in guests.
        }
        else
        {
            CHECK(call(lu::Operation::lseek,{(u64{1}<<32)|static_cast<u32>(fd),0x100000002u,(u64{1}<<32)|0,0,0})==0x100000002u);
            CHECK(position()==0x100000002u);
        }
    }
    if(lu::Codec<P>::syscall_number(lu::Operation::_llseek))
    {
        mock::memory.fill(std::byte{0x55});
        CHECK(call(lu::Operation::_llseek,{static_cast<u32>(fd),1,0x12345678u,output,0})==0);
        CHECK(read_output()==0x112345678u && position()==0x112345678u);
        auto outputs=mock::outputs;
        CHECK(call(lu::Operation::_llseek,{~u32{0},0,2,0,99})==-9 && mock::outputs==outputs);
        CHECK(call(lu::Operation::_llseek,{static_cast<u32>(fd),0,2,0,99})==-22 && mock::outputs==outputs);
        CHECK(call(lu::Operation::_llseek,{1,0,2,0,0})==-29 && mock::outputs==outputs);
        CHECK(call(lu::Operation::_llseek,{static_cast<u32>(fd),~u32{0},~u32{0},0,0})==-22 && mock::outputs==outputs);
        CHECK(call(lu::Operation::_llseek,{static_cast<u32>(fd),0,3,0,0})==-14 && position()==3);
        mock::forbidden=output+7;
        CHECK(call(lu::Operation::_llseek,{static_cast<u32>(fd),0,4,output,0})==-14 && position()==4);
        mock::forbidden=0;
        mock::partial_output=3;
        CHECK(call(lu::Operation::_llseek,{static_cast<u32>(fd),0,5,output,0})==-14 && position()==5);
        mock::partial_output=0;
        if constexpr(P==lu::Profile::mips)
        {
            mock::forbidden=stack+20; // Exactly the fifth word is readable; unused sixth word is not.
            CHECK(call(lu::Operation::_llseek,{static_cast<u32>(fd),0,0,output,2})==0 && position()==6);
            CHECK(mock::last_input==stack+16 && mock::last_input_size==4);
            mock::forbidden=stack+19;
            CHECK(call(lu::Operation::_llseek,{static_cast<u32>(fd),0,0,output,0})==-14 && position()==6);
            mock::forbidden=0;
            CHECK(call(lu::Operation::_llseek,{static_cast<u32>(fd),0,0,output,0},0xfffffff8u)==-14 && position()==6);
        }
    }
    auto shared_result = kernel.posix().memfd_create("English seek buffer", posix::mfd_ALLOW_SEALING);
    CHECK(shared_result);
    const i32 shared = shared_result.value();
    auto shared_duplicate = kernel.posix().duplicate(shared);
    CHECK(shared_duplicate);
    const auto *shared_slot = kernel.scheduler().find(pid)->fd_table().entry(shared);
    CHECK(shared_slot && shared_slot->used);
    const auto *shared_description = kernel.scheduler().open_description(shared_slot->description);
    CHECK(shared_description && !shared_description->node && shared_description->native_shared_file);
    auto shared_position = [&] {
        return kernel.posix().seek(shared_duplicate.value(), 0, posix::SeekWhence::current).value();
    };
    CHECK(seek(shared, 0, 2) == 0 && shared_position() == 0); // Empty shared files have a valid EOF.
    CHECK(kernel.posix().ftruncate(shared, 8192));
    CHECK(kernel.posix().fcntl(shared, posix::f_ADD_SEALS, 6));
    CHECK(kernel.posix().pwrite(shared, payload, 0).value() == payload.size());
    std::array<std::byte, 6> shared_output{};
    CHECK(kernel.posix().pread(shared_duplicate.value(), shared_output, 0).value() == payload.size() &&
          shared_output == payload);
    CHECK(seek(shared, 0, 1) == 0 && shared_position() == 0);
    CHECK(seek(shared, 64, 0) == 64);
    CHECK(kernel.posix().write(shared_duplicate.value(), payload).value() == payload.size());
    CHECK(seek(shared, 0, 1) == 70 && shared_position() == 70);
    CHECK(seek(shared, -1, 2) == 8191 && shared_position() == 8191);
    CHECK(kernel.posix().write(shared_duplicate.value(), {payload.data(), 1}).value() == 1);
    CHECK(seek(shared, 0, 2) == 8192 && shared_position() == 8192);
    CHECK(seek(shared, -8193, 1) == -22 && shared_position() == 8192);
    CHECK(seek(shared, 0, 3) == -22 && seek(shared, 0, 4) == -22 && shared_position() == 8192);
    CHECK(seek(shared, 0, 5) == -22 && shared_position() == 8192);
    CHECK(seek(shared, __INT64_MAX__, 0) == __INT64_MAX__);
    CHECK(seek(shared, 1, 1) == -75 && shared_position() == static_cast<u64>(__INT64_MAX__));
    CHECK(seek(shared, -__INT64_MAX__, 1) == 0 && shared_position() == 0);
    if (lu::Codec<P>::syscall_number(lu::Operation::_llseek))
    {
        const auto outputs = mock::outputs;
        CHECK(call(lu::Operation::_llseek, {static_cast<u32>(shared), 0, 8, 0, 0}) == -14 &&
              shared_position() == 8 && mock::outputs == outputs + 1);
        mock::partial_output = 3;
        CHECK(call(lu::Operation::_llseek, {static_cast<u32>(shared), 0, 9, output, 0}) == -14 &&
              shared_position() == 9);
        mock::partial_output = 0;
        CHECK(seek(shared_duplicate.value(), 0, 1) == 9);
    }
    CHECK(kernel.posix().close(shared) && kernel.posix().close(shared_duplicate.value()));
    CHECK(seek(shared, 0, 0) == -9 && seek(shared, 0, 99) == -9);
    const auto ledger = native::shared_files.ledger();
    CHECK(!ledger.files && !ledger.aliases && !ledger.retiring_files && !ledger.retained_frames &&
          !ledger.reserved_cells && ledger.first_cleanup_failure.ok());
    CHECK(native::page_pool.user_count() == 0);
    CHECK(kernel.posix().close(fd) && kernel.posix().close(duplicate));
    std::printf("PASS Linux seek %s: scalar widths, offset sharing, errno ordering, endian output, copy-fault position and MIPS stack\n",lu::Codec<P>::profile.name);
}
}
int main()
{
    static Kernel kernel;CHECK(kernel.boot({}));
    const auto context=kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096});
    CHECK(native::activate_pool() && native::shared_files.bind(native::page_pool));
    CHECK(kernel.scheduler().set_shared_memory_files(native::shared_files));
    const auto made=kernel.scheduler().create_user_process("seek-adapter",context);
    CHECK(made && kernel.posix().activate(made.value()));const auto pid=made.value();
    seeks<lu::Profile::i386>(kernel,pid,7,{4,6,5,1,0});
    seeks<lu::Profile::x86_64>(kernel,pid,14,{9,10,11,5,7});
    seeks<lu::Profile::aarch64>(kernel,pid,8,{0,1,2,3,4});
    seeks<lu::Profile::arm32>(kernel,pid,7,{0,1,2,3,4});
    seeks<lu::Profile::rv64>(kernel,pid,17,{10,11,12,13,14});
    seeks<lu::Profile::rv32>(kernel,pid,17,{10,11,12,13,14});
    seeks<lu::Profile::loongarch64>(kernel,pid,11,{4,5,6,7,8});
    seeks<lu::Profile::mips>(kernel,pid,2,{4,5,6,7,0});
    seeks<lu::Profile::mips64>(kernel,pid,2,{4,5,6,7,8});
    seeks<lu::Profile::ppc>(kernel,pid,0,{3,4,5,6,7});
    std::printf("PASS Linux seek adapter: %zu checks, ten profiles, shared-file SET/CUR/END and copy-fault positions\n", checks);
}
