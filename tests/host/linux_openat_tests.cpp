#include "unavailable_realtime.hpp"
// Actual Linux adapter/common native copy boundary with hosted mappings only.
#include "native_runtime_units.hpp"
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
// Sparse metadata fixture proves the size check precedes destructive truncate;
// it does not allocate a multi-gigabyte file or pretend to test disk storage.
class LargeNode final : public fs::Node
{
  public:
    explicit LargeNode(bool directory = false) : directory_(directory)
    {
    }
    LargeNode *child{};
    usize size = 0x8000'0000u;
    usize truncations{}, references{}, sync_calls{};
    Status next_sync{};
    Status sync() override
    {
        ++sync_calls;
        return next_sync;
    }
    std::string_view name() const override
    {
        return directory_ ? "large" : "file";
    }
    fs::Metadata metadata() const override
    {
        return {.type = directory_ ? fs::NodeType::directory : fs::NodeType::regular,
                .size = directory_ ? 0 : size,
                .mode = 0777};
    }
    Result<fs::FileBuffer> read(usize, usize) const override
    {
        return Status::unsupported();
    }
    Status write(usize, std::span<const std::byte>) override
    {
        return Status::unsupported();
    }
    Status truncate(usize length) override
    {
        ++truncations;
        size = length;
        return Status::success();
    }
    Status retain() override
    {
        ++references;
        return Status::success();
    }
    Status release() override
    {
        CHECK(references);
        --references;
        return Status::success();
    }
    Node *lookup(std::string_view part) override
    {
        return part == ".." ? this : directory_ && part == "file" ? child : nullptr;
    }
    Status create(std::string_view, fs::NodeType) override
    {
        return Status::unsupported();
    }
    Status chmod(u32) override
    {
        return Status::unsupported();
    }
    Status chown(u32, u32) override
    {
        return Status::unsupported();
    }

  private:
    bool directory_;
};
constexpr uptr path_address = hw::user_begin + 128;
void put_path(std::string_view path, uptr address = path_address)
{
    CHECK(address >= hw::user_begin && path.size() + 1 <= mock::memory.size() - (address - hw::user_begin));
    std::memcpy(mock::memory.data() + address - hw::user_begin, path.data(), path.size());
    mock::memory[address - hw::user_begin + path.size()] = std::byte{};
}
template <lu::Profile P>
void test_profile(Kernel &kernel, sched::ProcessId pid, LargeNode &large, usize number_slot, std::array<usize, 4> slots,
                  u32 openat_number)
{
    using Open = lu::OpenCodec<P>;
    auto *process = kernel.scheduler().find(pid);
    auto call = [&](i32 dirfd, uptr pathname, u64 flags, u32 mode = 0666) {
        hw::UserFrame frame{};
        frame.registers[number_slot] = openat_number;
        const std::array<u64, 4> args{static_cast<u32>(dirfd), pathname, flags, mode};
        for (usize i = 0; i < 4; ++i)
            frame.registers[slots[i]] = args[i];
        bool done = true;
        usize callbacks = 0;
        const auto result =
            user::linux_bootstrap::dispatch<P>(kernel, pid, frame, done, [&](hw::Syscall request, bool &complete) {
                ++callbacks;
                return native::dispatch(kernel, pid, request, complete);
            });
        CHECK(done && callbacks == 0);
        CHECK(user::linux_bootstrap::complete<P>(frame, result));
        auto returned = lu::NativeBridge<P>::decode_return(frame);
        CHECK(returned);
        const u32 expected = result >= 0     ? 0
                             : result == -75 ? (P == lu::Profile::mips || P == lu::Profile::mips64 ? 79 : 75)
                             : result == -36 ? (P == lu::Profile::mips || P == lu::Profile::mips64 ? 78 : 36)
                             : result == -40 ? (P == lu::Profile::mips || P == lu::Profile::mips64 ? 90 : 40)
                                             : static_cast<u32>(-result);
        CHECK(returned.value().error == expected);
        if (result >= 0)
            CHECK(returned.value().value == static_cast<u64>(result));
        return result;
    };
    auto open = [&](std::string_view path, u64 flags, u32 mode = 0666) {
        put_path(path);
        return call(posix::at_FDCWD, path_address, flags, mode);
    };
    CHECK(open("/work", Open::directory) == 3);
    put_path("file");
    auto fd = call(3, path_address, 2 | Open::create | Open::exclusive | Open::cloexec | Open::nonblock | Open::no_ctty,
                   07777);
    CHECK(fd == 4);
    const auto *slot = process->fd_table().entry(4);
    CHECK(slot && slot->close_on_exec);
    const auto *desc = kernel.scheduler().open_description(slot->description);
    CHECK(desc && desc->flags == (posix::o_RDWR | posix::o_CREAT | posix::o_EXCL | posix::o_NONBLOCK));
    const char data[] = "payload";
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte *>(data), sizeof(data) - 1};
    CHECK(kernel.posix().write(4, bytes).value() == sizeof(data) - 1);
    CHECK(kernel.posix().close(4));
    CHECK(open("/work/file", Open::create | Open::exclusive) == -17);
    CHECK(open("/work/file", Open::directory) == -20);
    CHECK(open("/work", 1) == -21);
    CHECK(open("/work", Open::truncate) == -21);
    CHECK(open("/work", Open::create) == -21);
    put_path("file");
    CHECK(call(-1, path_address, 0) == -9);
    CHECK(open("/work/file", 0) == 4);
    put_path("file");
    CHECK(call(4, path_address, 0) == -20);
    CHECK(kernel.posix().close(4));
    CHECK(open("/work/file", Open::append) == 4);
    std::byte first{};
    CHECK(kernel.posix().read(4, {&first, 1}).value() == 1 && first == std::byte{'p'});
    CHECK(kernel.posix().close(4));
    CHECK(open("/work/file", 2 | Open::append) == 4);
    first = std::byte{};
    CHECK(kernel.posix().read(4, {&first, 1}).value() == 1 && first == std::byte{'p'});
    CHECK(kernel.posix().write(4, bytes).value() == sizeof(data) - 1);
    CHECK(kernel.posix().close(4));
    CHECK(kernel.posix().stat("/work/file").value().size == 2 * (sizeof(data) - 1));
    CHECK(open("/work/link", Open::nofollow) == -40);
    CHECK(open("/work/link", 0) == 4);
    CHECK(kernel.posix().close(4));
    CHECK(open("", Open::create) == -2);
    CHECK(open("/work/invalid", 3 | Open::create) == -22);
    CHECK(open("/work/invalid", 0x80000000u | Open::create) == -22);
    CHECK(kernel.posix().stat("/work/invalid").status().code() == StatusCode::not_found);
    CHECK(call(-1, hw::user_begin - 1, Open::create) == -14);
    CHECK(call(posix::at_FDCWD, ~uptr{0}, Open::create) == -14);
    put_path("/work/file", hw::user_begin + mock::memory.size() - 11);
    CHECK(call(-1, hw::user_begin + mock::memory.size() - 11, 0) == 4);
    CHECK(kernel.posix().close(4));
    put_path("/work/file");
    mock::forbidden = path_address + 4;
    CHECK(call(posix::at_FDCWD, path_address, Open::truncate) == -14);
    mock::forbidden = 0;
    CHECK(kernel.posix().stat("/work/file").value().size == 2 * (sizeof(data) - 1));
    std::memset(mock::memory.data() + 128, 'a', 256);
    CHECK(call(posix::at_FDCWD, path_address, 0) == -36);
    CHECK(open("/work/abcdefghijklmnopqrstuvwxyz0123456789", 0) == -36);
    CHECK(open("/work/abcdefghijklmnopqrstuvwxyz0123456789", Open::create | Open::nofollow) == -36);
    if constexpr (lu::Codec<P>::profile.word_bytes == 8)
    {
        CHECK(open("/work/file", u64{1} << 40) == 4);
        CHECK(kernel.posix().close(4));
    }
    large.size = 0x8000'0000u;
    const auto before = large.truncations;
    const auto non_lfs = open("/large/file", 1 | Open::truncate);
    if constexpr (lu::Codec<P>::profile.word_bytes == 4)
    {
        CHECK(non_lfs == -75 && large.truncations == before && large.size == 0x8000'0000u);
        CHECK(open("/large/file", 1 | Open::truncate | Open::largefile) == 4);
    }
    else
        CHECK(non_lfs == 4);
    CHECK(large.truncations == before + 1 && large.size == 0);
    CHECK(kernel.posix().close(4));
    CHECK(large.references == 0);
    large.size = 0x7fff'ffffu;
    CHECK(open("/large/file", 0) == 4);
    CHECK(kernel.posix().close(4));
    large.size = 0x8000'0000u;
    auto native_open = kernel.posix().open("/large/file", posix::o_RDONLY);
    CHECK(native_open && native_open.value() == 4);
    CHECK(kernel.posix().close(4));
    CHECK(kernel.posix().close(3));
    CHECK(kernel.posix().unlink("/work/file"));
    CHECK(kernel.scheduler().open_description_count() == 3 && large.references == 0);
    std::printf(
        "PASS hosted Linux openat %s: flags, dirfd, copy faults/length, large-file policy, errno and lifetime\n",
        lu::Codec<P>::profile.name);
}
template <lu::Profile P>
void test_sync(Kernel &kernel, sched::ProcessId pid, LargeNode &large, usize number_slot, usize fd_slot,
               u32 fsync_number, u32 fdatasync_number)
{
    auto call = [&](u32 number, u64 fd) {
        hw::UserFrame frame{};
        frame.registers[number_slot] = number;
        frame.registers[fd_slot] = fd;
        bool done = true;
        usize calls = 0;
        auto result =
            user::linux_bootstrap::dispatch<P>(kernel, pid, frame, done, [&](hw::Syscall request, bool &complete) {
                ++calls;
                CHECK(request.number ==
                      static_cast<uptr>(number == fsync_number ? syscall::Number::fsync : syscall::Number::fdatasync));
                return native::dispatch(kernel, pid, request, complete);
            });
        CHECK(done && calls == 1 && user::linux_bootstrap::complete<P>(frame, result));
        auto returned = lu::NativeBridge<P>::decode_return(frame);
        CHECK(returned);
        CHECK(returned.value().error == static_cast<u32>(result < 0 ? -result : 0));
        return result;
    };
    CHECK(call(fsync_number, static_cast<u32>(-1)) == -9);
    CHECK(call(fdatasync_number, 31) == -9);
    CHECK(call(fsync_number, 1) == -22);
    auto pipes = kernel.posix().pipe();
    CHECK(pipes);
    CHECK(call(fsync_number, pipes.value()[0]) == -22 && call(fdatasync_number, pipes.value()[1]) == -22);
    CHECK(kernel.posix().close(pipes.value()[0]) && kernel.posix().close(pipes.value()[1]));
    large.size = 0;
    large.next_sync = Status::success();
    auto fd = kernel.posix().open("/large/file", posix::o_RDONLY);
    CHECK(fd);
    auto before = large.sync_calls;
    CHECK(call(fsync_number, fd.value()) == 0 && large.sync_calls == before + 1);
    CHECK(call(fdatasync_number, fd.value()) == 0 && large.sync_calls == before + 2);
    large.next_sync = Status::io_error("injected node sync error");
    CHECK(call(fsync_number, fd.value()) == -5 && large.sync_calls == before + 3);
    CHECK(call(fdatasync_number, fd.value()) == -5 && large.sync_calls == before + 4);
    large.next_sync = Status::success();
    if constexpr (lu::Codec<P>::profile.word_bytes == 8)
        CHECK(call(fsync_number, (u64{1} << 40) | fd.value()) == 0);
    CHECK(kernel.posix().close(fd.value()));
    CHECK(kernel.scheduler().open_description_count() == 3 && large.references == 0);
    std::printf("PASS hosted Linux sync %s: fd normalization, EBADF/EINVAL, common callback and EIO\n",
                lu::Codec<P>::profile.name);
}
template <lu::Profile P>
void test_legacy(Kernel &kernel, sched::ProcessId pid, LargeNode &large, usize number_slot,
                 std::array<usize, 3> slots, u32 open_number, u32 creat_number)
{
    using Open = lu::OpenCodec<P>;
    CHECK(lu::Codec<P>::syscall_number(lu::Operation::open).value() == open_number);
    CHECK(lu::Codec<P>::syscall_number(lu::Operation::creat).value() == creat_number);
    auto call = [&](u32 number, uptr path, u64 second, u64 third = 0) {
        hw::UserFrame frame{};
        frame.registers[number_slot] = number;
        const std::array<u64, 3> args{path, second, third};
        for (usize i=0;i<args.size();++i) frame.registers[slots[i]]=args[i];
        bool done=true;
        const auto result=user::linux_bootstrap::dispatch<P>(kernel,pid,frame,done,
            [&](hw::Syscall,bool &) -> i64 { CHECK(false); return -38; });
        CHECK(done && user::linux_bootstrap::complete<P>(frame,result));
        auto output=lu::NativeBridge<P>::decode_return(frame); CHECK(output);
        if(result>=0) CHECK(output.value().value==static_cast<u64>(result) && !output.value().error);
        else CHECK(output.value().error==Open::pathname_errno(result==-75 &&
                  (P==lu::Profile::mips || P==lu::Profile::mips64) ? 79 : static_cast<u32>(-result)));
        return result;
    };
    const auto old_mask=kernel.posix().umask(0);
    CHECK(kernel.posix().chdir("/work"));
    CHECK(kernel.posix().rename("/work","/moved-work"));
    put_path("legacy");
    auto fd=call(creat_number,path_address,(u64{0xbeef}<<16)|0612u);
    CHECK(fd==3 && kernel.posix().fstat(3).value().mode==(fs::mode_regular|0612u));
    const auto *slot=kernel.scheduler().find(pid)->fd_table().entry(3);
    CHECK(slot && !slot->close_on_exec);
    const auto *description=kernel.scheduler().open_description(slot->description);
    CHECK(description && (description->flags&3)==posix::o_WRONLY);
    const std::array<std::byte,4> bytes{std::byte{'k'},std::byte{'e'},std::byte{'e'},std::byte{'p'}};
    CHECK(kernel.posix().write(3,bytes).value()==bytes.size() && kernel.posix().close(3));
    CHECK(kernel.posix().stat("/moved-work/legacy").value().size==4);
    CHECK(call(open_number,path_address,2|Open::append|Open::cloexec|Open::nonblock)==3);
    slot=kernel.scheduler().find(pid)->fd_table().entry(3);
    CHECK(slot && slot->close_on_exec && kernel.posix().close(3));
    if constexpr(lu::Codec<P>::profile.word_bytes==8)
    {
        CHECK(call(open_number,path_address,(u64{1}<<45)|Open::cloexec)==3);
        CHECK(kernel.posix().close(3));
    }
    CHECK(call(open_number,~uptr{0},Open::truncate|1)==-14);
    CHECK(call(creat_number,~uptr{0},0600)==-14);
    CHECK(call(open_number,~uptr{0},3)==-22);
    CHECK(call(open_number,path_address,Open::create|Open::exclusive|1)==-17);
    CHECK(kernel.posix().stat("legacy").value().size==4);
    std::array<posix::Fd,sched::max_process_fds> descriptors{};
    usize count{};
    while(count<descriptors.size())
    {
        auto opened=kernel.posix().open("legacy",posix::o_RDONLY);
        if(!opened) break;
        descriptors[count++]=opened.value();
    }
    CHECK(count==sched::max_process_fds-3);
    CHECK(call(creat_number,path_address,0600)==-24);
    CHECK(call(open_number,path_address,Open::truncate|1)==-24);
    put_path("not-created");
    CHECK(call(creat_number,path_address,0600)==-24);
    CHECK(kernel.posix().stat("not-created").status().code()==StatusCode::not_found);
    CHECK(kernel.posix().stat("legacy").value().size==4);
    for(usize i=0;i<count;++i) CHECK(kernel.posix().close(descriptors[i]));
    put_path("/large/file");large.size=0x80000000U;
    const auto truncations=large.truncations;
    const auto result=call(creat_number,path_address,0600);
    if constexpr(lu::Codec<P>::profile.word_bytes==4)
        CHECK(result==-75 && large.truncations==truncations && large.size==0x80000000U);
    else
        CHECK(result==3 && large.truncations==truncations+1 && kernel.posix().close(3));
    put_path("legacy");CHECK(call(creat_number,path_address,0777)==3);
    CHECK(kernel.posix().fstat(3).value().size==0 && kernel.posix().fstat(3).value().mode==(fs::mode_regular|0612u));
    CHECK(kernel.posix().close(3));
    CHECK(kernel.posix().chmod("legacy",0000));
    CHECK(kernel.posix().set_identity(1001,1001));
    CHECK(call(open_number,path_address,0)==-13);
    CHECK(kernel.posix().set_identity(0,0));
    CHECK(kernel.posix().unlink("legacy"));
    CHECK(kernel.posix().rename("/moved-work","/work"));
    CHECK(kernel.posix().chdir("/"));
    static_cast<void>(kernel.posix().umask(old_mask));
    CHECK(kernel.scheduler().open_description_count()==3 && large.references==0);
    std::printf("PASS hosted Linux legacy open/creat %s: cwd identity, flags/mode, copies, permissions, pressure-before-mutation and large-file admission\n",lu::Codec<P>::profile.name);
}

int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    auto made = kernel.scheduler().create_user_process(
        "openat", kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096}));
    CHECK(made);
    const auto pid = made.value();
    CHECK(kernel.posix().activate(pid));
    CHECK(kernel.posix().set_identity(0, 0));
    CHECK(kernel.vfs().create("/work", fs::NodeType::directory));
    CHECK(kernel.vfs().create("/large", fs::NodeType::directory));
    CHECK(kernel.posix().symlinkat("file", posix::at_FDCWD, "/work/link"));
    static LargeNode directory(true), large;
    directory.child = &large;
    CHECK(kernel.vfs().mount("/large", directory));
    test_sync<lu::Profile::i386>(kernel, pid, large, 7, 4, 118, 148);
    test_sync<lu::Profile::x86_64>(kernel, pid, large, 14, 9, 74, 75);
    test_sync<lu::Profile::aarch64>(kernel, pid, large, 8, 0, 82, 83);
    test_sync<lu::Profile::arm32>(kernel, pid, large, 7, 0, 118, 148);
    test_sync<lu::Profile::rv64>(kernel, pid, large, 17, 10, 82, 83);
    test_sync<lu::Profile::rv32>(kernel, pid, large, 17, 10, 82, 83);
    test_sync<lu::Profile::loongarch64>(kernel, pid, large, 11, 4, 82, 83);
    test_sync<lu::Profile::mips>(kernel, pid, large, 2, 4, 4118, 4152);
    test_sync<lu::Profile::mips64>(kernel, pid, large, 2, 4, 5072, 5073);
    test_sync<lu::Profile::ppc>(kernel, pid, large, 0, 3, 118, 148);
    test_profile<lu::Profile::i386>(kernel, pid, large, 7, {4, 6, 5, 1}, 295);
    test_profile<lu::Profile::x86_64>(kernel, pid, large, 14, {9, 10, 11, 5}, 257);
    test_profile<lu::Profile::aarch64>(kernel, pid, large, 8, {0, 1, 2, 3}, 56);
    test_profile<lu::Profile::arm32>(kernel, pid, large, 7, {0, 1, 2, 3}, 322);
    test_profile<lu::Profile::rv64>(kernel, pid, large, 17, {10, 11, 12, 13}, 56);
    test_profile<lu::Profile::rv32>(kernel, pid, large, 17, {10, 11, 12, 13}, 56);
    test_profile<lu::Profile::loongarch64>(kernel, pid, large, 11, {4, 5, 6, 7}, 56);
    test_profile<lu::Profile::mips>(kernel, pid, large, 2, {4, 5, 6, 7}, 4288);
    test_profile<lu::Profile::mips64>(kernel, pid, large, 2, {4, 5, 6, 7}, 5247);
    test_profile<lu::Profile::ppc>(kernel, pid, large, 0, {3, 4, 5, 6}, 286);
    test_legacy<lu::Profile::i386>(kernel,pid,large,7,{4,6,5},5,8);
    test_legacy<lu::Profile::x86_64>(kernel,pid,large,14,{9,10,11},2,85);
    test_legacy<lu::Profile::arm32>(kernel,pid,large,7,{0,1,2},5,8);
    test_legacy<lu::Profile::mips>(kernel,pid,large,2,{4,5,6},4005,4008);
    test_legacy<lu::Profile::mips64>(kernel,pid,large,2,{4,5,6},5002,5083);
    test_legacy<lu::Profile::ppc>(kernel,pid,large,0,{3,4,5},5,8);
    for(auto profile : {lu::Profile::aarch64,lu::Profile::rv64,lu::Profile::rv32,lu::Profile::loongarch64})
        for(auto operation : {lu::Operation::open,lu::Operation::creat})
            CHECK(lu::profiles[static_cast<usize>(profile)].numbers[static_cast<usize>(operation)]==lu::unavailable_number);
}
