#include "ok/core/external_module.hpp"

namespace ok
{
namespace
{

constexpr gui::Rect dashboard_bounds{.x = 34, .y = 28, .width = 328, .height = 164};
constexpr u32 dashboard_background = 0xff101820u;
constexpr u32 dashboard_panel = 0xff172331u;
constexpr u32 dashboard_panel_alt = 0xff203344u;
constexpr u32 dashboard_accent = 0xff44aa88u;
constexpr u32 dashboard_warm = 0xffffcc66u;
constexpr u32 dashboard_text = 0xffd8f3ffu;
constexpr u32 dashboard_muted = 0xff9fc6d2u;
constexpr u32 app_background = 0xff101820u;
constexpr u32 app_panel = 0xff172331u;
constexpr u32 app_panel_alt = 0xff203344u;
constexpr u32 app_text = 0xffd8f3ffu;
constexpr u32 app_muted = 0xff9fc6d2u;

template <usize Capacity>
bool has_symbol(const StaticVector<ModuleSymbol, Capacity> &symbols, std::string_view name)
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

bool has_parameter(const ModuleImageInfo &image, std::string_view name, std::string_view value)
{
    for (const auto &parameter : image.parameters)
    {
        if (parameter.name.view() == name && parameter.value.view() == value)
        {
            return true;
        }
    }
    return false;
}

template <usize Capacity>
Status append_decimal(FixedString<Capacity> &out, u64 value)
{
    constexpr u64 powers[] = {
        10'000'000'000'000'000'000ull,
        1'000'000'000'000'000'000ull,
        100'000'000'000'000'000ull,
        10'000'000'000'000'000ull,
        1'000'000'000'000'000ull,
        100'000'000'000'000ull,
        10'000'000'000'000ull,
        1'000'000'000'000ull,
        100'000'000'000ull,
        10'000'000'000ull,
        1'000'000'000ull,
        100'000'000ull,
        10'000'000ull,
        1'000'000ull,
        100'000ull,
        10'000ull,
        1'000ull,
        100ull,
        10ull,
        1ull,
    };
    bool started = false;
    for (const auto power : powers)
    {
        u8 digit = 0;
        while (value >= power)
        {
            value -= power;
            ++digit;
        }
        if (digit != 0 || started || power == 1)
        {
            if (auto status = out.append(static_cast<char>('0' + digit)); !status.ok())
            {
                return status;
            }
            started = true;
        }
    }
    return Status::success();
}

template <usize Capacity>
Status assign_metric(FixedString<Capacity> &out, std::string_view label, u64 value)
{
    out.clear();
    if (auto status = out.append(label); !status.ok())
    {
        return status;
    }
    return append_decimal(out, value);
}

Result<u32> parse_decimal_u32(std::string_view value)
{
    if (value.empty())
    {
        return Status::invalid_argument("empty decimal parameter");
    }
    u32 out = 0;
    for (const auto ch : value)
    {
        if (ch < '0' || ch > '9')
        {
            return Status::invalid_argument("decimal parameter contains a non-digit");
        }
        const auto digit = static_cast<u32>(ch - '0');
        if (out > (0xffff'ffffu - digit) / 10u)
        {
            return Status::overflow("decimal parameter overflow");
        }
        out = out * 10u + digit;
    }
    return out;
}

u32 accent_for(std::string_view value)
{
    if (value == "rose")
    {
        return 0xffff7799u;
    }
    if (value == "blue")
    {
        return 0xff66b7ffu;
    }
    if (value == "mint")
    {
        return 0xff44aa88u;
    }
    if (value == "violet")
    {
        return 0xffaa99ffu;
    }
    return dashboard_warm;
}

gui::Rect fit_bounds(gui::Rect bounds, gui::Rect desktop)
{
    if (bounds.width > desktop.width)
    {
        bounds.width = desktop.width;
        bounds.x = 0;
    }
    if (bounds.height + gui::taskbar_height > desktop.height)
    {
        bounds.height = desktop.height > gui::taskbar_height ? desktop.height - gui::taskbar_height : desktop.height;
        bounds.y = 0;
    }
    const auto work_height = desktop.height > gui::taskbar_height ? desktop.height - gui::taskbar_height : desktop.height;
    if (bounds.x < 0)
    {
        bounds.x = 0;
    }
    if (bounds.y < 0)
    {
        bounds.y = 0;
    }
    if (static_cast<u32>(bounds.x) + bounds.width > desktop.width)
    {
        bounds.x = desktop.width > bounds.width ? static_cast<i32>(desktop.width - bounds.width) : 0;
    }
    if (static_cast<u32>(bounds.y) + bounds.height > work_height)
    {
        bounds.y = work_height > bounds.height ? static_cast<i32>(work_height - bounds.height) : 0;
    }
    return bounds;
}

} // namespace

Status ExternalGuiDesktopModule::configure_from_image(std::string_view path, const ModuleImageInfo &image)
{
    if (image.format != ModuleImageFormat::okmod)
    {
        return Status::unsupported("external C++ OOP module must use OKMOD metadata");
    }
    if (!has_parameter(image, "entry", "oop") || !has_parameter(image, "class", "desktop"))
    {
        return Status::unsupported("external GUI module requires entry:oop and class:desktop");
    }
    if (!has_symbol(image.imports, gui::gui_service_id) || !has_symbol(image.imports, gui::gui_desktop_service_id) ||
        image.exports.empty())
    {
        return Status::invalid_argument("external GUI module metadata is incomplete");
    }
    if (image.name.empty())
    {
        return Status::invalid_argument("external GUI module name is empty");
    }

    if (auto status = name_.assign(image.name.view()); !status.ok())
    {
        return status;
    }
    if (auto status = version_.assign(image.version.empty() ? std::string_view{"1"} : image.version.view());
        !status.ok())
    {
        return status;
    }
    if (auto status = service_id_.assign(image.exports[0].name.view()); !status.ok())
    {
        return status;
    }
    if (auto status = load_path_.assign(path); !status.ok())
    {
        return status;
    }
    exported_services_[0] = service_id_.view();
    for (const auto &parameter : image.parameters)
    {
        if (auto status = assign_parameter(parameter.name.view(), parameter.value.view()); !status.ok())
        {
            return status;
        }
    }
    configured_ = true;
    desktop_state_ = ExternalGuiDesktopState::stopped;
    dashboard_surface_ = 0;
    render_count_ = 0;
    return Status::success();
}

Status ExternalGuiDesktopModule::assign_parameter(std::string_view name, std::string_view value)
{
    if (name == "brand")
    {
        return brand_.assign(value);
    }
    if (name == "title")
    {
        return title_.assign(value);
    }
    if (name == "subtitle")
    {
        return subtitle_.assign(value);
    }
    return Status::success();
}

ModuleManifest ExternalGuiDesktopModule::manifest() const
{
    return ModuleManifest{
        .name = name_.empty() ? std::string_view{"external-gui-desktop"} : name_.view(),
        .version = version_.empty() ? std::string_view{"1"} : version_.view(),
        .module_class = "desktop",
        .dependencies = dependencies_,
        .exported_services = exported_services_,
        .required_services = required_services_,
        .built_in = false,
        .execution = ModuleExecution::inline_core,
        .init_priority = 85,
        .threading = ModuleThreading::single_threaded,
        .capability_mask = module_capability_bit(ModuleCapability::exports_services) |
                           module_capability_bit(ModuleCapability::requires_services) |
                           module_capability_bit(ModuleCapability::handles_gui),
        .restart_policy = ModuleRestartPolicy::manual,
        .resources = ModuleResourceBudget{.max_threads = 1,
                                          .max_services = exported_services_.size(),
                                          .max_memory_pages = 1,
                                          .max_handles = 2},
    };
}

void *ExternalGuiDesktopModule::service(std::string_view service_id)
{
    return service_id == service_id_.view() ? this : nullptr;
}

Status ExternalGuiDesktopModule::start(ServiceRegistry &services)
{
    if (!configured_)
    {
        return Status::not_initialized("external GUI desktop module is not configured");
    }
    compositor_ = services.query<gui::GuiCompositor>(gui::gui_service_id);
    desktop_ = services.query<gui::GuiDesktopService>(gui::gui_desktop_service_id);
    if (compositor_ == nullptr || desktop_ == nullptr)
    {
        return Status::not_initialized("external GUI desktop module requires GUI services");
    }
    if (scheduler_ == nullptr || topology_ == nullptr)
    {
        return Status::not_initialized("external GUI desktop metrics are not bound");
    }
    if (auto status = open_dashboard(); !status.ok())
    {
        return status;
    }
    desktop_state_ = ExternalGuiDesktopState::running;
    return Status::success();
}

Status ExternalGuiDesktopModule::stop()
{
    if (desktop_ != nullptr && dashboard_surface_ != 0)
    {
        auto status = desktop_->close_window(dashboard_surface_);
        if (!status.ok() && status.code() != StatusCode::not_found)
        {
            return status;
        }
    }
    dashboard_surface_ = 0;
    desktop_state_ = configured_ ? ExternalGuiDesktopState::stopped : ExternalGuiDesktopState::unloaded;
    compositor_ = nullptr;
    desktop_ = nullptr;
    return Status::success();
}

Status ExternalGuiDesktopModule::shutdown()
{
    return stop();
}

Status ExternalGuiDesktopModule::refresh()
{
    if (desktop_state_ != ExternalGuiDesktopState::running)
    {
        return Status::not_initialized("external GUI desktop is not running");
    }
    if (dashboard_surface_ == 0)
    {
        return open_dashboard();
    }
    auto status = render_dashboard();
    if (status.code() == StatusCode::not_found)
    {
        return open_dashboard();
    }
    return status;
}

Status ExternalGuiDesktopModule::open_dashboard()
{
    auto desktop = compositor_->desktop_bounds();
    if (!desktop)
    {
        return desktop.status();
    }
    auto bounds = dashboard_bounds;
    if (desktop.value().width > bounds.width)
    {
        bounds.x = static_cast<i32>((desktop.value().width - bounds.width) / 2);
    }
    if (desktop.value().height > bounds.height + gui::taskbar_height)
    {
        bounds.y = static_cast<i32>((desktop.value().height - gui::taskbar_height - bounds.height) / 2);
    }

    auto surface = desktop_->open_window(gui::DesktopWindowRequest{
        .bounds = bounds,
        .title = title_.view(),
        .app = gui::TaskbarApp::task_monitor,
    });
    if (!surface)
    {
        return surface.status();
    }
    dashboard_surface_ = surface.value();
    return render_dashboard();
}

Status ExternalGuiDesktopModule::render_dashboard()
{
    if (dashboard_surface_ == 0)
    {
        return Status::not_initialized("external GUI desktop dashboard is not open");
    }
    auto info = compositor_->surface_info(dashboard_surface_);
    if (!info)
    {
        dashboard_surface_ = 0;
        return Status::not_found("external GUI desktop dashboard surface is gone");
    }

    if (auto status = compositor_->fill(dashboard_surface_, dashboard_background); !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_,
                                             gui::Rect{.x = 8,
                                                       .y = 18,
                                                       .width = info.value().bounds.width - 16,
                                                       .height = 48},
                                             dashboard_panel);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_, gui::Rect{.x = 8, .y = 68, .width = 94, .height = 76},
                                             dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status =
            compositor_->fill_rect(dashboard_surface_, gui::Rect{.x = 106, .y = 68, .width = 98, .height = 76},
                                   dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status =
            compositor_->fill_rect(dashboard_surface_, gui::Rect{.x = 208, .y = 68, .width = 112, .height = 76},
                                   dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }

    for (u32 y = 24; y < 54; ++y)
    {
        for (u32 x = 18; x < 68; ++x)
        {
            const auto dx = static_cast<i32>(x) - 43;
            const auto dy = static_cast<i32>(y) - 39;
            const auto distance = dx * dx + dy * dy;
            if (distance <= 18 * 18 && distance >= 9 * 9)
            {
                if (auto status = compositor_->put_pixel(dashboard_surface_, x, y, dashboard_accent); !status.ok())
                {
                    return status;
                }
            }
        }
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_, gui::Rect{.x = 76, .y = 28, .width = 5, .height = 26},
                                             dashboard_warm);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_, gui::Rect{.x = 82, .y = 40, .width = 28, .height = 5},
                                             dashboard_warm);
        !status.ok())
    {
        return status;
    }

    if (auto status =
            compositor_->draw_text(dashboard_surface_, 13, 2, brand_.view(), dashboard_text, dashboard_panel);
        !status.ok())
    {
        return status;
    }
    if (auto status =
            compositor_->draw_text(dashboard_surface_, 13, 4, subtitle_.view(), dashboard_muted, dashboard_panel);
        !status.ok())
    {
        return status;
    }

    FixedString<64> metric;
    if (auto status = assign_metric(metric, "CPUs online: ", topology_->online_count()); !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(dashboard_surface_, 2, 8, metric.view(), dashboard_text,
                                             dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status = assign_metric(metric, "Processes: ", scheduler_->process_count()); !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(dashboard_surface_, 16, 8, metric.view(), dashboard_text,
                                             dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status = assign_metric(metric, "Background: ", scheduler_->background_process_count()); !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(dashboard_surface_, 29, 8, metric.view(), dashboard_text,
                                             dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(dashboard_surface_, 2, 12, "Launchers: shell files tasks",
                                             dashboard_muted, dashboard_background);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->present(); !status.ok())
    {
        return status;
    }
    ++render_count_;
    return Status::success();
}

Status ExternalGuiAppModule::configure_from_image(std::string_view path, const ModuleImageInfo &image)
{
    if (image.format != ModuleImageFormat::okmod)
    {
        return Status::unsupported("external C++ OOP module must use OKMOD metadata");
    }
    if (!has_parameter(image, "entry", "oop") || !has_parameter(image, "class", "app"))
    {
        return Status::unsupported("external GUI app requires entry:oop and class:app");
    }
    if (!has_symbol(image.imports, gui::gui_service_id) || !has_symbol(image.imports, gui::gui_desktop_service_id) ||
        image.exports.empty())
    {
        return Status::invalid_argument("external GUI app metadata is incomplete");
    }
    if (image.name.empty())
    {
        return Status::invalid_argument("external GUI app name is empty");
    }

    if (auto status = name_.assign(image.name.view()); !status.ok())
    {
        return status;
    }
    if (auto status = version_.assign(image.version.empty() ? std::string_view{"1"} : image.version.view());
        !status.ok())
    {
        return status;
    }
    if (auto status = service_id_.assign(image.exports[0].name.view()); !status.ok())
    {
        return status;
    }
    if (auto status = load_path_.assign(path); !status.ok())
    {
        return status;
    }

    exported_services_[0] = service_id_.view();
    required_services_[0] = gui::gui_service_id;
    required_services_[1] = gui::gui_desktop_service_id;
    required_services_[2] = {};
    required_service_count_ = 2;
    if (has_symbol(image.imports, "gui.system-desktop"))
    {
        required_services_[2] = "gui.system-desktop";
        required_service_count_ = 3;
    }

    if (auto status = title_.assign(image.name.view()); !status.ok())
    {
        return status;
    }
    if (auto status = subtitle_.assign("C++ OOP GUI module"); !status.ok())
    {
        return status;
    }
    if (auto status = body_.assign("Loaded from /boot/modules"); !status.ok())
    {
        return status;
    }
    bounds_ = gui::Rect{.x = 72, .y = 64, .width = 276, .height = 112};
    accent_ = dashboard_warm;
    for (const auto &parameter : image.parameters)
    {
        if (auto status = assign_parameter(parameter.name.view(), parameter.value.view()); !status.ok())
        {
            return status;
        }
    }
    configured_ = true;
    app_state_ = ExternalGuiAppState::stopped;
    surface_ = 0;
    render_count_ = 0;
    return Status::success();
}

Status ExternalGuiAppModule::assign_parameter(std::string_view name, std::string_view value)
{
    if (name == "title")
    {
        return title_.assign(value);
    }
    if (name == "subtitle")
    {
        return subtitle_.assign(value);
    }
    if (name == "body")
    {
        return body_.assign(value);
    }
    if (name == "accent")
    {
        accent_ = accent_for(value);
        return Status::success();
    }
    if (name == "x" || name == "y" || name == "width" || name == "height")
    {
        auto parsed = parse_decimal_u32(value);
        if (!parsed)
        {
            return parsed.status();
        }
        if (name == "x")
        {
            bounds_.x = static_cast<i32>(parsed.value());
        }
        else if (name == "y")
        {
            bounds_.y = static_cast<i32>(parsed.value());
        }
        else if (name == "width")
        {
            bounds_.width = parsed.value();
        }
        else
        {
            bounds_.height = parsed.value();
        }
    }
    return Status::success();
}

ModuleManifest ExternalGuiAppModule::manifest() const
{
    return ModuleManifest{
        .name = name_.empty() ? std::string_view{"external-gui-app"} : name_.view(),
        .version = version_.empty() ? std::string_view{"1"} : version_.view(),
        .module_class = "gui-app",
        .dependencies = dependencies_,
        .exported_services = exported_services_,
        .required_services = std::span<const std::string_view>{required_services_.data(), required_service_count_},
        .built_in = false,
        .execution = ModuleExecution::inline_core,
        .init_priority = 90,
        .threading = ModuleThreading::single_threaded,
        .capability_mask = module_capability_bit(ModuleCapability::exports_services) |
                           module_capability_bit(ModuleCapability::requires_services) |
                           module_capability_bit(ModuleCapability::handles_gui),
        .restart_policy = ModuleRestartPolicy::manual,
        .resources = ModuleResourceBudget{.max_threads = 1,
                                          .max_services = exported_services_.size(),
                                          .max_memory_pages = 1,
                                          .max_handles = 2},
    };
}

void *ExternalGuiAppModule::service(std::string_view service_id)
{
    return service_id == service_id_.view() ? this : nullptr;
}

Status ExternalGuiAppModule::start(ServiceRegistry &services)
{
    if (!configured_)
    {
        return Status::not_initialized("external GUI app module is not configured");
    }
    compositor_ = services.query<gui::GuiCompositor>(gui::gui_service_id);
    desktop_ = services.query<gui::GuiDesktopService>(gui::gui_desktop_service_id);
    if (compositor_ == nullptr || desktop_ == nullptr)
    {
        return Status::not_initialized("external GUI app requires GUI services");
    }
    if (auto status = open_window(); !status.ok())
    {
        return status;
    }
    app_state_ = ExternalGuiAppState::running;
    return Status::success();
}

Status ExternalGuiAppModule::stop()
{
    if (desktop_ != nullptr && surface_ != 0)
    {
        auto status = desktop_->close_window(surface_);
        if (!status.ok() && status.code() != StatusCode::not_found)
        {
            return status;
        }
    }
    surface_ = 0;
    app_state_ = configured_ ? ExternalGuiAppState::stopped : ExternalGuiAppState::unloaded;
    compositor_ = nullptr;
    desktop_ = nullptr;
    return Status::success();
}

Status ExternalGuiAppModule::shutdown()
{
    return stop();
}

Status ExternalGuiAppModule::refresh()
{
    if (app_state_ != ExternalGuiAppState::running)
    {
        return Status::not_initialized("external GUI app is not running");
    }
    if (surface_ == 0)
    {
        return open_window();
    }
    auto status = render_window();
    if (status.code() == StatusCode::not_found)
    {
        return open_window();
    }
    return status;
}

Status ExternalGuiAppModule::open_window()
{
    auto desktop_bounds = compositor_->desktop_bounds();
    if (!desktop_bounds)
    {
        return desktop_bounds.status();
    }
    auto surface = desktop_->open_window(gui::DesktopWindowRequest{
        .bounds = fit_bounds(bounds_, desktop_bounds.value()),
        .title = title_.view(),
        .app = gui::TaskbarApp::none,
    });
    if (!surface)
    {
        return surface.status();
    }
    surface_ = surface.value();
    return render_window();
}

Status ExternalGuiAppModule::render_window()
{
    if (surface_ == 0)
    {
        return Status::not_initialized("external GUI app window is not open");
    }
    auto info = compositor_->surface_info(surface_);
    if (!info)
    {
        surface_ = 0;
        return Status::not_found("external GUI app surface is gone");
    }
    if (auto status = compositor_->fill(surface_, app_background); !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(surface_,
                                             gui::Rect{.x = 8,
                                                       .y = 18,
                                                       .width = info.value().bounds.width - 16,
                                                       .height = 38},
                                             app_panel);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(surface_, gui::Rect{.x = 8, .y = 60, .width = 44, .height = 36},
                                             app_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(surface_, gui::Rect{.x = 18, .y = 68, .width = 24, .height = 20},
                                             accent_);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(surface_, 2, 2, subtitle_.view(), app_muted, app_panel); !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(surface_, 8, 7, body_.view(), app_text, app_background); !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(surface_, 8, 9, load_path_.view(), app_muted, app_background);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->present(); !status.ok())
    {
        return status;
    }
    ++render_count_;
    return Status::success();
}

} // namespace ok
