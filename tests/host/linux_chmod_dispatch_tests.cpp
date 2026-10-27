// Actual chmod dispatcher/service with hosted user memory and an injected provider.
// Provider outcomes are modeled; this does not qualify ext4 writes or guest execution.
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

static int filesystem_token;
class ModeProvider final : public fs::Node
{
  public:
    fs::Metadata info{};
    ModeProvider *child{};
    usize requests{}, legacy_requests{}, references{};
    u32 requested{};
    user::Credentials captured{};
    fs::ModeChangeResult outcome{fs::ModeChangeResult::applied()};
    Status cleanup{};
    explicit ModeProvider(fs::NodeType kind)
    {
        info.type = kind; info.mode = fs::mode_for(kind, 0777);
        info.uid = 37; info.gid = 73; info.size = 33;
    }
    std::string_view name() const override { return "mode-provider"; }
    const void *filesystem_identity() const override { return &filesystem_token; }
    fs::Metadata metadata() const override { return info; }
    fs::Node *lookup(std::string_view name) override
    { return name == "file" ? child : name == ".." ? this : nullptr; }
    Status retain() override { ++references; return {}; }
    Status release() override { CHECK(references); --references; return cleanup; }
    Status create(std::string_view, fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { ++legacy_requests; return Status::unsupported(); }
    fs::ModeChangeResult change_mode(u32 mode, user::Credentials credentials) override
    { ++requests; requested = mode; captured = credentials; return outcome; }
    Status chown(u32,u32) override { return Status::unsupported(); }
    Result<fs::FileBuffer> read(usize,usize) const override { return Status::unsupported(); }
    Status write(usize,std::span<const std::byte>) override { return Status::unsupported(); }
};
static ModeProvider provider{fs::NodeType::directory}, file{fs::NodeType::regular};

template <lu::Profile P>
void profile(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize,4> slots)
{
    const auto before = checks;
    using C = lu::Codec<P>;
    using O = lu::Operation;
    constexpr uptr path = hw::user_begin + 128;
    const auto credentials = user::Credentials{.uid=901,.gid=902,.euid=37,.egid=73};
    CHECK(kernel.posix().set_credentials(credentials));
    auto directory = kernel.posix().open("/mode-provider",posix::o_RDONLY|posix::o_DIRECTORY);
    CHECK(directory);
    auto name = [&](const char *value) {
        std::strcpy(reinterpret_cast<char *>(mock::memory.data()+128), value);
    };
    auto raw = [&](O operation, std::array<u64,4> arguments) -> i64 {
        const auto number = C::syscall_number(operation); CHECK(number);
        hw::UserFrame frame{};
        frame.registers.fill(~uptr{0});
        frame.sp = 0xfffffff0; // All four calls must ignore even an unusable O32 stack.
        frame.registers[number_slot] = *number;
        for (usize i=0; i<arguments.size(); ++i) frame.registers[slots[i]] = arguments[i];
        const auto outputs = mock::outputs;
        const auto memory = mock::memory;
        bool done = true;
        const auto result = user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
            [&](hw::Syscall,bool &) -> i64 { CHECK(false); return -38; });
        CHECK(done && mock::outputs == outputs && mock::memory == memory);
        CHECK(user::linux_bootstrap::complete<P>(frame,result));
        const auto returned = lu::NativeBridge<P>::decode_return(frame); CHECK(returned);
        u32 expected = result < 0 ? static_cast<u32>(-result) : 0;
        if constexpr (P == lu::Profile::mips || P == lu::Profile::mips64)
        {
            if (expected == 95) expected = 122;
            else if (expected == 38) expected = 89;
            else if (expected == 75) expected = 79;
            else if (expected == 36) expected = 78;
            else if (expected == 40) expected = 90;
        }
        CHECK(returned.value().error == expected);
        CHECK(expected || returned.value().value == 0);
        return result;
    };
    const u64 wide_fd = 0xfeed00000000ull | static_cast<u32>(directory.value());
    const u64 wide_path = C::profile.word_bytes == 4 ? 0xfeed00000000ull | path : path;
    const std::array<u64,4> basic{wide_fd,wide_path,0xffffffff00000000ull | 07541,0};
    name("file");
    const auto refs = file.references, dirrefs = provider.references;
    for (u32 flags : {0u,0x100u,0x1000u,0x1100u})
    {
        auto args = basic; args[3] = 0xffffffff00000000ull | flags;
        const auto requests = file.requests, copies = mock::copies;
        CHECK(raw(O::fchmodat2,args) == 0 && file.requests == requests + 1);
        CHECK(file.requested == 07541 && mock::copies == copies + 5);
        CHECK(file.captured.uid == 901 && file.captured.gid == 902 &&
              file.captured.euid == 37 && file.captured.egid == 73 && !file.captured.kernel_space);
        CHECK(file.references == refs && provider.references == dirrefs);
    }
    // Legacy fchmodat is exactly three arguments; the poisoned fourth register is ignored.
    { auto args=basic; args[3]=~u64{0}; CHECK(raw(O::fchmodat,args)==0); }
    for (u32 flags : {1u,0x800u,0x2000u,0xffffffffu})
    {
        auto args=basic; args[1]=0; args[3]=flags;
        const auto requests=file.requests,copies=mock::copies;
        CHECK(raw(O::fchmodat2,args)==-22 && file.requests==requests && mock::copies==copies);
    }
    // NULL remains EFAULT even with EMPTY_PATH, with no attempted user copy.
    for (u32 flags : {0u,0x1000u,0x1100u})
    {
        auto args=basic; args[1]=0; args[3]=flags;
        const auto copies=mock::copies; CHECK(raw(O::fchmodat2,args)==-14 && mock::copies==copies);
    }
    name("");
    CHECK(raw(O::fchmodat,basic)==-2 && raw(O::fchmodat2,basic)==-2);
    { auto args=basic; args[3]=0x1100;
      const auto requests=provider.requests; CHECK(raw(O::fchmodat2,args)==0 && provider.requests==requests+1); }
    { auto args=basic; args[0]=0xffffffffffffffffull; args[3]=0x1000; CHECK(raw(O::fchmodat2,args)==-9); }
    CHECK(kernel.posix().chdir("/mode-provider"));
    { auto args=basic; args[0]=0xfeed0000ffffff9cull; args[3]=0x1000;
      const auto requests=provider.requests; CHECK(raw(O::fchmodat2,args)==0 && provider.requests==requests+1); }
    CHECK(kernel.posix().chdir("/"));
    name("/mode-provider/file");
    { auto args=basic; args[0]=~u64{0}; CHECK(raw(O::fchmodat2,args)==0); }
    if constexpr (lu::ChmodCallAdapter<P>::has_chmod)
    {
        CHECK(C::syscall_number(O::chmod));
        CHECK(raw(O::chmod,{wide_path,~u64{0},0,0})==0 && file.requested==07777);
        CHECK(raw(O::chmod,{0,0600,0,0})==-14);
    }
    else CHECK(!C::syscall_number(O::chmod));
    name("file");
    { auto args=basic; args[0]=0xffffffff; CHECK(raw(O::fchmodat2,args)==-9); }
    for (u32 access : {posix::o_RDONLY,posix::o_WRONLY,posix::o_RDWR})
    {
        auto opened=kernel.posix().open("/mode-provider/file",access|posix::o_CLOEXEC); CHECK(opened);
        const i32 fd=opened.value();
        const auto *slot=kernel.scheduler().find(pid)->fd_table().entry(fd); CHECK(slot);
        const auto *description=kernel.scheduler().open_description(slot->description); CHECK(description);
        const auto offset=description->offset; const auto saved_flags=description->flags;
        const auto copies=mock::copies,requests=file.requests;
        CHECK(raw(O::fchmod,{0xfeed00000000ull|static_cast<u32>(fd),~u64{0},~u64{0},~u64{0}})==0);
        CHECK(file.requested==07777 && file.requests==requests+1 && mock::copies==copies);
        CHECK(description->offset==offset && description->flags==saved_flags && slot->close_on_exec);
        { auto args=basic; args[0]=fd; CHECK(raw(O::fchmodat2,args)==-20); }
        name("");
        { auto args=basic; args[0]=fd; args[3]=0x1100; CHECK(raw(O::fchmodat2,args)==0); }
        name("file");
        CHECK(kernel.posix().close(fd));
        CHECK(raw(O::fchmod,{static_cast<u32>(fd),0600,0,0})==-9);
    }
    CHECK(raw(O::fchmod,{0xffffffff,0600,0,0})==-9);
    const std::array outcomes{fs::ModeChangeResult::read_only(),fs::ModeChangeResult::unavailable(),
        fs::ModeChangeResult::failure(Status::permission()),fs::ModeChangeResult::failure(Status::denied()),
        fs::ModeChangeResult::failure(Status::io_error()),fs::ModeChangeResult::failure(Status::unsupported()),
        fs::ModeChangeResult::failure(Status::overflow())};
    const std::array<i64,7> errors{-30,-95,-1,-13,-5,-38,-75};
    for (usize i=0;i<outcomes.size();++i)
    { file.outcome=outcomes[i]; CHECK(raw(O::fchmodat2,basic)==errors[i]); }
    file.outcome=fs::ModeChangeResult::applied();
    provider.info.mode=fs::mode_for(fs::NodeType::directory,0000);
    const auto denied_requests=file.requests;
    CHECK(raw(O::fchmodat2,basic)==-13 && file.requests==denied_requests);
    provider.info.mode=fs::mode_for(fs::NodeType::directory,0777);
    file.cleanup=Status::io_error();
    const auto committed_requests=file.requests;
    CHECK(raw(O::fchmodat2,basic)==-5 && file.requests==committed_requests+1);
    file.outcome=fs::ModeChangeResult::read_only();
    CHECK(raw(O::fchmodat2,basic)==-30); // Primary provider failure wins over cleanup.
    file.cleanup={}; file.outcome=fs::ModeChangeResult::applied();
    name("file"); mock::forbidden=path+2;
    const auto requests=file.requests;
    CHECK(raw(O::fchmodat2,basic)==-14 && file.requests==requests); mock::forbidden=0;
    std::memset(mock::memory.data()+128,'x',256);
    CHECK(raw(O::fchmodat2,basic)==-36 && file.requests==requests);
    CHECK(file.legacy_requests==0 && provider.legacy_requests==0);
    CHECK(file.references==refs && provider.references==dirrefs);
    CHECK(kernel.posix().close(directory.value()));
    CHECK(kernel.posix().set_credentials(user::root_credentials()));
    std::printf("PROFILE %s checks=%u\n",C::profile.name,checks-before);
}

int main()
{
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    static Kernel kernel; CHECK(kernel.boot({}));
    auto made=kernel.scheduler().create_user_process("chmod-adapter",kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096}));
    CHECK(made && kernel.posix().activate(made.value()));
    provider.child=&file;
    CHECK(kernel.posix().mkdir("/mode-provider",0755) && kernel.vfs().mount("/mode-provider",provider));
    profile<lu::Profile::i386>(kernel,made.value(),7,{4,6,5,1});
    profile<lu::Profile::x86_64>(kernel,made.value(),14,{9,10,11,5});
    profile<lu::Profile::aarch64>(kernel,made.value(),8,{0,1,2,3});
    profile<lu::Profile::arm32>(kernel,made.value(),7,{0,1,2,3});
    profile<lu::Profile::rv64>(kernel,made.value(),17,{10,11,12,13});
    profile<lu::Profile::rv32>(kernel,made.value(),17,{10,11,12,13});
    profile<lu::Profile::loongarch64>(kernel,made.value(),11,{4,5,6,7});
    profile<lu::Profile::mips>(kernel,made.value(),2,{4,5,6,7});
    profile<lu::Profile::mips64>(kernel,made.value(),2,{4,5,6,7});
    profile<lu::Profile::ppc>(kernel,made.value(),0,{3,4,5,6});
    std::printf("PASS Linux chmod dispatcher hosted profiles=10 checks=%u guest=0\n",checks);
}
