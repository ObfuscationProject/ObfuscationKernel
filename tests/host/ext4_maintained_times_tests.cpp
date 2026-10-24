// Hosted provider model. This does not execute a kernel, syscall or device driver.
#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4_fs.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace ok;
namespace clock_model = ok::testing::ext4_clock;
using Bytes = std::vector<std::byte>;
using Times = std::array<time::EpochTime, 3>; // Access, modification, change.
static std::string context;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s line %d: %s\n", context.c_str(), __LINE__, #condition); std::exit(1); } } while (false)
constexpr fs::Ext4MountOptions maintained{true, fs::Ext4TimestampPolicy::maintained};
constexpr unsigned A = 1, M = 2, C = 4;
u16 r16(const Bytes &b, usize n) { return std::to_integer<u16>(b.at(n)) | (std::to_integer<u16>(b.at(n + 1)) << 8); }
u32 r32(const Bytes &b, usize n) { return r16(b, n) | (u32{r16(b, n + 2)} << 16); }
void w16(Bytes &b, usize n, u16 v) { b.at(n) = std::byte(v); b.at(n + 1) = std::byte(v >> 8); }
void w32(Bytes &b, usize n, u32 v) { w16(b, n, static_cast<u16>(v)); w16(b, n + 2, static_cast<u16>(v >> 16)); }
usize block_size(const Bytes &b) { return usize{1024} << r32(b, 1048); }
usize inode_offset(const Bytes &b, u32 ino)
{
    CHECK(ino && ino <= r32(b, 1024));
    const usize group = (ino - 1) / r32(b, 1064), index = (ino - 1) % r32(b, 1064);
    const usize descriptor = (r32(b, 1044) + 1) * block_size(b) + group * 32;
    return r32(b, descriptor + 8) * block_size(b) + index * r16(b, 1112);
}
// Independent signed-interval decoder: do not use the provider's timestamp codec.
Times raw_times(const Bytes &b, u32 ino)
{
    constexpr usize base[]{8, 16, 12}, extra[]{140, 136, 132}, required[]{16, 12, 8};
    const usize at = inode_offset(b, ino), stride = r16(b, 1112);
    const usize extent = stride == 256 ? r16(b, at + 128) : 0;
    CHECK(extent <= stride - 128 && extent % 4 == 0);
    Times result{};
    for (usize i = 0; i < 3; ++i)
    {
        const u32 low = r32(b, at + base[i]);
        i64 sec = low < 0x80000000u ? i64{low} : i64{low} - 4294967296LL;
        u32 nanos = 0;
        if (extent >= required[i])
        {
            const u32 high = r32(b, at + extra[i]);
            sec += i64{high & 3u} * 4294967296LL;
            nanos = high / 4;
        }
        CHECK(nanos < 1000000000u);
        result[i] = {sec, nanos};
    }
    return result;
}
time::EpochTime stored_sample(const Bytes &b, u32 ino, usize field, time::EpochTime sample)
{
    constexpr usize required[]{16, 12, 8};
    if (r16(b, 1112) == 128 || r16(b, inode_offset(b, ino) + 128) < required[field])
        sample.nanoseconds = 0;
    return sample;
}
std::map<std::string, u32> directory(const Bytes &b, u32 ino)
{
    const usize node = inode_offset(b, ino), size = block_size(b);
    CHECK(r16(b, node + 40) == 0xf30a && r16(b, node + 46) == 0);
    std::map<std::string, u32> result;
    for (usize extent = 0; extent < r16(b, node + 42); ++extent)
        for (usize i = 0; i < r16(b, node + 56 + extent * 12); ++i)
        {
            const usize begin = (r32(b, node + 60 + extent * 12) + i) * size;
            for (usize at = begin; at < begin + size;)
            {
                const usize length = r16(b, at + 4), name_size = std::to_integer<u8>(b.at(at + 6));
                CHECK(length >= 8 && length % 4 == 0 && at + length <= begin + size && name_size <= length - 8);
                if (r32(b, at))
                {
                    std::string name(reinterpret_cast<const char *>(b.data() + at + 8), name_size);
                    CHECK(result.emplace(name, r32(b, at)).second);
                }
                at += length;
            }
        }
    return result;
}
std::map<u32, Times> allocated_times(const Bytes &b)
{
    std::map<u32, Times> result;
    for (u32 ino = 2; ino <= r32(b, 1024); ++ino)
    {
        if (ino != 2 && ino < r32(b, 1108)) continue;
        const usize group = (ino - 1) / r32(b, 1064), index = (ino - 1) % r32(b, 1064);
        const usize descriptor = (r32(b, 1044) + 1) * block_size(b) + group * 32;
        const usize bitmap = r32(b, descriptor + 4) * block_size(b);
        if ((std::to_integer<u8>(b.at(bitmap + index / 8)) >> (index % 8)) & 1u)
            result.emplace(ino, raw_times(b, ino));
    }
    return result;
}
Bytes load(const std::filesystem::path &path)
{
    auto *file = std::fopen(path.c_str(), "rb"); CHECK(file);
    CHECK(!std::fseek(file, 0, SEEK_END)); const auto length = std::ftell(file); CHECK(length > 0);
    CHECK(!std::fseek(file, 0, SEEK_SET)); Bytes result(static_cast<usize>(length));
    CHECK(std::fread(result.data(), 1, result.size(), file) == result.size()); CHECK(!std::fclose(file));
    return result;
}
void save(const std::filesystem::path &path, const Bytes &bytes)
{
    auto *file = std::fopen(path.c_str(), "wb"); CHECK(file);
    if (!bytes.empty()) CHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    CHECK(!std::fclose(file));
}
std::span<const std::byte> text(std::string_view value)
{
    return {reinterpret_cast<const std::byte *>(value.data()), value.size()};
}
time::RealtimeSample sample(i64 seconds = 1700000000, u32 nanos = 123456789)
{
    return {{seconds, nanos}, {12345678, 7}, 3, time::RealtimeOrigin::administrator_software};
}
class Device final : public driver::BlockDevice
{
  public:
    explicit Device(const Bytes &image) : bytes(image), durable(image) {}
    driver::BlockGeometry geometry() const override { return {bytes.size() / 512, 512, true}; }
    struct Call { char kind; usize clock_calls; };
    bool fault(char kind)
    {
        trace.push_back({kind, clock_model::calls() - clock_start});
        if (armed && trace.size() == fail_at) { triggered = true; return true; }
        return false;
    }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        CHECK(first * 512 + out.size() <= bytes.size());
        const bool error = fault('r');
        const usize count = !error || effect == 2 ? out.size() : effect == 1 ? out.size() / 2 : 0;
        std::memcpy(out.data(), bytes.data() + first * 512, count);
        return error ? Status::not_found("injected read transport error") : Status::success();
    }
    Status write_blocks(u64 first, std::span<const std::byte> in) override
    {
        CHECK(first * 512 + in.size() <= bytes.size());
        if (require_sample) CHECK(clock_model::calls() == clock_start + 1);
        ++writes;
        const bool error = fault('w');
        const usize count = !error || effect == 2 ? in.size() : effect == 1 ? in.size() / 2 : 0;
        std::memcpy(bytes.data() + first * 512, in.data(), count);
        if (observer) observer();
        return error ? Status::unsupported("injected write transport error") : Status::success();
    }
    Status flush() override
    {
        const bool error = fault('f');
        if (!error || effect == 2) durable = bytes;
        else if (effect == 1) std::copy_n(bytes.begin(), bytes.size() / 2, durable.begin());
        return error ? Status::no_memory("injected flush transport error") : Status::success();
    }
    void arm(usize point = 0, unsigned outcome = 0)
    {
        trace.clear(); fail_at = point; effect = outcome; armed = point != 0; triggered = false;
        clock_start = clock_model::calls();
    }
    Bytes bytes, durable;
    std::vector<Call> trace;
    std::function<void()> observer;
    usize clock_start{}, fail_at{}, writes{};
    unsigned effect{};
    bool armed{}, triggered{}, require_sample{};
};
void mount(fs::Ext4FileSystem &fs, Device &disk)
{
    clock_model::reset(); clock_model::set(sample());
    const auto before = disk.bytes;
    CHECK(fs.mount(disk, maintained)); CHECK(clock_model::calls() == 1);
    CHECK(disk.writes == 0 && disk.bytes == before && fs.root());
}
u32 number(fs::Node &node)
{
    auto value = node.query_metadata(); CHECK(value);
    CHECK(value.value().attributes.has(fs::Attribute::inode));
    return static_cast<u32>(value.value().attributes.inode);
}
void checked_metadata(fs::Node &node, const Bytes &bytes)
{
    const auto calls = clock_model::calls();
    const auto result = node.query_metadata(); CHECK(result);
    const auto &attributes = result.value().attributes;
    CHECK(clock_model::calls() == calls);
    const auto expected = raw_times(bytes, static_cast<u32>(attributes.inode));
    const fs::EpochTimestamp fields[]{attributes.access, attributes.modification, attributes.change};
    const fs::Attribute flags[]{fs::Attribute::access_time, fs::Attribute::modification_time, fs::Attribute::change_time};
    for (usize i = 0; i < 3; ++i)
    {
        CHECK(attributes.has(flags[i])); CHECK(fields[i].source == fs::TimestampSource::maintained_ext4);
        CHECK(fields[i].seconds == expected[i].seconds && fields[i].nanoseconds == expected[i].nanoseconds);
    }
}
struct Change { u32 ino; unsigned mask; };
usize event_count{}, no_event_count{}, negative_count{}, fault_count{}, range_count{}, publication_count{};
void verify_event(const Bytes &before_bytes, const Bytes &after,
                  std::span<const Change> changes, time::EpochTime epoch,
                  std::span<const u32> removed = {})
{
    const auto before = allocated_times(before_bytes);
    const auto current = allocated_times(after);
    for (const auto &[ino, old] : before)
    {
        if (std::find(removed.begin(), removed.end(), ino) != removed.end()) { CHECK(!current.contains(ino)); continue; }
        CHECK(current.contains(ino));
        auto expected = old;
        unsigned mask = 0;
        for (const auto change : changes)
            if (change.ino == ino)
            {
                mask |= change.mask;
                for (usize i = 0; i < 3; ++i)
                    if (change.mask & (1u << i)) expected[i] = stored_sample(after, ino, i, epoch);
            }
        CHECK(current.at(ino) == expected);
        constexpr usize base[]{8, 16, 12}, extra[]{140, 136, 132}, required[]{16, 12, 8};
        const auto at = inode_offset(after, ino), old_at = inode_offset(before_bytes, ino);
        const bool wide = r16(after, 1112) == 256;
        for (usize field = 0; field < 3; ++field)
        {
            if (!(mask & (1u << field))) CHECK(r32(after, at + base[field]) == r32(before_bytes, old_at + base[field]));
            if (wide && (!(mask & (1u << field)) || r16(after, at + 128) < required[field]))
                CHECK(r32(after, at + extra[field]) == r32(before_bytes, old_at + extra[field]));
        }
        if (wide)
        {
            CHECK(r32(after, at + 128) == r32(before_bytes, old_at + 128));
            CHECK(!std::memcmp(after.data() + at + 144, before_bytes.data() + old_at + 144, 112));
        }
    }
    for (const auto &[ino, actual] : current)
        if (!before.contains(ino))
        {
            CHECK(std::any_of(changes.begin(), changes.end(), [ino](auto c) { return c.ino == ino && c.mask == 7; }));
            for (usize i = 0; i < 3; ++i) CHECK(actual[i] == stored_sample(after, ino, i, epoch));
            if (r16(after, 1112) == 256) CHECK(r16(after, inode_offset(after, ino) + 128) == 16);
        }
}
template <typename Operation>
void event(Device &disk, const char *name, std::initializer_list<Change> changes, Operation operation,
           std::initializer_list<u32> removed = {})
{
    context = name;
    const auto before = disk.bytes;
    ++event_count;
    const auto now = sample(1700000000 + static_cast<i64>(event_count), 123456789 + static_cast<u32>(event_count));
    clock_model::set(now); disk.arm(); disk.require_sample = true;
    CHECK(operation()); CHECK(clock_model::calls() == disk.clock_start + 1); disk.require_sample = false;
    verify_event(before, disk.bytes, changes, now.value, removed);
}
template <typename Operation>
void no_event(Device &disk, const char *name, Operation operation, bool storage_unchanged = true)
{
    context = name; const auto before = disk.bytes; const auto calls = clock_model::calls();
    CHECK(operation()); CHECK(clock_model::calls() == calls);
    if (storage_unchanged) CHECK(before == disk.bytes);
    ++no_event_count;
}
fs::Node *create(Device &disk, fs::Node &parent, const char *name, fs::InitialNodeAttributes attributes)
{
    context = std::string("create ") + name;
    const auto before = disk.bytes;
    const auto now = sample(1700000000 + static_cast<i64>(++event_count));
    const auto parent_number = number(parent); clock_model::set(now); disk.arm(); disk.require_sample = true;
    bool visible = false;
    disk.observer = [&] {
        const auto entries = directory(disk.bytes, parent_number);
        const auto entry = entries.find(name);
        if (entry == entries.end()) return;
        visible = true; const auto ino = entry->second; const auto at = inode_offset(disk.bytes, ino);
        CHECK(r16(disk.bytes, at) == fs::mode_for(attributes.type, attributes.mode));
        CHECK((r16(disk.bytes, at + 2) | u32{r16(disk.bytes, at + 120)} << 16) == attributes.uid);
        CHECK((r16(disk.bytes, at + 24) | u32{r16(disk.bytes, at + 122)} << 16) == attributes.gid);
        const auto times = raw_times(disk.bytes, ino);
        for (usize i = 0; i < 3; ++i) CHECK(times[i] == stored_sample(disk.bytes, ino, i, now.value));
        ++publication_count;
    };
    auto result = parent.create_child(name, attributes);
    CHECK(result && result.value() && visible); CHECK(clock_model::calls() == disk.clock_start + 1);
    disk.observer = {}; disk.require_sample = false;
    const Change changes[]{{parent_number, M | C}, {number(*result.value()), A | M | C}};
    verify_event(before, disk.bytes, changes, now.value); checked_metadata(*result.value(), disk.bytes);
    return result.value();
}
void create_link(Device &disk, fs::Node &parent, const char *name, const std::string &target)
{
    context = std::string("symlink ") + name;
    const auto before = disk.bytes;
    const auto now = sample(1700000000 + static_cast<i64>(++event_count));
    const auto parent_number = number(parent); clock_model::set(now); disk.arm(); disk.require_sample = true;
    bool visible = false;
    disk.observer = [&] {
        const auto entries = directory(disk.bytes, parent_number);
        const auto entry = entries.find(name);
        if (entry == entries.end()) return;
        visible = true; const auto ino = entry->second; const auto at = inode_offset(disk.bytes, ino);
        CHECK(r16(disk.bytes, at) == fs::mode_for(fs::NodeType::symlink, 0777));
        CHECK((r16(disk.bytes, at + 2) | u32{r16(disk.bytes, at + 120)} << 16) == 0x12345678u);
        CHECK((r16(disk.bytes, at + 24) | u32{r16(disk.bytes, at + 122)} << 16) == 0x87654321u);
        const auto times = raw_times(disk.bytes, ino);
        for (usize i = 0; i < 3; ++i) CHECK(times[i] == stored_sample(disk.bytes, ino, i, now.value));
        ++publication_count;
    };
    CHECK(parent.create_symlink(name, target, 0x12345678, 0x87654321));
    CHECK(visible && clock_model::calls() == disk.clock_start + 1); disk.observer = {}; disk.require_sample = false;
    auto *node = parent.lookup(name); CHECK(node);
    const Change changes[]{{parent_number, M | C}, {number(*node), A | M | C}};
    verify_event(before, disk.bytes, changes, now.value);
}
void write_oracle(const std::filesystem::path &path, const Bytes &bytes)
{
    auto *file = std::fopen(path.c_str(), "w"); CHECK(file);
    for (const auto &[ino, times] : allocated_times(bytes))
        std::fprintf(file, "%u\t%lld\t%u\t%lld\t%u\t%lld\t%u\n", ino,
            static_cast<long long>(times[0].seconds), times[0].nanoseconds,
            static_cast<long long>(times[1].seconds), times[1].nanoseconds,
            static_cast<long long>(times[2].seconds), times[2].nanoseconds);
    CHECK(!std::fclose(file));
}
Bytes events(const Bytes &input, const std::filesystem::path &output)
{
    Device disk(input); fs::Ext4FileSystem filesystem; mount(filesystem, disk); auto &root = *filesystem.root();
    checked_metadata(root, disk.bytes);
    auto *file = create(disk, root, "file", {fs::NodeType::regular, 06751, 0x12345678, 0x87654321});
    auto *old = create(disk, root, "old", {fs::NodeType::directory, 0750});
    auto *next = create(disk, root, "new", {fs::NodeType::directory, 0755});
    auto *empty = create(disk, root, "empty", {fs::NodeType::directory, 0700});
    auto *victim = create(disk, root, "victim", {fs::NodeType::regular, 0600});
    auto *move = create(disk, root, "move", {fs::NodeType::directory, 0755});
    create(disk, *move, "child", {fs::NodeType::regular, 0644});
    create(disk, *next, "dirvictim", {fs::NodeType::directory, 0700});
    create_link(disk, root, "short", "file"); create_link(disk, root, "long", std::string(80, 'x'));
    const auto id = number(*file), old_id = number(*old), new_id = number(*next), empty_id = number(*empty);
    event(disk, "initial data", {{id, M | C}}, [&] { return file->write(0, text("timestamp payload")); });
    no_event(disk, "prepare sync", [&] { return filesystem.sync(); }, false);
    save(output / "prepared.img", disk.durable); write_oracle(output / "prepared-times.tsv", disk.durable);
    const auto prepared = disk.durable;
    event(disk, "identical overwrite", {{id, M | C}}, [&] { return file->write(0, text("timestamp")); });
    event(disk, "positioned sparse write", {{id, M | C}}, [&] { return file->write(9000, text("position")); });
    const auto size = file->query_metadata().value().size;
    event(disk, "append provider event", {{id, M | C}}, [&] { return file->write(size, text("append")); });
    event(disk, "same size truncate", {{id, M | C}}, [&] { return file->truncate(size + 6); });
    event(disk, "shrink truncate", {{id, M | C}}, [&] { return file->truncate(3); });
    event(disk, "grow truncate", {{id, M | C}}, [&] { return file->truncate(5000); });
    event(disk, "zero truncate", {{id, M | C}}, [&] { return file->truncate(0); });
    event(disk, "already zero truncate", {{id, M | C}}, [&] { return file->truncate(0); });
    event(disk, "nonzero EOF read", {{id, A}}, [&] { auto r = file->read(17, 1); CHECK(r && r.value().size == 0); return r; });
    event(disk, "rewrite data", {{id, M | C}}, [&] { return file->write(0, text("read me")); });
    event(disk, "data read", {{id, A}}, [&] { auto r = file->read(0, 7); CHECK(r && r.value().size == 7 && !std::memcmp(r.value().data.data(), "read me", 7)); return r; });
    auto *short_link = root.lookup("short"), *long_link = root.lookup("long"); CHECK(short_link && long_link);
    event(disk, "inline symlink read", {{number(*short_link), A}}, [&] { auto r = short_link->read(0, 4096); CHECK(r && r.value().size == 4); return r; });
    event(disk, "block symlink read", {{number(*long_link), A}}, [&] { auto r = long_link->read(0, 4096); CHECK(r && r.value().size == 80); return r; });
    event(disk, "public directory list", {{2, A}}, [&] {
        auto result = root.list(); CHECK(result);
        for (usize i = 0; i < result.value().count; ++i)
        {
            const auto &attributes = result.value().entries[i].metadata.attributes;
            CHECK(attributes.has(fs::Attribute::inode));
            const auto raw = raw_times(disk.bytes, static_cast<u32>(attributes.inode));
            const fs::EpochTimestamp times[]{attributes.access, attributes.modification, attributes.change};
            for (usize field = 0; field < 3; ++field)
            {
                CHECK(times[field].source == fs::TimestampSource::maintained_ext4);
                CHECK(times[field].seconds == raw[field].seconds && times[field].nanoseconds == raw[field].nanoseconds);
            }
        }
        return result;
    });
    event(disk, "empty public directory list", {{empty_id, A}}, [&] { auto r = empty->list(); CHECK(r && !r.value().count); return r; });
    event(disk, "repeat empty enumeration", {{empty_id, A}}, [&] { return empty->list(); });
    event(disk, "chmod same value", {{id, C}}, [&] { return file->chmod(06751); });
    event(disk, "chown same values", {{id, C}}, [&] { return file->chown(0x12345678, 0x87654321); });
    event(disk, "chmod changed value", {{id, C}}, [&] { return file->chmod(0600); });
    event(disk, "chown changed values", {{id, C}}, [&] { return file->chown(1000, 2000); });
    no_event(disk, "zero read", [&] { return file->read(0, 0); });
    no_event(disk, "zero EOF read", [&] { return file->read(9000, 0); });
    no_event(disk, "zero inline symlink read", [&] { return short_link->read(0, 0); });
    no_event(disk, "zero write", [&] { return file->write(0, {}); });
    no_event(disk, "checked metadata", [&] { return file->query_metadata(); });
    no_event(disk, "lookup", [&] { return root.lookup_child("file"); });
    no_event(disk, "internal list", [&] { return root.list(fs::DirectoryReadIntent::inspection); });
    no_event(disk, "directory topology", [&] { return move->directory_path(root); });
    no_event(disk, "retain", [&] { return file->retain(); });
    no_event(disk, "release linked file", [&] { return file->release(); });
    event(disk, "hard link", {{id, C}, {old_id, M | C}}, [&] { return old->link("alias", *file); });
    no_event(disk, "same name rename", [&] { return root.rename("file", root, "file"); });
    no_event(disk, "same inode rename aliases", [&] { return root.rename("file", *old, "alias"); });
    event(disk, "same parent rename", {{id, C}, {2, M | C}}, [&] { return root.rename("file", root, "renamed-long-name"); });
    CHECK(!root.lookup("file") && root.lookup("renamed-long-name") == file);
    event(disk, "cross parent rename", {{id, C}, {2, M | C}, {new_id, M | C}}, [&] { return root.rename("renamed-long-name", *next, "moved-file"); });
    const auto victim_id = number(*victim); CHECK(victim->retain());
    event(disk, "replace retained file", {{id, C}, {new_id, M | C}, {2, M | C}, {victim_id, C}}, [&] { return next->rename("moved-file", root, "victim"); });
    checked_metadata(*victim, disk.bytes); CHECK(victim->query_metadata().value().link_count == 0);
    clock_model::fail(); no_event(disk, "replaced victim final reclamation", [&] { return victim->release(); }, false);
    const auto move_id = number(*move);
    event(disk, "move directory and dotdot", {{move_id, C}, {2, M | C}, {old_id, M | C}}, [&] { return root.rename("move", *old, "moved-directory"); });
    CHECK(directory(disk.bytes, move_id).at("..") == old_id);
    auto *dirvictim = next->lookup("dirvictim"); CHECK(dirvictim); const auto dirvictim_id = number(*dirvictim); CHECK(dirvictim->retain());
    event(disk, "replace retained directory", {{move_id, C}, {old_id, M | C}, {new_id, M | C}, {dirvictim_id, C}}, [&] { return old->rename("moved-directory", *next, "dirvictim"); });
    CHECK(directory(disk.bytes, move_id).at("..") == new_id); checked_metadata(*dirvictim, disk.bytes);
    clock_model::fail(); no_event(disk, "replaced directory reclamation", [&] { return dirvictim->release(); }, false);
    event(disk, "unlink surviving hard link", {{id, C}, {old_id, M | C}}, [&] { return old->remove("alias", false); });
    CHECK(file->retain());
    event(disk, "unlink retained file", {{id, C}, {2, M | C}}, [&] { return root.remove("victim", false); });
    checked_metadata(*file, disk.bytes); CHECK(file->query_metadata().value().link_count == 0);
    clock_model::fail(); no_event(disk, "orphan final reclamation", [&] { return file->release(); }, false);
    CHECK(empty->retain());
    event(disk, "rmdir retained empty directory", {{empty_id, C}, {2, M | C}}, [&] { return root.remove("empty", true); });
    checked_metadata(*empty, disk.bytes);
    clock_model::fail(); no_event(disk, "removed directory reclamation", [&] { return empty->release(); }, false);
    const auto short_id = number(*short_link), long_id = number(*long_link);
    event(disk, "inline symlink unlink cleanup", {{2, M | C}}, [&] { return root.remove("short", false); }, {short_id});
    event(disk, "block symlink unlink cleanup", {{2, M | C}}, [&] { return root.remove("long", false); }, {long_id});
    clock_model::fail(); no_event(disk, "sync with unavailable clock", [&] { return filesystem.sync(); }, false);
    checked_metadata(root, disk.bytes); checked_metadata(*move, disk.bytes);
    save(output / "events.img", disk.durable); write_oracle(output / "events-times.tsv", disk.durable);
    Device remount(disk.durable); fs::Ext4FileSystem fresh; mount(fresh, remount);
    checked_metadata(*fresh.root(), remount.bytes);
    CHECK(fresh.root()->lookup("new")->lookup("dirvictim")->lookup("child"));
    return prepared;
}

enum class Operation : unsigned
{
    create_file, create_directory, create_inline, create_block, overwrite, sparse_write,
    truncate_shrink, truncate_same, truncate_zero, truncate_grow, chmod, chown,
    read, eof_read, read_inline, read_block, list, empty_list, link, unlink,
    rmdir, rename_same_parent, rename_cross_parent, rename_replace, move_directory,
    replace_directory, release_orphan, sync, count
};
const char *operation_name(Operation op)
{
    constexpr const char *names[]{"create-file", "create-directory", "create-inline", "create-block", "overwrite",
        "sparse-write", "truncate-shrink", "truncate-same", "truncate-zero", "truncate-grow", "chmod", "chown",
        "read", "eof-read", "read-inline", "read-block", "list", "empty-list", "link", "unlink", "rmdir",
        "rename-same-parent", "rename-cross-parent", "rename-replace", "move-directory", "replace-directory",
        "release-orphan", "sync"};
    return names[static_cast<unsigned>(op)];
}
bool needs_clock(Operation op) { return op != Operation::sync && op != Operation::release_orphan; }
struct Fixture
{
    Device disk;
    fs::Ext4FileSystem filesystem;
    fs::Node *root{}, *file{}, *old{}, *next{}, *empty{}, *victim{}, *move{}, *dirvictim{}, *short_link{}, *long_link{};
    explicit Fixture(const Bytes &prepared) : disk(prepared)
    {
        mount(filesystem, disk); root = filesystem.root();
        file = root->lookup("file"); old = root->lookup("old"); next = root->lookup("new"); empty = root->lookup("empty");
        victim = root->lookup("victim"); move = root->lookup("move"); short_link = root->lookup("short"); long_link = root->lookup("long");
        CHECK(file && old && next && empty && victim && move && short_link && long_link);
        dirvictim = next->lookup("dirvictim"); CHECK(dirvictim);
    }
    void prepare(Operation op)
    {
        if (op == Operation::unlink) CHECK(file->retain());
        if (op == Operation::rmdir) CHECK(empty->retain());
        if (op == Operation::rename_replace) CHECK(victim->retain());
        if (op == Operation::replace_directory) CHECK(dirvictim->retain());
        if (op == Operation::release_orphan) { CHECK(victim->retain()); CHECK(root->remove("victim", false)); }
        if (op == Operation::sync) CHECK(file->chmod(0600));
    }
    Status run(Operation op)
    {
        switch (op)
        {
        case Operation::create_file: return root->create("created", fs::NodeType::regular);
        case Operation::create_directory: return root->create("created", fs::NodeType::directory);
        case Operation::create_inline: return root->create_symlink("created", "file", 5, 7);
        case Operation::create_block: return root->create_symlink("created", std::string(80, 'x'), 5, 7);
        case Operation::overwrite: return file->write(0, text("timestamp"));
        case Operation::sparse_write: return file->write(9000, text("cross allocation boundary"));
        case Operation::truncate_shrink: return file->truncate(2);
        case Operation::truncate_same: return file->truncate(17);
        case Operation::truncate_zero: return file->truncate(0);
        case Operation::truncate_grow: return file->truncate(9000);
        case Operation::chmod: return file->chmod(0600);
        case Operation::chown: return file->chown(25, 37);
        case Operation::read: return file->read(0, 5).status();
        case Operation::eof_read: return file->read(9999, 2).status();
        case Operation::read_inline: return short_link->read(0, 4096).status();
        case Operation::read_block: return long_link->read(0, 4096).status();
        case Operation::list: return root->list().status();
        case Operation::empty_list: return empty->list().status();
        case Operation::link: return old->link("alias", *file);
        case Operation::unlink: return root->remove("file", false);
        case Operation::rmdir: return root->remove("empty", true);
        case Operation::rename_same_parent: return root->rename("file", *root, "longer-renamed-file");
        case Operation::rename_cross_parent: return root->rename("file", *next, "renamed");
        case Operation::rename_replace: return root->rename("file", *root, "victim");
        case Operation::move_directory: return root->rename("move", *old, "moved");
        case Operation::replace_directory: return root->rename("move", *next, "dirvictim");
        case Operation::release_orphan: return victim->release();
        case Operation::sync: return filesystem.sync();
        case Operation::count: break;
        }
        return Status::invalid_argument();
    }
    std::vector<u32> targets(Operation op)
    {
        switch (op)
        {
        case Operation::create_file: case Operation::create_directory: case Operation::create_inline:
        case Operation::create_block: case Operation::list: return {2};
        case Operation::read_inline: return {number(*short_link)};
        case Operation::read_block: return {number(*long_link)};
        case Operation::empty_list: return {number(*empty)};
        case Operation::link: return {number(*file), number(*old)};
        case Operation::unlink: case Operation::rename_same_parent: return {number(*file), 2};
        case Operation::rmdir: return {number(*empty), 2};
        case Operation::rename_cross_parent: return {number(*file), 2, number(*next)};
        case Operation::rename_replace: return {number(*file), 2, number(*victim)};
        case Operation::move_directory: return {number(*move), 2, number(*old)};
        case Operation::replace_directory: return {number(*move), 2, number(*next), number(*dirvictim)};
        default: return {number(*file)};
        }
    }
    void cleanup(Operation op)
    {
        if (op == Operation::unlink) CHECK(file->release());
        if (op == Operation::rmdir) CHECK(empty->release());
        if (op == Operation::rename_replace) CHECK(victim->release());
        if (op == Operation::replace_directory) CHECK(dirvictim->release());
    }
};
// Every failed cache/durable image is retained losslessly relative to the named
// initial image. The Python runner independently reconstructs and hashes it.
void save_delta(const std::filesystem::path &path, const Bytes &base, const Bytes &changed)
{
    CHECK(base.size() == changed.size());
    Bytes output;
    for (usize at = 0; at < base.size(); at += 512)
        if (std::memcmp(base.data() + at, changed.data() + at, 512))
        {
            const auto offset = output.size(); output.resize(offset + 8 + 512);
            for (unsigned i = 0; i < 8; ++i) output[offset + i] = std::byte(static_cast<u64>(at) >> (i * 8));
            std::memcpy(output.data() + offset + 8, changed.data() + at, 512);
        }
    save(path, output);
}
void faults(const Bytes &prepared, const std::filesystem::path &output)
{
    const auto folder = output / "faults"; std::filesystem::create_directory(folder);
    auto *manifest = std::fopen((folder / "cases.tsv").c_str(), "w"); CHECK(manifest);
    std::fprintf(manifest, "operation\tpoint\teffect\ttransport\tsamples\tcache_delta\tdurable_delta\n");
    for (unsigned value = 0; value < static_cast<unsigned>(Operation::count); ++value)
    {
        const auto op = static_cast<Operation>(value); context = operation_name(op);
        Fixture success(prepared); success.prepare(op);
        const auto initial = success.disk.bytes;
        const auto initial_durable = success.disk.durable;
        save(folder / (context + "-initial.img"), initial);
        save(folder / (context + "-initial-durable.img"), initial_durable);
        clock_model::set(sample(1800000000, 765432100)); success.disk.arm(); success.disk.require_sample = needs_clock(op);
        CHECK(success.run(op)); CHECK(clock_model::calls() == success.disk.clock_start + (needs_clock(op) ? 1 : 0));
        success.disk.require_sample = false;
        const auto trace = success.disk.trace; CHECK(!trace.empty());
        auto *trace_file = std::fopen((folder / (context + "-trace.tsv")).c_str(), "w"); CHECK(trace_file);
        for (usize i = 0; i < trace.size(); ++i) std::fprintf(trace_file, "%zu\t%c\t%zu\n", i + 1, trace[i].kind, trace[i].clock_calls);
        CHECK(!std::fclose(trace_file));
        success.cleanup(op); CHECK(success.filesystem.sync());
        save(folder / (context + "-success.img"), success.disk.durable);
        for (usize point = 1; point <= trace.size(); ++point)
            for (unsigned effect = 0; effect < 3; ++effect)
            {
                context = std::string(operation_name(op)) + " fault " + std::to_string(point) + " effect " + std::to_string(effect);
                Fixture failed(prepared); failed.prepare(op); CHECK(failed.disk.bytes == initial && failed.disk.durable == initial_durable);
                clock_model::set(sample(1800000000, 765432100)); failed.disk.arm(point, effect);
                failed.disk.require_sample = needs_clock(op);
                const auto result = failed.run(op); CHECK(!result && result.code() == StatusCode::io_error);
                CHECK(failed.disk.triggered); CHECK(clock_model::calls() == failed.disk.clock_start + trace[point - 1].clock_calls);
                // No fresh rollback/read/write is permitted inside a failed
                // mutation. Only sync's documented dirty-marker repair remains.
                const bool repair = op == Operation::sync && point >= 3;
                CHECK(failed.disk.trace.size() == point + (repair ? 2 : 0));
                if (repair)
                {
                    CHECK(failed.disk.trace[point].kind == 'w');
                    CHECK(failed.disk.trace[point + 1].kind == 'f');
                }
                failed.disk.require_sample = false;
                const auto writes = failed.disk.writes, calls = clock_model::calls();
                CHECK(failed.root->query_metadata().status().code() == StatusCode::io_error);
                CHECK(failed.filesystem.sync().code() == StatusCode::io_error);
                CHECK(failed.root->create("after-failure", fs::NodeType::regular).code() == StatusCode::io_error);
                CHECK(failed.disk.writes == writes && clock_model::calls() == calls);
                const auto stem = std::string(operation_name(op)) + "-" + std::to_string(point) + "-" + std::to_string(effect);
                save_delta(folder / (stem + "-cache.delta"), initial, failed.disk.bytes);
                save_delta(folder / (stem + "-durable.delta"), initial_durable, failed.disk.durable);
                std::fprintf(manifest, "%s\t%zu\t%u\t%c\t%zu\t%s-cache.delta\t%s-durable.delta\n",
                    operation_name(op), point, effect, trace[point - 1].kind, clock_model::calls() - failed.disk.clock_start,
                    stem.c_str(), stem.c_str());
                ++fault_count;
            }
        std::printf("PASS fault operation %s positions=%zu outcomes=%zu\n", operation_name(op), trace.size(), trace.size() * 3);
        std::fflush(stdout);
    }
    CHECK(!std::fclose(manifest));
}
void clock_failures(const Bytes &input, const Bytes &prepared)
{
    context = "admission unavailable";
    Device missing(input); fs::Ext4FileSystem denied; clock_model::reset();
    CHECK(denied.mount(missing, maintained).code() == StatusCode::io_error);
    CHECK(!denied.root() && missing.bytes == input && !missing.writes && clock_model::calls() == 1); ++negative_count;
    clock_model::set(sample()); CHECK(denied.mount(missing, maintained)); CHECK(denied.root());
    for (bool writable : {false, true})
    {
        context = writable ? "legacy writable" : "legacy readonly";
        Device disk(input); fs::Ext4FileSystem legacy; clock_model::reset(); CHECK(legacy.mount(disk, writable));
        CHECK(!clock_model::calls()); auto result = legacy.root()->query_metadata(); CHECK(result);
        CHECK(result.value().attributes.access.source == (writable ? fs::TimestampSource::unavailable : fs::TimestampSource::readonly_ext4));
        CHECK(!clock_model::calls() && !disk.writes && disk.bytes == input); ++negative_count;
    }
    for (unsigned value = 0; value < static_cast<unsigned>(Operation::count); ++value)
    {
        const auto op = static_cast<Operation>(value); if (!needs_clock(op)) continue;
        context = std::string("clock unavailable ") + operation_name(op);
        Fixture f(prepared); f.prepare(op); const auto before = f.disk.bytes, durable = f.disk.durable;
        clock_model::fail(); f.disk.arm(); const auto writes = f.disk.writes;
        CHECK(f.run(op).code() == StatusCode::io_error); CHECK(clock_model::calls() == f.disk.clock_start + 1);
        CHECK(f.disk.bytes == before && f.disk.durable == durable && f.disk.writes == writes);
        checked_metadata(*f.root, f.disk.bytes); checked_metadata(*f.file, f.disk.bytes);
        const auto calls = clock_model::calls(); CHECK(f.root->lookup_child("file")); CHECK(f.root->list(fs::DirectoryReadIntent::inspection));
        CHECK(clock_model::calls() == calls);
        clock_model::set(sample()); CHECK(f.run(op)); f.cleanup(op); CHECK(f.filesystem.sync()); ++negative_count;
    }
    // Invalid sample fields are clock failures, never permission to write epoch0.
    for (unsigned defect = 0; defect < 5; ++defect)
    {
        context = "invalid realtime sample " + std::to_string(defect); Fixture f(prepared);
        auto invalid = sample();
        if (defect == 0) invalid.value.nanoseconds = 1000000000;
        if (defect == 1) invalid.wall_generation = 0;
        if (defect == 2) invalid.monotonic.generation = 0;
        if (defect == 3) invalid.origin = time::RealtimeOrigin::none;
        if (defect == 4) invalid.origin = static_cast<time::RealtimeOrigin>(255);
        clock_model::set(invalid); f.disk.arm(); const auto before = f.disk.bytes;
        CHECK(f.file->chmod(0600).code() == StatusCode::io_error);
        CHECK(f.disk.bytes == before && !f.disk.writes && clock_model::calls() == f.disk.clock_start + 1);
        clock_model::set(sample()); CHECK(f.file->chmod(0600)); ++negative_count;
    }
    for (unsigned code = 1; code <= static_cast<unsigned>(StatusCode::name_too_long); ++code)
    {
        context = "clock status normalization " + std::to_string(code); Fixture f(prepared);
        clock_model::fail(Status{static_cast<StatusCode>(code)}); const auto before = f.disk.bytes; const auto calls = clock_model::calls();
        CHECK(f.file->read(0, 1).status().code() == StatusCode::io_error);
        CHECK(f.disk.bytes == before && !f.disk.writes && clock_model::calls() == calls + 1);
        clock_model::set(sample()); CHECK(f.file->read(0, 1)); ++negative_count;
    }
    Fixture backwards(prepared); const auto ino = number(*backwards.file); const auto before = raw_times(backwards.disk.bytes, ino);
    auto earlier = sample(1000000000, 987654321); earlier.origin = time::RealtimeOrigin::boot_rtc; earlier.wall_generation = 8;
    clock_model::set(earlier); const auto calls = clock_model::calls(); CHECK(backwards.file->chmod(0700));
    const auto after = raw_times(backwards.disk.bytes, ino);
    CHECK(after[0] == before[0] && after[1] == before[1] && after[2] == stored_sample(backwards.disk.bytes, ino, 2, earlier.value));
    CHECK(clock_model::calls() == calls + 1); ++negative_count;
}

void range_and_malformed(const Bytes &input, const Bytes &prepared, const std::filesystem::path &output)
{
    constexpr i64 seconds[]{-2147483649LL, -2147483648LL, -1, 0, 2147483647LL,
        2147483648LL, 4294967295LL, 4294967296LL, 15032385535LL, 15032385536LL};
    const bool extended = r16(input, 1112) == 256;
    const unsigned extents = extended ? 7 : 1;
    constexpr u16 extents_values[]{0, 4, 8, 12, 16, 24, 128};
    auto *record = std::fopen((output / "range.tsv").c_str(), "w"); CHECK(record);
    std::fprintf(record, "extent\tfield\tseconds\tsuccess\n");
    for (unsigned e = 0; e < extents; ++e)
        for (unsigned field = 0; field < 3; ++field)
            for (const auto sec : seconds)
            {
                context = "range extent=" + std::to_string(extents_values[e]) + " field=" + std::to_string(field) + " seconds=" + std::to_string(sec);
                Fixture f(prepared); const auto ino = number(*f.file); const auto at = inode_offset(f.disk.bytes, ino);
                if (extended)
                {
                    w16(f.disk.bytes, at + 128, extents_values[e]);
                    for (usize offset = 132; offset < 256; offset += 4) w32(f.disk.bytes, at + offset, 0);
                }
                const auto before = f.disk.bytes; const auto old = raw_times(before, ino);
                const unsigned required[]{16, 12, 8};
                const bool has_extra = extended && extents_values[e] >= required[field];
                const bool fits = sec >= -2147483648LL && sec <= (has_extra ? 15032385535LL : 2147483647LL);
                clock_model::set(sample(sec, 999999999)); f.disk.arm();
                Status result;
                if (field == 0) result = f.file->read(0, 1).status();
                if (field == 1) result = f.file->truncate(17);
                if (field == 2) result = f.file->chmod(0600);
                // Truncate also selects C, whose extra word precedes M.
                CHECK(clock_model::calls() == f.disk.clock_start + 1);
                if (fits)
                {
                    CHECK(result); const auto actual = raw_times(f.disk.bytes, ino);
                    for (usize i = 0; i < 3; ++i)
                        CHECK(actual[i] == ((i == field || (field == 1 && i == 2))
                            ? stored_sample(f.disk.bytes, ino, i, sample(sec, 999999999).value) : old[i]));
                    if (extended) CHECK(r16(f.disk.bytes, at + 128) == r16(before, at + 128));
                }
                else
                {
                    CHECK(result.code() == StatusCode::overflow && f.disk.bytes == before && !f.disk.writes);
                    checked_metadata(*f.file, f.disk.bytes); clock_model::set(sample()); CHECK(f.file->chmod(0600));
                }
                std::fprintf(record, "%u\t%u\t%lld\t%u\n", extended ? extents_values[e] : 0, field, static_cast<long long>(sec), fits);
                ++range_count;
            }
    CHECK(!std::fclose(record));
    // Mount representability depends on all root fields, not just C.
    for (unsigned e = 0; e < extents; ++e)
    {
        auto bytes = input; const auto at = inode_offset(bytes, 2);
        if (extended) { w16(bytes, at + 128, extents_values[e]); for (usize offset = 132; offset < 256; offset += 4) w32(bytes, at + offset, 0); }
        Device disk(bytes); fs::Ext4FileSystem filesystem; clock_model::reset(); clock_model::set(sample(2147483648LL));
        context = "mount extended range " + std::to_string(extents_values[e]);
        auto status = filesystem.mount(disk, maintained);
        CHECK(extended && extents_values[e] >= 16 ? bool(status) : status.code() == StatusCode::overflow);
        CHECK(disk.bytes == bytes && !disk.writes && clock_model::calls() == 1); ++range_count;
    }
    if (!extended) return;
    for (unsigned defect = 0; defect < 5; ++defect)
    {
        auto bytes = input; const auto at = inode_offset(bytes, 2);
        if (defect < 3) { constexpr usize fields[]{140, 136, 132}; w32(bytes, at + fields[defect], 4000000000u); }
        if (defect == 3) w16(bytes, at + 128, 3);
        if (defect == 4) w16(bytes, at + 128, 132);
        Device disk(bytes); fs::Ext4FileSystem filesystem; clock_model::reset(); clock_model::set(sample());
        context = "malformed mount " + std::to_string(defect);
        CHECK(filesystem.mount(disk, maintained).code() == StatusCode::io_error);
        CHECK(!filesystem.root() && !clock_model::calls() && !disk.writes && disk.bytes == bytes); ++negative_count;
    }
    for (unsigned value = 0; value < static_cast<unsigned>(Operation::release_orphan); ++value)
    {
        const auto op = static_cast<Operation>(value); Fixture identity(prepared); const auto targets = identity.targets(op);
        for (const auto ino : targets)
            for (unsigned defect = 0; defect < 5; ++defect)
            {
                context = std::string("malformed ") + operation_name(op) + " inode=" + std::to_string(ino) + " defect=" + std::to_string(defect);
                Fixture f(prepared); f.prepare(op); const auto at = inode_offset(f.disk.bytes, ino);
                if (defect < 3) { constexpr usize fields[]{140, 136, 132}; w32(f.disk.bytes, at + fields[defect], 4000000000u); }
                if (defect == 3) w16(f.disk.bytes, at + 128, 3);
                if (defect == 4) w16(f.disk.bytes, at + 128, 132);
                const auto before = f.disk.bytes; const auto writes = f.disk.writes;
                f.disk.arm(); CHECK(f.run(op).code() == StatusCode::io_error);
                CHECK(clock_model::calls() == f.disk.clock_start && f.disk.bytes == before && f.disk.writes == writes);
                CHECK(f.root->query_metadata().status().code() == StatusCode::io_error);
                CHECK(f.filesystem.sync().code() == StatusCode::io_error && f.disk.writes == writes); ++negative_count;
            }
    }
    // A later participant cannot encode the event even though the source can.
    for (unsigned value : {static_cast<unsigned>(Operation::link), static_cast<unsigned>(Operation::rename_replace),
                          static_cast<unsigned>(Operation::replace_directory)})
    {
        const auto op = static_cast<Operation>(value); Fixture f(prepared); f.prepare(op);
        const auto ids = f.targets(op); const auto at = inode_offset(f.disk.bytes, ids.back());
        w16(f.disk.bytes, at + 128, 0); const auto before = f.disk.bytes; f.disk.arm(); clock_model::set(sample(2147483648LL));
        CHECK(f.run(op).code() == StatusCode::overflow && f.disk.bytes == before && !f.disk.writes);
        CHECK(clock_model::calls() == f.disk.clock_start + 1); clock_model::set(sample()); CHECK(f.run(op)); f.cleanup(op); ++negative_count;
    }
}
void preflight(const Bytes &input, const Bytes &prepared)
{
    for (bool directory_access : {false, true})
    {
        context = directory_access ? "unsupported directory mutation flags" : "unsupported regular mutation flags";
        Fixture f(prepared); auto *node = directory_access ? f.empty : f.file;
        const auto at = inode_offset(f.disk.bytes, number(*node));
        const auto flags = r32(f.disk.bytes, at + 32); w32(f.disk.bytes, at + 32, flags | 0x10u);
        const auto before = f.disk.bytes; const auto calls = clock_model::calls();
        const auto status = directory_access ? node->list().status() : node->read(0, 1).status();
        CHECK(status.code() == StatusCode::unsupported && f.disk.bytes == before && !f.disk.writes);
        CHECK(clock_model::calls() == calls);
        if (directory_access) CHECK(node->list(fs::DirectoryReadIntent::inspection));
        else CHECK(node->read(0, 0));
        CHECK(clock_model::calls() == calls && f.disk.bytes == before && !f.disk.writes);
        w32(f.disk.bytes, at + 32, flags);
        CHECK(directory_access ? bool(node->list()) : bool(node->read(0, 1))); ++negative_count;
    }
    for (unsigned test = 0; test < 19; ++test)
    {
        context = "ordinary preflight " + std::to_string(test); Fixture f(prepared);
        clock_model::fail(); const auto calls = clock_model::calls(); const auto before = f.disk.bytes;
        Status result;
        switch (test)
        {
        case 0: result = f.root->create("file", fs::NodeType::regular); break;
        case 1: result = f.root->create("bad/name", fs::NodeType::regular); break;
        case 2: result = f.root->create("", fs::NodeType::regular); break;
        case 3: result = f.root->create("device", fs::NodeType::device); break;
        case 4: result = f.root->create_symlink("bad", "", 0, 0); break;
        case 5: result = f.root->create_symlink("bad", std::string(block_size(input), 'x'), 0, 0); break;
        case 6: result = f.root->remove("missing", false); break;
        case 7: result = f.root->remove("move", true); break;
        case 8: result = f.root->rename("file", *f.root, "move"); break;
        case 9: result = f.root->rename("empty", *f.root, "move"); break;
        case 10: result = f.root->rename("missing", *f.root, "unused"); break;
        case 11: result = f.root->link("file", *f.file); break;
        case 12: result = f.root->link("directory-alias", *f.old); break;
        case 13: result = f.root->write(0, text("bad")); break;
        case 14: result = f.root->truncate(0); break;
        case 15: result = f.file->truncate(usize{0x100000000ULL}); break;
        case 16: result = f.file->write(usize{0xffffffffULL}, text("bad")); break;
        case 17: result = f.short_link->truncate(0); break;
        default: result = f.file->list().status(); break;
        }
        CHECK(!result && result.code() != StatusCode::io_error);
        CHECK(clock_model::calls() == calls && !f.disk.writes && f.disk.bytes == before);
        clock_model::set(sample()); CHECK(f.file->chmod(0600)); ++negative_count;
    }
    for (bool unsupported : {false, true})
    {
        auto bytes = input; if (unsupported) w32(bytes, 1116, 4); else w16(bytes, 1082, 0);
        Device disk(bytes); fs::Ext4FileSystem filesystem; clock_model::reset(); clock_model::set(sample());
        CHECK(!filesystem.mount(disk, maintained)); CHECK(!filesystem.root() && !clock_model::calls() && !disk.writes && disk.bytes == bytes); ++negative_count;
    }
    Device disk(input); fs::Ext4FileSystem filesystem; clock_model::reset(); clock_model::set(sample());
    CHECK(filesystem.mount(disk, fs::Ext4MountOptions{false, fs::Ext4TimestampPolicy::maintained}).code() == StatusCode::invalid_argument);
    CHECK(!filesystem.root() && !clock_model::calls() && !disk.writes && disk.bytes == input); ++negative_count;
}
usize largest_slot(const Bytes &b, u32 ino)
{
    usize largest = 0; const auto at = inode_offset(b, ino), size = block_size(b);
    for (usize extent = 0; extent < r16(b, at + 42); ++extent)
        for (usize block = 0; block < r16(b, at + 56 + extent * 12); ++block)
        {
            const auto start = (r32(b, at + 60 + extent * 12) + block) * size;
            for (usize entry = start; entry < start + size; entry += r16(b, entry + 4))
            {
                const usize used = r32(b, entry) ? ((8 + std::to_integer<usize>(b[entry + 6]) + 3) & ~usize{3}) : 0;
                largest = std::max(largest, r16(b, entry + 4) - used);
            }
        }
    return largest;
}
void growth(const Bytes &prepared, const std::filesystem::path &output)
{
    Fixture f(prepared); unsigned count = 0;
    while (largest_slot(f.disk.bytes, 2) >= 40)
    {
        char name[33]; std::snprintf(name, sizeof(name), "padding-%03u-xxxxxxxxxxxxxxxxxxx", count++);
        CHECK(std::strlen(name) == 31 && count < 115);
        create(f.disk, *f.root, name, {fs::NodeType::regular, 0644});
    }
    CHECK(f.filesystem.sync()); save(output / "growth-initial.img", f.disk.durable);
    const auto size = f.root->query_metadata().value().size;
    event(f.disk, "same parent rename forces directory growth", {{2, M | C}, {number(*f.file), C}},
        [&] { return f.root->rename("file", *f.root, "renamed-xxxxxxxxxxxxxxxxxxxxxxx"); });
    CHECK(f.root->query_metadata().value().size == size + block_size(prepared));
    CHECK(!f.root->lookup("file") && f.root->lookup("renamed-xxxxxxxxxxxxxxxxxxxxxxx") == f.file);
    CHECK(f.filesystem.sync()); save(output / "growth.img", f.disk.durable); write_oracle(output / "growth-times.tsv", f.disk.durable);
}
void mixed_precision(const Bytes &prepared, const std::filesystem::path &output)
{
    if (r16(prepared, 1112) != 256) return;
    for (const u16 extent : {0, 4, 8, 12, 16, 24, 128})
    {
        auto initial = prepared; const auto at = inode_offset(initial, 2); w16(initial, at + 128, extent);
        Fixture f(initial); const auto stem = "mixed-" + std::to_string(extent);
        save(output / (stem + "-initial.img"), initial);
        auto *child = create(f.disk, *f.root, "mixed-child", {fs::NodeType::regular, 0777, 0xffffffff, 0xfedcba98});
        CHECK(number(*child) != 2 && r16(f.disk.bytes, inode_offset(f.disk.bytes, number(*child)) + 128) == 16);
        CHECK(f.filesystem.sync()); save(output / (stem + ".img"), f.disk.durable);
        write_oracle(output / (stem + "-times.tsv"), f.disk.durable);
    }
}
void symlink_variants(const Bytes &prepared, const std::filesystem::path &output)
{
    Fixture f(prepared);
    unsigned iteration = 0;
    for (const auto length : {usize{1}, usize{59}, usize{60}, usize{255}, block_size(prepared) - 1})
    {
        const auto name = "boundary-" + std::to_string(iteration++);
        const std::string target(length, 'x'); create_link(f.disk, *f.root, name.c_str(), target);
        auto *node = f.root->lookup(name); CHECK(node); const auto ino = number(*node), parent = number(*f.old);
        event(f.disk, "shortened readlink provider", {{ino, A}}, [&] {
            auto result = node->read(0, 1); CHECK(result && result.value().size == 1 && result.value().data[0] == std::byte{'x'}); return result;
        });
        event(f.disk, "symlink EOF provider", {{ino, A}}, [&] { auto result = node->read(length + 1, 1); CHECK(result && !result.value().size); return result; });
        no_event(f.disk, "symlink zero read", [&] { return node->read(0, 0); });
        event(f.disk, "hardlink symlink inode", {{ino, C}, {parent, M | C}}, [&] { return f.old->link("link-alias", *node); });
        clock_model::fail(); no_event(f.disk, "same inode symlink rename with clock absent", [&] { return f.root->rename(name, *f.old, "link-alias"); });
        CHECK(node->retain());
        event(f.disk, "remove symlink original alias", {{ino, C}, {2, M | C}}, [&] { return f.root->remove(name, false); });
        event(f.disk, "rename retained symlink alias", {{ino, C}, {parent, M | C}}, [&] { return f.old->rename("link-alias", *f.old, "renamed-link"); });
        event(f.disk, "unlink retained symlink", {{ino, C}, {parent, M | C}}, [&] { return f.old->remove("renamed-link", false); });
        checked_metadata(*node, f.disk.bytes); clock_model::fail();
        no_event(f.disk, "final symlink reclamation", [&] { return node->release(); }, false);
    }
    CHECK(f.filesystem.sync()); save(output / "symlink-variants.img", f.disk.durable);
    write_oracle(output / "symlink-variants-times.tsv", f.disk.durable);
}
void capacity(const Bytes &prepared, const std::filesystem::path &output)
{
    // Deliberately synthetic allocation exhaustion, not fsck-qualified images.
    for (bool inodes : {false, true})
    {
        auto initial = prepared; const auto size = block_size(initial);
        const u32 groups = (r32(initial, 1028) - r32(initial, 1044) - 1) / r32(initial, 1056) + 1;
        w32(initial, inodes ? 1040 : 1036, 0);
        for (u32 group = 0; group < groups; ++group)
        {
            const auto descriptor = (r32(initial, 1044) + 1) * size + group * 32;
            const auto bitmap = r32(initial, descriptor + (inodes ? 4 : 0)) * size;
            std::fill_n(initial.begin() + bitmap, size, std::byte{0xff});
            w16(initial, descriptor + (inodes ? 14 : 12), 0);
        }
        for (unsigned action = 0; action < (inodes ? 2u : 3u); ++action)
        {
            context = std::string("synthetic capacity ") + (inodes ? "inodes " : "blocks ") + std::to_string(action);
            Fixture f(initial); const auto before = f.disk.bytes; const auto calls = clock_model::calls(); clock_model::fail();
            Status result;
            if (inodes) result = f.root->create("no-capacity", action ? fs::NodeType::directory : fs::NodeType::regular);
            else if (action == 0) result = f.root->create("no-capacity", fs::NodeType::directory);
            else if (action == 1) result = f.root->create_symlink("no-capacity", std::string(80, 'x'), 0, 0);
            else result = f.file->write(9000, text("no allocation capacity"));
            CHECK(!result && result.code() == (inodes || action == 0 ? StatusCode::no_space : StatusCode::no_memory));
            CHECK(f.disk.bytes == before && !f.disk.writes && clock_model::calls() == calls);
            checked_metadata(*f.root, f.disk.bytes); ++negative_count;
        }
    }
    Fixture exhausted(prepared); bool full = false;
    for (unsigned index = 0; index < 130; ++index)
    {
        context = "node-cache preflight " + std::to_string(index); const auto before = exhausted.disk.bytes;
        const auto calls = clock_model::calls(); const auto writes = exhausted.disk.writes;
        const auto now = sample(1750000000 + index, 500000001); clock_model::set(now);
        auto result = exhausted.root->create_child("cache-" + std::to_string(index), {fs::NodeType::regular, 0644});
        if (!result)
        {
            CHECK(result.status().code() == StatusCode::no_memory && before == exhausted.disk.bytes);
            CHECK(exhausted.disk.writes == writes && clock_model::calls() == calls);
            checked_metadata(*exhausted.root, exhausted.disk.bytes); full = true; ++negative_count; break;
        }
        CHECK(result.value() && clock_model::calls() == calls + 1);
        const Change changes[]{{2, M | C}, {number(*result.value()), A | M | C}};
        verify_event(before, exhausted.disk.bytes, changes, now.value); ++event_count;
    }
    CHECK(full && exhausted.filesystem.sync()); save(output / "node-cache.img", exhausted.disk.durable);
    write_oracle(output / "node-cache-times.tsv", exhausted.disk.durable);
}
void extent_capacity(const Bytes &prepared, const std::filesystem::path &output)
{
    Fixture f(prepared); const auto size = block_size(prepared);
    auto *file = create(f.disk, *f.root, "four-extents", {fs::NodeType::regular, 0644});
    const auto ino = number(*file);
    for (usize logical = 0; logical < 8; logical += 2)
        event(f.disk, "build four disjoint logical extents", {{ino, M | C}}, [&] { return file->write(logical * size, text("extent")); });
    CHECK(r16(f.disk.bytes, inode_offset(f.disk.bytes, ino) + 42) == 4);
    CHECK(f.filesystem.sync()); save(output / "file-extents-initial.img", f.disk.durable);
    const Bytes payload(2 * size, std::byte{'q'});
    event(f.disk, "merge fourth extent with two contiguous blocks", {{ino, M | C}}, [&] { return file->write(7 * size, payload); });
    CHECK(r16(f.disk.bytes, inode_offset(f.disk.bytes, ino) + 42) == 4);
    CHECK(r16(f.disk.bytes, inode_offset(f.disk.bytes, ino) + 56 + 3 * 12) == 3);
    const auto before = f.disk.bytes; const auto writes = f.disk.writes, calls = clock_model::calls();
    clock_model::fail(); CHECK(file->write(10 * size, text("fifth extent")).code() == StatusCode::overflow);
    CHECK(f.disk.bytes == before && f.disk.writes == writes && clock_model::calls() == calls);
    checked_metadata(*file, f.disk.bytes); ++negative_count;
    CHECK(f.filesystem.sync()); save(output / "file-extents.img", f.disk.durable);
    write_oracle(output / "file-extents-times.tsv", f.disk.durable);

    Fixture d(prepared); auto *parent = create(d.disk, *d.root, "extent-capacity", {fs::NodeType::directory, 0755});
    const auto parent_id = number(*parent), source_id = number(*d.file);
    usize old_blocks = 0; unsigned blockers = 0, links = 0;
    while (true)
    {
        const auto blocks = parent->query_metadata().value().blocks;
        if (blocks != old_blocks)
        {
            const auto name = "extent-blocker-" + std::to_string(blockers++);
            auto *blocker = create(d.disk, *d.root, name.c_str(), {fs::NodeType::regular, 0644});
            event(d.disk, "separate directory extents physically", {{number(*blocker), M | C}}, [&] { return blocker->write(0, text("block")); });
            old_blocks = blocks;
        }
        if (r16(d.disk.bytes, inode_offset(d.disk.bytes, parent_id) + 42) == 4 && largest_slot(d.disk.bytes, parent_id) < 40) break;
        char name[32]; std::snprintf(name, sizeof(name), "%031u", links++); CHECK(links < 1000);
        event(d.disk, "fill fragmented directory", {{source_id, C}, {parent_id, M | C}}, [&] { return parent->link(name, *d.file); });
    }
    CHECK(d.filesystem.sync()); save(output / "directory-extents-initial.img", d.disk.durable);
    for (unsigned action = 0; action < 6; ++action)
    {
        context = "directory fifth extent preflight " + std::to_string(action);
        const auto initial = d.disk.bytes; const auto previous_writes = d.disk.writes, samples = clock_model::calls();
        clock_model::fail(); Status result;
        constexpr auto name = "capacity-xxxxxxxxxxxxxxxxxxxxxx";
        if (action < 2) result = parent->create(name, action ? fs::NodeType::directory : fs::NodeType::regular);
        if (action == 2) result = parent->create_symlink(name, std::string(80, 'x'), 0, 0);
        if (action == 3) result = parent->link(name, *d.file);
        if (action == 4) result = d.root->rename("victim", *parent, name);
        if (action == 5) result = parent->list().status();
        CHECK(result.code() == (action < 2 ? StatusCode::no_space : StatusCode::overflow));
        CHECK(d.disk.bytes == initial && d.disk.writes == previous_writes && clock_model::calls() == samples);
        checked_metadata(*parent, d.disk.bytes); ++negative_count;
    }
    event(d.disk, "release directory entry at extent limit", {{source_id, C}, {parent_id, M | C}}, [&] { return parent->remove("0000000000000000000000000000000", false); });
    create(d.disk, *parent, "fits-again", {fs::NodeType::regular, 0600});
    CHECK(d.filesystem.sync()); save(output / "directory-extents.img", d.disk.durable);
    write_oracle(output / "directory-extents-times.tsv", d.disk.durable);
}
int main(int argc, char **argv)
{
    CHECK(argc == 3); const auto input = load(argv[1]); const std::filesystem::path output = argv[2];
    const auto prepared = events(input, output);
    clock_failures(input, prepared); preflight(input, prepared); range_and_malformed(input, prepared, output);
    growth(prepared, output); mixed_precision(prepared, output); symlink_variants(prepared, output);
    capacity(prepared, output); extent_capacity(prepared, output); faults(prepared, output);
    std::printf("COUNTS {\"events\":%zu,\"no_events\":%zu,\"negative\":%zu,\"range\":%zu,\"faults\":%zu,\"publication_observations\":%zu}\n",
        event_count, no_event_count, negative_count, range_count, fault_count, publication_count);
    std::puts("PASS maintained ext4 hosted timestamps");
}
