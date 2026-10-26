// Test the actual native VM transaction code with a fault-injectable backend.
// No hardware address space or process isolation is exercised by this binary.
#ifndef OK_ENABLE_TEST_POINTS
#define OK_ENABLE_TEST_POINTS
#endif
#include "../../src/user/runtime.cpp"

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <type_traits>

using namespace ok;
namespace native = ok::user::native;
namespace hw = ok::arch::execution;
namespace ok::user::native
{
struct SharedMemoryFilesFaultProbe
{
    static void final_release(SharedMemoryFiles &files, bool refuse) { files.refuse_final_release_ = refuse; }
};
struct NativePagePoolFaultProbe
{
    static void all_releases(NativePagePool &pool, bool refuse) { pool.refuse_all_reference_releases_ = refuse; }
    static void exact_release(NativePagePool &pool, memory::PhysicalFrame frame, usize count)
    { pool.refused_reference_ = frame; pool.refuse_reference_releases_ = count; }
};
}

// This fixture has no scheduler. These symbols satisfy the origin table's
// cold failure path; stage_space receives a null scheduler throughout.
namespace ok::sched
{
__attribute__((noinline)) ThreadControlBlock *SchedulerState::find_thread(ProcessId, ThreadId) { return nullptr; }
__attribute__((noinline)) ProcessControlBlock *SchedulerState::find(ProcessId) { return nullptr; }
__attribute__((noinline)) OpenFileDescription *SchedulerState::open_description(u16) { return nullptr; }
}

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!static_cast<bool>(condition))                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                         \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

namespace mock
{
struct Space
{
    bool live{};
    u32 generation{};
    hw::MappingView pages;
};
std::array<Space, hw::max_address_spaces> spaces;
bool fail_create{};
bool fail_all_retirement{};
uptr fail_destroy_root{};

usize live_spaces()
{
    usize count = 0;
    for (const auto &space : spaces)
        count += space.live;
    return count;
}

const hw::PageMapping *page(const hw::AddressSpace &space, uptr address)
{
    for (const auto &page : spaces[space.slot].pages)
        if (page.flags && page.virtual_address == address)
            return &page;
    return nullptr;
}

usize used_frames()
{
    return native::page_pool.user_count();
}
} // namespace mock

namespace ok::arch::execution
{
Result<AddressSpace> create_space(MappingView mappings)
{
    if (mock::fail_create)
        return Status::no_memory("injected page-table allocation failure");
    for (usize index = 0; index < mock::spaces.size(); ++index)
    {
        auto &space = mock::spaces[index];
        if (space.live)
            continue;
        space.pages = mappings;
        space.live = true;
        ++space.generation;
        return AddressSpace{index + 1, static_cast<u32>(index), space.generation};
    }
    return Status::no_memory("mock page-table slots exhausted");
}

Status destroy_space(AddressSpace &space)
{
    if (mock::fail_all_retirement || space.root == mock::fail_destroy_root)
        return Status::busy("injected retirement failure");
    CHECK(space.slot < mock::spaces.size());
    auto &storage = mock::spaces[space.slot];
    CHECK(storage.live && storage.generation == space.generation && space.root == space.slot + 1);
    storage.live = false;
    storage.pages = {};
    space = {};
    return Status::success();
}
} // namespace ok::arch::execution

struct Fixture
{
    sched::ProcessResources resources;
    Fixture()
    {
        CHECK(mock::live_spaces() == 0 && mock::used_frames() == 0);
        CHECK(native::allocate(resources.native_pages, hw::user_begin, hw::page_read | hw::page_execute));
        CHECK(native::allocate(resources.native_pages, hw::user_end - hw::page_size, hw::page_read | hw::page_write));
        auto space = native::create_page_space(resources.native_pages);
        CHECK(space);
        resources.native_space = space.value();
        CHECK(native::page_memory_map(resources.native_pages, resources.memory));
    }
    ~Fixture()
    {
        native::release(resources.native_space, resources.native_pages);
        CHECK(mock::live_spaces() == 0 && mock::used_frames() == 0);
    }
    Result<uptr> map(usize length, uptr hint = 0, uptr protection = 3)
    {
        return native::native_mmap(resources, hint, length, protection, 0x22, ~uptr{0}, 0);
    }
};

// The file provider deliberately returns short reads, then can fail after one
// successful read. The VM must stage all bytes before publishing a replacement.
struct FileNode final : fs::Node
{
    std::array<std::byte, 2 * hw::page_size + 37> bytes{};
    mutable usize reads{};
    usize fail_read{};

    FileNode()
    {
        for (usize i = 0; i < bytes.size(); ++i)
            bytes[i] = std::byte{static_cast<unsigned char>((i * 17 + 23) & 0xff)};
    }
    std::string_view name() const override { return "file-mapping-fixture"; }
    fs::Metadata metadata() const override
    {
        fs::Metadata result;
        result.size = bytes.size();
        return result;
    }
    Result<fs::FileBuffer> read(usize offset, usize count) const override
    {
        if (++reads == fail_read) return Status::io_error("injected file read failure");
        fs::FileBuffer result;
        if (offset >= bytes.size()) return result;
        result.size = count < 509 ? count : 509;
        if (result.size > bytes.size() - offset) result.size = bytes.size() - offset;
        for (usize i = 0; i < result.size; ++i) result.data[i] = bytes[offset + i];
        return result;
    }
    Status write(usize, std::span<const std::byte>) override { return Status::unsupported("unused"); }
    fs::Node *lookup(std::string_view) override { return nullptr; }
    Status create(std::string_view, fs::NodeType) override { return Status::unsupported("unused"); }
    Status chmod(u32) override { return Status::unsupported("unused"); }
    Status chown(u32, u32) override { return Status::unsupported("unused"); }
};

void check_private_file_mapping()
{
    Fixture fixture;
    auto &resources = fixture.resources;
    FileNode file;
    const uptr start = hw::user_begin + 0x10000;
    const usize offset = hw::page_size;
    const usize length = hw::page_size + 100;
    auto mapped = native::native_mmap(resources, start, length, 3, 0x2, 4, offset,
                                      &file, file.bytes.size());
    CHECK(mapped && mapped.value() == start);
    CHECK(file.reads > 2 && mock::used_frames() == 4);
    auto *first = reinterpret_cast<unsigned char *>(mock::page(resources.native_space, start)->physical_address);
    auto *last = reinterpret_cast<unsigned char *>(mock::page(resources.native_space, start + hw::page_size)->physical_address);
    for (usize i = 0; i < hw::page_size; ++i)
        CHECK(first[i] == static_cast<unsigned char>(file.bytes[offset + i]));
    for (usize i = 0; i < 37; ++i)
        CHECK(last[i] == static_cast<unsigned char>(file.bytes[2 * hw::page_size + i]));
    for (usize i = 37; i < hw::page_size; ++i) CHECK(last[i] == 0);
    first[0] = 0xa5;
    file.bytes[offset] = std::byte{0x5a};
    CHECK(first[0] == 0xa5);
    CHECK(native::native_mmap(resources, 0, hw::page_size, 3, 0x2, 4, offset + 1,
                              &file, file.bytes.size()).status().code() == StatusCode::invalid_argument);
    CHECK(native::native_mmap(resources, 0, 2 * hw::page_size, 3, 0x2, 4, 2 * hw::page_size,
                              &file, file.bytes.size()).status().code() == StatusCode::unsupported);
    CHECK(native::native_mmap(resources, 0, hw::page_size, 3, 0x2, 4, 0,
                              nullptr, file.bytes.size()).status().code() == StatusCode::bad_descriptor);

    const auto old_space = resources.native_space;
    const usize old_frames = mock::used_frames();
    file.reads = 0;
    file.fail_read = 2;
    CHECK(native::native_mmap(resources, start, length, 3, 0x12, 4, offset,
                              &file, file.bytes.size()).status().code() == StatusCode::io_error);
    CHECK(file.reads == 2 && mock::used_frames() == old_frames);
    CHECK(resources.native_space.root == old_space.root && resources.native_space.generation == old_space.generation);
    CHECK(first[0] == 0xa5 && last[37] == 0);
    file.fail_read = 0;
    auto replaced = native::native_mmap(resources, start, length, 3, 0x12, 4, offset,
                                        &file, file.bytes.size());
    CHECK(replaced && replaced.value() == start && mock::used_frames() == old_frames);
    first = reinterpret_cast<unsigned char *>(mock::page(resources.native_space, start)->physical_address);
    CHECK(first[0] == 0x5a);
}

void check_short_private_file_mapping()
{
    Fixture fixture;
    FileNode file;
    auto mapped = native::native_mmap(fixture.resources, 0, 1, 1, 0x2, 4, 0,
                                      &file, file.bytes.size());
    CHECK(mapped);
    auto *page = reinterpret_cast<const unsigned char *>(
        mock::page(fixture.resources.native_space, mapped.value())->physical_address);
    for (usize i = 0; i < hw::page_size; ++i)
        CHECK(page[i] == static_cast<unsigned char>(file.bytes[i]));

    auto eof = native::native_mmap(fixture.resources, 0, 1, 1, 0x2, 4, 2 * hw::page_size,
                                   &file, file.bytes.size());
    CHECK(eof);
    auto *tail = reinterpret_cast<const unsigned char *>(
        mock::page(fixture.resources.native_space, eof.value())->physical_address);
    for (usize i = 0; i < 37; ++i)
        CHECK(tail[i] == static_cast<unsigned char>(file.bytes[2 * hw::page_size + i]));
    for (usize i = 37; i < hw::page_size; ++i) CHECK(tail[i] == 0);
}

void check_map_protect_unmap()
{
    Fixture fixture;
    auto &resources = fixture.resources;
    auto mapped = fixture.map(3 * hw::page_size);
    CHECK(mapped && resources.native_pages.size() == 5 && mock::used_frames() == 5);
    const uptr address = mapped.value();
    auto *first = reinterpret_cast<unsigned char *>(mock::page(resources.native_space, address)->physical_address);
    for (usize i = 0; i < hw::page_size; ++i)
        CHECK(first[i] == 0);
    first[0] = 71;
    CHECK(native::native_mprotect(resources, address, 1, 1));
    CHECK(mock::page(resources.native_space, address)->flags == 1);
    CHECK(!resources.memory.allows_user_access(address, 1, memory::page_write));
    CHECK(native::native_mprotect(resources, address, hw::page_size, 0));
    CHECK(mock::page(resources.native_space, address) == nullptr);
    CHECK(mock::used_frames() == 5 && resources.native_pages.size() == 5);
    CHECK(!resources.memory.allows_user_access(address, 1, memory::page_read));
    CHECK(native::native_mprotect(resources, address, hw::page_size, 3));
    CHECK(mock::page(resources.native_space, address)->physical_address == reinterpret_cast<uptr>(first));
    CHECK(first[0] == 71);
    CHECK(native::native_munmap(resources, address + hw::page_size, hw::page_size));
    CHECK(mock::page(resources.native_space, address + hw::page_size) == nullptr);
    CHECK(resources.native_pages.size() == 4 && mock::used_frames() == 4);
    const auto previous = resources.native_space;
    CHECK(native::native_mprotect(resources, address, 3 * hw::page_size, 1).code() == StatusCode::no_memory);
    CHECK(resources.native_space.root == previous.root && resources.native_space.generation == previous.generation);
    CHECK(mock::page(resources.native_space, address)->flags == 3);
    CHECK(native::native_munmap(resources, address, 3 * hw::page_size));
    CHECK(resources.native_pages.size() == 2 && mock::used_frames() == 2);
    CHECK(native::native_munmap(resources, address, 3 * hw::page_size));
    auto fresh = fixture.map(hw::page_size, address);
    CHECK(fresh && fresh.value() == address);
    auto *zero = reinterpret_cast<unsigned char *>(mock::page(resources.native_space, address)->physical_address);
    CHECK(zero[0] == 0);
}

void check_validation_and_capacity()
{
    Fixture fixture;
    auto &resources = fixture.resources;
    const auto original = resources.native_space;
    CHECK(fixture.map(0).status().code() == StatusCode::invalid_argument);
    CHECK(fixture.map(~usize{0}).status().code() == StatusCode::invalid_argument);
    CHECK(fixture.map(native::Pages::capacity * hw::page_size).status().code() == StatusCode::no_memory);
    CHECK(!fixture.map(hw::page_size, 0, 2));
    CHECK(!fixture.map(hw::page_size, 0, 5));
    CHECK(!native::native_mmap(resources, 0, 4096, 3, 0x32, ~uptr{0}, 0));
    CHECK(!native::native_mmap(resources, hw::user_begin + 1, 4096, 3, 0x32, ~uptr{0}, 0));
    CHECK(!native::native_mmap(resources, 0, 4096, 3, 0x22, 0, 0));
    CHECK(!native::native_mmap(resources, 0, 4096, 3, 0x22, ~uptr{0}, 4096));
    CHECK(!native::native_mprotect(resources, hw::user_begin + 1, 4096, 1));
    CHECK(!native::native_munmap(resources, hw::user_end - 4096, 8192));
    CHECK(resources.native_space.root == original.root && mock::used_frames() == 2);
    auto collision = fixture.map(4096, hw::user_begin);
    CHECK(collision && collision.value() != hw::user_begin);
    auto all = fixture.map(200 * hw::page_size);
    CHECK(all && resources.native_pages.size() == 203);
    CHECK(fixture.map(native::page_pool.user_capacity_count() * hw::page_size).status().code() == StatusCode::no_memory);
    CHECK(mock::used_frames() == 203);
    CHECK(native::native_munmap(resources, all.value(), 200 * hw::page_size));
    CHECK(mock::used_frames() == 3);
    CHECK(fixture.map(4096));
}

void check_transaction_rollback()
{
    Fixture fixture;
    auto &resources = fixture.resources;
    auto mapped = fixture.map(4096);
    CHECK(mapped);
    const auto old_space = resources.native_space;
    const auto old_areas = resources.memory.area_count();
    const usize before = mock::used_frames();
    mock::fail_create = true;
    CHECK(!fixture.map(8192));
    CHECK(!native::native_mprotect(resources, mapped.value(), 4096, 1));
    CHECK(!native::native_munmap(resources, mapped.value(), 4096));
    mock::fail_create = false;
    CHECK(resources.native_space.root == old_space.root && resources.memory.area_count() == old_areas);
    CHECK(mock::used_frames() == before && mock::live_spaces() == 1);
    CHECK(mock::page(resources.native_space, mapped.value())->flags == 3);
    mock::fail_destroy_root = old_space.root;
    CHECK(!fixture.map(8192));
    CHECK(!native::native_mprotect(resources, mapped.value(), 4096, 1));
    CHECK(!native::native_munmap(resources, mapped.value(), 4096));
    CHECK(mock::used_frames() == before && mock::live_spaces() == 1);
    CHECK(resources.native_space.root == old_space.root && resources.native_space.generation == old_space.generation);
    CHECK(mock::page(resources.native_space, mapped.value())->flags == 3);
    mock::fail_destroy_root = 0;
    CHECK(native::native_mprotect(resources, mapped.value(), 4096, 1));
}

void check_backend_and_metadata_limits()
{
    Fixture fixture;
    auto &resources = fixture.resources;
    std::array<hw::AddressSpace, hw::max_address_spaces - 1> borrowed{};
    for (auto &space : borrowed)
    {
        auto created = native::create_page_space(resources.native_pages);
        CHECK(created);
        space = created.value();
    }
    CHECK(fixture.map(4096).status().code() == StatusCode::no_memory);
    CHECK(mock::used_frames() == 2 && mock::live_spaces() == hw::max_address_spaces);
    for (auto &space : borrowed)
        CHECK(hw::destroy_space(space));
    for (usize i = 0; i < sched::max_memory_areas - 2; ++i)
        CHECK(fixture.map(4096, hw::user_begin + 0x2000000 + i * 8192));
    CHECK(resources.memory.area_count() == sched::max_memory_areas);
    const auto previous = resources.native_space;
    const auto count = mock::used_frames();
    CHECK(fixture.map(4096, hw::user_begin + 0x4000000).status().code() == StatusCode::no_memory);
    CHECK(mock::used_frames() == count && mock::live_spaces() == 1);
    CHECK(resources.native_space.root == previous.root && resources.native_space.generation == previous.generation);
}

void check_pool_failure_rolls_back_partial_allocation()
{
    Fixture fixture;
    native::Pages borrowed[8];
    for (usize count = 0; count < native::page_pool.user_capacity_count() - 3; ++count)
        CHECK(native::allocate(borrowed[count / 128], hw::user_begin + (count % 128) * 4096, 3));
    CHECK(mock::used_frames() == native::page_pool.user_capacity_count() - 1);
    CHECK(fixture.map(8192).status().code() == StatusCode::no_memory);
    CHECK(mock::used_frames() == native::page_pool.user_capacity_count() - 1 && fixture.resources.native_pages.size() == 2);
    for (auto &pages : borrowed)
    {
        hw::AddressSpace empty;
        native::release(empty, pages);
    }
    CHECK(mock::used_frames() == 2);
}

void check_fixed_replacement()
{
    Fixture fixture;
    auto &r = fixture.resources;
    const uptr start = hw::user_begin + 0x10000;
    CHECK(fixture.map(3 * hw::page_size, start));
    auto *first = reinterpret_cast<unsigned char *>(mock::page(r.native_space, start)->physical_address);
    first[0] = 0x65;
    const usize before = mock::used_frames();
    const auto previous = r.native_space;
    mock::fail_create = true;
    CHECK(!native::native_mmap(r, start, 2 * hw::page_size, 0, 0x32, ~uptr{0}, 0));
    CHECK(mock::used_frames() == before && first[0] == 0x65);
    CHECK(r.native_space.root == previous.root && r.native_space.generation == previous.generation);
    mock::fail_create = false;
    mock::fail_destroy_root = previous.root;
    CHECK(!native::native_mmap(r, start, 2 * hw::page_size, 0, 0x32, ~uptr{0}, 0));
    CHECK(mock::used_frames() == before && first[0] == 0x65);
    mock::fail_destroy_root = 0;
    auto replaced = native::native_mmap(r, start, 2 * hw::page_size, 0, 0x32, ~uptr{0}, 0);
    CHECK(replaced && replaced.value() == start && mock::used_frames() == before);
    CHECK(!mock::page(r.native_space, start) && !mock::page(r.native_space, start + hw::page_size));
    CHECK(mock::page(r.native_space, start + 2 * hw::page_size));
    CHECK(native::native_mprotect(r, start, hw::page_size, 3));
    CHECK(*reinterpret_cast<unsigned char *>(mock::page(r.native_space, start)->physical_address) == 0);
    // Replacement can also cover holes and must respect the final page count.
    CHECK(native::native_mmap(r, start - hw::page_size, 5 * hw::page_size, 3, 0x32, ~uptr{0}, 0));
    CHECK(mock::used_frames() == before + 2);
}

void check_program_break()
{
    Fixture fixture;
    auto &r = fixture.resources;
    const uptr start = hw::user_begin + 0x2000;
    r.program_break_base = r.program_break = start;
    r.program_break_limit = start + 32 * hw::page_size;
    CHECK(native::native_brk(r, 0).value() == start);
    CHECK(!native::native_brk(r, start - 1));
    CHECK(!native::native_brk(r, r.program_break_limit + 1));
    CHECK(native::native_brk(r, start + hw::page_size + 1));
    CHECK(r.program_break == start + hw::page_size + 1 && mock::used_frames() == 4);
    auto *first = reinterpret_cast<unsigned char *>(mock::page(r.native_space, start)->physical_address);
    first[0] = 0x79;
    const auto previous = r.native_space;
    CHECK(native::native_brk(r, start + hw::page_size + 2));
    CHECK(r.native_space.root == previous.root && r.native_space.generation == previous.generation);
    mock::fail_create = true;
    CHECK(!native::native_brk(r, start + 4 * hw::page_size));
    CHECK(!native::native_brk(r, start));
    CHECK(r.program_break == start + hw::page_size + 2 && mock::used_frames() == 4 && first[0] == 0x79);
    mock::fail_create = false;
    CHECK(fixture.map(hw::page_size, start + 3 * hw::page_size));
    CHECK(!native::native_brk(r, start + 4 * hw::page_size));
    CHECK(r.program_break == start + hw::page_size + 2 && first[0] == 0x79);
    CHECK(native::native_brk(r, start + 1));
    CHECK(mock::used_frames() == 4 && first[0] == 0x79);
    CHECK(native::native_brk(r, start));
    CHECK(mock::used_frames() == 3);
    CHECK(native::native_brk(r, start + 1));
    CHECK(*reinterpret_cast<unsigned char *>(mock::page(r.native_space, start)->physical_address) == 0);
}


void review_fixed_full_process_capacity()
{
    Fixture f;
    auto &r = f.resources;
    const uptr start = hw::user_begin + 0x10000;
    CHECK(f.map(126 * hw::page_size, start));
    CHECK(r.native_pages.size() == 128 && mock::used_frames() == 128);
    auto *untouched = reinterpret_cast<unsigned char *>(mock::page(r.native_space, start + 2 * hw::page_size)->physical_address);
    untouched[0] = 73;
    auto replaced = native::native_mmap(r, start, 2 * hw::page_size, 3, 0x32, ~uptr{0}, 0);
    CHECK(replaced && replaced.value() == start);
    CHECK(r.native_pages.size() == 128 && mock::used_frames() == 128 && untouched[0] == 73);
    CHECK(*reinterpret_cast<unsigned char *>(mock::page(r.native_space, start)->physical_address) == 0);
}
void review_fixed_pool_failure_rollback()
{
    Fixture f;
    auto &r = f.resources;
    const uptr start = hw::user_begin + 0x10000;
    CHECK(f.map(3 * hw::page_size, start));
    auto *old = reinterpret_cast<unsigned char *>(mock::page(r.native_space, start)->physical_address);
    old[0] = 91;
    const auto previous = r.native_space;
    native::Pages borrowed[8];
    const usize count = native::page_pool.user_capacity_count() - mock::used_frames() - 1;
    for (usize i = 0; i < count; ++i)
        CHECK(native::allocate(borrowed[i / 128], hw::user_begin + (i % 128) * hw::page_size, 3));
    CHECK(mock::used_frames() == native::page_pool.user_capacity_count() - 1);
    CHECK(!native::native_mmap(r, start, 2 * hw::page_size, 0, 0x32, ~uptr{0}, 0));
    CHECK(mock::used_frames() == native::page_pool.user_capacity_count() - 1 && old[0] == 91);
    CHECK(r.native_space.root == previous.root && r.native_space.generation == previous.generation);
    CHECK(mock::page(r.native_space, start)->flags == 3);
    for (auto &pages : borrowed)
    {
        hw::AddressSpace empty;
        native::release(empty, pages);
    }
    CHECK(mock::used_frames() == 5);
}
void review_brk_prot_none_collision()
{
    Fixture f;
    auto &r = f.resources;
    const uptr start = hw::user_begin + 0x2000;
    r.program_break_base = r.program_break = start;
    r.program_break_limit = start + 16 * hw::page_size;
    CHECK(native::native_brk(r, start + 1));
    const auto blocked = f.map(hw::page_size, start + 2 * hw::page_size, 0);
    CHECK(blocked && blocked.value() == start + 2 * hw::page_size);
    const auto old_space = r.native_space;
    CHECK(!native::native_brk(r, start + 3 * hw::page_size));
    CHECK(r.program_break == start + 1 && mock::used_frames() == 4);
    CHECK(r.native_space.root == old_space.root && r.native_space.generation == old_space.generation);
    CHECK(native::native_munmap(r, blocked.value(), hw::page_size));
    CHECK(native::native_brk(r, start + 3 * hw::page_size));
    CHECK(r.program_break == start + 3 * hw::page_size && mock::used_frames() == 5);
    auto *tail = reinterpret_cast<unsigned char *>(mock::page(r.native_space, start + 2 * hw::page_size)->physical_address);
    tail[0] = 99;
    CHECK(native::native_brk(r, start + 1));
    CHECK(mock::used_frames() == 3);
    CHECK(native::native_brk(r, start + 3 * hw::page_size));
    CHECK(*reinterpret_cast<unsigned char *>(mock::page(r.native_space, start + 2 * hw::page_size)->physical_address) == 0);
}

void check_quarantine_retains_shared_and_staged_frames()
{
    Fixture fixture;
    auto &r = fixture.resources;
    const auto original = r.native_space;
    const auto count = mock::used_frames();
    mock::fail_all_retirement = true;
    CHECK(!fixture.map(4096));
    CHECK(mock::used_frames() == count + 1 && mock::live_spaces() == 2);
    CHECK(native::page_pool.pinned_count() == count + 1);
    CHECK(r.native_space.root == original.root);
    mock::fail_all_retirement = false;
    // The abandoned replacement keeps every mapped frame pinned. Even release
    // of the original process cannot make shared backing reusable too early.
    native::release(r.native_space,r.native_pages);
    CHECK(r.native_pages.empty() && mock::used_frames() == count + 1);
    for (auto &saved : native::quarantined_spaces)
        if (saved.space.root)
        {
            auto space = saved.space;
            auto &pages = saved.pages;
            for (const auto &page : pages)
                if (!native::page_pool.handle(page.physical_address)) CHECK(false);
            CHECK(native::retire_space(space,pages));
        }
    CHECK(mock::used_frames() == 0 && native::page_pool.pinned_count() == 0 && mock::live_spaces() == 0);
    native::runtime_failure = Status::success();
}

void check_noop_unmap_with_exhausted_metadata_quota()
{
    Fixture fixture;
    auto &resources = fixture.resources;
    const auto original = resources.native_space;
    auto *stack = reinterpret_cast<unsigned char *>(
        mock::page(original, hw::user_end - hw::page_size)->physical_address);
    stack[0] = 0x7b;
    std::array<native::NativePagePool::Lease, native::kernel_reserve_pages> held{};
    usize count = 0;
    for (;;)
    {
        auto lease = native::page_pool.allocate_kernel_page();
        if (!lease)
        {
            CHECK(lease.status().code() == StatusCode::no_memory);
            break;
        }
        CHECK(count < held.size());
        held[count++] = std::move(lease.value());
    }
    CHECK(count && native::page_pool.object_count() == native::page_pool.kernel_reserve_count());
    const auto allocations = native::page_pool.allocations();
    const auto releases = native::page_pool.releases();
    const auto users = native::page_pool.user_count();
    const auto areas = resources.memory.area_count();
    mock::fail_create = true;
    mock::fail_destroy_root = original.root;
    CHECK(native::native_munmap(resources, hw::user_begin + 0x4000000, hw::page_size));
    mock::fail_create = false;
    mock::fail_destroy_root = 0;
    CHECK(resources.native_space.root == original.root &&
          resources.native_space.generation == original.generation);
    CHECK(resources.native_pages.size() == 2 && resources.memory.area_count() == areas);
    CHECK(native::page_pool.allocations() == allocations && native::page_pool.releases() == releases);
    CHECK(native::page_pool.user_count() == users && mock::live_spaces() == 1 && stack[0] == 0x7b);
    while (count)
        CHECK(native::page_pool.release_kernel_page(held[--count]));
}

void check_munmap_failure_is_atomic_at_metadata_quota()
{
    Fixture fixture;
    auto &resources = fixture.resources;
    auto mapped = fixture.map(hw::page_size);
    CHECK(mapped);
    const auto original = resources.native_space;
    auto *bytes = reinterpret_cast<unsigned char *>(mock::page(original, mapped.value())->physical_address);
    bytes[0] = 0x6d;
    std::array<native::NativePagePool::Lease, native::kernel_reserve_pages> held{};
    usize count = 0;
    for (;;)
    {
        auto lease = native::page_pool.allocate_kernel_page();
        if (!lease)
        {
            CHECK(lease.status().code() == StatusCode::no_memory);
            break;
        }
        CHECK(count < held.size());
        held[count++] = std::move(lease.value());
    }
    const auto users = native::page_pool.user_count();
    const auto areas = resources.memory.area_count();
    CHECK(native::native_munmap(resources, mapped.value(), hw::page_size).code() == StatusCode::no_memory);
    CHECK(resources.native_space.root == original.root &&
          resources.native_space.generation == original.generation);
    CHECK(resources.native_pages.size() == 3 && resources.memory.area_count() == areas);
    CHECK(native::page_pool.user_count() == users && mock::live_spaces() == 1 && bytes[0] == 0x6d);
    CHECK(mock::page(resources.native_space, mapped.value())->physical_address == reinterpret_cast<uptr>(bytes));
    CHECK(count >= 2);
    // A native candidate now leases both mapping records and exact tokens.
    CHECK(native::page_pool.release_kernel_page(held[--count]));
    CHECK(native::page_pool.release_kernel_page(held[--count]));
    CHECK(native::native_munmap(resources, mapped.value(), hw::page_size));
    CHECK(resources.native_pages.size() == 2 && native::page_pool.user_count() == users - 1);
    CHECK(mock::page(resources.native_space, mapped.value()) == nullptr);
    while (count)
        CHECK(native::page_pool.release_kernel_page(held[--count]));
}

void check_complete_runtime_purpose_budget()
{
    CHECK(native::activate_pool());
    CHECK(native::page_pool.used_count() == 0);
    std::array<native::Pages, hw::max_address_spaces + 1> directories;
    for (usize i = 0; i < hw::max_address_spaces; ++i)
    {
        CHECK(directories[i].reserve(native::page_pool, native::Pages::capacity, true));
        CHECK(directories[i].block_count() == native::Pages::max_blocks);
        CHECK(directories[i].frame_block_count() == native::Pages::max_frame_blocks);
    }
    CHECK(directories.back().reserve(native::page_pool, native::Pages::capacity, true));
    CHECK(directories.back().block_count() == native::Pages::max_blocks);
    CHECK(directories.back().frame_block_count() == native::Pages::max_frame_blocks);
    CHECK(native::page_pool.object_count() == native::mapping_metadata_pages);
    std::array<native::NativePagePool::Lease, native::kernel_object_pages> objects;
    for (auto &object : objects)
    {
        auto acquired = native::page_pool.allocate_kernel_page();
        CHECK(acquired);
        object = std::move(acquired.value());
    }
    CHECK(native::page_pool.object_count() == native::kernel_reserve_pages);
    std::array<uptr, native::hosted_page_count> users{};
    const usize user_limit = native::page_pool.user_capacity_count();
    for (usize i = 0; i < user_limit; ++i)
    {
        auto acquired = native::page_pool.allocate();
        CHECK(acquired);
        users[i] = acquired.value();
    }
    CHECK(native::page_pool.free_count() == 0);
    CHECK(native::page_pool.allocate().status().code() == StatusCode::no_memory);
    CHECK(native::page_pool.allocate_kernel_page().status().code() == StatusCode::no_memory);
    CHECK(native::page_pool.user_count() == user_limit);
    CHECK(native::page_pool.object_count() == native::kernel_reserve_pages);
    for (usize i = 0; i < user_limit; ++i) CHECK(native::page_pool.release(users[i]));
    for (auto &object : objects) CHECK(native::page_pool.release_kernel_page(object));
    for (auto &directory : directories) CHECK(directory.clear());
    CHECK(native::page_pool.used_count() == 0 &&
          native::page_pool.free_count() == native::page_pool.total_count());
    CHECK(native::page_pool.quarantined_kernel_page_count() == 0);
}

void check_over_quota_mapping_publication()
{
    // Preserve the original token-less directory as an explicit early refusal;
    // valid alias publication beyond the data-page quota is tested separately.
    Fixture fixture;
    native::Pages aliases;
    const usize count = native::page_pool.user_capacity_count() + 1;
    CHECK(count <= native::Pages::capacity);
    CHECK(aliases.reserve(native::page_pool, count));
    const uptr frame = fixture.resources.native_pages[0].physical_address;
    for (usize i = 0; i < count; ++i)
        CHECK(aliases.push_back({hw::user_begin + i * hw::page_size, frame, hw::page_read}));
    const usize spaces = mock::live_spaces();
    CHECK(native::create_page_space(aliases).status().code() == StatusCode::fault);
    CHECK(mock::live_spaces() == spaces && native::page_pool.user_count() == 2);
    CHECK(aliases.clear());
}

void check_exact_alias_publication_above_data_page_quota()
{
    Fixture fixture;
    CHECK(native::shared_files.bind(native::page_pool));
    auto reservation = native::shared_files.reserve("exact alias capacity");
    CHECK(reservation);
    auto published = reservation.value().publish();
    CHECK(published);
    const auto file = published.value();
    CHECK(native::shared_files.initialize_size(file, hw::page_size));
    CHECK(native::shared_files.add_seals(file, 6));
    const auto frame = native::shared_files.page(file, 0).value();
    const usize count = native::page_pool.user_capacity_count() + 1;
    CHECK(count <= native::Pages::capacity && count > native::page_pool.user_capacity_count());
    const auto baseline_users = native::page_pool.user_count();
    const auto baseline_objects = native::page_pool.object_count();
    const auto baseline_spaces = mock::live_spaces();
    native::Pages aliases;
    CHECK(aliases.reserve(native::page_pool, count, true));
    const auto allocations = native::page_pool.allocations();
    for (usize index = 0; index < count; ++index)
    {
        CHECK(native::shared_files.retain_alias(frame));
        CHECK(aliases.push_back({hw::user_begin + index * hw::page_size, frame.address, hw::page_read}, frame));
    }
    CHECK(native::shared_files.ledger().aliases == count);
    CHECK(native::page_pool.reference_count(frame.address) == count + 1);
    CHECK(native::page_pool.user_count() == baseline_users && native::page_pool.allocations() == allocations);
    // The separate mapping record bound refuses before acquiring any new data
    // ownership or disturbing the already reserved exact-token directory.
    const auto metadata = native::page_pool.object_count();
    CHECK(aliases.reserve(native::page_pool, native::Pages::capacity + 1, true).code() == StatusCode::no_memory);
    CHECK(aliases.size() == count && native::page_pool.object_count() == metadata);
    CHECK(native::shared_files.ledger().aliases == count);
    mock::fail_create = true;
    CHECK(native::create_page_space(aliases).status().code() == StatusCode::no_memory);
    mock::fail_create = false;
    CHECK(mock::live_spaces() == baseline_spaces && native::page_pool.reference_count(frame.address) == count + 1);
    auto created = native::create_page_space(aliases);
    CHECK(created && mock::live_spaces() == baseline_spaces + 1);
    auto space = created.value();
    CHECK(mock::page(space, hw::user_begin)->physical_address == frame.address);
    CHECK(mock::page(space, hw::user_begin + (count - 1) * hw::page_size)->physical_address == frame.address);
    CHECK(native::shared_files.release_description(file));
    CHECK(!native::shared_files.info(file).value().description && native::shared_files.info(file).value().aliases == count);
    native::release(space, aliases);
    CHECK(!space.root && aliases.empty() && mock::live_spaces() == baseline_spaces);
    const auto ledger = native::shared_files.ledger();
    CHECK(!ledger.files && !ledger.aliases && !ledger.retiring_files && !ledger.retained_frames && !ledger.reserved_cells);
    CHECK(!native::page_pool.valid(frame) && native::page_pool.user_count() == baseline_users - 1);
    CHECK(native::page_pool.object_count() == baseline_objects);
}

void check_shared_pressure_with_live_image_footprint()
{
    {
        Fixture fixture;
        // Match the genuine shell plus POSIX image footprint, without claiming
        // that these hosted backing frames execute an ELF or isolate a process.
        std::array<uptr, 74> image_frames{};
        for (auto &frame : image_frames)
        {
            auto acquired = native::page_pool.allocate();
            CHECK(acquired);
            frame = acquired.value();
        }
        CHECK(native::page_pool.user_count() == 76);
        auto shared_map = [&](usize pages) {
            return native::native_mmap(fixture.resources, 0, pages * hw::page_size, 3,
                                       posix::map_SHARED | posix::map_ANONYMOUS, ~uptr{0}, 0);
        };
        const usize mapping_pages = (native::page_pool.user_capacity_count() - 76) / 2;
        CHECK(mapping_pages > 1);
        auto first = shared_map(mapping_pages);
        CHECK(first);
        auto *first_bytes = reinterpret_cast<unsigned char *>(mock::page(fixture.resources.native_space, first.value())->physical_address);
        auto *last_bytes = reinterpret_cast<unsigned char *>(mock::page(fixture.resources.native_space, first.value() + (mapping_pages - 1) * hw::page_size)->physical_address);
        first_bytes[0] = 61;
        last_bytes[0] = 62;
        auto second = shared_map(mapping_pages);
        CHECK(second);
        auto *second_bytes = reinterpret_cast<unsigned char *>(mock::page(fixture.resources.native_space, second.value())->physical_address);
        second_bytes[0] = 63;
        const usize occupied = 76 + 2 * mapping_pages;
        CHECK(native::page_pool.user_count() == occupied);
        const auto original = fixture.resources.native_space;
        const usize objects = native::page_pool.object_count();
        const usize records = fixture.resources.native_pages.size();
        CHECK(shared_map(native::page_pool.user_capacity_count() - occupied + 1).status().code() == StatusCode::no_memory);
        CHECK(fixture.resources.native_space.root == original.root &&
              fixture.resources.native_space.generation == original.generation);
        CHECK(native::page_pool.user_count() == occupied && native::page_pool.object_count() == objects);
        CHECK(fixture.resources.native_pages.size() == records && mock::live_spaces() == 1);
        CHECK(first_bytes[0] == 61 && last_bytes[0] == 62 && second_bytes[0] == 63);
        CHECK(native::page_pool.pinned_count() == 0 && native::page_pool.pending_count() == 0);
        CHECK(native::native_munmap(fixture.resources, second.value(), mapping_pages * hw::page_size));
        CHECK(native::native_munmap(fixture.resources, first.value(), mapping_pages * hw::page_size));
        CHECK(native::page_pool.user_count() == 76);
        for (uptr frame : image_frames) CHECK(native::page_pool.release(frame));
    }
    CHECK(native::page_pool.used_count() == 0 && mock::live_spaces() == 0);
    CHECK(native::page_pool.quarantined_kernel_page_count() == 0);
}

native::SharedFileHandle shared_file()
{
    CHECK(native::shared_files.bind(native::page_pool));
    auto reserved = native::shared_files.reserve("quarantine English buffer");
    CHECK(reserved);
    auto published = reserved.value().publish();
    CHECK(published);
    CHECK(native::shared_files.initialize_size(published.value(), hw::page_size));
    CHECK(native::shared_files.add_seals(published.value(), 6));
    return published.value();
}

native::QuarantinedSpace *quarantine_for(const hw::AddressSpace &space)
{
    for (auto &saved : native::quarantined_spaces)
        if (saved.used && native::same_space(saved.space, space)) return &saved;
    return nullptr;
}

static_assert(!std::is_copy_constructible_v<native::Image> && !std::is_copy_assignable_v<native::Image>);
static_assert(!std::is_copy_constructible_v<native::ImageRetirementReservation> &&
              !std::is_move_assignable_v<native::ImageRetirementReservation>);

native::Image modeled_image()
{
    auto reserved = native::ImageRetirementReservation::reserve();
    CHECK(reserved);
    native::Image image{std::move(reserved.value())};
    CHECK(native::allocate(image.pages, hw::user_begin + 4 * hw::page_size, hw::page_read | hw::page_execute));
    CHECK(native::allocate(image.pages, hw::user_begin + 5 * hw::page_size, hw::page_read | hw::page_write));
    const auto created = native::create_page_space(image.pages);
    CHECK(created);
    image.space = created.value();
    return image;
}

void finish_retained_image(hw::AddressSpace identity)
{
    native::Pages empty;
    CHECK(native::retire_space(identity, empty));
    CHECK(!identity.root);
}

void check_image_owner_move_and_destructor()
{
    Fixture fixture;
    const auto users = native::page_pool.user_count();
    const auto objects = native::page_pool.object_count();
    {
        auto source = modeled_image();
        const auto identity = source.space;
        const auto frame = *source.pages.frame(0);
        native::Image moved{std::move(source)};
        CHECK(!source.retirement.active() && !source.space.root && source.pages.empty());
        CHECK(native::same_space(moved.space, identity) && native::page_pool.valid(frame));
        CHECK(mock::live_spaces() == 2 && native::page_pool.user_count() == users + 2);
    }
    CHECK(mock::live_spaces() == 1 && native::page_pool.user_count() == users);
    CHECK(native::page_pool.object_count() == objects);
    for (const auto &saved : native::quarantined_spaces) CHECK(!saved.used && !saved.reserved);
}

void check_exec_failed_old_retirement_skips_install()
{
    Fixture fixture;
    auto old = native::ImageRetirementReservation::reserve(); CHECK(old);
    auto image = modeled_image();
    const auto previous = fixture.resources.native_space;
    const auto replacement_frame = *image.pages.frame(0);
    mock::fail_destroy_root = previous.root;
    usize calls{};
    CHECK(native::retire_then_install(fixture.resources, image, old.value(), [&] {
        ++calls; return Status::success();
    }).code() == StatusCode::busy);
    CHECK(!calls && !fixture.resources.native_space.root && fixture.resources.native_pages.empty());
    CHECK(!image.retirement.active() && !image.space.root && image.pages.empty());
    CHECK(!native::page_pool.valid(replacement_frame));
    auto *saved = quarantine_for(previous);
    CHECK(saved && saved->pages.size() == 2 && saved->pin_complete && !saved->table_retired);
    for (usize index = 0; index < saved->pages.size(); ++index)
    {
        CHECK(saved->ownership[index] == native::QuarantinedSpace::Ownership::discharged);
        CHECK(saved->pin_held[index] && native::page_pool.valid(*saved->pages.frame(index)));
        CHECK(native::page_pool.reference_count(saved->pages[index].physical_address) == 0);
    }
    CHECK(mock::live_spaces() == 1 && native::page_pool.user_count() == 2);
    mock::fail_destroy_root = 0;
    finish_retained_image(previous);
    CHECK(!saved->used && !native::page_pool.used_count() && !mock::live_spaces());
    native::runtime_failure = {};
}

void check_exec_failed_install_retains_replacement()
{
    Fixture fixture;
    auto old = native::ImageRetirementReservation::reserve(); CHECK(old);
    auto image = modeled_image();
    const auto replacement = image.space;
    const auto frame = *image.pages.frame(0);
    mock::fail_destroy_root = replacement.root;
    usize calls{};
    CHECK(native::retire_then_install(fixture.resources, image, old.value(), [&] {
        ++calls;
        CHECK(!fixture.resources.native_space.root && fixture.resources.native_pages.empty());
        CHECK(image.pages.frame(0)->generation == frame.generation);
        return Status::io_error("injected install preparation refusal");
    }).code() == StatusCode::io_error);
    CHECK(calls == 1 && native::runtime_failure.code() == StatusCode::io_error);
    auto *saved = quarantine_for(replacement);
    CHECK(saved && saved->pages.frame(0)->generation == frame.generation && saved->pages.frame(0)->owner == frame.owner);
    CHECK(saved->pin_complete && saved->pin_held[0] && native::page_pool.valid(frame));
    CHECK(!image.retirement.active() && image.pages.empty() && !image.space.root);
    CHECK(mock::live_spaces() == 1 && native::page_pool.user_count() == 2);
    mock::fail_destroy_root = 0;
    finish_retained_image(replacement);
    CHECK(!saved->used && !native::page_pool.used_count() && !mock::live_spaces());
    native::runtime_failure = {};
}

void check_exec_failed_old_shared_aliases_retire_once()
{
    Fixture fixture;
    const auto file = shared_file();
    const auto frame = native::shared_files.page(file, 0).value();
    CHECK(native::native_mmap(fixture.resources, 0, hw::page_size, 3,
        posix::map_SHARED, 3, 0, nullptr, hw::page_size, file));
    CHECK(native::native_mmap(fixture.resources, 0, hw::page_size, 3,
        posix::map_SHARED, 3, 0, nullptr, hw::page_size, file));
    CHECK(native::shared_files.release_description(file));
    CHECK(native::shared_files.ledger().files == 1 && native::shared_files.ledger().aliases == 2);
    auto previous = native::ImageRetirementReservation::reserve(); CHECK(previous);
    auto replacement = modeled_image();
    const auto identity = fixture.resources.native_space;
    mock::fail_destroy_root = identity.root;
    usize calls{};
    CHECK(!native::retire_then_install(fixture.resources, replacement, previous.value(), [&] {
        ++calls; return Status::success();
    }));
    CHECK(!calls && !native::shared_files.ledger().files && !native::shared_files.ledger().aliases);
    auto *saved = quarantine_for(identity); CHECK(saved && saved->pages.size() == 4 && saved->pinned_count == 4);
    usize aliases{};
    for (usize index = 0; index < saved->pages.size(); ++index)
        if (saved->pages[index].physical_address == frame.address)
        {
            ++aliases;
            CHECK(saved->pages.frame(index)->generation == frame.generation && saved->pin_held[index]);
            CHECK(saved->ownership[index] == native::QuarantinedSpace::Ownership::discharged);
        }
    CHECK(aliases == 2 && native::page_pool.valid(frame) && native::page_pool.reference_count(frame.address) == 0);
    CHECK(native::page_pool.pending_count() == 3 && mock::live_spaces() == 1);
    mock::fail_destroy_root = 0; finish_retained_image(identity);
    const auto ledger = native::shared_files.ledger();
    CHECK(!ledger.files && !ledger.aliases && !ledger.retiring_files && !ledger.retained_frames && !ledger.reserved_cells);
    CHECK(!native::page_pool.valid(frame) && !native::page_pool.used_count() && !mock::live_spaces());
    native::runtime_failure = {};
}

void check_exec_both_failed_tables_keep_distinct_tokens()
{
    Fixture fixture;
    auto old = native::ImageRetirementReservation::reserve(); CHECK(old);
    auto image = modeled_image();
    const auto previous = fixture.resources.native_space;
    const auto replacement = image.space;
    const auto previous_frame = *fixture.resources.native_pages.frame(0);
    const auto replacement_frame = *image.pages.frame(0);
    mock::fail_all_retirement = true;
    usize calls{};
    CHECK(!native::retire_then_install(fixture.resources, image, old.value(), [&] {
        ++calls; return Status::success();
    }));
    CHECK(!calls && mock::live_spaces() == 2);
    auto *first = quarantine_for(previous), *second = quarantine_for(replacement);
    CHECK(first && second && first != second && first->pages.size() == 2 && second->pages.size() == 2);
    CHECK(first->pages.frame(0)->address == previous_frame.address && first->pages.frame(0)->generation == previous_frame.generation);
    CHECK(second->pages.frame(0)->address == replacement_frame.address && second->pages.frame(0)->generation == replacement_frame.generation);
    CHECK(native::page_pool.pinned_count() == 4 && native::page_pool.pending_count() == 4);
    mock::fail_all_retirement = false;
    finish_retained_image(previous); finish_retained_image(replacement);
    CHECK(!native::page_pool.used_count() && !mock::live_spaces());
    native::runtime_failure = {};
}

void check_image_partial_pin_failure_keeps_pending_owners()
{
    Fixture fixture;
    const auto users = native::page_pool.user_count();
    auto image = modeled_image();
    const auto identity = image.space;
    const auto blocked = *image.pages.frame(1);
    for (usize count = 0; count < 65535; ++count) CHECK(native::page_pool.pin(blocked));
    mock::fail_destroy_root = identity.root;
    CHECK(image.retire().code() == StatusCode::fault);
    auto *saved = quarantine_for(identity);
    CHECK(saved && !saved->pin_complete && saved->pinned_count == 1 && saved->pin_held[0] && !saved->pin_held[1]);
    CHECK(saved->pages.frame(1)->generation == blocked.generation && mock::live_spaces() == 2);
    for (usize index = 0; index < saved->pages.size(); ++index)
    {
        CHECK(saved->ownership[index] == native::QuarantinedSpace::Ownership::pending);
        CHECK(native::page_pool.reference_count(saved->pages[index].physical_address) == 1);
    }
    CHECK(native::page_pool.user_count() == users + 2);
    for (usize count = 0; count < 65535; ++count) CHECK(native::page_pool.unpin(blocked));
    mock::fail_destroy_root = 0;
    finish_retained_image(identity);
    CHECK(!saved->used && native::page_pool.user_count() == users && native::page_pool.pinned_count() == 0);
    native::runtime_failure = {};
}

void check_image_failed_owner_release_retries_once()
{
    Fixture fixture;
    const auto users = native::page_pool.user_count();
    auto image = modeled_image();
    const auto first = *image.pages.frame(0), second = *image.pages.frame(1);
    native::NativePagePoolFaultProbe::exact_release(native::page_pool, first, 1);
    CHECK(!image.retire());
    native::QuarantinedSpace *saved{};
    for (auto &row : native::quarantined_spaces) if (row.used && row.table_retired) saved = &row;
    CHECK(saved && saved->pages.frame(0)->generation == first.generation);
    CHECK(saved->ownership[0] == native::QuarantinedSpace::Ownership::pending &&
          saved->ownership[1] == native::QuarantinedSpace::Ownership::discharged);
    CHECK(native::page_pool.valid(first) && !native::page_pool.valid(second));
    CHECK(native::page_pool.user_count() == users + 1 && mock::live_spaces() == 1);
    native::NativePagePoolFaultProbe::exact_release(native::page_pool, {}, 0);
    CHECK(native::retry_quarantined_retirement());
    CHECK(!saved->used && !native::page_pool.valid(first) && native::page_pool.user_count() == users);
    native::runtime_failure = {};
}

void check_image_retirement_capacity_preflight()
{
    Fixture fixture;
    const auto previous = fixture.resources.native_space;
    const auto users = native::page_pool.user_count(), objects = native::page_pool.object_count();
    const auto allocations = native::page_pool.allocations();
    std::array<std::optional<native::ImageRetirementReservation>, native::quarantined_spaces.size()> held;
    for (auto &slot : held)
    {
        auto reserved = native::ImageRetirementReservation::reserve(); CHECK(reserved);
        slot.emplace(std::move(reserved.value()));
    }
    CHECK(native::ImageRetirementReservation::reserve().status().code() == StatusCode::no_memory);
    CHECK(native::same_space(fixture.resources.native_space, previous) && fixture.resources.native_pages.size() == 2);
    CHECK(native::page_pool.user_count() == users && native::page_pool.object_count() == objects &&
          native::page_pool.allocations() == allocations && mock::live_spaces() == 1);
    for (auto &slot : held) slot.reset();
    // Metadata quota exhaustion is independent of reservation capacity. The
    // reservation succeeds, but staging refuses and returns its acquired frame.
    std::array<native::NativePagePool::Lease, native::kernel_reserve_pages> metadata;
    usize acquired{};
    for (;;) {
        auto lease = native::page_pool.allocate_kernel_page(); if (!lease) break;
        metadata[acquired++] = std::move(lease.value());
    }
    {
        auto owner = native::ImageRetirementReservation::reserve(); CHECK(owner);
        native::Image image{std::move(owner.value())};
        CHECK(!native::allocate(image.pages, hw::user_begin + 8 * hw::page_size, hw::page_read));
        CHECK(image.pages.empty() && native::page_pool.user_count() == users);
    }
    while (acquired) CHECK(native::page_pool.release_kernel_page(metadata[--acquired]));
    CHECK(native::page_pool.object_count() == objects && native::page_pool.user_count() == users);
    for (const auto &saved : native::quarantined_spaces) CHECK(!saved.used && !saved.reserved);
}

void check_repeated_teardown_discharges_shared_alias_once()
{
    Fixture fixture;
    const auto file = shared_file();
    const auto frame = native::shared_files.page(file, 0).value();
    auto mapped = native::native_mmap(fixture.resources, 0, hw::page_size, 3,
        posix::map_SHARED, 3, 0, nullptr, hw::page_size, file);
    CHECK(mapped);
    native::Pages survivor;
    CHECK(survivor.reserve(native::page_pool, 1, true));
    CHECK(native::retain_page_reference(frame));
    CHECK(survivor.push_back({hw::user_begin + 0x10000, frame.address, 3}, frame));
    CHECK(native::shared_files.info(file).value().aliases == 2);
    const auto original = fixture.resources.native_space;
    mock::fail_destroy_root = original.root;
    native::release(fixture.resources.native_space, fixture.resources.native_pages);
    auto *saved = quarantine_for(original);
    CHECK(saved && saved->pin_complete && fixture.resources.native_pages.empty());
    CHECK(native::shared_files.info(file).value().aliases == 1);
    CHECK(native::page_pool.reference_count(frame.address) == 2);
    const auto pins = native::page_pool.pinned_count();
    native::release(fixture.resources.native_space, fixture.resources.native_pages);
    CHECK(native::shared_files.info(file).value().aliases == 1);
    CHECK(native::page_pool.reference_count(frame.address) == 2 && native::page_pool.pinned_count() == pins);
    for (usize index = 0; index < saved->pages.size(); ++index)
        CHECK(saved->ownership[index] == native::QuarantinedSpace::Ownership::discharged);
    CHECK(native::shared_files.release_description(file));
    mock::fail_destroy_root = 0;
    native::release(fixture.resources.native_space, fixture.resources.native_pages);
    CHECK(!quarantine_for(original) && native::page_pool.pinned_count() == 0);
    CHECK(native::shared_files.info(file).value().aliases == 1);
    CHECK(native::page_pool.reference_count(frame.address) == 2);
    hw::AddressSpace empty;
    native::release(empty, survivor);
    CHECK(!native::shared_files.valid(file) && native::shared_files.ledger().files == 0);
    CHECK(native::page_pool.used_count() == 0 && mock::live_spaces() == 0);
    native::runtime_failure = {};
}

void check_alias_discharge_is_consumed_before_failed_base_retirement()
{
    Fixture fixture;
    const auto file = shared_file();
    const auto frame = native::shared_files.page(file, 0).value();
    CHECK(native::native_mmap(fixture.resources, 0, hw::page_size, 3,
        posix::map_SHARED, 3, 0, nullptr, hw::page_size, file));
    CHECK(native::shared_files.release_description(file));
    native::SharedMemoryFilesFaultProbe::final_release(native::shared_files, true);
    const auto original = fixture.resources.native_space;
    mock::fail_destroy_root = original.root;
    native::release(fixture.resources.native_space, fixture.resources.native_pages);
    auto *saved = quarantine_for(original);
    CHECK(saved && native::shared_files.ledger().retiring_files == 1);
    CHECK(native::shared_files.ledger().aliases == 0 && native::page_pool.reference_count(frame.address) == 1);
    for (usize index = 0; index < saved->pages.size(); ++index)
        CHECK(saved->ownership[index] == native::QuarantinedSpace::Ownership::discharged);
    native::release(fixture.resources.native_space, fixture.resources.native_pages);
    CHECK(native::shared_files.ledger().retiring_files == 1 && native::page_pool.reference_count(frame.address) == 1);
    native::SharedMemoryFilesFaultProbe::final_release(native::shared_files, false);
    CHECK(native::shared_files.retry_retirement());
    CHECK(native::page_pool.reference_count(frame.address) == 0 && native::page_pool.pending_count() == 3);
    mock::fail_destroy_root = 0;
    native::release(fixture.resources.native_space, fixture.resources.native_pages);
    CHECK(native::shared_files.ledger().files == 0 && native::page_pool.used_count() == 0);
    CHECK(mock::live_spaces() == 0);
    native::runtime_failure = {};
}

void check_write_seal_requires_retired_table_pins()
{
    Fixture fixture;
    const auto file = shared_file();
    const auto frame = native::shared_files.page(file, 0).value();
    const auto mapped = native::native_mmap(fixture.resources, 0, hw::page_size, 3,
        posix::map_SHARED, 3, 0, nullptr, hw::page_size, file);
    CHECK(mapped);
    const auto original = fixture.resources.native_space;
    mock::fail_destroy_root = original.root;
    native::release(fixture.resources.native_space, fixture.resources.native_pages);
    auto *saved = quarantine_for(original);
    CHECK(saved && saved->pin_complete && !saved->table_retired);
    CHECK(fixture.resources.native_pages.empty());
    CHECK(native::shared_files.info(file).value().aliases == 0);
    CHECK(native::page_pool.reference_count(frame.address) == 1);
    CHECK(native::page_pool.pin_count(frame).value() == 1);
    CHECK(mock::page(original, mapped.value())->flags == (hw::page_read | hw::page_write));
    CHECK(native::shared_files.add_seals(file, 8).code() == StatusCode::busy);
    CHECK(native::shared_files.add_seals(file, 8 | 1).code() == StatusCode::busy);
    CHECK(native::shared_files.info(file).value().seals == 6);
    for (usize index = 0; index < saved->pages.size(); ++index)
        CHECK(saved->ownership[index] == native::QuarantinedSpace::Ownership::discharged);
    const auto releases = native::page_pool.releases();
    native::release(fixture.resources.native_space, fixture.resources.native_pages);
    CHECK(native::shared_files.info(file).value().aliases == 0);
    CHECK(native::page_pool.reference_count(frame.address) == 1);
    CHECK(native::page_pool.pin_count(frame).value() == 1);
    CHECK(native::page_pool.releases() == releases);
    mock::fail_destroy_root = 0;
    native::release(fixture.resources.native_space, fixture.resources.native_pages);
    CHECK(!quarantine_for(original) && !mock::live_spaces());
    CHECK(native::page_pool.pin_count(frame).value() == 0);
    CHECK(native::page_pool.user_count() == 1 && native::shared_files.info(file).value().aliases == 0);
    CHECK(native::shared_files.add_seals(file, 8));
    CHECK(native::shared_files.info(file).value().seals == 14);
    native::release(fixture.resources.native_space, fixture.resources.native_pages);
    CHECK(native::page_pool.reference_count(frame.address) == 1 && native::shared_files.info(file).value().aliases == 0);
    CHECK(native::shared_files.release_description(file));
    CHECK(!native::page_pool.valid(frame) && !native::page_pool.pin_count(frame));
    CHECK(native::page_pool.used_count() == 0);
    native::runtime_failure = {};
}

void check_write_seal_mapping_refusals_are_read_only()
{
    Fixture fixture;
    const auto file = shared_file();
    const auto frame = native::shared_files.page(file, 0).value();
    reinterpret_cast<std::byte *>(frame.address)[0] = std::byte{'S'};
    const auto before_seal = native::native_mmap(fixture.resources, 0, hw::page_size, posix::prot_READ,
        posix::map_SHARED, 3, 0, nullptr, hw::page_size, file);
    CHECK(before_seal);
    CHECK(native::shared_files.add_seals(file, 8 | 1).code() == StatusCode::busy);
    CHECK(native::shared_files.info(file).value().seals == 6);
    CHECK(native::native_mprotect(fixture.resources, before_seal.value(), hw::page_size, posix::prot_NONE));
    CHECK(!mock::page(fixture.resources.native_space, before_seal.value()));
    CHECK(native::shared_files.add_seals(file, 8).code() == StatusCode::busy);
    CHECK(native::shared_files.info(file).value().seals == 6);
    CHECK(native::native_munmap(fixture.resources, before_seal.value(), hw::page_size));
    CHECK(native::shared_files.info(file).value().aliases == 0 && native::page_pool.pin_count(frame).value() == 0);
    CHECK(native::shared_files.add_seals(file, 8));

    const auto anonymous = fixture.map(2 * hw::page_size, 0, posix::prot_READ);
    CHECK(anonymous);
    const auto address = anonymous.value(), shared_address = address + hw::page_size;
    const auto sealed = native::native_mmap(fixture.resources, shared_address, hw::page_size, posix::prot_READ,
        posix::map_SHARED | posix::map_FIXED, 3, 0, nullptr, hw::page_size, file);
    CHECK(sealed && sealed.value() == shared_address);
    CHECK(native::shared_files.add_seals(file, 8));
    const auto original = fixture.resources.native_space;
    const auto *anonymous_owner = native::owned_frame(fixture.resources.native_pages, *mock::page(original, address));
    CHECK(anonymous_owner);
    const auto anonymous_frame = *anonymous_owner;
    const auto allocations = native::page_pool.allocations(), releases = native::page_pool.releases();
    const auto users = native::page_pool.user_count(), objects = native::page_pool.object_count();
    const auto aliases = native::shared_files.info(file).value().aliases;
    const auto references = native::page_pool.reference_count(frame.address);
    const auto spaces = mock::live_spaces();
    CHECK(native::native_mmap(fixture.resources, 0, hw::page_size, 3,
        posix::map_SHARED, 3, 0, nullptr, hw::page_size, file).status().code() == StatusCode::permission);
    CHECK(native::native_mmap(fixture.resources, address, hw::page_size, 3,
        posix::map_SHARED | posix::map_FIXED, 3, 0, nullptr, hw::page_size, file).status().code() == StatusCode::permission);
    CHECK(native::native_mmap(fixture.resources, shared_address, hw::page_size, 3,
        posix::map_SHARED | posix::map_FIXED, 3, 0, nullptr, hw::page_size, file).status().code() == StatusCode::permission);
    CHECK(native::native_mprotect(fixture.resources, shared_address, hw::page_size, posix::prot_WRITE).code() == StatusCode::denied);
    CHECK(native::native_mprotect(fixture.resources, address, 2 * hw::page_size, 3).code() == StatusCode::denied);
    CHECK(native::same_space(fixture.resources.native_space, original));
    CHECK(mock::page(original, address)->physical_address == anonymous_frame.address);
    CHECK(mock::page(original, address)->flags == hw::page_read);
    CHECK(mock::page(original, shared_address)->physical_address == frame.address);
    CHECK(mock::page(original, shared_address)->flags == hw::page_read);
    CHECK(!fixture.resources.memory.allows_user_access(address, 1, memory::page_write));
    CHECK(!fixture.resources.memory.allows_user_access(shared_address, 1, memory::page_write));
    CHECK(native::page_pool.allocations() == allocations && native::page_pool.releases() == releases);
    CHECK(native::page_pool.user_count() == users && native::page_pool.object_count() == objects);
    CHECK(native::shared_files.info(file).value().aliases == aliases && native::shared_files.info(file).value().seals == 14);
    CHECK(native::page_pool.reference_count(frame.address) == references && native::page_pool.pin_count(frame).value() == 0);
    CHECK(mock::live_spaces() == spaces && reinterpret_cast<const std::byte *>(frame.address)[0] == std::byte{'S'});
    CHECK(native::native_mprotect(fixture.resources, shared_address, hw::page_size, posix::prot_NONE));
    CHECK(!mock::page(fixture.resources.native_space, shared_address));
    CHECK(native::native_mprotect(fixture.resources, shared_address, hw::page_size, posix::prot_READ));
    CHECK(native::shared_files.release_description(file));
    CHECK(mock::page(fixture.resources.native_space, shared_address)->physical_address == frame.address);
    CHECK(reinterpret_cast<const std::byte *>(frame.address)[0] == std::byte{'S'});
    CHECK(native::native_munmap(fixture.resources, address, 2 * hw::page_size));
    CHECK(!native::shared_files.valid(file) && !native::page_pool.valid(frame));
}

void check_partial_pin_retirement_preserves_failed_token()
{
    Fixture fixture;
    const auto original = fixture.resources.native_space;
    mock::fail_destroy_root = original.root;
    native::release(fixture.resources.native_space, fixture.resources.native_pages);
    auto *saved = quarantine_for(original);
    CHECK(saved && saved->pages.size() == 2 && saved->pinned_count == 2);
    const auto failed = *saved->pages.frame(1);
    auto *registry = const_cast<memory::PhysicalFrame *>(native::page_pool.handle(failed.address));
    CHECK(registry);
    ++registry->generation;
    mock::fail_destroy_root = 0;
    auto retired = original;
    CHECK(!native::retire_space(retired, fixture.resources.native_pages));
    CHECK(!retired.root && saved->used && saved->table_retired);
    CHECK(!saved->pin_held[0] && saved->pin_held[1] && saved->pinned_count == 1);
    CHECK(saved->pages.size() == 2 && saved->pages.frame(1)->generation == failed.generation);
    CHECK(native::page_pool.user_count() == 1 && native::page_pool.pinned_count() == 1);
    // The first token is already physically retired. A retry must skip it,
    // rather than fail on its now-stale generation or unpin a reused address.
    registry->generation = failed.generation;
    CHECK(native::retry_quarantined_retirement());
    CHECK(!saved->used && native::page_pool.used_count() == 0 && mock::live_spaces() == 0);
    fixture.resources.native_space = {};
    native::runtime_failure = {};
}

void check_incomplete_quarantine_keeps_all_owner_edges()
{
    Fixture fixture;
    const auto original = fixture.resources.native_space;
    auto *token = const_cast<memory::PhysicalFrame *>(fixture.resources.native_pages.frame(1));
    CHECK(token);
    const auto generation = token->generation;
    ++token->generation;
    CHECK(!native::quarantine(original, fixture.resources.native_pages));
    auto *saved = quarantine_for(original);
    CHECK(saved && saved->used && !saved->pin_complete && saved->pinned_count == 1);
    CHECK(fixture.resources.native_pages.empty() && mock::used_frames() == 2 && mock::live_spaces() == 1);
    CHECK(!native::discharge_quarantine(*saved, true));
    CHECK(native::page_pool.reference_count(saved->pages[0].physical_address) == 1);
    CHECK(native::page_pool.reference_count(saved->pages[1].physical_address) == 1);
    for (usize index = 0; index < saved->pages.size(); ++index)
        CHECK(saved->ownership[index] == native::QuarantinedSpace::Ownership::pending);
    const_cast<memory::PhysicalFrame *>(saved->pages.frame(1))->generation = generation;
    CHECK(native::retire_space(fixture.resources.native_space, fixture.resources.native_pages));
    CHECK(!saved->used && native::page_pool.used_count() == 0 && mock::live_spaces() == 0);
    native::runtime_failure = {};
}

void check_missing_and_stale_tokens_refuse_without_mutation()
{
    Fixture fixture;
    auto &resources = fixture.resources;
    const auto original = resources.native_space;
    const auto records = resources.native_pages.size();
    const auto users = native::page_pool.user_count();
    auto *frame = const_cast<memory::PhysicalFrame *>(resources.native_pages.frame(0));
    CHECK(frame);
    const auto generation = frame->generation;
    ++frame->generation;
    CHECK(fixture.map(hw::page_size).status().code() == StatusCode::fault);
    CHECK(native::native_munmap(resources, hw::user_end - hw::page_size, hw::page_size).code() == StatusCode::fault);
    native::release(resources.native_space, resources.native_pages);
    CHECK(!native::runtime_failure && resources.native_pages.size() == records);
    CHECK(native::page_pool.user_count() == users && native::same_space(resources.native_space, original));
    frame->generation = generation;
    native::runtime_failure = {};
    native::Pages missing;
    CHECK(missing.reserve(native::page_pool, records));
    for (const auto &page : resources.native_pages) CHECK(missing.push_back(page));
    resources.native_pages.swap(missing);
    CHECK(fixture.map(hw::page_size).status().code() == StatusCode::fault);
    CHECK(native::native_munmap(resources, hw::user_end - hw::page_size, hw::page_size).code() == StatusCode::fault);
    native::release(resources.native_space, resources.native_pages);
    CHECK(!native::runtime_failure && resources.native_pages.size() == records);
    CHECK(native::page_pool.user_count() == users && native::same_space(resources.native_space, original));
    resources.native_pages.swap(missing);
    CHECK(missing.clear());
    native::runtime_failure = {};
}

native::QuarantinedSpace *retained_range()
{
    native::QuarantinedSpace *result{};
    for (auto &saved : native::quarantined_spaces)
        if (saved.used && saved.table_retired)
        {
            CHECK(!result);
            result = &saved;
        }
    return result;
}

void check_range_owner_states(const native::QuarantinedSpace &saved, usize pending, usize discharged, usize borrowed)
{
    usize actual_pending{}, actual_discharged{}, actual_borrowed{};
    for (usize index = 0; index < saved.pages.size(); ++index)
    {
        actual_pending += saved.ownership[index] == native::QuarantinedSpace::Ownership::pending;
        actual_discharged += saved.ownership[index] == native::QuarantinedSpace::Ownership::discharged;
        actual_borrowed += saved.ownership[index] == native::QuarantinedSpace::Ownership::borrowed;
        CHECK(!saved.pin_held[index]);
    }
    CHECK(actual_pending == pending && actual_discharged == discharged && actual_borrowed == borrowed);
}

void check_range_rollback_retains_unconsumed_owners()
{
    Fixture fixture;
    const auto mapped = fixture.map(2 * hw::page_size, hw::user_begin + 0x10000);
    CHECK(mapped);
    auto &resources = fixture.resources;
    const auto original = resources.native_space;
    const auto users = native::page_pool.user_count();
    const auto old_first = mock::page(original, mapped.value())->physical_address;
    reinterpret_cast<unsigned char *>(old_first)[0] = 77;
    mock::fail_create = true;
    native::NativePagePoolFaultProbe::all_releases(native::page_pool, true);
    CHECK(!native::native_mmap(resources, mapped.value(), 2 * hw::page_size, 3,
        posix::map_PRIVATE | posix::map_ANONYMOUS | posix::map_FIXED, ~uptr{0}, 0));
    auto *saved = retained_range();
    CHECK(saved && saved->pages.exact_frames());
    check_range_owner_states(*saved, 2, 0, 2);
    CHECK(native::page_pool.user_count() == users + 2 && mock::live_spaces() == 1);
    CHECK(native::same_space(resources.native_space, original));
    CHECK(mock::page(original, mapped.value())->physical_address == old_first);
    CHECK(reinterpret_cast<unsigned char *>(old_first)[0] == 77);
    for (const auto &page : resources.native_pages)
        CHECK(native::page_pool.reference_count(page.physical_address) == 1);
    native::NativePagePoolFaultProbe::all_releases(native::page_pool, false);
    mock::fail_create = false;
    CHECK(native::retry_quarantined_retirement());
    CHECK(!saved->used && native::page_pool.user_count() == users && mock::live_spaces() == 1);
    CHECK(reinterpret_cast<unsigned char *>(old_first)[0] == 77);
    native::runtime_failure = {};
}

void check_fixed_replacement_retains_only_failed_removed_owner()
{
    Fixture fixture;
    const auto mapped = fixture.map(3 * hw::page_size, hw::user_begin + 0x10000);
    CHECK(mapped);
    auto &resources = fixture.resources;
    const auto old_address = mock::page(resources.native_space, mapped.value())->physical_address;
    const auto old = *native::page_pool.handle(old_address);
    const auto users = native::page_pool.user_count();
    native::NativePagePoolFaultProbe::exact_release(native::page_pool, old, 1);
    auto replacement = native::native_mmap(resources, mapped.value(), 2 * hw::page_size, 3,
        posix::map_PRIVATE | posix::map_ANONYMOUS | posix::map_FIXED, ~uptr{0}, 0);
    CHECK(replacement && replacement.value() == mapped.value());
    CHECK(!native::runtime_failure);
    auto *saved = retained_range();
    CHECK(saved);
    check_range_owner_states(*saved, 1, 1, 3);
    CHECK(native::page_pool.valid(old) && native::page_pool.reference_count(old.address) == 1);
    CHECK(native::page_pool.user_count() == users + 1);
    CHECK(mock::page(resources.native_space, mapped.value())->physical_address != old.address);
    for (const auto &page : resources.native_pages)
        CHECK(native::page_pool.reference_count(page.physical_address) == 1);
    CHECK(native::retry_quarantined_retirement());
    CHECK(!saved->used && !native::page_pool.valid(old) && native::page_pool.user_count() == users);
    native::NativePagePoolFaultProbe::exact_release(native::page_pool, {}, 0);
    native::runtime_failure = {};
}

void check_munmap_retains_only_failed_removed_owner()
{
    Fixture fixture;
    const auto mapped = fixture.map(3 * hw::page_size);
    CHECK(mapped);
    auto &resources = fixture.resources;
    const auto old_address = mock::page(resources.native_space, mapped.value())->physical_address;
    const auto old = *native::page_pool.handle(old_address);
    const auto users = native::page_pool.user_count();
    native::NativePagePoolFaultProbe::exact_release(native::page_pool, old, 1);
    CHECK(native::native_munmap(resources, mapped.value(), 2 * hw::page_size));
    CHECK(!native::runtime_failure);
    auto *saved = retained_range();
    CHECK(saved);
    check_range_owner_states(*saved, 1, 1, 3);
    CHECK(!mock::page(resources.native_space, mapped.value()));
    CHECK(mock::page(resources.native_space, mapped.value() + 2 * hw::page_size));
    CHECK(native::page_pool.valid(old) && native::page_pool.reference_count(old.address) == 1);
    CHECK(native::page_pool.user_count() == users - 1);
    for (const auto &page : resources.native_pages)
        CHECK(native::page_pool.reference_count(page.physical_address) == 1);
    CHECK(native::retry_quarantined_retirement());
    CHECK(!saved->used && !native::page_pool.valid(old) && native::page_pool.user_count() == users - 2);
    native::NativePagePoolFaultProbe::exact_release(native::page_pool, {}, 0);
    native::runtime_failure = {};
}

void check_range_retirement_capacity_preflight_is_read_only()
{
    Fixture fixture;
    CHECK(fixture.map(hw::page_size));
    auto &resources = fixture.resources;
    const auto original = resources.native_space;
    const auto allocation_count = native::page_pool.allocations();
    const auto release_count = native::page_pool.releases();
    const auto users = native::page_pool.user_count();
    for (auto &saved : native::quarantined_spaces)
    {
        CHECK(!saved.used && !saved.reserved && saved.pages.empty());
        saved.reserved = true;
    }
    CHECK(fixture.map(hw::page_size).status().code() == StatusCode::no_memory);
    CHECK(native::native_munmap(resources, hw::user_begin, hw::page_size).code() == StatusCode::no_memory);
    CHECK(native::same_space(resources.native_space, original) && resources.native_pages.size() == 3);
    CHECK(native::page_pool.user_count() == users && mock::live_spaces() == 1);
    CHECK(native::page_pool.allocations() == allocation_count && native::page_pool.releases() == release_count);
    for (auto &saved : native::quarantined_spaces) saved.reserved = false;
}

void check_shared_file_write_request_uses_existing_rw_ptes()
{
    Fixture fixture;
    const auto file = shared_file();
    const auto frame = native::shared_files.page(file, 0).value();
    const auto mapped = native::native_mmap(fixture.resources, 0, hw::page_size, posix::prot_WRITE,
        posix::map_SHARED, 3, 0, nullptr, hw::page_size, file);
    CHECK(mapped);
    const auto aliases = native::shared_files.info(file).value().aliases;
    const auto references = native::page_pool.reference_count(frame.address);
    CHECK(mock::page(fixture.resources.native_space, mapped.value())->flags == (hw::page_read | hw::page_write));
    CHECK(native::native_mprotect(fixture.resources, mapped.value(), hw::page_size, posix::prot_NONE));
    CHECK(!mock::page(fixture.resources.native_space, mapped.value()));
    CHECK(native::native_mprotect(fixture.resources, mapped.value(), hw::page_size, posix::prot_WRITE));
    CHECK(mock::page(fixture.resources.native_space, mapped.value())->physical_address == frame.address);
    CHECK(mock::page(fixture.resources.native_space, mapped.value())->flags == (hw::page_read | hw::page_write));
    const auto original = fixture.resources.native_space;
    CHECK(!native::native_mprotect(fixture.resources, mapped.value(), hw::page_size, hw::page_execute));
    CHECK(native::same_space(fixture.resources.native_space, original));
    CHECK(native::shared_files.info(file).value().aliases == aliases);
    CHECK(native::page_pool.reference_count(frame.address) == references);
    CHECK(native::shared_files.release_description(file));
    native::NativePagePoolFaultProbe::exact_release(native::page_pool, frame, 1);
    CHECK(native::native_munmap(fixture.resources, mapped.value(), hw::page_size));
    CHECK(!native::runtime_failure);
    auto *saved = retained_range();
    CHECK(saved);
    check_range_owner_states(*saved, 1, 0, 2);
    CHECK(native::shared_files.info(file).value().aliases == aliases);
    CHECK(!native::shared_files.info(file).value().description);
    CHECK(native::page_pool.reference_count(frame.address) == references);
    CHECK(native::retry_quarantined_retirement());
    CHECK(!saved->used && !native::page_pool.valid(frame));
    CHECK(native::shared_files.ledger().files == 0);
    native::NativePagePoolFaultProbe::exact_release(native::page_pool, {}, 0);
    native::runtime_failure = {};
}

int main(int argc, char **)
{
    if (argc > 1)
    {
        // A host-owned RAM array models already-qualified backing. Firmware
        // discovery and architecture mappings are covered separately in QEMU.
        alignas(4096) static std::array<std::byte, native::page_count * hw::page_size> backing;
        const memory::MemoryRegion region{reinterpret_cast<uptr>(backing.data()),backing.size(),memory::RegionType::usable};
        CHECK(native::native_frames.initialize({&region,1},hw::page_size));
        CHECK(native::page_pool.activate(native::native_frames,native::NativePagePool::Source::qualified_firmware,native::kernel_reserve_pages));
    }
    check_complete_runtime_purpose_budget();
    check_over_quota_mapping_publication();
    check_exact_alias_publication_above_data_page_quota();
    check_image_owner_move_and_destructor();
    check_exec_failed_old_retirement_skips_install();
    check_exec_failed_install_retains_replacement();
    check_exec_failed_old_shared_aliases_retire_once();
    check_exec_both_failed_tables_keep_distinct_tokens();
    check_image_partial_pin_failure_keeps_pending_owners();
    check_image_failed_owner_release_retries_once();
    check_image_retirement_capacity_preflight();
    check_shared_pressure_with_live_image_footprint();
    check_quarantine_retains_shared_and_staged_frames();
    check_repeated_teardown_discharges_shared_alias_once();
    check_write_seal_requires_retired_table_pins();
    check_write_seal_mapping_refusals_are_read_only();
    check_alias_discharge_is_consumed_before_failed_base_retirement();
    check_partial_pin_retirement_preserves_failed_token();
    check_incomplete_quarantine_keeps_all_owner_edges();
    check_missing_and_stale_tokens_refuse_without_mutation();
    check_range_rollback_retains_unconsumed_owners();
    check_fixed_replacement_retains_only_failed_removed_owner();
    check_munmap_retains_only_failed_removed_owner();
    check_range_retirement_capacity_preflight_is_read_only();
    check_shared_file_write_request_uses_existing_rw_ptes();
    check_noop_unmap_with_exhausted_metadata_quota();
    check_munmap_failure_is_atomic_at_metadata_quota();
    review_fixed_full_process_capacity();
    review_fixed_pool_failure_rollback();
    review_brk_prot_none_collision();
    check_private_file_mapping();
    check_short_private_file_mapping();
    check_map_protect_unmap();
    check_validation_and_capacity();
    check_transaction_rollback();
    check_backend_and_metadata_limits();
    check_pool_failure_rolls_back_partial_allocation();
    check_fixed_replacement();
    check_program_break();
    std::puts("PASS: native VM transaction tests (hosted backend model)");
}
