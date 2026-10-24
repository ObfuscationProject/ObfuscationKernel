#include "ext4_owned_support.hpp"
#include <memory>

using namespace owned_fixture;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL create reuse:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

namespace
{
using Owner = Ext4OwnedTransaction<1, 8>;
constexpr Ext4CreateUpdate update{2, 1700000333, 123456789, "reused-note",
    {NodeType::regular, 0640, 1000, 1001}};
usize mixtures{}, barrier_rejects{}, hostile_rejects{}, failures{}, marker_rejects{};
struct Publication
{
    Bytes image, completed;
    usize journal{}, count{}, writes{}, flushes{}, checkpoint_writes{}, checkpoint_flushes{};
    std::array<u32, 6> targets{};
};
void explain(Status status)
{
    if (!status) std::fprintf(stderr, "status %u: %.*s\n", u32(status.code()),
        int(status.message().size()), status.message().data());
}
void check_identity(const Bytes &image, u32 generation)
{
    Disk disk(image);
    auto preview = std::make_unique<Ext4ReplayPreview<6, 2>>(false, true, Ext4ReplayPolicy::directory_create);
    const auto status = preview->open(disk, {0, image.size() / 512}); explain(status); CHECK(status);
    const auto found = preview->lookup(2, update.name); CHECK(found && found.value() == 12);
    const auto inode = preview->inode(12); CHECK(inode);
    CHECK(inode.value().generation == generation && inode.value().links == 1 && inode.value().size == 0);
    CHECK(inode.value().mode == 0100640 && inode.value().uid == 1000 && inode.value().gid == 1001);
    CHECK(r32(image, inode_offset(image, 12) + 160) == 0);
    CHECK(!disk.writes && !disk.flushes);
}
Publication publish(const Bytes &input, u32 generation, const std::filesystem::path &out)
{
    Disk disk(input); disk.origin = 37;
    Owner::Registry registry; const auto key = registry.register_device(disk, 49); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {disk.origin, input.size() / 512}));
    auto status = owner->prepare_create(update); explain(status); CHECK(status);
    CHECK(owner->created_inode() == 12 && !disk.writes && !disk.flushes);
    CHECK(owner->commit());
    Publication result{}; result.image = disk.durable; result.journal = journal(result.image);
    result.writes = disk.writes; result.flushes = disk.flushes;
    for (usize i = 0; i < 6; ++i)
    {
        const usize at = result.journal + 1024 + (i ? 44 + (i - 1) * 16 : 12);
        result.targets[i] = be32(result.image, at);
        if (be32(result.image, at + 4) & jbd2::flag_last_tag) { result.count = i + 1; break; }
    }
    CHECK(result.count == 6 && result.writes == 10 && result.flushes == 4);
    CHECK(owner->checkpoint());
    result.checkpoint_writes = disk.writes - result.writes;
    result.checkpoint_flushes = disk.flushes - result.flushes;
    CHECK(result.checkpoint_writes == 7 && result.checkpoint_flushes == 5);
    CHECK(owner->close()); result.completed = disk.durable;
    check_identity(result.completed, generation);
    if (!out.empty()) { save(out / "published.img", result.image); save(out / "completed.img", result.completed); }
    return result;
}
void apply_home(Bytes &image, const Publication &publication, usize index)
{
    const usize descriptor = publication.journal + 1024;
    const usize tag = descriptor + (index ? 44 + (index - 1) * 16 : 12);
    const usize destination = usize(publication.targets[index]) * 1024;
    std::copy_n(image.begin() + publication.journal + (index + 2) * 1024, 1024,
                image.begin() + destination);
    if (be32(image, tag + 4) & jbd2::flag_escape) big32(image, destination, jbd2::magic);
}
void cold(Bytes image, const Publication &publication, bool accept)
{
    Disk disk(image);
    auto completion = std::make_unique<Ext4RecoveryCompletion<6, 2>>(false, true, Ext4ReplayPolicy::directory_create);
    const auto status = completion->run(disk, {0, image.size() / 512});
    if (accept)
    {
        explain(status); CHECK(status && completion->report().complete());
        CHECK(disk.live == publication.completed && disk.live == disk.durable); ++mixtures;
    }
    else { CHECK(!status && !disk.writes && !disk.flushes && disk.live == image); ++barrier_rejects; }
}
void hostile(Bytes image)
{
    Disk disk(image);
    auto completion = std::make_unique<Ext4RecoveryCompletion<6, 2>>(false, true, Ext4ReplayPolicy::directory_create);
    CHECK(!completion->run(disk, {0, image.size() / 512}));
    CHECK(!disk.writes && !disk.flushes && disk.live == image); ++hostile_rejects;
}
void mixtures_test(const Publication &publication)
{
    for (u32 mask = 0; mask < 64; ++mask)
    {
        Bytes image = publication.image;
        for (usize i = 0; i < 6; ++i) if (mask & (1U << i)) apply_home(image, publication, i);
        const bool accept = (!(mask & 0x1cU) || (mask & 3U) == 3U) &&
                            (!(mask & 0x20U) || (mask & 0x1fU) == 0x1fU);
        cold(std::move(image), publication, accept);
    }
}
void faults(const Bytes &input, const Publication &publication)
{
    for (bool checkpoint : {false, true}) for (bool after : {false, true})
        for (bool writing : {false, true})
    {
        const usize count = checkpoint ? (writing ? publication.checkpoint_writes : publication.checkpoint_flushes) :
                                         (writing ? publication.writes : publication.flushes);
        for (usize step = 1; step <= count; ++step)
        {
            Disk disk(input); Owner::Registry registry;
            const auto key = registry.register_device(disk, 49); CHECK(key);
            auto owner = std::make_unique<Owner>(true);
            CHECK(owner->open(registry, key.value(), {0, input.size() / 512}));
            CHECK(owner->prepare_create(update)); if (checkpoint) CHECK(owner->commit());
            if (writing) (after ? disk.fail_write_after : disk.fail_write) = disk.writes + step;
            else (after ? disk.fail_flush_after : disk.fail_flush) = disk.flushes + step;
            CHECK(!(checkpoint ? owner->checkpoint() : owner->commit()));
            CHECK(owner->report().phase == Ext4OwnedPhase::failed); disk.crash();
            auto completion = std::make_unique<Ext4RecoveryCompletion<6, 2>>(false, true, Ext4ReplayPolicy::directory_create);
            const auto status = completion->run(disk, {0, input.size() / 512}); explain(status); CHECK(status);
            const bool created = checkpoint || (after && ((writing && step == publication.writes) ||
                                                          (!writing && step == publication.flushes)));
            if (created) CHECK(disk.live == publication.completed);
            else
            {
                // Failed prepublication may leave inactive ring debris. No
                // home or empty journal head can depend on those dead records.
                const usize first = publication.journal + 1024;
                const usize end = publication.journal + r32(input, inode_offset(input, 8) + 4);
                CHECK(std::equal(disk.live.begin(), disk.live.begin() + first, input.begin()));
                CHECK(std::equal(disk.live.begin() + end, disk.live.end(), input.begin() + end));
            }
            ++failures;
        }
    }
}
void marker_test(const Bytes &input)
{
    const usize at = inode_offset(input, 12);
    const auto rejected = [&](Bytes image, StatusCode code) {
        Disk disk(image); Owner::Registry registry; const auto key = registry.register_device(disk, 49); CHECK(key);
        auto owner = std::make_unique<Owner>(true); CHECK(owner->open(registry, key.value(), {0, image.size() / 512}));
        const auto status = owner->prepare_create(update);
        CHECK(!status && status.code() == code && !disk.writes && !disk.flushes);
        if (code == StatusCode::io_error)
            CHECK(owner->report().phase == Ext4OwnedPhase::failed && !owner->close());
        else CHECK(owner->report().phase == Ext4OwnedPhase::rejected && owner->close());
        ++marker_rejects;
    };
    Bytes image = input; image[at + 200] ^= 1; rejected(image, StatusCode::io_error);
    image = input; image[at + 200] = 1; inode_crc(image, 12, at); rejected(image, StatusCode::unsupported);
    image = input; w32(image, at + 160, 0); inode_crc(image, 12, at); rejected(image, StatusCode::unsupported);
    image = input; w32(image, at + 100, 0); inode_crc(image, 12, at); rejected(image, StatusCode::unsupported);
    image = input; w32(image, at + 100, ~u32{0}); inode_crc(image, 12, at); rejected(image, StatusCode::overflow);
    image = input; inode_crc(image, 14, at); rejected(image, StatusCode::io_error);
}
void live_foreign(const Bytes &input, const Bytes &foreign)
{
    Disk disk(input); Owner::Registry registry; const auto key = registry.register_device(disk, 49); CHECK(key);
    auto owner = std::make_unique<Owner>(true); CHECK(owner->open(registry, key.value(), {0, input.size() / 512}));
    CHECK(owner->prepare_create(update) && owner->commit());
    // The foreign historical parent yields identical head and all after homes.
    const usize at = journal(disk.live) + 1024;
    std::copy_n(foreign.begin() + at, 1024, disk.live.begin() + at);
    const usize writes = disk.writes, flushes = disk.flushes;
    CHECK(!owner->checkpoint() && owner->report().phase == Ext4OwnedPhase::failed);
    CHECK(disk.writes == writes && disk.flushes == flushes); ++hostile_rejects;
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3); std::ifstream stream(argv[1], std::ios::binary); CHECK(stream);
    const Bytes input((std::istreambuf_iterator<char>(stream)), {}); CHECK(input.size() == 32 * 1024 * 1024);
    std::filesystem::create_directory(argv[2]); const std::filesystem::path out(argv[2]);
    CHECK(r32(input, inode_offset(input, 12) + 100) == 1);
    const auto publication = publish(input, 1, out); mixtures_test(publication); faults(input, publication); marker_test(input);
    // Generations consumed by create are exactly those retained by reclaim.
    for (u32 generation : {2U, 17U, ~u32{0} - 1})
    {
        Bytes next = input; const usize at = inode_offset(next, 12);
        w32(next, at + 100, generation); inode_crc(next, 12, at);
        const auto result = publish(next, generation, {}); mixtures_test(result);
    }
    const usize descriptor = publication.journal + 1024;
    for (usize byte : {128U, 136U, 138U, 140U, 141U, 144U, 148U, 152U, 156U, 160U,
                       164U, 168U, 172U, 196U, 200U, 231U, 232U, 233U, 235U, 1019U})
    {
        auto bad = publication.image; bad[descriptor + byte] ^= 1;
        journal_record_crc(bad, descriptor, 1020); hostile(std::move(bad));
    }
    {
        auto bad = publication.image;
        std::fill(bad.begin() + descriptor + 128, bad.begin() + descriptor + 1020, 0);
        journal_record_crc(bad, descriptor, 1020); hostile(std::move(bad));
    }
    // A valid foreign witness can change history without changing final homes;
    // cold replay checks its homes, and the live owner checks original publication.
    auto foreign_input = input; const usize parent = inode_offset(input, 2);
    w32(foreign_input, parent + 12, r32(input, parent + 12) + 17); inode_crc(foreign_input, 2, parent);
    const auto foreign = publish(foreign_input, 1, {});
    CHECK(foreign.image != publication.image);
    for (usize index = 0; index < publication.count; ++index)
    {
        const usize at = usize(publication.targets[index]) * 1024;
        CHECK(std::equal(foreign.completed.begin() + at, foreign.completed.begin() + at + 1024,
                         publication.completed.begin() + at));
    }
    CHECK(std::equal(foreign.completed.begin() + publication.journal,
        foreign.completed.begin() + publication.journal + 1024, publication.completed.begin() + publication.journal));
    live_foreign(input, foreign.image);
    std::printf("PASS create reuse: accepted_mixtures=%zu barrier_rejects=%zu hostile_rejects=%zu failures=%zu marker_rejects=%zu generations=4 exact_preimages=1 no_runtime_reclaim=1\n",
        mixtures, barrier_rejects, hostile_rejects, failures, marker_rejects);
}
