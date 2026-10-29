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


class CreationProvider final : public fs::Node
{
  public:
    fs::Metadata info{.type=fs::NodeType::directory, .mode=fs::mode_for(fs::NodeType::directory, 02777), .uid=37, .gid=73};
    fs::InitialNodeAttributes received{};
    fs::Node *child{};
    Status failure{Status::io_error("injected creation failure")};
    bool fail_metadata{};
    usize creates{}, legacy_creates{}, chmods{}, chowns{};
    std::string_view name() const override { return "mkdir-provider"; }
    fs::Metadata metadata() const override { return info; }
    Result<fs::Metadata> query_metadata() const override
    {
        return fail_metadata ? Result<fs::Metadata>{Status::io_error()} : Result<fs::Metadata>{info};
    }
    fs::Node *lookup(std::string_view name) override
    {
        return name == ".." ? this : name == "new" ? child : nullptr;
    }
    Result<fs::Node *> create_child(std::string_view name, const fs::InitialNodeAttributes &attributes) override
    {
        CHECK(name == "new"); ++creates; received=attributes;
        return failure;
    }
    Status create(std::string_view, fs::NodeType) override { ++legacy_creates; return Status::unsupported(); }
    Status chmod(u32) override { ++chmods; return Status::unsupported(); }
    Status chown(u32, u32) override { ++chowns; return Status::unsupported(); }
    Result<fs::FileBuffer> read(usize, usize) const override { return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { return Status::unsupported(); }
};
static CreationProvider provider;

template <lu::Profile P>
void directory_creation(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize,3> slots)
{
    char base[48];
    std::snprintf(base, sizeof base, "/mkdir-%s", lu::Codec<P>::profile.name);
    CHECK(kernel.posix().mkdir(base,0777));
    CHECK(kernel.posix().chmod(base,0777));
    auto directory=kernel.posix().open(base,posix::o_RDONLY|posix::o_DIRECTORY); CHECK(directory);
    CHECK(kernel.posix().chdir(base));
    constexpr uptr pointer=hw::user_begin+128;
    usize forms{};
    for (auto operation : {lu::Operation::mkdir, lu::Operation::mkdirat})
    {
        auto number=lu::Codec<P>::syscall_number(operation);
        if (!number) continue;
        ++forms;
        const bool at=operation == lu::Operation::mkdirat;
        auto raw=[&](u64 address, u64 mode, u64 dirfd=static_cast<u32>(posix::at_FDCWD)) {
            hw::UserFrame frame{}; frame.registers.fill(~uptr{0}); frame.sp=0;
            frame.registers[number_slot]=*number;
            frame.registers[slots[0]]=at ? dirfd : address;
            frame.registers[slots[1]]=at ? address : mode;
            if (at) frame.registers[slots[2]]=mode;
            bool complete=true;
            const auto outputs=mock::outputs;
            const auto result=user::linux_bootstrap::dispatch<P>(kernel,pid,frame,complete,
                [&](hw::Syscall, bool &) -> i64 { CHECK(false); return -38; });
            CHECK(complete && outputs == mock::outputs);
            CHECK(user::linux_bootstrap::complete<P>(frame,result));
            auto returned=lu::NativeBridge<P>::decode_return(frame); CHECK(returned);
            if (result >= 0) CHECK(returned.value().error == 0 && returned.value().value == static_cast<u64>(result));
            else
            {
                u32 error=static_cast<u32>(-result);
                if constexpr (P == lu::Profile::mips || P == lu::Profile::mips64)
                {
                    if (error == 38) error=89;
                    else if (error == 36) error=78;
                    else if (error == 40) error=90;
                }
                CHECK(returned.value().error == error);
            }
            return result;
        };
        auto invoke=[&](std::string_view path, u64 mode=0777, u64 dirfd=static_cast<u32>(posix::at_FDCWD)) {
            CHECK(path.size() < 512);
            std::memcpy(mock::memory.data()+128,path.data(),path.size());
            mock::memory[128+path.size()]=std::byte{0};
            const auto copies=mock::copies;
            auto result=raw(pointer,mode,dirfd);
            // These calls use only registers, including MIPS O32; no stack reads.
            CHECK(mock::copies-copies == (path.size() < 256 ? path.size()+1 : 256));
            return result;
        };
        auto metadata=[&](std::string_view path, u32 mode, u32 uid, u32 gid) {
            auto info=kernel.posix().stat(path); CHECK(info);
            CHECK(info.value().type == fs::NodeType::directory);
            if (info.value().mode != fs::mode_for(fs::NodeType::directory,mode))
                std::fprintf(stderr,"%s %.*s: mode %o, expected %o\n",lu::Codec<P>::profile.name,
                             static_cast<int>(path.size()),path.data(),info.value().mode,fs::mode_for(fs::NodeType::directory,mode));
            CHECK(info.value().mode == fs::mode_for(fs::NodeType::directory,mode));
            CHECK(info.value().uid == uid && info.value().gid == gid);
        };
        CHECK(kernel.posix().umask(0027) == 0022);
        CHECK(invoke("masked",(~u64{0} << 16)|07777) == 0);
        metadata("masked",01750,0,0);
        CHECK(invoke("masked",0) == -17); metadata("masked",01750,0,0);
        CHECK(kernel.posix().rmdir("masked"));
        CHECK(kernel.posix().umask(0777) == 0027);
        CHECK(invoke("sticky",07777) == 0); metadata("sticky",01000,0,0);
        CHECK(kernel.posix().rmdir("sticky"));
        CHECK(kernel.posix().umask(0022) == 0777);
        CHECK(invoke("trailing///",0755) == 0); metadata("trailing",0755,0,0);
        CHECK(invoke("trailing///") == -17);
        CHECK(kernel.posix().rmdir("trailing"));
        auto normal=kernel.posix().open("file",posix::o_CREAT|posix::o_RDWR,0644); CHECK(normal);
        CHECK(invoke("file") == -17 && invoke("file/") == -17 && invoke("file/child") == -20);
        CHECK(kernel.posix().symlinkat("missing",posix::at_FDCWD,"link"));
        CHECK(invoke("link") == -17 && invoke("link/") == -17 && invoke("link/child") == -2);
        CHECK(kernel.posix().mkdir("target",0777));
        CHECK(kernel.posix().symlinkat("target",posix::at_FDCWD,"middle"));
        CHECK(invoke("middle/child",0700) == 0); metadata("target/child",0700,0,0);
        CHECK(kernel.posix().rmdir("target/child") && kernel.posix().unlink("middle") && kernel.posix().rmdir("target"));
        CHECK(kernel.posix().symlinkat("loop",posix::at_FDCWD,"loop"));
        CHECK(invoke("loop/child") == -40 && invoke("loop") == -17);
        CHECK(invoke(".") == -17 && invoke("..") == -17 && invoke("/") == -17);
        CHECK(invoke("") == -2);
        CHECK(invoke("missing/child") == -2);
        if (at)
        {
            CHECK(invoke("",0777,0x1234ffffU) == -2);
            CHECK(invoke("relative",0777,0x1234ffffU) == -9);
            CHECK(invoke("relative",0777,normal.value()) == -20);
            CHECK(invoke(base,0777,~u64{0}) == -17);
            CHECK(invoke("descriptor",0700,(u64{0xcafe} << 32)|static_cast<u32>(directory.value())) == 0);
            metadata("descriptor",0700,0,0); CHECK(kernel.posix().rmdir("descriptor"));
            CHECK(invoke("signed",0700,(u64{0xdead} << 32)|static_cast<u32>(posix::at_FDCWD)) == 0);
            CHECK(kernel.posix().rmdir("signed"));
        }
        CHECK(kernel.posix().close(normal.value()));
        CHECK(kernel.posix().unlink("file") && kernel.posix().unlink("link") && kernel.posix().unlink("loop"));

        // Effective credentials and inherited directory group precede publication.
        CHECK(kernel.posix().chown(base,0,73) && kernel.posix().chmod(base,02777));
        CHECK(kernel.posix().set_credentials({.uid=11,.gid=12,.euid=37,.egid=41}));
        CHECK(invoke("owned",07777) == 0); metadata("owned",03755,37,73);
        auto file=kernel.posix().open("inherited-file",posix::o_CREAT|posix::o_RDWR,0666); CHECK(file);
        auto info=kernel.posix().stat("inherited-file"); CHECK(info);
        CHECK(info.value().gid == 73 && info.value().uid == 37 && info.value().mode == fs::mode_for(fs::NodeType::regular,0644));
        CHECK(kernel.posix().close(file.value()));
        CHECK(kernel.posix().set_credentials(user::root_credentials()));
        CHECK(kernel.posix().chmod(base,0555));
        CHECK(kernel.posix().set_credentials({.uid=11,.gid=12,.euid=37,.egid=41}));
        CHECK(invoke("owned") == -17 && invoke("denied") == -13);
        CHECK(kernel.posix().set_credentials(user::root_credentials()));
        CHECK(kernel.posix().chmod(base,0777));
        CHECK(kernel.posix().rmdir("owned") && kernel.posix().unlink("inherited-file"));
        CHECK(kernel.posix().chown(base,0,0));

        // A terminating NUL can be the final readable byte in a mapping/page.
        for (const usize end : {usize{4096}, mock::memory.size()})
        {
            constexpr char name[]="boundary";
            const usize begin=end-sizeof name;
            std::memcpy(mock::memory.data()+begin,name,sizeof name);
            mock::forbidden=hw::user_begin+end;
            const auto copies=mock::copies;
            CHECK(raw(hw::user_begin+begin,0700) == 0 && mock::copies-copies == sizeof name);
            CHECK(kernel.posix().rmdir(name));
            mock::forbidden=0;
        }
        CHECK(raw(0,0777,0x1234ffffU) == -14);
        CHECK(raw(~u64{0},0777) == -14);
        std::memcpy(mock::memory.data()+128,"faulting",9);
        mock::forbidden=pointer+3;
        CHECK(raw(pointer,0777,0x1234ffffU) == -14);
        mock::forbidden=0;
        CHECK(!kernel.posix().stat("faulting"));
        std::memcpy(mock::memory.data()+128,"word",5);
        if constexpr (lu::Codec<P>::profile.word_bytes == 4)
        {
            CHECK(raw((u64{0xdeadbeef} << 32)|pointer,0700) == 0);
            CHECK(kernel.posix().rmdir("word"));
        }
        else CHECK(raw((u64{1} << 48)|pointer,0700) == -14);
        const std::string_view component31="1234567890123456789012345678901";
        CHECK(invoke(component31) == 0 && kernel.posix().rmdir(component31));
        CHECK(invoke("12345678901234567890123456789012") == -36);
        std::array<char,257> long_path{}; long_path.fill('/');
        CHECK(invoke({long_path.data(),255}) == -17);
        CHECK(invoke({long_path.data(),256}) == -36);

        const auto creates=provider.creates;
        provider.fail_metadata=true;
        CHECK(invoke("/mkdir-provider/new") == -5 && provider.creates == creates);
        provider.fail_metadata=false;
        for (const auto failure : {Status::io_error(),Status::unsupported(),Status::no_space(),Status::link_limit()})
        {
            provider.failure=failure;
            CHECK(invoke("/mkdir-provider/new",07777) == -syscall::ErrnoMapper::errno_for(failure));
            CHECK(provider.received.type == fs::NodeType::directory && provider.received.mode == 03755);
            CHECK(provider.received.uid == 0 && provider.received.gid == 73 && !provider.child);
        }
        CHECK(provider.creates == creates+4 && !provider.legacy_creates && !provider.chmods && !provider.chowns);
        // Native special bits remain a separate policy from Linux mkdir.
        CHECK(kernel.posix().mkdir("native",07777)); metadata("native",07755,0,0);
        CHECK(kernel.posix().rmdir("native"));
        std::printf("PASS Linux %s %s: modes, credentials, dirfd, path copies, provider failures\n",
                    lu::Codec<P>::profile.name,at ? "mkdirat" : "mkdir");
    }
    constexpr bool legacy=P == lu::Profile::i386 || P == lu::Profile::x86_64 || P == lu::Profile::arm32 ||
                          P == lu::Profile::mips || P == lu::Profile::mips64 || P == lu::Profile::ppc;
    CHECK(forms == (legacy ? 2 : 1));
    CHECK(kernel.posix().chdir("/") && kernel.posix().close(directory.value()) && kernel.posix().rmdir(base));
}

int main()
{
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    static Kernel kernel; CHECK(kernel.boot({}));
    auto made=kernel.scheduler().create_user_process("mkdir-adapter",kernel.arch().make_user_context({hw::user_begin,hw::user_end-4096}));
    CHECK(made && kernel.posix().activate(made.value()));
    CHECK(kernel.posix().mkdir("/mkdir-provider",0755) && kernel.vfs().mount("/mkdir-provider",provider));
    directory_creation<lu::Profile::i386>(kernel,made.value(),7,{4,6,5});
    directory_creation<lu::Profile::x86_64>(kernel,made.value(),14,{9,10,11});
    directory_creation<lu::Profile::aarch64>(kernel,made.value(),8,{0,1,2});
    directory_creation<lu::Profile::arm32>(kernel,made.value(),7,{0,1,2});
    directory_creation<lu::Profile::rv64>(kernel,made.value(),17,{10,11,12});
    directory_creation<lu::Profile::rv32>(kernel,made.value(),17,{10,11,12});
    directory_creation<lu::Profile::loongarch64>(kernel,made.value(),11,{4,5,6});
    directory_creation<lu::Profile::mips>(kernel,made.value(),2,{4,5,6});
    directory_creation<lu::Profile::mips64>(kernel,made.value(),2,{4,5,6});
    directory_creation<lu::Profile::ppc>(kernel,made.value(),0,{3,4,5});
}
