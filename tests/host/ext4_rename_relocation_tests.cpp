#include "../fixtures/ext4_rename_relocation_fixture.hpp"
#include <cstdio>
#include <cstdlib>
extern "C" unsigned int ext2fs_crc32c_le(unsigned int, const unsigned char *, size_t);

using namespace ok;
using namespace ok::testing::rename_fixture;
using namespace ok::fs::ext4_checked;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "rename relocation:%d: %s\n", __LINE__, #x); std::abort(); } } while (false)
namespace
{
usize checks{};
void verify(const Fixture &fixture, const fs::Ext4DirectoryRenamePlan &plan, usize relocated)
{
    CHECK(plan.before == fixture.image && plan.directory_inode == 2 && plan.source_inode == 12 &&
          plan.directory_block == 20 && plan.entry_offset == source_offset);
    CHECK(find(plan.after, "notes.txt") == tail && find(plan.after, "saved-note.txt") == relocated);
    CHECK(le32(plan.after, relocated) == 12 && plan.after[relocated + 7] == 1);
    CHECK(le32(plan.after, source_offset) == 0 && le16(plan.after, source_offset + 4) == 20);
    for (usize i = 0; i < 20; ++i)
        if (i != 4 && i != 5) CHECK(plan.after[source_offset + i] == 0);
    const auto seed = ext2fs_crc32c_le(~u32{0}, fixture.binding.uuid.data(), fixture.binding.uuid.size());
    const std::array<u8, 4> ino{2, 0, 0, 0};
    auto crc = ext2fs_crc32c_le(seed, ino.data(), ino.size());
    crc = ext2fs_crc32c_le(crc, fixture.directory.data() + 100, 4);
    crc = ext2fs_crc32c_le(crc, plan.after.data(), tail);
    CHECK(le32(plan.after, 1020) == crc);
    CHECK(find(plan.after, ".") == 0 && find(plan.after, "..") == 12);
    ++checks;
}
void reject(const Fixture &fixture, StatusCode expected)
{
    const auto input = fixture.image; const auto result = fixture.plan();
    CHECK(!result && result.status().code() == expected && fixture.image == input); ++checks;
}
}
int main()
{
    Fixture actual;
    auto free = actual.plan(); CHECK(free); verify(actual, free.value(), 44);
    CHECK(le16(free.value().after, 48) == 968);
    for (usize i = 44 + 8 + 14; i < tail; ++i) CHECK(free.value().after[i] == 0);
    auto fit = actual.plan("saved.txt"); CHECK(fit);
    CHECK(find(fit.value().after, "saved.txt") == source_offset && fit.value().before == actual.image);
    CHECK(le16(fit.value().after, source_offset + 4) == 20 && le32(fit.value().after, source_offset) == 12);
    for (usize i = 44; i < tail; ++i) CHECK(fit.value().after[i] == actual.image[i]);
    ++checks;

    Fixture exact;
    entry(exact.image, 44, 24, 0, 0, ""); entry(exact.image, 68, 944, 13, 1, "keeper"); exact.seal();
    auto exact_plan = exact.plan(); CHECK(exact_plan); verify(exact, exact_plan.value(), 44);
    CHECK(le16(exact_plan.value().after, 48) == 24 && find(exact_plan.value().after, "keeper") == 68);
    for (usize i = 68; i < tail; ++i) CHECK(exact_plan.value().after[i] == exact.image[i]);

    Fixture slack;
    entry(slack.image, 44, 968, 13, 1, "keeper"); slack.image[58] = 0xa5; slack.image[59] = 0x5a; slack.seal();
    auto split = slack.plan(); CHECK(split); verify(slack, split.value(), 60);
    CHECK(le16(split.value().after, 48) == 16 && le16(split.value().after, 64) == 952);
    CHECK(find(split.value().after, "keeper") == 44);
    for (usize i = 44; i < 60; ++i)
        if (i != 48 && i != 49) CHECK(split.value().after[i] == slack.image[i]);
    for (usize i = 60 + 8 + 14; i < tail; ++i) CHECK(split.value().after[i] == 0);

    Fixture nonadjacent;
    entry(nonadjacent.image, 44, 16, 13, 1, "keep"); entry(nonadjacent.image, 60, 952, 0, 0, ""); nonadjacent.seal();
    auto distant = nonadjacent.plan(); CHECK(distant); verify(nonadjacent, distant.value(), 60);
    for (usize i = 44; i < 60; ++i) CHECK(distant.value().after[i] == nonadjacent.image[i]);

    Fixture before;
    entry(before.image, 0, 12, 2, 2, "."); entry(before.image, 12, 12, 2, 2, "..");
    entry(before.image, 24, 20, 12, 1, "notes.txt");
    entry(before.image, 44, 40, 13, 1, "keeper"); entry(before.image, 84, 928, 0, 0, ""); before.seal();
    auto preferred = before.plan(); CHECK(preferred); verify(before, preferred.value(), 84);
    CHECK(le16(preferred.value().after, 48) == 40);

    Fixture full;
    entry(full.image, 44, 20, 0, 0, "");
    for (usize offset = 64, index = 0; offset < tail; offset += 12, ++index)
    {
        const char name[4]{'a', char('0' + index / 10), char('0' + index % 10), 0};
        entry(full.image, offset, 12, u32(13 + index), 1, name);
    }
    full.seal(); reject(full, StatusCode::no_space);
    Fixture full_late = full;
    put16(full_late.image, 1000 + 4, 3); full_late.seal(); reject(full_late, StatusCode::io_error);

    Fixture late;
    entry(late.image, 44, 24, 0, 0, ""); entry(late.image, 68, 944, 13, 1, "keeper");
    put16(late.image, 68 + 4, 3); late.seal(); reject(late, StatusCode::io_error);
    CHECK(!late.plan("saved.txt") && late.plan("saved.txt").status().code() == StatusCode::io_error); ++checks;
    Fixture late_slack;
    entry(late_slack.image, 44, 40, 13, 1, "keeper"); entry(late_slack.image, 84, 928, 14, 1, "last");
    put16(late_slack.image, 84 + 4, 3); late_slack.seal(); reject(late_slack, StatusCode::io_error);
    Fixture destination;
    entry(destination.image, 44, 24, 0, 0, "");
    entry(destination.image, 68, 944, 13, 1, "saved-note.txt"); destination.seal(); reject(destination, StatusCode::already_exists);
    Fixture duplicate;
    entry(duplicate.image, 44, 24, 0, 0, ""); entry(duplicate.image, 68, 944, 12, 1, "notes.txt");
    duplicate.seal(); reject(duplicate, StatusCode::io_error);
    Fixture alias;
    entry(alias.image, 44, 24, 0, 0, ""); entry(alias.image, 68, 944, 12, 1, "alias");
    alias.seal(); reject(alias, StatusCode::io_error);
    Fixture bad_name;
    entry(bad_name.image, 44, 24, 0, 0, ""); entry(bad_name.image, 68, 944, 13, 1, "bad/name");
    bad_name.seal(); reject(bad_name, StatusCode::io_error);
    Fixture bad_type;
    entry(bad_type.image, 44, 24, 0, 0, ""); entry(bad_type.image, 68, 944, 13, 8, "keeper");
    bad_type.seal(); reject(bad_type, StatusCode::io_error);
    Fixture bad_tail; bad_tail.image[1020] ^= 1; reject(bad_tail, StatusCode::io_error);
    Fixture bad_inode; bad_inode.source[124] ^= 1; reject(bad_inode, StatusCode::io_error);
    Fixture bad_layout; bad_layout.binding.layout.block_size = 4096; reject(bad_layout, StatusCode::unsupported);
    CHECK(!actual.plan("bad/name") && actual.plan("bad/name").status().code() == StatusCode::invalid_argument); ++checks;
    CHECK(!actual.plan(std::string_view{"12345678901234567890123456789012"}) &&
          actual.plan("12345678901234567890123456789012").status().code() == StatusCode::invalid_argument); ++checks;
    std::printf("PASS hosted bounded directory relocation: checks=%zu free=4 slack=1 fit=1 before_image=unchanged checksum=libext2fs scope=model\n", checks);
}
