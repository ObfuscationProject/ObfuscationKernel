#include "ok/core/external_module.hpp"

namespace ok
{
namespace
{

constexpr u32 dashboard_background = 0xff08121au;
constexpr u32 dashboard_panel = 0xff122334u;
constexpr u32 dashboard_panel_alt = 0xff1d3444u;
constexpr u32 dashboard_accent = 0xff44aa88u;
constexpr u32 dashboard_warm = 0xffffcc66u;
constexpr u32 dashboard_text = 0xffd8f3ffu;
constexpr u32 dashboard_muted = 0xff9fc6d2u;
constexpr u32 app_background = 0xff101820u;
constexpr u32 app_panel = 0xff172331u;
constexpr u32 app_panel_alt = 0xff203344u;
constexpr u32 app_text = 0xffd8f3ffu;
constexpr u32 app_muted = 0xff9fc6d2u;
constexpr u32 greeter_card_height = 178;
constexpr u32 greeter_card_max_width = 360;
constexpr u32 greeter_card_margin = 24;
constexpr u32 greeter_choice_height = 36;
constexpr u32 greeter_login_height = 26;
constexpr u32 system_dock_height = 44;
constexpr u32 system_dock_tile_width = 64;
constexpr u32 system_dock_tile_height = 28;
constexpr u32 system_dock_tile_gap = 10;
constexpr u32 system_dock_tile_left = 20;

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

Status draw_text_px(gui::GuiCompositor &compositor, gui::SurfaceId surface, i32 x, i32 y, std::string_view text,
                    u32 foreground, u32 background)
{
    const auto column = static_cast<u32>((x > 0 ? x : 0) / static_cast<i32>(gui::gui_glyph_width));
    const auto row = static_cast<u32>((y > 0 ? y : 0) / static_cast<i32>(gui::gui_glyph_height));
    return compositor.draw_text(surface, column, row, text, foreground, background);
}

[[nodiscard]] bool rect_contains(gui::Rect rect, i32 x, i32 y)
{
    return x >= rect.x && y >= rect.y && x < rect.x + static_cast<i32>(rect.width) &&
           y < rect.y + static_cast<i32>(rect.height);
}

[[nodiscard]] gui::Rect greeter_card_bounds(u32 width, u32 height)
{
    const auto card_width = width > greeter_card_max_width + greeter_card_margin
                                ? greeter_card_max_width
                                : (width > greeter_card_margin ? width - greeter_card_margin : width);
    const auto card_height = height > greeter_card_height + 8 ? greeter_card_height : (height > 8 ? height - 8 : height);
    return gui::Rect{.x = static_cast<i32>(width > card_width ? (width - card_width) / 2 : 0),
                     .y = static_cast<i32>(height > card_height ? (height - card_height) / 2 : 0),
                     .width = card_width,
                     .height = card_height};
}

[[nodiscard]] u32 greeter_choice_width(gui::Rect card)
{
    return card.width > 56 ? (card.width - 56) / 2 : card.width / 2;
}

[[nodiscard]] gui::Rect greeter_root_bounds(gui::Rect card)
{
    return gui::Rect{.x = card.x + 24,
                     .y = card.y + 72,
                     .width = greeter_choice_width(card),
                     .height = greeter_choice_height};
}

[[nodiscard]] gui::Rect greeter_user_bounds(gui::Rect card)
{
    const auto choice_width = greeter_choice_width(card);
    return gui::Rect{.x = card.x + 32 + static_cast<i32>(choice_width),
                     .y = card.y + 72,
                     .width = choice_width,
                     .height = greeter_choice_height};
}

[[nodiscard]] gui::Rect greeter_login_bounds(gui::Rect card)
{
    return gui::Rect{.x = card.x + 24,
                     .y = card.y + 138,
                     .width = card.width > 48 ? card.width - 48 : card.width,
                     .height = greeter_login_height};
}

[[nodiscard]] gui::Rect system_dock_bounds(u32 width, u32 height)
{
    const auto dock_height = height > system_dock_height ? system_dock_height : height / 4;
    return gui::Rect{.x = 0,
                     .y = static_cast<i32>(height > dock_height ? height - dock_height : 0),
                     .width = width,
                     .height = dock_height};
}

[[nodiscard]] gui::Rect system_dock_tile_bounds(gui::Rect dock, usize index)
{
    const auto x = dock.x + static_cast<i32>(system_dock_tile_left +
                                             index * (system_dock_tile_width + system_dock_tile_gap));
    const auto y = dock.y + static_cast<i32>(dock.height > system_dock_tile_height
                                                 ? (dock.height - system_dock_tile_height) / 2
                                                 : 0);
    return gui::Rect{.x = x, .y = y, .width = system_dock_tile_width, .height = system_dock_tile_height};
}

Status fill_outline(gui::GuiCompositor &compositor, gui::SurfaceId surface, gui::Rect rect, u32 color)
{
    if (rect.width == 0 || rect.height == 0)
    {
        return Status::success();
    }
    if (auto status = compositor.fill_rect(surface, gui::Rect{.x = rect.x, .y = rect.y, .width = rect.width, .height = 2},
                                           color);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.fill_rect(surface,
                                           gui::Rect{.x = rect.x,
                                                     .y = rect.y + static_cast<i32>(rect.height) - 2,
                                                     .width = rect.width,
                                                     .height = 2},
                                           color);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.fill_rect(surface, gui::Rect{.x = rect.x, .y = rect.y, .width = 2, .height = rect.height},
                                           color);
        !status.ok())
    {
        return status;
    }
    return compositor.fill_rect(surface,
                                gui::Rect{.x = rect.x + static_cast<i32>(rect.width) - 2,
                                          .y = rect.y,
                                          .width = 2,
                                          .height = rect.height},
                                color);
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
    desktop_state_ = ExternalGuiDesktopState::greeter;
    if (auto status = open_shell_surface(title_.view(), gui::GuiShellMode::system_greeter); !status.ok())
    {
        return status;
    }
    return render_greeter();
}

Status ExternalGuiDesktopModule::stop()
{
    if (compositor_ != nullptr && compositor_->state() == gui::GuiState::running)
    {
        static_cast<void>(compositor_->set_shell_mode(gui::GuiShellMode::kernel_shell));
    }
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
    if (desktop_state_ != ExternalGuiDesktopState::greeter && desktop_state_ != ExternalGuiDesktopState::desktop)
    {
        return Status::not_initialized("external GUI desktop is not running");
    }
    if (dashboard_surface_ == 0)
    {
        return open_shell_surface(desktop_state_ == ExternalGuiDesktopState::greeter ? title_.view()
                                                                                    : std::string_view{"ObfuscationOS Desktop"},
                                  desktop_state_ == ExternalGuiDesktopState::greeter
                                      ? gui::GuiShellMode::system_greeter
                                      : gui::GuiShellMode::system_shell);
    }
    auto status = desktop_state_ == ExternalGuiDesktopState::greeter ? render_greeter() : render_desktop_shell();
    if (status.code() == StatusCode::not_found)
    {
        dashboard_surface_ = 0;
        return refresh();
    }
    return status;
}

Status ExternalGuiDesktopModule::handle_key(int key)
{
    if (desktop_state_ != ExternalGuiDesktopState::greeter)
    {
        return Status::success();
    }
    if (key == '\t')
    {
        selected_login_user_ = selected_login_user_ == ExternalGuiLoginUser::root ? ExternalGuiLoginUser::user
                                                                                  : ExternalGuiLoginUser::root;
        return render_greeter();
    }
    if (key == '\r' || key == '\n' || key == ' ')
    {
        return login_default_user();
    }
    return Status::success();
}

Status ExternalGuiDesktopModule::handle_pointer_click(i32 x, i32 y)
{
    if (desktop_state_ != ExternalGuiDesktopState::greeter || compositor_ == nullptr || dashboard_surface_ == 0)
    {
        return Status::success();
    }
    auto info = compositor_->surface_info(dashboard_surface_);
    if (!info)
    {
        return info.status();
    }
    const auto card = greeter_card_bounds(info.value().bounds.width, info.value().bounds.height);
    if (rect_contains(greeter_root_bounds(card), x, y))
    {
        selected_login_user_ = ExternalGuiLoginUser::root;
        return render_greeter();
    }
    if (rect_contains(greeter_user_bounds(card), x, y))
    {
        selected_login_user_ = ExternalGuiLoginUser::user;
        return render_greeter();
    }
    if (rect_contains(greeter_login_bounds(card), x, y))
    {
        return login_default_user();
    }
    return Status::success();
}

std::string_view ExternalGuiDesktopModule::selected_login_user_name() const
{
    return selected_login_user_ == ExternalGuiLoginUser::root ? std::string_view{"root"} : std::string_view{"user"};
}

Result<ExternalGuiDockApp> ExternalGuiDesktopModule::dock_launcher_at(i32 x, i32 y) const
{
    if (desktop_state_ != ExternalGuiDesktopState::desktop || compositor_ == nullptr || dashboard_surface_ == 0)
    {
        return Status::not_found("system shell dock is not active");
    }
    auto info = compositor_->surface_info(dashboard_surface_);
    if (!info)
    {
        return info.status();
    }
    const auto bounds = info.value().bounds;
    if (x < bounds.x || y < bounds.y || x >= bounds.x + static_cast<i32>(bounds.width) ||
        y >= bounds.y + static_cast<i32>(bounds.height))
    {
        return Status::not_found("system shell dock miss");
    }

    const auto local_x = static_cast<u32>(x - bounds.x);
    const auto local_y = static_cast<u32>(y - bounds.y);
    const auto dock = system_dock_bounds(bounds.width, bounds.height);
    if (!rect_contains(dock, static_cast<i32>(local_x), static_cast<i32>(local_y)))
    {
        return Status::not_found("system shell dock miss");
    }

    const auto point_x = static_cast<i32>(local_x);
    const auto point_y = static_cast<i32>(local_y);
    if (rect_contains(system_dock_tile_bounds(dock, 0), point_x, point_y))
    {
        return ExternalGuiDockApp::about;
    }
    if (rect_contains(system_dock_tile_bounds(dock, 1), point_x, point_y))
    {
        return ExternalGuiDockApp::prefs;
    }
    if (rect_contains(system_dock_tile_bounds(dock, 2), point_x, point_y))
    {
        return ExternalGuiDockApp::notes;
    }
    return Status::not_found("system shell dock launcher miss");
}

Status ExternalGuiDesktopModule::login_default_user()
{
    if (desktop_state_ == ExternalGuiDesktopState::desktop)
    {
        return Status::success();
    }
    if (desktop_state_ != ExternalGuiDesktopState::greeter)
    {
        return Status::not_initialized("system GUI greeter is not active");
    }
    desktop_state_ = ExternalGuiDesktopState::desktop;
    if (auto status = open_shell_surface("ObfuscationOS Desktop", gui::GuiShellMode::system_shell); !status.ok())
    {
        return status;
    }
    return render_desktop_shell();
}

Status ExternalGuiDesktopModule::open_shell_surface(std::string_view title, gui::GuiShellMode mode)
{
    auto desktop = compositor_->desktop_bounds();
    if (!desktop)
    {
        return desktop.status();
    }
    if (auto status = compositor_->set_shell_mode(mode); !status.ok())
    {
        return status;
    }
    if (dashboard_surface_ != 0)
    {
        if (auto status = compositor_->set_title(dashboard_surface_, title); !status.ok() &&
                          status.code() != StatusCode::not_found)
        {
            return status;
        }
        if (auto status = compositor_->resize_surface(dashboard_surface_, desktop.value()); !status.ok() &&
                          status.code() != StatusCode::not_found)
        {
            return status;
        }
        if (auto status = compositor_->move_surface(dashboard_surface_, 0, 0); !status.ok() &&
                          status.code() != StatusCode::not_found)
        {
            return status;
        }
        if (auto status = compositor_->set_surface_chrome(dashboard_surface_, gui::SurfaceChrome::plain);
            !status.ok() && status.code() != StatusCode::not_found)
        {
            return status;
        }
        auto info = compositor_->surface_info(dashboard_surface_);
        if (info)
        {
            return compositor_->raise_surface(dashboard_surface_);
        }
        dashboard_surface_ = 0;
    }

    auto surface = desktop_->open_window(gui::DesktopWindowRequest{
        .bounds = desktop.value(),
        .title = title,
        .app = gui::TaskbarApp::none,
    });
    if (!surface)
    {
        return surface.status();
    }
    dashboard_surface_ = surface.value();
    if (auto status = compositor_->set_surface_chrome(dashboard_surface_, gui::SurfaceChrome::plain); !status.ok())
    {
        return status;
    }
    return compositor_->raise_surface(dashboard_surface_);
}

Status ExternalGuiDesktopModule::render_greeter()
{
    if (dashboard_surface_ == 0)
    {
        return Status::not_initialized("external GUI greeter is not open");
    }
    auto info = compositor_->surface_info(dashboard_surface_);
    if (!info)
    {
        dashboard_surface_ = 0;
        return Status::not_found("external GUI greeter surface is gone");
    }

    if (auto status = compositor_->fill(dashboard_surface_, dashboard_background); !status.ok())
    {
        return status;
    }
    const auto width = info.value().bounds.width;
    const auto height = info.value().bounds.height;
    for (u32 y = 0; y < height; y += 18)
    {
        if (auto status = compositor_->fill_rect(dashboard_surface_, gui::Rect{.x = 0, .y = static_cast<i32>(y), .width = width, .height = 1},
                                                 0xff102131u);
            !status.ok())
        {
            return status;
        }
    }
    for (u32 x = 0; x < width; x += 48)
    {
        if (auto status = compositor_->fill_rect(dashboard_surface_,
                                                 gui::Rect{.x = static_cast<i32>(x), .y = 0, .width = 1, .height = height},
                                                 0xff0b1a27u);
            !status.ok())
        {
            return status;
        }
    }

    const auto card = greeter_card_bounds(width, height);
    const auto root = greeter_root_bounds(card);
    const auto user = greeter_user_bounds(card);
    const auto login = greeter_login_bounds(card);
    if (auto status = compositor_->fill_rect(dashboard_surface_, card, dashboard_panel); !status.ok())
    {
        return status;
    }
    if (auto status = fill_outline(*compositor_, dashboard_surface_, card, 0xff24465au); !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_,
                                             gui::Rect{.x = card.x, .y = card.y, .width = 5, .height = card.height},
                                             dashboard_accent);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_,
                                             gui::Rect{.x = card.x + 5,
                                                       .y = card.y,
                                                       .width = card.width > 5 ? card.width - 5 : card.width,
                                                       .height = 48},
                                             0xff162b3cu);
        !status.ok())
    {
        return status;
    }

    const auto root_selected = selected_login_user_ == ExternalGuiLoginUser::root;
    const auto user_selected = selected_login_user_ == ExternalGuiLoginUser::user;
    if (auto status = compositor_->fill_rect(dashboard_surface_, root, root_selected ? 0xfff2c866u : dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_, user, user_selected ? 0xfff2c866u : dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status = fill_outline(*compositor_, dashboard_surface_, root,
                                   root_selected ? 0xffffe2a0u : 0xff35566au);
        !status.ok())
    {
        return status;
    }
    if (auto status = fill_outline(*compositor_, dashboard_surface_, user,
                                   user_selected ? 0xffffe2a0u : 0xff35566au);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_, login, dashboard_warm); !status.ok())
    {
        return status;
    }
    if (auto status = fill_outline(*compositor_, dashboard_surface_, login, 0xffffe2a0u); !status.ok())
    {
        return status;
    }

    if (auto status = compositor_->fill_rect(dashboard_surface_, gui::Rect{.x = card.x + 24, .y = card.y + 15, .width = 26, .height = 18},
                                             dashboard_accent);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_, gui::Rect{.x = card.x + 31, .y = card.y + 22, .width = 12, .height = 4},
                                             dashboard_panel);
        !status.ok())
    {
        return status;
    }
    if (auto status = draw_text_px(*compositor_, dashboard_surface_, card.x + 64, card.y + 16, brand_.view(),
                                   dashboard_text, 0xff162b3cu);
        !status.ok())
    {
        return status;
    }
    if (auto status = draw_text_px(*compositor_, dashboard_surface_, card.x + 64, card.y + 32, "login session",
                                   dashboard_muted, 0xff162b3cu);
        !status.ok())
    {
        return status;
    }
    if (auto status = draw_text_px(*compositor_, dashboard_surface_, root.x + 14, root.y + 7, "root",
                                   root_selected ? 0xff071018u : dashboard_text,
                                   root_selected ? 0xfff2c866u : dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status = draw_text_px(*compositor_, dashboard_surface_, root.x + 14, root.y + 21, "admin",
                                   root_selected ? 0xff2a2209u : dashboard_muted,
                                   root_selected ? 0xfff2c866u : dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status = draw_text_px(*compositor_, dashboard_surface_, user.x + 14, user.y + 7, "user",
                                   user_selected ? 0xff071018u : dashboard_text,
                                   user_selected ? 0xfff2c866u : dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status = draw_text_px(*compositor_, dashboard_surface_, user.x + 14, user.y + 21, "uid 1000",
                                   user_selected ? 0xff2a2209u : dashboard_muted,
                                   user_selected ? 0xfff2c866u : dashboard_panel_alt);
        !status.ok())
    {
        return status;
    }
    if (auto status = draw_text_px(*compositor_, dashboard_surface_, login.x + static_cast<i32>(login.width / 2) - 28,
                                   login.y + 6, "login", 0xff071018u, dashboard_warm);
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

Status ExternalGuiDesktopModule::render_desktop_shell()
{
    if (dashboard_surface_ == 0)
    {
        return Status::not_initialized("external GUI desktop shell is not open");
    }
    auto info = compositor_->surface_info(dashboard_surface_);
    if (!info)
    {
        dashboard_surface_ = 0;
        return Status::not_found("external GUI desktop shell surface is gone");
    }
    if (auto status = compositor_->fill(dashboard_surface_, 0xff071018u); !status.ok())
    {
        return status;
    }
    const auto width = info.value().bounds.width;
    const auto height = info.value().bounds.height;
    for (u32 y = 0; y < height; y += 36)
    {
        if (auto status = compositor_->fill_rect(dashboard_surface_, gui::Rect{.x = 0, .y = static_cast<i32>(y), .width = width, .height = 1},
                                                 0xff10283au);
            !status.ok())
        {
            return status;
        }
    }
    for (u32 x = 0; x < width; x += 72)
    {
        if (auto status = compositor_->fill_rect(dashboard_surface_,
                                                 gui::Rect{.x = static_cast<i32>(x), .y = 0, .width = 1, .height = height},
                                                 0xff0b1b27u);
            !status.ok())
        {
            return status;
        }
    }
    const auto dock = system_dock_bounds(width, height);
    if (auto status = compositor_->fill_rect(dashboard_surface_, gui::Rect{.x = 0, .y = 0, .width = width, .height = 34},
                                             0xff102233u);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_, dock, 0xff102233u); !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_, gui::Rect{.x = 0, .y = dock.y, .width = width, .height = 2},
                                             dashboard_accent);
        !status.ok())
    {
        return status;
    }
    FixedString<64> metric;
    if (auto status = compositor_->draw_text(dashboard_surface_, 3, 1, brand_.view(), dashboard_text, 0xff102233u);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(dashboard_surface_, 22, 1, "system desktop", dashboard_muted,
                                             0xff102233u);
        !status.ok())
    {
        return status;
    }
    if (auto status = assign_metric(metric, "cpus=", topology_->online_count()); !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(dashboard_surface_, 3, 5, metric.view(), dashboard_text, 0xff071018u);
        !status.ok())
    {
        return status;
    }
    if (auto status = assign_metric(metric, "procs=", scheduler_->process_count()); !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(dashboard_surface_, 15, 5, metric.view(), dashboard_text, 0xff071018u);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->fill_rect(dashboard_surface_,
                                             gui::Rect{.x = 20, .y = 70, .width = 210, .height = 58},
                                             0xff0f2233u);
        !status.ok())
    {
        return status;
    }
    if (auto status = fill_outline(*compositor_, dashboard_surface_, gui::Rect{.x = 20, .y = 70, .width = 210, .height = 58},
                                   0xff24465au);
        !status.ok())
    {
        return status;
    }
    if (auto status = draw_text_px(*compositor_, dashboard_surface_, 34, 84, "System apps are ready", dashboard_text,
                                   0xff0f2233u);
        !status.ok())
    {
        return status;
    }
    if (auto status = draw_text_px(*compositor_, dashboard_surface_, 34, 104, "About / Prefs / Notes", dashboard_muted,
                                   0xff0f2233u);
        !status.ok())
    {
        return status;
    }

    if (auto status = compositor_->fill_rect(dashboard_surface_,
                                             gui::Rect{.x = static_cast<i32>(width > 196 ? width - 196 : 12),
                                                       .y = 58,
                                                       .width = width > 220 ? 176u : width - 24,
                                                       .height = 76},
                                             0xff102233u);
        !status.ok())
    {
        return status;
    }
    if (auto status = fill_outline(*compositor_, dashboard_surface_,
                                   gui::Rect{.x = static_cast<i32>(width > 196 ? width - 196 : 12),
                                             .y = 58,
                                             .width = width > 220 ? 176u : width - 24,
                                             .height = 76},
                                   0xff254a5au);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(dashboard_surface_, width > 196 ? (width - 182) / gui::gui_glyph_width : 2,
                                             6, "session", dashboard_muted, 0xff102233u);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor_->draw_text(dashboard_surface_, width > 196 ? (width - 182) / gui::gui_glyph_width : 2,
                                             8, selected_login_user_name(), dashboard_text, 0xff102233u);
        !status.ok())
    {
        return status;
    }

    constexpr std::string_view labels[] = {"About", "Prefs", "Notes"};
    constexpr u32 colors[] = {dashboard_warm, 0xff66b7ffu, 0xffff7799u};
    for (usize i = 0; i < 3; ++i)
    {
        const auto tile = system_dock_tile_bounds(dock, i);
        if (auto status = compositor_->fill_rect(dashboard_surface_, tile, 0xff162b3cu); !status.ok())
        {
            return status;
        }
        if (auto status = fill_outline(*compositor_, dashboard_surface_, tile, colors[i]); !status.ok())
        {
            return status;
        }
        if (auto status = compositor_->fill_rect(dashboard_surface_,
                                                 gui::Rect{.x = tile.x + 7, .y = tile.y + 8, .width = 8, .height = 12},
                                                 colors[i]);
            !status.ok())
        {
            return status;
        }
        if (auto status = draw_text_px(*compositor_, dashboard_surface_, tile.x + 20, tile.y + 9, labels[i],
                                       dashboard_text, 0xff162b3cu);
            !status.ok())
        {
            return status;
        }
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
