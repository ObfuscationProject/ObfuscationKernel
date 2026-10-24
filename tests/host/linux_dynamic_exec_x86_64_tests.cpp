#include "ok/abi/linux_dynamic_exec_x86_64.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

namespace
{
using namespace ok;
using ok::abi::linux_user::X86DynamicExec;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL model x86-64 dynamic ELF: " << message << '\n';
        std::exit(1);
    }
}

struct File
{
    std::vector<std::byte> prefix;
    usize size{};
};

File read(const char *path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(input.good(), "fixture is readable");
    const auto size = input.tellg();
    require(size >= 64, "fixture contains a complete ELF header");
    const auto prefix_size = static_cast<usize>(size) < X86DynamicExec::max_prefix_bytes ?
        static_cast<usize>(size) : X86DynamicExec::max_prefix_bytes;
    File file{std::vector<std::byte>(prefix_size), static_cast<usize>(size)};
    input.seekg(0);
    input.read(reinterpret_cast<char *>(file.prefix.data()), prefix_size);
    require(input.gcount() == static_cast<std::streamsize>(prefix_size), "fixture prefix is complete");
    return file;
}

u64 get(const File &file, usize at, usize width)
{
    u64 value = 0;
    for (usize i = 0; i < width; ++i)
        value |= static_cast<u64>(std::to_integer<u8>(file.prefix[at + i])) << (8 * i);
    return value;
}

void put(File &file, usize at, u64 value, usize width)
{
    for (usize i = 0; i < width; ++i)
        file.prefix[at + i] = static_cast<std::byte>(value >> (8 * i));
}

usize program(const File &file, u32 type, usize occurrence = 0)
{
    const auto start = static_cast<usize>(get(file, 32, 8));
    const auto count = static_cast<usize>(get(file, 56, 2));
    for (usize i = 0; i < count; ++i)
    {
        const auto at = start + i * X86DynamicExec::program_header_bytes;
        if (get(file, at, 4) == type)
        {
            if (occurrence == 0)
                return at;
            --occurrence;
        }
    }
    require(false, "expected fixture program header exists");
    return 0;
}

bool accepts(const File &file, bool interpreter)
{
    return X86DynamicExec::inspect(file.prefix, file.size, interpreter).ok();
}
} // namespace

int main(int argc, char **argv)
{
    require(argc == 3, "main and interpreter fixture paths are supplied");
    const auto main = read(argv[1]);
    const auto loader = read(argv[2]);
    const auto parsed = X86DynamicExec::inspect(main.prefix, main.size, false);
    require(parsed.ok(), "actual shared-musl PIE is admitted");
    require(parsed.value().metadata.program_headers == 64 &&
            parsed.value().metadata.entry == parsed.value().loaded.entry &&
            parsed.value().interpreter_size == sizeof("/lib/ld-musl-x86_64.so.1"),
            "PIE metadata matches the admitted on-disk image");
    require(accepts(loader, true), "actual shared musl interpreter is admitted");
    require(!accepts(main, true) && !accepts(loader, false),
            "main and interpreter admission roles cannot be interchanged");
    auto reject = [&](auto mutation, const char *message) {
        auto changed = main;
        mutation(changed);
        require(!accepts(changed, false), message);
    };
    reject([](File &file) { put(file, 16, 2, 2); }, "ET_EXEC cannot enter dynamic path");
    reject([](File &file) { put(file, 7, 255, 1); }, "unknown ELF OSABI is rejected");
    reject([](File &file) { put(file, 32, file.prefix.size() - 8, 8); },
           "incomplete program header table is rejected");
    reject([](File &file) { put(file, program(file, 3), 0, 4); }, "missing PT_INTERP is rejected");
    reject([](File &file) { put(file, program(file, 2), 0, 4); }, "missing PT_DYNAMIC is rejected");
    reject([](File &file) { put(file, program(file, 6) + 16, 0x80, 8); },
           "inconsistent PT_PHDR is rejected");
    reject([](File &file) { put(file, program(file, 1) + 4, 7, 4); },
           "writable executable LOAD is rejected");
    reject([](File &file) {
        const auto second = program(file, 1, 1);
        put(file, second + 16, get(file, second + 8, 8), 8);
    }, "distinct LOAD byte ranges cannot share a page");
    reject([](File &file) { put(file, program(file, 3) + 32, 1, 8); },
           "interpreter path requires at least one byte and a terminator");
    auto tls = main;
    const auto relro = program(tls, 0x6474e552);
    put(tls, relro, 7, 4);
    require(accepts(tls, false), "fully contained PT_TLS template is admitted");
    put(tls, relro + 40, 0x3000, 8);
    require(!accepts(tls, false), "PT_TLS zero-fill cannot escape every load mapping");
    std::cout << "PASS model x86-64 dynamic ELF admission and negative controls\n";
}
