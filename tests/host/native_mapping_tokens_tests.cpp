#include "ok/user/native_mapping_pages.hpp"
#include <cstdio>
#include <cstdlib>
using namespace ok;
using namespace ok::user::native;
static usize checks{};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::abort(); } } while (false)
struct Fixture
{
    alignas(4096) std::array<std::byte, 12 * 4096> backing{};
    memory::FrameAllocator frames;
    memory::FrameMetadataStorage<2048> frames_metadata;
    NativePagePool pool;
    Fixture(usize reserve)
    {
        const memory::MemoryRegion region{reinterpret_cast<uptr>(backing.data()), backing.size(), memory::RegionType::usable};
        CHECK(frames.initialize({&region, 1}, 4096, frames_metadata.bytes));
        CHECK(pool.activate(frames, NativePagePool::Source::linker_reserved, reserve));
    }
    ~Fixture() { CHECK(pool.used_count() == 0 && pool.free_count() == 12); }
};
template<class Function> void test(usize reserve, Function function)
{
    alignas(Fixture) static std::array<std::byte, sizeof(Fixture)> storage{};
    auto &fixture = *::new(storage.data()) Fixture(reserve);
    function(fixture); fixture.~Fixture();
}
int main()
{
    test(8, [](Fixture &f) {
        const auto first_address = f.pool.allocate().value(), second_address = f.pool.allocate().value();
        const auto first = *f.pool.handle(first_address), second = *f.pool.handle(second_address);
        MappingPages pages;
        CHECK(pages.reserve(f.pool, 2, true));
        CHECK(pages.exact_frames() && pages.block_count() == 1 && pages.frame_block_count() == 1);
        CHECK(pages.push_back({0x40001000, first.address, 3}, first));
        CHECK(pages.push_back({0x40000000, second.address, 1}, second));
        CHECK(pages[0].physical_address == second.address && pages.frame(0)->generation == second.generation);
        CHECK(pages[1].physical_address == first.address && pages.frame(1)->owner == first.owner);
        CHECK(!pages.push_back({0x40002000, first.address, 1}));
        auto stale = first; ++stale.generation;
        CHECK(!pages.push_back({0x40002000, first.address, 1}, stale));
        CHECK(!pages.push_back({0x40002000, second.address, 1}, first));
        CHECK(!pages.push_back({0x40001000, first.address, 1}, first));
        CHECK(pages.size() == 2 && f.pool.user_count() == 2 && f.pool.object_count() == 2);
        auto copied = MappingPages::clone(pages, f.pool); CHECK(copied);
        CHECK(copied.value().exact_frames() && copied.value().frame(0)->address == second.address);
        CHECK(copied.value().frame(1)->generation == first.generation && f.pool.reference_count(first.address) == 1);
        CHECK(f.pool.object_count() == 4);
        CHECK(copied.value().clear() && pages.clear());
        CHECK(f.pool.user_count() == 2 && f.pool.object_count() == 0);
        CHECK(f.pool.release_reference(first) && f.pool.release_reference(second));
    });
    test(1, [](Fixture &f) {
        MappingPages pages;
        CHECK(!pages.reserve(f.pool, 1, true));
        CHECK(!pages.exact_frames() && !pages.block_count() && !pages.frame_block_count());
        CHECK(f.pool.object_count() == 0);
        CHECK(pages.reserve(f.pool, 1));
        CHECK(!pages.reserve(f.pool, 1, true));
        CHECK(pages.block_count() == 1 && !pages.frame_block_count() && !pages.exact_frames());
        CHECK(pages.clear() && f.pool.object_count() == 0);
    });
    test(6, [](Fixture &f) {
        const auto address = f.pool.allocate().value(); const auto old = *f.pool.handle(address);
        MappingPages pages; CHECK(pages.reserve(f.pool, 1, true));
        CHECK(pages.push_back({0x40000000, old.address, 1}, old));
        CHECK(f.pool.release_reference(old));
        const auto successor_address = f.pool.allocate().value(); const auto successor = *f.pool.handle(successor_address);
        CHECK(successor.address == old.address && successor.generation != old.generation);
        CHECK(!f.pool.valid(*pages.frame(0)) && f.pool.valid(successor));
        CHECK(!MappingPages::clone(pages, f.pool));
        CHECK(f.pool.object_count() == 2 && f.pool.reference_count(successor.address) == 1);
        CHECK(pages.clear()); CHECK(f.pool.release_reference(successor));
    });
    test(2, [](Fixture &f) {
        MappingPages unowned;
        CHECK(unowned.reserve(f.pool, 1));
        CHECK(unowned.push_back({0x40000000, 0x2000, 1}));
        CHECK(!unowned.reserve(f.pool, 1, true));
        CHECK(!unowned.frame(0) && unowned.size() == 1 && !unowned.exact_frames());
        CHECK(unowned.clear());
    });
    std::printf("PASS exact mapping frame tokens: checks=%zu hosted=1 backend_execution=0 guest=0\n", checks);
}
