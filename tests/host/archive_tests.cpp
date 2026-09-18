#include "ok/fs/archive.hpp"
#include <cstdio>
#include <cstdlib>
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
int main()
{
    test_large_readonly_files();
    test_archive_does_not_use_writable_file_pool();
    test_malformed_archive();
    std::puts("PASS archive: large reads, immutable backing, file-pool independence, malformed input rejection");
}
