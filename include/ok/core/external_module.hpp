#pragma once

#include "ok/core/module.hpp"
#include "ok/gui/desktop.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/smp/smp.hpp"

#include <array>

namespace ok
{

inline constexpr usize max_loaded_module_path = 96;
inline constexpr usize max_loaded_gui_text = 48;
inline constexpr usize max_external_gui_apps = 6;

enum class ExternalGuiDesktopState : u8
{
    unloaded,
    stopped,
    running,
};

enum class ExternalGuiAppState : u8
{
    unloaded,
    stopped,
    running,
};

class ExternalGuiDesktopModule final : public KernelModule, public KernelService
{
  public:
    void bind_metrics(const sched::Scheduler &scheduler, const smp::CpuTopology &topology)
    {
        scheduler_ = &scheduler;
        topology_ = &topology;
    }

    Status configure_from_image(std::string_view path, const ModuleImageInfo &image);

    [[nodiscard]] ModuleManifest manifest() const override;
    [[nodiscard]] std::string_view service_id() const override
    {
        return service_id_.view();
    }
    [[nodiscard]] void *service(std::string_view service_id) override;
    Status start(ServiceRegistry &services) override;
    Status stop() override;
    Status shutdown() override;
    Status refresh();

    [[nodiscard]] bool configured() const
    {
        return configured_;
    }
    [[nodiscard]] ExternalGuiDesktopState desktop_state() const
    {
        return desktop_state_;
    }
    [[nodiscard]] std::string_view module_name() const
    {
        return name_.view();
    }
    [[nodiscard]] std::string_view load_path() const
    {
        return load_path_.view();
    }
    [[nodiscard]] gui::SurfaceId dashboard_surface() const
    {
        return dashboard_surface_;
    }
    [[nodiscard]] usize render_count() const
    {
        return render_count_;
    }

  private:
    Status open_dashboard();
    Status render_dashboard();
    Status assign_parameter(std::string_view name, std::string_view value);

    bool configured_{false};
    FixedString<max_module_name> name_{};
    FixedString<max_module_name> version_{};
    FixedString<max_module_name> service_id_{};
    FixedString<max_loaded_module_path> load_path_{};
    FixedString<max_loaded_gui_text> brand_{"ObfuscationOS"};
    FixedString<max_loaded_gui_text> title_{"ObfuscationOS Login"};
    FixedString<max_loaded_gui_text> subtitle_{"default user root"};
    std::array<ModuleDependency, 1> dependencies_{{ModuleDependency{.name = gui::gui_module_name, .required = true}}};
    std::array<std::string_view, 2> required_services_{{gui::gui_service_id, gui::gui_desktop_service_id}};
    std::array<std::string_view, 1> exported_services_{};
    gui::GuiCompositor *compositor_{nullptr};
    gui::GuiDesktopService *desktop_{nullptr};
    const sched::Scheduler *scheduler_{nullptr};
    const smp::CpuTopology *topology_{nullptr};
    ExternalGuiDesktopState desktop_state_{ExternalGuiDesktopState::unloaded};
    gui::SurfaceId dashboard_surface_{0};
    usize render_count_{0};
};

class ExternalGuiAppModule final : public KernelModule, public KernelService
{
  public:
    Status configure_from_image(std::string_view path, const ModuleImageInfo &image);

    [[nodiscard]] ModuleManifest manifest() const override;
    [[nodiscard]] std::string_view service_id() const override
    {
        return service_id_.view();
    }
    [[nodiscard]] void *service(std::string_view service_id) override;
    Status start(ServiceRegistry &services) override;
    Status stop() override;
    Status shutdown() override;
    Status refresh();

    [[nodiscard]] bool configured() const
    {
        return configured_;
    }
    [[nodiscard]] ExternalGuiAppState app_state() const
    {
        return app_state_;
    }
    [[nodiscard]] std::string_view module_name() const
    {
        return name_.view();
    }
    [[nodiscard]] std::string_view load_path() const
    {
        return load_path_.view();
    }
    [[nodiscard]] gui::SurfaceId surface() const
    {
        return surface_;
    }
    [[nodiscard]] usize render_count() const
    {
        return render_count_;
    }

  private:
    Status open_window();
    Status render_window();
    Status assign_parameter(std::string_view name, std::string_view value);

    bool configured_{false};
    FixedString<max_module_name> name_{};
    FixedString<max_module_name> version_{};
    FixedString<max_module_name> service_id_{};
    FixedString<max_loaded_module_path> load_path_{};
    FixedString<max_loaded_gui_text> title_{"System App"};
    FixedString<max_loaded_gui_text> subtitle_{"C++ OOP GUI module"};
    FixedString<max_loaded_gui_text> body_{"Loaded from /boot/modules"};
    std::array<ModuleDependency, 1> dependencies_{{ModuleDependency{.name = gui::gui_module_name, .required = true}}};
    std::array<std::string_view, 3> required_services_{{gui::gui_service_id, gui::gui_desktop_service_id, {}}};
    usize required_service_count_{2};
    std::array<std::string_view, 1> exported_services_{};
    gui::GuiCompositor *compositor_{nullptr};
    gui::GuiDesktopService *desktop_{nullptr};
    ExternalGuiAppState app_state_{ExternalGuiAppState::unloaded};
    gui::SurfaceId surface_{0};
    gui::Rect bounds_{.x = 72, .y = 64, .width = 276, .height = 112};
    u32 accent_{0xffffcc66u};
    usize render_count_{0};
};

} // namespace ok
