#pragma once

#include "ok/abi/linux_exec.hpp"

namespace ok::abi::linux_user
{

// A bounded admission contract for the first x86-64 musl interpreter guest.
// It owns metadata only; the runtime retains both file identities and maps
// their segments before publishing an address space.
struct X86DynamicImage
{
    sched::LoadedElf loaded{};
    ExecMetadata metadata{};
    u64 interpreter_offset{0};
    u64 interpreter_size{0};
};

class X86DynamicExec final
{
  public:
    static constexpr usize max_prefix_bytes = 64 * 1024;
    static constexpr usize max_interpreter_bytes = 64;
    static constexpr usize program_header_bytes = 56;

    [[nodiscard]] static Result<X86DynamicImage> inspect(std::span<const std::byte> prefix,
                                                         usize file_bytes, bool interpreter)
    {
        if (prefix.size() < 64 || prefix.size() > max_prefix_bytes || prefix.size() > file_bytes)
            return Status::invalid_argument("dynamic ELF prefix size");
        if (read(prefix, 0, 4) != 0x464c457f || read(prefix, 4, 1) != 2 ||
            read(prefix, 5, 1) != 1 || read(prefix, 6, 1) != 1 ||
            (read(prefix, 7, 1) != 0 && read(prefix, 7, 1) != 3) ||
            read(prefix, 8, 1) != 0 || read(prefix, 16, 2) != 3 ||
            read(prefix, 18, 2) != 62 || read(prefix, 20, 4) != 1 ||
            read(prefix, 48, 4) != 0 || read(prefix, 52, 2) != 64)
            return Status::unsupported("unsupported x86-64 dynamic ELF identity");
        const u64 phoff = read(prefix, 32, 8);
        const u64 phent = read(prefix, 54, 2);
        const u64 phnum = read(prefix, 56, 2);
        if (phent != program_header_bytes || !phnum || phnum == 0xffff ||
            phoff < 64 || phoff > prefix.size() || phnum > (prefix.size() - phoff) / phent)
            return Status::invalid_argument("dynamic ELF program table is incomplete");
        auto loaded = sched::ElfLoader{}.load_prefix(prefix, file_bytes, arch::Architecture::x86_64);
        if (!loaded)
            return loaded.status();
        const u64 table_size = phnum * phent;
        u64 table_address = 0;
        if (exact_readable_mapping(loaded.value(), phoff, table_size, 0, false, table_address) != 1)
            return Status::invalid_argument("dynamic ELF table has no unique readable load");
        if (!table_address || table_address % 8)
            return Status::invalid_argument("dynamic ELF table address is unaligned");
        for (const auto &segment : loaded.value().segments)
        {
            if (!(segment.permissions & memory::page_read) ||
                ((segment.permissions & memory::page_write) && (segment.permissions & memory::page_execute)))
                return Status::denied("dynamic ELF load permissions violate W^X");
            if (segment.memory_size > ~u64{0} - 4095 ||
                segment.virtual_address > ~u64{0} - segment.memory_size - 4095)
                return Status::overflow("dynamic ELF rounded load wraps");
            const u64 begin = segment.virtual_address & ~u64{4095};
            const u64 end = (segment.virtual_address + segment.memory_size + 4095) & ~u64{4095};
            for (const auto &other : loaded.value().segments)
            {
                if (&segment == &other)
                    continue;
                if (other.memory_size > ~u64{0} - 4095 ||
                    other.virtual_address > ~u64{0} - other.memory_size - 4095)
                    return Status::overflow("dynamic ELF rounded load wraps");
                const u64 other_begin = other.virtual_address & ~u64{4095};
                const u64 other_end = (other.virtual_address + other.memory_size + 4095) & ~u64{4095};
                if (begin < other_end && other_begin < end)
                    return Status::invalid_argument("dynamic ELF load pages overlap");
            }
        }
        X86DynamicImage image{.loaded = std::move(loaded.value()),
                              .metadata = {.program_headers = table_address,
                                           .program_header_size = phent,
                                           .program_header_count = phnum,
                                           .entry = read(prefix, 24, 8)}};
        bool saw_phdr = false, saw_dynamic = false, saw_stack = false, saw_interp = false, saw_relro = false;
        for (usize i = 0; i < phnum; ++i)
        {
            const usize at = static_cast<usize>(phoff) + i * program_header_bytes;
            const u64 type = read(prefix, at, 4);
            const u64 flags = read(prefix, at + 4, 4);
            const u64 offset = read(prefix, at + 8, 8);
            const u64 address = read(prefix, at + 16, 8);
            const u64 files = read(prefix, at + 32, 8);
            const u64 memory = read(prefix, at + 40, 8);
            const u64 alignment = read(prefix, at + 48, 8);
            if (flags & ~u64{7})
                return Status::invalid_argument("dynamic ELF program flags are invalid");
            if (type == 1)
                continue;
            if (type != 2 && type != 3 && type != 6 && type != 7 &&
                type != 0x6474e551 && type != 0x6474e552 && type != 0)
                return Status::unsupported("unsupported x86-64 dynamic ELF program type");
            if (type == 0)
                continue;
            if (files > memory || offset > file_bytes || files > file_bytes - offset ||
                address > ~u64{0} - memory ||
                (alignment > 1 && ((alignment & (alignment - 1)) ||
                  (address & (alignment - 1)) != (offset & (alignment - 1)))))
                return Status::invalid_argument("dynamic ELF program range or alignment");
            if (type == 0x6474e551)
            {
                if (saw_stack || (flags & 1))
                    return Status::denied("dynamic ELF executable or duplicate stack");
                saw_stack = true;
                continue;
            }
            if (type == 0x6474e552)
            {
                if (saw_relro || !contained_memory(image.loaded, address, memory))
                    return Status::invalid_argument("dynamic ELF RELRO is duplicate or unmapped");
                saw_relro = true;
                continue;
            }
            if (type == 7 && memory != 0 && !contained_memory(image.loaded, address, memory))
                return Status::invalid_argument("dynamic ELF TLS memory is not fully mapped");
            u64 mapped = 0;
            if (type == 7 && memory == 0 && files == 0)
            {
                if (image.metadata.tls)
                    return Status::invalid_argument("duplicate dynamic ELF TLS");
                image.metadata.tls = TlsTemplate{offset, address, files, memory, alignment ? alignment : 1};
                continue;
            }
            if (!(flags & 4) || exact_readable_mapping(image.loaded, offset, files, address, true, mapped) != 1)
                return Status::invalid_argument("dynamic ELF program bytes have no exact readable load");
            if (type == 6)
            {
                if (saw_phdr || offset != phoff || address != table_address ||
                    files != table_size || memory != table_size)
                    return Status::invalid_argument("dynamic ELF PT_PHDR is inconsistent");
                saw_phdr = true;
            }
            else if (type == 2)
            {
                if (saw_dynamic || !files || memory != files)
                    return Status::invalid_argument("dynamic ELF PT_DYNAMIC is inconsistent");
                saw_dynamic = true;
            }
            else if (type == 3)
            {
                if (interpreter || saw_interp || files < 2 || files > max_interpreter_bytes || memory != files)
                    return Status::unsupported("dynamic ELF interpreter is absent or nested");
                if (offset > prefix.size() || files > prefix.size() - offset)
                    return Status::invalid_argument("dynamic ELF interpreter path exceeds admission prefix");
                saw_interp = true;
                image.interpreter_offset = offset;
                image.interpreter_size = files;
            }
            else if (type == 7)
            {
                if (image.metadata.tls)
                    return Status::invalid_argument("duplicate dynamic ELF TLS");
                image.metadata.tls = TlsTemplate{offset, address, files, memory, alignment ? alignment : 1};
            }
        }
        if (!saw_dynamic || !saw_stack || (!interpreter && (!saw_interp || !saw_phdr)))
            return Status::unsupported("dynamic ELF lacks required musl program metadata");
        return image;
    }

  private:
    [[nodiscard]] static u64 read(std::span<const std::byte> bytes, usize offset, usize width)
    {
        u64 result = 0;
        for (usize i = 0; i < width; ++i)
            result |= static_cast<u64>(std::to_integer<u8>(bytes[offset + i])) << (i * 8);
        return result;
    }

    [[nodiscard]] static usize exact_readable_mapping(const sched::LoadedElf &loaded, u64 offset,
                                                       u64 size, u64 address, bool check_address, u64 &mapped)
    {
        usize matches = 0;
        for (const auto &segment : loaded.segments)
        {
            if (!(segment.permissions & memory::page_read) || offset < segment.file_offset ||
                offset - segment.file_offset > segment.file_size ||
                size > segment.file_size - (offset - segment.file_offset))
                continue;
            const u64 candidate = segment.virtual_address + (offset - segment.file_offset);
            if (check_address && address != candidate)
                continue;
            mapped = candidate;
            ++matches;
        }
        return matches;
    }

    [[nodiscard]] static bool contained_memory(const sched::LoadedElf &loaded, u64 address, u64 size)
    {
        for (const auto &segment : loaded.segments)
            if (address >= segment.virtual_address && address - segment.virtual_address <= segment.memory_size &&
                size <= segment.memory_size - (address - segment.virtual_address))
                return true;
        return false;
    }
};

} // namespace ok::abi::linux_user
