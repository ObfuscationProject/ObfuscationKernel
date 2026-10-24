// Hosted record tests; no device, journal transaction or recovery I/O.
#include "ok/fs/ext4_regular_move_witness.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::fs;
namespace
{
using Block = Ext4RegularMoveBlock;
using Codec = Ext4RegularMoveWitnessCodec;
usize checks{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "regular move witness:%d: %s\n", __LINE__, #expression); std::abort(); \
} } while (false)

Ext4RegularMoveUpdate update()
{ return {2, 11, 47, 7, 8, 9, "document.txt", "renamed.txt", 1700000111, 123456789}; }

Ext4RegularMovePlan record_images(usize count)
{
    const auto event = update();
    Ext4RegularMovePlan result{};
    result.count = count; result.from_parent = event.from_parent; result.to_parent = event.to_parent;
    result.source = event.source; result.source_generation = event.source_generation;
    result.old_name_length = event.old_name.size(); result.new_name_length = event.new_name.size();
    for (usize at = 0; at < event.old_name.size(); ++at) result.old_name[at] = event.old_name[at];
    for (usize at = 0; at < event.new_name.size(); ++at) result.new_name[at] = event.new_name[at];
    result.allocation_unchanged = result.file_identity_retained = result.source_link_count_unchanged = true;
    for (usize home = 0; home < count; ++home)
    {
        result.targets[home] = u32(600 + home);
        for (usize at = 0; at < 1024; ++at) result.before[home][at] = u8((at * 17 + home * 31) % 251);
        result.after[home] = result.before[home];
        for (usize at : {usize{12}, usize{13}, usize{16}, usize{124}, usize{125}, usize{900}})
            result.after[home][at] ^= 0x55;
    }
    return result;
}

Block descriptor(const Ext4RegularMovePlan &plan)
{
    Block result{}; result.fill(0xa5);
    CHECK(Codec::encode(update(), plan, result));
    const usize tag_end = 44 + (plan.count - 1) * 16;
    for (usize at = 0; at < tag_end; ++at) CHECK(result[at] == 0xa5);
    for (usize at = 1020; at < 1024; ++at) CHECK(result[at] == 0xa5);
    return result;
}

void roundtrip(usize count)
{
    const auto plan = record_images(count); const auto encoded = descriptor(plan);
    std::array<Block, 5> before{}; Ext4RegularMoveWitness decoded{};
    CHECK(Codec::present(encoded));
    CHECK(Codec::decode(encoded, std::span{plan.after}.first(count), std::span{before}.first(count), decoded));
    CHECK(decoded.count == count && decoded.targets == plan.targets);
    CHECK(decoded.update.from_parent == 2 && decoded.update.to_parent == 11 && decoded.update.source == 47);
    CHECK(decoded.update.from_generation == 7 && decoded.update.to_generation == 8 && decoded.update.source_generation == 9);
    CHECK(decoded.update.old_name == "document.txt" && decoded.update.new_name == "renamed.txt");
    CHECK(decoded.update.seconds == update().seconds && decoded.update.nanoseconds == update().nanoseconds);
    for (usize home = 0; home < count; ++home) CHECK(before[home] == plan.before[home]);
    Block reencoded = encoded;
    CHECK(Codec::encode(decoded.update, plan, reencoded)); CHECK(reencoded == encoded);

    // Decode checks the full reconstructed block, including bytes with no patch.
    auto changed_after = plan.after; changed_after[0][777] ^= 1;
    CHECK(Codec::decode(encoded, std::span{changed_after}.first(count), std::span{before}.first(count), decoded).code() == StatusCode::io_error);
    CHECK(decoded.count == 0);
}

void corruptions()
{
    const auto plan = record_images(5); const auto encoded = descriptor(plan);
    std::array<Block, 5> before{};
    for (usize at : {usize{143}, usize{196}, usize{220}, usize{271}, usize{303}, usize{306}, usize{319}, usize{1019}})
    {
        Block malformed = encoded; malformed[at] ^= 1; Ext4RegularMoveWitness result{}; result.count = 5;
        CHECK(!Codec::decode(malformed, plan.after, before, result)); CHECK(result.count == 0);
    }
    for (usize at : {usize{136}, usize{140}, usize{141}, usize{142}, usize{144}, usize{148}, usize{152}, usize{175}})
    {
        Block malformed = encoded; malformed[at] = at == 175 ? 0xff : 0; Ext4RegularMoveWitness result{};
        CHECK(!Codec::decode(malformed, plan.after, before, result));
    }
    Block absent = encoded; absent[128] ^= 1; Ext4RegularMoveWitness result{};
    CHECK(Codec::decode(absent, plan.after, before, result).code() == StatusCode::unsupported);
    Block duplicate = encoded;
    for (usize byte = 0; byte < 4; ++byte) duplicate[204 + byte] = duplicate[200 + byte];
    CHECK(Codec::decode(duplicate, plan.after, before, result).code() == StatusCode::io_error);
    for (unsigned kind = 0; kind < 4; ++kind)
    {
        Block malformed = encoded;
        if (kind == 0) malformed[Codec::patch_begin] = 5;
        if (kind == 1) malformed[Codec::patch_begin + 3] = malformed[Codec::patch_begin + 4] = 0;
        if (kind == 2) malformed[Codec::patch_begin + 5] = plan.after[0][12];
        if (kind == 3) malformed[Codec::patch_begin + 5] ^= 1;
        CHECK(Codec::decode(malformed, plan.after, before, result).code() == StatusCode::io_error);
    }
    Block illegal_name = encoded; illegal_name[240] = '/';
    CHECK(Codec::decode(illegal_name, plan.after, before, result).code() == StatusCode::invalid_argument);
}

void refusals_and_names()
{
    auto plan = record_images(3); Block destination{}; destination.fill(0x71); const auto saved = destination;
    auto wrong = update(); ++wrong.source;
    CHECK(Codec::encode(wrong, plan, destination).code() == StatusCode::invalid_argument && destination == saved);
    plan.targets[2] = plan.targets[1];
    CHECK(Codec::encode(update(), plan, destination).code() == StatusCode::invalid_argument && destination == saved);
    plan = record_images(3); plan.after[0].fill(0xff);
    CHECK(Codec::encode(update(), plan, destination).code() == StatusCode::unsupported && destination == saved);
    plan = record_images(3); const auto old_plan = plan;
    CHECK(Codec::encode(update(), plan, plan.before[0]).code() == StatusCode::invalid_argument && plan.before == old_plan.before);
    const auto encoded = descriptor(plan); auto alias = plan.after; const auto old_alias = alias;
    Ext4RegularMoveWitness output{}; output.count = 17;
    CHECK(Codec::decode(encoded, std::span{alias}.first(3), std::span{alias}.first(3), output).code() == StatusCode::invalid_argument);
    CHECK(output.count == 17 && alias == old_alias);
    std::array<Block, 3> descriptor_alias{}; descriptor_alias[0] = encoded;
    const auto prior = descriptor_alias;
    CHECK(Codec::decode(descriptor_alias[0], std::span{plan.after}.first(3), descriptor_alias, output).code() == StatusCode::invalid_argument);
    CHECK(descriptor_alias == prior && output.count == 17);

    std::array<char, 12> old_name{'d','o','c','u','m','e','n','t','.','t','x','t'};
    std::array<char, 11> new_name{'r','e','n','a','m','e','d','.','t','x','t'};
    auto named = update(); named.old_name = {old_name.data(), old_name.size()}; named.new_name = {new_name.data(), new_name.size()};
    CHECK(Codec::encode(named, plan, destination)); old_name.fill('x'); new_name.fill('y');
    std::array<Block, 3> before{};
    CHECK(Codec::decode(destination, std::span{plan.after}.first(3), before, output));
    CHECK(output.update.old_name == "document.txt" && output.update.new_name == "renamed.txt");
}
}

int main()
{
    roundtrip(3); roundtrip(4); roundtrip(5); corruptions(); refusals_and_names();
    std::printf("EXT4_REGULAR_MOVE_WITNESS_MODEL_PASS checks=%zu\n", checks);
}
