#include <ok/fs/ext4/dir.hpp>
#include <ok/fs/ext4/mount.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

using namespace ok;
using namespace ok::fs;
namespace
{
using Budget = Ext4DirectoryInitializationBudget;
usize checks{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "directory initialization budget:%u: %s\n", unsigned(__LINE__), #expression); \
    std::exit(1); } } while (false)

// Independent source arithmetic. Production contains only its fixed maxima;
// this oracle reconstructs the nested decoder and the actual stage operations.
struct Oracle
{
    u64 decode{}, no_reference{}, strict{}, preparation{}, initialization{}, checkpoint{}, continuation{}, whole{};
    u64 nonzero_preparation{}, zero_preparation{}, zero_initialization{}, nonzero_whole{}, zero_whole{};
};
constexpr Oracle source_oracle(u64 groups, u64 inodes_per_group, u64 filesystem_blocks)
{
    constexpr u64 allocated = 128, directory_blocks = 1024, callbacks = 84, homes = 8, participants = 9;
    const u64 decode = 2 + 4 * groups;
    const u64 view = 2 + 2 * decode + directory_blocks;
    const u64 unique = decode + 1 + groups + allocated * decode;
    const u64 scan = 1 + groups + allocated * (1 + decode);
    const u64 one_lookup = 2 * decode + 1, generic_lookup = 2 * decode + directory_blocks;
    const u64 directory = 3 * decode + directory_blocks +
        directory_blocks * callbacks * (2 * decode + directory_blocks);
    const u64 inspection = directory + groups + allocated * (decode + directory);
    const u64 binding = 4 + 2 * groups;
    const u64 parser = filesystem_blocks - 1 < 32767 ? filesystem_blocks - 1 : 32767;
    const u64 refusal = binding + 1 + parser + view + homes * (2 + 4 * decode) + inspection + 2;
    const u64 witness = 2 * homes + 1 + scan + 3 * unique + 3 * one_lookup;
    const u64 strict = binding + 1 + parser + 2 * view + witness + inspection + 2;
    const u64 candidate = generic_lookup + unique + 2 * groups + 6 + scan;
    const u64 nonzero_preparation = 1 + refusal + candidate + participants;
    const u64 zero_preparation = nonzero_preparation + strict;
    const u64 preparation = zero_preparation;
    const u64 before_zero = participants + scan + participants;
    const u64 after_zero = 1 + participants + scan;
    const u64 initialization = before_zero + after_zero + strict + participants;
    const u64 zero_initialization = before_zero + participants;
    const u64 checkpoint = homes + 4 + strict + homes;
    const u64 stored = groups + (1 + groups * inodes_per_group) * decode;
    const u64 prefix = decode + generic_lookup;
    const u64 continuation = prefix + participants + refusal + view + stored + 3 * one_lookup + 3 * decode;
    const u64 nonzero_whole = nonzero_preparation + initialization + checkpoint + continuation;
    const u64 zero_whole = zero_preparation + zero_initialization + checkpoint + continuation;
    const u64 whole = nonzero_whole > zero_whole ? nonzero_whole : zero_whole;
    return {decode, scan, strict, preparation, initialization, checkpoint, continuation, whole,
            nonzero_preparation, zero_preparation, zero_initialization, nonzero_whole, zero_whole};
}

block::SessionReport inert_healthy()
{
    block::SessionReport result{};
    result.state = block::DeviceState::owned;
    result.identity = 41; result.registration_generation = 3; result.session_serial = 7;
    return result;
}
using Field = block::SessionCounter block::SessionReport::*;
constexpr std::array<Field, 6> fields{
    &block::SessionReport::reads, &block::SessionReport::completed_reads,
    &block::SessionReport::writes, &block::SessionReport::completed_writes,
    &block::SessionReport::flushes, &block::SessionReport::completed_flushes};

void oracle_and_routes()
{
    static_assert(std::is_same_v<block::SessionCounter, u64>);
    constexpr auto largest = source_oracle(32, 8192, 262145);
    static_assert(largest.decode == 130 && largest.no_reference == 16801);
    CHECK(largest.strict == 14'247'648'674ULL);
    CHECK(largest.nonzero_preparation == 14'247'618'522ULL);
    CHECK(largest.zero_preparation == 28'495'267'196ULL);
    CHECK(largest.zero_initialization == 16'828ULL);
    CHECK(largest.nonzero_whole == 57'024'615'847ULL);
    CHECK(largest.zero_whole == 57'024'599'036ULL);
    CHECK(largest.preparation == Budget::preparation_reads);
    CHECK(largest.initialization == Budget::initialization_reads);
    CHECK(largest.checkpoint == Budget::checkpoint_reads);
    CHECK(largest.continuation == Budget::continuation_reads);
    CHECK(largest.whole == Budget::whole_reads);
    CHECK(largest.whole == 57'024'615'847ULL);
    CHECK(largest.zero_whole <= largest.whole && largest.nonzero_whole <= largest.whole);
    CHECK(largest.preparation + largest.initialization + largest.checkpoint + largest.continuation > largest.whole);
    for (u64 groups = 1; groups <= 32; ++groups)
    {
        const auto row = source_oracle(groups, 8192, 1 + groups * 8192);
        CHECK(row.preparation <= largest.preparation);
        CHECK(row.initialization <= largest.initialization);
        CHECK(row.checkpoint <= largest.checkpoint);
        CHECK(row.continuation <= largest.continuation);
        CHECK(row.whole <= largest.whole);
        CHECK(row.nonzero_preparation <= largest.nonzero_preparation);
        CHECK(row.zero_preparation <= largest.zero_preparation);
        CHECK(row.zero_initialization <= largest.zero_initialization);
        CHECK(row.nonzero_whole <= largest.whole && row.zero_whole <= largest.whole);
        // Directory callback cost remains inside its linear source envelope,
        // including a prefix whose last callback refuses before completion.
        for (u64 blocks = 0; blocks <= 1024; ++blocks)
        {
            const u64 nested = 3 * row.decode + blocks + 84 * blocks * (2 * row.decode + blocks);
            const u64 envelope = 3 * row.decode + (1 + 168 * row.decode + 84 * 1024) * blocks;
            CHECK(nested <= envelope);
        }
    }
    const auto cold = Ext4MountedCommonOpeningBudget::route(Ext4MountedCommonFamily::directory_create);
    CHECK(cold);
    CHECK(cold.value().reads == 23'524'132'523ULL);
    CHECK(cold.value().writes == 9 && cold.value().flushes == 5);
    CHECK(cold.value().sequence_delta == 2 && cold.value().locator_reads == 5);
    for (u64 homes : {u64{7}, u64{8}})
    {
        const u64 publication = 1 + homes + 1 + 1 + 1;
        const u64 checkpoint = homes + 1;
        CHECK(publication == homes + 4 && checkpoint == homes + 1);
        CHECK(1 + publication + checkpoint <= Budget::whole_writes);
        CHECK(1 + 4 + 5 == Budget::whole_flushes);
    }
}

void six_field_boundaries()
{
    constexpr u64 limit = ~u64{0};
    constexpr std::array<u64, 6> credits{
        Budget::whole_reads, Budget::whole_reads, Budget::whole_writes,
        Budget::whole_writes, Budget::whole_flushes, Budget::whole_flushes};
    auto edge = inert_healthy();
    for (usize index = 0; index < fields.size(); ++index) edge.*fields[index] = limit - credits[index];
    CHECK(Budget::reserve(edge));
    for (usize index = 0; index < fields.size(); ++index)
    {
        auto exhausted = edge;
        ++(exhausted.*fields[index]);
        const auto refused = Budget::reserve(exhausted);
        CHECK(!refused && refused.code() == StatusCode::overflow);
        auto parity = inert_healthy();
        ++(parity.*fields[index]);
        const auto mismatch = Budget::reserve(parity);
        CHECK(!mismatch && mismatch.code() == StatusCode::fault);
    }
    const auto preserved = edge;
    CHECK(Budget::reserve(edge));
    for (usize index = 0; index < fields.size(); ++index) CHECK(edge.*fields[index] == preserved.*fields[index]);
}

template <block::SessionCounterPolicy Policy> void finite_policy_controls()
{
    constexpr u64 limit = block::session_counter_limit<Policy>;
    for (u64 credit : {u64{3}, u64{3}, u64{5}, u64{5}, u64{7}, u64{7}})
    {
        CHECK(block::session_counter_fits<Policy>(limit - credit, credit));
        CHECK(!block::session_counter_fits<Policy>(limit - credit + 1, credit));
        CHECK(block::session_counter_fits<Policy>(limit, 0));
    }
    const auto initial = inert_healthy();
    const auto result = Budget::reserve<Policy>(initial);
    CHECK(bool(result) == (Budget::whole_reads <= limit));
    if constexpr (sizeof(Policy) < sizeof(u64))
        CHECK(!result && result.code() == StatusCode::overflow);
}

void unhealthy_controls()
{
    CHECK(Budget::reserve(inert_healthy()));
    for (u32 field = 0; field < 7; ++field)
    {
        auto value = inert_healthy();
        switch (field)
        {
        case 0: value.state = block::DeviceState::quarantined; break;
        case 1: value.first_error = StatusCode::io_error; break;
        case 2: value.identity = 0; break;
        case 3: value.registration_generation = 0; break;
        case 4: value.session_serial = 0; break;
        case 5: value.in_flight = true; break;
        case 6: value.pending_writes = true; break;
        }
        const auto status = Budget::reserve(value);
        CHECK(!status && status.code() == StatusCode::fault);
    }
}
} // namespace

int main()
{
    oracle_and_routes(); six_field_boundaries(); unhealthy_controls();
    finite_policy_controls<u8>(); finite_policy_controls<u16>();
    finite_policy_controls<u32>(); finite_policy_controls<u64>();
    std::printf("EXT4_DIRECTORY_INITIALIZATION_BUDGET_ORACLE_MODEL_PASS checks=%zu source_only=1 transport_execution=0 native_acceptance=0\n", checks);
    return 0;
}
