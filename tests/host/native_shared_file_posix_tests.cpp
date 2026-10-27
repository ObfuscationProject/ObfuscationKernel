// Actual POSIX/OFD ownership with hosted frame aliases; no native PTE execution.
#include "ok/posix/posix.hpp"
#include "ok/user/native_shared_files.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
using namespace ok;
using Store = user::native::SharedMemoryFiles;
static usize checks{};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (false)
std::span<const std::byte> bytes(std::string_view text)
{ return {reinterpret_cast<const std::byte *>(text.data()), text.size()}; }
struct Fixture
{
    alignas(4096) std::array<std::byte, 32 * 4096> backing{};
    memory::FrameAllocator frames;
    memory::FrameMetadataStorage<2048> frames_metadata;
    user::native::NativePagePool pool;
    Store store, alternative;
    sched::Scheduler scheduler;
    fs::VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService service;
    Fixture()
    {
        const memory::MemoryRegion region{reinterpret_cast<uptr>(backing.data()), backing.size(), memory::RegionType::usable};
        CHECK(frames.initialize({&region, 1}, 4096, frames_metadata.bytes));
        CHECK(pool.activate(frames, user::native::NativePagePool::Source::linker_reserved, 2));
        CHECK(store.bind(pool) && alternative.bind(pool));
        CHECK(scheduler.set_shared_memory_files(store));
        CHECK(console.start() && service.initialize(vfs, console, scheduler));
        CHECK(service.set_identity(1200, 1300));
    }
    ~Fixture()
    {
        CHECK(scheduler.drain_all_resources());
        const auto ledger = store.ledger();
        CHECK(!ledger.files && !ledger.aliases && !ledger.retained_frames && !ledger.reserved_cells);
        CHECK(pool.used_count() == 0);
    }
    user::native::SharedFileHandle handle(posix::Fd fd)
    {
        const auto *process = scheduler.find(service.getpid());
        const auto *slot = process ? process->fd_table().entry(fd) : scheduler.boot_resources().files.entry(fd);
        CHECK(slot && slot->used);
        const auto *description = scheduler.open_description(slot->description);
        CHECK(description && description->native_shared_file);
        return description->native_shared_file;
    }
    posix::Fd file()
    {
        auto made = service.memfd_create("English notes buffer", posix::mfd_ALLOW_SEALING | posix::mfd_CLOEXEC);
        CHECK(made); return made.value();
    }
};
template<class Function> void test(Function function)
{
    alignas(Fixture) static std::array<std::byte, sizeof(Fixture)> storage{};
    auto &fixture = *::new(storage.data()) Fixture;
    function(fixture);
    fixture.~Fixture();
}
int main()
{
    test([](Fixture &f) {
        const auto fd = f.file(); CHECK(f.service.ftruncate(fd, 8192));
        const auto duplicate = f.service.duplicate(fd).value();
        CHECK(f.service.pwrite(fd, bytes("Notes saved\n"), 0).value() == 12);
        CHECK(f.service.pwrite(fd, bytes("English tail"), 4092).value() == 12);
        CHECK(f.service.fcntl(fd, posix::f_ADD_SEALS, 14));
        CHECK(f.service.seek(fd, 19, posix::SeekWhence::set).value() == 19);
        const auto handle = f.handle(fd);
        const auto frame = f.store.page(handle, 0).value();
        CHECK(f.store.retain_alias(frame));
        CHECK(f.service.fcntl(duplicate, posix::f_ADD_SEALS, 8));
        const posix::IoVector vectors[]{{reinterpret_cast<uptr>("Changed"), 7},
            {reinterpret_cast<uptr>("again"), 5}};
        CHECK(f.service.write(duplicate, bytes("Changed")).status().code() == StatusCode::permission);
        CHECK(f.service.pwrite(fd, bytes("Changed"), 0).status().code() == StatusCode::permission);
        CHECK(f.service.writev(fd, vectors).status().code() == StatusCode::permission);
        CHECK(f.service.pwrite_with_policy<posix::PositionedWritePolicy::linux_append>(
            fd, bytes("Changed"), 0, 8192).status().code() == StatusCode::permission);
        CHECK(f.service.seek(duplicate, 0, posix::SeekWhence::current).value() == 19);
        CHECK(f.service.write(fd, {}).value() == 0 && f.service.pwrite(fd, {}, 0).value() == 0);
        const posix::IoVector empty[]{{0, 0}, {0, 0}};
        CHECK(f.service.writev(fd, empty).value() == 0);
        CHECK(f.service.seek(duplicate, 0, posix::SeekWhence::current).value() == 19);
        CHECK(f.service.fcntl(fd, posix::f_SETFL, posix::o_APPEND));
        CHECK(f.service.write(fd, bytes("Changed")).status().code() == StatusCode::permission);
        CHECK(f.service.write(fd, {}).value() == 0);
        CHECK(f.service.seek(duplicate, 0, posix::SeekWhence::current).value() == 19);
        std::array<std::byte, 12> output{};
        CHECK(f.service.pread(fd, output, 0).value() == 12 && std::memcmp(output.data(), "Notes saved\n", 12) == 0);
        CHECK(f.service.pread(fd, output, 4092).value() == 12 && std::memcmp(output.data(), "English tail", 12) == 0);
        CHECK(f.service.fcntl(fd, posix::f_ADD_SEALS, 1));
        CHECK(f.service.fcntl(fd, posix::f_GET_SEALS, 0).value() == 15);
        CHECK(f.service.fcntl(fd, posix::f_ADD_SEALS, 0).status().code() == StatusCode::permission);
        CHECK(f.service.close(fd) && f.service.close(duplicate));
        CHECK(f.store.check_mapping_write(frame).code() == StatusCode::denied);
        CHECK(f.store.release_alias(frame));
    });
    test([](Fixture &f) {
        CHECK(!f.service.memfd_create("flags", 0));
        CHECK(!f.service.memfd_create("flags", posix::mfd_ALLOW_SEALING | 4));
        const auto fd = f.file();
        const auto handle = f.handle(fd);
        CHECK(f.store.ledger().files == 1 && !f.pool.used_count());
        const auto empty = f.service.fstat(fd).value();
        CHECK(empty.size == 0 && empty.uid == 1200 && empty.gid == 1300);
        CHECK(empty.link_count == 0 && empty.mode == fs::mode_for(fs::NodeType::regular, 0600));
        CHECK(empty.attributes.has(fs::Attribute::inode) && empty.attributes.inode != 0);
        CHECK(empty.provider == posix::FileStatusProvider::volatile_shared_file);
        CHECK(empty.attributes.valid == (static_cast<u32>(fs::Attribute::inode) | static_cast<u32>(fs::Attribute::blocks512)));
        CHECK(!empty.attributes.has(fs::Attribute::device) && !empty.attributes.has(fs::Attribute::access_time) &&
              !empty.attributes.has(fs::Attribute::modification_time) && !empty.attributes.has(fs::Attribute::change_time));
        CHECK(empty.attributes.device == fs::FilesystemDevice{});
        for (const auto *time : {&empty.attributes.access, &empty.attributes.modification, &empty.attributes.change})
            CHECK(time->source == fs::TimestampSource::unavailable && !time->seconds && !time->nanoseconds);
        CHECK(!f.scheduler.set_shared_memory_files(f.alternative));
        CHECK(!f.scheduler.allocate_description({.readable = true, .native_shared_file = handle}));
        CHECK(!f.scheduler.allocate_description({.readable = true, .console = true, .native_shared_file = handle}));
        CHECK(f.service.fcntl(fd, posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
        CHECK(f.service.fcntl(fd, posix::f_GETFL, 0).value() == posix::o_RDWR);
        CHECK(f.service.ftruncate(fd, 8192));
        CHECK(f.service.fstat(fd).value().blocks == 16 && f.service.fstat(fd).value().attributes.inode == empty.attributes.inode);
        CHECK(f.service.set_identity(99, 98));
        CHECK(f.service.fstat(fd).value().uid == 1200 && f.service.fstat(fd).value().gid == 1300);
        const auto duplicate = f.service.duplicate(fd).value();
        CHECK(f.handle(duplicate) == handle && f.store.ledger().files == 1);
        CHECK(f.service.fcntl(duplicate, posix::f_GETFD, 0).value() == 0);
        CHECK(f.service.write(fd, bytes("abcdef")).value() == 6);
        CHECK(f.service.seek(duplicate, 0, posix::SeekWhence::current).value() == 6);
        CHECK(f.service.pwrite(duplicate, bytes("XY"), 2).value() == 2);
        CHECK(f.service.seek(fd, 0, posix::SeekWhence::current).value() == 6);
        std::array<std::byte, 6> output{};
        CHECK(f.service.pread(fd, output, 0).value() == 6 && std::memcmp(output.data(), "abXYef", 6) == 0);
        CHECK(f.service.seek(fd, 0, posix::SeekWhence::set));
        CHECK(f.service.read(duplicate, output).value() == 6);
        CHECK(f.service.seek(fd, 0, posix::SeekWhence::current).value() == 6);
        CHECK(f.service.fcntl(fd, posix::f_ADD_SEALS, 2 | 16).status().code() == StatusCode::unsupported);
        CHECK(f.service.fcntl(fd, posix::f_GET_SEALS, 0).value() == 0);
        CHECK(f.service.fcntl(duplicate, posix::f_ADD_SEALS, 6));
        CHECK(f.service.fcntl(fd, posix::f_GET_SEALS, 0).value() == 6);
        CHECK(f.service.ftruncate(fd, 4096).code() == StatusCode::permission);
        CHECK(f.service.ftruncate(fd, 12288).code() == StatusCode::permission);
        CHECK(f.service.ftruncate(fd, 8192));
        const auto frame = f.store.page(handle, 0).value();
        CHECK(f.store.retain_alias(frame));
        reinterpret_cast<std::byte *>(frame.address)[0] = std::byte{'Z'};
        CHECK(f.service.pread(duplicate, output, 0).value() == output.size() && output[0] == std::byte{'Z'});
        CHECK(f.service.pwrite(fd, bytes("Q"), 0).value() == 1 && reinterpret_cast<char *>(frame.address)[0] == 'Q');
        CHECK(f.service.fsync(fd).code() == StatusCode::unsupported);
        CHECK(f.service.close(fd) && f.store.info(handle).value().description);
        // Closing one slot leaves the duplicate's OFD and alias live, but the
        // retired descriptor must report EBADF independently of the command.
        for (const u32 command : {posix::f_GETFD, posix::f_SETFD, posix::f_GETFL, posix::f_SETFL,
                                  posix::f_GET_SEALS, posix::f_ADD_SEALS, posix::f_DUPFD, ~u32{0}})
            CHECK(f.service.fcntl(fd, command, 0).status().code() == StatusCode::bad_descriptor);
        CHECK(f.service.fcntl(duplicate, posix::f_GETFD, 0).value() == 0);
        CHECK(f.service.fcntl(duplicate, posix::f_GET_SEALS, 0).value() == 6);
        CHECK(f.service.close(duplicate) && !f.store.info(handle).value().description);
        for (const posix::Fd invalid : {posix::Fd{-1}, static_cast<posix::Fd>(posix::max_open_files), duplicate})
            CHECK(f.service.fcntl(invalid, posix::f_GETFD, 0).status().code() == StatusCode::bad_descriptor);
        CHECK(!f.scheduler.set_shared_memory_files(f.alternative));
        CHECK(f.store.release_alias(frame));
        CHECK(f.scheduler.set_shared_memory_files(f.alternative));
        CHECK(f.scheduler.set_shared_memory_files(f.store));
    });
    test([](Fixture &f) {
        const auto fd = f.file(); CHECK(f.service.ftruncate(fd, 4096));
        const posix::IoVector vectors[]{{reinterpret_cast<uptr>("first"), 5}, {reinterpret_cast<uptr>("second"), 6}};
        CHECK(f.service.writev(fd, vectors).value() == 11);
        CHECK(f.service.seek(fd, 0, posix::SeekWhence::set));
        std::array<char, 5> first{}; std::array<char, 6> second{};
        const posix::IoVector outputs[]{{reinterpret_cast<uptr>(first.data()), first.size()},
            {reinterpret_cast<uptr>(second.data()), second.size()}};
        CHECK(f.service.readv(fd, outputs).value() == 11);
        CHECK(std::memcmp(first.data(), "first", 5) == 0 && std::memcmp(second.data(), "second", 6) == 0);
        CHECK(f.service.seek(fd, 0, posix::SeekWhence::set));
        const posix::IoVector malformed[]{{reinterpret_cast<uptr>("bad"), 3}, {0, 1}};
        CHECK(!f.service.writev(fd, malformed));
        CHECK(f.service.seek(fd, 0, posix::SeekWhence::current).value() == 0);
        const posix::IoVector wrapping[]{{reinterpret_cast<uptr>(first.data()), 1}, {~uptr{0} - 1, 3}};
        CHECK(!f.service.readv(fd, wrapping));
        CHECK(f.service.seek(fd, 0, posix::SeekWhence::current).value() == 0 && first[0] == 'f');
        CHECK(f.service.fcntl(fd, posix::f_ADD_SEALS, 6));
        CHECK(f.service.fcntl(fd, posix::f_SETFL, posix::o_APPEND));
        CHECK(f.service.write(fd, {}).value() == 0);
        CHECK(f.service.seek(fd, 0, posix::SeekWhence::current).value() == 0);
        CHECK(f.service.pwrite_with_policy<posix::PositionedWritePolicy::linux_append>(fd, {}, 0, 0).value() == 0);
        CHECK(f.service.seek(fd, 0, posix::SeekWhence::current).value() == 0);
        CHECK(f.service.fcntl(fd, posix::f_SETFL, 0));
        CHECK(f.service.pwrite(fd, bytes("tail"), 4095).value() == 1);
        CHECK(f.service.pwrite(fd, bytes("x"), 4096).status().code() == StatusCode::permission);
        CHECK(f.service.pread(fd, {reinterpret_cast<std::byte *>(first.data()), first.size()}, 4096).value() == 0);
        CHECK(f.service.fcntl(fd, posix::f_ADD_SEALS, 1));
        CHECK(f.service.fcntl(fd, posix::f_ADD_SEALS, 0).status().code() == StatusCode::permission);
    });
    test([](Fixture &f) {
        std::array<posix::Fd, Store::capacity> fds{};
        for (auto &fd : fds) fd = f.file();
        CHECK(f.service.memfd_create("capacity", 2).status().code() == StatusCode::system_file_limit);
        CHECK(f.store.ledger().files == fds.size() && f.service.open_file_count() == fds.size() + 3);
        CHECK(f.service.close(fds[0]));
        CHECK(f.file() == fds[0]);
    });
    test([](Fixture &f) {
        const auto parent = f.scheduler.create_user_process("shared-file-fork",
            arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0}));
        CHECK(parent && f.service.activate(parent.value()));
        CHECK(f.service.set_identity(1400, 1500));
        const auto fd = f.file(); const auto handle = f.handle(fd);
        CHECK(f.service.ftruncate(fd, 4096));
        CHECK(f.service.write(fd, bytes("parent child")).value() == 12);
        CHECK(f.service.seek(fd, 0, posix::SeekWhence::set));
        const auto inode = f.service.fstat(fd).value().attributes.inode;
        const auto child = f.service.fork(); CHECK(child);
        CHECK(f.store.ledger().files == 1 && f.pool.user_count() == 1);
        CHECK(f.service.activate(child.value()) && f.handle(fd) == handle);
        CHECK(f.service.fstat(fd).value().attributes.inode == inode && f.service.fstat(fd).value().uid == 1400);
        std::array<std::byte, 6> output{};
        CHECK(f.service.read(fd, output).value() == 6 && std::memcmp(output.data(), "parent", 6) == 0);
        CHECK(f.service.activate(parent.value()));
        CHECK(f.service.seek(fd, 0, posix::SeekWhence::current).value() == 6);
        CHECK(f.service.close(fd) && f.store.info(handle).value().description);
        CHECK(f.service.activate(child.value()));
        CHECK(f.service.read(fd, output).value() == 6 && std::memcmp(output.data(), " child", 6) == 0);
        CHECK(f.service.close(fd) && !f.store.valid(handle));
        CHECK(f.service.activate(parent.value()));
    });
    test([](Fixture &f) {
        const auto fd = f.file();
        while (f.service.duplicate(fd)) {}
        CHECK(f.service.open_file_count() == posix::max_open_files);
        CHECK(f.service.memfd_create("fd full", 2).status().code() == StatusCode::process_file_limit);
        CHECK(f.store.ledger().files == 1 && !f.pool.used_count());
    });
    test([](Fixture &f) {
        std::array<u16, sched::max_open_descriptions> held{};
        usize count{};
        for (;;) {
            auto allocated = f.scheduler.allocate_description({.console = true});
            if (!allocated) break;
            held[count++] = allocated.value(); CHECK(f.scheduler.retain_description(allocated.value()));
        }
        CHECK(f.service.memfd_create("OFD full", 2).status().code() == StatusCode::system_file_limit);
        CHECK(!f.store.ledger().files && !f.store.ledger().reserved_cells && f.service.open_file_count() == 3);
        while (count) CHECK(f.scheduler.release_description(held[--count]));
        const auto fd = f.file(); CHECK(f.service.close(fd));
    });
    std::printf("PASS shared file POSIX ownership: checks=%zu hosted=1 actual_native_alias=0 guest=0\n", checks);
}
