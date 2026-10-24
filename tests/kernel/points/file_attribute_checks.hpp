#pragma once
#include "ok/fs/ext4_times.hpp"
#include "ok/posix/native_stat.hpp"

namespace ok::tests
{
inline Status check_native_stat_projection()
{
    static_assert(OK_SYS_FSTAT_V2 == 1026 && OK_SYS_NEWFSTATAT_V2 == 1027);
    static_assert(offsetof(ok_stat_v2, basic) == 0);
    static_assert(offsetof(ok_stat_v2, inode) >= sizeof(ok_stat));
    posix::FileStatus status{.type = fs::NodeType::regular, .size = 17, .mode = 0100640,
                             .uid = 1000, .gid = 1001, .link_count = 2,
                             .block_size = 4096, .blocks = 8};
    status.attributes.inode = 0xfedcba9876543210ULL;
    const auto legacy = posix::native_file_status(status);
    const auto missing = posix::native_file_status_v2(status);
    std::array<bool, sizeof(ok_stat_v2)> fields{};
    auto mark = [&](usize offset, usize size) {
        for (usize i = 0; i < size; ++i) fields[offset + i] = true;
    };
    mark(offsetof(ok_stat, type), sizeof(ok_stat::type));
    mark(offsetof(ok_stat, size), sizeof(ok_stat::size));
    mark(offsetof(ok_stat, mode), sizeof(ok_stat::mode));
    mark(offsetof(ok_stat, uid), sizeof(ok_stat::uid));
    mark(offsetof(ok_stat, gid), sizeof(ok_stat::gid));
    mark(offsetof(ok_stat, link_count), sizeof(ok_stat::link_count));
    mark(offsetof(ok_stat, block_size), sizeof(ok_stat::block_size));
    mark(offsetof(ok_stat, blocks), sizeof(ok_stat::blocks));
    mark(offsetof(ok_stat_v2, inode), sizeof(ok_stat_v2::inode));
    for (usize i = 0; i < legacy.size(); ++i)
        if (missing[i] != legacy[i]) return Status::fault("native stat v2 changed legacy prefix");
    for (usize i = 0; i < missing.size(); ++i)
        if ((!fields[i] || i >= offsetof(ok_stat_v2, inode)) && missing[i] != std::byte{0})
            return Status::fault("native stat v2 leaked padding or unadvertised inode");
    status.attributes.mark(fs::Attribute::inode);
    const auto present = posix::native_file_status_v2(status);
    const auto high = std::bit_cast<std::array<std::byte, sizeof(u64)>>(status.attributes.inode);
    for (usize i = 0; i < legacy.size(); ++i)
        if (present[i] != legacy[i]) return Status::fault("native stat v2 changed legacy fields");
    for (usize i = 0; i < present.size(); ++i)
        if (!fields[i] && present[i] != std::byte{0})
            return Status::fault("native stat v2 padding was not zero");
    for (usize i = 0; i < high.size(); ++i)
        if (present[offsetof(ok_stat_v2, inode) + i] != high[i])
            return Status::fault("native stat v2 lost high inode identity");
    std::array<std::byte, sizeof(ok_stat_v2) + 2> bounded{};
    for (auto &byte : bounded) byte = std::byte{0xa5};
    posix::write_native_file_status(bounded.data() + 1, status);
    if (bounded.front() != std::byte{0xa5} || bounded[1 + sizeof(ok_stat)] != std::byte{0xa5})
        return Status::fault("legacy native stat overran output size");
    for (usize i = 0; i < legacy.size(); ++i)
        if (bounded[1 + i] != legacy[i]) return Status::fault("legacy native stat bytes changed");
    for (auto &byte : bounded) byte = std::byte{0xa5};
    posix::write_native_file_status_v2(bounded.data() + 1, status);
    if (bounded.front() != std::byte{0xa5} || bounded.back() != std::byte{0xa5})
        return Status::fault("native stat v2 overran output size");
    for (usize i = 0; i < present.size(); ++i)
        if (bounded[1 + i] != present[i]) return Status::fault("native stat v2 bytes changed");
    return {};
}

inline Status check_file_attributes()
{
    fs::IdentitySequence<2> sequence;
    auto one = sequence.next(), two = sequence.next();
    if (!one || !two || one.value() != 1 || two.value() != 2 ||
        sequence.next().status().code() != StatusCode::overflow || sequence.next())
        return Status::fault("identity exhaustion reused a number");
    int a{}, b{}, c{};
    fs::FilesystemIdentities<3, 2> devices;
    auto first = devices.admit(&a), alias = devices.admit(&a), second = devices.admit(&b);
    if (!first || !alias || !second || first.value() != alias.value() || first.value() == second.value() ||
        first.value().major != 0 || first.value().minor == 0 || devices.admit(&c).status().code() != StatusCode::overflow ||
        devices.find(&c) || devices.admit(nullptr) || devices.find(nullptr) || devices.find(&a).value() != first.value())
        return Status::fault("filesystem registration identity failed");
    fs::FilesystemIdentities<1> bounded;
    if (!bounded.admit(&a) || bounded.admit(&b).status().code() != StatusCode::no_memory || !bounded.admit(&a))
        return Status::fault("filesystem capacity changed prior identity");
    std::array<std::byte, 256> inode{};
    auto put = [&](usize at, u32 value) {
        for (usize i = 0; i < 4; ++i) inode[at + i] = std::byte((value >> (8 * i)) & 255);
    };
    put(8, 0xffffffff); put(12, 0x80000000); put(16, 0);
    fs::FileAttributes output;
    output.inode = 123; output.mark(fs::Attribute::inode);
    if (!fs::decode_ext4_times(std::span<const std::byte>{inode}.first(128), output) ||
        output.access.seconds != -1 || output.change.seconds != -2147483648LL || output.modification.seconds != 0 ||
        output.access.nanoseconds != 0 || output.access.source != fs::TimestampSource::readonly_ext4 ||
        !output.has(fs::Attribute::modification_time) || output.inode != 123)
        return Status::fault("ext4 base epoch decoding failed");
    // Every epoch extension and each independent extra-field boundary.
    for (u32 epoch = 0; epoch < 4; ++epoch)
        for (u32 size : {0u, 4u, 8u, 12u, 16u, 128u})
        {
            put(128, size); put(132, (999999999u << 2) | epoch);
            put(136, (123456789u << 2) | epoch); put(140, (1u << 2) | epoch);
            if (!fs::decode_ext4_times(inode, output) ||
                output.change.seconds != -2147483648LL + (size >= 8 ? static_cast<i64>(epoch) * 4294967296LL : 0) ||
                output.modification.seconds != (size >= 12 ? static_cast<i64>(epoch) * 4294967296LL : 0) ||
                output.access.seconds != -1 + (size >= 16 ? static_cast<i64>(epoch) * 4294967296LL : 0) ||
                output.change.nanoseconds != (size >= 8 ? 999999999u : 0) ||
                output.modification.nanoseconds != (size >= 12 ? 123456789u : 0) ||
                output.access.nanoseconds != (size >= 16 ? 1u : 0))
                return Status::fault("ext4 extra epoch boundary failed");
        }
    const auto saved = output;
    auto unchanged = [&] {
        return output.valid == saved.valid && output.inode == saved.inode &&
            output.access.seconds == saved.access.seconds && output.access.nanoseconds == saved.access.nanoseconds &&
            output.modification.seconds == saved.modification.seconds && output.modification.nanoseconds == saved.modification.nanoseconds &&
            output.change.seconds == saved.change.seconds && output.change.nanoseconds == saved.change.nanoseconds;
    };
    for (usize field : {usize{132}, usize{136}, usize{140}})
    {
        put(field, 1000000000u << 2);
        if (fs::decode_ext4_times(inode, output) || !unchanged()) return Status::fault("invalid ext4 time modified output");
        put(field, 0);
    }
    for (u32 size : {1u, 2u, 3u, 129u, 132u, 65535u})
    {
        put(128, size);
        if (fs::decode_ext4_times(inode, output) || !unchanged()) return Status::fault("invalid ext4 extent modified output");
    }
    for (usize size : {usize{0}, usize{127}, usize{129}, usize{255}})
        if (fs::decode_ext4_times(std::span<const std::byte>{inode}.first(size), output) || !unchanged())
            return Status::fault("unsupported ext4 inode modified output");
    return check_native_stat_projection();
}
} // namespace ok::tests
