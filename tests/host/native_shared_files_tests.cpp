#include "ok/user/native_shared_files.hpp"
#include <cstdio>
#include <cstdlib>
#include <type_traits>
using namespace ok;
using namespace ok::user::native;
static usize checks{};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (false)
static_assert(!std::is_copy_constructible_v<SharedMemoryFiles>);
static_assert(!std::is_move_constructible_v<SharedMemoryFiles>);
static_assert(!std::is_copy_constructible_v<SharedMemoryFiles::Reservation>);
static_assert(std::is_nothrow_move_constructible_v<SharedMemoryFiles::Reservation>);
namespace ok::user::native
{
struct SharedMemoryFilesFaultProbe
{
    static void allocation(SharedMemoryFiles &store, usize after) { store.refuse_allocation_after_ = after; }
    static void staged_release(SharedMemoryFiles &store, bool refuse) { store.refuse_staged_release_ = refuse; }
    static void final_release(SharedMemoryFiles &store, bool refuse) { store.refuse_final_release_ = refuse; }
};
}
// Tables, page indexes and the ledger are object runs: the reserve covers them.
template <usize Pages = 16, usize Reserve = 6> struct Store
{
    alignas(4096) std::array<std::byte, Pages * 4096> backing{};
    memory::FrameAllocator frames;
    memory::FrameMetadataStorage<2048> frames_metadata;
    NativePagePool pool;
    SharedMemoryFiles store;
    Store()
    {
        const memory::MemoryRegion region{reinterpret_cast<uptr>(backing.data()), backing.size(), memory::RegionType::usable};
        CHECK(frames.initialize({&region, 1}, 4096, frames_metadata.bytes));
        CHECK(pool.activate(frames, NativePagePool::Source::linker_reserved, Reserve));
        CHECK(store.bind(pool));
    }
    ~Store()
    {
        const auto ledger = store.ledger();
        CHECK(!ledger.files && !ledger.aliases && !ledger.retained_frames && !ledger.reserved_cells);
        CHECK(pool.used_count() == 0 && pool.free_count() == Pages);
    }
    SharedFileHandle file()
    {
        auto reservation = store.reserve("English buffer"); CHECK(reservation);
        auto handle = reservation.value().publish(); CHECK(handle);
        CHECK(!reservation.value().publish());
        return handle.value();
    }
};
using Fixture = Store<>;
template<class Kind = Fixture, class Function> void test(Function function)
{
    alignas(Kind) static std::array<std::byte, sizeof(Kind)> storage{};
    auto &fixture = *::new(storage.data()) Kind;
    function(fixture);
    fixture.~Kind();
}
std::span<const std::byte> bytes(std::string_view text)
{ return {reinterpret_cast<const std::byte *>(text.data()), text.size()}; }

int main()
{
    test([](Fixture &f) {
        const auto handle = f.file(); CHECK(f.store.initialize_size(handle, 4096));
        CHECK(f.store.write(handle, 0, bytes("English Notes snapshot")).value() == 22);
        CHECK(f.store.add_seals(handle, 6));
        const auto frame = f.store.page(handle, 0).value();
        CHECK(f.store.retain_alias(frame) && f.store.retain_alias(frame));
        CHECK(f.store.check_mapping_write(frame));
        // Alias ownership, rather than current protection bits, is the gate.
        CHECK(f.store.add_seals(handle, 8 | 1).code() == StatusCode::busy);
        CHECK(f.store.info(handle).value().seals == 6);
        CHECK(f.store.release_alias(frame));
        CHECK(f.store.add_seals(handle, 8).code() == StatusCode::busy);
        CHECK(f.store.info(handle).value().seals == 6);
        CHECK(f.store.release_alias(frame));
        CHECK(f.store.add_seals(handle, 8 | 16).code() == StatusCode::unsupported);
        CHECK(f.store.info(handle).value().seals == 6);
        CHECK(f.store.add_seals(handle, 8));
        CHECK(f.store.check_write(handle, 1).code() == StatusCode::permission);
        CHECK(f.store.write(handle, 0, bytes("Changed")).status().code() == StatusCode::permission);
        CHECK(f.store.check_write(handle, 0) && f.store.write(handle, 4096, {}).value() == 0);
        std::array<std::byte, 22> output{};
        CHECK(f.store.read(handle, 0, output).value() == output.size());
        CHECK((std::string_view{reinterpret_cast<const char *>(output.data()), output.size()} == "English Notes snapshot"));
        CHECK(f.store.retain_alias(frame));
        CHECK(f.store.add_seals(handle, 8));
        CHECK(f.store.info(handle).value().seals == 14);
        CHECK(f.store.check_mapping_write(frame).code() == StatusCode::denied);
        auto stale = frame; ++stale.generation;
        CHECK(f.store.check_mapping_write(stale).code() == StatusCode::bad_descriptor);
        CHECK(f.store.add_seals(handle, 1));
        CHECK(f.store.add_seals(handle, 8).code() == StatusCode::permission);
        CHECK(f.store.info(handle).value().seals == 15);
        CHECK(f.store.release_description(handle));
        CHECK(f.store.check_mapping_write(frame).code() == StatusCode::denied);
        CHECK(f.store.read(handle, 0, output).value() == output.size());
        CHECK(f.store.release_alias(frame));
    });
    test([](Fixture &f) {
        const auto handle = f.file(); CHECK(f.store.initialize_size(handle, 2 * 4096));
        CHECK(f.store.add_seals(handle, 6));
        const auto first = f.store.page(handle, 0).value();
        const auto second = f.store.page(handle, 1).value();
        CHECK(f.pool.pin_count(first).value() == 0 && f.pool.pin_count(second).value() == 0);
        auto stale = first; ++stale.generation;
        CHECK(f.pool.pin_count(stale).status().code() == StatusCode::invalid_argument);
        auto foreign = first; foreign.owner = nullptr;
        CHECK(!f.pool.pin_count(foreign));
        auto changed = first; changed.address = second.address;
        CHECK(!f.pool.pin_count(changed));
        // A fully discharged mapping can leave independent hardware-table
        // pins. Check every canonical file page before publishing any seal.
        CHECK(f.pool.pin(first) && f.pool.pin(second) && f.pool.pin(second));
        CHECK(f.store.info(handle).value().aliases == 0);
        CHECK(f.store.add_seals(handle, 8 | 1).code() == StatusCode::busy);
        CHECK(f.store.info(handle).value().seals == 6);
        CHECK(f.pool.unpin(first) && f.pool.unpin(second));
        CHECK(f.pool.pin_count(first).value() == 0 && f.pool.pin_count(second).value() == 1);
        CHECK(f.store.add_seals(handle, 8).code() == StatusCode::busy);
        CHECK(f.store.info(handle).value().seals == 6);
        CHECK(f.pool.unpin(second));
        CHECK(f.store.add_seals(handle, 8));
        CHECK(f.store.info(handle).value().seals == 14);
        CHECK(f.store.retain_alias(second) && f.pool.pin(second));
        CHECK(f.store.add_seals(handle, 8));
        CHECK(f.store.add_seals(handle, 1));
        CHECK(f.store.info(handle).value().seals == 15);
        CHECK(f.pool.unpin(second) && f.store.release_alias(second));
        CHECK(f.store.release_description(handle));
        CHECK(!f.pool.pin_count(first) && !f.pool.pin_count(second));
    });
    test([](Fixture &f) {
        auto reservation = f.store.reserve("cancel"); CHECK(reservation);
        const auto stale = reservation.value().handle();
        SharedMemoryFiles::Reservation moved{std::move(reservation.value())};
        CHECK(!reservation.value().publish());
        moved.cancel(); moved.cancel(); CHECK(!f.store.valid(stale));
        const auto handle = f.file();
        CHECK(handle.slot == stale.slot && handle.generation != stale.generation);
        auto foreign = handle; ++foreign.owner; CHECK(!f.store.info(foreign));
        CHECK(!f.store.info(stale));
        CHECK(f.store.release_description(handle)); CHECK(!f.store.valid(handle));
    });
    test([](Fixture &f) {
        const auto handle = f.file();
        CHECK(f.store.initialize_size(handle, 1).code() == StatusCode::unsupported);
        CHECK(f.store.initialize_size(handle, 64 * 4096).code() == StatusCode::no_memory);
        CHECK(f.store.info(handle).value().size == 0 && f.pool.user_count() == 0 && f.pool.object_count() == 1);
        CHECK(!f.store.ledger().reserved_cells && !f.store.ledger().retained_frames);
        CHECK(f.store.initialize_size(handle, 2 * 4096));
        CHECK(f.store.initialize_size(handle, 2 * 4096));
        CHECK(f.store.initialize_size(handle, 4096).code() == StatusCode::unsupported);
        CHECK(f.store.page(handle, 0).status().code() == StatusCode::unsupported);
        CHECK(!f.store.add_seals(handle, SharedMemoryFiles::seal_grow | 16));
        CHECK(f.store.info(handle).value().seals == 0);
        CHECK(f.store.add_seals(handle, SharedMemoryFiles::seal_grow | SharedMemoryFiles::seal_shrink));
        CHECK(f.store.initialize_size(handle, 4096).code() == StatusCode::permission);
        CHECK(f.store.initialize_size(handle, 3 * 4096).code() == StatusCode::permission);
        CHECK(f.store.add_seals(handle, SharedMemoryFiles::seal_seal));
        CHECK(f.store.add_seals(handle, 0).code() == StatusCode::permission);
        CHECK(f.store.release_description(handle));
    });
    test([](Fixture &f) {
        const auto handle = f.file(); CHECK(f.store.initialize_size(handle, 2 * 4096));
        CHECK(f.store.add_seals(handle, 6));
        const auto page = f.store.page(handle, 0).value();
        const auto second = f.store.page(handle, 1).value();
        CHECK(f.store.owns(page) && f.store.retain_alias(page) && f.store.retain_alias(page));
        CHECK(f.pool.reference_count(page.address) == 3 && f.store.info(handle).value().aliases == 2);
        auto *alias = reinterpret_cast<std::byte *>(page.address);
        alias[0] = std::byte{'A'};
        std::array<std::byte, 8> output{};
        CHECK(f.store.read(handle, 0, output).value() == output.size() && output[0] == std::byte{'A'});
        CHECK(f.store.write(handle, 4094, bytes("abcd")).value() == 4);
        CHECK(alias[4094] == std::byte{'a'} && alias[4095] == std::byte{'b'});
        CHECK(reinterpret_cast<const std::byte *>(second.address)[0] == std::byte{'c'});
        CHECK(f.store.write(handle, 8191, bytes("tail")).value() == 1);
        CHECK(f.store.read(handle, 8192, output).value() == 0);
        CHECK(f.store.write(handle, 8192, bytes("x")).status().code() == StatusCode::permission);
        auto changed = page; ++changed.generation;
        CHECK(!f.store.retain_alias(changed) && !f.store.release_alias(changed));
        CHECK(f.store.release_description(handle));
        CHECK(f.store.info(handle).value().aliases == 2 && !f.store.info(handle).value().description);
        CHECK(f.store.release_alias(page) && f.pool.reference_count(page.address) == 2);
        CHECK(f.store.release_alias(page) && !f.store.valid(handle));
        CHECK(!f.store.release_alias(page));
        const auto successor = f.file(); CHECK(f.store.initialize_size(successor, 4096));
        CHECK(f.store.add_seals(successor, 6));
        const auto reused = f.store.page(successor, 0).value();
        CHECK(reused.address == page.address && reused.generation != page.generation);
        CHECK(!f.store.owns(page) && !f.store.release_alias(page));
        CHECK(f.store.release_description(successor));
    });
    test([](Fixture &f) {
        const auto handle = f.file();
        SharedMemoryFilesFaultProbe::allocation(f.store, 2);
        SharedMemoryFilesFaultProbe::staged_release(f.store, true);
        CHECK(f.store.initialize_size(handle, 3 * 4096).code() == StatusCode::no_memory);
        CHECK(f.store.info(handle).value().size == 0 && f.store.info(handle).value().description);
        CHECK(f.store.ledger().retained_frames == 2 && !f.store.ledger().reserved_cells);
        CHECK(f.pool.user_count() == 2);
        usize seen{};
        for (usize index = 0; index < f.store.ledger_capacity(); ++index)
            if (const auto *frame = f.store.retained_frame(index))
            {
                CHECK(frame->owner == &f.frames && frame->generation && f.pool.reference_count(frame->address) == 1);
                CHECK(!f.store.owns(*frame) && !f.store.retain_alias(*frame)); ++seen;
            }
        CHECK(seen == 2);
        SharedMemoryFilesFaultProbe::staged_release(f.store, false);
        CHECK(f.store.retry_retirement()); CHECK(f.pool.user_count() == 0 && !f.store.ledger_capacity());
        SharedMemoryFilesFaultProbe::allocation(f.store, ~usize{0});
        CHECK(f.store.initialize_size(handle, 4096));
        CHECK(f.store.release_description(handle));
    });
    test([](Fixture &f) {
        const auto handle = f.file();
        // Ledger and page-index growth draw on the object reserve first.
        std::array<NativePagePool::Lease, 8> held{};
        usize count{};
        for (auto lease = f.pool.allocate_kernel_page(); lease; lease = f.pool.allocate_kernel_page())
            held[count++] = std::move(lease.value());
        const auto objects = f.pool.object_count();
        CHECK(f.store.initialize_size(handle, 4096).code() == StatusCode::no_memory);
        CHECK(f.store.info(handle).value().size == 0 && f.pool.user_count() == 0 && f.pool.object_count() == objects);
        CHECK(!f.store.ledger().reserved_cells && !f.store.ledger().retained_frames);
        while (count) CHECK(f.pool.release_kernel_page(held[--count]));
        CHECK(f.store.initialize_size(handle, 4096));
        SharedMemoryFilesFaultProbe::final_release(f.store, true);
        CHECK(!f.store.release_description(handle)); CHECK(!f.store.valid(handle));
        CHECK(f.store.ledger().retiring_files == 1 && f.pool.user_count() == 1);
        const auto next = f.file(); CHECK(next.slot != handle.slot); CHECK(f.store.release_description(next));
        CHECK(!f.store.retry_retirement()); CHECK(f.store.ledger().retiring_files == 1);
        SharedMemoryFilesFaultProbe::final_release(f.store, false);
        CHECK(f.store.retry_retirement()); CHECK(f.pool.used_count() == 0);
    });
    test([](Fixture &f) {
        const auto handle = f.file(); CHECK(f.store.initialize_size(handle, 4096));
        CHECK(f.store.add_seals(handle, 6)); const auto page = f.store.page(handle, 0).value();
        CHECK(f.store.retain_alias(page)); CHECK(f.pool.pin(page));
        CHECK(f.store.release_description(handle)); CHECK(f.store.release_alias(page));
        CHECK(!f.store.valid(handle) && f.pool.pending_count() == 1 && f.pool.pinned_count() == 1);
        CHECK(f.pool.unpin(page)); CHECK(f.pool.used_count() == 0);
    });
    for (usize boundary = 0; boundary < 4; ++boundary)
        for (bool retained : {false, true})
            test([&](Fixture &f) {
                const auto handle = f.file();
                SharedMemoryFilesFaultProbe::allocation(f.store, boundary);
                SharedMemoryFilesFaultProbe::staged_release(f.store, retained);
                CHECK(f.store.initialize_size(handle, 4 * 4096).code() == StatusCode::no_memory);
                CHECK(f.store.info(handle).value().size == 0 && f.store.info(handle).value().description);
                CHECK(!f.store.ledger().reserved_cells);
                CHECK(f.store.ledger().retained_frames == (retained ? boundary : 0));
                CHECK(f.pool.user_count() == (retained ? boundary : 0));
                SharedMemoryFilesFaultProbe::staged_release(f.store, false);
                CHECK(f.store.retry_retirement());
                SharedMemoryFilesFaultProbe::allocation(f.store, ~usize{0});
                CHECK(f.store.initialize_size(handle, 4 * 4096));
                std::array<std::byte, 4 * 4096> output{};
                output.fill(std::byte{0xff});
                CHECK(f.store.read(handle, 0, output).value() == output.size());
                for (auto value : output) CHECK(value == std::byte{});
                CHECK(f.store.release_description(handle));
            });
    test([](Fixture &f) {
        const auto handle = f.file(); CHECK(f.store.initialize_size(handle, 4096));
        CHECK(f.store.add_seals(handle, 6)); const auto page = f.store.page(handle, 0).value();
        CHECK(f.store.retain_alias(page)); CHECK(f.store.release_description(handle));
        SharedMemoryFilesFaultProbe::final_release(f.store, true);
        bool consumed{};
        CHECK(!f.store.release_alias(page, consumed) && consumed);
        CHECK(f.store.ledger().aliases == 0 && f.store.ledger().retiring_files == 1);
        consumed = true;
        CHECK(!f.store.release_alias(page, consumed) && !consumed);
        SharedMemoryFilesFaultProbe::final_release(f.store, false);
        CHECK(f.store.retry_retirement());
    });
    // No fixed file count: the table grows past sixteen files, keeps every
    // handle valid and goes back to the pool once the last file retires.
    test<Store<24, 12>>([](Store<24, 12> &f) {
        std::array<SharedFileHandle, 40> handles{};
        for (auto &handle : handles) handle = f.file();
        CHECK(f.store.ledger().files == handles.size() && f.pool.user_count() == 0 && f.pool.object_count());
        for (usize index = 0; index < handles.size(); ++index)
        {
            CHECK(f.store.valid(handles[index]) && handles[index].slot == index);
            if (index) CHECK(handles[index].generation > handles[index - 1].generation);
        }
        for (auto handle : handles) CHECK(f.store.release_description(handle));
        CHECK(f.pool.object_count() == 0);
        for (auto handle : handles) CHECK(!f.store.valid(handle));
        const auto successor = f.file();
        CHECK(successor.slot == 0 && successor.generation > handles.back().generation);
        CHECK(f.store.release_description(successor));
    });
    // No fixed page count: a file larger than the former 64-page bound maps,
    // reads back zeroed and retires every page and its index.
    test<Store<96, 8>>([](Store<96, 8> &f) {
        const auto handle = f.file();
        CHECK(f.store.initialize_size(handle, 80 * 4096));
        CHECK(f.store.info(handle).value().pages == 80 && f.pool.user_count() == 80);
        CHECK(f.store.add_seals(handle, 6));
        const auto last = f.store.page(handle, 79).value();
        CHECK(f.store.retain_alias(last) && f.store.write(handle, 79 * 4096, bytes("tail")).value() == 4);
        std::array<std::byte, 4> output{};
        CHECK(f.store.read(handle, 79 * 4096, output).value() == 4 && output[0] == std::byte{'t'});
        CHECK(f.store.release_description(handle) && f.store.valid(handle));
        CHECK(f.store.release_alias(last) && !f.store.valid(handle));
        // A size the user quota cannot hold is refused before any allocation.
        const auto oversized = f.file();
        CHECK(f.store.initialize_size(oversized, 96 * 4096).code() == StatusCode::no_memory);
        CHECK(f.pool.user_count() == 0 && f.store.release_description(oversized));
    });
    std::printf("PASS shared file owner ledger: checks=%zu hosted=1 actual_native_alias=0 guest=0\n", checks);
}
