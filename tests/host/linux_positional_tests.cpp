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
class FaultNode final : public fs::Node
{
  public:
    fs::Metadata info{};
    bool fail_metadata{}, fail_read{}, fail_write{};
    mutable usize reads{}, last_offset{}, last_count{};
    usize writes{};
    std::string_view name() const override { return "positional-fault"; }
    fs::Metadata metadata() const override { return info; }
    Result<fs::Metadata> query_metadata() const override
    { return fail_metadata ? Result<fs::Metadata>{Status::io_error()} : Result<fs::Metadata>{info}; }
    Result<fs::FileBuffer> read(usize offset,usize count) const override
    {
        ++reads;last_offset=offset;last_count=count;
        if(fail_read)return Status::io_error();
        fs::FileBuffer out{};out.size=count;return out;
    }
    Status write(usize offset,std::span<const std::byte> bytes) override
    {
        ++writes;last_offset=offset;last_count=bytes.size();
        return fail_write?Status::io_error():Status::success();
    }
    fs::Node *lookup(std::string_view) override { return nullptr; }
    Status create(std::string_view,fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32,u32) override { return Status::unsupported(); }
};
template <lu::Profile P>
void positional(Kernel &kernel, sched::ProcessId parent, usize number_slot, std::array<usize,6> slots)
{
    constexpr uptr output=hw::user_begin+128, stack=hw::user_begin+1024;
    sched::ProcessId pid=parent;
    auto opened=kernel.posix().open("/positioned-model",posix::o_CREAT|posix::o_TRUNC|posix::o_RDWR,0600);
    CHECK(opened);const auto fd=opened.value();
    auto dup=kernel.posix().duplicate(fd);CHECK(dup);
    const std::array<std::byte,6> initial{std::byte{'a'},std::byte{'b'},std::byte{'c'},std::byte{'d'},std::byte{'e'},std::byte{'f'}};
    CHECK(kernel.posix().write(fd,initial).value()==6 && kernel.posix().seek(fd,2,posix::SeekWhence::set));
    auto position=[&]{return kernel.posix().seek(dup.value(),0,posix::SeekWhence::current).value();};
    auto raw=[&](lu::Operation operation,u64 file,u64 address,u64 size,u64 offset,uptr sp=hw::user_begin+1024) {
        auto number=lu::Codec<P>::syscall_number(operation);CHECK(number);
        hw::UserFrame frame{};frame.sp=sp;frame.registers[number_slot]=*number;
        std::array<u64,6> args{file,address,size,0xbad0cafe,0,0};
        if constexpr(lu::Codec<P>::profile.word_bytes==8)args[3]=offset;
        else {
            constexpr usize first=lu::Codec<P>::profile.aligned_64bit_pairs?4:3;
            args[first]=lu::Codec<P>::profile.big_endian?offset>>32:static_cast<u32>(offset);
            args[first+1]=lu::Codec<P>::profile.big_endian?static_cast<u32>(offset):offset>>32;
        }
        for(usize i=0;i<6;++i) {
            if constexpr(P==lu::Profile::mips)if(i>=4) {
                for(usize byte=0;byte<4;++byte)mock::memory[1024+i*4+byte]=static_cast<std::byte>(args[i]>>(8*(3-byte)));
                continue;
            }
            frame.registers[slots[i]]=static_cast<uptr>(args[i]);
        }
        bool done=true;
        auto result=user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
            [&](hw::Syscall request,bool &complete){return native::dispatch(kernel,pid,request,complete);});
        CHECK(done && user::linux_bootstrap::complete<P>(frame,result));
        auto returned=lu::NativeBridge<P>::decode_return(frame);CHECK(returned);
        if(result>=0)CHECK(!returned.value().error && returned.value().value==static_cast<u64>(result));
        else {
            const auto expected=result==-75?*lu::Codec<P>::errno_number(lu::Error::e_overflow):static_cast<u32>(-result);
            CHECK(returned.value().error==expected);
        }
        return result;
    };
    auto read=[&](u64 file,u64 address,u64 size,u64 offset){return raw(lu::Operation::pread64,file,address,size,offset);};
    auto write=[&](u64 file,u64 address,u64 size,u64 offset){return raw(lu::Operation::pwrite64,file,address,size,offset);};
    CHECK(read(fd,output,3,1)==3 && !std::memcmp(mock::memory.data()+128,"bcd",3) && position()==2);
    CHECK(read(fd,output,8,4)==2 && !std::memcmp(mock::memory.data()+128,"ef",2) && position()==2);
    CHECK(read(fd,output,8,8)==0 && position()==2);
    CHECK(read(fd,0,0,999)==0 && write(fd,0,0,999)==0 && kernel.posix().fstat(fd).value().size==6);
    mock::memory[128]=std::byte{'X'};mock::memory[129]=std::byte{'Y'};
    CHECK(write(fd,output,2,1)==2 && position()==2);
    CHECK(read(fd,output,6,0)==6 && !std::memcmp(mock::memory.data()+128,"aXYdef",6));
    CHECK(read(~u32{0},0,1,~u64{0})==-22 && write(~u32{0},0,1,~u64{0})==-22);
    CHECK(read(~u32{0},0,1,0)==-9 && write(~u32{0},0,1,0)==-9);
    auto pipes=kernel.posix().pipe();CHECK(pipes);
    for(auto file:pipes.value()) {
        CHECK(read(file,0,1,0)==-29 && write(file,0,1,0)==-29);
        CHECK(kernel.posix().close(file));
    }
    CHECK(read(0,0,1,0)==-29 && write(1,0,1,0)==-29);
    auto ro=kernel.posix().open("/positioned-model",posix::o_RDONLY);auto wo=kernel.posix().open("/positioned-model",posix::o_WRONLY);
    CHECK(ro && wo && write(ro.value(),0,1,0)==-9 && read(wo.value(),0,1,0)==-9);
    CHECK(kernel.posix().close(ro.value()) && kernel.posix().close(wo.value()));
    auto directory=kernel.posix().open("/",posix::o_RDONLY|posix::o_DIRECTORY);
    CHECK(directory && read(directory.value(),output,1,0)==-21 && read(directory.value(),0,0,0)==-21);
    CHECK(kernel.posix().close(directory.value()));
    CHECK(read(fd,0,1,0)==-14 && write(fd,0,1,0)==-14 && position()==2);
    mock::forbidden=output+1;
    CHECK(read(fd,output,2,0)==-14 && write(fd,output,2,0)==-14 && position()==2);
    mock::forbidden=0;mock::partial_output=1;
    CHECK(read(fd,output,2,0)==-14 && position()==2);mock::partial_output=0;
    CHECK(read(fd,output,6,0)==6 && !std::memcmp(mock::memory.data()+128,"aXYdef",6));
    constexpr u64 limit=lu::Codec<P>::word_mask>>1;
    CHECK(read(fd,output,1,limit)==-75 && write(fd,output,1,limit)==-75 && position()==2);
    CHECK(read(fd,output,0,limit)==0);
    CHECK(read(fd,output,limit+1,0)==-22);
    if constexpr(lu::Codec<P>::profile.word_bytes==4) {
        CHECK(read(fd,output,0,0x100000001ULL)==-75 && write(fd,output,0,0x100000001ULL)==-75);
        CHECK(read(fd,output,0,0x0000000100000000ULL)==-75);
    } else CHECK(read((u64{1}<<40)|static_cast<u32>(fd),output,1,0)==1 && mock::memory[128]==std::byte{'a'});
    if constexpr(P==lu::Profile::mips) {
        mock::forbidden=stack+23;
        CHECK(read(fd,output,1,0)==-14 && position()==2);
        mock::forbidden=0;
        CHECK(raw(lu::Operation::pread64,fd,output,1,0,0xfffffff8)==-14);
        CHECK(read(fd,output,0,0)==0 && mock::last_input==stack+16 && mock::last_input_size==8);
    }
    CHECK(kernel.posix().fcntl(fd,posix::f_SETFL,posix::o_APPEND));
    mock::memory[128]=std::byte{'!'};
    CHECK(write(fd,output,1,0)==1 && position()==2);
    CHECK(read(fd,output,8,0)==7 && !std::memcmp(mock::memory.data()+128,"aXYdef!",7));
    std::byte native_byte=std::byte{'N'};
    CHECK(kernel.posix().pwrite(fd,{&native_byte,1},0).value()==1 && position()==2);
    CHECK(read(fd,output,8,0)==7 && !std::memcmp(mock::memory.data()+128,"NXYdef!",7));
    CHECK(kernel.posix().fcntl(fd,posix::f_SETFL,0));
    // Swap only in this serialized model, restoring the retained node before teardown.
    auto *description=kernel.scheduler().open_description(kernel.scheduler().find(pid)->fd_table().entry(fd)->description);
    auto *original=description->node;FaultNode fault;description->node=&fault;
    fault.fail_metadata=true;
    CHECK(read(fd,output,1,0)==-5 && position()==2 && fault.reads==0);
    fault.fail_metadata=false;fault.fail_read=true;
    CHECK(read(fd,output,1,0)==-5 && position()==2 && fault.reads==1);
    fault.fail_write=true;
    CHECK(write(fd,output,1,0)==-5 && position()==2 && fault.writes==1);
    fault.fail_write=false;fault.fail_read=false;
    CHECK(write(fd,0,0,999)==0 && fault.writes==1);
    CHECK(read(fd,hw::user_begin,8192,0)==4096 && fault.last_count==4096 && position()==2);
    CHECK(write(fd,hw::user_begin,8192,0)==4096 && fault.last_count==4096 && position()==2);
    if constexpr(lu::Codec<P>::profile.word_bytes==8) {
        CHECK(read(fd,output,1,0x123456789abcdef0ULL)==1 && fault.last_offset==0x123456789abcdef0ULL);
    }
    CHECK(kernel.posix().fcntl(fd,posix::f_SETFL,posix::o_APPEND));
    fault.fail_metadata=true;auto writes=fault.writes;
    CHECK(write(fd,output,1,0)==-5 && fault.writes==writes && position()==2);
    fault.fail_metadata=false;fault.info.size=static_cast<usize>(limit);
    CHECK(write(fd,output,1,0)==-75 && fault.writes==writes && position()==2);
    fault.info.size=17;
    CHECK(write(fd,output,1,3)==1 && fault.last_offset==17 && position()==2);
    description->node=original;CHECK(kernel.posix().fcntl(fd,posix::f_SETFL,0));
    CHECK(kernel.posix().unlink("/positioned-model"));
    auto child=kernel.posix().fork(parent);CHECK(child);
    CHECK(kernel.posix().activate(child.value()));pid=child.value();
    mock::memory[128]=std::byte{'C'};CHECK(write(fd,output,1,0)==1 && position()==2);
    CHECK(kernel.posix().exit(child.value(),17));CHECK(kernel.posix().activate(parent));pid=parent;
    CHECK(kernel.posix().wait(parent,child.value()).value()==17);
    CHECK(read(dup.value(),output,8,0)==7 && !std::memcmp(mock::memory.data()+128,"CXYdef!",7));
    CHECK(position()==2);
    auto truncate=[&](u64 file,u64 length) {
        constexpr auto operation=lu::Codec<P>::profile.word_bytes==4?
            lu::Operation::ftruncate64:lu::Operation::ftruncate;
        auto number=lu::Codec<P>::syscall_number(operation);CHECK(number);
        hw::UserFrame frame{};frame.sp=stack;frame.registers[number_slot]=*number;
        std::array<u64,6> args{};args[0]=file;
        if constexpr(lu::Codec<P>::profile.word_bytes==8)args[1]=length;
        else {
            constexpr usize first=lu::Codec<P>::profile.aligned_64bit_pairs?2:1;
            args[first]=lu::Codec<P>::profile.big_endian?length>>32:static_cast<u32>(length);
            args[first+1]=lu::Codec<P>::profile.big_endian?static_cast<u32>(length):length>>32;
        }
        for(usize i=0;i<6;++i) {
            if constexpr(P==lu::Profile::mips)if(i>=4)continue;
            frame.registers[slots[i]]=static_cast<uptr>(args[i]);
        }
        bool done=true;
        auto result=user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
            [&](hw::Syscall request,bool &complete){return native::dispatch(kernel,pid,request,complete);});
        CHECK(done && user::linux_bootstrap::complete<P>(frame,result));
        auto returned=lu::NativeBridge<P>::decode_return(frame);CHECK(returned);
        CHECK(returned.value().error==static_cast<u32>(result<0?-result:0));
        return result;
    };
    CHECK(truncate(~u32{0},5)==-9 && truncate(fd,~u64{0})==-22);
    CHECK(truncate(fd,5)==0 && kernel.posix().fstat(fd).value().size==5 && position()==2);
    CHECK(read(dup.value(),output,8,0)==5 && !std::memcmp(mock::memory.data()+128,"CXYde",5));
    CHECK(kernel.posix().close(fd) && kernel.posix().close(dup.value()));
    std::printf("PASS Linux positional %s: offset pairs, ftruncate pairs, copies, append, zero/EOF, retained descriptions and errors\n",lu::Codec<P>::profile.name);
}
}
int main()
{
    static Kernel kernel;CHECK(kernel.boot({}));
    auto made=kernel.scheduler().create_user_process("positional-adapter",kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096}));
    CHECK(made && kernel.posix().activate(made.value()));const auto pid=made.value();
    positional<lu::Profile::i386>(kernel,pid,7,{4,6,5,1,0,2});
    positional<lu::Profile::x86_64>(kernel,pid,14,{9,10,11,5,7,6});
    positional<lu::Profile::aarch64>(kernel,pid,8,{0,1,2,3,4,5});
    positional<lu::Profile::arm32>(kernel,pid,7,{0,1,2,3,4,5});
    positional<lu::Profile::rv64>(kernel,pid,17,{10,11,12,13,14,15});
    positional<lu::Profile::rv32>(kernel,pid,17,{10,11,12,13,14,15});
    positional<lu::Profile::loongarch64>(kernel,pid,11,{4,5,6,7,8,9});
    positional<lu::Profile::mips>(kernel,pid,2,{4,5,6,7,0,0});
    positional<lu::Profile::mips64>(kernel,pid,2,{4,5,6,7,8,9});
    positional<lu::Profile::ppc>(kernel,pid,0,{3,4,5,6,7,8});
}
