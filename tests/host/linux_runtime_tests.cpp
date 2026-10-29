#include "unavailable_realtime.hpp"
// Hosted runtime integration: no guest CPU execution.
#include "native_runtime_units.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace ok;
namespace native = ok::user::native;
namespace hw = ok::arch::execution;
class HostedEntropy final : public driver::EntropySource
{
  public:
    Status fill(std::span<std::byte> out) override
    {
        for (auto &byte : out)
            byte = std::byte{static_cast<u8>(++next_)};
        return Status::success();
    }
  private:
    u8 next_{};
};
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
struct Space
{
    bool live{};
    u32 generation{};
    hw::MappingView pages;
};
std::array<Space, 16> spaces; // Fixed model slot table.
bool fail_create{};
uptr expect_clear{};
const hw::PageMapping *page(const hw::AddressSpace &space, uptr address)
{
    if (!space.root || space.slot >= spaces.size() || !spaces[space.slot].live ||
        spaces[space.slot].generation != space.generation)
        return nullptr;
    for (const auto &p : spaces[space.slot].pages)
        if (p.virtual_address == (address & ~uptr(hw::page_size - 1)))
            return &p;
    return nullptr;
}
usize frames()
{
    return native::page_pool.used_count();
}
usize live()
{
    usize n = 0;
    for (const auto &s : spaces)
        n += s.live;
    return n;
}
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
Syscall syscall(const UserFrame &)
{
    return {};
}
UserFrame initial_frame(uptr pc, uptr sp)
{
    UserFrame f{};
    f.pc = pc;
    f.sp = sp;
    return f;
}
void set_return_value(UserFrame &f, i64 value)
{
    f.registers[0] = static_cast<uptr>(value);
}
Result<AddressSpace> create_space(MappingView pages)
{
    if (mock::fail_create)
        return Status::no_memory();
    for (usize i = 0; i < mock::spaces.size(); ++i)
    {
        auto &s = mock::spaces[i];
        if (s.live)
            continue;
        s.pages = pages;
        s.live = true;
        ++s.generation;
        return AddressSpace{i + 1, static_cast<u32>(i), s.generation};
    }
    return Status::no_memory();
}
bool user_range(const AddressSpace &space, uptr address, usize size, bool write)
{
    if (!size)
        return true;
    if (address < hw::user_begin || address >= hw::user_end || size > hw::user_end - address)
        return false;
    while (size)
    {
        auto *p = mock::page(space, address);
        if (!p || !(p->flags & (write ? page_write : page_read)))
            return false;
        usize n = hw::page_size - (address % hw::page_size);
        if (n > size)
            n = size;
        address += n;
        size -= n;
    }
    return true;
}
Status copy_from_user(const AddressSpace &space, void *out, uptr address, usize size)
{
    if (!user_range(space, address, size, false))
        return Status::fault();
    auto *dst = static_cast<std::byte *>(out);
    while (size)
    {
        auto *p = mock::page(space, address);
        usize off = address % hw::page_size, n = hw::page_size - off;
        if (n > size)
            n = size;
        std::memcpy(dst, reinterpret_cast<void *>(p->physical_address + off), n);
        dst += n;
        address += n;
        size -= n;
    }
    return Status::success();
}
Status copy_to_user(const AddressSpace &space, uptr address, const void *in, usize size)
{
    if (!user_range(space, address, size, true))
        return Status::fault();
    const auto *src = static_cast<const std::byte *>(in);
    while (size)
    {
        auto *p = mock::page(space, address);
        usize off = address % hw::page_size, n = hw::page_size - off;
        if (n > size)
            n = size;
        std::memcpy(reinterpret_cast<void *>(p->physical_address + off), src, n);
        src += n;
        address += n;
        size -= n;
    }
    return Status::success();
}
Status destroy_space(AddressSpace &space)
{
    CHECK(space.root && mock::spaces[space.slot].live);
    if (mock::expect_clear)
    {
        u32 v = 1;
        CHECK(copy_from_user(space, &v, mock::expect_clear, sizeof(v)));
        CHECK(v == 0);
        mock::expect_clear = 0;
    }
    mock::spaces[space.slot].live = false;
    mock::spaces[space.slot].pages = {};
    space = {};
    return Status::success();
}
} // namespace ok::arch::execution
std::vector<std::byte> read_file(const char *path)
{
    auto *f = std::fopen(path, "rb");
    CHECK(f);
    CHECK(std::fseek(f, 0, SEEK_END) == 0);
    auto n = std::ftell(f);
    CHECK(n > 0);
    CHECK(std::fseek(f, 0, SEEK_SET) == 0);
    std::vector<std::byte> b(static_cast<usize>(n));
    CHECK(std::fread(b.data(), 1, b.size(), f) == b.size());
    std::fclose(f);
    return b;
}
uptr word(const hw::AddressSpace &space, uptr address)
{
    uptr v{};
    CHECK(hw::copy_from_user(space, &v, address, sizeof(v)));
    return v;
}
class FaultingExecutable final : public fs::Node
{
public:
    fs::Node *backing{}, *child{};
    bool directory{}, fail_read{}, fail_release{};
    mutable usize queries{};
    usize fail_query{}, pins{}, releases{};
    std::string_view name() const override { return "fault-executable"; }
    fs::Metadata metadata() const override
    {
        return directory ? fs::Metadata{.type=fs::NodeType::directory,.mode=fs::mode_for(fs::NodeType::directory,0755)}
                         : backing->metadata();
    }
    Result<fs::Metadata> query_metadata() const override
    {
        if (++queries == fail_query) return Status::io_error("exec metadata failure");
        return metadata();
    }
    Result<fs::FileBuffer> read(usize offset, usize count) const override
    {
        return fail_read ? Result<fs::FileBuffer>{Status::denied("exec read failure")} : backing->read(offset,count);
    }
    fs::Node *lookup(std::string_view name) override { return name=="image" ? child : nullptr; }
    Status retain() override { ++pins; return Status::success(); }
    Status release() override
    {
        CHECK(pins); --pins; ++releases;
        return fail_release ? Status::io_error("exec cleanup failure") : Status::success();
    }
    Status write(usize,std::span<const std::byte>) override { return Status::unsupported(); }
    Status create(std::string_view,fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32,u32) override { return Status::unsupported(); }
};
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    static Kernel kernel;
    CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(kernel.posix().activate(0));
    HostedEntropy entropy;
    kernel.posix().set_entropy_source(&entropy);
    auto linux_bytes = read_file(argv[1]);
    auto native_bytes = linux_bytes, invalid_bytes = linux_bytes;
    native_bytes[7] = std::byte{0};
    invalid_bytes[7] = std::byte{9};
    CHECK(kernel.vfs().create_readonly_file("/linux", linux_bytes));
    CHECK(kernel.vfs().create_readonly_file("/native", native_bytes));
    CHECK(kernel.vfs().create_readonly_file("/bad", invalid_bytes));
    native::Arguments arguments;
    for (int i = 0; i < 16; ++i)
    {
        CHECK(arguments.argv.push_back(FixedString<256>("argument")));
        CHECK(arguments.envp.push_back(FixedString<256>("ENV=value")));
    }
    FaultingExecutable fault_root, fault_file;
    fault_root.directory=true;fault_root.child=&fault_file;
    fault_file.backing=kernel.vfs().lookup("/linux");CHECK(fault_file.backing);
    CHECK(kernel.vfs().create("/fault",fs::NodeType::directory));
    CHECK(kernel.vfs().mount("/fault",fault_root));
    fault_file.fail_query=2;fault_file.fail_release=true;
    auto rejected=native::load(kernel,"/fault/image",arguments);
    CHECK(!rejected && rejected.status().message()=="exec metadata failure");
    CHECK(mock::frames()==0 && mock::live()==0 && fault_file.pins==0 && fault_file.releases==1);
    fault_file.fail_query=0;
    rejected=native::load(kernel,"/fault/image",arguments);
    CHECK(!rejected && rejected.status().message()=="exec cleanup failure");
    CHECK(mock::frames()==0 && mock::live()==0 && fault_file.pins==0 && fault_file.releases==2);
    fault_file.fail_read=true;
    rejected=native::load(kernel,"/fault/image",arguments);
    CHECK(!rejected && rejected.status().message()=="exec read failure");
    CHECK(mock::frames()==0 && mock::live()==0 && fault_file.pins==0 && fault_file.releases==3);
    fault_file.fail_read=fault_file.fail_release=false;
    auto recovered=native::load(kernel,"/fault/image",arguments);CHECK(recovered);
    CHECK(fault_file.pins==0 && fault_file.releases==4);
    native::release(recovered.value().space,recovered.value().pages);
    CHECK(mock::frames()==0 && mock::live()==0);
    auto image = native::load(kernel, "/linux", arguments);
    CHECK(image);
    CHECK(image.value().personality == sched::UserPersonality::linux);
    auto &loaded = image.value();
    CHECK((loaded.frame.sp & 15) == 0);
    CHECK(word(loaded.space, loaded.frame.sp) == 16);
    uptr cursor = loaded.frame.sp + sizeof(uptr);
    for (int i = 0; i < 16; ++i, cursor += sizeof(uptr))
    {
        char text[9]{};
        CHECK(hw::copy_from_user(loaded.space, text, word(loaded.space, cursor), sizeof(text)));
        CHECK(std::strcmp(text, "argument") == 0);
    }
    CHECK(word(loaded.space, cursor) == 0);
    cursor += sizeof(uptr);
    for (int i = 0; i < 16; ++i, cursor += sizeof(uptr))
    {
        char text[10]{};
        CHECK(hw::copy_from_user(loaded.space, text, word(loaded.space, cursor), sizeof(text)));
        CHECK(std::strcmp(text, "ENV=value") == 0);
    }
    CHECK(word(loaded.space, cursor) == 0);
    cursor += sizeof(uptr);
    const uptr keys[]{3, 4, 5, 6, 9, 11, 12, 13, 14, 23, 31, 25, 0};
    for (auto key : keys)
    {
        CHECK(word(loaded.space, cursor) == key);
        auto value = word(loaded.space, cursor + sizeof(uptr));
        if (key == 3)
            CHECK(value == hw::user_begin + 64);
        if (key == 4)
            CHECK(value == 56);
        if (key == 5)
            CHECK(value == 4);
        if (key == 6)
            CHECK(value == 4096);
        if (key == 9)
            CHECK(value == loaded.frame.pc);
        if (key == 31)
        {
            char text[7]{};
            CHECK(hw::copy_from_user(loaded.space, text, value, sizeof(text)));
            CHECK(std::strcmp(text, "/linux") == 0);
        }
        if (key == 25)
        {
            std::array<std::byte, 16> random{};
            CHECK(hw::copy_from_user(loaded.space, random.data(), value, random.size()));
            CHECK(random[0] != std::byte{} && random[15] != std::byte{});
        }
        cursor += 2 * sizeof(uptr);
    }
    native::release(loaded.space, loaded.pages);
    CHECK(mock::frames() == 0 && mock::live() == 0);
    auto original = native::load(kernel, "/native", arguments);
    CHECK(original);
    auto made = kernel.scheduler().create_user_process(
        "staged", kernel.arch().make_user_context({original.value().frame.pc, original.value().frame.sp}));
    CHECK(made);
    const auto pid = made.value();
    auto *process = kernel.scheduler().find(pid);
    CHECK(native::prepare_native_thread(process->threads()[0]));
    native::install(*process, original.value());
    CHECK(kernel.posix().activate(pid));
    auto &resources = process->resources();
    auto &thread = process->threads()[0];
    CHECK(resources.program_break == original.value().program_break &&
          resources.program_break_base == original.value().program_break);
    const uptr initial_break = resources.program_break;
    CHECK(native::native_brk(resources, initial_break + 4097));
    thread.tls.fs_base = 123;
    const auto before = resources.native_space;
    const auto before_frames = mock::frames();
    const uptr scratch = thread.native_frame.sp - 1024;
    const char bad[] = "/bad";
    CHECK(hw::copy_to_user(resources.native_space, scratch, bad, sizeof(bad)));
    bool complete = true;
    CHECK(native::dispatch(kernel, pid, {OK_SYS_EXECVE, {scratch, 0, 0, 0, 0, 0}}, complete) == -38);
    CHECK(complete && resources.personality == sched::UserPersonality::native &&
          resources.native_space.root == before.root && resources.native_space.generation == before.generation &&
          mock::frames() == before_frames && thread.tls.fs_base == 123);
    CHECK(resources.program_break == initial_break + 4097);
    const char good[] = "/linux";
    CHECK(hw::copy_to_user(resources.native_space, scratch, good, sizeof(good)));
    mock::fail_create = true;
    CHECK(native::dispatch(kernel, pid, {OK_SYS_EXECVE, {scratch, 0, 0, 0, 0, 0}}, complete) == -12);
    mock::fail_create = false;
    CHECK(complete && resources.personality == sched::UserPersonality::native &&
          resources.native_space.root == before.root && resources.native_space.generation == before.generation &&
          mock::frames() == before_frames && thread.tls.fs_base == 123);
    CHECK(native::dispatch(kernel, pid, {OK_SYS_EXECVE, {scratch, 0, 0, 0, 0, 0}}, complete) == 0 && !complete);
    CHECK(resources.personality == sched::UserPersonality::linux && thread.tls.fs_base == 0 && thread.native_active &&
          !thread.syscall_pending && mock::live() == 1);
    CHECK(resources.program_break == initial_break && resources.program_break_base == initial_break);
    // Successful exec returns the new image to the scheduler before its next syscall.
    CHECK(kernel.scheduler().current_thread() == nullptr && thread.state == sched::ProcessState::runnable);
    CHECK(kernel.posix().activate(pid));
    auto shared = [&](hw::Syscall call, bool &done) { return native::dispatch(kernel, pid, call, done); };
    CHECK(kernel.vfs().create("/mmap-file", fs::NodeType::regular));
    auto *mapping_file = kernel.vfs().lookup("/mmap-file");
    CHECK(mapping_file);
    std::array<std::byte, hw::page_size> mapping_bytes;
    mapping_bytes.fill(std::byte{0x4a});
    CHECK(mapping_file->write(0, std::span<const std::byte>{mapping_bytes}));
    auto write_only = kernel.posix().open("/mmap-file", OK_O_WRONLY);
    auto read_only = kernel.posix().open("/mmap-file", OK_O_RDONLY);
    CHECK(write_only && read_only);
    auto linux_mmap = [&](uptr fd, uptr flags, uptr hint = 0) {
        thread.native_frame.registers[14] = 9; // x86_64 mmap
        thread.native_frame.registers[9] = hint;
        thread.native_frame.registers[10] = 1; // requested length
        thread.native_frame.registers[11] = flags & 0x20 ? 3 : 1; // Writable anonymous pages.
        thread.native_frame.registers[5] = flags;
        thread.native_frame.registers[7] = fd;
        thread.native_frame.registers[6] = 0;  // file offset
        complete = true;
        return user::linux_bootstrap::dispatch<abi::linux_user::Profile::x86_64>(
            kernel, pid, thread.native_frame, complete, shared);
    };
    CHECK(linux_mmap(~uptr{0}, 2) == -9);
    CHECK(linux_mmap(static_cast<uptr>(write_only.value()), 2) == -13);
    const auto file_map = linux_mmap(static_cast<uptr>(read_only.value()), 2);
    CHECK(file_map > 0);
    u8 last_file_byte = 0;
    CHECK(hw::copy_from_user(resources.native_space, &last_file_byte,
                             static_cast<uptr>(file_map) + hw::page_size - 1, 1));
    CHECK(last_file_byte == 0x4a);
    CHECK(native::native_munmap(resources, static_cast<uptr>(file_map), 1));
    CHECK(linux_mmap(static_cast<uptr>(read_only.value()), 1) == -38);
    CHECK(linux_mmap(~uptr{0}, 3 | 0x20) == -22);
    const auto shared_map = linux_mmap(~uptr{0}, 1 | 0x20);
    CHECK(shared_map > 0);
    bool shared_frame = false;
    uptr shared_backing = 0;
    for (const auto &page : resources.native_pages)
        if (page.virtual_address == static_cast<uptr>(shared_map))
        {
            shared_frame = native::shared_pool_page(page);
            shared_backing = page.physical_address;
        }
    CHECK(shared_frame && shared_backing);
    const u8 shared_byte = 0x5a;
    CHECK(hw::copy_to_user(resources.native_space, static_cast<uptr>(shared_map), &shared_byte, 1));
    const auto shared_space = resources.native_space;
    const auto shared_frames = mock::frames();
    mock::fail_create = true;
    CHECK(linux_mmap(~uptr{0}, 1 | 0x20) == -12);
    CHECK(linux_mmap(~uptr{0}, 1 | 0x20 | 0x10, static_cast<uptr>(shared_map)) == -12);
    CHECK(native::dispatch(kernel, pid, {OK_SYS_FORK, {}}, complete) == -12);
    mock::fail_create = false;
    u8 retained_byte = 0;
    CHECK(resources.native_space.root == shared_space.root &&
          resources.native_space.generation == shared_space.generation &&
          mock::frames() == shared_frames && native::page_pool.reference_count(shared_backing) == 1 &&
          hw::copy_from_user(resources.native_space, &retained_byte, static_cast<uptr>(shared_map), 1) &&
          retained_byte == shared_byte);
    CHECK(native::native_munmap(resources, static_cast<uptr>(shared_map), 1));
    CHECK(kernel.posix().close(write_only.value()) && kernel.posix().close(read_only.value()));
    const uptr tid_address = thread.native_frame.sp - 64;
    u32 tid = 99;
    CHECK(hw::copy_to_user(resources.native_space, tid_address, &tid, sizeof(tid)));
    thread.native_frame.registers[14] = 218;
    thread.native_frame.registers[9] = tid_address;
    // Linux failure returns the current break, while the native request retains
    // the explicit error contract. Both share the same actual mapped-page path.
    thread.native_frame.registers[14] = 12;
    thread.native_frame.registers[9] = initial_break - 1;
    CHECK(user::linux_bootstrap::dispatch<abi::linux_user::Profile::x86_64>(kernel, pid, thread.native_frame, complete,
                                                                            shared) == static_cast<i64>(initial_break));
    thread.native_frame.registers[9] = initial_break + 1;
    CHECK(user::linux_bootstrap::dispatch<abi::linux_user::Profile::x86_64>(kernel, pid, thread.native_frame, complete,
                                                                            shared) == static_cast<i64>(initial_break + 1));
    CHECK(resources.program_break == initial_break + 1);
    thread.native_frame.registers[14] = 218;
    thread.native_frame.registers[9] = tid_address;
    complete = true;
    // The leader's task ID is its PID/TGID in the common identity domain.
    CHECK(user::linux_bootstrap::dispatch<abi::linux_user::Profile::x86_64>(kernel, pid, thread.native_frame, complete,
                                                                            shared) == static_cast<i64>(pid));
    CHECK(thread.tls.clear_tid_address == tid_address);
    CHECK(native::set_result(*process, thread.native_frame, -9));
    CHECK(thread.native_frame.registers[14] == static_cast<uptr>(-9));
    // Linux clears a TID word only when another address-space user remains.
    CHECK(kernel.scheduler().create_thread_prepared(pid,thread.context,
        [](sched::ThreadControlBlock &created) { return native::prepare_native_thread(created); }));
    mock::expect_clear = tid_address;
    native::release_process(kernel, pid);
    CHECK(mock::expect_clear == 0 && mock::frames() == 0 && mock::live() == 0 && thread.tls.clear_tid_address == 0);
    native::release_process(kernel, pid);
    CHECK(mock::frames() == 0 && mock::live() == 0);
    const auto stacks = native::entry_stacks.accounting();
    CHECK(!stacks.leased_slots && !stacks.active_slots && !stacks.quarantined_slots);
    CHECK(stacks.reservations == stacks.releases && native::runtime_status());
    std::puts("PASS hosted Linux runtime: 16 argv/envp, mapped PHDR/auxv/execfn, admission, failed exec rollback, "
              "personality install, file mmap short page/EBADF/EACCES, shared mmap rollback, errno completion, exit clear-tid before teardown");
}
