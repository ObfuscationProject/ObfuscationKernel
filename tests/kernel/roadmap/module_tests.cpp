#include "roadmap_tests.hpp"

#include "ok/core/module.hpp"

#include <array>
#include <span>

namespace ok
{
namespace
{

class DebugModule final : public KernelModule
{
  public:
    DebugModule(std::string_view name, std::string_view klass, std::span<const ModuleDependency> dependencies,
                std::span<const std::string_view> exports, std::span<const std::string_view> required_services,
                u32 priority, usize *start_sequence = nullptr, usize *stop_sequence = nullptr,
                ModuleExecution execution = ModuleExecution::inline_core,
                ModuleThreading threading = ModuleThreading::single_threaded)
        : name_(name), klass_(klass), dependencies_(dependencies), exports_(exports), requires_(required_services),
          priority_(priority), start_sequence_(start_sequence), stop_sequence_(stop_sequence), execution_(execution),
          threading_(threading)
    {
    }

    [[nodiscard]] ModuleManifest manifest() const override
    {
        u64 capabilities = 0;
        if (!exports_.empty())
        {
            capabilities |= module_capability_bit(ModuleCapability::exports_services);
        }
        if (!requires_.empty())
        {
            capabilities |= module_capability_bit(ModuleCapability::requires_services);
        }
        if (execution_ == ModuleExecution::kernel_process)
        {
            capabilities |= module_capability_bit(ModuleCapability::owns_kernel_process);
        }
        if (threading_ == ModuleThreading::per_cpu)
        {
            capabilities |= module_capability_bit(ModuleCapability::uses_per_cpu_workers);
        }
        return ModuleManifest{
            .name = name_.view(),
            .version = "1",
            .module_class = klass_.view(),
            .dependencies = dependencies_,
            .exported_services = exports_,
            .required_services = requires_,
            .built_in = true,
            .execution = execution_,
            .init_priority = priority_,
            .threading = threading_,
            .capability_mask = capabilities,
            .resources = ModuleResourceBudget{.max_threads = sched::max_threads_per_process,
                                              .max_services = exports_.size() == 0 ? static_cast<usize>(4)
                                                                                  : exports_.size()},
        };
    }

    Status start(ServiceRegistry &) override
    {
        if (start_sequence_ != nullptr)
        {
            started_at_ = ++(*start_sequence_);
        }
        return Status::success();
    }

    Status stop() override
    {
        if (stop_sequence_ != nullptr)
        {
            stopped_at_ = ++(*stop_sequence_);
        }
        return Status::success();
    }

    [[nodiscard]] usize started_at() const
    {
        return started_at_;
    }
    [[nodiscard]] usize stopped_at() const
    {
        return stopped_at_;
    }

  private:
    FixedString<max_module_name> name_{};
    FixedString<max_module_name> klass_{};
    std::span<const ModuleDependency> dependencies_{};
    std::span<const std::string_view> exports_{};
    std::span<const std::string_view> requires_{};
    u32 priority_{0};
    usize *start_sequence_{nullptr};
    usize *stop_sequence_{nullptr};
    ModuleExecution execution_{ModuleExecution::inline_core};
    ModuleThreading threading_{ModuleThreading::single_threaded};
    usize started_at_{0};
    usize stopped_at_{0};
};

Status test_single_module()
{
    DebugModule module{"standalone", "test", {}, {}, {}, 0};
    ModuleManager manager;
    if (auto status = manager.register_module(module); !status.ok())
    {
        return status;
    }
    if (auto status = manager.start_all(); !status.ok())
    {
        return status;
    }
    if (manager.module_count() != 1 || manager.started_count() != 1 || module.state() != ModuleState::started)
    {
        return Status::fault("single module lifecycle validation failed");
    }
    if (auto status = manager.stop_all(); !status.ok())
    {
        return status;
    }
    return module.state() == ModuleState::stopped ? Status::success()
                                                  : Status::fault("single module stop validation failed");
}

Status test_missing_dependency()
{
    constexpr std::array<ModuleDependency, 1> missing_dep{ModuleDependency{.name = "missing", .required = true}};
    DebugModule missing{"needs-missing", "test", missing_dep, {}, {}, 0};
    ModuleManager manager;
    if (auto status = manager.register_module(missing); !status.ok())
    {
        return status;
    }
    if (manager.start_all().code() != StatusCode::not_found || missing.state() != ModuleState::failed)
    {
        return Status::fault("missing module dependency was not rejected");
    }
    return Status::success();
}

Status test_dependency_cycle()
{
    constexpr std::array<ModuleDependency, 1> dep_b{ModuleDependency{.name = "cycle-b", .required = true}};
    constexpr std::array<ModuleDependency, 1> dep_a{ModuleDependency{.name = "cycle-a", .required = true}};
    DebugModule cycle_a{"cycle-a", "test", dep_b, {}, {}, 0};
    DebugModule cycle_b{"cycle-b", "test", dep_a, {}, {}, 0};
    ModuleManager manager;
    static_cast<void>(manager.register_module(cycle_a));
    static_cast<void>(manager.register_module(cycle_b));
    if (manager.start_all().code() != StatusCode::invalid_argument || manager.failed_count() == 0)
    {
        return Status::fault("module dependency cycle was not rejected");
    }
    return Status::success();
}

Status test_missing_required_service()
{
    constexpr std::array<std::string_view, 1> required_service{"missing.service"};
    DebugModule module{"needs-service", "test", {}, {}, required_service, 0};
    ModuleManager manager;
    if (auto status = manager.register_module(module); !status.ok())
    {
        return status;
    }
    if (manager.start_all().code() != StatusCode::not_found || module.state() != ModuleState::failed)
    {
        return Status::fault("missing required service was not rejected");
    }
    return Status::success();
}

Status test_duplicate_service()
{
    constexpr std::array<std::string_view, 1> exports{"duplicate.service"};
    DebugModule first{"service-a", "test", {}, exports, {}, 0};
    DebugModule second{"service-b", "test", {}, exports, {}, 10};
    ModuleManager manager;
    static_cast<void>(manager.register_module(first));
    static_cast<void>(manager.register_module(second));
    if (manager.start_all().code() != StatusCode::already_exists || second.state() != ModuleState::failed ||
        manager.services().query_raw("duplicate.service") != &first)
    {
        return Status::fault("duplicate service was not rejected");
    }
    return Status::success();
}

Status test_dependency_priority_order()
{
    usize start_sequence = 0;
    constexpr std::array<ModuleDependency, 1> depends_provider{ModuleDependency{.name = "provider", .required = true}};
    DebugModule dependent{"dependent", "test", depends_provider, {}, {}, 0, &start_sequence};
    DebugModule provider{"provider", "test", {}, {}, {}, 100, &start_sequence};
    ModuleManager manager;
    static_cast<void>(manager.register_module(dependent));
    static_cast<void>(manager.register_module(provider));
    if (auto status = manager.start_all(); !status.ok())
    {
        return status;
    }
    if (provider.started_at() == 0 || dependent.started_at() == 0 || provider.started_at() >= dependent.started_at())
    {
        return Status::fault("module init priority overrode dependency order");
    }
    return Status::success();
}

class ManifestOverrideModule final : public KernelModule
{
  public:
    explicit ManifestOverrideModule(ModuleManifest manifest) : manifest_(manifest)
    {
    }

    [[nodiscard]] ModuleManifest manifest() const override
    {
        return manifest_;
    }

  private:
    ModuleManifest manifest_{};
};

void module_write_le16(std::span<std::byte> out, usize offset, u16 value)
{
    out[offset] = static_cast<std::byte>(value & 0xffu);
    out[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void module_write_le32(std::span<std::byte> out, usize offset, u32 value)
{
    module_write_le16(out, offset, static_cast<u16>(value & 0xffffu));
    module_write_le16(out, offset + 2, static_cast<u16>((value >> 16) & 0xffffu));
}

void module_write_le64(std::span<std::byte> out, usize offset, u64 value)
{
    module_write_le32(out, offset, static_cast<u32>(value & 0xffff'ffffu));
    module_write_le32(out, offset + 4, static_cast<u32>((value >> 32) & 0xffff'ffffu));
}

usize module_put_cstr(std::span<std::byte> out, usize offset, std::string_view value)
{
    for (usize i = 0; i < value.size(); ++i)
    {
        out[offset + i] = static_cast<std::byte>(value[i]);
    }
    out[offset + value.size()] = std::byte{0};
    return offset + value.size() + 1;
}

usize module_append_cstr(std::span<std::byte> out, usize &cursor, std::string_view value)
{
    const auto offset = cursor;
    cursor = module_put_cstr(out, cursor, value);
    return offset;
}

void module_write_elf64_section(std::span<std::byte> out, usize section_header_offset, u32 name_offset, u32 type,
                                u64 data_offset, u64 size, u64 entry_size)
{
    module_write_le32(out, section_header_offset, name_offset);
    module_write_le32(out, section_header_offset + 4, type);
    module_write_le64(out, section_header_offset + 24, data_offset);
    module_write_le64(out, section_header_offset + 32, size);
    module_write_le64(out, section_header_offset + 56, entry_size);
}

std::array<std::byte, 1024> make_fake_linux_ko()
{
    std::array<std::byte, 1024> image{};
    auto bytes = std::span<std::byte>{image.data(), image.size()};
    bytes[0] = std::byte{0x7f};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'L'};
    bytes[3] = std::byte{'F'};
    bytes[4] = std::byte{2};
    bytes[5] = std::byte{1};
    module_write_le16(bytes, 16, 1);
    module_write_le16(bytes, 18, 0x3e);
    module_write_le64(bytes, 40, 512);
    module_write_le16(bytes, 52, 64);
    module_write_le16(bytes, 58, 64);
    module_write_le16(bytes, 60, 7);
    module_write_le16(bytes, 62, 6);

    usize modinfo_cursor = 128;
    module_put_cstr(bytes, modinfo_cursor, "name=virtio_blk");
    modinfo_cursor += 16;
    module_put_cstr(bytes, modinfo_cursor, "vermagic=mainline-test SMP");
    modinfo_cursor += 28;
    module_put_cstr(bytes, modinfo_cursor, "parm=queue_depth:int");
    modinfo_cursor += 21;
    module_put_cstr(bytes, modinfo_cursor, "depends=virtio_pci");
    modinfo_cursor += 19;
    module_put_cstr(bytes, modinfo_cursor, "signature=test-sig");
    modinfo_cursor += 19;
    module_put_cstr(bytes, modinfo_cursor, "export=virtblk_probe");
    modinfo_cursor += 21;
    module_put_cstr(bytes, modinfo_cursor, "import=pci_register_driver");
    modinfo_cursor += 27;
    const auto modinfo_size = modinfo_cursor - 128;

    usize shstr_cursor = 384;
    bytes[shstr_cursor++] = std::byte{0};
    const auto modinfo_name = module_append_cstr(bytes, shstr_cursor, ".modinfo") - 384;
    const auto ksymtab_name = module_append_cstr(bytes, shstr_cursor, "__ksymtab") - 384;
    const auto init_name = module_append_cstr(bytes, shstr_cursor, ".init.text") - 384;
    const auto exit_name = module_append_cstr(bytes, shstr_cursor, ".exit.text") - 384;
    const auto rela_name = module_append_cstr(bytes, shstr_cursor, ".rela.text") - 384;
    const auto shstr_name = module_append_cstr(bytes, shstr_cursor, ".shstrtab") - 384;
    const auto shstr_size = shstr_cursor - 384;

    module_write_elf64_section(bytes, 512 + 64, static_cast<u32>(modinfo_name), 1, 128, modinfo_size, 0);
    module_write_elf64_section(bytes, 512 + 64 * 2, static_cast<u32>(ksymtab_name), 1, 256, 16, 0);
    module_write_elf64_section(bytes, 512 + 64 * 3, static_cast<u32>(init_name), 1, 272, 16, 0);
    module_write_elf64_section(bytes, 512 + 64 * 4, static_cast<u32>(exit_name), 1, 288, 16, 0);
    module_write_elf64_section(bytes, 512 + 64 * 5, static_cast<u32>(rela_name), 4, 304, 24, 24);
    module_write_elf64_section(bytes, 512 + 64 * 6, static_cast<u32>(shstr_name), 3, 384, shstr_size, 0);
    return image;
}

Status test_module_abi_contract()
{
    ManifestOverrideModule wrong_abi{ModuleManifest{
        .name = "wrong-abi",
        .version = "1",
        .module_class = "test",
        .abi_version = kernel_module_abi_version + 1,
    }};
    ModuleManager wrong_abi_manager;
    if (wrong_abi_manager.register_module(wrong_abi).code() != StatusCode::unsupported ||
        wrong_abi.state() != ModuleState::failed)
    {
        return Status::fault("unsupported module ABI version was not rejected");
    }

    static constexpr std::array<std::string_view, 1> exports{"bad.service"};
    ManifestOverrideModule missing_capability{ModuleManifest{
        .name = "missing-capability",
        .version = "1",
        .module_class = "test",
        .exported_services = exports,
        .abi_version = kernel_module_abi_version,
    }};
    ModuleManager capability_manager;
    if (capability_manager.register_module(missing_capability).code() != StatusCode::denied ||
        missing_capability.state() != ModuleState::failed)
    {
        return Status::fault("module capability declaration was not enforced");
    }

    DebugModule restartable{"restartable", "test", {}, {}, {}, 0};
    ModuleManager restart_manager;
    if (auto status = restart_manager.register_module(restartable); !status.ok())
    {
        return status;
    }
    if (auto status = restart_manager.start_all(); !status.ok())
    {
        return status;
    }
    if (auto status = restart_manager.restart_module("restartable"); !status.ok())
    {
        return status;
    }
    if (restartable.restart_count() != 1 || restartable.state() != ModuleState::started)
    {
        return Status::fault("module restart count or state was not recorded");
    }

    ManifestOverrideModule no_restart{ModuleManifest{
        .name = "no-restart",
        .version = "1",
        .module_class = "test",
        .abi_version = kernel_module_abi_version,
        .restart_policy = ModuleRestartPolicy::never,
    }};
    ModuleManager no_restart_manager;
    if (auto status = no_restart_manager.register_module(no_restart); !status.ok())
    {
        return status;
    }
    if (auto status = no_restart_manager.start_all(); !status.ok())
    {
        return status;
    }
    if (no_restart_manager.restart_module("no-restart").code() != StatusCode::denied)
    {
        return Status::fault("module restart policy was not enforced");
    }

    return Status::success();
}

Status test_module_image_loader_and_symbol_registry()
{
    ModuleImageLoader loader;
    constexpr std::string_view okmod_text{
        "OKMOD\n"
        "name=virtio-net-ok\n"
        "version=1\n"
        "vermagic=mainline-tracking\n"
        "export=virtio_net_probe\n"
        "require=pci_register_driver\n"
        "param=queues:int\n"
        "reloc=0x40\n"
        "signature=test-signature\n"};
    auto okmod = loader.parse(std::span<const std::byte>{reinterpret_cast<const std::byte *>(okmod_text.data()),
                                                         okmod_text.size()},
                              arch::Architecture::x86_64);
    if (!okmod || okmod.value().format != ModuleImageFormat::okmod || okmod.value().name.view() != "virtio-net-ok" ||
        okmod.value().exports.size() != 1 || okmod.value().imports.size() != 1 ||
        okmod.value().parameters.size() != 1 || !okmod.value().signed_image ||
        okmod.value().relocation_count != 1)
    {
        return Status::fault("okmod image metadata validation failed");
    }

    ModuleSymbolRegistry symbols;
    if (auto status = symbols.export_symbol("pci_register_driver", 0xfeed); !status.ok())
    {
        return status;
    }
    if (auto status = symbols.resolve_imports(okmod.value()); !status.ok())
    {
        return status;
    }
    if (!okmod.value().imports[0].resolved || okmod.value().imports[0].address != 0xfeed ||
        symbols.resolve("missing").status().code() != StatusCode::not_found)
    {
        return Status::fault("module symbol registry validation failed");
    }

    auto linux_image = make_fake_linux_ko();
    auto linux_module = loader.parse_linux_ko(linux_image, arch::Architecture::x86_64);
    if (!linux_module || linux_module.value().format != ModuleImageFormat::linux_ko ||
        linux_module.value().name.view() != "virtio_blk" || linux_module.value().architecture != arch::Architecture::x86_64 ||
        !linux_module.value().relocatable || !linux_module.value().has_modinfo ||
        !linux_module.value().has_kallsyms || !linux_module.value().has_init || !linux_module.value().has_exit ||
        !linux_module.value().signed_image || linux_module.value().relocation_count != 1 ||
        linux_module.value().imports.size() != 1 || linux_module.value().exports.size() != 1 ||
        linux_module.value().parameters.size() < 2)
    {
        return Status::fault("Linux .ko metadata validation failed");
    }

    LinuxAbiSnapshot snapshot;
    if (auto status = snapshot.begin("mainline-tracking", true); !status.ok())
    {
        return status;
    }
    if (auto status = snapshot.record_required_symbol("pci_register_driver"); !status.ok())
    {
        return status;
    }
    if (auto status = snapshot.record_required_symbol("dma_map_single"); !status.ok())
    {
        return status;
    }
    if (auto status = snapshot.record_implemented_symbol("pci_register_driver"); !status.ok())
    {
        return status;
    }
    if (auto status = snapshot.record_layout("struct pci_driver", 64, 8); !status.ok())
    {
        return status;
    }
    if (!snapshot.tracks_mainline() || snapshot.required_symbol_count() != 2 ||
        snapshot.implemented_symbol_count() != 1 || snapshot.layout_count() != 1 || snapshot.coverage_x100() != 5000)
    {
        return Status::fault("Linux mainline ABI snapshot validation failed");
    }

    return Status::success();
}

Status test_kernel_process_backed_module()
{
    auto &ops = arch::arch_operations(arch::configured_architecture());
    sched::Scheduler scheduler;
    if (auto status = scheduler.configure_cpus(4); !status.ok())
    {
        return status;
    }

    DebugModule module{"gui-worker", "gui", {}, {}, {}, 0, nullptr, nullptr, ModuleExecution::kernel_process};
    ModuleManager manager;
    if (auto status = manager.register_module(module); !status.ok())
    {
        return status;
    }
    if (manager.start_all().code() != StatusCode::not_initialized || module.state() != ModuleState::failed)
    {
        return Status::fault("kernel-process module started without a module process binding");
    }

    ModuleManager bound_manager;
    DebugModule bound_module{"gui-worker",
                             "gui",
                             {},
                             {},
                             {},
                             0,
                             nullptr,
                             nullptr,
                             ModuleExecution::kernel_process,
                             ModuleThreading::per_cpu};
    if (auto status = bound_manager.bind_kernel_process(scheduler, ops, 0x3000, 0xa000); !status.ok())
    {
        return status;
    }
    if (auto status = bound_manager.register_module(bound_module); !status.ok())
    {
        return status;
    }
    if (auto status = bound_manager.start_all(); !status.ok())
    {
        return status;
    }
    const auto pid = bound_manager.kernel_process_pid();
    auto *process = scheduler.find(pid);
    if (pid == 0 || process == nullptr || process->name() != "mod:gui-worker" || !process->background() ||
        bound_manager.kernel_process_module_count() != 1 || scheduler.background_process_count() != 1 ||
        process->threads().size() != scheduler.cpu_count() || bound_module.state() != ModuleState::started ||
        module_execution_name(bound_module.manifest().execution) != "kernel-process")
    {
        return Status::fault("kernel-process module manager binding failed");
    }
    if (auto status = bound_manager.restart_module("gui-worker"); !status.ok())
    {
        return status;
    }
    if (bound_manager.kernel_process_pid() != pid || bound_manager.kernel_process_module_count() != 1)
    {
        return Status::fault("kernel-process module restart did not reuse module process");
    }
    return Status::success();
}

Status test_builtin_module_graph(KernelTestReport &report)
{
    usize start_sequence = 0;
    usize stop_sequence = 0;
    constexpr std::array<std::string_view, 1> arch_exports{"arch.ops"};
    constexpr std::array<std::string_view, 1> memory_exports{"memory.frames"};
    constexpr std::array<std::string_view, 1> vfs_exports{"vfs.root"};
    constexpr std::array<std::string_view, 1> required_arch{"arch.ops"};
    constexpr std::array<ModuleDependency, 1> depends_arch{ModuleDependency{.name = "arch", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_interrupt{
        ModuleDependency{.name = "interrupt", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_scheduler{
        ModuleDependency{.name = "scheduler", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_driver{ModuleDependency{.name = "driver-core", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_vfs{ModuleDependency{.name = "vfs", .required = true}};
    constexpr std::array<ModuleDependency, 2> depends_vfs_and_syscall{
        ModuleDependency{.name = "vfs", .required = true},
        ModuleDependency{.name = "syscall", .required = true},
    };

    DebugModule arch_module{"arch", "architecture", {}, arch_exports, {}, 0, &start_sequence, &stop_sequence};
    DebugModule memory_module{"memory",      "memory", depends_arch,    memory_exports,
                              required_arch, 10,       &start_sequence, &stop_sequence};
    DebugModule interrupt_module{"interrupt", "interrupt", depends_arch, {}, {}, 20, &start_sequence, &stop_sequence};
    DebugModule scheduler_module{"scheduler", "scheduler", depends_interrupt, {},
                                 {},          30,          &start_sequence,   &stop_sequence};
    DebugModule smp_module{"smp", "smp", depends_scheduler, {}, {}, 40, &start_sequence, &stop_sequence};
    DebugModule ipc_module{"ipc", "ipc", depends_scheduler, {}, {}, 50, &start_sequence, &stop_sequence};
    DebugModule syscall_module{"syscall", "syscall", depends_scheduler, {}, {}, 60, &start_sequence, &stop_sequence};
    DebugModule driver_module{"driver-core", "driver", depends_interrupt, {}, {}, 70, &start_sequence, &stop_sequence};
    DebugModule vfs_module{"vfs", "filesystem", depends_driver, vfs_exports, {}, 80, &start_sequence, &stop_sequence};
    DebugModule posix_module{"posix", "posix", depends_vfs_and_syscall, {}, {}, 90, &start_sequence, &stop_sequence};
    DebugModule user_module{"user-mode", "user", depends_scheduler, {}, {}, 100, &start_sequence, &stop_sequence};
    DebugModule shell_module{"debug-shell", "shell", depends_vfs, {}, {}, 110, &start_sequence, &stop_sequence};

    ModuleManager manager;
    for (auto *module : {&arch_module, &memory_module, &interrupt_module, &scheduler_module, &smp_module, &ipc_module,
                         &syscall_module, &driver_module, &vfs_module, &posix_module, &user_module, &shell_module})
    {
        if (auto status = manager.register_module(*module); !status.ok())
        {
            return status;
        }
    }
    if (manager.register_module(arch_module).code() != StatusCode::already_exists)
    {
        return Status::fault("duplicate module name was not rejected");
    }
    if (auto status = manager.start_all(); !status.ok())
    {
        return status;
    }
    if (manager.failed_count() != 0 || manager.module_count() != 12 || manager.started_count() != 12 ||
        manager.services().query<DebugModule>("memory.frames") != &memory_module ||
        manager.services().query_raw("missing.service") != nullptr ||
        arch_module.started_at() >= memory_module.started_at() ||
        arch_module.started_at() >= interrupt_module.started_at() ||
        interrupt_module.started_at() >= scheduler_module.started_at() ||
        scheduler_module.started_at() >= smp_module.started_at() ||
        scheduler_module.started_at() >= ipc_module.started_at() ||
        scheduler_module.started_at() >= syscall_module.started_at() ||
        interrupt_module.started_at() >= driver_module.started_at() ||
        driver_module.started_at() >= vfs_module.started_at() || vfs_module.started_at() >= posix_module.started_at() ||
        syscall_module.started_at() >= posix_module.started_at() ||
        scheduler_module.started_at() >= user_module.started_at() ||
        vfs_module.started_at() >= shell_module.started_at())
    {
        return Status::fault("module manager dependency or service validation failed");
    }
    if (auto status = manager.stop_all(); !status.ok())
    {
        return status;
    }
    if (memory_module.stopped_at() >= arch_module.stopped_at() ||
        scheduler_module.stopped_at() >= interrupt_module.stopped_at() ||
        vfs_module.stopped_at() >= driver_module.stopped_at() || posix_module.stopped_at() >= vfs_module.stopped_at() ||
        shell_module.stopped_at() >= vfs_module.stopped_at())
    {
        return Status::fault("module manager stop order is not reverse dependency order");
    }

    report.modules = true;
    report.module_count = manager.module_count();
    report.module_failed_count = manager.failed_count();
    return Status::success();
}

} // namespace

Status run_module_roadmap_tests(KernelTestReport &report)
{
    if (auto status = test_single_module(); !status.ok())
    {
        return status;
    }
    if (auto status = test_missing_dependency(); !status.ok())
    {
        return status;
    }
    if (auto status = test_dependency_cycle(); !status.ok())
    {
        return status;
    }
    if (auto status = test_missing_required_service(); !status.ok())
    {
        return status;
    }
    if (auto status = test_duplicate_service(); !status.ok())
    {
        return status;
    }
    if (auto status = test_dependency_priority_order(); !status.ok())
    {
        return status;
    }
    if (auto status = test_module_abi_contract(); !status.ok())
    {
        return status;
    }
    if (auto status = test_module_image_loader_and_symbol_registry(); !status.ok())
    {
        return status;
    }
    if (auto status = test_kernel_process_backed_module(); !status.ok())
    {
        return status;
    }
    return test_builtin_module_graph(report);
}

} // namespace ok
