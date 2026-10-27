// Actual Linux adapter/common native copy boundary with hosted mappings only.
#include "native_runtime_units.hpp"
#include "unavailable_realtime.hpp"
#include "ok/fs/mode_policy.hpp"
#include "../kernel/points/directory_checks.hpp"
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

class DirectoryProvider final : public fs::Node
{
  public:
    fs::Metadata info{.type = fs::NodeType::directory, .mode = fs::mode_for(fs::NodeType::directory, 0755)};
    fs::DirectoryListing entries{};
    DirectoryProvider *parent{this};
    bool fail_query{}, fail_list{}, fail_parent{};
    DirectoryProvider()
    {
        info.attributes.inode = 100;
        info.attributes.mark(fs::Attribute::inode);
    }
    std::string_view name() const override { return "directory-faults"; }
    fs::Metadata metadata() const override { return info; }
    Result<fs::Metadata> query_metadata() const override
    {
        return fail_query ? Result<fs::Metadata>{Status::io_error()} : Result<fs::Metadata>{info};
    }
    Result<fs::DirectoryListing> list(fs::DirectoryReadIntent = fs::DirectoryReadIntent::access) const override
    {
        return fail_list ? Result<fs::DirectoryListing>{Status::io_error()} : Result<fs::DirectoryListing>{entries};
    }
    fs::Node *lookup(std::string_view child) override { return child == ".." ? parent : nullptr; }
    Result<fs::Node *> lookup_child(std::string_view child) override
    {
        if (fail_parent) return Status::io_error();
        return lookup(child);
    }
    Result<fs::FileBuffer> read(usize, usize) const override { return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { return Status::unsupported(); }
    Status create(std::string_view, fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32, u32) override { return Status::unsupported(); }
};
static DirectoryProvider fault_provider, fault_parent;

// RamNode deliberately has no checked mode-change service. This test-only
// forwarding provider supplies that boundary for the hosted dispatch model;
// mounted ext4 persistence is exercised by the separate directory-mode suite.
class RetainedDirectory final : public fs::Node
{
  public:
    fs::Node *backing{};
    std::string_view name() const override { return backing->name(); }
    fs::Metadata metadata() const override { return backing->metadata(); }
    Result<fs::Metadata> query_metadata() const override { return backing->query_metadata(); }
    Result<fs::DirectoryListing> list(fs::DirectoryReadIntent intent) const override { return backing->list(intent); }
    fs::Node *lookup(std::string_view child) override { return child == "." ? this : backing->lookup(child); }
    Result<fs::Node *> lookup_child(std::string_view child) override
    { return child == "." ? Result<fs::Node *>{this} : backing->lookup_child(child); }
    Result<fs::FileBuffer> read(usize offset, usize count) const override { return backing->read(offset,count); }
    Status write(usize offset,std::span<const std::byte> data) override { return backing->write(offset,data); }
    Status create(std::string_view name,fs::NodeType type) override { return backing->create(name,type); }
    Status chmod(u32 mode) override { return backing->chmod(mode); }
    Status chown(u32 uid,u32 gid) override { return backing->chown(uid,gid); }
    fs::ModeChangeResult change_mode(u32 mode,user::Credentials caller) override
    {
        const auto info=backing->query_metadata();
        if (!info) return fs::ModeChangeResult::failure(info.status());
        const auto decision=fs::mode_change_policy(info.value().uid,info.value().gid,mode,caller);
        return fs::ModeChangeResult::failure(decision.authorized ? backing->chmod(decision.permissions) :
                                             Status::permission("hosted directory owner required"));
    }
};

template <lu::Profile P>
void directory(Kernel &kernel, sched::ProcessId parent, usize number_slot, std::array<usize,6> slots)
{
    CHECK(tests::check_directory_codec<P>());
    constexpr uptr output = hw::user_begin + 128;
    sched::ProcessId pid = parent;
    auto fd = kernel.posix().open("/directory-model", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(fd);
    auto duplicate = kernel.posix().duplicate(fd.value()); CHECK(duplicate);
    auto position = [&] { return kernel.posix().seek(fd.value(), 0, posix::SeekWhence::current).value(); };
    auto reset = [&] { CHECK(kernel.posix().seek(fd.value(), 0, posix::SeekWhence::set)); };
    auto raw = [&](u64 descriptor, u64 pointer, u64 count) {
        auto number = lu::Codec<P>::syscall_number(lu::Operation::getdents64); CHECK(number);
        hw::UserFrame frame{}; frame.registers[number_slot] = *number;
        frame.registers[slots[0]] = descriptor; frame.registers[slots[1]] = pointer; frame.registers[slots[2]] = count;
        bool done = true;
        auto result = user::linux_bootstrap::dispatch<P>(kernel, pid, frame, done,
            [&](hw::Syscall request, bool &complete) { return native::dispatch(kernel, pid, request, complete); });
        CHECK(done && user::linux_bootstrap::complete<P>(frame, result));
        auto returned = lu::NativeBridge<P>::decode_return(frame); CHECK(returned);
        if (result >= 0) CHECK(returned.value().error == 0 && returned.value().value == static_cast<u64>(result));
        else
        {
            const auto error = static_cast<u32>(-result);
            const auto target_error = error == 38 && (P == lu::Profile::mips || P == lu::Profile::mips64) ? 89 : error;
            CHECK(returned.value().error == target_error);
        }
        return result;
    };
    auto word = [&](usize at, usize width) {
        u64 value = 0;
        for (usize i = 0; i < width; ++i)
        {
            const usize index = lu::Codec<P>::profile.big_endian ? at + i : at + width - i - 1;
            value = (value << 8) | std::to_integer<u8>(mock::memory[index]);
        }
        return value;
    };
    auto record = [&](usize at, std::string_view name, u64 inode, u64 next, u8 type) {
        CHECK(word(at, 8) == inode && word(at + 8, 8) == next);
        const auto size = word(at + 16, 2);
        CHECK(size == ((20 + name.size() + 7) & ~usize{7}));
        CHECK(std::to_integer<u8>(mock::memory[at + 18]) == type);
        CHECK(!std::memcmp(mock::memory.data() + at + 19, name.data(), name.size()));
        for (usize i = 19 + name.size(); i < size; ++i) CHECK(mock::memory[at + i] == std::byte{0});
        return size;
    };
    const auto self = kernel.posix().stat("/directory-model").value().attributes.inode;
    const auto root = kernel.posix().stat("/").value().attributes.inode;
    const auto child = kernel.posix().stat("/directory-model/file").value().attributes.inode;
    CHECK(self && root && child && self != child);
    mock::memory.fill(std::byte{0xa5});
    CHECK(raw(fd.value(), output, 4096) == 128 && position() == 5);
    usize at = 128;
    at += record(at, ".", self, 1, 4); at += record(at, "..", root, 2, 4);
    at += record(at, "file", child, 3, 8); at += record(at, "alias", child, 4, 8);
    const auto symlink = kernel.posix().statat(posix::at_FDCWD, "/directory-model/link", posix::at_SYMLINK_NOFOLLOW).value().attributes.inode;
    at += record(at, "link", symlink, 5, 10);
    CHECK(at == 256 && mock::memory[127] == std::byte{0xa5} && mock::memory[256] == std::byte{0xa5});
    const auto outputs = mock::outputs;
    CHECK(raw(fd.value(), 0, 0) == 0 && raw(fd.value(), 0, ~u32{0}) == 0 && mock::outputs == outputs);
    reset();
    CHECK(raw(~u32{0}, 0, 0) == -9);
    CHECK(raw(fd.value(), 0, 23) == -22 && position() == 0);
    CHECK(raw(fd.value(), output, 0) == -22 && raw(fd.value(), output, 0x80000000U) == -22);
    CHECK(raw(fd.value(), 0, 24) == -14 && position() == 0);
    // Linux getdents uses retained open-description authority after chmod,
    // while a fresh open or path traversal must consult the current mode.
    const auto saved_credentials=kernel.posix().user_credentials();
    const auto saved_directory=kernel.posix().fstat(fd.value());CHECK(saved_directory);
    CHECK(kernel.posix().chown("/directory-model",1000,1000));
    CHECK(kernel.posix().set_credentials(user::default_user_credentials()));
    CHECK(kernel.posix().credentials().uid==1000);
    CHECK(kernel.posix().change_mode_fd(fd.value(),0000).ok());
    CHECK(raw(fd.value(),output,24)==24 && position()==1);record(128,".",self,1,4);
    CHECK(raw(duplicate.value(),output,24)==24 && position()==2);record(128,"..",root,2,4);
    auto retained_child=kernel.posix().fork(parent);CHECK(retained_child && kernel.posix().activate(retained_child.value()));
    pid=retained_child.value();CHECK(raw(fd.value(),output,4096)==80 && position()==5);
    usize retained_at=128;retained_at+=record(retained_at,"file",child,3,8);
    retained_at+=record(retained_at,"alias",child,4,8);retained_at+=record(retained_at,"link",symlink,5,10);
    CHECK(retained_at==208);
    CHECK(kernel.posix().exit(pid,19) && kernel.posix().activate(parent));pid=parent;
    CHECK(kernel.posix().wait(parent,retained_child.value()).value()==19);
    CHECK(kernel.posix().open("/directory-model",posix::o_RDONLY|posix::o_DIRECTORY).status().code()==StatusCode::denied);
    CHECK(kernel.posix().openat(fd.value(),"file",posix::o_RDONLY).status().code()==StatusCode::denied);
    CHECK(kernel.posix().fchdir(fd.value()).code()==StatusCode::denied);
    CHECK(kernel.posix().change_mode_at(fd.value(),".",0700,0).status().code()==StatusCode::denied);
    CHECK(kernel.posix().fstat(fd.value()).value().mode==0040000);
    reset();
    std::array<std::byte,256> retained_native{};
    CHECK(kernel.posix().getdents64(duplicate.value(),retained_native).value()==80 && position()==3);
    auto *retained_slot=kernel.scheduler().find(pid)->fd_table().entry(fd.value());
    auto *retained_description=kernel.scheduler().open_description(retained_slot->description);
    CHECK(retained_description && retained_description->readable);
    retained_description->readable=false;reset();const auto no_outputs=mock::outputs;
    CHECK(raw(fd.value(),output,4096)==-9 && position()==0 && mock::outputs==no_outputs);
    CHECK(kernel.posix().getdents64(duplicate.value(),retained_native).status().code()==StatusCode::bad_descriptor);
    retained_description->readable=true;
    CHECK(kernel.posix().change_mode_fd(duplicate.value(),0700).ok());
    CHECK(kernel.posix().stat("/directory-model/file"));
    CHECK(kernel.posix().set_credentials(saved_credentials));
    CHECK(kernel.posix().chown("/directory-model",saved_directory.value().uid,saved_directory.value().gid));
    const auto normal = kernel.posix().open("/directory-model/file", posix::o_RDONLY); CHECK(normal);
    CHECK(raw(normal.value(), 0, 0) == -20 && raw(0, 0, 0) == -20); CHECK(kernel.posix().close(normal.value()));
    auto pipes = kernel.posix().pipe(); CHECK(pipes);
    for (const auto pipe : pipes.value()) { CHECK(raw(pipe, 0, 0) == -20); CHECK(kernel.posix().close(pipe)); }
    CHECK(raw(fd.value(), output, (u64{1} << 32) | 24) == 24 && position() == 1);
    CHECK(raw(duplicate.value(), output, 24) == 24 && position() == 2);
    record(128, "..", root, 2, 4);
    auto seek_end = [&](bool wide) {
        auto number = lu::Codec<P>::syscall_number(wide ? lu::Operation::_llseek : lu::Operation::lseek);
        if (!number) return;
        hw::UserFrame frame{}; frame.registers[number_slot] = *number;
        frame.registers[slots[0]] = fd.value();
        if (wide)
        {
            frame.registers[slots[3]] = output;
            if constexpr (P == lu::Profile::mips)
            {
                frame.sp = hw::user_begin + 4096;
                const auto stack = 4096 + 16;
                mock::memory[stack] = std::byte{0}; mock::memory[stack+1] = std::byte{0};
                mock::memory[stack+2] = std::byte{0}; mock::memory[stack+3] = std::byte{2};
            }
            else frame.registers[slots[4]] = 2;
        }
        else frame.registers[slots[2]] = 2;
        bool done = true;
        CHECK(user::linux_bootstrap::dispatch<P>(kernel, pid, frame, done,
            [&](hw::Syscall request, bool &complete) { return native::dispatch(kernel, pid, request, complete); }) == -22);
        CHECK(position() == 2);
    };
    seek_end(false); seek_end(true);
    reset();
    mock::forbidden = output + 24;
    CHECK(raw(fd.value(), output, 4096) == 24 && position() == 1);
    mock::forbidden = 0;
    CHECK(raw(fd.value(), output, 24) == 24); record(128, "..", root, 2, 4);
    CHECK(kernel.posix().seek(fd.value(), 3, posix::SeekWhence::set));
    CHECK(raw(fd.value(), output, 32) == 32); record(128, "alias", child, 4, 8);
    reset();
    auto forked = kernel.posix().fork(parent); CHECK(forked && kernel.posix().activate(forked.value())); pid = forked.value();
    CHECK(raw(fd.value(), output, 24) == 24);
    CHECK(kernel.posix().exit(pid, 17) && kernel.posix().activate(parent)); pid = parent;
    CHECK(kernel.posix().wait(parent, forked.value()).value() == 17 && position() == 1);
    CHECK(raw(duplicate.value(), output, 24) == 24); record(128, "..", root, 2, 4);
    auto separate = kernel.posix().open("/directory-model", posix::o_RDONLY | posix::o_DIRECTORY); CHECK(separate);
    CHECK(raw(separate.value(), output, 24) == 24); record(128, ".", self, 1, 4);
    CHECK(position() == 2 && kernel.posix().close(separate.value()));
    reset();
    std::array<std::byte, 256> legacy{};
    CHECK(kernel.posix().getdents64(fd.value(), legacy).value() == 80 && position() == 3);
    CHECK(kernel.posix().close(fd.value()) && kernel.posix().close(duplicate.value()));

    // Actual adapter calls against a fallible provider: failures are not EOF.
    const auto checked = kernel.posix().open("/directory-faults", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(checked);
    auto *slot = kernel.scheduler().find(pid)->fd_table().entry(checked.value());
    auto *description = kernel.scheduler().open_description(slot->description);
    const auto original = fault_provider.info;
    const auto original_parent = fault_parent.info;
    fault_provider.parent = &fault_parent;
    auto rejects = [&](i64 error) {
        mock::memory.fill(std::byte{0xa5});
        const auto before = mock::outputs;
        CHECK(raw(checked.value(), output, 4096) == error && description->offset == 0);
        CHECK(mock::outputs == before);
        for (auto byte : mock::memory) CHECK(byte == std::byte{0xa5});
    };
    fault_provider.fail_query = true; rejects(-5); fault_provider.fail_query = false;
    fault_provider.fail_list = true; rejects(-5); fault_provider.fail_list = false;
    fault_provider.entries.count = fs::max_child_nodes + 1; rejects(-5); fault_provider.entries.count = 0;
    fault_provider.fail_parent = true; rejects(-5); fault_provider.fail_parent = false;
    fault_provider.parent = nullptr; rejects(-5); fault_provider.parent = &fault_parent;
    fault_parent.fail_query = true; rejects(-5); fault_parent.fail_query = false;
    fault_parent.info.type = fs::NodeType::regular; rejects(-5); fault_parent.info = original_parent;
    fault_provider.info.attributes.valid = 0; rejects(-38); fault_provider.info = original;
    fault_parent.info.attributes.inode = 0; rejects(-38); fault_parent.info = original_parent;

    auto &bad = fault_provider.entries.entries[0];
    fault_provider.entries.count = 1;
    bad.metadata = original;
    for (const auto name : {std::string_view{}, std::string_view{"a/b"}, std::string_view{"."},
                            std::string_view{".."}, std::string_view{"a\0b", 3}})
    {
        CHECK(bad.name.assign(name));
        CHECK(raw(checked.value(), output, 4096) == 48 && description->offset == 2);
        const auto before = mock::outputs;
        CHECK(raw(checked.value(), output, 4096) == -5 && description->offset == 2 && before == mock::outputs);
        description->offset = 0;
    }
    CHECK(bad.name.assign("missing-inode")); bad.metadata.attributes.valid = 0;
    CHECK(raw(checked.value(), output, 4096) == 48 && description->offset == 2);
    CHECK(raw(checked.value(), output, 4096) == -38 && description->offset == 2);
    fault_provider.entries = {}; description->offset = 0;
    mock::partial_output = 3;
    CHECK(raw(checked.value(), output, 4096) == -14 && description->offset == 0);
    mock::partial_output = 0;
    CHECK(raw(checked.value(), output, 4096) == 48 && description->offset == 2);
    CHECK(kernel.posix().close(checked.value()));
    std::printf("PASS Linux directory %s: endian records, inode identities, cookies, dup/fork, errors and copy boundaries\n", lu::Codec<P>::profile.name);
}

int main()
{
    static Kernel kernel; CHECK(kernel.boot({}));
    auto made = kernel.scheduler().create_user_process("directory-adapter", kernel.arch().make_user_context({hw::user_begin, hw::user_end-4096}));
    CHECK(made && kernel.posix().activate(made.value())); const auto pid = made.value();
    CHECK(kernel.posix().mkdir("/directory-faults", 0700));
    CHECK(kernel.vfs().mount("/directory-faults", fault_provider));
    CHECK(kernel.posix().mkdir("/directory-model", 0700));
    auto file = kernel.posix().open("/directory-model/file", posix::o_CREAT | posix::o_RDWR, 0600); CHECK(file);
    CHECK(kernel.posix().close(file.value()) && kernel.posix().link("/directory-model/file", "/directory-model/alias"));
    CHECK(kernel.posix().symlinkat("file", posix::at_FDCWD, "/directory-model/link"));
    static RetainedDirectory retained_directory;
    retained_directory.backing=kernel.vfs().lookup("/directory-model"); CHECK(retained_directory.backing);
    CHECK(kernel.vfs().mount("/directory-model",retained_directory));
    directory<lu::Profile::i386>(kernel,pid,7,{4,6,5,1,0,2});
    directory<lu::Profile::x86_64>(kernel,pid,14,{9,10,11,5,7,6});
    directory<lu::Profile::aarch64>(kernel,pid,8,{0,1,2,3,4,5});
    directory<lu::Profile::arm32>(kernel,pid,7,{0,1,2,3,4,5});
    directory<lu::Profile::rv64>(kernel,pid,17,{10,11,12,13,14,15});
    directory<lu::Profile::rv32>(kernel,pid,17,{10,11,12,13,14,15});
    directory<lu::Profile::loongarch64>(kernel,pid,11,{4,5,6,7,8,9});
    directory<lu::Profile::mips>(kernel,pid,2,{4,5,6,7,0,0});
    directory<lu::Profile::mips64>(kernel,pid,2,{4,5,6,7,8,9});
    directory<lu::Profile::ppc>(kernel,pid,0,{3,4,5,6,7,8});
}
