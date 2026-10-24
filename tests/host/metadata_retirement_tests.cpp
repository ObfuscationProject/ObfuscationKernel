#include "ok/fs/metadata_buffers.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ok;
using namespace ok::block;
using namespace ok::fs;

namespace
{
usize checks{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "cache retirement:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

class Disk final : public driver::BlockDevice
{
  public:
    static constexpr u64 origin = 7;
    std::vector<std::byte> bytes = std::vector<std::byte>(64 * 512);
    usize reads{}, writes{}, flushes{};
    bool check_inflight{};
    Status inflight_status{};
    void *cache{};
    Disk()
    {
        for (usize index = 0; index < bytes.size(); ++index)
            bytes[index] = std::byte(u8(index * 7 + index / 512));
    }
    driver::BlockGeometry geometry() const override { return {origin + 64, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads;
        if (check_inflight)
        {
            auto *owner = static_cast<MetadataBuffers<3, 1024, 2> *>(cache);
            const std::array<u64, 1> released{1};
            const auto attempted = owner->retire_clean_blocks(released);
            inflight_status = attempted ? Status::fault("inflight retirement accepted") : attempted.status();
        }
        if (first < origin || first >= origin + 64 || out.size() > bytes.size() - (first - origin) * 512)
            return Status::io_error("test transport extent");
        for (usize index = 0; index < out.size(); ++index)
            out[index] = bytes[(first - origin) * 512 + index];
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("retirement must not write"); }
    Status flush() override
    { ++flushes; return Status::denied("retirement must not flush"); }
};

template <usize Capacity>
struct Harness
{
    using Cache = MetadataBuffers<Capacity, 1024, 2>;
    Disk disk;
    typename Cache::Registry registry;
    Cache cache;
    typename Cache::Registry::Registration registration{};
    explicit Harness(SessionAccess access = SessionAccess::writable)
    {
        const auto key = registry.register_device(disk, 47); CHECK(key);
        registration = key.value();
        CHECK(cache.open(registry, registration, {Disk::origin, 64}, access));
        if constexpr (Capacity == 3) disk.cache = &cache;
    }
    void close()
    {
        CHECK(cache.close());
        CHECK(disk.writes == 0 && disk.flushes == 0);
    }
};

template <usize Capacity>
usize slot_for(typename Harness<Capacity>::Cache &cache, u64 block)
{
    for (usize index = 0; index < Capacity; ++index)
        if (cache.entries_[index].report.state == MetadataBufferState::ready &&
            cache.entries_[index].report.block == block) return index;
    return Capacity;
}

void clean_retirement()
{
    Harness<3> test;
    using Pin = Harness<3>::Cache::Pin;
    Pin first, victim, third, reloaded;
    CHECK(test.cache.pin(1, first) && test.cache.pin(2, victim) && test.cache.pin(3, third));
    const usize victim_slot = slot_for<3>(test.cache, 2);
    CHECK(victim_slot < 3);
    const u64 old_generation = test.cache.entries_[victim_slot].report.generation;
    const u64 session = test.cache.report().session_serial;
    CHECK(first.release() && victim.release() && third.release());
    const std::array<u64, 2> release{2, 4};
    const auto retired = test.cache.retire_clean_blocks(release);
    CHECK(retired && retired.value() == 1);
    CHECK(test.cache.report().resident_blocks == 2 && test.cache.report().session_serial == session);
    CHECK(test.cache.entries_[victim_slot].report.state == MetadataBufferState::unused);
    CHECK(test.cache.entries_[victim_slot].report.generation == old_generation + 1);
    CHECK(test.cache.entries_[victim_slot].canonical == std::array<std::byte, 1024>{});
    const usize reads = test.disk.reads;
    Pin still_first, still_third;
    CHECK(test.cache.pin(1, still_first) && test.cache.pin(3, still_third));
    CHECK(test.disk.reads == reads);
    // Model later physical-block reuse only after retirement; a fresh pin must
    // fetch the new whole image instead of exposing the old canonical bytes.
    std::fill_n(test.disk.bytes.begin() + 2 * 1024, 1024, std::byte{0xa5});
    CHECK(test.cache.pin(2, reloaded) && test.disk.reads == reads + 1);
    CHECK(reloaded.report().value().generation == old_generation + 2);
    std::array<std::byte, 1024> reused{};
    CHECK(reloaded.read(0, reused) &&
          std::all_of(reused.begin(), reused.end(), [](std::byte value) { return value == std::byte{0xa5}; }));
    CHECK(still_first.release() && still_third.release() && reloaded.release());
    test.close();
}

void preflight_and_busy()
{
    Harness<3> test;
    using Pin = Harness<3>::Cache::Pin;
    Pin first, second, loading;
    CHECK(test.cache.pin(1, first) && test.cache.pin(2, second));
    CHECK(first.release() && second.release());
    const auto original = test.cache.entries_;
    const usize reads = test.disk.reads;
    const std::array<u64, 2> duplicate{1, 1}, out_of_bounds{1, 32};
    std::array<u64, 17> oversized{};
    for (usize index = 0; index < oversized.size(); ++index) oversized[index] = index;
    CHECK(test.cache.retire_clean_blocks(duplicate).status().code() == StatusCode::invalid_argument);
    CHECK(test.cache.retire_clean_blocks(out_of_bounds).status().code() == StatusCode::invalid_argument);
    CHECK(test.cache.retire_clean_blocks(oversized).status().code() == StatusCode::unsupported);
    CHECK(test.cache.report().resident_blocks == 2 && test.disk.reads == reads);
    for (usize index = 0; index < 3; ++index)
        CHECK(test.cache.entries_[index].report.generation == original[index].report.generation &&
              test.cache.entries_[index].report.state == original[index].report.state);
    const usize first_slot = slot_for<3>(test.cache, 1), second_slot = slot_for<3>(test.cache, 2);
    test.cache.entries_[second_slot].report.block = 1;
    const std::array<u64, 1> duplicate_resident{1};
    CHECK(test.cache.retire_clean_blocks(duplicate_resident).status().code() == StatusCode::fault);
    CHECK(test.cache.report().resident_blocks == 2 &&
          test.cache.entries_[first_slot].report.state == MetadataBufferState::ready &&
          test.cache.entries_[second_slot].report.state == MetadataBufferState::ready);
    test.cache.entries_[second_slot].report.block = 2;
    CHECK(test.cache.pin(2, second).code() == StatusCode::already_exists);
    Pin active;
    CHECK(test.cache.pin(2, active));
    const std::array<u64, 1> release_first{1};
    CHECK(test.cache.retire_clean_blocks(release_first).status().code() == StatusCode::busy);
    CHECK(slot_for<3>(test.cache, 1) < 3 && active.release());
    auto &entry = test.cache.entries_[slot_for<3>(test.cache, 2)];
    entry.report.staged_dirty = true;
    const std::array<u64, 2> release_both{1, 2};
    CHECK(test.cache.retire_clean_blocks(release_both).status().code() == StatusCode::busy);
    CHECK(slot_for<3>(test.cache, 1) < 3 && slot_for<3>(test.cache, 2) < 3);
    entry.report.staged_dirty = false;
    entry.report.transaction_serial = 7;
    CHECK(test.cache.retire_clean_blocks(release_both).status().code() == StatusCode::busy);
    CHECK(slot_for<3>(test.cache, 1) < 3 && slot_for<3>(test.cache, 2) < 3);
    entry.report.transaction_serial = 0;
    typename Harness<3>::Cache::Transaction transaction;
    Pin participant;
    CHECK(test.cache.pin(1, participant));
    std::array<Pin *, 1> pins{&participant};
    CHECK(test.cache.begin(pins, transaction));
    CHECK(participant.release());
    CHECK(test.cache.retire_clean_blocks(release_both).status().code() == StatusCode::busy);
    CHECK(transaction.discard());
    Pin reserve_first, reserve_second;
    CHECK(test.cache.pin(1, reserve_first) && test.cache.pin(2, reserve_second));
    test.disk.check_inflight = true;
    CHECK(test.cache.pin(3, loading));
    CHECK(test.disk.inflight_status.code() == StatusCode::busy);
    CHECK(loading.release() && reserve_first.release() && reserve_second.release());
    const auto retired = test.cache.retire_clean_blocks(release_both);
    CHECK(retired && retired.value() == 2 && test.cache.report().resident_blocks == 1);
    test.close();
}

void exhausted_and_readonly()
{
    Harness<1> test;
    using Pin = Harness<1>::Cache::Pin;
    Pin victim, retry;
    CHECK(test.cache.pin(1, victim) && victim.release());
    CHECK(slot_for<1>(test.cache, 1) == 0);
    test.cache.entries_[0].report.generation = ~u64{0};
    const std::array<u64, 1> release{1};
    const auto retired = test.cache.retire_clean_blocks(release);
    CHECK(retired && retired.value() == 1);
    CHECK(test.cache.entries_[0].report.state == MetadataBufferState::exhausted &&
          test.cache.entries_[0].report.generation == ~u64{0} &&
          test.cache.report().resident_blocks == 0);
    const usize reads = test.disk.reads;
    CHECK(test.cache.pin(1, retry).code() == StatusCode::no_memory && test.disk.reads == reads);
    test.close();

    Harness<1> readonly(SessionAccess::readonly);
    const auto rejected = readonly.cache.retire_clean_blocks(release);
    CHECK(!rejected && rejected.status().code() == StatusCode::denied);
    readonly.close();
}
} // namespace

int main()
{
    clean_retirement();
    preflight_and_busy();
    exhausted_and_readonly();
    std::printf("PASS private metadata retirement: checks=%zu atomic_preflight=1 generation_exhaustion=1 inflight=1 no_write_flush=1\n", checks);
}
