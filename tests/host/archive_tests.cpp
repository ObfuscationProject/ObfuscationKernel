#include "ok/fs/archive.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
using namespace ok;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!static_cast<bool>(condition))                                                                             \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                       \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

using Blob = std::vector<std::byte>;
void write32(Blob &blob, usize at, u32 value)
{
    for (usize i = 0; i < 4; ++i)
        blob[at + i] = static_cast<std::byte>((value >> (i * 8)) & 0xffu);
}
Blob header(u32 count)
{
    Blob blob(16);
    write32(blob, 0, 0x52414b4f);
    write32(blob, 4, 1);
    write32(blob, 8, count);
    return blob;
}
usize record(Blob &blob, std::string_view path, std::span<const std::byte> data, u32 mode)
{
    const auto begin = blob.size();
    blob.resize(begin + 16);
    write32(blob, begin, static_cast<u32>(path.size()));
    write32(blob, begin + 4, static_cast<u32>(data.size()));
    write32(blob, begin + 8, mode);
    for (const auto c : path)
        blob.push_back(static_cast<std::byte>(c));
    blob.insert(blob.end(), data.begin(), data.end());
    while (blob.size() % 8)
        blob.push_back(std::byte{});
    return begin;
}
void check_rejected_without_files(const Blob &blob)
{
    fs::VirtualFileSystem vfs;
    CHECK(!fs::mount_archive(vfs, blob));
    CHECK(vfs.lookup("/bin") == nullptr);
    CHECK(vfs.lookup("/payload") == nullptr);
}
void test_large_readonly_files()
{
    auto blob = header(2);
    record(blob, "/bin", {}, fs::mode_directory | 0755);
    Blob payload(2 * fs::max_file_data + 37);
    for (usize i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::byte>((i * 17 + 5) & 0xffu);
    record(blob, "/bin/large", payload, fs::mode_regular | 0555);
    fs::VirtualFileSystem vfs;
    CHECK(fs::mount_archive(vfs, blob));
    auto *node = vfs.lookup("/bin/large");
    CHECK(node);
    CHECK(node->metadata().size == payload.size());
    CHECK((node->metadata().mode & 0777) == 0555);
    const auto first = vfs.read_file("/bin/large");
    CHECK(first && first.value().size == fs::max_file_data);
    usize offset = 0;
    while (offset != payload.size())
    {
        auto chunk = node->read(offset, ~usize{0});
        CHECK(chunk);
        const auto remaining = payload.size() - offset;
        CHECK(chunk.value().size == (remaining < fs::max_file_data ? remaining : fs::max_file_data));
        for (usize i = 0; i < chunk.value().size; ++i)
            CHECK(chunk.value().data[i] == payload[offset + i]);
        offset += chunk.value().size;
    }
    CHECK(node->read(offset, 9999).value().size == 0);
    CHECK(node->read(10, 0).value().size == 0);
    CHECK(node->write(0, {}).code() == StatusCode::denied);
    CHECK(vfs.write_file("/bin/large", {payload.data(), 1}).code() == StatusCode::denied);
    CHECK(node->metadata().size == payload.size());
}
void test_archive_does_not_use_writable_file_pool()
{
    auto blob = header(42);
    record(blob, "/bin", {}, fs::mode_directory | 0755);
    record(blob, "/work", {}, fs::mode_directory | 0755);
    std::array<std::byte, 1> byte{std::byte{42}};
    for (unsigned i = 0; i < 40; ++i)
        record(blob, "/bin/a" + std::to_string(i), byte, fs::mode_regular | 0444);
    fs::VirtualFileSystem vfs;
    CHECK(fs::mount_archive(vfs, blob));
    for (usize i = 0; i < fs::max_ram_file_buffers; ++i)
    {
        const auto path = "/work/w" + std::to_string(i);
        CHECK(vfs.create(path, fs::NodeType::regular));
        CHECK(vfs.write_file(path, byte));
    }
    CHECK(!vfs.create("/work/full", fs::NodeType::regular));
    CHECK(vfs.read_file("/bin/a39").value().data[0] == byte[0]);
}
void test_malformed_archive()
{
    auto good = header(2);
    record(good, "/bin", {}, fs::mode_directory | 0755);
    const auto file_record = record(good, "/bin/app", {}, fs::mode_regular | 0555);
    for (usize size = 0; size < good.size(); ++size)
    {
        Blob truncated(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(size));
        check_rejected_without_files(truncated);
    }
    for (const auto [field, value] :
         std::array<std::array<u32, 2>, 6>{{{0, 0}, {4, 2}, {8, 1000}, {8, 1}, {8, 3}, {12, 1}}})
    {
        auto bad = good;
        write32(bad, field, value);
        check_rejected_without_files(bad);
    }
    for (const auto [field, value] : std::array<std::array<u32, 2>, 4>{
             {{0, 0xffffffffu}, {4, 0xffffffffu}, {8, fs::mode_character_device}, {12, 1}}})
    {
        auto bad = good;
        write32(bad, file_record + field, value);
        check_rejected_without_files(bad);
    }
    auto bad_directory = good;
    write32(bad_directory, 20, 1);
    check_rejected_without_files(bad_directory);
    auto trailing = good;
    trailing.push_back(std::byte{});
    check_rejected_without_files(trailing);
    auto padding = header(1);
    record(padding, "/payload", {reinterpret_cast<const std::byte *>("x"), 1}, fs::mode_regular | 0444);
    CHECK(padding.back() == std::byte{});
    padding.back() = std::byte{1};
    check_rejected_without_files(padding);
    for (const std::string &path : std::vector<std::string>{"relative", "/", "/bin/", "/../payload", "/bin/../payload",
                                                            "/./payload", "/bin//app", "/" + std::string(32, 'a')})
    {
        auto invalid = header(1);
        record(invalid, path, {}, fs::mode_regular | 0555);
        check_rejected_without_files(invalid);
    }
    auto nul = header(1);
    record(nul, std::string_view("/bin\0/app", 9), {}, fs::mode_regular | 0555);
    check_rejected_without_files(nul);
    fs::VirtualFileSystem empty;
    CHECK(fs::mount_archive(empty, header(0)));
}

void directory_mode(fs::VirtualFileSystem &vfs, std::string_view path, u32 permissions)
{
    const auto metadata = vfs.stat(path);
    CHECK(metadata);
    CHECK(metadata.value().type == fs::NodeType::directory);
    CHECK(metadata.value().mode == (fs::mode_directory | permissions));
    CHECK(metadata.value().uid == 0 && metadata.value().gid == 0);
}

void test_exact_directory_metadata()
{
    auto owner = std::make_unique<fs::VirtualFileSystem>();
    auto &vfs = *owner;
    auto blob = header(7);
    record(blob, "/tmp", {}, fs::mode_directory | 01777);
    record(blob, "/private", {}, fs::mode_directory | 0700);
    record(blob, "/zero", {}, fs::mode_directory);
    record(blob, "/group", {}, fs::mode_directory | 02777);
    record(blob, "/group/plain", {}, fs::mode_directory | 0750);
    record(blob, "/group/inherited", {}, fs::mode_directory | 02770);
    record(blob, "/group/program", {}, fs::mode_regular | 0755);
    CHECK(fs::mount_archive(vfs, blob));
    directory_mode(vfs, "/tmp", 01777);
    directory_mode(vfs, "/private", 0700);
    directory_mode(vfs, "/zero", 0);
    directory_mode(vfs, "/group", 02777);
    // Import final metadata instead of inheriting the parent's SGID bit.
    directory_mode(vfs, "/group/plain", 0750);
    directory_mode(vfs, "/group/inherited", 02770);
    auto program = vfs.stat("/group/program");
    CHECK(program && program.value().mode == (fs::mode_regular | 0755));
    CHECK(program.value().uid == 0 && program.value().gid == 0);
    CHECK(fs::mount_archive(vfs, blob));
}

void test_existing_directory_collisions()
{
    auto owner = std::make_unique<fs::VirtualFileSystem>();
    auto &vfs = *owner;
    auto old_tmp = header(1);
    record(old_tmp, "/tmp", {}, fs::mode_directory | 0777);
    CHECK(!fs::mount_archive(vfs, old_tmp));
    directory_mode(vfs, "/tmp", 01777);
    CHECK(vfs.create("/existing", fs::NodeType::directory));
    CHECK(vfs.import_archive_directory("/existing", 0755));
    CHECK(!vfs.import_archive_directory("/existing", 0700));
    directory_mode(vfs, "/existing", 0755);
    for (const auto [uid, gid] : std::array<std::array<u32, 2>, 2>{{{1000, 0}, {0, 1000}}})
    {
        CHECK(vfs.chown("/existing", uid, gid));
        CHECK(!vfs.import_archive_directory("/existing", 0755));
        CHECK(!vfs.import_archive_directory("/existing/child", 0755));
        auto unchanged = vfs.stat("/existing");
        CHECK(unchanged && unchanged.value().uid == uid && unchanged.value().gid == gid);
        CHECK(unchanged.value().mode == (fs::mode_directory | 0755));
        CHECK(vfs.lookup("/existing/child") == nullptr);
    }
    CHECK(vfs.chown("/existing", 0, 0));
    CHECK(vfs.create("/file", fs::NodeType::regular));
    const std::array<std::byte, 1> payload{std::byte{42}};
    CHECK(vfs.write_file("/file", payload));
    CHECK(!vfs.import_archive_directory("/file", 0755));
    CHECK(!vfs.import_archive_directory("/file/child", 0755));
    CHECK(vfs.read_file("/file").value().data[0] == payload[0]);
    CHECK(vfs.create({}, "/alias", fs::NodeType::symlink, 0777, 0, 0, "/existing"));
    CHECK(!vfs.import_archive_directory("/alias", 0755));
    CHECK(!vfs.import_archive_directory("/alias/child", 0755));
    CHECK(vfs.lookup("/existing/child") == nullptr);
    directory_mode(vfs, "/existing", 0755);
    CHECK(vfs.chown("/", 1000, 0));
    CHECK(!vfs.import_archive_directory("/root-owned", 0755));
    CHECK(vfs.lookup("/root-owned") == nullptr);
}

class ArchiveProvider final : public fs::Node
{
  public:
    mutable usize calls{}, reentrant_checks{};
    usize mutations{};
    fs::VirtualFileSystem *reenter{};
    std::string_view name() const override { ++calls; return "archive-provider"; }
    fs::Metadata metadata() const override
    {
        ++calls;
        if (reenter)
        {
            CHECK(reenter->import_archive_directory("/nested", 0755).code() == StatusCode::busy);
            ++reentrant_checks;
        }
        return {.type = fs::NodeType::directory, .mode = fs::mode_directory | 0755};
    }
    fs::Node *lookup(std::string_view) override { ++calls; return nullptr; }
    Result<fs::FileBuffer> read(usize, usize) const override { ++calls; return fs::FileBuffer{}; }
    Status write(usize, std::span<const std::byte>) override { ++calls; ++mutations; return {}; }
    Status create(std::string_view, fs::NodeType) override { ++calls; ++mutations; return {}; }
    Status chmod(u32) override { ++calls; ++mutations; return {}; }
    Status chown(u32, u32) override { ++calls; ++mutations; return {}; }
    Result<fs::DirectoryListing> list(fs::DirectoryReadIntent) const override
    { ++calls; return fs::DirectoryListing{}; }
};

void test_mounted_directory_boundary()
{
    ArchiveProvider provider;
    auto owner = std::make_unique<fs::VirtualFileSystem>();
    auto &vfs = *owner;
    CHECK(vfs.create("/mount", fs::NodeType::directory));
    CHECK(vfs.mount("/mount", provider));
    provider.calls = 0;
    CHECK(!vfs.import_archive_directory("/mount", 0755));
    CHECK(!vfs.import_archive_directory("/mount/child", 0755));
    CHECK(provider.calls == 0 && provider.mutations == 0);
    provider.reenter = &vfs;
    CHECK(vfs.stat("/mount"));
    CHECK(provider.reentrant_checks > 0 && provider.mutations == 0);
    CHECK(vfs.lookup("/nested") == nullptr);
    provider.reenter = nullptr;

    auto other = std::make_unique<fs::VirtualFileSystem>();
    CHECK(other->create("/cover", fs::NodeType::directory));
    auto overlay = other->mount_ram_owned("/cover");
    CHECK(overlay);
    CHECK(!other->import_archive_directory("/cover", 0755));
    CHECK(!other->import_archive_directory("/cover/new", 0755));
    CHECK(other->lookup("/cover/new") == nullptr);
    CHECK(other->detach(overlay.value()));
    CHECK(other->import_archive_directory("/cover/new", 0700));
    directory_mode(*other, "/cover/new", 0700);

    auto covered_root = std::make_unique<fs::VirtualFileSystem>();
    CHECK(covered_root->mount("/", provider));
    provider.calls = 0;
    CHECK(!covered_root->import_archive_directory("/child", 0755));
    CHECK(provider.calls == 0 && provider.mutations == 0);
}

void test_foreign_ram_directory_boundary()
{
    fs::RamNode foreign;
    CHECK(foreign.configure("foreign", fs::NodeType::directory, nullptr));
    auto owner = std::make_unique<fs::VirtualFileSystem>();
    auto *root = static_cast<fs::RamNode *>(owner->lookup("/"));
    CHECK(root && root->attach_child(foreign));
    CHECK(!owner->import_archive_directory("/foreign", 0755));
    CHECK(!owner->import_archive_directory("/foreign/new", 0755));
    CHECK(foreign.lookup("new") == nullptr);
    CHECK(foreign.metadata().mode == (fs::mode_directory | 0755));
}

void test_archive_mode_validation_precedes_import()
{
    for (const u32 mode : {fs::mode_directory | 04755, fs::mode_directory | 01000000000u,
                          fs::mode_regular | 01755, fs::mode_regular | 02755, fs::mode_regular | 04755,
                          fs::mode_regular | 07755, fs::mode_regular | 01000000000u})
    {
        auto owner = std::make_unique<fs::VirtualFileSystem>();
        auto blob = header(3);
        record(blob, "/accepted", {}, fs::mode_directory | 0755);
        record(blob, "/accepted/program", {}, fs::mode_regular | 0755);
        record(blob, "/invalid", {}, mode);
        CHECK(!fs::mount_archive(*owner, blob));
        CHECK(owner->lookup("/accepted") == nullptr);
        directory_mode(*owner, "/tmp", 01777);
    }
    auto owner = std::make_unique<fs::VirtualFileSystem>();
    for (const std::string &path : std::vector<std::string>{"relative", "/", "/bin/", "/../child", "/./child",
                                                          "/bin//child", "/" + std::string(32, 'a'),
                                                          std::string("/bin\0/child", 11)})
        CHECK(!owner->import_archive_directory(path, 0755));
    for (const u32 mode : {04000u, 010000u, 01000000000u})
        CHECK(!owner->import_archive_directory("/invalid", mode));
    CHECK(owner->lookup("/invalid") == nullptr);
}

int main()
{
    test_large_readonly_files();
    test_archive_does_not_use_writable_file_pool();
    test_malformed_archive();
    test_exact_directory_metadata();
    test_existing_directory_collisions();
    test_mounted_directory_boundary();
    test_foreign_ram_directory_boundary();
    test_archive_mode_validation_precedes_import();
    std::puts("PASS archive: immutable backing, bounded parsing, exact root directory metadata, protected collisions");
}
