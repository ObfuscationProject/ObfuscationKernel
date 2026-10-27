// Actual Linux adapter/common native copy boundary with hosted mappings only.
#include "native_runtime_units.hpp"
#include "unavailable_realtime.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace ok;
namespace native = ok::user::native;
namespace hw = ok::arch::execution;
namespace lu = ok::abi::linux_user;
static unsigned checks;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks; if (!(x))                                                                                                      \
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
usize stack_word_copies{};
usize path_byte_copies{};
hw::UserFrame *active_frame{};
bool mutate_frame_on_stack_copy{};
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
    if (address == user_begin + 1040 && size == 4)
    {
        ++mock::stack_word_copies;
        if (mock::mutate_frame_on_stack_copy && mock::active_frame)
        {
            // A checked copy may observe a live trap frame changing. Dispatch
            // must complete from the saved snapshot and captured fifth word.
            mock::active_frame->registers[4] = 30;
            mock::active_frame->registers[5] = 0;
            mock::active_frame->registers[6] = 0x8000;
            mock::active_frame->registers[7] = 0x80000000;
            mock::active_frame->sp = 0xfffffff0;
            mock::mutate_frame_on_stack_copy = false;
        }
    }
    if (address >= user_begin + 128 && address < user_begin + 384 && size == 1)
        ++mock::path_byte_copies;
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

// Deliberately injectable metadata provider. Ext4 allocation/timestamp semantics
// are exercised separately; this test reaches the actual syscall dispatcher.
static int filesystem_token;
class StatProvider final : public fs::Node
{
  public:
    fs::Metadata info{};
    StatProvider *child{};
    mutable usize queries{};
    Status failure{};
    explicit StatProvider(fs::NodeType kind)
    {
        info.type=kind; info.mode=fs::mode_for(kind,0755); info.uid=37; info.gid=73;
        info.size=33; info.block_size=1024; info.blocks=2; info.link_count=1;
        auto &a=info.attributes; a.inode=27; a.device={0,9};
        for(auto bit : {fs::Attribute::inode,fs::Attribute::device,fs::Attribute::access_time,
            fs::Attribute::modification_time,fs::Attribute::change_time,fs::Attribute::blocks512}) a.mark(bit);
        a.access={-1,999999999,fs::TimestampSource::readonly_ext4};
        a.modification={2208988800,1234,fs::TimestampSource::readonly_ext4};
        a.change={0,0,fs::TimestampSource::readonly_ext4};
    }
    std::string_view name() const override { return "stat-provider"; }
    const void *filesystem_identity() const override { return &filesystem_token; }
    fs::Metadata metadata() const override { return info; }
    Result<fs::Metadata> query_metadata() const override
    { ++queries; return failure ? Result<fs::Metadata>{info} : Result<fs::Metadata>{failure}; }
    fs::Node *lookup(std::string_view name) override { return name=="file" ? child : name==".." ? this : nullptr; }
    Status create(std::string_view,fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32,u32) override { return Status::unsupported(); }
    Result<fs::FileBuffer> read(usize,usize) const override { return Status::unsupported(); }
    Status write(usize,std::span<const std::byte>) override { return Status::unsupported(); }
};
static StatProvider provider{fs::NodeType::directory}, file{fs::NodeType::regular};

template<lu::Profile P>
void profile(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize,6> slots)
{
    const unsigned before=checks;
    using C=lu::Codec<P>;
    using O=lu::Operation;
    constexpr uptr path=hw::user_begin+128, output=hw::user_begin+2048;
    auto opened=kernel.posix().open("/stat-provider/file",posix::o_RDONLY); CHECK(opened);
    const i32 fd=opened.value();
    auto raw=[&](O operation,std::array<u64,5> args,bool bad_stack=false) -> i64 {
        auto number=C::syscall_number(operation); CHECK(number);
        hw::UserFrame frame{}; frame.registers.fill(~uptr{0}); frame.sp=hw::user_begin+1024;
        frame.registers[number_slot]=*number;
        for(usize i=0;i<5;++i)
        {
            if constexpr(P==lu::Profile::mips)
            {
                if(i==4)
                {
                    const u32 word=static_cast<u32>(args[i]);
                    for(usize byte=0;byte<4;++byte) mock::memory[1040+byte]=std::byte(word>>((3-byte)*8));
                    continue;
                }
            }
            frame.registers[slots[i]]=static_cast<uptr>(args[i]);
        }
        if(bad_stack) frame.sp=0xfffffff0;
        bool complete=true;
        mock::active_frame=&frame;
        auto result=user::linux_bootstrap::dispatch<P>(kernel,pid,frame,complete,
            [&](hw::Syscall,bool &) -> i64 { CHECK(false); return -38; });
        mock::active_frame=nullptr;
        CHECK(complete && user::linux_bootstrap::complete<P>(frame,result));
        auto value=lu::NativeBridge<P>::decode_return(frame); CHECK(value);
        u32 expected=result<0 ? static_cast<u32>(-result) : 0;
        if constexpr(P==lu::Profile::mips || P==lu::Profile::mips64)
        {
            if(expected==95) expected=122;
            else if(expected==38) expected=89;
            else if(expected==75) expected=79;
            else if(expected==36) expected=78;
            else if(expected==40) expected=90;
        }
        CHECK(value.value().error==expected && (expected || value.value().value==0));
        return result;
    };
    auto name=[&](const char *value) { std::strcpy(reinterpret_cast<char *>(mock::memory.data()+128),value); };
    const std::array<u64,5> basic{0xffffff9cu,path,0,0x7ff,output};
    auto statx=[&](std::array<u64,5> args) { return raw(O::statx,args); };
    auto reset=[&] { mock::memory.fill(std::byte{0xa5}); name("/stat-provider/file"); mock::forbidden=0;
                     mock::stack_word_copies=0; mock::path_byte_copies=0;
                     mock::mutate_frame_on_stack_copy=false; };
    auto unchanged=[&] { for(usize i=2040;i<2320;++i) CHECK(mock::memory[i]==std::byte{0xa5}); };
    auto get=[&](usize offset,usize width) {
        u64 value=0; for(usize i=0;i<width;++i)
            value|=u64{std::to_integer<u8>(mock::memory[2048+offset+i])} << ((C::profile.big_endian ? width-1-i : i)*8);
        return value;
    };
    reset();
    const auto copies=mock::outputs;
    CHECK(statx(basic)==0 && mock::outputs==copies+1);
    CHECK(mock::path_byte_copies==std::strlen("/stat-provider/file")+1);
    CHECK(mock::stack_word_copies==(P==lu::Profile::mips ? 1u : 0u));
    CHECK(get(0,4)==0x7ff && get(4,4)==1024 && get(20,4)==37 && get(24,4)==73);
    CHECK(get(28,2)==0100755 && get(32,8)==27 && get(40,8)==33 && get(48,8)==2);
    CHECK(get(64,8)==~u64{0} && get(72,4)==999999999 && get(112,8)==2208988800ull && get(120,4)==1234);
    for(usize i=144;i<256;++i) CHECK(get(i,1)==0);
    for(usize i : {usize{2047},usize{2304}}) CHECK(mock::memory[i]==std::byte{0xa5});
    for(u32 flags : {0u,0x100u,0x800u,0x2000u,0x4000u})
    { reset(); auto a=basic; a[2]=0xffffffff00000000ull|flags; a[3]=0xffffffff40000000ull; CHECK(statx(a)==0); }
    for(u32 flags : {1u,0x6000u,0x8000u})
    { reset(); auto a=basic; a[1]=a[4]=0; a[2]=flags; const auto q=file.queries; CHECK(statx(a)==-22 && q==file.queries); unchanged(); }
    { reset(); auto a=basic; a[1]=a[4]=0; a[3]=0x80000000; CHECK(statx(a)==-22); unchanged(); }
    { reset(); auto a=basic; a[1]=0; CHECK(statx(a)==-14); unchanged(); }
    { reset(); name(""); CHECK(statx(basic)==-2); unchanged(); }
    for(u64 null_path : {u64{0},u64{path}})
    {
        reset(); name(""); auto a=basic; a[0]=0xfeed00000000ull|static_cast<u32>(fd); a[1]=null_path; a[2]=0x1100;
        const auto q=file.queries; CHECK(statx(a)==0 && file.queries==q+1);
        CHECK(mock::path_byte_copies==(null_path ? 1u : 0u));
        for(u64 bad : {0xfeedffffffffull,0xfeed0000001eull})
        { reset(); name(""); a[0]=bad; CHECK(statx(a)==-9); unchanged(); }
    }
    CHECK(kernel.posix().chdir("/stat-provider"));
    { reset(); auto a=basic; a[1]=0; a[2]=0x1000; CHECK(statx(a)==0 && get(28,2)==0040755); }
    CHECK(kernel.posix().chdir("/"));
    { reset(); auto a=basic; a[0]=0xdeadffffffffull; CHECK(statx(a)==0); }
    { reset(); name("relative"); auto a=basic; a[0]=static_cast<u32>(fd); CHECK(statx(a)==-20); unchanged(); }
    { reset(); name("/missing"); auto a=basic; a[4]=0; CHECK(statx(a)==-2); unchanged(); }
    for(auto failure : {Status::io_error(),Status::unsupported(),Status::invalid_argument(),Status::overflow()})
    {
        reset(); file.failure=failure; auto a=basic; a[4]=0;
        CHECK(statx(a)==(failure.code()==StatusCode::unsupported ? -95 : -syscall::ErrnoMapper::errno_for(failure)));
        unchanged(); file.failure={};
    }
    { reset(); name("/"); auto a=basic; a[4]=0; CHECK(statx(a)==-95); unchanged(); }
    { reset(); const auto saved=file.info.attributes; file.info.attributes.valid=0;
      CHECK(statx(basic)==-95); unchanged(); file.info.attributes=saved; }
    for(u64 out : {u64{0},C::word_mask-127,u64{output}})
    {
        reset(); auto a=basic; a[4]=out; mock::forbidden=output+128;
        const auto n=mock::outputs; CHECK(statx(a)==-14 && mock::outputs==n); unchanged();
    }
    mock::forbidden=0;
    for(auto operation : {O::stat,O::lstat,O::fstat,O::newfstatat,O::fstat64,O::fstatat64})
    {
        if(!C::syscall_number(operation)) continue;
        const bool supported=lu::StatCodec<P>::has_kernel_stat &&
            (operation==O::fstat || operation==O::newfstatat ||
             (P==lu::Profile::x86_64 && (operation==O::stat || operation==O::lstat)));
        reset(); std::array<u64,5> a{path,output,0,0,0};
        if(operation==O::fstat) a[0]=0xfeed00000000ull|static_cast<u32>(fd);
        if(operation==O::newfstatat) a={0xffffff9cu,path,output,0,0};
        CHECK(raw(operation,a)==(supported ? 0 : -38));
        if(!supported) { unchanged(); continue; }
        CHECK(mock::path_byte_copies==(operation==O::fstat ? 0u : std::strlen("/stat-provider/file")+1));
        CHECK(get(8,8)==27 && get(48,8)==33 && get(64,8)==2 && get(72,8)==~u64{0});
        const usize size=P==lu::Profile::x86_64 ? 144 : 128;
        for(usize i=2048+size;i<2320;++i) CHECK(mock::memory[i]==std::byte{0xa5});
        if(operation==O::fstat)
        {
            for(u64 bad : {0xffffffffull,30ull})
            { reset(); a[0]=bad; a[1]=0; CHECK(raw(operation,a)==-9); unchanged(); }
        }
    }
    if constexpr(P==lu::Profile::mips)
    {
        reset(); mock::mutate_frame_on_stack_copy=true;
        CHECK(statx(basic)==0);
        CHECK(mock::stack_word_copies==1 && mock::path_byte_copies==std::strlen("/stat-provider/file")+1);
        CHECK(get(32,8)==27 && get(40,8)==33);
        reset(); auto a=basic; a[2]=~u32{0}; const auto q=file.queries,n=mock::copies;
        CHECK(raw(O::statx,a,true)==-14 && q==file.queries && n==mock::copies); unchanged();
        reset(); mock::forbidden=hw::user_begin+1040; CHECK(statx(basic)==-14);
        CHECK(mock::path_byte_copies==0); unchanged(); mock::forbidden=0;
    }
    CHECK(kernel.posix().close(fd));
    std::printf("PROFILE %s checks=%u\n",C::profile.name,checks-before);
}
int main()
{
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    static Kernel kernel; CHECK(kernel.boot({}));
    auto made=kernel.scheduler().create_user_process("stat-adapter",kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096}));
    CHECK(made && kernel.posix().activate(made.value()));
    provider.child=&file;
    CHECK(kernel.posix().mkdir("/stat-provider",0755) && kernel.vfs().mount("/stat-provider",provider));
    profile<lu::Profile::i386>(kernel,made.value(),7,{4,6,5,1,0,2});
    profile<lu::Profile::x86_64>(kernel,made.value(),14,{9,10,11,5,7,6});
    profile<lu::Profile::aarch64>(kernel,made.value(),8,{0,1,2,3,4,5});
    profile<lu::Profile::arm32>(kernel,made.value(),7,{0,1,2,3,4,5});
    profile<lu::Profile::rv64>(kernel,made.value(),17,{10,11,12,13,14,15});
    profile<lu::Profile::rv32>(kernel,made.value(),17,{10,11,12,13,14,15});
    profile<lu::Profile::loongarch64>(kernel,made.value(),11,{4,5,6,7,8,9});
    profile<lu::Profile::mips>(kernel,made.value(),2,{4,5,6,7,8,9});
    profile<lu::Profile::mips64>(kernel,made.value(),2,{4,5,6,7,8,9});
    profile<lu::Profile::ppc>(kernel,made.value(),0,{3,4,5,6,7,8});
    std::printf("PASS actual Linux stat dispatcher hosted profiles=10 checks=%u guest=0\n",checks);
}
