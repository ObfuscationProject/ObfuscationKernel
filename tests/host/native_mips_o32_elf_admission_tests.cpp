#include <ok/user/native_mips_o32_elf_admission.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

using namespace ok;
namespace E = ok::user::mips_o32_elf_admission;
using Bytes = std::vector<std::byte>;
static usize checks;
static void check(bool pass, const char* label)
{
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL %s at assertion %zu\n", label, checks); std::exit(1); }
}
static void put(Bytes& b, usize off, u32 value, usize width)
{
    for (usize i = 0; i < width; ++i)
        b.at(off + i) = std::byte((value >> ((width - i - 1) * 8)) & 255);
}
static Bytes fixture()
{
    Bytes b(256);
    put(b, 0, 0x7f454c46, 4); put(b, 4, 1, 1); put(b, 5, 2, 1); put(b, 6, 1, 1);
    put(b, 16, 2, 2); put(b, 18, 8, 2); put(b, 20, 1, 4); put(b, 28, 52, 4);
    put(b, 36, 0x70001001, 4); put(b, 40, 52, 2); put(b, 42, 32, 2); put(b, 44, 2, 2);
    put(b, 52, 0x70000003, 4); put(b, 56, 128, 4); put(b, 68, 24, 4); put(b, 72, 24, 4);
    put(b, 84, 1, 4); put(b, 88, 0, 4); put(b, 100, 256, 4); put(b, 104, 4096, 4);
    put(b, 130, 32, 1); put(b, 131, 2, 1); put(b, 132, 1, 1); put(b, 133, 1, 1);
    put(b, 135, 1, 1); put(b, 144, 1, 4);
    return b;
}
static void error(const Bytes& b, E::Error expected, const char* label)
{
    const auto r = E::admit(b, b.size());
    check(r.error == expected && r.required_prefix_bytes == 0, label);
}
static void changed(usize offset, u32 value, usize width, E::Error expected, const char* label)
{
    auto b = fixture(); put(b, offset, value, width); error(b, expected, label);
}
static void structural()
{
    auto b = fixture(); check(E::admit(b, b.size()).ok(), "baseline metadata");
    check(E::admit(b, b.size() - 1).error == E::Error::prefix_exceeds_file, "prefix exceeds file");
    check(E::admit({}, 0).error == E::Error::header_outside_file, "empty file");
    for (usize n = 0; n <= b.size(); ++n)
    {
        Bytes short_prefix(b.begin(), b.begin() + n);
        auto r = E::admit(short_prefix, b.size());
        const auto want = n < 52 ? E::Error::header_outside_prefix :
                          n < 116 ? E::Error::program_table_outside_prefix :
                          n < 152 ? E::Error::abi_record_outside_prefix : E::Error::none;
        check(r.error == want, "every prefix boundary");
        check(r.required_prefix_bytes == (n < 52 ? 52 : n < 116 ? 116 : n < 152 ? 152 : 0),
              "exact next bounded read");
        r = E::admit(short_prefix, n);
        check(r.ok() == (n == 256), "every actual file truncation including unrelated LOAD");
    }
    for (usize off = 0; off < 7; ++off) changed(off, 255, 1, E::Error::invalid_ident, "ident");
    changed(16, 3, 2, E::Error::unsupported_type, "ET_DYN rejected");
    changed(18, 10, 2, E::Error::unsupported_machine, "machine");
    changed(20, 0, 4, E::Error::unsupported_version, "ELF version");
    changed(40, 51, 2, E::Error::unsupported_header_size, "header size");
    changed(42, 31, 2, E::Error::unsupported_program_entry_size, "phdr stride");
    changed(44, 0, 2, E::Error::unsupported_program_count, "missing phdrs");
    changed(44, 0xffff, 2, E::Error::unsupported_program_count, "extended numbering");
    changed(28, 51, 4, E::Error::overlapping_program_table, "overlapping table");
    changed(28, 0xfffffff0, 4, E::Error::program_table_outside_file, "table addition wrap");
    changed(44, 0xfffe, 2, E::Error::program_table_outside_file, "large phnum");
    changed(52, 0, 4, E::Error::missing_abi_flags, "PT_NULL is not ABIflags");
    changed(84, 0x70000003, 4, E::Error::duplicate_abi_flags, "duplicate before record read");
    changed(84, 2, 4, E::Error::dynamic_image, "late PT_DYNAMIC");
    changed(84, 3, 4, E::Error::dynamic_image, "late PT_INTERP");
    changed(56, 0xfffffff0, 4, E::Error::segment_outside_file, "record addition wrap");
    changed(88, 0xffffffff, 4, E::Error::segment_outside_file, "other segment offset wrap");
    changed(100, 0xffffffff, 4, E::Error::segment_outside_file, "other segment size wrap");
    for (u32 size : {0u, 23u, 25u})
    {
        changed(68, size, 4, E::Error::unsupported_abi_record_size, "exact record filesize");
        changed(72, size, 4, E::Error::unsupported_abi_record_size, "exact record memsize");
    }
    for (u32 low = 0; low < 8; ++low)
    {
        put(b, 36, 0x70001000 | low, 4); check(E::admit(b, b.size()).ok(), "all allowed low flags");
    }
    for (unsigned bit = 3; bit < 32; ++bit)
        changed(36, 0x70001000 ^ (u32{1} << bit), 4, E::Error::unsupported_flags, "every forbidden flag bit");
    // These identities belong to the personality loader, not this metadata gate.
    b = fixture(); put(b, 7, 3, 1); put(b, 8, 1, 1);
    check(E::admit(b, b.size()).ok(), "Linux OSABI accepted for separate personality admission");
    b = fixture(); put(b, 84, 0, 4); put(b, 88, 0xffffffff, 4); put(b, 100, 0xffffffff, 4);
    check(E::admit(b, b.size()).ok(), "undefined PT_NULL payload ignored");
    // Complete-table validation must precede a retry for an absent ABI payload.
    b = fixture(); put(b, 84, 3, 4);
    check(E::admit(std::span(b).first(116), b.size()).error == E::Error::dynamic_image, "no early ABI success/retry");
    b = fixture(); put(b, 84, 0x70000003, 4);
    check(E::admit(std::span(b).first(116), b.size()).error == E::Error::duplicate_abi_flags, "no hidden duplicate");
    // A huge declared file is not a huge prefix, and may never authorize a read.
    b = fixture(); put(b, 56, 0xfffffff0, 4);
    auto r = E::admit(b, std::numeric_limits<usize>::max());
    if constexpr (sizeof(usize) == 8)
        check(r.error == E::Error::abi_record_outside_prefix && r.required_prefix_bytes == 0x100000008ULL,
              "host wide file extent remains bounded prefix");
}
static void profile()
{
    struct Field { usize offset, width; u32 allowed; E::Error rejected; };
    const Field fields[] = {
        {128,2,0,E::Error::unsupported_abi_version}, {130,1,32,E::Error::unsupported_isa},
        {131,1,2,E::Error::unsupported_isa}, {132,1,1,E::Error::unsupported_gpr_size},
        {133,1,1,E::Error::unsupported_cpr1_size}, {134,1,0,E::Error::unsupported_cpr2_size},
        {135,1,1,E::Error::unsupported_fp_abi}, {136,4,0,E::Error::unsupported_isa_extension},
        {140,4,0,E::Error::unsupported_ases}, {148,4,0,E::Error::unsupported_abi_flags2},
    };
    for (const auto& f : fields)
    {
        for (u32 value = 0; value < 256; ++value)
        {
            if (value == f.allowed) continue;
            changed(f.offset, value, f.width, f.rejected, "all field low-byte alternatives");
        }
        for (usize bit = 0; bit < f.width * 8; ++bit)
            changed(f.offset, f.allowed ^ (u32{1} << bit), f.width, f.rejected, "field high-bit alternatives");
    }
    for (unsigned bit = 1; bit < 32; ++bit)
        changed(144, u32{1} << bit, 4, E::Error::unsupported_abi_flags1, "only ODDSPREG flag");
    auto b = fixture(); put(b, 144, 0, 4); check(E::admit(b, b.size()).ok(), "ODDSPREG optional");
}
static void guard_pages()
{
    const long system_page = sysconf(_SC_PAGESIZE); check(system_page >= 256, "system page size");
    const usize page = static_cast<usize>(system_page);
    auto* mem = static_cast<std::byte*>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    check(mem != MAP_FAILED, "guard map"); check(mprotect(mem + page, page, PROT_NONE) == 0, "guard protect");
    const auto b = fixture();
    for (usize n = 0; n <= b.size(); ++n)
    {
        auto* start = mem + page - n;
        for (usize i = 0; i < n; ++i) start[i] = b[i];
        check(E::admit({start, n}, b.size()).ok() == (n >= 152), "guarded exact prefix");
    }
    check(munmap(mem, page * 2) == 0, "guard unmap");
}
static void real(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    check(file.good(), "open explicitly selected real ELF");
    const std::vector<char> chars((std::istreambuf_iterator<char>(file)), {});
    Bytes b(chars.size());
    for (usize i = 0; i < chars.size(); ++i) b[i] = std::byte(static_cast<unsigned char>(chars[i]));
    check(E::admit(b, b.size()).ok(), "actual complete ELF admitted");
    usize limit = 0, retries = 0;
    while (true)
    {
        const auto r = E::admit(std::span(b).first(limit), b.size());
        if (r.ok()) break;
        check(r.required_prefix_bytes > limit && r.required_prefix_bytes <= b.size(), "actual bounded retry");
        limit = r.required_prefix_bytes; check(++retries <= 3, "finite staged metadata reads");
    }
    for (usize n = 0; n <= limit; ++n)
        check(E::admit(std::span(b).first(n), b.size()).ok() == (n == limit), "actual every prefix boundary");
    std::printf("ELF path=%s file_bytes=%zu minimum_prefix=%zu retries=%zu admitted=1\n", path, b.size(), limit, retries);
}
static void corruptions()
{
    // Deterministic multi-field and truncated-input stress. Preserve the hostile
    // e_flags bit so acceptance is always forbidden, independently of parser order.
    u32 rng = 0x96540217;
    const auto next = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    for (usize trial = 0; trial < 40000; ++trial)
    {
        auto b = fixture();
        for (usize count = 1 + next() % 12; count; --count)
        { const auto offset = next() % b.size(); b[offset] = std::byte(next() & 255); }
        b[36] |= std::byte{0x80};
        const usize prefix = next() % (b.size() + 1);
        const usize file = next() & 1 ? b.size() : next() % (b.size() + 1);
        check(!E::admit(std::span(b).first(prefix), file), "hostile multi-field truncation stress");
    }
}
static_assert(!E::detail::contains(0xffffffffu, 0xfffffff0u, 32));
static_assert(E::admit({}, 52).error == E::Error::header_outside_prefix);
int main(int argc, char** argv)
{
    structural(); profile(); guard_pages(); corruptions();
    for (int i = 1; i < argc; ++i) real(argv[i]);
    std::printf("PASS checks=%zu domain=hosted-metadata-parser ASan_UBSan=enabled\n", checks);
}
