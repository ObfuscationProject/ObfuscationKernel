// Hosted real VFS/POSIX/provider composition. No user instructions or hardware.
#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <ok/abi/linux_chmod_call.hpp>
#include <ok/abi/linux_stat.hpp>
#include <memory>

using namespace owned_fixture;
namespace clock_model = testing::ext4_clock;
using Mount = Ext4MountedMetadata<1>;
using Change = ModeChangeResult;
usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL journalled mode %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

time::RealtimeSample sample(i64 seconds = 1700000077, u32 nanos = 123456789)
{ return {{seconds, nanos}, {200, 1}, 1, time::RealtimeOrigin::administrator_software}; }
user::Credentials owner(u32 group = 1001)
{ return {.uid = 71, .gid = 72, .euid = 1000, .egid = group}; }
struct Mounted
{
    Disk disk;
    Mount::Registry registry;
    Mount mount;
    Mount::Registry::Registration key;
    explicit Mounted(const Bytes &bytes, Ext4MountedPolicy policy = Ext4MountedPolicy::maintained_noatime)
        : disk(bytes), mount(policy)
    {
        auto registration = registry.register_device(disk, 17); CHECK(registration); key = registration.value();
        CHECK(mount.mount(registry, key, {0, bytes.size() / 512}));
        // The dirty policy durably marks an initially clean superblock.
        CHECK(policy == Ext4MountedPolicy::ordered_data_dirty_noatime || (!disk.writes && !disk.flushes));
    }
};
struct Services
{
    sched::Scheduler scheduler;
    VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService service;
    u32 pid{};
    explicit Services(Node &root)
    {
        CHECK(console.start() && service.initialize(vfs, console, scheduler));
        auto p = scheduler.create_user_process("journal-mode", arch::configured_arch_operations().make_user_context({0x400000,0x800000,0}));
        CHECK(p && service.activate(p.value())); pid = p.value();
        CHECK(vfs.create("/journal", NodeType::directory) && vfs.create("/alias", NodeType::directory));
        CHECK(vfs.mount("/journal", root) && vfs.mount("/alias", root));
        CHECK(service.set_credentials(owner()));
    }
    ~Services()
    {
        CHECK(service.activate(pid));
        auto &resources = scheduler.find(pid)->resources();
        for (i32 fd = 3; fd < static_cast<i32>(posix::max_open_files); ++fd)
            if (resources.files.contains(fd)) CHECK(service.close(fd));
        if (resources.cwd_node) { CHECK(resources.cwd_node->release()); resources.cwd_node = nullptr; }
    }
};
void unchanged(Mounted &m, const Bytes &bytes, usize writes, usize flushes)
{
    CHECK(m.disk.live == bytes && m.disk.durable == bytes && m.disk.writes == writes && m.disk.flushes == flushes);
    CHECK(m.mount.report().phase == Ext4MountPhase::readable && !m.mount.buffers().read_pins);
}
void expect_update(Mounted &m, const Bytes &before, u16 mode, i64 seconds = 1700000077, u32 nanos = 123456789)
{
    Ext4InodeMetadataUpdate update{12, 1000, 1001, static_cast<u32>(seconds), nanos, mode};
    CHECK(m.disk.durable == mounted_completion(mounted_publication(before, update), 12));
    CHECK(m.disk.live == m.disk.durable && m.mount.report().phase == Ext4MountPhase::readable);
}
template <abi::linux_user::Profile P>
void codec(const posix::FileStatus &value)
{
    using namespace abi::linux_user;
    std::array<u8,256> out{};
    CHECK(StatCodec<P>::encode(value, StatLayout::statx, out));
    auto load = [&](usize offset, usize count) {
        u64 v{}; for (usize i = 0; i < count; ++i)
            v |= u64{out[offset + i]} << (8 * (Codec<P>::profile.big_endian ? count - i - 1 : i));
        return v;
    };
    CHECK(load(0,4) == 0x7ff && load(28,2) == value.mode && load(32,8) == value.attributes.inode);
    CHECK(load(48,8) == value.blocks && load(96,8) == static_cast<u64>(value.attributes.change.seconds));
    CHECK(load(104,4) == value.attributes.change.nanoseconds);
    auto invalid = value; invalid.attributes.valid &= ~static_cast<u32>(Attribute::blocks512);
    out.fill(0xa5); CHECK(!StatCodec<P>::encode(invalid, StatLayout::statx, out));
    CHECK(std::all_of(out.begin(), out.end(), [](u8 x) { return x == 0xa5; }));
    invalid = value; invalid.attributes.change.source = TimestampSource::unavailable;
    CHECK(!StatCodec<P>::encode(invalid, StatLayout::statx, out));
    invalid = value; invalid.attributes.change.source = TimestampSource::readonly_ext4;
    CHECK(!StatCodec<P>::encode(invalid, StatLayout::statx, out));
    if constexpr (StatCodec<P>::has_kernel_stat)
        CHECK(StatCodec<P>::encode(value, StatLayout::kernel_stat, std::span<u8>{out}.first(StatCodec<P>::size(StatLayout::kernel_stat))));
}
template <abi::linux_user::Profile P>
void adapter(Mounted &m, Services &s, i32 fd)
{
    using namespace abi::linux_user;
    auto read = [](u64 address, FixedString<256> &path) -> Status {
        if (address == 0x1000) return path.assign("/alias/seed");
        if (address == 0x2000) return path.assign("");
        return Status::fault();
    };
    auto change = [&](ChmodRequest request, std::string_view path) {
        return request.direct_fd ? s.service.change_mode_fd(request.fd, request.permissions) :
            s.service.change_mode_at(request.fd, path, request.permissions, request.flags);
    };
    auto before = m.disk.live;
    const std::array<u64,3> at{0xffffff9cu,0x1000,06750};
    CHECK(ChmodCallAdapter<P>::execute(ChmodCall::fchmodat, at, read, change) == 0);
    expect_update(m, before, 06750); before = m.disk.live;
    const std::array<u64,4> empty{static_cast<u32>(fd),0x2000,0644,0x1000};
    CHECK(ChmodCallAdapter<P>::execute(ChmodCall::fchmodat2, empty, read, change) == 0);
    expect_update(m, before, 0644);
    auto file = s.service.fstat(fd); CHECK(file); codec<P>(file.value());
    const auto writes = m.disk.writes, flushes = m.disk.flushes;
    const std::array<u64,4> bad{0xffffff9cu,0,0600,0x1000};
    CHECK(ChmodCallAdapter<P>::execute(ChmodCall::fchmodat2, bad, read, change) == -14);
    CHECK(m.disk.writes == writes && m.disk.flushes == flushes);
}
void operations(const Bytes &clean)
{
    clock_model::reset(); auto m = std::make_unique<Mounted>(clean); CHECK(!clock_model::calls());
    auto *root = m->mount.root(); CHECK(root);
    auto s = std::make_unique<Services>(*root); auto &service = s->service;
    auto before = service.stat("/journal/seed"); CHECK(before);
    CHECK(before.value().blocks == 8 && before.value().attributes.has(Attribute::blocks512));
    CHECK(before.value().attributes.access.source == TimestampSource::maintained_ext4);
    const auto fd = service.open("/journal/seed", posix::o_RDONLY); CHECK(fd);
    const auto dup = service.duplicate(fd.value()); CHECK(dup);
    auto child = service.fork(); CHECK(child);
    clock_model::set(sample()); auto original = m->disk.live;
    CHECK(service.change_mode_at(posix::at_FDCWD,"/alias/seed",06754,0).ok()); expect_update(*m,original,06754);
    auto after = service.fstat(dup.value()); CHECK(after);
    CHECK(after.value().attributes.access.seconds == before.value().attributes.access.seconds && after.value().attributes.access.nanoseconds == before.value().attributes.access.nanoseconds && after.value().attributes.modification.seconds == before.value().attributes.modification.seconds && after.value().attributes.modification.nanoseconds == before.value().attributes.modification.nanoseconds);
    CHECK(after.value().attributes.device == before.value().attributes.device && after.value().attributes.inode == before.value().attributes.inode);
    CHECK(service.activate(child.value())); CHECK(service.fstat(fd.value()).value().mode == 0106754);
    CHECK(service.close(fd.value()) && service.close(dup.value())); CHECK(service.activate(s->pid));
    original = m->disk.live; const auto epoch = m->mount.report().epoch;
    CHECK(service.change_mode_fd(fd.value(),06754).ok()); expect_update(*m,original,06754);
    CHECK(m->mount.report().epoch == epoch + 1);
    CHECK(service.set_credentials(owner(72))); original = m->disk.live;
    CHECK(service.change_mode_fd(fd.value(),06754).ok()); expect_update(*m,original,04754);
    CHECK(service.set_credentials(user::root_credentials())); original = m->disk.live;
    CHECK(service.change_mode_fd(fd.value(),06754).ok()); expect_update(*m,original,06754);
    CHECK(service.set_credentials(owner()));
    auto dir = service.open("/journal",posix::o_RDONLY); CHECK(dir); original = m->disk.live;
    CHECK(service.change_mode_at(dir.value(),"seed",0600,0).ok()); expect_update(*m,original,0600);
    auto seed_node = root->lookup_child("seed"); CHECK(seed_node);
    const auto writes = m->disk.writes, flushes = m->disk.flushes; original = m->disk.live;
    CHECK(!seed_node.value()->chmod(0777)); CHECK(!m->mount.update_metadata(update));
    CHECK(service.change_mode_at(posix::at_FDCWD,"/journal",0600,0).kind() == Change::Kind::unavailable);
    CHECK(service.change_mode_at(posix::at_FDCWD,"/journal/seed-link",0600,posix::at_SYMLINK_NOFOLLOW).kind() == Change::Kind::unavailable);
    unchanged(*m,original,writes,flushes);
    clock_model::fail(); const auto calls = clock_model::calls();
    CHECK(service.set_credentials({.uid=1000,.gid=1001,.euid=71,.egid=1001}));
    auto denied = service.change_mode_fd(fd.value(),0600); CHECK(denied.status().code() == StatusCode::permission);
    CHECK(clock_model::calls() == calls); unchanged(*m,original,writes,flushes);
    CHECK(service.set_credentials(owner()));
    CHECK(service.change_mode_fd(fd.value(),0600).status().code() == StatusCode::io_error); unchanged(*m,original,writes,flushes);
    for (i64 seconds : {-1LL, 2147483648LL}) {
        clock_model::set(sample(seconds)); CHECK(service.change_mode_fd(fd.value(),0600).status().code() == StatusCode::overflow);
        unchanged(*m,original,writes,flushes);
    }
    for (unsigned bad = 0; bad != 4; ++bad) {
        auto value = sample();
        if (bad == 0) value.value.nanoseconds = 1000000000;
        if (bad == 1) value.wall_generation = 0;
        if (bad == 2) value.monotonic.generation = 0;
        if (bad == 3) value.origin = time::RealtimeOrigin::none;
        clock_model::set(value); CHECK(service.change_mode_fd(fd.value(),0600).status().code() == StatusCode::io_error);
        unchanged(*m,original,writes,flushes);
    }
    for (i64 seconds : {0LL, 2147483647LL}) {
        clock_model::set(sample(seconds,999999999)); original = m->disk.live;
        CHECK(service.change_mode_fd(fd.value(),0600).ok()); expect_update(*m,original,0600,seconds,999999999);
    }
    clock_model::set(sample());
    bool reentered{}; m->disk.hook = [&](char) {
        reentered = true;
        CHECK(root->query_metadata().status().code() == StatusCode::busy);
        CHECK(seed_node.value()->change_mode(0600,owner()).status().code() == StatusCode::busy);
    };
    original = m->disk.live; CHECK(service.change_mode_fd(fd.value(),0644).ok()); m->disk.hook = {};
    CHECK(reentered); expect_update(*m,original,0644);
#define TARGET(p) adapter<abi::linux_user::Profile::p>(*m,*s,fd.value())
    TARGET(i386); TARGET(x86_64); TARGET(aarch64); TARGET(arm32); TARGET(rv64);
    TARGET(rv32); TARGET(loongarch64); TARGET(mips); TARGET(mips64); TARGET(ppc);
#undef TARGET
    clock_model::fail(); const auto noatime = m->disk.live; const auto wc = m->disk.writes, fc = m->disk.flushes;
    CHECK(service.stat("/journal/seed") && root->list() && seed_node.value()->read(0,4));
    unchanged(*m,noatime,wc,fc);
}
void admission(const Bytes &clean)
{
    clock_model::reset();
    auto legacy = std::make_unique<Mounted>(clean,Ext4MountedPolicy::maintenance_only);
    const auto admission_reads = legacy->disk.reads;
    // The legacy and maintained paths are identical before the added scan.
    // Inject at its first actual transport read, after ordinary mount admission.
    for (auto error : {Status::not_found(),Status::unsupported(),Status::overflow(),Status::permission()}) {
        Disk disk(clean); Mount::Registry registry;
        auto mount = std::make_unique<Mount>(Ext4MountedPolicy::maintained_noatime);
        auto key = registry.register_device(disk,17); CHECK(key);
        disk.fail_read = admission_reads + 1; disk.read_error = error;
        CHECK(!mount->mount(registry,key.value(),{0,clean.size()/512}));
        CHECK(disk.reads == admission_reads + 1 && mount->report().phase == Ext4MountPhase::failed && !mount->root());
        CHECK(disk.live == clean && disk.durable == clean && !disk.writes && !disk.flushes);
        CHECK(!registry.detach(key.value()));
    }
    auto *root = legacy->mount.root(); auto seed_node = root->lookup_child("seed"); CHECK(seed_node);
    CHECK(seed_node.value()->change_mode(0600,owner()).kind() == Change::Kind::unavailable);
    CHECK(!seed_node.value()->query_metadata().value().attributes.has(Attribute::blocks512));
    CHECK(legacy->mount.update_metadata(update)); CHECK(!clock_model::calls());
    for (u32 number : {2u,12u,13u}) for (usize field : {132u,136u,140u}) {
        auto bytes = clean; const auto at = inode_offset(bytes,number);
        w32(bytes,at+field,1000000000u<<2); inode_crc(bytes,number,at);
        Disk disk(bytes); Mount::Registry registry; auto mount = std::make_unique<Mount>(Ext4MountedPolicy::maintained_noatime);
        auto key = registry.register_device(disk,17); CHECK(key);
        CHECK(!mount->mount(registry,key.value(),{0,bytes.size()/512}) && !mount->root());
        CHECK(disk.live==bytes && !disk.writes && !disk.flushes);
        if (number != 2) { auto allowed = std::make_unique<Mounted>(bytes,Ext4MountedPolicy::maintenance_only); CHECK(allowed->mount.root()); }
    }
    auto wide = clean; auto at = inode_offset(wide,12);
    w32(wide,at+8,0xffffffff); w32(wide,at+140,0); w32(wide,at+16,0x80000000); w32(wide,at+136,1);
    inode_crc(wide,12,at); auto m = std::make_unique<Mounted>(wide);
    auto node = m->mount.root()->lookup_child("seed"); CHECK(node);
    auto metadata = node.value()->query_metadata(); CHECK(metadata);
    CHECK(metadata.value().attributes.access.seconds == -1 && metadata.value().attributes.modification.seconds == 2147483648LL);
    CHECK(!clock_model::calls());
}
void failures(const Bytes &clean)
{
    clock_model::set(sample()); usize reads{};
    {
        auto m = std::make_unique<Mounted>(clean); auto node = m->mount.root()->lookup_child("seed"); CHECK(node);
        const auto before = m->disk.reads; CHECK(node.value()->change_mode(0644,owner()).ok()); reads = m->disk.reads - before;
        CHECK(m->disk.writes == 11 && m->disk.flushes == 9);
    }
    for (char kind : {'r','w','f'}) for (usize point = 1; point <= (kind == 'r' ? reads : kind == 'w' ? 11 : 9); ++point) {
        auto m = std::make_unique<Mounted>(clean); auto *root = m->mount.root(); auto node = root->lookup_child("seed"); CHECK(node);
        CHECK(node.value()->retain());
        if (kind == 'r') m->disk.fail_read = m->disk.reads + point;
        if (kind == 'w') m->disk.fail_write = point;
        if (kind == 'f') m->disk.fail_flush = point;
        CHECK(!node.value()->change_mode(0644,owner()).ok()); CHECK(m->mount.report().phase == Ext4MountPhase::failed);
        const auto calls = m->disk.reads + m->disk.writes + m->disk.flushes;
        CHECK(!root->query_metadata() && !node.value()->change_mode(0644,owner()).ok());
        CHECK(m->disk.reads + m->disk.writes + m->disk.flushes == calls); CHECK(node.value()->release());
    }
    for (auto status : {Status::not_found(),Status::unsupported(),Status::overflow(),Status::permission()}) {
        auto m = std::make_unique<Mounted>(clean); auto node = m->mount.root()->lookup_child("seed"); CHECK(node);
        // The first uncached operation read is authoritative transport, regardless of Status spelling.
        m->disk.fail_read = m->disk.reads + 1; m->disk.read_error = status;
        CHECK(!node.value()->change_mode(0644,owner()).ok()); CHECK(m->mount.report().phase == Ext4MountPhase::failed);
    }
}
int main(int argc,char **argv)
{
    CHECK(argc==2); std::ifstream input(argv[1],std::ios::binary); CHECK(input);
    Bytes clean((std::istreambuf_iterator<char>(input)),{}); CHECK(!clean.empty());
    operations(clean); admission(clean); failures(clean);
    std::printf("PASS journalled checked mode checks=%zu profiles=10 hosted=1 user_execution=0 hardware=0\n",checks);
    return 0;
}
