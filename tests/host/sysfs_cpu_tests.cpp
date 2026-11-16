// The CPU sysfs provider (docs/modules/sysfs.md): the tree, modes, cpulists
// for UP, SMP, offline and wedged views, strict lookup, directory paths,
// constant initialization, and the kernfs open semantics through the actual
// PosixService open with root and uid 1000 callers. Hosted model only; no
// write route, hotplug service, guest or hardware claim.
#include "ok/fs/sysfs.hpp"
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
smp::CpuView current_view{};
smp::CpuView fake_view() noexcept { return current_view; }
} // namespace

// The kernel's source (src/smp/hotplug.cpp), here the model's view.
smp::CpuView ok::smp::cpu_view() noexcept { return current_view; }

namespace
{
// Constant initialization is the kernel contract: no dynamic initializer.
constinit fs::CpuSysFileSystem constant_default;
constinit fs::CpuSysFileSystem constant_model{&fake_view};

std::string_view text(const fs::Node &node)
{
    static fs::FileBuffer buffer;
    auto read = node.read(0, 4096);
    CHECK(read);
    buffer = read.value();
    return {reinterpret_cast<const char *>(buffer.data.data()), buffer.size};
}

fs::Node *walk(fs::CpuSysFileSystem &sysfs, std::initializer_list<std::string_view> path)
{
    fs::Node *node = &sysfs.root();
    for (const auto part : path)
    {
        if (!node) return nullptr;
        node = node->lookup(part);
    }
    return node;
}

usize names(fs::Node &directory, std::string_view expected)
{
    auto listing = directory.list();
    CHECK(listing);
    std::array<char, 256> joined{};
    usize size = 0;
    for (usize i = 0; i < listing.value().count; ++i)
    {
        if (i) joined[size++] = ' ';
        for (char value : listing.value().entries[i].name.view()) joined[size++] = value;
    }
    CHECK(std::string_view(joined.data(), size) == expected);
    return listing.value().count;
}

bool attribute(const fs::Node &node, u32 mode, u64 inode, fs::ControlNodeKind kind)
{
    const auto metadata = node.metadata();
    return metadata.type == fs::NodeType::regular && metadata.mode == (fs::mode_regular | mode) && metadata.uid == 0 &&
           metadata.gid == 0 && metadata.size == 4096 && metadata.link_count == 1 &&
           metadata.attributes.inode == inode && metadata.attributes.access.source == fs::TimestampSource::kernel_fixed &&
           node.control_node().kind == kind;
}

// The tree with four CPUs, CPU 2 offline.
usize smp_tree(fs::CpuSysFileSystem &sysfs)
{
    current_view = {.present = 0xf, .online = 0b1011};
    auto *cpu = walk(sysfs, {"devices", "system", "cpu"});
    CHECK(cpu && cpu->metadata().type == fs::NodeType::directory);
    CHECK(cpu->metadata().mode == (fs::mode_directory | 0755) && cpu->metadata().link_count == 6);
    usize nodes = names(*cpu, "kernel_max possible present online offline cpu0 cpu1 cpu2 cpu3");
    CHECK(text(*cpu->lookup("kernel_max")) == "15\n");
    CHECK(text(*cpu->lookup("possible")) == "0-3\n" && text(*cpu->lookup("present")) == "0-3\n");
    CHECK(text(*cpu->lookup("online")) == "0-1,3\n" && text(*cpu->lookup("offline")) == "2\n");
    u64 inode = 5;
    for (const auto name : {"kernel_max", "possible", "present", "online", "offline"})
        CHECK(attribute(*cpu->lookup(name), 0444, inode++, fs::ControlNodeKind::attribute));
    // cpu0/ exists without online; cpuN/online for N >= 1.
    auto *cpu0 = cpu->lookup("cpu0");
    CHECK(cpu0 && cpu0->metadata().mode == (fs::mode_directory | 0555) && names(*cpu0, "") == 0);
    CHECK(!cpu0->lookup("online") && cpu0->lookup("..") == cpu);
    for (u16 n = 1; n < 4; ++n)
    {
        const std::array<char, 5> name{'c', 'p', 'u', static_cast<char>('0' + n), 0};
        auto *directory = cpu->lookup(std::string_view(name.data(), 4));
        CHECK(directory && names(*directory, "online") == 1);
        auto *online = directory->lookup("online");
        CHECK(online && attribute(*online, 0644, 32 + n, fs::ControlNodeKind::cpu_online));
        CHECK(online->control_node().index == n);
        CHECK(text(*online) == (n == 2 ? "0\n" : "1\n"));
        ++nodes;
    }
    // A partial read at an offset, and the end of file.
    auto partial = cpu->lookup("online")->read(2, 2);
    CHECK(partial && partial.value().size == 2 && partial.value().data[0] == std::byte{'1'});
    CHECK(cpu->lookup("online")->read(6, 10).value().size == 0);
    // Strict lookup: absent CPUs, leading zeros and unknown names.
    for (const auto name : {"cpu4", "cpu01", "cpu", "cpu16", "online0", ""}) CHECK(!cpu->lookup(name));
    // Nodes are fixed.
    CHECK(cpu->create("x", fs::NodeType::regular).code() == StatusCode::denied);
    CHECK(cpu->create_child("x", {}).status().code() == StatusCode::denied);
    CHECK(cpu->chmod(0700).code() == StatusCode::permission);
    CHECK(cpu->lookup("online")->chown(1, 1).code() == StatusCode::permission);
    CHECK(cpu->lookup("online")->truncate(0));
    const std::array<std::byte, 1> one{std::byte{'1'}};
    CHECK(cpu->lookup("online")->write(0, one).code() == StatusCode::denied);
    CHECK(cpu->lookup("cpu1")->lookup("online")->write(0, one).code() == StatusCode::unsupported);
    // Directory paths for getcwd, and the parents.
    CHECK(cpu->directory_path(sysfs.root()).value().view() == "/devices/system/cpu");
    CHECK(cpu0->directory_path(*cpu).value().view() == "/cpu0");
    CHECK(sysfs.root().directory_path(sysfs.root()).value().empty());
    CHECK(sysfs.root().directory_path(*cpu).status().code() == StatusCode::not_found);
    CHECK(cpu->lookup("..")->lookup("..")->lookup("..") == &sysfs.root() && sysfs.root().lookup("..") == &sysfs.root());
    names(sysfs.root(), "devices");
    return nodes;
}

// A UP profile: online = present = possible = "0", cpu0/ without online.
void up_tree(fs::CpuSysFileSystem &sysfs)
{
    current_view = {.present = 1, .online = 1};
    auto *cpu = walk(sysfs, {"devices", "system", "cpu"});
    names(*cpu, "kernel_max possible present online offline cpu0");
    CHECK(text(*cpu->lookup("possible")) == "0\n" && text(*cpu->lookup("online")) == "0\n");
    CHECK(text(*cpu->lookup("offline")) == "\n" && !cpu->lookup("cpu1"));
    CHECK(!cpu->lookup("cpu0")->lookup("online"));
}

// A wedged stop: the CPU stays present and reads offline; a CPU whose start
// failed is absent.
void wedged_tree(fs::CpuSysFileSystem &sysfs)
{
    current_view = {.present = 0b1011, .online = 0b0011};
    auto *cpu = walk(sysfs, {"devices", "system", "cpu"});
    names(*cpu, "kernel_max possible present online offline cpu0 cpu1 cpu3");
    CHECK(text(*cpu->lookup("present")) == "0-1,3\n" && text(*cpu->lookup("offline")) == "3\n");
    CHECK(text(*cpu->lookup("cpu3")->lookup("online")) == "0\n" && !cpu->lookup("cpu2"));
}

struct Fixture
{
    fs::CpuSysFileSystem sysfs{&fake_view}; // Outlives the VFS, which borrows its root.
    sched::Scheduler scheduler;
    fs::VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService posix;
};

// kernfs open semantics through the actual open path.
usize opens()
{
    current_view = {.present = 0b11, .online = 0b11};
    auto f = std::make_unique<Fixture>();
    CHECK(f->console.start() && f->posix.initialize(f->vfs, f->console, f->scheduler));
    CHECK(f->vfs.create({.credentials = {}}, "/sys", fs::NodeType::directory, 0755, 0, 0));
    CHECK(f->vfs.mount("/sys", f->sysfs.root()));
    const auto root = f->scheduler.create_process("root", {});
    const auto user = f->scheduler.create_process("user", {});
    CHECK(root && user);
    f->scheduler.find(user.value())->set_credentials({.uid = 1000, .gid = 1000, .euid = 1000, .egid = 1000});
    constexpr u32 rdonly = 0, wronly = 1, rdwr = 2, creat = 0100, trunc = 01000;
    auto open = [&](sched::ProcessId pid, std::string_view path, u32 flags) {
        CHECK(f->posix.activate(pid));
        return f->posix.open(path, flags, 0644);
    };
    auto control = [&](sched::ProcessId pid, posix::Fd fd) {
        const auto *slot = f->scheduler.find(pid)->fd_table().entry(fd);
        return f->scheduler.open_description(slot->description)->control;
    };
    usize refused = 0;
    // A read-only attribute opened for writing: EACCES even for root.
    for (const auto flags : {wronly, rdwr, wronly | creat | trunc})
    {
        const auto opened = open(root.value(), "/sys/devices/system/cpu/online", flags);
        CHECK(!opened && opened.status().code() == StatusCode::denied);
        ++refused;
    }
    const auto read = open(root.value(), "/sys/devices/system/cpu/online", rdonly);
    CHECK(read && control(root.value(), read.value()).kind == fs::ControlNodeKind::attribute);
    // cpu0/online: ENOENT without O_CREAT, EACCES with it (the directory refuses create).
    const auto absent = open(root.value(), "/sys/devices/system/cpu/cpu0/online", wronly);
    CHECK(!absent && absent.status().code() == StatusCode::not_found);
    const auto created = open(root.value(), "/sys/devices/system/cpu/cpu0/online", wronly | creat | trunc);
    CHECK(!created && created.status().code() == StatusCode::denied);
    refused += 2;
    // Root opens cpu1/online like a shell redirection; the description carries the control node.
    const auto written = open(root.value(), "/sys/devices/system/cpu/cpu1/online", wronly | creat | trunc);
    CHECK(written);
    const auto kind = control(root.value(), written.value());
    CHECK(kind.kind == fs::ControlNodeKind::cpu_online && kind.index == 1);
    // uid 1000: reads everything, writes nothing.
    const auto user_read = open(user.value(), "/sys/devices/system/cpu/cpu1/online", rdonly);
    CHECK(user_read && control(user.value(), user_read.value()).kind == fs::ControlNodeKind::cpu_online);
    const auto user_write = open(user.value(), "/sys/devices/system/cpu/cpu1/online", wronly);
    CHECK(!user_write && user_write.status().code() == StatusCode::denied);
    const auto user_create = open(user.value(), "/sys/devices/system/cpu/cpu1/new", wronly | creat);
    CHECK(!user_create && user_create.status().code() == StatusCode::denied);
    refused += 2;
    // An ordinary file elsewhere has no control node.
    CHECK(f->vfs.create({.credentials = {}}, "/plain", fs::NodeType::regular, 0644, 0, 0));
    const auto plain = open(root.value(), "/plain", wronly);
    CHECK(plain && control(root.value(), plain.value()).kind == fs::ControlNodeKind::none);
    const auto listing = f->vfs.list("/sys/devices/system/cpu");
    CHECK(listing && listing.value().count == 7);
    return refused;
}
} // namespace

int main()
{
    current_view = {.present = 1, .online = 1};
    CHECK(constant_default.view() == current_view);
    const usize nodes = smp_tree(constant_model);
    up_tree(constant_model);
    wedged_tree(constant_default);
    const usize refused = opens();
    std::printf("SYSFS_CPU_MODEL_PASS nodes=%zu refused_opens=%zu cpulist_max=%zu constinit=1 guest=0\n", nodes, refused,
                fs::cpulist(0x5555).size);
    std::printf("checks=%zu\n", checks);
}
