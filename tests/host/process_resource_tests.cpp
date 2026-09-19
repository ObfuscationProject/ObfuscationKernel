#include "ok/posix/posix.hpp"
#include "ok/sched/process.hpp"
#include "ok/syscall/linux.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

using namespace ok;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!static_cast<bool>(condition))                                                                             \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                       \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

static_assert(std::is_same_v<sched::Process, sched::ProcessControlBlock>);
static_assert(std::is_same_v<sched::Thread, sched::ThreadControlBlock>);

std::span<const std::byte> bytes(std::string_view value)
{
    return {reinterpret_cast<const std::byte *>(value.data()), value.size()};
}

struct Fixture
{
    sched::Scheduler scheduler;
    fs::VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService posix;
    Fixture()
    {
        CHECK(console.start());
        CHECK(posix.initialize(vfs, console, scheduler));
        CHECK(vfs.create("/a", fs::NodeType::directory));
        CHECK(vfs.create("/b", fs::NodeType::directory));
        CHECK(vfs.create("/data", fs::NodeType::regular));
        CHECK(vfs.chmod("/data", 0666));
        CHECK(vfs.write_file("/data", bytes("abcdefghij")));
    }
    sched::ProcessId create(std::string_view name)
    {
        auto result = scheduler.create_user_process(
            name, arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0}));
        CHECK(result);
        CHECK(scheduler.set_runnable(result.value()));
        return result.value();
    }
};

void test_unique_owner_and_isolation()
{
    static Fixture f;
    sched::ProcessManager lifecycle(f.scheduler);
    const auto a = f.create("alpha");
    const auto b = f.create("beta");
    CHECK(lifecycle.find(a) == f.scheduler.find(a));
    CHECK(lifecycle.process_count() == f.scheduler.process_count());
    CHECK(f.posix.activate(a));
    CHECK(f.posix.chdir("/a"));
    CHECK(f.posix.set_identity(1000, 1001));
    CHECK(f.posix.umask(0077) == 0022);
    CHECK(f.posix.brk(0x10001000));
    CHECK(f.posix.arch_prctl(posix::arch_SET_FS, 0x12340000));
    auto fd = f.posix.open("/data", posix::o_RDWR);
    CHECK(fd && fd.value() == 3);
    auto map = f.posix.mmap(0, 4096, posix::prot_READ, posix::map_ANONYMOUS, -1, 0);
    CHECK(map);
    CHECK(f.posix.activate(b));
    CHECK(f.posix.getcwd() == "/");
    CHECK(f.posix.getuid() != 1000 || f.posix.getgid() != 1001);
    CHECK(f.posix.open_file_count() == 3);
    CHECK(f.posix.brk(0).value() == 0x10000000);
    CHECK(f.posix.umask(0022) == 0022);
    uptr tls = 1;
    CHECK(f.posix.arch_prctl(posix::arch_GET_FS, reinterpret_cast<uptr>(&tls)));
    CHECK(tls == 0);
    CHECK(!f.posix.munmap(map.value(), 4096));
    CHECK(f.posix.activate(a));
    CHECK(f.posix.getcwd() == "/a");
    CHECK(f.posix.getuid() == 1000 && f.posix.getgid() == 1001);
    CHECK(f.posix.brk(0).value() == 0x10001000);
    CHECK(f.posix.arch_prctl(posix::arch_GET_FS, reinterpret_cast<uptr>(&tls)));
    CHECK(tls == 0x12340000);
    CHECK(f.posix.munmap(map.value(), 4096));
    CHECK(f.posix.close(fd.value()));
}

void test_fork_dup_offsets_and_lifetime()
{
    static Fixture f;
    const auto parent = f.create("parent");
    CHECK(f.posix.activate(parent));
    auto fd = f.posix.open("/data", posix::o_RDWR | posix::o_CLOEXEC);
    CHECK(fd);
    auto &owned = f.scheduler.find(parent)->fd_table();
    sched::FileDescriptorTable metadata;
    CHECK(metadata.open_slot(7));
    CHECK(!metadata.clone_from(owned) && metadata.contains(7) && metadata.open_count() == 1);
    CHECK(!owned.clone_from(metadata) && owned.contains(fd.value()) && owned.contains(0));
    CHECK(!owned.close_slot(fd.value()) && owned.contains(fd.value()));
    CHECK(f.scheduler.open_description_count() == 4);
    sched::FileDescriptorTable copied_metadata;
    CHECK(copied_metadata.clone_from(metadata) && copied_metadata.contains(7));
    CHECK(copied_metadata.close_slot(7) && copied_metadata.open_count() == 0);
    auto duplicate = f.posix.duplicate(fd.value());
    CHECK(duplicate);
    CHECK(f.posix.fcntl(duplicate.value(), posix::f_GETFD, 0).value() == 0);
    CHECK(f.posix.fcntl(fd.value(), posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
    std::array<std::byte, 2> out{};
    CHECK(f.posix.read(fd.value(), out).value() == 2 && out[0] == std::byte{'a'});
    CHECK(f.posix.read(duplicate.value(), out).value() == 2 && out[0] == std::byte{'c'});
    CHECK(f.posix.chdir("/a"));
    auto child = f.posix.fork();
    CHECK(child);
    CHECK(f.posix.activate(child.value()));
    CHECK(f.posix.getppid() == parent && f.posix.getcwd() == "/a");
    CHECK(f.posix.read(fd.value(), out).value() == 2 && out[0] == std::byte{'e'});
    CHECK(f.posix.chdir("/b"));
    CHECK(f.posix.close(fd.value()));
    CHECK(f.posix.activate(parent));
    CHECK(f.posix.getcwd() == "/a");
    CHECK(f.posix.read(fd.value(), out).value() == 2 && out[0] == std::byte{'g'});
    CHECK(f.posix.close(fd.value()));
    CHECK(f.posix.close(duplicate.value()));
    CHECK(f.posix.activate(child.value()));
    CHECK(f.posix.read(duplicate.value(), out).value() == 2 && out[0] == std::byte{'i'});
    CHECK(f.scheduler.open_description_count() == 4);
    CHECK(f.posix.exit(17));
    CHECK(f.scheduler.open_description_count() == 3);
    CHECK(f.posix.exit(child.value(), 99).code() == StatusCode::already_exists);
    CHECK(f.scheduler.set_runnable(child.value()).code() == StatusCode::invalid_argument);
    CHECK(f.posix.activate(parent));
    auto waited = f.posix.wait(parent, child.value());
    CHECK(waited && waited.value() == 17);
    CHECK(!f.posix.wait(parent, child.value()));
    CHECK(f.scheduler.find(child.value()) == nullptr);
    CHECK(!f.posix.duplicate_to(31, 31));
    CHECK(f.posix.close(0));
    auto redirected = f.posix.open("/data", posix::o_RDONLY);
    CHECK(redirected && redirected.value() == 0);
    CHECK(f.posix.read(0, out).value() == 2 && out[0] == std::byte{'a'});
}

void test_mounted_paths_and_directory_descriptors()
{
    static Fixture f;
    static fs::VirtualFileSystem mounted;
    static fs::VirtualFileSystem nested;
    CHECK(mounted.create("/inside", fs::NodeType::regular));
    CHECK(mounted.create("/nested", fs::NodeType::directory));
    CHECK(nested.create("/deep", fs::NodeType::regular));
    const auto pid = f.create("mounts");
    CHECK(f.posix.activate(pid));
    auto covered = f.posix.open("/a", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(covered);
    CHECK(!f.vfs.mount("/./a", *mounted.lookup("/")));
    CHECK(f.vfs.mount("/a", *mounted.lookup("/")));
    CHECK(f.vfs.lookup("/a/inside") == mounted.lookup("/inside"));
    CHECK(f.vfs.lookup("/a/../data") == f.vfs.lookup("/data"));
    CHECK(!f.vfs.lookup("/missing/../data"));
    CHECK(!f.vfs.lookup("/data/../a"));
    CHECK(f.vfs.mount("/a/nested", *nested.lookup("/")));
    CHECK(f.vfs.lookup("/a/nested/deep") == nested.lookup("/deep"));
    CHECK(f.vfs.lookup("/a/nested/../inside") == mounted.lookup("/inside"));
    CHECK(f.vfs.rmdir("/a").code() == StatusCode::busy);
    CHECK(f.vfs.rmdir("/a/nested").code() == StatusCode::busy);
    std::array<std::byte, 512> output{};
    // Opening before a mount keeps the covered directory, not its old path.
    CHECK(f.posix.getdents64(covered.value(), output).value() == 0);
    auto directory = f.posix.open("/a", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(directory);
    auto count = f.posix.getdents64(directory.value(), output);
    CHECK(count && count.value() >= 24);
    u64 ino, offset;
    u16 reclen;
    std::memcpy(&ino, output.data(), sizeof(ino));
    std::memcpy(&offset, output.data() + 8, sizeof(offset));
    std::memcpy(&reclen, output.data() + 16, sizeof(reclen));
    CHECK(ino == 1 && offset == 1 && reclen == 24);
    CHECK(std::memcmp(output.data() + 19, "dev", 4) == 0);
    CHECK(f.posix.close(directory.value()) && f.posix.close(covered.value()));
}

class RetainedNode final : public fs::Node
{
  public:
    explicit RetainedNode(fs::Node &backing) : backing_(backing)
    {
    }
    usize retained{}, released{};
    std::string_view name() const override
    {
        return backing_.name();
    }
    fs::Metadata metadata() const override
    {
        return backing_.metadata();
    }
    Result<fs::FileBuffer> read(usize offset, usize count) const override
    {
        return backing_.read(offset, count);
    }
    Status write(usize offset, std::span<const std::byte> data) override
    {
        return backing_.write(offset, data);
    }
    Status retain() override
    {
        ++retained;
        return Status::success();
    }
    Status release() override
    {
        ++released;
        return Status::success();
    }
    Node *lookup(std::string_view name) override
    {
        return backing_.lookup(name);
    }
    Status create(std::string_view name, fs::NodeType type) override
    {
        return backing_.create(name, type);
    }
    Status chmod(u32 mode) override
    {
        return backing_.chmod(mode);
    }
    Status chown(u32 uid, u32 gid) override
    {
        return backing_.chown(uid, gid);
    }

  private:
    fs::Node &backing_;
};

void test_retained_open_description_lifetime()
{
    static Fixture f;
    static RetainedNode node(*f.vfs.lookup("/data"));
    const auto parent = f.create("retained-description");
    CHECK(f.posix.activate(parent));
    sched::OpenFileDescription description{};
    description.node = &node;
    description.readable = description.writable = true;
    auto id = f.scheduler.allocate_description(description);
    CHECK(id);
    CHECK(f.scheduler.install_descriptor(f.scheduler.find(parent)->resources(), 3, id.value()));
    CHECK(node.retained == 1 && node.released == 0);
    auto duplicate = f.posix.duplicate(3);
    CHECK(duplicate);
    auto child = f.posix.fork();
    CHECK(child);
    CHECK(node.retained == 1);
    CHECK(f.posix.unlink("/data") && !f.vfs.lookup("/data"));
    CHECK(f.posix.close(3) && f.posix.close(duplicate.value()));
    CHECK(node.released == 0);
    CHECK(f.posix.activate(child.value()));
    CHECK(f.posix.close(3));
    CHECK(node.released == 0);
    std::array<std::byte, 2> output{};
    CHECK(f.posix.pread(duplicate.value(), output, 0).value() == 2 && output[0] == std::byte{'a'});
    CHECK(f.posix.exit(0));
    CHECK(node.retained == 1 && node.released == 1);
    CHECK(f.posix.activate(parent));
    CHECK(f.posix.wait(parent, child.value()));
}

void test_positioned_io_and_descriptor_errors()
{
    static Fixture f;
    const auto pid = f.create("positioned-io");
    CHECK(f.posix.activate(pid));
    std::array<std::byte, 4> output{};
    const auto errno_for = [](Status status) { return syscall::ErrnoMapper::errno_for(status); };
    CHECK(errno_for(Status::io_error("device failure")) == 5);
    CHECK(errno_for(Status::fault("invalid user pointer")) == 14);
    CHECK(errno_for(f.posix.fsync(-1)) == 9);
    CHECK(errno_for(f.posix.ftruncate(-1, 0)) == 9);
    CHECK(errno_for(f.posix.pread(-1, output, 0).status()) == 9);
    CHECK(errno_for(f.posix.pwrite(-1, bytes("x"), 0).status()) == 9);
    auto read_only = f.posix.open("/data", posix::o_RDONLY);
    CHECK(read_only);
    CHECK(errno_for(f.posix.ftruncate(read_only.value(), 0)) == 9);
    CHECK(errno_for(f.posix.pwrite(read_only.value(), bytes("x"), 0).status()) == 9);
    CHECK(f.posix.fsync(read_only.value()));
    auto pipe = f.posix.pipe();
    CHECK(pipe);
    CHECK(errno_for(f.posix.pread(pipe.value()[0], output, 0).status()) == 29);
    CHECK(errno_for(f.posix.pwrite(pipe.value()[1], bytes("x"), 0).status()) == 29);
    CHECK(errno_for(f.posix.fsync(pipe.value()[1])) == 22);
    std::array<std::byte, sched::pipe_capacity + 1> large{};
    const posix::IoVector vectors[] = {{reinterpret_cast<uptr>(large.data()), large.size()},
                                       {reinterpret_cast<uptr>(large.data()), 1}};
    CHECK(f.posix.writev(pipe.value()[1], vectors).value() == sched::pipe_capacity);
    CHECK(f.scheduler.find(pid)->state() != sched::ProcessState::blocked);
    CHECK(f.posix.read(pipe.value()[0], large).value() == sched::pipe_capacity);
    auto fd = f.posix.open("/data", posix::o_RDWR | posix::o_APPEND);
    CHECK(fd);
    CHECK(f.posix.seek(fd.value(), 2, posix::SeekWhence::set));
    CHECK(f.posix.pwrite(fd.value(), bytes("Z"), 0).value() == 1);
    CHECK(f.posix.pread(fd.value(), output, 0).value() == 4 && output[0] == std::byte{'Z'});
    CHECK(f.posix.seek(fd.value(), 0, posix::SeekWhence::current).value() == 2);
    CHECK(f.posix.ftruncate(fd.value(), 0));
    CHECK(f.posix.fstat(fd.value()).value().blocks == 0);
    CHECK(f.posix.ftruncate(fd.value(), 1000));
    CHECK(f.posix.fstat(fd.value()).value().blocks == 2);
    CHECK(f.posix.seek(fd.value(), 0, posix::SeekWhence::current).value() == 2);
    CHECK(f.posix.pread(fd.value(), output, 999).value() == 1 && output[0] == std::byte{0});
    CHECK(f.posix.close(fd.value()) && f.posix.close(read_only.value()));
    CHECK(f.posix.close(pipe.value()[0]) && f.posix.close(pipe.value()[1]));
}

void test_exec_metadata_and_rollback()
{
    static Fixture f;
    const auto pid = f.create("exec");
    CHECK(f.posix.activate(pid));
    CHECK(f.posix.chdir("/a"));
    CHECK(f.posix.set_identity(1000, 1000));
    auto closed = f.posix.open("/data", posix::o_RDONLY | posix::o_CLOEXEC);
    auto kept = f.posix.open("/data", posix::o_RDONLY);
    CHECK(closed && kept);
    CHECK(
        f.scheduler.create_thread(pid, arch::configured_arch_operations().make_user_context({0x500000, 0x810000, 0})));
    CHECK(f.posix.arch_prctl(posix::arch_SET_FS, 0x1234));
    sched::MemoryMap map;
    CHECK(map.add_area({.base = 0x600000,
                        .length = 4096,
                        .permissions = memory::page_read | memory::page_execute | memory::page_user}));
    auto context = arch::configured_arch_operations().make_user_context({0x600000, 0x900000, 0});
    auto bad = context;
    bad.architecture = arch::configured_architecture() == arch::Architecture::x86_64 ? arch::Architecture::rv64
                                                                                     : arch::Architecture::x86_64;
    CHECK(!f.posix.exec(pid, bad, map));
    CHECK(f.scheduler.find(pid)->fd_table().contains(closed.value()));
    CHECK(f.scheduler.find(pid)->threads().size() == 2);
    CHECK(f.posix.exec(pid, context, map));
    auto *process = f.scheduler.find(pid);
    CHECK(process && process->threads().size() == 1 && process->threads()[0].context.program_counter == 0x600000);
    CHECK(process->threads()[0].tls.fs_base == 0);
    CHECK(!process->fd_table().contains(closed.value()) && process->fd_table().contains(kept.value()));
    CHECK(process->memory_map().contains(0x600000, 1));
    CHECK(f.posix.activate(pid));
    CHECK(f.posix.getcwd() == "/a" && f.posix.getuid() == 1000);
}

void test_pipe_wait_and_wakeup()
{
    static Fixture f;
    const auto parent = f.create("reader");
    CHECK(f.posix.activate(parent));
    auto pipe = f.posix.pipe();
    CHECK(pipe);
    const auto read_fd = pipe.value()[0];
    const auto write_fd = pipe.value()[1];
    auto child = f.posix.fork();
    CHECK(child);
    CHECK(f.posix.close(write_fd));
    std::array<std::byte, 8> out{};
    CHECK(f.posix.read(read_fd, out).status().code() == StatusCode::would_block);
    CHECK(f.scheduler.find(parent)->state() == sched::ProcessState::blocked);
    CHECK(f.posix.activate(child.value()));
    CHECK(f.posix.close(read_fd));
    CHECK(f.posix.write(write_fd, bytes("pipe")).value() == 4);
    CHECK(f.scheduler.find(parent)->state() == sched::ProcessState::runnable);
    CHECK(f.posix.activate(parent));
    CHECK(f.posix.read(read_fd, out).value() == 4 && out[0] == std::byte{'p'});
    CHECK(f.posix.wait(parent, child.value()).status().code() == StatusCode::would_block);
    CHECK(f.scheduler.find(parent)->state() == sched::ProcessState::blocked);
    CHECK(f.posix.activate(child.value()));
    CHECK(f.posix.exit(23));
    CHECK(f.scheduler.find(parent)->state() == sched::ProcessState::runnable);
    CHECK(f.posix.activate(parent));
    CHECK(f.posix.read(read_fd, out).value() == 0);
    CHECK(f.posix.wait(parent, child.value()).value() == 23);
    CHECK(f.posix.close(read_fd));
    CHECK(f.scheduler.open_description_count() == 3);
    auto broken = f.posix.pipe();
    CHECK(broken);
    CHECK(f.posix.close(broken.value()[0]));
    CHECK(f.posix.write(broken.value()[1], bytes("x")).status().code() == StatusCode::broken_pipe);
    CHECK(f.posix.close(broken.value()[1]));
    CHECK(f.scheduler.open_description_count() == 3);
}

void test_orphan_reaping_and_native_ownership()
{
    static Fixture f;
    const auto root = f.create("reaper-test-root");
    CHECK(f.posix.activate(root));
    CHECK(f.posix.open("/data", posix::o_RDONLY));
    for (usize iteration = 0; iteration < 2 * sched::max_processes; ++iteration)
    {
        auto parent = f.posix.fork(root);
        CHECK(parent);
        auto zombie_child = f.posix.fork(parent.value());
        auto live_child = f.posix.fork(parent.value());
        CHECK(zombie_child && live_child);
        CHECK(f.posix.exit(zombie_child.value(), 31));
        CHECK(f.posix.exit(parent.value(), 17));
        CHECK(f.scheduler.find(zombie_child.value())->parent_pid() == 0);
        CHECK(f.scheduler.find(live_child.value())->parent_pid() == 0);
        CHECK(f.scheduler.find(parent.value())->children().empty());
        CHECK(f.scheduler.reap_orphans() == 1);
        CHECK(f.scheduler.find(zombie_child.value()) == nullptr);
        CHECK(f.scheduler.find(live_child.value()) != nullptr);
        CHECK(f.posix.wait(root, parent.value()).value() == 17);
        CHECK(f.posix.exit(live_child.value(), 0));
        CHECK(f.scheduler.reap_orphans() == 1);
        CHECK(f.scheduler.reap_orphans() == 0);
        CHECK(f.scheduler.process_count() == 1 && f.scheduler.find(root)->children().empty());
        CHECK(f.scheduler.open_description_count() == 4);
    }
    const auto orphan = f.create("native-backing-guard");
    auto &resources = f.scheduler.find(orphan)->resources();
    resources.native_space.root = 0x1000;
    CHECK(resources.native_pages.push_back({0x40000000, 0x2000, arch::execution::page_read}));
    CHECK(f.posix.exit(orphan, 0));
    CHECK(f.scheduler.reap_orphans() == 0);
    resources.native_space = {};
    CHECK(f.scheduler.reap_orphans() == 0);
    resources.native_pages.clear();
    CHECK(f.scheduler.reap_orphans() == 1);
    CHECK(f.scheduler.find(orphan) == nullptr);
}

void test_exit_status_and_signal_status()
{
    static Fixture f;
    const auto parent = f.create("exit-status-parent");
    const std::array<i32, 7> codes{
        0, 23, -1, -256, 256, std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max()};
    const std::array<i32, 7> expected{0, 23 << 8, 255 << 8, 0, 0, 0, 255 << 8};
    for (usize i = 0; i < codes.size(); ++i)
    {
        auto child = f.posix.fork(parent);
        CHECK(child);
        CHECK(f.posix.exit(child.value(), codes[i]));
        CHECK(f.scheduler.find(child.value())->wait_status() == expected[i]);
        CHECK(f.posix.wait(parent, child.value()).value() == codes[i]);
    }
    auto child = f.posix.fork(parent);
    CHECK(child);
    CHECK(!f.scheduler.terminate_process(child.value(), 0));
    CHECK(!f.scheduler.terminate_process(child.value(), 128));
    CHECK(f.posix.activate(parent));
    CHECK(f.posix.wait(parent, child.value()).status().code() == StatusCode::would_block);
    CHECK(f.scheduler.terminate_process(child.value(), 11));
    CHECK(f.scheduler.find(parent)->state() == sched::ProcessState::runnable);
    CHECK(f.scheduler.find(child.value())->wait_status() == 11);
    CHECK(f.posix.exit(child.value(), -1).code() == StatusCode::already_exists);
    CHECK(f.scheduler.find(child.value())->wait_status() == 11);
    CHECK(f.posix.wait(parent, child.value()));
}

void test_elf_validation_and_failed_exec()
{
    constexpr auto architecture = arch::configured_architecture();
    const auto machine = [] {
        switch (architecture)
        {
        case arch::Architecture::x86_64:
            return 0x3eu;
        case arch::Architecture::i386:
            return 3u;
        case arch::Architecture::aarch64:
            return 0xb7u;
        case arch::Architecture::arm32:
            return 0x28u;
        case arch::Architecture::rv32:
        case arch::Architecture::rv64:
            return 0xf3u;
        case arch::Architecture::mips:
        case arch::Architecture::mips64:
            return 8u;
        case arch::Architecture::ppc:
            return 0x14u;
        case arch::Architecture::loongarch64:
            return 0x102u;
        }
        return 0u;
    }();
    std::array<std::byte, 192> image{};
    auto set = [&](usize offset, usize width, u64 value, bool big = false) {
        for (usize i = 0; i < width; ++i)
            image[offset + i] = static_cast<std::byte>((value >> ((big ? width - i - 1 : i) * 8)) & 0xffu);
    };
    auto reset = [&](bool big = false) {
        image = {};
        image[0] = std::byte{0x7f};
        image[1] = std::byte{'E'};
        image[2] = std::byte{'L'};
        image[3] = std::byte{'F'};
        image[4] = std::byte{2};
        image[5] = static_cast<std::byte>(big ? 2 : 1);
        image[6] = std::byte{1};
        set(16, 2, 2, big);
        set(18, 2, machine, big);
        set(20, 4, 1, big);
        set(24, 8, 0x400000, big);
        set(32, 8, 64, big);
        set(52, 2, 64, big);
        set(54, 2, 56, big);
        set(56, 2, 1, big);
        set(64, 4, 1, big);
        set(68, 4, 5, big);
        set(72, 8, 176, big);
        set(80, 8, 0x400000, big);
        set(96, 8, 4, big);
        set(104, 8, 4096, big);
        set(112, 8, 1, big);
    };
    sched::ElfLoader loader;
    reset();
    CHECK(loader.load(image, architecture));
    reset(true);
    CHECK(loader.load(image, architecture));
    reset();
    image[5] = std::byte{};
    CHECK(!loader.load(image, architecture));
    reset();
    set(32, 8, ~u64{0} - 15);
    CHECK(!loader.load(image, architecture));
    reset();
    set(72, 8, ~u64{0} - 3);
    CHECK(!loader.load(image, architecture));
    reset();
    set(80, 8, ~u64{0} - 1024);
    CHECK(!loader.load(image, architecture));
    reset();
    set(24, 8, 0);
    CHECK(!loader.load(image, architecture));
    reset();
    set(24, 8, 0x900000);
    CHECK(!loader.load(image, architecture));
    reset();
    set(68, 4, 4);
    CHECK(!loader.load(image, architecture));
    reset();
    set(16, 2, 1);
    CHECK(!loader.load(image, architecture));
    reset();
    set(112, 8, 3);
    CHECK(!loader.load(image, architecture));
    for (bool big : {false, true})
    {
        reset(big);
        image[4] = std::byte{1};
        set(24, 4, 0x400000, big);
        set(28, 4, 52, big);
        set(40, 2, 52, big);
        set(42, 2, 32, big);
        set(44, 2, 1, big);
        set(52, 4, 1, big);
        set(56, 4, 176, big);
        set(60, 4, 0x400000, big);
        set(68, 4, 4, big);
        set(72, 4, 4096, big);
        set(76, 4, 5, big);
        set(80, 4, 1, big);
        auto elf32 = loader.load(image, architecture);
        CHECK(elf32 && elf32.value().entry == 0x400000 && elf32.value().segments[0].file_offset == 176);
        set(56, 4, 0xfffffff0, big);
        CHECK(!loader.load(image, architecture));
    }
    static Fixture f;
    sched::ProcessManager manager(f.scheduler);
    const auto pid = f.create("invalid-exec");
    const auto original_entry = f.scheduler.find(pid)->threads()[0].context.program_counter;
    reset();
    set(56, 2, 2);
    set(120, 4, 1);
    set(124, 4, 4);
    set(128, 8, 180);
    set(136, 8, 0x400800);
    set(152, 8, 4);
    set(160, 8, 4096);
    set(168, 8, 1);
    CHECK(!manager.execve(pid, image, architecture));
    CHECK(f.scheduler.find(pid)->threads()[0].context.program_counter == original_entry);
    CHECK(f.scheduler.find(pid)->memory_map().area_count() == 0);
    CHECK(f.scheduler.process_count() == 1);
    CHECK(!manager.create_user_process("overlap", image, architecture));
    CHECK(f.scheduler.process_count() == 1);
}

int main()
{
    test_unique_owner_and_isolation();
    test_fork_dup_offsets_and_lifetime();
    test_mounted_paths_and_directory_descriptors();
    test_retained_open_description_lifetime();
    test_positioned_io_and_descriptor_errors();
    test_exec_metadata_and_rollback();
    test_pipe_wait_and_wakeup();
    test_orphan_reaping_and_native_ownership();
    test_exit_status_and_signal_status();
    test_elf_validation_and_failed_exec();
    std::puts("PASS canonical owner, process isolation, shared offsets, exec, exit/wait, pipe wakeup, orphan reaping, "
              "exit status");
}
