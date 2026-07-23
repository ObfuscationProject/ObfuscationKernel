#include "ok/core/module.hpp"

namespace ok
{
namespace
{

constexpr uptr module_entry_stride = 0x100;
constexpr uptr module_stack_stride = 0x1000;
constexpr uptr module_thread_entry_stride = 0x10;
constexpr uptr module_thread_stack_stride = 0x100;
constexpr u32 elf_section_type_rel = 9;
constexpr u32 elf_section_type_rela = 4;

[[nodiscard]] u8 byte_at(std::span<const std::byte> bytes, usize offset)
{
    return offset < bytes.size() ? std::to_integer<u8>(bytes[offset]) : 0;
}

[[nodiscard]] u64 read_int(std::span<const std::byte> bytes, usize offset, usize width, bool little_endian)
{
    if (offset + width > bytes.size())
    {
        return 0;
    }
    u64 value = 0;
    for (usize i = 0; i < width; ++i)
    {
        const auto index = little_endian ? i : width - 1 - i;
        value |= static_cast<u64>(byte_at(bytes, offset + index)) << (i * 8u);
    }
    return value;
}

[[nodiscard]] u16 read_u16(std::span<const std::byte> bytes, usize offset, bool little_endian)
{
    return static_cast<u16>(read_int(bytes, offset, 2, little_endian));
}

[[nodiscard]] u32 read_u32(std::span<const std::byte> bytes, usize offset, bool little_endian)
{
    return static_cast<u32>(read_int(bytes, offset, 4, little_endian));
}

[[nodiscard]] u64 read_u64(std::span<const std::byte> bytes, usize offset, bool little_endian)
{
    return read_int(bytes, offset, 8, little_endian);
}

[[nodiscard]] std::string_view bytes_view(std::span<const std::byte> bytes)
{
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

[[nodiscard]] bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool contains(std::string_view value, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    if (needle.size() > value.size())
    {
        return false;
    }
    for (usize offset = 0; offset <= value.size() - needle.size(); ++offset)
    {
        if (value.substr(offset, needle.size()) == needle)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] u64 parse_unsigned(std::string_view value)
{
    u64 out = 0;
    usize cursor = 0;
    u32 base = 10;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
    {
        cursor = 2;
        base = 16;
    }
    for (; cursor < value.size(); ++cursor)
    {
        const auto ch = value[cursor];
        u8 digit = 0;
        if (ch >= '0' && ch <= '9')
        {
            digit = static_cast<u8>(ch - '0');
        }
        else if (base == 16 && ch >= 'a' && ch <= 'f')
        {
            digit = static_cast<u8>(10 + ch - 'a');
        }
        else if (base == 16 && ch >= 'A' && ch <= 'F')
        {
            digit = static_cast<u8>(10 + ch - 'A');
        }
        else
        {
            break;
        }
        if (digit >= base)
        {
            break;
        }
        out = out * base + digit;
    }
    return out;
}

[[nodiscard]] arch::Architecture arch_for_elf_machine(u16 machine, bool elf64, arch::Architecture fallback)
{
    switch (machine)
    {
    case 0x03:
        return arch::Architecture::i386;
    case 0x3e:
        return arch::Architecture::x86_64;
    case 0xb7:
        return arch::Architecture::aarch64;
    case 0x28:
        return arch::Architecture::arm32;
    case 0xf3:
        return elf64 ? arch::Architecture::rv64 : arch::Architecture::rv32;
    case 0x102:
        return arch::Architecture::loongarch64;
    case 0x08:
        return elf64 ? arch::Architecture::mips64 : arch::Architecture::mips;
    case 0x14:
        return arch::Architecture::ppc;
    default:
        return fallback;
    }
}

struct ElfSection
{
    u32 name_offset{0};
    u32 type{0};
    u64 offset{0};
    u64 size{0};
    u64 entry_size{0};
};

[[nodiscard]] Result<ElfSection> read_section(std::span<const std::byte> image, usize header_offset, bool elf64,
                                              bool little_endian)
{
    if (header_offset >= image.size())
    {
        return Status::invalid_argument("ELF section header is out of range");
    }
    if (elf64)
    {
        if (header_offset + 64 > image.size())
        {
            return Status::invalid_argument("ELF64 section header is truncated");
        }
        return ElfSection{
            .name_offset = read_u32(image, header_offset, little_endian),
            .type = read_u32(image, header_offset + 4, little_endian),
            .offset = read_u64(image, header_offset + 24, little_endian),
            .size = read_u64(image, header_offset + 32, little_endian),
            .entry_size = read_u64(image, header_offset + 56, little_endian),
        };
    }
    if (header_offset + 40 > image.size())
    {
        return Status::invalid_argument("ELF32 section header is truncated");
    }
    return ElfSection{
        .name_offset = read_u32(image, header_offset, little_endian),
        .type = read_u32(image, header_offset + 4, little_endian),
        .offset = read_u32(image, header_offset + 16, little_endian),
        .size = read_u32(image, header_offset + 20, little_endian),
        .entry_size = read_u32(image, header_offset + 36, little_endian),
    };
}

[[nodiscard]] std::string_view c_string_at(std::span<const std::byte> bytes, usize offset)
{
    if (offset >= bytes.size())
    {
        return {};
    }
    usize size = 0;
    while (offset + size < bytes.size() && byte_at(bytes, offset + size) != 0)
    {
        ++size;
    }
    return {reinterpret_cast<const char *>(bytes.data() + offset), size};
}

Status push_module_symbol(StaticVector<ModuleSymbol, max_module_symbols> &symbols, std::string_view name,
                          ModuleSymbolBinding binding, uptr address = 0)
{
    if (name.empty())
    {
        return Status::invalid_argument("module symbol name is empty");
    }
    ModuleSymbol symbol{.address = address, .binding = binding, .resolved = address != 0};
    if (auto status = symbol.name.assign(name); !status.ok())
    {
        return status;
    }
    return symbols.push_back(symbol);
}

Status push_module_parameter(ModuleImageInfo &info, std::string_view name, std::string_view value)
{
    if (name.empty())
    {
        return Status::invalid_argument("module parameter name is empty");
    }
    ModuleParameter parameter;
    if (auto status = parameter.name.assign(name); !status.ok())
    {
        return status;
    }
    if (auto status = parameter.value.assign(value); !status.ok())
    {
        return status;
    }
    return info.parameters.push_back(parameter);
}

Status parse_parameter(ModuleImageInfo &info, std::string_view value)
{
    const auto split = value.find(':');
    if (split == std::string_view::npos)
    {
        return push_module_parameter(info, value, {});
    }
    return push_module_parameter(info, value.substr(0, split), value.substr(split + 1));
}

Status parse_modinfo_entry(ModuleImageInfo &info, std::string_view entry)
{
    if (entry.empty())
    {
        return Status::success();
    }
    if (starts_with(entry, "name="))
    {
        return info.name.assign(entry.substr(5));
    }
    if (starts_with(entry, "version="))
    {
        return info.version.assign(entry.substr(8));
    }
    if (starts_with(entry, "vermagic="))
    {
        return info.vermagic.assign(entry.substr(9));
    }
    if (starts_with(entry, "parm="))
    {
        return parse_parameter(info, entry.substr(5));
    }
    if (starts_with(entry, "depends="))
    {
        return push_module_parameter(info, "depends", entry.substr(8));
    }
    if (starts_with(entry, "import="))
    {
        return push_module_symbol(info.imports, entry.substr(7), ModuleSymbolBinding::imported);
    }
    if (starts_with(entry, "export="))
    {
        return push_module_symbol(info.exports, entry.substr(7), ModuleSymbolBinding::exported);
    }
    if (starts_with(entry, "signature="))
    {
        info.signed_image = true;
        return info.signature.assign(entry.substr(10));
    }
    if (starts_with(entry, "sig_id="))
    {
        info.signed_image = true;
        return info.signature.assign(entry.substr(7));
    }
    if (contains(entry, "Module signature"))
    {
        info.signed_image = true;
    }
    return Status::success();
}

Status parse_modinfo_block(ModuleImageInfo &info, std::span<const std::byte> bytes)
{
    usize cursor = 0;
    while (cursor < bytes.size())
    {
        const auto entry = c_string_at(bytes, cursor);
        if (auto status = parse_modinfo_entry(info, entry); !status.ok())
        {
            return status;
        }
        cursor += entry.size() + 1;
    }
    return Status::success();
}

Status parse_okmod_line(ModuleImageInfo &info, std::string_view line)
{
    if (line.empty() || line == "OKMOD")
    {
        return Status::success();
    }
    if (starts_with(line, "name="))
    {
        return info.name.assign(line.substr(5));
    }
    if (starts_with(line, "version="))
    {
        return info.version.assign(line.substr(8));
    }
    if (starts_with(line, "vermagic="))
    {
        return info.vermagic.assign(line.substr(9));
    }
    if (starts_with(line, "signature="))
    {
        info.signed_image = true;
        return info.signature.assign(line.substr(10));
    }
    if (starts_with(line, "export="))
    {
        return push_module_symbol(info.exports, line.substr(7), ModuleSymbolBinding::exported);
    }
    if (starts_with(line, "require="))
    {
        return push_module_symbol(info.imports, line.substr(8), ModuleSymbolBinding::imported);
    }
    if (starts_with(line, "param="))
    {
        return parse_parameter(info, line.substr(6));
    }
    if (starts_with(line, "reloc="))
    {
        ModuleRelocation relocation{.offset = parse_unsigned(line.substr(6)), .kind = ModuleRelocationKind::relative};
        ++info.relocation_count;
        return info.relocations.push_back(relocation);
    }
    return Status::success();
}

Status assign_module_process_name(FixedString<sched::max_process_name> &out, std::string_view module_name)
{
    if (auto status = out.assign(kernel_module_process_prefix); !status.ok())
    {
        return status;
    }
    constexpr usize max_chars = sched::max_process_name - 1;
    const auto room = out.size() < max_chars ? max_chars - out.size() : 0;
    return out.append(module_name.substr(0, room));
}

usize module_thread_target(ModuleThreading threading, usize cpu_count)
{
    if (threading != ModuleThreading::per_cpu)
    {
        return 1;
    }
    if (cpu_count == 0)
    {
        return 1;
    }
    return cpu_count < sched::max_threads_per_process ? cpu_count : sched::max_threads_per_process;
}

} // namespace

Status ModuleSymbolRegistry::export_symbol(std::string_view name, uptr address)
{
    if (address == 0)
    {
        return Status::invalid_argument("module symbol address is zero");
    }
    for (const auto &symbol : symbols_)
    {
        if (symbol.name.view() == name)
        {
            return Status::already_exists("module symbol already exported");
        }
    }
    return push_module_symbol(symbols_, name, ModuleSymbolBinding::exported, address);
}

Result<uptr> ModuleSymbolRegistry::resolve(std::string_view name) const
{
    for (const auto &symbol : symbols_)
    {
        if (symbol.name.view() == name)
        {
            return symbol.address;
        }
    }
    return Status::not_found("module symbol is not exported");
}

Status ModuleSymbolRegistry::resolve_imports(ModuleImageInfo &image) const
{
    for (auto &symbol : image.imports)
    {
        auto address = resolve(symbol.name.view());
        if (!address)
        {
            symbol.resolved = false;
            return address.status();
        }
        symbol.address = address.value();
        symbol.resolved = true;
    }
    return Status::success();
}

Result<ModuleImageInfo> ModuleImageLoader::parse(std::span<const std::byte> image,
                                                 arch::Architecture fallback_architecture) const
{
    if (image.size() >= 5 && starts_with(bytes_view(image), "OKMOD"))
    {
        return parse_okmod(image, fallback_architecture);
    }
    if (image.size() >= 4 && byte_at(image, 0) == 0x7f && byte_at(image, 1) == static_cast<u8>('E') &&
        byte_at(image, 2) == static_cast<u8>('L') && byte_at(image, 3) == static_cast<u8>('F'))
    {
        return parse_linux_ko(image, fallback_architecture);
    }
    return Status::invalid_argument("module image format is not recognized");
}

Result<ModuleImageInfo> ModuleImageLoader::parse_okmod(std::span<const std::byte> image,
                                                       arch::Architecture fallback_architecture) const
{
    const auto text = bytes_view(image);
    if (!starts_with(text, "OKMOD"))
    {
        return Status::invalid_argument("okmod image header is missing");
    }

    ModuleImageInfo info{
        .format = ModuleImageFormat::okmod,
        .architecture = fallback_architecture,
        .has_modinfo = true,
    };
    usize line_start = 0;
    while (line_start <= text.size())
    {
        usize line_end = line_start;
        while (line_end < text.size() && text[line_end] != '\n')
        {
            ++line_end;
        }
        auto line = text.substr(line_start, line_end - line_start);
        if (!line.empty() && line[line.size() - 1] == '\r')
        {
            line = line.substr(0, line.size() - 1);
        }
        if (auto status = parse_okmod_line(info, line); !status.ok())
        {
            return status;
        }
        if (line_end == text.size())
        {
            break;
        }
        line_start = line_end + 1;
    }
    if (info.name.empty())
    {
        return Status::invalid_argument("okmod module name is missing");
    }
    return info;
}

Result<ModuleImageInfo> ModuleImageLoader::parse_linux_ko(std::span<const std::byte> image,
                                                          arch::Architecture fallback_architecture) const
{
    if (image.size() < 52 || byte_at(image, 0) != 0x7f || byte_at(image, 1) != static_cast<u8>('E') ||
        byte_at(image, 2) != static_cast<u8>('L') || byte_at(image, 3) != static_cast<u8>('F'))
    {
        return Status::invalid_argument("Linux module image is not ELF");
    }
    const auto elf_class = byte_at(image, 4);
    const bool elf64 = elf_class == 2;
    if (!elf64 && elf_class != 1)
    {
        return Status::unsupported("ELF class is not supported");
    }
    const auto data_encoding = byte_at(image, 5);
    const bool little_endian = data_encoding != 2;
    const auto type = read_u16(image, 16, little_endian);
    const auto machine = read_u16(image, 18, little_endian);
    const auto section_header_offset =
        elf64 ? read_u64(image, 40, little_endian) : read_u32(image, 32, little_endian);
    const auto section_entry_size =
        elf64 ? read_u16(image, 58, little_endian) : read_u16(image, 46, little_endian);
    const auto section_count = elf64 ? read_u16(image, 60, little_endian) : read_u16(image, 48, little_endian);
    const auto section_string_index =
        elf64 ? read_u16(image, 62, little_endian) : read_u16(image, 50, little_endian);

    if (section_count == 0 || section_entry_size == 0 ||
        section_header_offset + static_cast<u64>(section_entry_size) * section_count > image.size())
    {
        return Status::invalid_argument("ELF section table is invalid");
    }

    ModuleImageInfo info{
        .format = ModuleImageFormat::linux_ko,
        .architecture = arch_for_elf_machine(machine, elf64, fallback_architecture),
        .elf64 = elf64,
        .relocatable = type == 1,
        .section_count = section_count,
    };
    static_cast<void>(info.name.assign("linux-ko"));

    ElfSection string_section{};
    bool has_string_section = false;
    if (section_string_index < section_count)
    {
        auto section = read_section(image, static_cast<usize>(section_header_offset) +
                                               static_cast<usize>(section_string_index) * section_entry_size,
                                    elf64, little_endian);
        if (!section)
        {
            return section.status();
        }
        string_section = section.value();
        has_string_section = string_section.offset + string_section.size <= image.size();
    }

    std::span<const std::byte> section_names{};
    if (has_string_section)
    {
        section_names = image.subspan(static_cast<usize>(string_section.offset), static_cast<usize>(string_section.size));
    }

    for (usize i = 0; i < section_count; ++i)
    {
        auto section = read_section(image, static_cast<usize>(section_header_offset) + i * section_entry_size, elf64,
                                    little_endian);
        if (!section)
        {
            return section.status();
        }
        const auto current = section.value();
        std::string_view section_name{};
        if (!section_names.empty())
        {
            section_name = c_string_at(section_names, current.name_offset);
        }
        if (contains(section_name, "modinfo") && current.offset + current.size <= image.size())
        {
            info.has_modinfo = true;
            if (auto status = parse_modinfo_block(
                    info, image.subspan(static_cast<usize>(current.offset), static_cast<usize>(current.size)));
                !status.ok())
            {
                return status;
            }
        }
        if (contains(section_name, "ksymtab") || contains(section_name, "kallsyms") ||
            contains(section_name, "symtab"))
        {
            info.has_kallsyms = true;
        }
        if (contains(section_name, "init"))
        {
            info.has_init = true;
        }
        if (contains(section_name, "exit"))
        {
            info.has_exit = true;
        }
        if (current.type == elf_section_type_rela || current.type == elf_section_type_rel)
        {
            info.relocation_count += current.entry_size == 0 ? static_cast<usize>(1)
                                                             : static_cast<usize>(current.size / current.entry_size);
            ModuleRelocation relocation{
                .section_index = static_cast<u32>(i),
                .offset = static_cast<uptr>(current.offset),
                .kind = contains(section_name, "plt") ? ModuleRelocationKind::plt : ModuleRelocationKind::relative,
            };
            if (!info.relocations.full())
            {
                if (auto status = info.relocations.push_back(relocation); !status.ok())
                {
                    return status;
                }
            }
        }
    }

    if (info.vermagic.empty())
    {
        if (auto status = info.vermagic.assign("mainline-tracking"); !status.ok())
        {
            return status;
        }
    }
    return info;
}

Status LinuxAbiSnapshot::begin(std::string_view baseline, bool tracks_mainline)
{
    if (auto status = baseline_.assign(baseline); !status.ok())
    {
        return status;
    }
    tracks_mainline_ = tracks_mainline;
    required_symbols_.clear();
    implemented_symbols_.clear();
    layouts_.clear();
    return Status::success();
}

Status LinuxAbiSnapshot::record_required_symbol(std::string_view name)
{
    if (contains(std::span<const ModuleSymbol>{required_symbols_.begin(), required_symbols_.size()}, name))
    {
        return Status::already_exists("required Linux ABI symbol already recorded");
    }
    return push_module_symbol(required_symbols_, name, ModuleSymbolBinding::imported);
}

Status LinuxAbiSnapshot::record_implemented_symbol(std::string_view name)
{
    if (contains(std::span<const ModuleSymbol>{implemented_symbols_.begin(), implemented_symbols_.size()}, name))
    {
        return Status::already_exists("implemented Linux ABI symbol already recorded");
    }
    return push_module_symbol(implemented_symbols_, name, ModuleSymbolBinding::exported, 1);
}

Status LinuxAbiSnapshot::record_layout(std::string_view name, u32 size, u32 field_count)
{
    for (const auto &layout : layouts_)
    {
        if (layout.name.view() == name)
        {
            return Status::already_exists("Linux ABI layout already recorded");
        }
    }
    LinuxStructLayout layout{.size = size, .field_count = field_count};
    if (auto status = layout.name.assign(name); !status.ok())
    {
        return status;
    }
    return layouts_.push_back(layout);
}

bool LinuxAbiSnapshot::contains(std::span<const ModuleSymbol> symbols, std::string_view name) const
{
    for (const auto &symbol : symbols)
    {
        if (symbol.name.view() == name)
        {
            return true;
        }
    }
    return false;
}

u32 LinuxAbiSnapshot::coverage_x100() const
{
    if (required_symbols_.empty())
    {
        return 10000;
    }
    usize covered = 0;
    for (const auto &symbol : required_symbols_)
    {
        if (contains(std::span<const ModuleSymbol>{implemented_symbols_.begin(), implemented_symbols_.size()},
                     symbol.name.view()))
        {
            ++covered;
        }
    }
    return static_cast<u32>((covered * 10000u) / required_symbols_.size());
}

Status ServiceRegistry::register_service(std::string_view service_id, void *service)
{
    if (service_id.empty() || service == nullptr)
    {
        return Status::invalid_argument("invalid service registration");
    }
    for (const auto &entry : services_)
    {
        if (entry.service_id == service_id)
        {
            return Status::already_exists("service already registered");
        }
    }
    return services_.push_back(Entry{.service_id = service_id, .service = service});
}

void *ServiceRegistry::query_raw(std::string_view service_id) const
{
    for (const auto &entry : services_)
    {
        if (entry.service_id == service_id)
        {
            return entry.service;
        }
    }
    return nullptr;
}

Status ModuleManager::register_module(KernelModule &module)
{
    const auto manifest = module.manifest();
    if (manifest.name.empty())
    {
        return Status::invalid_argument("module name is empty");
    }
    if (has_module(manifest.name))
    {
        return Status::already_exists("module already registered");
    }
    if (auto status = validate_manifest(module); !status.ok())
    {
        module.fail(status.message());
        return status;
    }
    module.set_state(ModuleState::created);
    return modules_.push_back(&module);
}

Status ModuleManager::bind_kernel_process(sched::Scheduler &scheduler, arch::ArchOperations &arch, uptr entry,
                                          uptr stack)
{
    if (entry == 0 || stack == 0)
    {
        return Status::invalid_argument("kernel module process context is invalid");
    }
    kernel_process_scheduler_ = &scheduler;
    kernel_process_arch_ = &arch;
    kernel_process_entry_ = entry;
    kernel_process_stack_ = stack;
    kernel_process_pid_ = 0;
    kernel_process_modules_ = 0;
    module_processes_.clear();
    return Status::success();
}

Result<sched::ProcessId> ModuleManager::ensure_kernel_process()
{
    for (const auto &record : module_processes_)
    {
        if (kernel_process_scheduler_ != nullptr && kernel_process_scheduler_->find(record.pid) != nullptr)
        {
            return record.pid;
        }
    }
    return Status::not_initialized("kernel module process requires a module manifest");
}

Result<sched::ProcessId> ModuleManager::ensure_kernel_process(KernelModule &module)
{
    const auto manifest = module.manifest();
    ModuleProcessRecord *existing_record = nullptr;
    usize slot = module_processes_.size();
    for (usize i = 0; i < module_processes_.size(); ++i)
    {
        auto &record = module_processes_[i];
        if (record.module_name.view() != manifest.name)
        {
            continue;
        }
        existing_record = &record;
        slot = i;
        if (kernel_process_scheduler_ != nullptr && kernel_process_scheduler_->find(record.pid) != nullptr)
        {
            if (auto status = ensure_kernel_process_threads(record.pid, manifest.threading, slot); !status.ok())
            {
                module.fail(status.message());
                return status;
            }
            kernel_process_pid_ = record.pid;
            return record.pid;
        }
        break;
    }

    if (kernel_process_scheduler_ == nullptr || kernel_process_arch_ == nullptr)
    {
        return Status::not_initialized("kernel module process is not bound to a scheduler");
    }

    FixedString<sched::max_process_name> process_name;
    if (auto status = assign_module_process_name(process_name, manifest.name); !status.ok())
    {
        return status;
    }
    ModuleProcessRecord new_record{};
    if (existing_record == nullptr)
    {
        if (module_processes_.full())
        {
            return Status::overflow("kernel module process table is full");
        }
        if (auto status = new_record.module_name.assign(manifest.name); !status.ok())
        {
            return status;
        }
    }
    const auto context = kernel_process_arch_->make_kernel_context(
        kernel_process_entry_ + static_cast<uptr>(slot) * module_entry_stride,
        kernel_process_stack_ + static_cast<uptr>(slot) * module_stack_stride);
    auto process = kernel_process_scheduler_->spawn(sched::ScheduleRequest{
        .name = process_name.view(),
        .initial_context = context,
        .priority = sched::scheduler_default_priority,
        .cpu_affinity_mask = sched::cpu_affinity_any,
        .credentials = user::kernel_credentials(),
        .background = true,
        .cpu_accounting = sched::ProcessCpuAccounting::passive,
    });
    if (!process)
    {
        return process.status();
    }
    if (auto status = ensure_kernel_process_threads(process.value(), manifest.threading, slot); !status.ok())
    {
        static_cast<void>(kernel_process_scheduler_->kill_process(process.value()));
        module.fail(status.message());
        return status;
    }

    if (existing_record != nullptr)
    {
        existing_record->pid = process.value();
        kernel_process_pid_ = existing_record->pid;
        return existing_record->pid;
    }

    new_record.pid = process.value();
    if (auto status = module_processes_.push_back(new_record); !status.ok())
    {
        return status;
    }
    kernel_process_pid_ = process.value();
    return process.value();
}

Status ModuleManager::ensure_kernel_process_threads(sched::ProcessId pid, ModuleThreading threading, usize slot)
{
    if (kernel_process_scheduler_ == nullptr || kernel_process_arch_ == nullptr)
    {
        return Status::not_initialized("kernel module process is not bound to a scheduler");
    }
    auto *process = kernel_process_scheduler_->find(pid);
    if (process == nullptr)
    {
        return Status::not_found("kernel module process not found");
    }

    const auto target = module_thread_target(threading, kernel_process_scheduler_->cpu_count());
    while (process->threads().size() < target)
    {
        const auto worker = process->threads().size();
        const auto context = kernel_process_arch_->make_kernel_context(
            kernel_process_entry_ + static_cast<uptr>(slot) * module_entry_stride +
                static_cast<uptr>(worker) * module_thread_entry_stride,
            kernel_process_stack_ + static_cast<uptr>(slot) * module_stack_stride +
                static_cast<uptr>(worker) * module_thread_stack_stride);
        auto thread = kernel_process_scheduler_->create_thread(pid, context);
        if (!thread)
        {
            return thread.status();
        }
        process = kernel_process_scheduler_->find(pid);
        if (process == nullptr)
        {
            return Status::not_found("kernel module process disappeared");
        }
    }
    return Status::success();
}

KernelModule *ModuleManager::find(std::string_view name) const
{
    for (auto *module : modules_)
    {
        if (module->manifest().name == name)
        {
            return module;
        }
    }
    return nullptr;
}

usize ModuleManager::failed_count() const
{
    usize count = 0;
    for (auto *module : modules_)
    {
        if (module->state() == ModuleState::failed)
        {
            ++count;
        }
    }
    return count;
}

Result<usize> ModuleManager::index_of(std::string_view name) const
{
    for (usize i = 0; i < modules_.size(); ++i)
    {
        if (modules_[i]->manifest().name == name)
        {
            return i;
        }
    }
    return Status::not_found("module dependency is missing");
}

Status ModuleManager::check_dependencies(KernelModule &module) const
{
    const auto manifest = module.manifest();
    for (const auto &dependency : manifest.dependencies)
    {
        if (!dependency.required)
        {
            continue;
        }
        if (find(dependency.name) == nullptr)
        {
            return Status::not_found("module dependency is missing");
        }
    }
    return Status::success();
}

Status ModuleManager::validate_manifest(KernelModule &module) const
{
    const auto manifest = module.manifest();
    if (manifest.abi_version != kernel_module_abi_version)
    {
        return Status::unsupported("module ABI version is not supported");
    }
    if (manifest.resources.max_services != 0 && manifest.exported_services.size() > manifest.resources.max_services)
    {
        return Status::overflow("module service export budget exceeded");
    }
    const auto worker_budget = manifest.resources.max_threads == 0 ? static_cast<usize>(1) : manifest.resources.max_threads;
    if (manifest.threading == ModuleThreading::per_cpu && manifest.resources.max_threads != 0 &&
        kernel_process_scheduler_ != nullptr && kernel_process_scheduler_->cpu_count() > worker_budget)
    {
        return Status::overflow("module thread budget is below per-CPU worker count");
    }
    if (manifest.execution == ModuleExecution::kernel_process &&
        (manifest.capability_mask & module_capability_bit(ModuleCapability::owns_kernel_process)) == 0)
    {
        return Status::denied("kernel-process module is missing its process capability");
    }
    if (!manifest.exported_services.empty() &&
        (manifest.capability_mask & module_capability_bit(ModuleCapability::exports_services)) == 0)
    {
        return Status::denied("service-exporting module is missing its service capability");
    }
    if (!manifest.required_services.empty() &&
        (manifest.capability_mask & module_capability_bit(ModuleCapability::requires_services)) == 0)
    {
        return Status::denied("service-consuming module is missing its service capability");
    }
    if (manifest.threading == ModuleThreading::per_cpu &&
        (manifest.capability_mask & module_capability_bit(ModuleCapability::uses_per_cpu_workers)) == 0)
    {
        return Status::denied("per-CPU module is missing its worker capability");
    }
    return Status::success();
}

Status ModuleManager::visit(usize index)
{
    if (index >= modules_.size())
    {
        return Status::invalid_argument("module index out of range");
    }
    if (visit_state_[index] == VisitState::visited)
    {
        return Status::success();
    }
    if (visit_state_[index] == VisitState::visiting)
    {
        modules_[index]->fail("module dependency cycle");
        return Status::invalid_argument("module dependency cycle");
    }

    visit_state_[index] = VisitState::visiting;
    const auto manifest = modules_[index]->manifest();
    for (const auto &dependency : manifest.dependencies)
    {
        if (!dependency.required)
        {
            continue;
        }
        auto dependency_index = index_of(dependency.name);
        if (!dependency_index)
        {
            modules_[index]->fail("module dependency is missing");
            return dependency_index.status();
        }
        if (auto status = visit(dependency_index.value()); !status.ok())
        {
            return status;
        }
    }

    visit_state_[index] = VisitState::visited;
    return sorted_order_.push_back(index);
}

Status ModuleManager::sort_modules()
{
    sorted_order_ = {};
    for (usize i = 0; i < visit_state_.size(); ++i)
    {
        visit_state_[i] = VisitState::unvisited;
    }

    for (usize i = 0; i < modules_.size(); ++i)
    {
        if (auto status = check_dependencies(*modules_[i]); !status.ok())
        {
            modules_[i]->fail(status.message());
            return status;
        }
    }

    std::array<usize, max_kernel_modules> visit_order{};
    for (usize i = 0; i < modules_.size(); ++i)
    {
        visit_order[i] = i;
    }
    for (usize i = 0; i < modules_.size(); ++i)
    {
        for (usize j = i + 1; j < modules_.size(); ++j)
        {
            const auto left = modules_[visit_order[i]]->manifest();
            const auto right = modules_[visit_order[j]]->manifest();
            if (right.init_priority < left.init_priority)
            {
                const auto tmp = visit_order[i];
                visit_order[i] = visit_order[j];
                visit_order[j] = tmp;
            }
        }
    }

    for (usize i = 0; i < modules_.size(); ++i)
    {
        if (auto status = visit(visit_order[i]); !status.ok())
        {
            return status;
        }
    }

    return Status::success();
}

Status ModuleManager::check_required_services(KernelModule &module) const
{
    const auto manifest = module.manifest();
    for (const auto service_id : manifest.required_services)
    {
        if (!services_.contains(service_id))
        {
            module.fail("required service is missing");
            return Status::not_found("required service is missing");
        }
    }
    return Status::success();
}

Status ModuleManager::publish_services(KernelModule &module)
{
    const auto manifest = module.manifest();
    for (const auto service_id : manifest.exported_services)
    {
        auto *provider = module.service(service_id);
        if (provider == nullptr)
        {
            module.fail("module service provider is null");
            return Status::invalid_argument("module service provider is null");
        }
        if (services_.query_raw(service_id) == provider)
        {
            continue;
        }
        if (auto status = services_.register_service(service_id, provider); !status.ok())
        {
            module.fail(status.message());
            return status;
        }
    }
    return Status::success();
}

bool ModuleManager::started_order_contains(const KernelModule &module) const
{
    for (const auto *started : started_order_)
    {
        if (started == &module)
        {
            return true;
        }
    }
    return false;
}

Status ModuleManager::record_started(KernelModule &module)
{
    if (started_order_contains(module))
    {
        return Status::success();
    }
    return started_order_.push_back(&module);
}

Status ModuleManager::transition(KernelModule &module, ModuleState next, Status status)
{
    if (!status.ok())
    {
        module.fail(status.message());
        return status;
    }
    module.set_state(next);
    return Status::success();
}

Status ModuleManager::start_module(KernelModule &module)
{
    module.clear_failure();
    const auto manifest = module.manifest();
    if (auto status = validate_manifest(module); !status.ok())
    {
        module.fail(status.message());
        return status;
    }
    const bool already_recorded = started_order_contains(module);
    if (manifest.execution == ModuleExecution::kernel_process)
    {
        auto process = ensure_kernel_process(module);
        if (!process)
        {
            module.fail(process.status().message());
            return process.status();
        }
    }
    if (auto status = check_dependencies(module); !status.ok())
    {
        module.fail(status.message());
        return status;
    }
    if (auto status = transition(module, ModuleState::probed, module.probe()); !status.ok())
    {
        return status;
    }
    if (auto status = transition(module, ModuleState::initialized, module.init(services_)); !status.ok())
    {
        return status;
    }
    if (auto status = check_required_services(module); !status.ok())
    {
        return status;
    }
    if (auto status = transition(module, ModuleState::started, module.start(services_)); !status.ok())
    {
        return status;
    }
    if (auto status = publish_services(module); !status.ok())
    {
        return status;
    }
    if (auto status = record_started(module); !status.ok())
    {
        module.fail(status.message());
        return status;
    }
    if (manifest.execution == ModuleExecution::kernel_process && !already_recorded)
    {
        ++kernel_process_modules_;
    }
    return Status::success();
}

Status ModuleManager::start_all()
{
    if (auto status = sort_modules(); !status.ok())
    {
        return status;
    }

    started_order_ = {};
    kernel_process_modules_ = 0;
    for (const auto index : sorted_order_)
    {
        if (auto status = start_module(*modules_[index]); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status ModuleManager::start_registered_module(std::string_view name)
{
    auto *module = find(name);
    if (module == nullptr)
    {
        return Status::not_found("module is not registered");
    }
    return start_module(*module);
}

Status ModuleManager::restart_module(std::string_view name)
{
    auto *module = find(name);
    if (module == nullptr)
    {
        return Status::not_found("module is not registered");
    }
    if (module->manifest().restart_policy == ModuleRestartPolicy::never)
    {
        return Status::denied("module restart policy forbids restart");
    }
    if (module->state() == ModuleState::started)
    {
        if (auto status = module->stop(); !status.ok())
        {
            module->fail(status.message());
            return status;
        }
        module->set_state(ModuleState::stopped);
    }
    if (auto status = start_module(*module); !status.ok())
    {
        return status;
    }
    ++module->restart_count_;
    return Status::success();
}

Status ModuleManager::supervise_kernel_processes(StaticVector<ModuleProcessRestart, max_kernel_modules> &restarts)
{
    restarts.clear();
    if (kernel_process_scheduler_ == nullptr || kernel_process_arch_ == nullptr)
    {
        return Status::not_initialized("kernel module process is not bound to a scheduler");
    }

    for (usize i = 0; i < module_processes_.size(); ++i)
    {
        auto &record = module_processes_[i];
        if (record.pid != 0 && kernel_process_scheduler_->find(record.pid) != nullptr)
        {
            continue;
        }
        auto *module = find(record.module_name.view());
        if (module == nullptr || module->state() != ModuleState::started)
        {
            continue;
        }
        const auto manifest = module->manifest();
        if (manifest.restart_policy == ModuleRestartPolicy::never || manifest.restart_policy == ModuleRestartPolicy::manual)
        {
            continue;
        }

        const auto previous_pid = record.pid;
        if (auto status = restart_module(record.module_name.view()); !status.ok())
        {
            return status;
        }

        ModuleProcessRestart restart{.previous_pid = previous_pid, .pid = record.pid};
        if (auto status = assign_module_process_name(restart.process_name, record.module_name.view()); !status.ok())
        {
            return status;
        }
        if (auto status = restarts.push_back(restart); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status ModuleManager::stop_all()
{
    for (usize i = started_order_.size(); i != 0; --i)
    {
        auto &module = *started_order_[i - 1];
        if (auto status = transition(module, ModuleState::stopped, module.stop()); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status ModuleManager::shutdown_all()
{
    for (usize i = started_order_.size(); i != 0; --i)
    {
        auto &module = *started_order_[i - 1];
        if (auto status = module.shutdown(); !status.ok())
        {
            module.fail(status.message());
            return status;
        }
    }
    return Status::success();
}

std::string_view module_state_name(ModuleState state)
{
    switch (state)
    {
    case ModuleState::created:
        return "created";
    case ModuleState::probed:
        return "probed";
    case ModuleState::initialized:
        return "initialized";
    case ModuleState::started:
        return "started";
    case ModuleState::stopped:
        return "stopped";
    case ModuleState::failed:
        return "failed";
    }
    return "unknown";
}

std::string_view module_execution_name(ModuleExecution execution)
{
    switch (execution)
    {
    case ModuleExecution::inline_core:
        return "inline-core";
    case ModuleExecution::kernel_process:
        return "kernel-process";
    }
    return "unknown";
}

} // namespace ok
