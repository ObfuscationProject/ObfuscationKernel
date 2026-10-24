#include <ok/user/native_ppc32_elf_admission.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace ok;
namespace p = ok::user::ppc32_elf_admission;
using Bytes = std::vector<std::byte>;
constexpr auto profile = p::RuntimeProfile::e500v2_scalar_soft_float;
static unsigned long long checks = 0;

static void require(bool value, const char *name)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s check=%llu\n", name, checks); std::exit(1); }
}
static void put(Bytes &data, usize at, u64 value, unsigned bytes = 4)
{
    for (unsigned i = 0; i < bytes; ++i)
        data.at(at + i) = std::byte((value >> (8 * (bytes - i - 1))) & 255);
}
static u32 get(const Bytes &data, usize at)
{
    return p::detail::be32(data, at);
}
static Bytes read(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "input available");
    std::vector<char> raw((std::istreambuf_iterator<char>(file)), {});
    Bytes out(raw.size());
    for (usize i = 0; i != raw.size(); ++i) out[i] = std::byte(raw[i]);
    return out;
}
static p::Result admit(const Bytes &data, p::Personality personality = p::Personality::native_os)
{
    return p::admit(data, data.size(), personality, profile);
}
static void expect(const Bytes &data, p::Error error, const char *name)
{
    const auto result = admit(data);
    require(result.error == error, name);
    require(result.required_prefix_bytes == 0, "non-prefix error has no request");
}
static Bytes fixture(const std::vector<u8> &tags = {4, 2})
{
    Bytes data(1024);
    put(data, 0, 0x7f454c46); put(data, 4, 0x01020100);
    put(data, 16, 2, 2); put(data, 18, 20, 2); put(data, 20, 1);
    put(data, 24, 0x40000000); put(data, 28, 52); put(data, 32, 148);
    put(data, 40, 52, 2); put(data, 42, 32, 2); put(data, 44, 3, 2);
    put(data, 46, 40, 2); put(data, 48, 4, 2);
    put(data, 52, 1); put(data, 56, 768); put(data, 68, 4); put(data, 72, 4);
    put(data, 76, 5); put(data, 80, 4);
    put(data, 188 + 4, 0x6ffffff5); put(data, 188 + 16, 512);
    put(data, 188 + 20, 14 + tags.size()); put(data, 188 + 32, 1);
    put(data, 512, 'A', 1); put(data, 513, 13 + tags.size());
    put(data, 517, 0x676e7500); put(data, 521, 1, 1);
    put(data, 522, 5 + tags.size());
    for (usize i = 0; i < tags.size(); ++i) put(data, 526 + i, tags[i], 1);
    return data;
}
static void edit(usize at, u64 value, unsigned width, p::Error error, const char *name)
{
    auto data = fixture(); put(data, at, value, width); expect(data, error, name);
}

static void headers()
{
    require(bool(admit(fixture())), "native valid soft float");
    auto data = fixture(); data[7] = std::byte{3};
    require(bool(admit(data, p::Personality::linux_user)), "linux exact ABI0");
    expect(data, p::Error::unsupported_osabi, "native rejects Linux OSABI");
    require(admit(fixture(), p::Personality::linux_user).error == p::Error::unsupported_osabi,
            "linux rejects native OSABI");
    for (unsigned v = 0; v != 256; ++v)
    {
        data = fixture(); data[7] = std::byte(v);
        require(admit(data).ok() == (v == 0), "all native OSABIs");
        require(admit(data, p::Personality::linux_user).ok() == (v == 3), "all Linux OSABIs");
        data[7] = std::byte{0}; data[8] = std::byte(v);
        require(admit(data).ok() == (v == 0), "all native ABI versions");
        data[7] = std::byte{3};
        require(admit(data, p::Personality::linux_user).ok() == (v == 0), "all Linux ABI versions");
    }
    require(p::admit(data, data.size(), static_cast<p::Personality>(254), profile).error ==
            p::Error::unsupported_personality, "invalid personality");
    require(p::admit(data, data.size(), p::Personality::native_os,
                    static_cast<p::RuntimeProfile>(254)).error ==
            p::Error::unsupported_profile, "invalid profile");
    for (usize offset = 0; offset != 16; ++offset)
    {
        if (offset == 7 || offset == 8) continue;
        data = fixture(); data[offset] ^= std::byte{0x10};
        expect(data, p::Error::invalid_ident, "ELF ident and padding");
    }
    edit(4, 2, 1, p::Error::invalid_ident, "ELF64 rejects");
    edit(5, 1, 1, p::Error::invalid_ident, "little endian rejects");
    edit(16, 3, 2, p::Error::unsupported_type, "ET_DYN rejects");
    edit(18, 21, 2, p::Error::unsupported_machine, "PPC64 machine rejects");
    edit(20, 2, 4, p::Error::unsupported_version, "ELF version rejects");
    edit(40, 64, 2, p::Error::unsupported_header_size, "header size");
    for (unsigned bit = 0; bit != 32; ++bit)
        edit(36, u32{1} << bit, 4, p::Error::unsupported_flags, "all e_flags bits");
    edit(42, 31, 2, p::Error::unsupported_program_entry_size, "ph entry size");
    edit(44, 0, 2, p::Error::unsupported_program_count, "no programs");
    edit(44, 0xffff, 2, p::Error::unsupported_program_count, "extended programs");
    edit(28, 51, 4, p::Error::overlapping_program_table, "program overlaps header");
    edit(28, 1000, 4, p::Error::program_table_outside_file, "program exceeds file");
    edit(28, 0xfffffff0, 4, p::Error::program_table_outside_file, "program overflow32");
    for (usize ph : {52U, 84U, 116U})
    {
        edit(ph, 2, 4, p::Error::dynamic_image, "dynamic at every position");
        edit(ph, 3, 4, p::Error::dynamic_image, "interp at every position");
    }
    edit(56, 1025, 4, p::Error::segment_outside_file, "segment beyond file");
    edit(68, 0xffffffff, 4, p::Error::segment_outside_file, "segment overflow32");
    data = fixture(); put(data, 88, 0xffffffff); put(data, 100, 0xffffffff);
    require(bool(admit(data)), "PT_NULL payload ignored");
    edit(46, 39, 2, p::Error::unsupported_section_entry_size, "section size");
    for (u16 count : {u16{0}, u16{0xff00}, u16{0xffff}})
        edit(48, count, 2, p::Error::unsupported_section_count, "section counts");
    edit(50, 4, 2, p::Error::unsupported_section_names, "section name index");
    edit(50, 0xffff, 2, p::Error::unsupported_section_names, "extended section names");
    edit(32, 1, 4, p::Error::overlapping_section_table, "section overlaps header");
    edit(32, 84, 4, p::Error::overlapping_section_table, "table overlap");
    edit(32, 1000, 4, p::Error::section_table_outside_file, "sections beyond file");
    edit(32, 0xfffffff0, 4, p::Error::section_table_outside_file, "sections overflow32");
    for (usize i = 148; i != 188; ++i)
        edit(i, 1, 1, p::Error::invalid_null_section, "all null section bytes");
    edit(204, 1025, 4, p::Error::section_outside_file, "attribute exceeds file");
    edit(208, 0xffffffff, 4, p::Error::section_outside_file, "attribute overflow32");
    data = fixture(); put(data, 232, 1); put(data, 244, 1025);
    expect(data, p::Error::section_outside_file, "later section range checked");
    put(data, 232, 8); put(data, 248, 0xffffffff);
    require(bool(admit(data)), "NOBITS has no file payload");
    data = fixture(); put(data, 50, 1, 2);
    expect(data, p::Error::invalid_section_names, "shstrtab must be STRTAB");
    for (usize sh : {188U, 228U, 268U})
        edit(sh + 4, 6, 4, p::Error::dynamic_image, "dynamic section any position");
    data = fixture(); put(data, 232, 0x6ffffff5);
    expect(data, p::Error::duplicate_attributes, "duplicate attribute sections");
    for (usize field : {196U, 200U, 212U, 216U, 224U})
        edit(field, 1, 4, p::Error::invalid_attribute_section, "attribute descriptor flags");
    edit(220, 2, 4, p::Error::invalid_attribute_section, "attribute alignment");
    edit(208, 0, 4, p::Error::invalid_attribute_section, "empty attribute section");
    for (usize at : {0U, 52U, 147U, 148U, 307U})
        edit(204, at, 4, p::Error::invalid_attribute_section, "attribute table overlap");
    data = fixture(); put(data, 32, 0); put(data, 48, 0, 2);
    require(bool(admit(data)), "sectionless 40-byte shentsize");
    put(data, 46, 0, 2); require(bool(admit(data)), "sectionless zero shentsize");
    put(data, 46, 1, 2); expect(data, p::Error::unsupported_section_entry_size, "sectionless bad size");
    put(data, 46, 0, 2); put(data, 50, 1, 2);
    expect(data, p::Error::unsupported_section_names, "sectionless names");
    put(data, 50, 0, 2); put(data, 48, 1, 2);
    expect(data, p::Error::unsupported_section_count, "sectionless count");
    data = fixture(); put(data, 192, 1);
    require(bool(admit(data)), "absent attributes under explicit profile");
}

static void attributes()
{
    for (unsigned value = 0; value != 128; ++value)
    {
        const auto fp = admit(fixture({4, static_cast<u8>(value)}));
        require(fp.ok() == (value == 2 || value == 10), "FP ABI exhaustive one-byte values");
        const auto vec = admit(fixture({8, static_cast<u8>(value)}));
        require(vec.ok() == (value == 1), "vector ABI exhaustive one-byte values");
        if (!fp) require(fp.error == p::Error::unsupported_fp_abi, "FP error type");
        if (!vec) require(vec.error == p::Error::unsupported_vector_abi, "vector error type");
    }
    require(bool(admit(fixture({}))), "empty file attributes allowed");
    require(bool(admit(fixture({4, 10, 8, 1}))), "soft64 generic vector");
    require(bool(admit(fixture({8, 1, 4, 2}))), "attribute order independent");
    expect(fixture({4, 2, 4, 2}), p::Error::duplicate_attribute, "same FP duplicated");
    expect(fixture({8, 1, 8, 1}), p::Error::duplicate_attribute, "same vector duplicated");
    expect(fixture({4, 2, 4, 1}), p::Error::duplicate_attribute, "conflicting FP duplicate");
    expect(fixture({12, 1}), p::Error::unknown_attribute, "unsupported struct return tag");
    expect(fixture({5, 'x', 0}), p::Error::unknown_attribute, "unknown string attribute");
    expect(fixture({4}), p::Error::invalid_uleb, "missing attribute value");
    expect(fixture({0x84, 0, 2}), p::Error::invalid_uleb, "overlong tag");
    expect(fixture({4, 0x82, 0}), p::Error::invalid_uleb, "overlong value");
    expect(fixture({4, 0xff, 0xff, 0xff, 0xff, 0x10}), p::Error::invalid_uleb, "32-bit ULEB overflow");
    expect(fixture({4, 0xff, 0xff, 0xff, 0xff, 0x8f}), p::Error::invalid_uleb, "ULEB continuation overflow");
    expect(fixture({4, 0xff, 0xff, 0xff, 0xff, 0x0f}), p::Error::unsupported_fp_abi, "max ULEB known value");
    expect(fixture({0xff, 0xff, 0xff, 0xff, 0x0f, 2}), p::Error::unknown_attribute, "max ULEB tag");
    edit(512, 'B', 1, p::Error::invalid_attribute_format, "attribute format");
    edit(208, 1, 4, p::Error::invalid_vendor_record, "only format byte");
    for (u32 size : {0U, 8U, 16U, 0xffffffffU})
        edit(513, size, 4, p::Error::invalid_vendor_record, "vendor bounds");
    edit(517, 'x', 1, p::Error::unsupported_vendor, "unknown vendor");
    auto data = fixture(); for (usize i = 517; i != 528; ++i) data[i] = std::byte{1};
    expect(data, p::Error::invalid_vendor_record, "unterminated vendor");
    for (u8 scope : {u8{0}, u8{2}, u8{3}, u8{4}})
        edit(521, scope, 1, p::Error::unsupported_attribute_scope, "scoped attributes reject");
    for (u32 size : {0U, 4U, 8U, 0xffffffffU})
        edit(522, size, 4, p::Error::invalid_file_record, "file record bounds");
    data = fixture(); put(data, 208, 9); put(data, 513, 8);
    expect(data, p::Error::invalid_vendor_record, "vendor without file");
    data = fixture(); put(data, 208, 31);
    for (usize i = 0; i != 15; ++i) data[528 + i] = data[513 + i];
    expect(data, p::Error::duplicate_vendor, "duplicate vendor");
    data = fixture(); put(data, 208, 23); put(data, 513, 22);
    for (usize i = 0; i != 7; ++i) data[528 + i] = data[521 + i];
    expect(data, p::Error::duplicate_file_record, "duplicate file record");
    // Structural edits never permit bytes beyond the declared vendor/record.
    for (usize at = 512; at != 528; ++at)
        for (unsigned value = 0; value != 256; ++value)
        {
            data = fixture(); data[at] = std::byte(value);
            const auto result = admit(data);
            require(result.required_prefix_bytes == 0, "bounded attribute single-byte sweep");
        }
}

static void prefixes()
{
    const auto data = fixture();
    for (usize length = 0; length <= data.size(); ++length)
    {
        const auto result = p::admit(std::span(data).first(length), data.size(),
                                     p::Personality::native_os, profile);
        const u64 needed = length < 52 ? 52 : length < 148 ? 148 :
                           length < 308 ? 308 : length < 528 ? 528 : 0;
        require(result.required_prefix_bytes == needed, "every synthetic prefix exact next step");
        require(result.ok() == (needed == 0), "every synthetic prefix status");
    }
    for (usize length = 0; length != data.size(); ++length)
    {
        const auto result = p::admit(data, length, p::Personality::native_os, profile);
        require(result.error == p::Error::prefix_exceeds_file, "prefix exceeds file");
        require(result.required_prefix_bytes == 0, "no request when prefix exceeds file");
    }
    for (usize length = 0; length != 52; ++length)
        require(p::admit(std::span(data).first(length), length, p::Personality::native_os,
                         profile).error == p::Error::header_outside_file, "file shorter than header");
    auto high = fixture(); put(high, 28, 0xfffffff0);
    auto result = p::admit(high, 0x100000100ULL, p::Personality::native_os, profile);
    require(result.error == p::Error::program_table_outside_prefix &&
            result.required_prefix_bytes == 0x100000050ULL, "u64 program endpoint beyond4G");
    high = fixture(); put(high, 32, 0xfffffff0);
    result = p::admit(high, 0x100000100ULL, p::Personality::native_os, profile);
    require(result.error == p::Error::section_table_outside_prefix &&
            result.required_prefix_bytes == 0x100000090ULL, "u64 section endpoint beyond4G");
    high = fixture(); put(high, 204, 0xfffffff8);
    result = p::admit(high, 0x100000100ULL, p::Personality::native_os, profile);
    require(result.error == p::Error::attributes_outside_prefix &&
            result.required_prefix_bytes == 0x100000008ULL, "u64 attribute endpoint beyond4G");
    // Table-wide errors take precedence over a request for the first payload.
    put(high, 232, 6);
    result = p::admit(std::span(high).first(308), 0x100000100ULL,
                      p::Personality::native_os, profile);
    require(result.error == p::Error::dynamic_image && result.required_prefix_bytes == 0,
            "later dynamic section before attribute refill");
    put(high, 232, 0x6ffffff5);
    result = p::admit(std::span(high).first(308), 0x100000100ULL,
                      p::Personality::native_os, profile);
    require(result.error == p::Error::duplicate_attributes, "later duplicate before attribute refill");
}

static void artifacts(const std::string &root)
{
    for (const char *name : {"native-init", "static-musl-futex", "static-musl-masks", "musl-build-probe"})
    {
        const auto data = read(root + "/" + name + ".elf");
        const auto personality = data[7] == std::byte{3} ? p::Personality::linux_user :
                                                        p::Personality::native_os;
        require(bool(admit(data, personality)), "copied actual artifact accepted by matching personality");
        const u64 ph_end = get(data, 28) + u64{p::detail::be16(data, 44)} * 32;
        const u64 sh_end = get(data, 32) + u64{p::detail::be16(data, 48)} * 40;
        // All copied artifacts carry section tables after their attribute data.
        for (usize length = 0; length <= data.size(); ++length)
        {
            const auto result = p::admit(std::span(data).first(length), data.size(), personality, profile);
            const u64 needed = length < 52 ? 52 : length < ph_end ? ph_end :
                               length < sh_end ? sh_end : 0;
            require(result.required_prefix_bytes == needed, "actual every-prefix next extent");
            require(result.ok() == (needed == 0), "actual every-prefix acceptance");
        }
        auto edited = data; put(edited, 36, 0x80000000);
        require(admit(edited, personality).error == p::Error::unsupported_flags,
                "actual artifact incompatible flag");
        std::printf("actual=%s bytes=%zu metadata=pass personality=%u\n", name, data.size(),
                    static_cast<unsigned>(personality));
    }
}

static void fuzz()
{
    u32 state = 0x72894631;
    auto random = [&]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
    for (unsigned iteration = 0; iteration != 30000; ++iteration)
    {
        auto data = fixture();
        const unsigned changes = random() % 8 + 1;
        for (unsigned i = 0; i != changes; ++i) data[random() % data.size()] = std::byte(random() & 255);
        const usize length = random() % (data.size() + 1);
        const u64 file_bytes = (iteration & 1) ? data.size() : random();
        const auto result = p::admit(std::span(data).first(length), file_bytes,
                                     p::Personality::native_os, profile);
        require(result.required_prefix_bytes == 0 ||
                (result.required_prefix_bytes > length && result.required_prefix_bytes <= file_bytes),
                "fuzz request range invariant");
        require(!result.ok() || result.required_prefix_bytes == 0, "fuzz success request invariant");
    }
}

int main(int argc, char **argv)
{
    require(argc == 2, "artifact directory argument");
    headers(); attributes(); prefixes(); artifacts(argv[1]); fuzz();
    std::printf("PPC32_ELF_METADATA_MODEL_PASS checks=%llu (hosted; no execution or readiness)\n", checks);
}
