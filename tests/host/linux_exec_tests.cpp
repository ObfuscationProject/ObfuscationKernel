#include "ok/abi/linux_exec.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace ok;
using namespace ok::abi::linux_user;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

template <Profile P> struct Fixture
{
    using Bootstrap = ExecBootstrap<P>;
    static constexpr bool wide = Bootstrap::profile.word_bytes == 8;
    static constexpr usize width = Bootstrap::profile.word_bytes;
    static constexpr usize header = Bootstrap::elf_header_bytes;
    static constexpr usize phent = Bootstrap::program_header_bytes;
    std::array<std::byte, 0x500> bytes{};

    void put(usize offset, u64 value, usize count)
    {
        for (usize index = 0; index < count; ++index)
        {
            const usize shift = (Bootstrap::profile.big_endian ? count - 1 - index : index) * 8;
            bytes[offset + index] = static_cast<std::byte>(value >> shift);
        }
    }
    void type(usize index, u64 value) { put(header + index * phent, value, 4); }
    void offset(usize index, u64 value) { put(header + index * phent + (wide ? 8 : 4), value, width); }
    void address(usize index, u64 value) { put(header + index * phent + (wide ? 16 : 8), value, width); }
    void filesz(usize index, u64 value) { put(header + index * phent + (wide ? 32 : 16), value, width); }
    void memsz(usize index, u64 value) { put(header + index * phent + (wide ? 40 : 20), value, width); }
    void flags(usize index, u64 value) { put(header + index * phent + (wide ? 4 : 24), value, 4); }
    void align(usize index, u64 value) { put(header + index * phent + (wide ? 48 : 28), value, width); }
    void segment(usize index, u64 kind, u64 file_offset, u64 virtual_address,
                 u64 file_bytes, u64 memory_bytes, u64 permissions, u64 alignment)
    {
        type(index, kind);
        offset(index, file_offset);
        address(index, virtual_address);
        filesz(index, file_bytes);
        memsz(index, memory_bytes);
        flags(index, permissions);
        align(index, alignment);
    }
    Fixture()
    {
        put(0, 0x7f, 1); put(1, 'E', 1); put(2, 'L', 1); put(3, 'F', 1);
        put(4, wide ? 2 : 1, 1); put(5, Bootstrap::profile.big_endian ? 2 : 1, 1); put(6, 1, 1);
        constexpr std::array<u16, 10> machines{3, 62, 183, 40, 243, 243, 258, 8, 8, 20};
        put(16, 2, 2); put(18, machines[static_cast<usize>(P)], 2); put(20, 1, 4);
        put(24, 0x40000200, width); put(wide ? 32 : 28, header, width);
        put(wide ? 52 : 40, header, 2); put(wide ? 54 : 42, phent, 2); put(wide ? 56 : 44, 4, 2);
        segment(0, 1, 0, 0x40000000, 0x300, 0x300, 5, 0x1000);
        segment(1, 1, 0x300, 0x40001300, 0x20, 0x80, 6, 0x1000);
        segment(2, 7, 0x300, 0x40001300, 4, 16, 4, 16);
        segment(3, 6, header, 0x40000000 + header, 4 * phent, 4 * phent, 4, width);
    }
    Result<ExecImage> inspect() const { return Bootstrap::inspect(std::span(bytes).first(0x200), bytes.size()); }
};

template <Profile P> void metadata_tests()
{
    using F = Fixture<P>;
    using B = ExecBootstrap<P>;
    F valid;
    auto parsed = valid.inspect();
    require(parsed.ok(), "valid metadata");
    const auto &metadata = parsed.value().metadata;
    require(metadata.program_headers == 0x40000000 + F::header, "PHDR uses exact loaded file displacement");
    require(metadata.program_header_size == F::phent && metadata.program_header_count == 4,
            "program header size and count");
    require(metadata.entry == 0x40000200 && parsed.value().loaded.segments.size() == 2, "reuse loaded ELF contract");
    require(metadata.tls && metadata.tls->file_offset == 0x300 && metadata.tls->virtual_address == 0x40001300 &&
            metadata.tls->file_size == 4 && metadata.tls->memory_size == 16 && metadata.tls->alignment == 16,
            "TLS initialization metadata");
    auto bad = [&](auto mutation, const char *message) {
        F changed;
        mutation(changed);
        require(!changed.inspect(), message);
    };
    bad([](F &f) { f.put(0, 0, 1); }, "invalid magic delegated to loader");
    bad([](F &f) { f.put(4, F::wide ? 1 : 2, 1); }, "wrong profile class");
    bad([](F &f) { f.put(5, B::profile.big_endian ? 1 : 2, 1); }, "wrong profile endian");
    bad([](F &f) { f.put(18, 0, 2); }, "wrong machine");
    bad([](F &f) { f.put(16, 3, 2); }, "ET_DYN is not a static bootstrap executable");
    bad([](F &f) { f.put(F::wide ? 54 : 42, F::phent + 8, 2); }, "oversized PHENT rejected");
    bad([](F &f) { f.put(F::wide ? 56 : 44, 0xffff, 2); }, "extended PHNUM unsupported");
    bad([](F &f) { f.put(F::wide ? 32 : 28, B::word_mask, F::width); }, "PHOFF arithmetic bounded");
    bad([](F &f) { f.type(3, 2); }, "PT_DYNAMIC rejected");
    bad([](F &f) { f.type(3, 3); }, "PT_INTERP rejected");
    bad([](F &f) { f.filesz(0, F::header); }, "program table not file backed");
    bad([](F &f) { f.flags(0, 1); }, "program table load is unreadable");
    bad([](F &f) { f.address(0, 0x40000001); f.align(0, 1); f.type(3, 0); }, "loaded program table lacks target alignment");
    bad([](F &f) { f.segment(1, 1, 0, 0x50000000, 0x300, 0x300, 4, 0x1000); }, "aliased program table rejected");
    bad([](F &f) { f.address(3, 0x40001000 + F::header); }, "PT_PHDR address mismatch");
    bad([](F &f) { f.offset(3, F::header + F::width); }, "PT_PHDR offset mismatch");
    bad([](F &f) { f.filesz(3, 4 * F::phent - 1); }, "PT_PHDR extent mismatch");
    bad([](F &f) { f.memsz(3, 4 * F::phent + 1); }, "PT_PHDR memory extent mismatch");
    bad([](F &f) { f.flags(3, 0); }, "PT_PHDR unreadable");
    bad([](F &f) { f.segment(2, 6, F::header, 0x40000000 + F::header, 4 * F::phent, 4 * F::phent, 4, F::width); },
        "duplicate PT_PHDR");
    bad([](F &f) { f.segment(3, 7, 0x300, 0x40001300, 4, 16, 4, 16); }, "duplicate PT_TLS");
    bad([](F &f) { f.memsz(2, 3); }, "TLS file exceeds memory");
    bad([](F &f) { f.offset(2, f.bytes.size()); }, "TLS exceeds complete file");
    bad([](F &f) { f.address(2, B::word_mask - 3); f.align(2, 1); }, "TLS wraps target address");
    bad([](F &f) { f.align(2, 3); }, "TLS non-power alignment");
    bad([](F &f) { f.address(2, 0x40001301); }, "TLS alignment incongruence");
    bad([](F &f) { f.address(2, 0x40002300); }, "TLS template not mapped at declared address");
    bad([](F &f) { f.offset(2, 0x310); }, "TLS file displacement mismatch");
    bad([](F &f) { f.flags(2, 0); }, "TLS header unreadable");
    bad([](F &f) { f.flags(1, 2); }, "TLS load unreadable");
    bad([](F &f) { f.filesz(2, 0x21); f.memsz(2, 0x21); }, "TLS initialized bytes outside load");
    bad([](F &f) { f.address(1, B::word_mask - 0x7f); f.align(1, 1); }, "load exceeds target width");
    F no_phdr;
    no_phdr.type(3, 0x6474e551);
    require(no_phdr.inspect().ok(), "derive PHDR without PT_PHDR");
    F no_tls;
    no_tls.type(2, 0);
    require(no_tls.inspect().ok() && !no_tls.inspect().value().metadata.tls, "absent TLS is explicit");
    F empty_tls;
    empty_tls.segment(2, 7, 0, 0, 0, 0, 4, 8);
    auto empty = empty_tls.inspect();
    require(empty.ok() && empty.value().metadata.tls && empty.value().loaded.segments.size() == 2,
            "empty linker PT_TLS needs no load mapping and remains explicit");
    const auto &empty_template = *empty.value().metadata.tls;
    require(empty_template.file_offset == 0 && empty_template.virtual_address == 0 &&
            empty_template.file_size == 0 && empty_template.memory_size == 0 && empty_template.alignment == 8,
            "empty TLS retains its declared metadata without adding a mapping");
    auto bad_empty = [&](auto mutation, const char *message) {
        F changed = empty_tls;
        mutation(changed);
        require(!changed.inspect(), message);
    };
    bad_empty([](F &f) { f.segment(3, 7, 0, 0, 0, 0, 4, 8); }, "duplicate empty PT_TLS rejected");
    bad_empty([](F &f) { f.segment(3, 7, 0x300, 0x40001300, 4, 16, 4, 16); },
              "empty PT_TLS does not hide a later nonempty duplicate");
    bad([](F &f) { f.segment(3, 7, 0, 0, 0, 0, 4, 8); },
        "nonempty PT_TLS does not admit a later empty duplicate");
    bad_empty([](F &f) { f.filesz(2, 1); }, "empty TLS cannot contain initialized bytes");
    bad_empty([](F &f) { f.memsz(2, 1); }, "nonempty zero-fill TLS still requires a load anchor");
    bad_empty([](F &f) { f.offset(2, f.bytes.size() + 8); }, "empty TLS file offset remains bounded");
    bad_empty([](F &f) { f.align(2, 3); }, "empty TLS alignment remains a power of two");
    bad_empty([](F &f) { f.address(2, 1); }, "empty TLS alignment congruence remains required");
    bad_empty([](F &f) { f.flags(2, 0); }, "empty TLS header must remain readable");
    empty_tls.align(2, 0);
    require(empty_tls.inspect().ok() && empty_tls.inspect().value().metadata.tls->alignment == 1,
            "empty TLS zero alignment is normalized consistently");
    F tbss;
    tbss.memsz(2, 0x2000);
    require(tbss.inspect().ok(), "TLS zero fill need not be file-image memory");
    F pure_tbss;
    pure_tbss.segment(2, 7, 0x320, 0x40001320, 0, 0x2000, 4, 0);
    require(pure_tbss.inspect().ok() && pure_tbss.inspect().value().metadata.tls->alignment == 1,
            "pure tbss has exact end-of-file load anchor and normalized zero alignment");
    pure_tbss.offset(2, 0x330);
    pure_tbss.address(2, 0x40001330);
    require(!pure_tbss.inspect(), "pure tbss anchor outside file-backed load rejected");
    F processor_metadata;
    processor_metadata.segment(3, 0x70000003, 0x400, 0, 0x20, 0, 4, 1);
    require(processor_metadata.inspect().ok(), "processor metadata is not a load or TLS extent");
    for (usize length = 0; length < F::header + 4 * F::phent; ++length)
        require(!B::inspect(std::span(valid.bytes).first(length), valid.bytes.size()), "all truncated prefixes rejected");
    require(!B::inspect(valid.bytes, valid.bytes.size() - 1), "prefix cannot exceed complete file size");
    std::vector<std::byte> oversized(B::max_prefix_bytes + 1);
    require(!B::inspect(oversized, oversized.size()), "explicit prefix capacity enforced");
    // Deterministic mutation sweep exercises bounded reads under sanitizers.
    for (usize index = 0; index < F::header + 4 * F::phent; ++index)
    {
        F changed;
        changed.bytes[index] ^= std::byte{0xff};
        static_cast<void>(changed.inspect());
    }
}

template <Profile P> void auxv_tests()
{
    using B = ExecBootstrap<P>;
    const auto metadata = Fixture<P>{}.inspect().value().metadata;
    AuxvParameters parameters{.page_size = 4096, .uid = 0x12345678, .euid = 2, .gid = 3, .egid = 4,
                               .secure = true, .execfn = 0x70000123, .random = 0x70000200};
    std::array<u8, B::max_auxv_bytes + 16> output;
    output.fill(0xa5);
    const auto result = B::encode_auxv(output, metadata, parameters);
    require(result.ok() && result.value() == B::max_auxv_bytes - 2 * B::profile.word_bytes,
            "static auxv byte count stays unchanged");
    auto word = [&](usize index) {
        u64 value = 0;
        const usize offset = index * B::profile.word_bytes;
        for (usize byte = 0; byte < B::profile.word_bytes; ++byte)
        {
            const usize at = B::profile.big_endian ? byte : B::profile.word_bytes - 1 - byte;
            value = (value << 8) | output[offset + at];
        }
        return value;
    };
    const std::array<u64, 26> expected{3, metadata.program_headers, 4, metadata.program_header_size,
        5, 4, 6, 4096, 9, metadata.entry, 11, 0x12345678, 12, 2, 13, 3, 14, 4, 23, 1,
        31, 0x70000123, 25, 0x70000200, 0, 0};
    for (usize index = 0; index < expected.size(); ++index)
        require(word(index) == expected[index], "exact target-word/endian auxv content");
    require(std::all_of(output.begin() + result.value(), output.end(), [](u8 value) { return value == 0xa5; }),
            "auxv serialization preserves trailing bytes");
    auto dynamic_metadata = metadata;
    dynamic_metadata.interpreter_base = 0x60000000;
    output.fill(0xa5);
    const auto dynamic_result = B::encode_auxv(output, dynamic_metadata, parameters);
    require(dynamic_result.ok() && dynamic_result.value() == B::max_auxv_bytes,
            "dynamic auxv includes exactly one AT_BASE pair");
    require(word(8) == 7 && word(9) == 0x60000000 && word(26) == 0 && word(27) == 0,
            "AT_BASE is encoded in target word order before AT_ENTRY");
    auto bad = [&](auto mutation, const char *message) {
        auto changed_metadata = metadata;
        auto changed_parameters = parameters;
        mutation(changed_metadata, changed_parameters);
        output.fill(0xa5);
        require(!B::encode_auxv(output, changed_metadata, changed_parameters), message);
        require(std::all_of(output.begin(), output.end(), [](u8 value) { return value == 0xa5; }),
                "failed auxv validation leaves all output untouched");
    };
    bad([](auto &, auto &p) { p.page_size = 0; }, "zero page size");
    bad([](auto &, auto &p) { p.page_size = 3; }, "non-power page size");
    bad([](auto &m, auto &) { m.program_headers = B::word_mask - 8; }, "PHDR range overflows target");
    bad([](auto &m, auto &) { m.program_headers += 1; }, "auxv PHDR lacks target alignment");
    bad([](auto &m, auto &) { m.program_header_size = 1; }, "incorrect auxv PHENT");
    bad([](auto &m, auto &) { m.program_header_count = 0xffff; }, "extended auxv PHNUM");
    bad([](auto &m, auto &) { m.entry = 0; }, "null entry");
    bad([](auto &m, auto &) { m.interpreter_base = 0; }, "null interpreter base");
    bad([](auto &m, auto &) { m.interpreter_base = 0x60000001; }, "unaligned interpreter base");
    bad([](auto &, auto &p) { p.execfn = 0; }, "null EXECFN pointer");
    bad([](auto &, auto &p) { p.random = 0; }, "null RANDOM pointer");
    bad([](auto &, auto &p) { p.random = B::word_mask - 14; }, "RANDOM 16-byte target range overflow");
    if constexpr (B::profile.word_bytes == 4)
    {
        bad([](auto &, auto &p) { p.uid = 0x100000000ULL; }, "UID cannot truncate to target word");
        bad([](auto &, auto &p) { p.execfn = 0x100000000ULL; }, "EXECFN cannot truncate to target word");
        bad([](auto &m, auto &) { m.entry = 0x100000000ULL; }, "entry cannot truncate to target word");
    }
    output.fill(0xa5);
    require(!B::encode_auxv(std::span(output).first(result.value() - 1), metadata, parameters), "short auxv output");
    require(std::all_of(output.begin(), output.end(), [](u8 value) { return value == 0xa5; }), "short output is atomic");
    parameters.execfn.reset();
    parameters.random.reset();
    auto minimal = B::encode_auxv(output, metadata, parameters);
    require(minimal.ok() && minimal.value() == 11 * 2 * B::profile.word_bytes, "optional pointers are omitted");
    require(word(20) == 0 && word(21) == 0, "minimal auxv terminator");
    parameters.random = B::word_mask - 15;
    require(B::encode_auxv(output, metadata, parameters).ok(), "last complete RANDOM span is representable");
}

template <Profile P> void test_profile()
{
    metadata_tests<P>();
    auxv_tests<P>();
    std::cout << "PASS model Linux ELF metadata/auxv " << ExecBootstrap<P>::profile.name << '\n';
}

template <Profile P> bool audit(const std::string &profile, const char *path, bool expected_rejection)
{
    using B = ExecBootstrap<P>;
    if (profile != B::profile.name)
        return false;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(file.good(), "open explicit ELF probe");
    const auto size = file.tellg();
    require(size >= 0 && static_cast<unsigned long long>(size) <= ~usize{0}, "probe file length representable");
    const usize file_bytes = static_cast<usize>(size);
    const usize prefix_bytes = std::min(file_bytes, B::max_prefix_bytes);
    std::vector<std::byte> prefix(prefix_bytes);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
    require(file.good(), "read explicit ELF prefix");
    const auto result = B::inspect(prefix, file_bytes);
    if (expected_rejection)
    {
        require(!result, "expected explicit ELF probe rejection");
        std::cout << "REJECT metadata " << profile << ' ' << path << ": " << result.status().message() << '\n';
    }
    else
    {
        if (!result)
            std::cerr << result.status().message() << '\n';
        require(result.ok(), "explicit ELF probe admission");
        const auto &m = result.value().metadata;
        std::cout << "PASS metadata " << profile << ' ' << path << " PHDR=0x" << std::hex << m.program_headers
                  << " ENTRY=0x" << m.entry << std::dec << " PHENT=" << m.program_header_size
                  << " PHNUM=" << m.program_header_count;
        if (m.tls)
            std::cout << " TLS=0x" << std::hex << m.tls->virtual_address << std::dec << " FILE=" << m.tls->file_size
                      << " MEMORY=" << m.tls->memory_size << " ALIGN=" << m.tls->alignment;
        std::cout << " (not executed)\n";
    }
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    test_profile<Profile::i386>(); test_profile<Profile::x86_64>();
    test_profile<Profile::aarch64>(); test_profile<Profile::arm32>();
    test_profile<Profile::rv64>(); test_profile<Profile::rv32>();
    test_profile<Profile::loongarch64>(); test_profile<Profile::mips>();
    test_profile<Profile::mips64>(); test_profile<Profile::ppc>();
    require((argc - 1) % 3 == 0, "probe arguments are expectation/profile/path triples");
    for (int index = 1; index < argc; index += 3)
    {
        const std::string expectation = argv[index];
        const std::string profile = argv[index + 1];
        require(expectation == "accept" || expectation == "reject", "explicit probe expectation");
        const bool reject = expectation == "reject";
        const char *path = argv[index + 2];
        require(audit<Profile::i386>(profile, path, reject) || audit<Profile::x86_64>(profile, path, reject) ||
                audit<Profile::aarch64>(profile, path, reject) || audit<Profile::arm32>(profile, path, reject) ||
                audit<Profile::rv64>(profile, path, reject) || audit<Profile::rv32>(profile, path, reject) ||
                audit<Profile::loongarch64>(profile, path, reject) || audit<Profile::mips>(profile, path, reject) ||
                audit<Profile::mips64>(profile, path, reject) || audit<Profile::ppc>(profile, path, reject),
                "recognized explicit probe profile");
    }
}
