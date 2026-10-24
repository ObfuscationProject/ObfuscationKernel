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
    fs::VirtualFileSystem vfs;
    sched::Scheduler scheduler;
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
    ~Fixture()
    {
        static_cast<void>(scheduler.drain_all_resources());
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

void test_naming_and_open_file_lifetimes()
{
    static Fixture f;
    CHECK(f.posix.activate(f.create("file-names")));
    auto left = f.posix.open("/a", posix::o_RDONLY | posix::o_DIRECTORY);
    auto right = f.posix.open("/b", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(left && right);
    auto source = f.posix.openat(left.value(), "source", posix::o_CREAT | posix::o_RDWR);
    auto replaced = f.posix.openat(right.value(), "target", posix::o_CREAT | posix::o_RDWR);
    CHECK(source && replaced);
    CHECK(f.posix.write(source.value(), bytes("source")).value() == 6);
    CHECK(f.posix.write(replaced.value(), bytes("old")).value() == 3);
    CHECK(f.posix.linkat(left.value(), "source", right.value(), "linked"));
    CHECK(f.vfs.lookup("/a/source") == f.vfs.lookup("/b/linked"));
    CHECK(f.posix.fstat(source.value()).value().link_count == 2);
    CHECK(f.posix.chdir("/a"));
    CHECK(f.posix.link("source", "../b/relative"));
    CHECK(f.posix.fstat(source.value()).value().link_count == 3);
    CHECK(f.posix.renameat(left.value(), "source", right.value(), "target"));
    CHECK(!f.posix.stat("/a/source"));
    CHECK(f.vfs.lookup("/b/target") == f.vfs.lookup("/b/linked"));
    CHECK(f.posix.fstat(replaced.value()).value().link_count == 0);
    CHECK(f.posix.rename("../b/./relative", "./renamed"));
    CHECK(f.vfs.lookup("/a/renamed") == f.vfs.lookup("/b/target"));
    CHECK(!f.vfs.lookup("/b/relative"));
    CHECK(f.posix.rename("/b/target", "/b/linked"));
    CHECK(f.posix.stat("/b/target").value().link_count == 3);
    CHECK(f.posix.stat("/b/linked").value().link_count == 3);
    std::array<std::byte, 8> output{};
    CHECK(f.posix.pread(replaced.value(), output, 0).value() == 3);
    CHECK(std::memcmp(output.data(), "old", 3) == 0);
    CHECK(f.posix.pwrite(replaced.value(), bytes("OLD"), 0).value() == 3);
    CHECK(f.posix.unlink("renamed"));
    CHECK(f.posix.fstat(source.value()).value().link_count == 2);
    CHECK(f.posix.unlink("/b/target"));
    CHECK(f.posix.fstat(source.value()).value().link_count == 1);
    CHECK(f.posix.unlink("/b/linked"));
    CHECK(f.posix.fstat(source.value()).value().link_count == 0);
    auto fresh = f.posix.open("/b/target", posix::o_CREAT | posix::o_RDWR);
    CHECK(fresh && f.posix.write(fresh.value(), bytes("fresh")).value() == 5);
    CHECK(f.posix.pwrite(source.value(), bytes("S"), 0).value() == 1);
    CHECK(f.posix.pread(source.value(), output, 0).value() == 6);
    CHECK(std::memcmp(output.data(), "Source", 6) == 0);
    CHECK(f.posix.pread(replaced.value(), output, 0).value() == 3);
    CHECK(std::memcmp(output.data(), "OLD", 3) == 0);
    CHECK(f.posix.pread(fresh.value(), output, 0).value() == 5);
    CHECK(std::memcmp(output.data(), "fresh", 5) == 0);
    CHECK(f.posix.close(source.value()) && f.posix.close(replaced.value()) && f.posix.close(fresh.value()));
    CHECK(f.posix.close(left.value()) && f.posix.close(right.value()));
}

void test_naming_errno_and_sticky_permissions()
{
    static Fixture f;
    CHECK(f.posix.activate(f.create("name-permissions")));
    const auto error = [](Status status) { return syscall::ErrnoMapper::errno_for(status); };
    CHECK(f.vfs.chmod("/a", 01777) && f.vfs.chmod("/b", 01777));
    CHECK(f.vfs.create("/a/foreign", fs::NodeType::regular));
    CHECK(f.vfs.create("/a/own", fs::NodeType::regular));
    CHECK(f.vfs.create("/b/foreign", fs::NodeType::regular));
    CHECK(f.vfs.chown("/a/foreign", 2000, 2000));
    CHECK(f.vfs.chown("/a/own", 1000, 1000));
    CHECK(f.vfs.chown("/b/foreign", 3000, 3000));
    CHECK(f.posix.set_identity(1000, 1000));
    CHECK(error(f.posix.rename("/a/foreign", "/b/new")) == 1);
    CHECK(error(f.posix.rename("/a/own", "/b/foreign")) == 1);
    CHECK(f.vfs.lookup("/a/foreign") && f.vfs.lookup("/a/own") && !f.vfs.lookup("/b/new"));
    CHECK(f.posix.set_identity(2000, 2000));
    CHECK(f.posix.rename("/a/foreign", "/b/new"));
    CHECK(f.posix.set_identity(0, 0));
    CHECK(f.posix.rename("/a/own", "/b/foreign"));
    CHECK(error(f.posix.link("/a", "/directory-link")) == 1);
    CHECK(error(f.posix.rename("/data", "/a")) == 21);
    CHECK(error(f.posix.rename("/a", "/data")) == 20);
    CHECK(error(f.posix.rename("/data/child", "/a/new")) == 20);
    CHECK(error(f.posix.link("/data", "/data/child")) == 20);
    CHECK(f.posix.mkdir("/empty") && f.posix.mkdir("/nonempty"));
    CHECK(f.vfs.create("/nonempty/child", fs::NodeType::regular));
    CHECK(error(f.posix.rename("/empty", "/nonempty")) == 39);
    CHECK(f.vfs.lookup("/empty") && f.vfs.lookup("/nonempty/child"));
    auto file = f.posix.open("/data", posix::o_RDONLY);
    CHECK(file);
    CHECK(error(f.posix.linkat(file.value(), "child", posix::at_FDCWD, "/a/new")) == 20);
    CHECK(error(f.posix.renameat(-1, "child", posix::at_FDCWD, "/a/new")) == 9);
    CHECK(error(f.posix.linkat(posix::at_FDCWD, "/data", posix::at_FDCWD, "/a/new", 1)) == 22);
    CHECK(f.posix.close(file.value()));
    static fs::VirtualFileSystem mounted;
    CHECK(f.vfs.mount("/b", *mounted.lookup("/")));
    CHECK(error(f.posix.link("/data", "/b/link")) == 18);
    CHECK(error(f.posix.rename("/data", "/b/moved")) == 18);
    CHECK(f.vfs.lookup("/data") && !mounted.lookup("/link") && !mounted.lookup("/moved"));
}

void test_directory_mount_identity()
{
    static Fixture f;
    static fs::VirtualFileSystem volume;
    const auto parent = f.create("mount-identity");
    CHECK(f.posix.activate(parent));
    CHECK(f.vfs.create("/a/mount", fs::NodeType::directory));
    CHECK(f.vfs.create("/b/mount", fs::NodeType::directory));
    CHECK(f.vfs.create("/a/parent-file", fs::NodeType::regular));
    CHECK(f.vfs.create("/b/parent-file", fs::NodeType::regular));
    CHECK(f.vfs.write_file("/a/parent-file", bytes("a")));
    CHECK(f.vfs.write_file("/b/parent-file", bytes("b")));
    CHECK(volume.create("/source", fs::NodeType::regular));
    CHECK(volume.create("/child", fs::NodeType::directory));
    CHECK(f.vfs.mount("/a/mount", *volume.lookup("/")));
    CHECK(f.vfs.mount("/b/mount", *volume.lookup("/")));
    auto first = f.posix.open("/a/mount", posix::o_RDONLY | posix::o_DIRECTORY);
    auto second = f.posix.open("/b/mount", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(first && second);
    const auto error = [](Status status) { return syscall::ErrnoMapper::errno_for(status); };
    CHECK(error(f.posix.link("/a/mount/source", "/b/mount/absolute-link")) == 18);
    CHECK(error(f.posix.rename("/a/mount/source", "/b/mount/absolute-move")) == 18);
    CHECK(error(f.posix.linkat(first.value(), "source", second.value(), "relative-link")) == 18);
    CHECK(error(f.posix.renameat(first.value(), "source", second.value(), "relative-move")) == 18);
    CHECK(f.posix.fchdir(second.value()) && f.posix.getcwd() == "/b/mount");
    CHECK(error(f.posix.link("source", "/a/mount/cwd-link")) == 18);
    CHECK(error(f.posix.rename("source", "/a/mount/cwd-move")) == 18);
    auto parent_file = f.posix.openat(second.value(), "../parent-file", posix::o_RDONLY);
    CHECK(parent_file);
    std::array<std::byte, 1> content{};
    CHECK(f.posix.read(parent_file.value(), content).value() == 1 && content[0] == std::byte{'b'});
    CHECK(f.posix.close(parent_file.value()));
    CHECK(f.posix.chdir("..") && f.posix.getcwd() == "/b");
    CHECK(f.posix.chdir("/b/mount/child"));
    auto inherited = f.posix.fork();
    CHECK(inherited);
    CHECK(f.posix.rename("/a/mount/child", "/a/mount/moved"));
    CHECK(f.posix.getcwd() == "/b/mount/moved");
    CHECK(f.posix.activate(inherited.value()));
    CHECK(f.posix.getcwd() == "/b/mount/moved");
    CHECK(f.posix.fchdir(second.value()) && f.posix.getcwd() == "/b/mount");
    CHECK(f.posix.chdir("moved"));
    auto covered = f.posix.open(".", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(covered);
    // Even when the covering mount uses the same Node, the previous handle must
    // not silently switch mount identities or fall back to the /a alias.
    CHECK(f.vfs.mount("/b/mount/moved", *volume.lookup("/moved")));
    CHECK(error(f.posix.current_directory().status()) == 2);
    auto still_covered = f.posix.openat(covered.value(), ".", posix::o_RDONLY);
    CHECK(still_covered);
    CHECK(f.posix.fchdir(still_covered.value()));
    CHECK(error(f.posix.current_directory().status()) == 2);
    CHECK(f.posix.close(still_covered.value()));
    CHECK(f.posix.exit(0));
    CHECK(f.posix.activate(parent));
    CHECK(error(f.posix.current_directory().status()) == 2);
    CHECK(f.posix.chdir("/") && f.posix.wait(parent, inherited.value()));
    CHECK(f.posix.close(first.value()) && f.posix.close(second.value()));
}

void test_naming_path_search_permissions()
{
    static Fixture f;
    CHECK(f.posix.activate(f.create("name-search")));
    CHECK(f.posix.mkdir("/private") && f.posix.mkdir("/private/sub"));
    CHECK(f.vfs.chmod("/private/sub", 0777) && f.vfs.chmod("/a", 0777));
    CHECK(f.vfs.create("/private/sub/source", fs::NodeType::regular));
    auto held = f.posix.open("/private/sub", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(held && f.posix.chdir("/private/sub"));
    CHECK(f.vfs.chmod("/private", 0000));
    CHECK(f.posix.set_identity(1000, 1000));
    const auto denied = [](Status status) { return syscall::ErrnoMapper::errno_for(status) == 13; };
    CHECK(denied(f.posix.link("/private/sub/source", "/a/escaped")));
    CHECK(denied(f.posix.rename("/private/sub/source", "/a/escaped")));
    CHECK(denied(f.posix.link("/data", "/private/sub/target")));
    CHECK(denied(f.posix.rename("/a/../data", "/private/sub/target")));
    CHECK(denied(f.posix.link("/private/../data", "/a/escaped")));
    CHECK(!f.vfs.lookup("/a/escaped") && !f.vfs.lookup("/private/sub/target"));
    CHECK(f.posix.linkat(held.value(), "source", held.value(), "alias"));
    CHECK(f.posix.rename("alias", "renamed"));
    CHECK(f.posix.link("renamed", "cwd-link"));
    CHECK(f.posix.renameat(held.value(), "renamed", held.value(), "moved"));
    CHECK(f.vfs.lookup("/private/sub/source") == f.vfs.lookup("/private/sub/moved"));
    CHECK(f.vfs.chmod("/private/sub", 0666));
    CHECK(denied(f.posix.linkat(held.value(), "source", held.value(), "blocked")));
    CHECK(denied(f.posix.renameat(held.value(), "source", held.value(), "blocked")));
    CHECK(f.posix.set_identity(0, 0) && f.posix.chdir("/") && f.posix.close(held.value()));
}

void test_renamed_and_detached_directory_resources()
{
    static Fixture f;
    const auto parent = f.create("directory-owner");
    const auto mover = f.create("directory-mover");
    CHECK(f.posix.activate(parent));
    CHECK(f.posix.mkdir("/a/tree") && f.posix.mkdir("/a/tree/sub"));
    CHECK(f.vfs.create("/a/tree/sub/value", fs::NodeType::regular));
    CHECK(f.vfs.write_file("/a/tree/sub/value", bytes("original")));
    auto tree = f.posix.open("/a/tree", posix::o_RDONLY | posix::o_DIRECTORY);
    auto sub = f.posix.open("/a/tree/sub", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(tree && sub);
    CHECK(f.posix.chdir("/a/./tree/sub/../sub"));
    CHECK(f.posix.getcwd() == "/a/tree/sub");
    auto child = f.posix.fork();
    CHECK(child);
    CHECK(f.posix.activate(mover));
    CHECK(f.posix.rename("/a/tree", "/b/renamed"));
    CHECK(f.posix.mkdir("/a/tree") && f.posix.mkdir("/a/tree/sub"));
    CHECK(f.vfs.create("/a/tree/sub/value", fs::NodeType::regular));
    CHECK(f.vfs.write_file("/a/tree/sub/value", bytes("decoy")));
    CHECK(f.posix.activate(parent));
    CHECK(f.posix.getcwd() == "/b/renamed/sub");
    CHECK(f.posix.fchdir(tree.value()) && f.posix.getcwd() == "/b/renamed");
    CHECK(f.posix.fchdir(sub.value()) && f.posix.getcwd() == "/b/renamed/sub");
    CHECK(f.posix.chdir("/"));
    CHECK(f.posix.activate(child.value()));
    CHECK(f.posix.getcwd() == "/b/renamed/sub");
    auto inherited = f.posix.openat(sub.value(), "./value", posix::o_RDONLY);
    auto relative = f.posix.open("../sub/value", posix::o_RDONLY);
    CHECK(inherited && relative);
    std::array<std::byte, 16> output{};
    CHECK(f.posix.read(inherited.value(), output).value() == 8);
    CHECK(std::memcmp(output.data(), "original", 8) == 0);
    CHECK(f.posix.read(relative.value(), output).value() == 8);
    CHECK(std::memcmp(output.data(), "original", 8) == 0);
    CHECK(f.posix.close(inherited.value()) && f.posix.close(relative.value()));
    CHECK(f.posix.activate(mover));
    CHECK(f.posix.rename("/b/renamed", "/a/again"));
    CHECK(f.posix.activate(child.value()));
    CHECK(f.posix.getcwd() == "/a/again/sub");
    CHECK(f.posix.fchdir(tree.value()) && f.posix.getcwd() == "/a/again");
    CHECK(f.posix.exit(0));
    CHECK(f.posix.activate(parent));
    CHECK(f.posix.wait(parent, child.value()));
    CHECK(f.posix.close(tree.value()) && f.posix.close(sub.value()));

    CHECK(f.posix.mkdir("/old-empty") && f.posix.mkdir("/replacement"));
    CHECK(f.vfs.create("/replacement/value", fs::NodeType::regular));
    auto detached = f.posix.open("/old-empty", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(detached);
    CHECK(f.posix.rename("/replacement", "/old-empty"));
    const auto error = [](Status status) { return syscall::ErrnoMapper::errno_for(status); };
    CHECK(error(f.posix.openat(detached.value(), "value", posix::o_RDONLY).status()) == 2);
    CHECK(error(f.posix.openat(detached.value(), "", posix::o_RDONLY).status()) == 2);
    CHECK(error(f.posix.stat("").status()) == 2);
    CHECK(error(f.posix.chdir("")) == 2);
    CHECK(error(f.posix.openat(detached.value(), "new", posix::o_CREAT | posix::o_RDWR).status()) == 2);
    CHECK(!f.vfs.lookup("/old-empty/new"));
    CHECK(f.posix.fchdir(detached.value()));
    CHECK(error(f.posix.current_directory().status()) == 2 && f.posix.getcwd().empty());
    CHECK(error(f.posix.open("value", posix::o_RDONLY).status()) == 2);
    auto absolute = f.posix.openat(detached.value(), "/data", posix::o_RDONLY);
    CHECK(absolute && f.posix.close(absolute.value()));
    auto detached_child = f.posix.fork();
    CHECK(detached_child);
    CHECK(f.posix.chdir("/") && f.posix.close(detached.value()));
    CHECK(f.posix.activate(detached_child.value()));
    CHECK(f.posix.close(detached.value()));
    CHECK(error(f.posix.current_directory().status()) == 2);
    CHECK(error(f.posix.open("value", posix::o_RDONLY).status()) == 2);
    CHECK(f.posix.exit(0));
    CHECK(f.posix.activate(parent));
    CHECK(f.posix.wait(parent, detached_child.value()));
    CHECK(f.posix.getcwd() == "/");
    auto replacement = f.posix.open("/old-empty/value", posix::o_RDONLY);
    CHECK(replacement && f.posix.close(replacement.value()));
}

class RetainedNode final : public fs::Node
{
  public:
    explicit RetainedNode(fs::Node &backing) : backing_(backing)
    {
    }
    usize retained{}, released{};
    Status retain_status{};
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
        if (!retain_status)
            return retain_status;
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
    alignas(4096) static std::array<std::byte, 4096> metadata_backing{};
    static memory::FrameAllocator metadata_frames;
    static user::native::NativePagePool metadata_pool;
    const memory::MemoryRegion metadata_region{reinterpret_cast<uptr>(metadata_backing.data()),
        metadata_backing.size(), memory::RegionType::usable};
    CHECK(metadata_frames.initialize({&metadata_region, 1}, 4096));
    CHECK(metadata_pool.activate(metadata_frames, user::native::NativePagePool::Source::linker_reserved, 1));
    CHECK(resources.native_pages.reserve(metadata_pool, 1));
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
    // The common completion path accepts the MIPS signal-128 identity, while
    // the legacy raw wait word cannot distinguish it from the core bit.
    CHECK(f.scheduler.terminate_process(child.value(), 128));
    CHECK(f.scheduler.find(child.value())->wait_status() == 128);
    CHECK(f.posix.wait(parent, child.value()));
    child = f.posix.fork(parent);
    CHECK(child);
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
    for (bool big : {false, true})
    {
        reset(big);
        set(72, 8, 1024 * 1024, big);
        CHECK(loader.load_prefix({image.data(), 120}, 1024 * 1024 + 4, architecture));
        CHECK(!loader.load(image, architecture));
        CHECK(!loader.load_prefix({image.data(), 119}, 1024 * 1024 + 4, architecture));
        CHECK(!loader.load_prefix({image.data(), 120}, 1024 * 1024 + 3, architecture));
        CHECK(!loader.load_prefix(image, 100, architecture));
        reset(big);
        set(56, 2, 2, big);
        set(120, 4, 3, big);
        auto dynamic = loader.load(image, architecture);
        CHECK(dynamic && !dynamic.value().static_executable);
    }
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

void test_unified_path_resolver()
{
    static Fixture f;
    CHECK(f.posix.activate(f.create("path-resolver")));
    const auto error = [](Status status) { return syscall::ErrnoMapper::errno_for(status); };
    CHECK(f.posix.mkdir("/a/tree"));
    CHECK(f.posix.chdir("/a/tree"));
    auto held = f.posix.open(".", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(held);
    CHECK(f.posix.symlinkat("../../data", held.value(), "relative"));
    CHECK(f.posix.symlinkat("/data", held.value(), "absolute"));
    CHECK(f.posix.symlinkat("/a/tree", posix::at_FDCWD, "/directory-link"));
    CHECK(f.posix.stat("relative").value().type == fs::NodeType::regular);
    CHECK(f.posix.statat(held.value(), "relative", posix::at_SYMLINK_NOFOLLOW).value().type == fs::NodeType::symlink);
    CHECK(f.posix.statat(held.value(), "", posix::at_EMPTY_PATH));
    CHECK(f.posix.statat(held.value(), "", posix::at_EMPTY_PATH | 0x80000000u).status().code() ==
          StatusCode::invalid_argument);
    std::array<std::byte, 4> target{};
    CHECK(f.posix.readlinkat(held.value(), "relative", target).value() == target.size());
    CHECK(std::memcmp(target.data(), "../.", target.size()) == 0);
    CHECK(error(f.posix.open("relative", posix::o_RDONLY | posix::o_NOFOLLOW).status()) == 40);
    CHECK(error(f.posix.open("relative", posix::o_CREAT | posix::o_EXCL).status()) == 17);
    CHECK(error(f.posix.stat("relative/").status()) == 20);
    CHECK(error(f.posix.stat("/missing/../data").status()) == 2);
    CHECK(error(f.posix.stat("/data/../data").status()) == 20);
    CHECK(f.posix.stat("/directory-link/").value().type == fs::NodeType::directory);
    CHECK(f.posix.symlinkat("new-file", held.value(), "dangling"));
    auto created = f.posix.open("dangling", posix::o_CREAT | posix::o_RDWR);
    CHECK(created && f.posix.close(created.value()));
    CHECK(f.posix.stat("new-file") && f.posix.statat(held.value(), "dangling", posix::at_SYMLINK_NOFOLLOW));
    CHECK(f.posix.symlinkat("missing-directory/child", held.value(), "broken-parent"));
    CHECK(error(f.posix.open("broken-parent", posix::o_CREAT | posix::o_RDWR).status()) == 2);
    CHECK(!f.posix.stat("missing-directory"));
    CHECK(f.posix.symlinkat("trailing-target/", held.value(), "trailing-link"));
    CHECK(error(f.posix.open("trailing-link", posix::o_CREAT | posix::o_RDWR).status()) == 20);
    CHECK(!f.posix.stat("trailing-target"));
    CHECK(!f.posix.unlink(".") && !f.posix.rename(".", "bad") && !f.posix.mkdir("."));
    CHECK(f.posix.stat(".").value().type == fs::NodeType::directory);
    CHECK(f.posix.symlinkat("cycle-b", held.value(), "cycle-a"));
    CHECK(f.posix.symlinkat("cycle-a", held.value(), "cycle-b"));
    CHECK(error(f.posix.stat("cycle-a").status()) == 40);
    CHECK(f.posix.link("relative", "hard-link"));
    CHECK(f.posix.statat(held.value(), "hard-link", posix::at_SYMLINK_NOFOLLOW).value().link_count == 2);
    CHECK(f.posix.rename("hard-link", "renamed-link"));
    CHECK(f.posix.unlink("renamed-link"));
    CHECK(f.posix.stat("/data") && f.posix.statat(held.value(), "relative", posix::at_SYMLINK_NOFOLLOW).value().link_count == 1);
    CHECK(f.posix.rename("/a/tree", "/b/moved"));
    CHECK(f.posix.getcwd() == "/b/moved");
    CHECK(f.posix.stat("relative") && f.posix.statat(held.value(), "absolute"));
    CHECK(f.posix.mkdir("/a/tree"));
    CHECK(f.posix.symlinkat("/missing", posix::at_FDCWD, "/a/tree/absolute"));
    CHECK(f.posix.statat(held.value(), "absolute"));

    CHECK(f.vfs.chmod("/b", 0700));
    CHECK(f.vfs.chmod("/b/moved", 0777));
    CHECK(f.posix.set_identity(1000, 1000));
    CHECK(f.posix.stat("absolute")); // Already held start does not recheck /b.
    CHECK(error(f.posix.stat("/b/moved/absolute").status()) == 13);
    CHECK(error(f.posix.stat("relative").status()) == 13); // Dot-dot consumes /b search permission.
    CHECK(f.posix.mkdirat(held.value(), "allowed"));
    CHECK(f.vfs.chmod("/b/moved", 0666));
    CHECK(error(f.posix.stat(".").status()) == 13);
    CHECK(error(f.posix.open("absolute", posix::o_RDONLY).status()) == 13);
    CHECK(error(f.posix.mkdir("blocked")) == 13);
    CHECK(error(f.posix.unlink("absolute")) == 13);
    CHECK(error(f.posix.chmod("absolute", 0600)) == 13);
    CHECK(error(f.posix.chdir(".")) == 13);
    CHECK(f.posix.set_identity(0, 0));
    CHECK(f.posix.chdir("/") && f.posix.close(held.value()));
}

void test_deadline_lifecycle()
{
    static Fixture f;
    const auto pid = f.create("deadline-owner");
    CHECK(f.posix.activate(pid));
    auto &thread = f.scheduler.find(pid)->threads()[0];
    auto generation = thread.native_deadline.arm(10, 1, 0, false);
    CHECK(generation);
    CHECK(f.scheduler.block_current(sched::ThreadWaitReason::deadline, generation.value()));
    CHECK(thread.state == sched::ProcessState::blocked);
    auto context = arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0});
    CHECK(f.scheduler.replace_image(pid, context, f.scheduler.find(pid)->memory_map()));
    CHECK(!thread.native_deadline.armed());
    CHECK(!thread.native_deadline.complete(2000000000, generation.value()));
    CHECK(f.posix.activate(pid));
    auto next = thread.native_deadline.arm(0, 2, 0, false);
    CHECK(next && next.value() != generation.value());
    CHECK(f.scheduler.block_current(sched::ThreadWaitReason::deadline, next.value()));
    CHECK(f.scheduler.terminate_process(pid, 9));
    CHECK(!thread.native_deadline.armed() && thread.state == sched::ProcessState::exited);
    f.scheduler.wake_waiters(sched::ThreadWaitReason::deadline, next.value());
    CHECK(thread.state == sched::ProcessState::exited);
    CHECK(!thread.native_deadline.complete(3000000000, next.value()));
}

void test_open_admission_and_errors()
{
    static Fixture f;
    const auto pid = f.create("open-admission");
    CHECK(f.posix.activate(pid));
    CHECK(f.posix.set_identity(0, 0));
    for (usize i = 3; i < sched::max_process_fds; ++i)
        CHECK(f.posix.duplicate(0));
    auto failure = f.posix.open("/blocked-by-fd", posix::o_WRONLY | posix::o_CREAT | posix::o_TRUNC);
    CHECK(!failure && failure.status().code() == StatusCode::process_file_limit);
    CHECK(syscall::ErrnoMapper::errno_for(failure.status()) == 24);
    CHECK(f.posix.stat("/blocked-by-fd").status().code() == StatusCode::not_found);
    CHECK(f.posix.open("/data", posix::o_WRONLY | posix::o_TRUNC).status().code() == StatusCode::process_file_limit);
    CHECK(f.posix.stat("/data").value().size == 10);
    for (usize i = 3; i < sched::max_process_fds; ++i)
        CHECK(f.posix.close(static_cast<i32>(i)));
    CHECK(f.scheduler.open_description_count() == 3);
    StaticVector<u16, sched::max_open_descriptions> held;
    while (true)
    {
        auto reserved = f.scheduler.allocate_description({});
        if (!reserved)
        {
            CHECK(reserved.status().code() == StatusCode::system_file_limit);
            break;
        }
        CHECK(f.scheduler.retain_description(reserved.value()));
        CHECK(held.push_back(reserved.value()));
    }
    failure = f.posix.open("/blocked-by-system", posix::o_WRONLY | posix::o_CREAT | posix::o_TRUNC);
    CHECK(!failure && failure.status().code() == StatusCode::system_file_limit);
    CHECK(syscall::ErrnoMapper::errno_for(failure.status()) == 23);
    CHECK(f.posix.stat("/blocked-by-system").status().code() == StatusCode::not_found);
    CHECK(f.posix.open("/data", posix::o_WRONLY | posix::o_TRUNC).status().code() == StatusCode::system_file_limit);
    CHECK(f.posix.stat("/data").value().size == 10);
    for (const auto id : held)
        CHECK(f.scheduler.release_description(id));
    for (usize attempt = 0; attempt < 1000; ++attempt)
        CHECK(f.posix.open("/missing", posix::o_RDONLY).status().code() == StatusCode::not_found);
    CHECK(f.scheduler.open_description_count() == 3);
    CHECK(f.posix.open("/data", posix::o_DIRECTORY).status().code() == StatusCode::not_directory);
    CHECK(f.posix.open("/a", posix::o_WRONLY).status().code() == StatusCode::is_directory);
    CHECK(f.posix.open("/a", posix::o_RDONLY | posix::o_CREAT).status().code() == StatusCode::is_directory);
    CHECK(f.posix.open("/a", posix::o_RDONLY | posix::o_TRUNC).status().code() == StatusCode::is_directory);
    CHECK(f.posix.open("/data", 0x80000000u | posix::o_TRUNC).status().code() == StatusCode::invalid_argument);
    CHECK(f.posix.open("/invalid-access", 3 | posix::o_CREAT).status().code() == StatusCode::invalid_argument);
    CHECK(f.posix.stat("/invalid-access").status().code() == StatusCode::not_found);
    CHECK(f.posix.open("/invalid-directory", posix::o_DIRECTORY | posix::o_CREAT).status().code() ==
          StatusCode::invalid_argument);
    CHECK(f.posix.stat("/invalid-directory").status().code() == StatusCode::not_found);
    CHECK(f.posix.stat("/data").value().size == 10);
    CHECK(f.scheduler.open_description_count() == 3);
    auto success = f.posix.open("/valid-new", posix::o_WRONLY | posix::o_CREAT | posix::o_TRUNC);
    CHECK(success && f.scheduler.open_description_count() == 4);
    CHECK(f.posix.write(success.value(), bytes("admitted")).value() == 8);
    CHECK(f.posix.close(success.value()));
    CHECK(f.posix.stat("/valid-new").value().size == 8 && f.scheduler.open_description_count() == 3);
    auto append_read = f.posix.open("/data", posix::o_RDONLY | posix::o_APPEND);
    CHECK(append_read);
    std::array<std::byte, 12> output{};
    CHECK(f.posix.seek(append_read.value(), 0, posix::SeekWhence::current).value() == 0);
    CHECK(f.posix.read(append_read.value(), {output.data(), 2}).value() == 2);
    CHECK(output[0] == std::byte{'a'} && output[1] == std::byte{'b'});
    CHECK(f.posix.close(append_read.value()));
    auto append_update = f.posix.open("/data", posix::o_RDWR | posix::o_APPEND);
    CHECK(append_update);
    CHECK(f.posix.read(append_update.value(), {output.data(), 2}).value() == 2);
    CHECK(output[0] == std::byte{'a'} && output[1] == std::byte{'b'});
    CHECK(f.posix.write(append_update.value(), bytes("XY")).value() == 2);
    CHECK(f.posix.seek(append_update.value(), 0, posix::SeekWhence::current).value() == 12);
    CHECK(f.posix.pread(append_update.value(), output, 0).value() == 12);
    CHECK(std::memcmp(output.data(), "abcdefghijXY", 12) == 0);
    CHECK(f.posix.close(append_update.value()));
    CHECK(f.scheduler.open_description_count() == 3);
    std::puts("PASS hosted append open: initial read offset zero, append write follows read at current EOF");
    std::puts(
        "PASS hosted open admission: EMFILE/ENFILE without create/truncate, pin cleanup, directory and flag errors");
}


void test_fork_capacity_and_tid_identity()
{
    static Fixture f;
    const auto parent = f.create("fork-capacity");
    CHECK(f.posix.activate(parent));
    const auto leader_tid = f.scheduler.current_tid(0);
    CHECK(f.posix.gettid() == leader_tid && f.posix.getpid() == parent);
    // Deliberately advance the TID sequence independently of process IDs.
    auto another_thread = f.scheduler.create_thread(parent,
        arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0}));
    CHECK(another_thread && another_thread.value() != leader_tid);
    auto fd = f.posix.open("/data", posix::o_RDONLY);
    CHECK(fd);
    auto *entry = f.scheduler.find(parent)->fd_table().entry(fd.value());
    const auto description = entry->description;
    const auto base_refs = f.scheduler.open_description(description)->references;
    static RetainedNode cwd(*f.vfs.lookup("/a"));
    CHECK(cwd.retain());
    auto &resources = f.scheduler.find(parent)->resources();
    auto *original_cwd = resources.cwd_node;
    resources.cwd_node = &cwd;
    cwd.retain_status = Status::overflow("injected cwd reference overflow");
    const auto failed_pin = f.posix.fork(parent);
    CHECK(!failed_pin && failed_pin.status().code() == StatusCode::overflow);
    CHECK(syscall::ErrnoMapper::errno_for(failed_pin.status()) == 75);
    CHECK(cwd.retained == 1 && cwd.released == 0 && f.scheduler.process_count() == 1);
    CHECK(f.scheduler.open_description(description)->references == base_refs);
    resources.cwd_node = original_cwd;
    cwd.retain_status = Status::success();
    CHECK(cwd.release());
    std::array<sched::ProcessId, sched::max_processes> children{};
    usize count = 0;
    while (count < children.size())
    {
        auto child = f.posix.fork(parent);
        if (!child)
        {
            CHECK(child.status().code() == StatusCode::would_block);
            CHECK(syscall::ErrnoMapper::errno_for(child.status()) == 11);
            break;
        }
        children[count++] = child.value();
    }
    CHECK(count > 0 && count < children.size());
    // The process table now bounds the child list so PID 1 can adopt every
    // surviving user process when a service parent exits.
    CHECK(count == sched::max_processes - 1);
    CHECK(f.scheduler.open_description(description)->references == base_refs + count);
    const auto total = f.scheduler.process_count();
    for (usize attempt = 0; attempt < 4; ++attempt)
    {
        CHECK(f.posix.fork(parent).status().code() == StatusCode::would_block);
        CHECK(f.scheduler.process_count() == total);
        CHECK(f.scheduler.open_description(description)->references == base_refs + count);
    }
    for (usize i = 0; i < count; ++i)
    {
        CHECK(f.posix.exit(children[i], 19));
        CHECK(f.posix.wait(parent, children[i]).value() == 19);
    }
    CHECK(f.scheduler.process_count() == 1 && f.scheduler.open_description(description)->references == base_refs);
    auto reused = f.posix.fork(parent);
    CHECK(reused && f.posix.activate(reused.value()));
    CHECK(f.posix.gettid() == f.scheduler.find(reused.value())->threads()[0].tid);
    CHECK(f.posix.gettid() == f.posix.getpid());
    CHECK(f.posix.exit(reused.value(), 23) && f.posix.activate(parent));
    CHECK(f.posix.wait(parent, reused.value()).value() == 23);
    CHECK(f.posix.close(fd.value()));
    // Fill the common process table without consuming the parent's child list.
    std::array<sched::ProcessId, sched::max_processes> fillers{};
    count = 0;
    while (f.scheduler.process_count() < sched::max_processes)
        fillers[count++] = f.create("table-filler");
    CHECK(f.scheduler.find(parent)->children().empty());
    CHECK(f.posix.fork(parent).status().code() == StatusCode::would_block);
    CHECK(f.scheduler.create_process("other", {}).status().code() == StatusCode::overflow);
    CHECK(syscall::ErrnoMapper::errno_for(Status::overflow("unrelated")) == 75);
    for (usize i = 0; i < count; ++i)
        CHECK(f.posix.exit(fillers[i], 0));
    // Reaping orphans is an existing separate scheduler path.
    for (usize i = 0; i < count; ++i)
        CHECK(f.scheduler.reap_process(fillers[i]));
    CHECK(f.scheduler.process_count() == 1);
    reused = f.posix.fork(parent);
    CHECK(reused && f.posix.exit(reused.value(), 0));
    CHECK(f.posix.wait(parent, reused.value()));
    std::puts("PASS hosted fork capacity: EAGAIN, unchanged unrelated EOVERFLOW, descriptor accounting, reap/retry and true TID");
}

void test_unix_socketpair_lifetime_and_streams()
{
    static Fixture f;
    const auto parent = f.create("unix-parent");
    CHECK(f.posix.activate(parent));
    const auto baseline = f.scheduler.open_description_count();
    auto pair = f.posix.unix_socketpair(posix::o_NONBLOCK | posix::o_CLOEXEC);
    CHECK(pair && pair.value()[0] == 3 && pair.value()[1] == 4);
    CHECK(f.scheduler.open_description_count() == baseline + 2);
    CHECK((f.posix.fstat(3).value().mode & fs::mode_type_mask) == fs::mode_socket);
    CHECK(f.posix.fcntl(3, posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
    CHECK(f.posix.fcntl(4, posix::f_GETFL, 0).value() == (posix::o_RDWR | posix::o_NONBLOCK));
    std::array<std::byte, 4> out{};
    CHECK(f.posix.read(3, out).status().code() == StatusCode::would_block);
    CHECK(f.posix.write(3, bytes("left")).value() == 4);
    CHECK(f.posix.write(4, bytes("rght")).value() == 4);
    CHECK(f.posix.read(3, out).value() == 4 && out[0] == std::byte{'r'});
    CHECK(f.posix.read(4, out).value() == 4 && out[0] == std::byte{'l'});
    static std::array<std::byte, sched::pipe_capacity> full{};
    CHECK(f.posix.write(3, full).value() == full.size());
    CHECK(f.posix.write(3, bytes("x")).status().code() == StatusCode::would_block);
    CHECK(f.posix.read(4, {out.data(), 1}).value() == 1);
    CHECK(f.posix.write(3, bytes("xy")).value() == 1); // Stream writes may make partial progress.
    auto duplicate = f.posix.duplicate(3);
    CHECK(duplicate && duplicate.value() == 5);
    auto child = f.posix.fork();
    CHECK(child);
    CHECK(f.posix.close(3) && f.posix.close(4) && f.posix.close(5));
    CHECK(f.scheduler.open_description_count() == baseline + 2);
    CHECK(f.posix.activate(child.value()));
    CHECK(f.posix.read(3, {out.data(), 1}).status().code() == StatusCode::would_block);
    CHECK(f.posix.close(3) && f.posix.close(5));
    CHECK(f.posix.write(4, bytes("x")).status().code() == StatusCode::broken_pipe);
    CHECK(f.posix.write(4, bytes("")).value() == 0);
    auto ready = f.posix.ready_events(4, posix::ReadyEvents::readable | posix::ReadyEvents::writable);
    CHECK(ready && (ready.value().events & posix::ReadyEvents::readable) != posix::ReadyEvents::none);
    CHECK((ready.value().events & posix::ReadyEvents::hangup) != posix::ReadyEvents::none);
    CHECK((ready.value().events & posix::ReadyEvents::error) != posix::ReadyEvents::none);
    CHECK(f.posix.read(4, full).value() == full.size());
    CHECK(full.back() == std::byte{'x'});
    CHECK(f.posix.read(4, {out.data(), 1}).value() == 0);
    CHECK(f.posix.close(4));
    CHECK(f.scheduler.open_description_count() == baseline);
    auto closed_peer = f.posix.unix_socketpair(posix::o_NONBLOCK);
    CHECK(closed_peer);
    const auto right = closed_peer.value()[1];
    const auto id = f.scheduler.find(child.value())->resources().files.entry(right)->description;
    CHECK(f.posix.close(closed_peer.value()[0]));
    const auto requested = posix::ReadyEvents::readable | posix::ReadyEvents::writable;
    for (usize iteration = 0; iteration < 2; ++iteration)
    {
        auto linux = f.posix.poll_description<posix::ReadinessPolicy::linux>(id, requested);
        CHECK(linux);
        CHECK((linux.value().events & posix::ReadyEvents::readable) != posix::ReadyEvents::none);
        CHECK((linux.value().events & posix::ReadyEvents::writable) != posix::ReadyEvents::none);
        CHECK((linux.value().events & posix::ReadyEvents::hangup) != posix::ReadyEvents::none);
        CHECK((linux.value().events & posix::ReadyEvents::error) == posix::ReadyEvents::none);
        CHECK(f.posix.read(right, out).value() == 0);
    }
    CHECK(f.posix.close(right));
    CHECK(f.scheduler.open_description_count() == baseline);
    CHECK(f.posix.exit(child.value(), 0));
    CHECK(f.posix.activate(parent) && f.posix.wait(parent, child.value()));

    // Both buffer reservations and both OFDs must roll back at a process limit.
    std::array<i32, 28> held{};
    for (auto &fd : held) { auto added = f.posix.duplicate(1); CHECK(added); fd = added.value(); }
    CHECK(f.posix.unix_socketpair().status().code() == StatusCode::process_file_limit);
    CHECK(f.scheduler.open_description_count() == baseline);
    for (auto fd : held) CHECK(f.posix.close(fd));
    CHECK(f.posix.unix_socketpair());
    CHECK(f.posix.close(3) && f.posix.close(4));
    CHECK(f.scheduler.open_description_count() == baseline);
    std::puts("PASS hosted AF_UNIX streams: duplex, partial/EAGAIN/EPIPE, dup/fork, EOF ownership and rollback");
}

int main()
{
    test_unix_socketpair_lifetime_and_streams();
    test_fork_capacity_and_tid_identity();
    test_open_admission_and_errors();
    test_deadline_lifecycle();
    test_unified_path_resolver();
    test_unique_owner_and_isolation();
    test_fork_dup_offsets_and_lifetime();
    test_mounted_paths_and_directory_descriptors();
    test_naming_and_open_file_lifetimes();
    test_naming_errno_and_sticky_permissions();
    test_directory_mount_identity();
    test_naming_path_search_permissions();
    test_renamed_and_detached_directory_resources();
    test_retained_open_description_lifetime();
    test_positioned_io_and_descriptor_errors();
    test_exec_metadata_and_rollback();
    test_pipe_wait_and_wakeup();
    test_orphan_reaping_and_native_ownership();
    test_exit_status_and_signal_status();
    test_elf_validation_and_failed_exec();
    std::puts("PASS canonical owner, process isolation, shared offsets, exec, exit/wait, pipe wakeup, orphan reaping, "
              "exit status, rename/link lifetimes and retained directories");
}
