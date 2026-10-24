// Actual canonical OFD, UNIX queue and shared-file owners with hosted backing.
// Physical frame tokens are real allocator tokens; no target PTEs execute here.
#include "ok/posix/posix.hpp"
#include "ok/abi/linux_native.hpp"
#include "ok/user/native_shared_files.hpp"
#include "ok/user/unix_message.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
using namespace ok;
using Store = user::native::SharedMemoryFiles;
namespace lu = abi::linux_user;
static usize checks{};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (false)
namespace ok::sched
{
struct SchedulerIdentityTestAccess
{
    static usize pipes(const SchedulerState &owner)
    { usize count{}; for (const auto &pipe : owner.pipes_) if (pipe.used) ++count; return count; }
    static usize records(const SchedulerState &owner)
    { usize count{}; for (const auto &pipe : owner.pipes_) for (const auto &record : pipe.rights) if (record.used) ++count; return count; }
    static usize receives(const SchedulerState &owner) { return owner.active_unix_receives_; }
    static bool canonical_unix(const SchedulerState &owner, u16 id) { return owner.canonical_unix_description(id); }
};
}
using Access = sched::SchedulerIdentityTestAccess;
std::span<const std::byte> bytes(std::string_view text)
{ return {reinterpret_cast<const std::byte *>(text.data()), text.size()}; }
bool same_frame(const memory::PhysicalFrame &a, const memory::PhysicalFrame &b)
{ return a.address == b.address && a.index == b.index && a.owner == b.owner && a.generation == b.generation; }
struct Fixture
{
    alignas(4096) std::array<std::byte, 32 * 4096> backing{};
    memory::FrameAllocator frames;
    user::native::NativePagePool pool;
    Store store;
    sched::Scheduler scheduler;
    fs::VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService posix;
    Fixture()
    {
        const memory::MemoryRegion region{reinterpret_cast<uptr>(backing.data()), backing.size(), memory::RegionType::usable};
        CHECK(frames.initialize({&region, 1}, 4096));
        CHECK(pool.activate(frames, user::native::NativePagePool::Source::linker_reserved, 2));
        CHECK(store.bind(pool) && scheduler.set_shared_memory_files(store));
        CHECK(console.start() && posix.initialize(vfs, console, scheduler));
        baseline();
    }
    ~Fixture()
    {
        CHECK(scheduler.drain_all_resources());
        CHECK(!scheduler.open_description_count());
        CHECK(!Access::pipes(scheduler) && !Access::records(scheduler) && !Access::receives(scheduler));
        const auto ledger = store.ledger();
        CHECK(!ledger.files && !ledger.aliases && !ledger.retiring_files && !ledger.retained_frames && !ledger.reserved_cells);
        CHECK(ledger.first_cleanup_failure);
        CHECK(!pool.used_count() && !pool.pinned_count() && !pool.pending_count() && !pool.quarantined_kernel_page_count());
        CHECK(frames.free_frames() == frames.total_frames());
    }
    sched::ProcessResources &resources() { return scheduler.boot_resources(); }
    u16 id(i32 fd) { const auto *slot = resources().files.entry(fd); CHECK(slot && slot->used); return slot->description; }
    user::native::SharedFileHandle handle(i32 fd)
    { const auto *open = scheduler.open_description(id(fd)); CHECK(open && open->native_shared_file); return open->native_shared_file; }
    auto pair() { auto made = posix.unix_socketpair(posix::o_NONBLOCK); CHECK(made); return made.value(); }
    i32 file(usize pages = 1)
    {
        auto made = posix.memfd_create("English rights notes", posix::mfd_ALLOW_SEALING | posix::mfd_CLOEXEC);
        CHECK(made); CHECK(posix.ftruncate(made.value(), pages * 4096));
        CHECK(posix.fcntl(made.value(), posix::f_ADD_SEALS, Store::seal_shrink | Store::seal_grow));
        CHECK(!Access::canonical_unix(scheduler, id(made.value())));
        return made.value();
    }
    void idle()
    {
        CHECK(!resources().unix_receive_operations && !Access::receives(scheduler));
        for (const auto &slot : resources().files.entries()) CHECK(!slot.reservation);
    }
    void baseline()
    {
        idle(); CHECK(resources().files.open_count() == 3 && scheduler.open_description_count() == 3);
        CHECK(!Access::pipes(scheduler) && !Access::records(scheduler));
    }
    void ledger(usize files, usize aliases, usize pages)
    {
        const auto value = store.ledger();
        CHECK(value.files == files && value.aliases == aliases);
        CHECK(!value.retained_frames && !value.retiring_files && !value.reserved_cells && value.first_cleanup_failure);
        CHECK(pool.user_count() == pages && pool.used_count() == pages && !pool.object_count());
        CHECK(!pool.pinned_count() && !pool.pending_count());
        CHECK(frames.free_frames() + pages == frames.total_frames());
    }
};
template<class Function> void test(Function function)
{
    alignas(Fixture) static std::array<std::byte, sizeof(Fixture)> storage{};
    auto &fixture = *::new(storage.data()) Fixture;
    function(fixture); fixture.~Fixture();
}
void last_queue_owner_and_aliases()
{
    test([](Fixture &f) {
        const auto pair = f.pair(); const auto fd = f.file(2); const auto id = f.id(fd); const auto handle = f.handle(fd);
        const auto first = f.store.page(handle, 0).value(), second = f.store.page(handle, 1).value();
        CHECK(f.store.retain_alias(first) && f.store.retain_alias(second)); f.ledger(1, 2, 2);
        CHECK(f.posix.pwrite(fd, bytes("page crossing"), 4090).value() == 13);
        CHECK(std::memcmp(reinterpret_cast<const char *>(second.address), "rossing", 7) == 0);
        CHECK(f.posix.seek(fd, 4090, posix::SeekWhence::set));
        const std::array<i32, 2> rights{fd, fd}; std::array<std::byte, 32> output{};
        CHECK(f.posix.send_unix_rights(pair[0], bytes("handoff"), rights).value() == 7);
        CHECK(f.scheduler.open_description(id)->references == 3 && f.scheduler.open_description(id)->unix_queue_references == 2);
        CHECK(f.posix.close(fd)); CHECK(f.scheduler.open_description(id)->references == 2);
        CHECK(f.store.info(handle).value().description && Access::records(f.scheduler) == 1);
        {
            auto preview = f.posix.prepare_unix_receive(pair[1], output, 2, false, true); CHECK(preview);
            CHECK(preview.value().descriptors().size() == 2 && f.resources().unix_receive_operations == 1);
            CHECK(f.scheduler.open_description(id)->references == 4 && f.scheduler.open_description(id)->unix_queue_references == 2);
            CHECK(f.scheduler.drain_resources(f.resources()).code() == StatusCode::busy);
            sched::UnixReceive moved{std::move(preview.value())}; CHECK(!preview.value().commit()); moved.cancel(); moved.cancel();
        }
        f.idle(); CHECK(f.scheduler.open_description(id)->references == 2); f.ledger(1, 2, 2);
        auto receive = f.posix.prepare_unix_receive(pair[1], output, 2, false, true); CHECK(receive);
        const auto a = receive.value().descriptors()[0], b = receive.value().descriptors()[1];
        CHECK(receive.value().commit().value() == 7 && !receive.value().commit()); f.idle();
        CHECK(f.id(a) == id && f.id(b) == id && f.handle(a) == handle && f.handle(b) == handle);
        CHECK(f.scheduler.open_description(id)->references == 2 && !f.scheduler.open_description(id)->unix_queue_references);
        CHECK(f.posix.fcntl(a, posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
        CHECK(f.posix.fcntl(b, posix::f_GET_SEALS, 0).value() == 6);
        CHECK(same_frame(first, f.store.page(handle, 0).value()) && same_frame(second, f.store.page(handle, 1).value()));
        CHECK(f.pool.reference_count(first.address) == 2 && f.pool.reference_count(second.address) == 2);
        CHECK(f.posix.read(a, {output.data(), 6}).value() == 6 && std::memcmp(output.data(), "page c", 6) == 0);
        CHECK(f.posix.seek(b, 0, posix::SeekWhence::current).value() == 4096);
        const posix::IoVector vectors[]{{reinterpret_cast<uptr>(output.data()), 3}, {reinterpret_cast<uptr>(output.data() + 3), 4}};
        CHECK(f.posix.readv(b, vectors).value() == 7 && std::memcmp(output.data(), "rossing", 7) == 0);
        reinterpret_cast<char *>(second.address)[0] = 'R';
        CHECK(f.posix.pread(a, {output.data(), 7}, 4096).value() == 7 && std::memcmp(output.data(), "Rossing", 7) == 0);
        CHECK(f.posix.pwrite(b, bytes("Q"), 4096).value() == 1 && reinterpret_cast<char *>(second.address)[0] == 'Q');
        CHECK(f.posix.close(a) && f.store.info(handle).value().description);
        CHECK(f.posix.close(b) && !f.scheduler.open_description(id) && !f.store.info(handle).value().description);
        CHECK(f.posix.close(pair[0]) && f.posix.close(pair[1])); f.baseline(); f.ledger(1, 2, 2);
        CHECK(f.store.release_alias(first)); f.ledger(1, 1, 2);
        CHECK(f.store.release_alias(second)); CHECK(!f.store.valid(handle)); f.ledger(0, 0, 0);
        CHECK(!f.store.release_alias(second));
    });
}
void discarded_queue_owners()
{
    for (usize mode = 0; mode < 4; ++mode) test([mode](Fixture &f) {
        const auto pair = f.pair(); const auto fd = f.file(); const auto id = f.id(fd); const auto handle = f.handle(fd);
        const auto frame = f.store.page(handle, 0).value(); CHECK(f.store.retain_alias(frame));
        const std::array<i32, 1> rights{fd}; std::array<std::byte, 8> output{};
        CHECK(f.posix.send_unix_rights(pair[0], bytes("drop"), rights)); CHECK(f.posix.close(fd));
        CHECK(f.scheduler.open_description(id)->references == 1 && f.scheduler.open_description(id)->unix_queue_references == 1);
        if (mode == 0) CHECK(f.posix.read(pair[1], output).value() == 4);
        if (mode == 1)
        {
            const posix::IoVector vectors[]{{reinterpret_cast<uptr>(output.data()), 2}, {reinterpret_cast<uptr>(output.data() + 2), 2}};
            CHECK(f.posix.readv(pair[1], vectors).value() == 4);
        }
        if (mode == 2)
        {
            auto receive = f.posix.prepare_unix_receive(pair[1], output, 0); CHECK(receive);
            CHECK(receive.value().control_truncated() && receive.value().descriptors().empty());
            CHECK(receive.value().commit().value() == 4);
        }
        if (mode == 3) CHECK(f.posix.close(pair[1]));
        CHECK(!f.scheduler.open_description(id) && !f.store.info(handle).value().description); f.idle();
        CHECK(!Access::records(f.scheduler)); f.ledger(1, 1, 1);
        CHECK(f.posix.close(pair[0])); if (mode != 3) CHECK(f.posix.close(pair[1]));
        f.baseline(); CHECK(f.pool.reference_count(frame.address) == 2);
        CHECK(f.store.release_alias(frame)); f.ledger(0, 0, 0);
    });
}
void cycles_do_not_root_aliases()
{
    test([](Fixture &f) {
        const auto first = f.pair(), second = f.pair(); const auto fd = f.file(); const auto handle = f.handle(fd);
        const auto frame = f.store.page(handle, 0).value(); CHECK(f.store.retain_alias(frame));
        const auto id = f.id(fd), first_id = f.id(first[1]), second_id = f.id(second[1]);
        const std::array<i32, 3> first_rights{first[1], second[1], fd}; const std::array<i32, 1> second_rights{first[1]};
        CHECK(f.posix.send_unix_rights(first[0], bytes("cycle"), first_rights));
        CHECK(f.posix.send_unix_rights(second[0], bytes("cross"), second_rights));
        CHECK(f.posix.close(fd)); CHECK(f.posix.close(first[0]) && f.posix.close(second[0]));
        CHECK(f.posix.close(first[1])); CHECK(f.scheduler.open_description(first_id) && f.scheduler.open_description(id));
        CHECK(f.posix.close(second[1]));
        CHECK(!f.scheduler.open_description(id) && !f.scheduler.open_description(first_id) && !f.scheduler.open_description(second_id));
        CHECK(f.scheduler.collect_unix_rights()); f.baseline(); f.ledger(1, 1, 1);
        CHECK(!f.store.info(handle).value().description && same_frame(frame, f.store.page(handle, 0).value()));
        CHECK(f.store.release_alias(frame)); f.ledger(0, 0, 0);
        // Exact pipe vacancy is reusable after collection while no alias remains.
        for (usize index = 0; index < sched::max_process_pipes / 2; ++index) CHECK(f.posix.unix_socketpair());
    });
}
template<lu::Profile Profile, bool Native = false> void message_copy_faults()
{
    using C = lu::UnixMessageCodec<Profile, Native>;
    test([](Fixture &f) {
        const auto pair = f.pair(); const auto fd = f.file(); const auto id = f.id(fd); const auto handle = f.handle(fd);
        const auto frame = f.store.page(handle, 0).value(); CHECK(f.store.retain_alias(frame));
        const std::array<i32, 1> rights{fd}; CHECK(f.posix.send_unix_rights(pair[0], bytes("copy"), rights)); CHECK(f.posix.close(fd));
        constexpr usize header_at = 64, vector_at = 160, control_at = 256, data_at = 512;
        std::array<u8, 1024> memory{}; usize outputs{}, writable_calls{}, failed_output{}, failed_writable{};
        auto span = [&](usize address, usize length) { return std::span<u8>{memory}.subspan(address, length); };
        auto initialize = [&] {
            memory.fill(0); outputs = writable_calls = 0;
            C::store(span(header_at + C::vectors_offset, C::word_bytes), vector_at);
            C::store(span(header_at + C::vector_count_offset, C::word_bytes), 1);
            C::store(span(header_at + C::control_offset, C::word_bytes), control_at);
            C::store(span(header_at + C::control_length_offset, C::word_bytes), C::max_control_bytes);
            C::store(span(vector_at, C::word_bytes), data_at); C::store(span(vector_at + C::word_bytes, C::word_bytes), 4);
        };
        auto range = [&](u64 address, usize length) { return address <= memory.size() && length <= memory.size() - address; };
        auto call = [&] {
            return user::native::unix_message<Profile, Native>(f.posix, true, pair[1], header_at, C::cmsg_cloexec,
                [&](u64 address, std::span<u8> output) -> Status {
                    if (!range(address, output.size())) return Status::fault("copy input outside fixture");
                    std::memcpy(output.data(), memory.data() + address, output.size()); return {};
                },
                [&](u64 address, std::span<const u8> input) -> Status {
                    if (++outputs == failed_output || !range(address, input.size())) return Status::fault("injected copy output refusal");
                    std::memcpy(memory.data() + address, input.data(), input.size()); return {};
                },
                [&](u64 address, usize length) { return ++writable_calls != failed_writable && range(address, length); });
        };
        for (usize phase = 1; phase <= 6; ++phase)
        {
            initialize(); failed_writable = phase <= 3 ? phase : 0; failed_output = phase > 3 ? phase - 3 : 0;
            CHECK(call() == -14); f.idle(); f.ledger(1, 1, 1);
            CHECK(f.resources().files.open_count() == 5 && f.scheduler.open_description_count() == 6);
            CHECK(f.scheduler.open_description(id)->references == 1 && f.scheduler.open_description(id)->unix_queue_references == 1);
            CHECK(Access::records(f.scheduler) == 1 && f.pool.reference_count(frame.address) == 2);
        }
        initialize(); failed_output = failed_writable = 0; CHECK(call() == 4); f.idle();
        const auto received = static_cast<i32>(C::load(span(control_at + C::cmsg_header_bytes, 4)));
        CHECK(f.id(received) == id && f.handle(received) == handle);
        CHECK(f.posix.fcntl(received, posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
        CHECK(C::load(span(header_at + C::flags_offset, 4)) == C::cmsg_cloexec && std::memcmp(memory.data() + data_at, "copy", 4) == 0);
        CHECK(f.scheduler.open_description(id)->references == 1 && !f.scheduler.open_description(id)->unix_queue_references);
        CHECK(f.posix.close(received) && !f.store.info(handle).value().description);
        CHECK(f.posix.close(pair[0]) && f.posix.close(pair[1])); f.baseline();
        CHECK(f.store.release_alias(frame)); f.ledger(0, 0, 0);
    });
    std::printf("PASS shared file rights codec %s native=%d: exact queue owner and six receive refusals\n", C::profile.name, Native);
}
int main()
{
    last_queue_owner_and_aliases(); discarded_queue_owners(); cycles_do_not_root_aliases();
    message_copy_faults<lu::Profile::i386>(); message_copy_faults<lu::Profile::x86_64>();
    message_copy_faults<lu::Profile::aarch64>(); message_copy_faults<lu::Profile::arm32>();
    message_copy_faults<lu::Profile::rv64>(); message_copy_faults<lu::Profile::rv32>();
    message_copy_faults<lu::Profile::loongarch64>(); message_copy_faults<lu::Profile::mips>();
    message_copy_faults<lu::Profile::mips64>(); message_copy_faults<lu::Profile::ppc>();
    message_copy_faults<lu::configured_native_profile(), true>();
    std::printf("PASS shared file UNIX rights ownership: checks=%zu profiles=10 native_codec=1 hosted=1 target_PTE=0 guest=0\n", checks);
}
