#pragma once
#include "ok/abi/linux_directory.hpp"
#include "ok/abi/linux_native.hpp"
#include "../../fixtures/ext4_rename_relocation_fixture.hpp"

namespace ok::tests
{
template <abi::linux_user::Profile P>
Status check_directory_codec()
{
    fs::DirectoryEntry entry{};
    if (!entry.name.assign("x")) return Status::fault("directory golden name");
    entry.metadata.type = fs::NodeType::regular;
    entry.metadata.attributes.inode = 0x1122334455667788ULL;
    entry.metadata.attributes.mark(fs::Attribute::inode);
    std::array<u8, 32> bytes{}; bytes.fill(0xa5);
    auto result = abi::linux_user::DirectoryCodec<P>::encode(entry, 0x0102030405060708ULL, bytes);
    if (!result || result.value() != 24) return Status::fault("directory golden size");
    constexpr std::array<u8, 24> little{
        0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x08,0x07,0x06,0x05,0x04,0x03,0x02,0x01,
        0x18,0,8,'x',0,0,0,0};
    constexpr std::array<u8, 24> big{
        0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0,0x18,8,'x',0,0,0,0};
    const auto &golden = abi::linux_user::Codec<P>::profile.big_endian ? big : little;
    for (usize i = 0; i < 24; ++i)
        if (bytes[i] != golden[i]) return Status::fault("directory endian wire mismatch");
    for (usize i = 24; i < bytes.size(); ++i)
        if (bytes[i] != 0xa5) return Status::fault("directory output extent mismatch");
    bytes.fill(0xa5);
    if (abi::linux_user::DirectoryCodec<P>::encode(entry, 1, std::span{bytes}.first(23)).status().code() !=
        StatusCode::invalid_argument) return Status::fault("directory short record accepted");
    for (u8 value : bytes)
        if (value != 0xa5) return Status::fault("directory short record mutated");
    testing::rename_fixture::Fixture rename;
    const auto free = rename.plan();
    if (!free || free.value().before != rename.image ||
        testing::rename_fixture::find(free.value().after, "notes.txt") != testing::rename_fixture::tail ||
        testing::rename_fixture::find(free.value().after, "saved-note.txt") != 44 ||
        fs::ext4_checked::le32(free.value().after, 44) != 12)
        return Status::fault("directory free-record relocation model");
    testing::rename_fixture::entry(rename.image, 44, 968, 13, 1, "keeper"); rename.seal();
    const auto slack = rename.plan();
    if (!slack || fs::ext4_checked::le16(slack.value().after, 48) != 16 ||
        testing::rename_fixture::find(slack.value().after, "keeper") != 44 ||
        testing::rename_fixture::find(slack.value().after, "saved-note.txt") != 60)
        return Status::fault("directory occupied-slack relocation model");
    return {};
}
} // namespace ok::tests
