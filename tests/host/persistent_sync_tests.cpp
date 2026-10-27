// Actual mounted provider/POSIX/native dispatch over hosted model transport.
#include "native_clock_model.hpp"
#include "ext4_mounted_support.hpp"
#include "ext4_mounted_retention_support.hpp"
#include "ext4_realtime_model.hpp"
#include <memory>

using namespace owned_fixture;
using Mount = fs::Ext4MountedMetadata<1>;
using N = syscall::Number;

namespace
{
sched::ProcessId pid{};
struct VolatileStore
{
    alignas(4096) std::array<std::byte, 32 * 4096> backing{};
    memory::FrameAllocator frames;
    memory::FrameMetadataStorage<2048> frames_metadata;
    native::NativePagePool pool;
    native::SharedMemoryFiles store;
    void bind(Kernel &kernel)
    {
        const memory::MemoryRegion region{reinterpret_cast<uptr>(backing.data()), backing.size(), memory::RegionType::usable};
        CHECK(frames.initialize({&region, 1}, 4096, frames_metadata.bytes));
        CHECK(pool.activate(frames, native::NativePagePool::Source::linker_reserved, 2));
        CHECK(store.bind(pool) && kernel.scheduler().set_shared_memory_files(store));
    }
} volatile_store;
i64 call(Kernel &kernel, std::array<uptr, 6> args)
{
    bool complete = true;
    const auto result = native::dispatch(kernel, pid, {OK_SYS_SYNC_PERSISTENT, args}, complete);
    CHECK(complete); return result;
}
struct Mounted
{
    Disk disk;
    Mount::Registry registry;
    Mount mount;
    fs::VfsMountHandle attachment{};
    Kernel &kernel;
    posix::Fd fd{-1};
    Mounted(Kernel &k, const Bytes &bytes, fs::Ext4MountedPolicy policy = fs::Ext4MountedPolicy::ordered_data_dirty_noatime)
        : disk(bytes), mount(policy), kernel(k)
    {
        const auto registration = registry.register_device(disk, 17); CHECK(registration);
        CHECK(mount.mount(registry, registration.value(), {0, bytes.size() / 512}));
        const auto published = publish_owned_root(mount, kernel.vfs(), "/persistent"); CHECK(published);
        attachment = published.value();
        const auto opened = kernel.posix().open("/persistent/seed", posix::o_RDWR); CHECK(opened);
        fd = opened.value();
    }
    void close()
    {
        if (fd >= 0) { CHECK(kernel.posix().close(fd)); fd = -1; }
        CHECK(kernel.vfs().detach(attachment));
    }
    void no_io_since(usize reads, usize writes, usize flushes) const
    { CHECK(disk.reads == reads && disk.writes == writes && disk.flushes == flushes); }
};
// VFS identity keys remain allocated until that namespace is destroyed.
std::vector<std::unique_ptr<Mounted>> retained_fixtures;

void unqualified(Kernel &kernel, const Bytes &bytes)
{
    CHECK(kernel.vfs().create("/volatile", fs::NodeType::regular));
    auto ram = kernel.posix().open("/volatile", posix::o_RDWR); CHECK(ram);
    CHECK(kernel.posix().fsync(ram.value()));
    CHECK(call(kernel, {static_cast<uptr>(ram.value())}) == -OK_EOPNOTSUPP);
    CHECK(call(kernel, {0}) == -OK_EOPNOTSUPP);
    CHECK(call(kernel, {0x7fffffffU}) == -9);
    CHECK(call(kernel, {static_cast<uptr>(-1)}) == -9);
    CHECK(kernel.syscalls().dispatch({N::sync_persistent, pid, {static_cast<u64>(-1)}}).status.code() == StatusCode::bad_descriptor);
    const auto pipe = kernel.posix().pipe(); CHECK(pipe);
    const auto sockets = kernel.posix().unix_socketpair(); CHECK(sockets);
    volatile_store.bind(kernel);
    const auto shared = kernel.posix().memfd_create("volatile-sync", posix::mfd_ALLOW_SEALING); CHECK(shared);
    for (const auto descriptor : {pipe.value()[0], pipe.value()[1], sockets.value()[0], sockets.value()[1], shared.value()})
    {
        CHECK(call(kernel, {static_cast<uptr>(descriptor)}) == -OK_EOPNOTSUPP);
        CHECK(kernel.posix().close(descriptor));
    }
    for (usize index = 1; index < 6; ++index)
    {
        std::array<uptr, 6> args{static_cast<uptr>(ram.value())}; args[index] = 1;
        CHECK(call(kernel, args) == -OK_EINVAL);
        const auto direct = kernel.syscalls().dispatch({N::sync_persistent, pid, args});
        CHECK(direct.status.code() == StatusCode::invalid_argument);
    }
    auto *process = kernel.scheduler().find(pid); CHECK(process);
    process->resources().personality = sched::UserPersonality::linux;
    CHECK(call(kernel, {static_cast<uptr>(ram.value())}) == -OK_ENOSYS);
    hw::UserFrame frame{}; frame.registers[14] = OK_SYS_SYNC_PERSISTENT;
    frame.registers[9] = static_cast<uptr>(ram.value()); bool complete = true;
    CHECK(user::linux_bootstrap::dispatch<lu::Profile::x86_64>(kernel, pid, frame, complete,
        [&](hw::Syscall request, bool &done) { return native::dispatch(kernel, pid, request, done); }) == -OK_ENOSYS);
    CHECK(complete);
    process->resources().personality = sched::UserPersonality::native;
    CHECK(kernel.posix().close(ram.value()));
    for (auto policy : {fs::Ext4MountedPolicy::maintenance_only, fs::Ext4MountedPolicy::maintained_noatime,
                        fs::Ext4MountedPolicy::ordered_data_noatime})
    {
        auto mounted = std::make_unique<Mounted>(kernel, bytes, policy);
        const auto reads = mounted->disk.reads, writes = mounted->disk.writes, flushes = mounted->disk.flushes;
        CHECK(call(kernel, {static_cast<uptr>(mounted->fd)}) == -OK_EOPNOTSUPP);
        mounted->no_io_since(reads, writes, flushes); mounted->close();
        retained_fixtures.push_back(std::move(mounted));
    }
}

void persistent(Kernel &kernel, const Bytes &bytes)
{
    auto mounted = std::make_unique<Mounted>(kernel, bytes);
    auto path = kernel.vfs().resolve({}, "/persistent/seed"); CHECK(path);
    auto *node = path.value().node();
    const std::array<std::byte, 4> payload{std::byte{'s'}, std::byte{'y'}, std::byte{'n'}, std::byte{'c'}};
    CHECK(node->write(100, payload));
    const auto durable = mounted->disk.durable;
    const auto writes = mounted->disk.writes, flushes = mounted->disk.flushes;
    usize callbacks{};
    mounted->disk.hook = [&](char operation) {
        const auto state = mounted->mount.lifetime_snapshot(12); CHECK(state);
        CHECK(state.value().holds[static_cast<usize>(fs::Ext4InodeHold::io)] == 1);
        CHECK(mounted->mount.report().reading);
        CHECK(node->sync_persistent().code() == StatusCode::busy);
        if (operation == 'f') ++callbacks;
    };
    CHECK(call(kernel, {static_cast<uptr>(mounted->fd)}) == 0);
    mounted->disk.hook = {};
    CHECK(callbacks == 1 && mounted->disk.flushes == flushes + 1 && mounted->disk.writes == writes);
    CHECK(mounted->disk.durable == durable && (r32(durable, 1120) & 4));
    const auto state = mounted->mount.lifetime_snapshot(12); CHECK(state);
    CHECK(state.value().holds[static_cast<usize>(fs::Ext4InodeHold::io)] == 0);
    CHECK(!mounted->mount.report().reading && mounted->mount.report().phase == fs::Ext4MountPhase::readable);
    CHECK(kernel.syscalls().dispatch({N::sync_persistent, pid, {static_cast<uptr>(mounted->fd)}}).status);
    const auto directory = kernel.posix().open("/persistent", posix::o_RDONLY | posix::o_DIRECTORY); CHECK(directory);
    const auto before_directory = mounted->disk.flushes;
    CHECK(call(kernel, {static_cast<uptr>(directory.value())}) == 0);
    CHECK(mounted->disk.flushes == before_directory + 1);
    CHECK(kernel.posix().close(directory.value()));
    CHECK(path.value().release()); mounted->close();
    retained_fixtures.push_back(std::move(mounted));
}

void failures(Kernel &kernel, const Bytes &bytes)
{
    for (usize failure = 0; failure < 4; ++failure)
    {
        auto mounted = std::make_unique<Mounted>(kernel, bytes);
        const auto flushes = mounted->disk.flushes;
        if (!failure) mounted->disk.fail_read = mounted->disk.reads + 1;
        else if (failure == 3) mounted->disk.fail_flush_after = flushes + 1;
        else
        {
            mounted->disk.fail_flush = flushes + 1;
            if (failure == 2) mounted->disk.flush_error = Status::unsupported("transport unsupported fault");
        }
        CHECK(call(kernel, {static_cast<uptr>(mounted->fd)}) == (failure == 2 ? -OK_EOPNOTSUPP : -OK_EIO));
        CHECK(mounted->mount.report().phase == fs::Ext4MountPhase::failed && !mounted->mount.report().reading);
        const auto state = mounted->mount.lifetime_snapshot(12); CHECK(state);
        CHECK(state.value().holds[static_cast<usize>(fs::Ext4InodeHold::io)] == 0);
        const auto reads = mounted->disk.reads, writes = mounted->disk.writes, after = mounted->disk.flushes;
        CHECK(call(kernel, {static_cast<uptr>(mounted->fd)}) < 0);
        mounted->no_io_since(reads, writes, after); mounted->close();
        retained_fixtures.push_back(std::move(mounted));
    }
}

#ifdef OK_PERSISTENT_SYNC_PRIVATE_TESTS
void adversarial(Kernel &kernel, const Bytes &bytes)
{
    for (usize fault = 0; fault < 6; ++fault)
    {
        auto mounted = std::make_unique<Mounted>(kernel, bytes);
        auto found = kernel.vfs().resolve({}, "/persistent/seed"); CHECK(found);
        auto *node = static_cast<Mount::MountedNode *>(found.value().node());
        const auto reads = mounted->disk.reads, writes = mounted->disk.writes, flushes = mounted->disk.flushes;
        if (fault == 0)
        {
            auto copied = *node;
            CHECK(!copied.sync_persistent());
        }
        else if (fault == 1)
        {
            CHECK(mounted->mount.lifetime_.mark_orphan(node->lifetime_key_));
            CHECK(!node->sync_persistent());
            for (auto &record : mounted->mount.lifetime_.records_)
                if (record.used && record.key == node->lifetime_key_)
                    record.phase = decltype(record.phase)::linked;
        }
        else if (fault == 2)
        {
            CHECK(found.value().release());
            CHECK(kernel.posix().close(mounted->fd)); mounted->fd = -1;
            CHECK(!node->sync_persistent());
        }
        else if (fault == 3)
        {
            static_cast<void>(mounted->mount.owner_.fail(Status::io_error("external abort")));
            CHECK(!node->sync_persistent());
        }
        else if (fault == 4)
        {
            ++node->lifetime_key_.session;
            CHECK(!node->sync_persistent());
            --node->lifetime_key_.session;
        }
        else
        {
            mounted->disk.hook = [&](char operation) { if (operation == 'f') ++node->lifetime_key_.session; };
            CHECK(!node->sync_persistent()); mounted->disk.hook = {};
            --node->lifetime_key_.session;
            CHECK(mounted->mount.report().phase == fs::Ext4MountPhase::failed);
            CHECK(mounted->disk.flushes == flushes + 1);
        }
        if (fault != 5) mounted->no_io_since(reads, writes, flushes);
        if (found.value().node()) CHECK(found.value().release());
        mounted->close();
        retained_fixtures.push_back(std::move(mounted));
    }
}
#endif
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    static_assert(OK_NATIVE_PERSISTENT_SYNC_VERSION == 1 && OK_UAPI_VERSION_MINOR == 16);
    static_assert(OK_SYS_SYNC_PERSISTENT == static_cast<u64>(N::sync_persistent));
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes bytes((std::istreambuf_iterator<char>(input)), {});
    testing::ext4_clock::set({{1700000077, 123456789}, {200, 1}, 1, time::RealtimeOrigin::administrator_software});
    static Kernel kernel; CHECK(kernel.boot({}));
    auto process = kernel.scheduler().create_user_process("persistent-sync-test", kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096})); CHECK(process);
    pid = process.value(); CHECK(kernel.scheduler().set_runnable(pid));
    auto *thread = &kernel.scheduler().find(pid)->threads()[0]; thread->native_active = true;
    CHECK(kernel.scheduler().activate_thread(pid, thread->tid)); CHECK(kernel.posix().activate(pid));
    CHECK(kernel.vfs().create("/persistent", fs::NodeType::directory));
    const std::string_view group = argv[2];
    if (group == "unqualified") unqualified(kernel, bytes);
    else if (group == "persistent") persistent(kernel, bytes);
    else if (group == "failures") failures(kernel, bytes);
#ifdef OK_PERSISTENT_SYNC_PRIVATE_TESTS
    else if (group == "adversarial") adversarial(kernel, bytes);
#endif
    else return 2;
    std::printf("PERSISTENT_SYNC_GROUP_PASS group=%s guest=0 physical=0\n", argv[2]);
}
