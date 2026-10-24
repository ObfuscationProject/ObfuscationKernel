#pragma once
#include "ext4_owned_support.hpp"

namespace relocation_fixture
{
using namespace ok;
using namespace fixture;
inline Bytes with_notes_source(Bytes image)
{
    const usize block = r32(image, inode_offset(image, 2) + 60) * 1024;
    std::array<u8, 1024> directory{};
    usize next{}, last{};
    for (usize offset = 0; offset < 1012; offset += r16(image, block + offset + 4))
    {
        const usize at = block + offset;
        if (!r32(image, at)) continue;
        std::string_view name{reinterpret_cast<const char *>(image.data() + at + 8), image[at + 6]};
        if (name == "seed") name = "notes.txt";
        const usize minimum = (8 + name.size() + 3) & ~usize{3};
        fixture::check(next + minimum <= 1012, "notes fixture directory capacity");
        last = next;
        const u32 number = r32(image, at);
        for (usize byte = 0; byte < 4; ++byte) directory[next + byte] = u8(number >> (byte * 8));
        directory[next + 4] = u8(minimum); directory[next + 5] = u8(minimum >> 8);
        directory[next + 6] = u8(name.size()); directory[next + 7] = image[at + 7];
        for (usize byte = 0; byte < name.size(); ++byte) directory[next + 8 + byte] = u8(name[byte]);
        next += minimum;
    }
    fixture::check(next && 1012 - next >= 24, "notes fixture requires slack");
    directory[last + 4] = u8(1012 - last); directory[last + 5] = u8((1012 - last) >> 8);
    std::copy(directory.begin(), directory.begin() + 1012, image.begin() + block);
    w32(image, block + 1020, crc(inode_seed(image, 2, inode_offset(image, 2)), image, block, 1012));
    return image;
}
inline Bytes with_free_record(Bytes image)
{
    const usize block = r32(image, inode_offset(image, 2) + 60) * 1024;
    for (usize offset = 0; offset < 1012; offset += r16(image, block + offset + 4))
    {
        const usize at = block + offset, record = r16(image, at + 4), length = image[at + 6];
        const usize minimum = (8 + length + 3) & ~usize{3};
        if (!r32(image, at) || record - minimum < 40) continue;
        w16(image, at + 4, u16(minimum));
        std::fill(image.begin() + at + minimum, image.begin() + at + record, 0);
        w16(image, at + minimum + 4, u16(record - minimum));
        w32(image, block + 1020, crc(inode_seed(image, 2, inode_offset(image, 2)), image, block, 1012));
        return image;
    }
    fixture::check(false, "relocation fixture requires one occupied slack donor");
    return image;
}
} // namespace relocation_fixture
