#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/sched/process.hpp"

namespace ok::abi::linux_user
{

struct TlsTemplate
{
    u64 file_offset{0};
    u64 virtual_address{0};
    u64 file_size{0};
    u64 memory_size{0};
    u64 alignment{1};
};

struct ExecMetadata
{
    u64 program_headers{0};
    u64 program_header_size{0};
    u64 program_header_count{0};
    u64 entry{0};
    std::optional<TlsTemplate> tls{};
    std::optional<u64> interpreter_base{};
};

struct ExecImage
{
    sched::LoadedElf loaded{};
    ExecMetadata metadata{};
};

struct AuxvParameters
{
    u64 page_size{0};
    u64 uid{0};
    u64 euid{0};
    u64 gid{0};
    u64 egid{0};
    bool secure{false};
    // These are established user addresses, never kernel pointers. The caller
    // owns string termination, 16 random bytes, mapping and copy lifetimes.
    std::optional<u64> execfn{};
    std::optional<u64> random{};
};

// Validates bootstrap metadata; does not map an image, allocate TLS, build a
// complete initial stack, generate entropy or transfer control to user mode.
template <Profile Selected>
    requires SupportedProfile<Selected>
class ExecBootstrap final
{
  public:
    static constexpr const ProfileData &profile = Codec<Selected>::profile;
    static constexpr u64 word_mask = Codec<Selected>::word_mask;
    static constexpr usize max_prefix_bytes = 64 * 1024;
    static constexpr usize max_auxv_bytes = 14 * 2 * profile.word_bytes;
    static constexpr usize elf_header_bytes = profile.word_bytes == 8 ? 64 : 52;
    static constexpr usize program_header_bytes = profile.word_bytes == 8 ? 56 : 32;
    static constexpr arch::Architecture architecture = [] {
        if constexpr (Selected == Profile::i386) return arch::Architecture::i386;
        if constexpr (Selected == Profile::x86_64) return arch::Architecture::x86_64;
        if constexpr (Selected == Profile::aarch64) return arch::Architecture::aarch64;
        if constexpr (Selected == Profile::arm32) return arch::Architecture::arm32;
        if constexpr (Selected == Profile::rv64) return arch::Architecture::rv64;
        if constexpr (Selected == Profile::rv32) return arch::Architecture::rv32;
        if constexpr (Selected == Profile::loongarch64) return arch::Architecture::loongarch64;
        if constexpr (Selected == Profile::mips) return arch::Architecture::mips;
        if constexpr (Selected == Profile::mips64) return arch::Architecture::mips64;
        return arch::Architecture::ppc;
    }();

    [[nodiscard]] static Result<ExecImage> inspect(std::span<const std::byte> prefix, usize file_bytes)
    {
        if (prefix.size() < elf_header_bytes || prefix.size() > max_prefix_bytes || prefix.size() > file_bytes)
            return Status::invalid_argument("Linux ELF bootstrap prefix size");
        if (read(prefix, 4, 1) != (is64 ? 2u : 1u) || read(prefix, 5, 1) != (profile.big_endian ? 2u : 1u))
            return Status::unsupported("Linux ELF class or byte order differs from selected profile");
        if (read(prefix, 16, 2) != 2)
            return Status::unsupported("Linux bootstrap requires static ET_EXEC");
        const u64 phoff = read(prefix, is64 ? 32 : 28, profile.word_bytes);
        const u64 phent = read(prefix, is64 ? 54 : 42, 2);
        const u64 phnum = read(prefix, is64 ? 56 : 44, 2);
        if (phent != program_header_bytes || phnum == 0 || phnum == 0xffff || phoff < elf_header_bytes ||
            phoff > prefix.size() || phnum > (prefix.size() - phoff) / phent)
            return Status::invalid_argument("Linux ELF program table is incomplete or unsupported");
        const u64 table_bytes = phent * phnum;
        auto loaded = sched::ElfLoader{}.load_prefix(prefix, file_bytes, architecture);
        if (!loaded)
            return loaded.status();
        if (!loaded.value().static_executable)
            return Status::unsupported("Linux bootstrap rejects PT_INTERP and PT_DYNAMIC");
        for (const auto &segment : loaded.value().segments)
            if (!target_range(segment.virtual_address, segment.memory_size))
                return Status::overflow("Linux load segment exceeds target address width");

        // A table is usable only if exactly one readable file-backed mapping
        // contains all of it. The ELF file offset alone is not a user address.
        u64 table_address = 0;
        usize table_mappings = 0;
        for (const auto &segment : loaded.value().segments)
        {
            if (phoff < segment.file_offset || phoff - segment.file_offset > segment.file_size ||
                table_bytes > segment.file_size - (phoff - segment.file_offset))
                continue;
            if ((segment.permissions & memory::page_read) == 0)
                return Status::invalid_argument("Linux ELF program table mapping is not readable");
            table_address = segment.virtual_address + (phoff - segment.file_offset);
            ++table_mappings;
        }
        if (table_mappings != 1 || table_address == 0 || table_address % profile.word_bytes != 0 ||
            !target_range(table_address, table_bytes))
            return Status::invalid_argument("Linux ELF program table mapping is absent or ambiguous");

        ExecMetadata metadata{.program_headers = table_address,
                              .program_header_size = phent,
                              .program_header_count = phnum,
                              .entry = loaded.value().entry};
        bool saw_phdr = false;
        for (usize i = 0; i < phnum; ++i)
        {
            const usize offset = static_cast<usize>(phoff) + i * program_header_bytes;
            const u64 type = read(prefix, offset, 4);
            if (type != 6 && type != 7)
                continue;
            const auto segment = program_header(prefix, offset);
            if (segment.file_size > segment.memory_size || segment.file_offset > file_bytes ||
                segment.file_size > file_bytes - segment.file_offset ||
                !target_range(segment.virtual_address, segment.memory_size) ||
                !valid_alignment(segment.alignment) ||
                (segment.virtual_address & (segment.alignment - 1)) !=
                    (segment.file_offset & (segment.alignment - 1)))
                return Status::invalid_argument("Linux ELF bootstrap segment range or alignment");
            if ((read(prefix, offset + (is64 ? 4 : 24), 4) & 4) == 0)
                return Status::invalid_argument("Linux ELF bootstrap segment is not readable");
            if (type == 6)
            {
                if (saw_phdr || segment.file_offset != phoff || segment.virtual_address != table_address ||
                    segment.file_size != table_bytes || segment.memory_size != table_bytes)
                    return Status::invalid_argument("Linux PT_PHDR is duplicate or disagrees with loaded table");
                saw_phdr = true;
            }
            else
            {
                if (metadata.tls)
                    return Status::invalid_argument("Linux ELF contains duplicate PT_TLS");
                usize mappings = 0;
                for (const auto &load : loaded.value().segments)
                {
                    if (segment.file_offset < load.file_offset ||
                        segment.file_offset - load.file_offset > load.file_size)
                        continue;
                    const u64 delta = segment.file_offset - load.file_offset;
                    if (segment.file_size > load.file_size - delta ||
                        segment.virtual_address != load.virtual_address + delta ||
                        (load.permissions & memory::page_read) == 0)
                        continue;
                    ++mappings;
                }
                // An empty PT_TLS has no per-thread storage or template bytes;
                // linkers may leave its address and offset at zero. Keep the
                // metadata present so a second PT_TLS is still rejected.
                if (segment.memory_size != 0 && mappings != 1)
                    return Status::invalid_argument("Linux PT_TLS template has no exact readable load mapping");
                // Only p_filesz is a loaded template. The remaining per-thread
                // zero fill may extend beyond the containing PT_LOAD's extent.
                metadata.tls = segment;
            }
        }
        return ExecImage{.loaded = std::move(loaded.value()), .metadata = metadata};
    }

    // Returns bytes written, including AT_NULL. Capacity/value failures leave
    // the destination untouched. Optional pointers require external validation.
    [[nodiscard]] static Result<usize> encode_auxv(std::span<u8> destination, const ExecMetadata &metadata,
                                                   const AuxvParameters &parameters)
    {
        const usize pairs = 11 + (parameters.execfn ? 1 : 0) + (parameters.random ? 1 : 0) +
                            (metadata.interpreter_base ? 1 : 0);
        const usize bytes = pairs * 2 * profile.word_bytes;
        if (destination.size() < bytes)
            return Status::no_memory("Linux auxiliary vector output capacity");
        if (metadata.program_header_size != program_header_bytes || metadata.program_header_count == 0 ||
            metadata.program_header_count >= 0xffff || metadata.program_headers == 0 || metadata.entry == 0 ||
            metadata.program_headers % profile.word_bytes != 0 ||
            !valid_alignment(parameters.page_size) || parameters.page_size == 0 ||
            !target_range(metadata.program_headers, metadata.program_header_count * program_header_bytes))
            return Status::invalid_argument("Linux auxiliary vector bootstrap metadata");
        const std::array<u64, 6> words{metadata.entry, parameters.page_size, parameters.uid,
                                      parameters.euid, parameters.gid, parameters.egid};
        for (const auto word : words)
            if (word > word_mask)
                return Status::overflow("Linux auxiliary vector value exceeds target word");
        if (metadata.interpreter_base &&
            (*metadata.interpreter_base < parameters.page_size ||
             *metadata.interpreter_base > word_mask ||
             *metadata.interpreter_base % parameters.page_size != 0))
            return Status::invalid_argument("Linux interpreter base is not a mapped page address");
        if ((parameters.execfn && !user_pointer(*parameters.execfn, 1)) ||
            (parameters.random && !user_pointer(*parameters.random, 16)))
            return Status::invalid_argument("Linux auxiliary vector optional pointer range");
        std::array<u8, max_auxv_bytes> staged{};
        usize cursor = 0;
        auto append = [&](u64 key, u64 value) {
            write(staged, cursor, key);
            write(staged, cursor + profile.word_bytes, value);
            cursor += 2 * profile.word_bytes;
        };
        append(3, metadata.program_headers);
        append(4, metadata.program_header_size);
        append(5, metadata.program_header_count);
        append(6, parameters.page_size);
        if (metadata.interpreter_base) append(7, *metadata.interpreter_base);
        append(9, metadata.entry);
        append(11, parameters.uid);
        append(12, parameters.euid);
        append(13, parameters.gid);
        append(14, parameters.egid);
        append(23, parameters.secure ? 1 : 0);
        if (parameters.execfn) append(31, *parameters.execfn);
        if (parameters.random) append(25, *parameters.random);
        append(0, 0);
        for (usize index = 0; index < cursor; ++index)
            destination[index] = staged[index];
        return cursor;
    }

  private:
    static constexpr bool is64 = profile.word_bytes == 8;

    [[nodiscard]] static u64 read(std::span<const std::byte> bytes, usize offset, usize width)
    {
        u64 value = 0;
        for (usize index = 0; index < width; ++index)
        {
            const usize shift = (profile.big_endian ? width - 1 - index : index) * 8;
            value |= static_cast<u64>(std::to_integer<u8>(bytes[offset + index])) << shift;
        }
        return value;
    }

    static void write(std::span<u8> bytes, usize offset, u64 value)
    {
        for (usize index = 0; index < profile.word_bytes; ++index)
        {
            const usize shift = (profile.big_endian ? profile.word_bytes - 1 - index : index) * 8;
            bytes[offset + index] = static_cast<u8>(value >> shift);
        }
    }

    [[nodiscard]] static bool target_range(u64 address, u64 length)
    {
        return address <= word_mask && length <= word_mask - address;
    }

    [[nodiscard]] static bool user_pointer(u64 address, u64 length)
    {
        return address != 0 && address <= word_mask && length != 0 && length - 1 <= word_mask - address;
    }

    [[nodiscard]] static bool valid_alignment(u64 alignment)
    {
        return alignment != 0 && alignment <= word_mask && (alignment & (alignment - 1)) == 0;
    }

    [[nodiscard]] static TlsTemplate program_header(std::span<const std::byte> prefix, usize offset)
    {
        const u64 alignment = read(prefix, offset + (is64 ? 48 : 28), profile.word_bytes);
        return TlsTemplate{.file_offset = read(prefix, offset + (is64 ? 8 : 4), profile.word_bytes),
                           .virtual_address = read(prefix, offset + (is64 ? 16 : 8), profile.word_bytes),
                           .file_size = read(prefix, offset + (is64 ? 32 : 16), profile.word_bytes),
                           .memory_size = read(prefix, offset + (is64 ? 40 : 20), profile.word_bytes),
                           .alignment = alignment == 0 ? 1 : alignment};
    }
};

} // namespace ok::abi::linux_user
