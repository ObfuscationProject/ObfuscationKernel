#include "ok/apps/file_manager.hpp"

namespace ok::apps
{
namespace
{

using gui::GuiCompositor;
using gui::Rect;
using gui::SurfaceId;
using gui::SurfaceInfo;
using gui::TaskbarApp;
using gui::WindowState;
using gui::gui_glyph_height;
using gui::gui_glyph_width;
using gui::taskbar_height;

constexpr u32 title_color = 0xff12313du;
constexpr Rect file_manager_bounds{.x = 28, .y = 34, .width = 300, .height = 182};
constexpr u32 file_manager_sidebar_width = 64;
constexpr u32 file_manager_title_height = gui_glyph_height * 2 + 2;
constexpr usize file_manager_nav_row = 4;
constexpr usize file_manager_list_row = 6;

Status append_decimal(FixedString<96> &out, u64 value)
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

[[nodiscard]] std::string_view node_type_label(fs::NodeType type)
{
    switch (type)
    {
    case fs::NodeType::directory:
        return "dir";
    case fs::NodeType::regular:
        return "file";
    case fs::NodeType::device:
        return "dev";
    case fs::NodeType::symlink:
        return "link";
    }
    return "node";
}

Status fill_rect_if_non_empty(GuiCompositor &compositor, SurfaceId surface, Rect rect, u32 rgba)
{
    if (rect.width == 0 || rect.height == 0)
    {
        return Status::success();
    }
    return compositor.fill_rect(surface, rect, rgba);
}

struct FileManagerLayout
{
    u32 width{0};
    u32 height{0};
    u32 sidebar_width{0};
    u32 body_x{2};
    u32 body_width{0};
    u32 body_height{0};
    u32 body_y{file_manager_title_height + 2};
};

[[nodiscard]] FileManagerLayout file_manager_layout(const SurfaceInfo &info)
{
    FileManagerLayout layout{.width = info.bounds.width, .height = info.bounds.height};
    layout.sidebar_width = info.bounds.width >= 140 ? file_manager_sidebar_width : 0;
    layout.body_x = layout.sidebar_width == 0 ? 2 : layout.sidebar_width + 2;
    if (info.bounds.width > layout.body_x + 2)
    {
        layout.body_width = info.bounds.width - layout.body_x - 2;
    }
    if (info.bounds.height > layout.body_y + 2)
    {
        layout.body_height = info.bounds.height - layout.body_y - 2;
    }
    return layout;
}

Status append_child_path(FixedString<96> &out, std::string_view base, std::string_view child)
{
    out.clear();
    if (base.empty())
    {
        base = "/";
    }
    if (auto status = out.append(base); !status.ok())
    {
        return status;
    }
    if (out.view() != "/")
    {
        if (auto status = out.append('/'); !status.ok())
        {
            return status;
        }
    }
    return out.append(child);
}

Status assign_parent_path(FixedString<96> &out, std::string_view path)
{
    out.clear();
    if (path.empty() || path == "/")
    {
        return out.assign("/");
    }
    usize last_slash = 0;
    for (usize i = 0; i < path.size(); ++i)
    {
        if (path[i] == '/')
        {
            last_slash = i;
        }
    }
    if (last_slash == 0)
    {
        return out.assign("/");
    }
    return out.assign(path.substr(0, last_slash));
}

[[nodiscard]] bool has_parent_directory(std::string_view path)
{
    return !path.empty() && path != "/";
}

} // namespace

Status KernelFileManager::open(GuiCompositor &compositor, fs::VirtualFileSystem &vfs, std::string_view path,
                               user::Credentials credentials, sched::ProcessId process_id)
{
    if (path.empty())
    {
        path = "/";
    }
    credentials_ = credentials;
    process_id_ = process_id;
    if (auto status = require_directory_access(vfs, path); !status.ok())
    {
        return status;
    }
    if (auto status = path_.assign(path); !status.ok())
    {
        return status;
    }
    selected_entry_ = fs::max_child_nodes;

    if (surface_id_ != 0 && !compositor.surface_info(surface_id_))
    {
        surface_id_ = 0;
    }
    if (surface_id_ == 0)
    {
        auto bounds = file_manager_bounds;
        const auto offset = static_cast<i32>((compositor.surface_count() % 5) * 18);
        bounds.x += offset;
        bounds.y += offset;
        auto desktop = compositor.desktop_bounds();
        if (desktop)
        {
            const auto max_x =
                static_cast<i32>(desktop.value().width > bounds.width ? desktop.value().width - bounds.width : 0);
            const auto work_height = desktop.value().height > taskbar_height ? desktop.value().height - taskbar_height
                                                                             : desktop.value().height;
            const auto max_y = static_cast<i32>(work_height > bounds.height ? work_height - bounds.height : 0);
            bounds.x = bounds.x > max_x ? max_x : bounds.x;
            bounds.y = bounds.y > max_y ? max_y : bounds.y;
        }
        auto surface = compositor.create_surface(bounds, "kernel-files");
        if (!surface)
        {
            return surface.status();
        }
        surface_id_ = surface.value();
        if (auto status = compositor.set_surface_app(surface_id_, TaskbarApp::file_manager); !status.ok())
        {
            return status;
        }
    }
    if (auto status = compositor.raise_surface(surface_id_); !status.ok())
    {
        return status;
    }
    return render(compositor, vfs);
}

Status KernelFileManager::refresh(GuiCompositor &compositor, fs::VirtualFileSystem &vfs)
{
    if (surface_id_ == 0)
    {
        return Status::not_initialized("file manager surface is not open");
    }
    if (auto status = require_directory_access(vfs, path_.view()); !status.ok())
    {
        return status;
    }
    return render(compositor, vfs);
}

Status KernelFileManager::handle_surface_changed(GuiCompositor &compositor, fs::VirtualFileSystem &vfs)
{
    if (surface_id_ == 0)
    {
        return Status::success();
    }
    auto info = compositor.surface_info(surface_id_);
    if (!info)
    {
        mark_closed();
        return Status::success();
    }
    if (info.value().window_state == WindowState::minimized)
    {
        return compositor.present();
    }
    return render(compositor, vfs);
}

Status KernelFileManager::close(GuiCompositor &compositor)
{
    if (surface_id_ == 0)
    {
        return Status::success();
    }
    const auto id = surface_id_;
    surface_id_ = 0;
    process_id_ = 0;
    if (!compositor.surface_info(id))
    {
        return Status::success();
    }
    if (auto status = compositor.destroy_surface(id); !status.ok())
    {
        return status;
    }
    return compositor.present();
}

void KernelFileManager::mark_closed()
{
    surface_id_ = 0;
    process_id_ = 0;
    selected_entry_ = fs::max_child_nodes;
    key_escape_state_ = 0;
}

Status KernelFileManager::require_directory_access(fs::VirtualFileSystem &vfs, std::string_view path) const
{
    auto metadata = vfs.stat(path);
    if (!metadata)
    {
        return metadata.status();
    }
    if (metadata.value().type != fs::NodeType::directory)
    {
        return Status::invalid_argument("path is not a directory");
    }
    return fs::require_access(metadata.value(), fs::Credentials{.uid = credentials_.euid, .gid = credentials_.egid},
                              fs::access_read | fs::access_execute);
}

Status KernelFileManager::open_parent(GuiCompositor &compositor, fs::VirtualFileSystem &vfs)
{
    FixedString<96> parent;
    if (auto status = assign_parent_path(parent, path_.view()); !status.ok())
    {
        return status;
    }
    if (parent.view() == path_.view())
    {
        return Status::success();
    }
    return open(compositor, vfs, parent.view(), credentials_, process_id_);
}

Status KernelFileManager::handle_mouse(GuiCompositor &compositor, fs::VirtualFileSystem &vfs, i32 x, i32 y, bool click)
{
    if (!click || surface_id_ == 0)
    {
        return Status::success();
    }
    auto info = compositor.surface_info(surface_id_);
    if (!info)
    {
        surface_id_ = 0;
        return Status::success();
    }
    const auto &bounds = info.value().bounds;
    if (x < bounds.x || y < bounds.y || x >= bounds.x + static_cast<i32>(bounds.width) ||
        y >= bounds.y + static_cast<i32>(bounds.height))
    {
        return Status::success();
    }

    const auto local_x = x - bounds.x;
    const auto local_y = y - bounds.y;
    const auto row = static_cast<usize>(local_y / static_cast<i32>(gui_glyph_height));
    const auto layout = file_manager_layout(info.value());
    if (layout.sidebar_width != 0 && local_x >= 2 && local_x < static_cast<i32>(layout.sidebar_width + 2) &&
        row >= file_manager_nav_row && row <= file_manager_nav_row + 3)
    {
        constexpr std::string_view nav_paths[] = {"/", "/dev", "/tmp", "/proc"};
        const auto next = nav_paths[row - file_manager_nav_row];
        if (require_directory_access(vfs, next).ok())
        {
            return open(compositor, vfs, next, credentials_, process_id_);
        }
        return Status::success();
    }
    if (local_x < static_cast<i32>(layout.body_x) || row < file_manager_list_row)
    {
        return Status::success();
    }

    auto listing = vfs.list(path_.view());
    if (!listing)
    {
        return listing.status();
    }
    const auto index = row - file_manager_list_row;
    const auto parent_entries = has_parent_directory(path_.view()) ? static_cast<usize>(1) : static_cast<usize>(0);
    const auto logical_count = listing.value().count + parent_entries;
    if (index >= logical_count)
    {
        return Status::success();
    }
    selected_entry_ = index;
    if (index < parent_entries)
    {
        return open_parent(compositor, vfs);
    }
    const auto &entry = listing.value().entries[index - parent_entries];
    if (entry.metadata.type == fs::NodeType::directory)
    {
        FixedString<96> child_path;
        if (auto status = append_child_path(child_path, path_.view(), entry.name.view()); !status.ok())
        {
            return status;
        }
        return open(compositor, vfs, child_path.view(), credentials_, process_id_);
    }
    return render(compositor, vfs);
}

Status KernelFileManager::handle_key(GuiCompositor &compositor, fs::VirtualFileSystem &vfs, int key)
{
    if (surface_id_ == 0)
    {
        return Status::success();
    }
    if (key == 0x1b)
    {
        key_escape_state_ = 1;
        return Status::success();
    }
    if (key_escape_state_ == 1)
    {
        key_escape_state_ = key == '[' ? 2 : 0;
        return Status::success();
    }

    auto listing = vfs.list(path_.view());
    if (!listing)
    {
        return listing.status();
    }
    const auto parent_entries = has_parent_directory(path_.view()) ? static_cast<usize>(1) : static_cast<usize>(0);
    const auto count = listing.value().count + parent_entries;
    bool changed = false;
    if (key_escape_state_ == 2)
    {
        key_escape_state_ = 0;
        if (key == 'A' && count != 0)
        {
            if (selected_entry_ == fs::max_child_nodes || selected_entry_ == 0)
            {
                selected_entry_ = count - 1;
            }
            else
            {
                --selected_entry_;
            }
            changed = true;
        }
        else if (key == 'B' && count != 0)
        {
            if (selected_entry_ == fs::max_child_nodes || selected_entry_ + 1 >= count)
            {
                selected_entry_ = 0;
            }
            else
            {
                ++selected_entry_;
            }
            changed = true;
        }
    }
    else if ((key == '\r' || key == '\n') && selected_entry_ < count)
    {
        if (selected_entry_ < parent_entries)
        {
            return open_parent(compositor, vfs);
        }
        const auto &entry = listing.value().entries[selected_entry_ - parent_entries];
        if (entry.metadata.type == fs::NodeType::directory)
        {
            FixedString<96> child_path;
            if (auto status = append_child_path(child_path, path_.view(), entry.name.view()); !status.ok())
            {
                return status;
            }
            return open(compositor, vfs, child_path.view(), credentials_, process_id_);
        }
    }
    else if (key == '\b' || key == 0x7f || key == 'u' || key == 'U')
    {
        return open_parent(compositor, vfs);
    }
    else
    {
        key_escape_state_ = 0;
    }

    return changed ? render(compositor, vfs) : Status::success();
}

Status KernelFileManager::render(GuiCompositor &compositor, fs::VirtualFileSystem &vfs)
{
    if (auto status = require_directory_access(vfs, path_.view()); !status.ok())
    {
        return status;
    }
    auto listing = vfs.list(path_.view());
    if (!listing)
    {
        return listing.status();
    }
    auto info = compositor.surface_info(surface_id_);
    if (!info)
    {
        mark_closed();
        return Status::success();
    }
    const auto layout = file_manager_layout(info.value());

    if (auto status = compositor.fill(surface_id_, 0xff111820u); !status.ok())
    {
        return status;
    }
    if (auto status = fill_rect_if_non_empty(
            compositor, surface_id_,
            Rect{.x = 2,
                 .y = 2,
                 .width = layout.width > 4 ? layout.width - 4 : 0,
                 .height = layout.height > 4 ? file_manager_title_height : 0},
            title_color);
        !status.ok())
    {
        return status;
    }
    if (layout.sidebar_width != 0)
    {
        if (auto status = fill_rect_if_non_empty(
                compositor, surface_id_,
                Rect{.x = 2,
                     .y = static_cast<i32>(layout.body_y),
                     .width = layout.sidebar_width,
                     .height = layout.body_height},
                0xff172331u);
            !status.ok())
        {
            return status;
        }
    }
    if (auto status = fill_rect_if_non_empty(
            compositor, surface_id_,
            Rect{.x = static_cast<i32>(layout.body_x),
                 .y = static_cast<i32>(layout.body_y),
                 .width = layout.body_width,
                 .height = layout.body_height},
            0xff0d141cu);
        !status.ok())
    {
        return status;
    }
    if (auto status = fill_rect_if_non_empty(
            compositor, surface_id_,
            Rect{.x = static_cast<i32>(layout.body_x),
                 .y = static_cast<i32>((file_manager_nav_row + 1) * gui_glyph_height),
                 .width = layout.body_width,
                 .height = 1},
            0xff44aa88u);
        !status.ok())
    {
        return status;
    }

    FixedString<96> title;
    if (auto status = title.assign("FILES "); !status.ok())
    {
        return status;
    }
    if (auto status = title.append(path_.view()); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.draw_text(surface_id_, 2, 1, title.view(), 0xffd8f3ffu, title_color); !status.ok())
    {
        return status;
    }
    if (layout.sidebar_width != 0)
    {
        if (auto status = compositor.draw_text(surface_id_, 2, file_manager_nav_row, "ROOT\nDEV\nTMP\nPROC",
                                               0xff9fc6d2u, 0xff172331u);
            !status.ok())
        {
            return status;
        }
    }
    const auto body_column = (layout.body_x + 2) / gui_glyph_width;
    if (auto status = compositor.draw_text(surface_id_, body_column, file_manager_nav_row,
                                           "NAME              TYPE SIZE", 0xffffcc66u, 0xff0d141cu);
        !status.ok())
    {
        return status;
    }

    const auto surface_rows = layout.height / gui_glyph_height;
    const auto max_visible_entries =
        surface_rows > file_manager_list_row ? surface_rows - file_manager_list_row : static_cast<usize>(0);
    const auto parent_entries = has_parent_directory(path_.view()) ? static_cast<usize>(1) : static_cast<usize>(0);
    const auto logical_count = listing.value().count + parent_entries;
    const auto count = logical_count < max_visible_entries ? logical_count : max_visible_entries;
    for (usize i = 0; i < count; ++i)
    {
        const auto row = static_cast<u32>(file_manager_list_row + i);
        const auto row_color = i == selected_entry_ ? 0xff23485au : ((i % 2) == 0 ? 0xff111c24u : 0xff0d141cu);
        if (auto status = fill_rect_if_non_empty(
                compositor, surface_id_,
                Rect{.x = static_cast<i32>(layout.body_x + 2),
                     .y = static_cast<i32>(row * gui_glyph_height),
                     .width = layout.body_width > 4 ? layout.body_width - 4 : 0,
                     .height = gui_glyph_height},
                row_color);
            !status.ok())
        {
            return status;
        }

        FixedString<96> line;
        auto type = fs::NodeType::directory;
        usize size = 0;
        if (i < parent_entries)
        {
            if (auto status = line.assign("[D] "); !status.ok())
            {
                return status;
            }
            if (auto status = line.append("../"); !status.ok())
            {
                return status;
            }
        }
        else
        {
            const auto &entry = listing.value().entries[i - parent_entries];
            type = entry.metadata.type;
            size = entry.metadata.size;
            if (entry.metadata.type == fs::NodeType::directory)
            {
                if (auto status = line.assign("[D] "); !status.ok())
                {
                    return status;
                }
            }
            else
            {
                if (auto status = line.assign("[F] "); !status.ok())
                {
                    return status;
                }
            }
            if (auto status = line.append(entry.name.view()); !status.ok())
            {
                return status;
            }
            if (entry.metadata.type == fs::NodeType::directory)
            {
                if (auto status = line.append("/"); !status.ok())
                {
                    return status;
                }
            }
        }
        while (line.size() < 22)
        {
            if (auto status = line.append(" "); !status.ok())
            {
                return status;
            }
        }
        if (auto status = line.append(node_type_label(type)); !status.ok())
        {
            return status;
        }
        while (line.size() < 27)
        {
            if (auto status = line.append(" "); !status.ok())
            {
                return status;
            }
        }
        if (auto status = append_decimal(line, size); !status.ok())
        {
            return status;
        }
        if (auto status = compositor.draw_text(surface_id_, body_column, row, line.view(), 0xffd8f3ffu, row_color);
            !status.ok())
        {
            return status;
        }
    }

    if (auto status = compositor.present(); !status.ok())
    {
        return status;
    }
    ++render_count_;
    return Status::success();
}

} // namespace ok::apps
