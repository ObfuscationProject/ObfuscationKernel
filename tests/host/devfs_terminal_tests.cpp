// The kernel device filesystem's terminal nodes and /dev/pts directory
// (docs/modules/tty.md section 2): names, numbers, modes, listing, strict
// lookup, stale entries and the namespace attachment, alone and mounted in an
// actual VFS. Hosted model only; no open path, guest or hardware claim.
#include "ok/fs/devfs.hpp"
#include "ok/posix/posix.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>

using namespace ok;
static usize checks{};
#define CHECK(expression) do { ++checks; if (!static_cast<bool>(expression)) { \
    std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #expression); std::abort(); } } while (false)

namespace
{
// Constant initialization is the kernel contract: no dynamic initializer.
constinit fs::DeviceFileSystem constant_devfs;

struct Expected
{
    std::string_view name;
    fs::FilesystemDevice rdev;
    u64 inode;
    u32 mode;
    fs::TerminalNodeKind kind;
};
constexpr std::array<Expected, fs::DeviceFileSystem::device_count> devices{{
    {"null", {1, 3}, 3, 0666, fs::TerminalNodeKind::none},
    {"zero", {1, 5}, 5, 0666, fs::TerminalNodeKind::none},
    {"full", {1, 7}, 7, 0666, fs::TerminalNodeKind::none},
    {"tty", {5, 0}, 1280, 0666, fs::TerminalNodeKind::controlling},
    {"console", {5, 1}, 1281, 0600, fs::TerminalNodeKind::console},
    {"ptmx", {5, 2}, 1282, 0666, fs::TerminalNodeKind::multiplexer},
}};

bool device_metadata(const fs::Metadata &metadata, fs::FilesystemDevice rdev, u64 inode, u32 mode, u32 uid = 0,
                     u32 gid = 0)
{
    const auto &a = metadata.attributes;
    return metadata.type == fs::NodeType::device && metadata.mode == (fs::mode_character_device | mode) &&
        metadata.uid == uid && metadata.gid == gid && metadata.link_count == 1 && metadata.size == 0 &&
        a.has(fs::Attribute::special_device) && a.special == rdev && a.has(fs::Attribute::inode) && a.inode == inode &&
        a.has(fs::Attribute::blocks512) && a.access.source == fs::TimestampSource::kernel_fixed;
}

usize root_nodes(fs::DeviceFileSystem &devfs)
{
    auto listing = devfs.root().list();
    CHECK(listing && listing.value().count == fs::DeviceFileSystem::root_entry_count);
    for (usize i = 0; i < devices.size(); ++i)
    {
        const auto &entry = listing.value().entries[i];
        const auto &expected = devices[i];
        CHECK(entry.name.view() == expected.name);
        CHECK(device_metadata(entry.metadata, expected.rdev, expected.inode, expected.mode));
        auto *node = devfs.root().lookup(expected.name);
        CHECK(node && node->terminal_node().kind == expected.kind && node->filesystem_identity() == &devfs);
        CHECK(node->terminal_node().index == 0 && node->terminal_node().generation == 0);
        // Terminal nodes are names only; the memory devices keep their I/O.
        const auto read = node->read(0, 4);
        if (expected.kind == fs::TerminalNodeKind::none) CHECK(read && read.value().size == (i == 0 ? 0u : 4u));
        else CHECK(read.status().code() == StatusCode::unsupported);
        const std::array<std::byte, 1> byte{};
        const auto written = node->write(0, byte);
        CHECK(written.code() == (i == 2 ? StatusCode::no_space : expected.kind == fs::TerminalNodeKind::none
                                     ? StatusCode::ok : StatusCode::unsupported));
        CHECK(node->truncate(0)); // Linux ignores O_TRUNC on character devices.
        CHECK(node->chmod(0600).code() == StatusCode::permission);
    }
    const auto &pts = listing.value().entries[devices.size()];
    CHECK(pts.name.view() == "pts" && pts.metadata.type == fs::NodeType::directory);
    CHECK(pts.metadata.mode == (fs::mode_directory | 0755) && pts.metadata.attributes.inode == 2);
    CHECK(!pts.metadata.attributes.has(fs::Attribute::special_device));
    CHECK(devfs.root().lookup("pts") && devfs.root().lookup("pts")->lookup("..") == &devfs.root());
    CHECK(!devfs.root().lookup("pty") && !devfs.root().lookup(""));
    return listing.value().count;
}

usize pts_entries(fs::DeviceFileSystem &devfs, std::string_view expected)
{
    auto listing = devfs.root().lookup("pts")->list();
    CHECK(listing);
    std::array<char, 16> names{};
    usize count = 0;
    for (usize i = 0; i < listing.value().count; ++i)
    {
        CHECK(listing.value().entries[i].name.view().size() == 1);
        names[count++] = listing.value().entries[i].name.view()[0];
    }
    CHECK(std::string_view(names.data(), count) == expected);
    return count;
}

bool unattached()
{
    static fs::DeviceFileSystem devfs;
    CHECK(root_nodes(devfs) == 7);
    CHECK(!devfs.terminals() && pts_entries(devfs, "") == 0);
    // Without a namespace every pts name is absent.
    for (const auto name : {"0", "1", "7"}) CHECK(!devfs.root().lookup("pts")->lookup(name));
    return true;
}

bool attached_listing(bool &strict)
{
    static fs::DeviceFileSystem devfs, other;
    fs::TerminalNamespace terminals, second;
    CHECK(devfs.attach_terminals(terminals));
    CHECK(devfs.attach_terminals(terminals)); // The same namespace again.
    CHECK(devfs.attach_terminals(second).code() == StatusCode::busy);
    CHECK(other.attach_terminals(terminals).code() == StatusCode::busy); // One provider per namespace.
    CHECK(terminals.provider() == &devfs && !second.provider());
    CHECK(pts_entries(devfs, "") == 0);
    // A locked entry is listed (Linux devpts lists every allocated index).
    terminals.publish(2, {.generation = 11, .uid = 1000, .gid = 5, .mode = 0620, .present = true, .locked = true});
    terminals.publish(5, {.generation = 4, .uid = 0, .gid = 0, .mode = 0600, .present = true, .locked = false});
    CHECK(pts_entries(devfs, "25") == 2);
    auto *pts = devfs.root().lookup("pts");
    auto *two = pts->lookup("2");
    CHECK(two && two->name() == "2");
    CHECK(device_metadata(two->metadata(), {136, 2}, 34818, 0620, 1000, 5));
    const auto queried = two->query_metadata();
    CHECK(queried && device_metadata(queried.value(), {136, 2}, 34818, 0620, 1000, 5));
    CHECK(two->terminal_node() == (fs::TerminalNode{fs::TerminalNodeKind::pts_slave, 2, 11}));
    CHECK(two->read(0, 1).status().code() == StatusCode::unsupported);
    CHECK(two->chmod(0666).code() == StatusCode::permission && two->chown(1, 1).code() == StatusCode::permission);
    CHECK(two->change_mode(0666, user::kernel_credentials()).kind() == fs::ModeChangeResult::Kind::unavailable);
    CHECK(pts->lookup("5") && device_metadata(pts->lookup("5")->metadata(), {136, 5}, 34821, 0600));
    // Strict decimal: no sign, no leading zero, no suffix, below the capacity.
    usize refused = 0;
    for (const auto name : {"02", "+2", "-1", "", "2a", " 2", "8", "9", "10", "25", "3", "."})
        refused += pts->lookup(name) == nullptr;
    strict = refused == 12;
    CHECK(strict);
    // Changes apply only to the generation they name.
    terminals.set_locked(2, 12, false);
    CHECK(terminals.entry(2).locked);
    terminals.set_locked(2, 11, false);
    CHECK(!terminals.entry(2).locked);
    const auto revision = terminals.revision();
    terminals.remove(5, 3);
    CHECK(pts_entries(devfs, "25") == 2 && terminals.revision() == revision);
    // The master's last close removes the entry: a held reference is stale.
    auto *five = pts->lookup("5");
    terminals.remove(5, 4);
    CHECK(pts_entries(devfs, "2") == 1 && !pts->lookup("5"));
    CHECK(five->query_metadata().status().code() == StatusCode::not_found);
    CHECK(terminals.entry(5).uid == 0 && terminals.entry(5).mode == 0600 && !terminals.entry(5).present);
    CHECK(terminals.entry(fs::pts_capacity).generation == 0); // Out of range is absent.
    for (usize i = 0; i < fs::pts_capacity; ++i)
        terminals.publish(i, {.generation = i + 1, .present = true, .locked = (i & 1) != 0});
    CHECK(pts_entries(devfs, "01234567") == fs::pts_capacity);
    return terminals.entry(1).locked && pts->lookup("1");
}

// Mounted in an actual VFS whose POSIX service attached the scheduler's namespace.
struct Fixture
{
    fs::DeviceFileSystem devfs; // Outlives the VFS, which borrows its root.
    sched::Scheduler scheduler;
    fs::VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService posix;
};

bool mounted()
{
    auto f = std::make_unique<Fixture>();
    auto bare = std::make_unique<fs::VirtualFileSystem>();
    CHECK(f->devfs.attach_terminals(*bare).code() == StatusCode::not_initialized);
    CHECK(f->console.start() && f->posix.initialize(f->vfs, f->console, f->scheduler));
    CHECK(f->posix.initialize(f->vfs, f->console, f->scheduler)); // Same namespace again.
    auto stranger = std::make_unique<sched::Scheduler>();
    CHECK(f->posix.initialize(f->vfs, f->console, *stranger).code() == StatusCode::busy);
    CHECK(f->vfs.terminal_namespace() == &f->scheduler.terminal_namespace());
    CHECK(f->devfs.attach_terminals(f->vfs));
    CHECK(f->vfs.mount("/dev", f->devfs.root()));
    const auto listing = f->vfs.list("/dev");
    CHECK(listing && listing.value().count == 7);
    const auto identity = f->vfs.filesystem_device(&f->devfs);
    CHECK(identity && f->vfs.filesystem_device(f->scheduler.terminal_namespace().provider()));
    const auto ptmx = f->vfs.stat("/dev/ptmx");
    CHECK(ptmx && device_metadata(ptmx.value(), {5, 2}, 1282, 0666));
    CHECK(ptmx.value().attributes.has(fs::Attribute::device) && ptmx.value().attributes.device == identity.value());
    CHECK(f->vfs.list("/dev/pts") && f->vfs.list("/dev/pts").value().count == 0);
    // A ptmx-style pair is listed at once, locked, and goes with its master.
    const auto made = f->scheduler.allocate_description({});
    CHECK(made && f->scheduler.retain_description(made.value()));
    const auto master = f->scheduler.bind_pty_multiplexer(made.value());
    CHECK(master);
    const auto name = std::array<char, 2>{static_cast<char>('0' + master.value().slot), 0};
    const auto pts = f->vfs.list("/dev/pts");
    CHECK(pts && pts.value().count == 1 && pts.value().entries[0].name.view() == std::string_view(name.data(), 1));
    CHECK(f->scheduler.terminal_namespace().entry(master.value().slot).locked);
    CHECK(f->scheduler.set_pty_lock(made.value(), false));
    CHECK(!f->scheduler.terminal_namespace().entry(master.value().slot).locked);
    const std::array<char, 11> path{'/', 'd', 'e', 'v', '/', 'p', 't', 's', '/', name[0], 0};
    const auto slave = f->vfs.stat(std::string_view(path.data(), 10));
    CHECK(slave && slave.value().attributes.special == (fs::FilesystemDevice{136, master.value().slot}));
    CHECK(f->scheduler.release_description(made.value()));
    CHECK(f->vfs.list("/dev/pts").value().count == 0);
    CHECK(f->vfs.stat(std::string_view(path.data(), 10)).status().code() == StatusCode::not_found);
    CHECK(f->scheduler.terminal_audit() && !f->scheduler.pty_audit().live_pairs);
    return true;
}
} // namespace

int main()
{
    CHECK(!constant_devfs.terminals());
    const usize nodes = root_nodes(constant_devfs);
    const bool alone = unattached();
    bool strict = false;
    const bool locked = attached_listing(strict);
    const bool vfs = mounted();
    CHECK(alone && vfs);
    std::printf("DEVFS_TERMINAL_MODEL_PASS nodes=%zu pts_capacity=%zu locked_listed=%d strict_lookup=%d constinit=1 "
                "guest=0\n", nodes, fs::pts_capacity, locked, strict);
    std::printf("checks=%zu\n", checks);
}
