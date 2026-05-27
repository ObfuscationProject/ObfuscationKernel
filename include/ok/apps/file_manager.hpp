#pragma once

#include "ok/fs/vfs.hpp"
#include "ok/gui/compositor.hpp"
#include "ok/user/user.hpp"

#include <string_view>

namespace ok::apps
{

class KernelFileManager final
{
  public:
    Status open(gui::GuiCompositor &compositor, fs::VirtualFileSystem &vfs, std::string_view path,
                user::Credentials credentials, sched::ProcessId process_id);
    Status refresh(gui::GuiCompositor &compositor, fs::VirtualFileSystem &vfs);
    Status close(gui::GuiCompositor &compositor);
    void mark_closed();
    Status render_tui(fs::VirtualFileSystem &vfs, std::string_view path, user::Credentials credentials,
                      FixedString<4096> &out) const;
    Status handle_surface_changed(gui::GuiCompositor &compositor, fs::VirtualFileSystem &vfs);
    Status handle_mouse(gui::GuiCompositor &compositor, fs::VirtualFileSystem &vfs, i32 x, i32 y, bool click);
    Status handle_key(gui::GuiCompositor &compositor, fs::VirtualFileSystem &vfs, int key);

    [[nodiscard]] gui::SurfaceId surface_id() const
    {
        return surface_id_;
    }
    [[nodiscard]] std::string_view path() const
    {
        return path_.view();
    }
    [[nodiscard]] usize render_count() const
    {
        return render_count_;
    }
    [[nodiscard]] sched::ProcessId process_id() const
    {
        return process_id_;
    }
    [[nodiscard]] const user::Credentials &credentials() const
    {
        return credentials_;
    }

  private:
    [[nodiscard]] Status require_directory_access(fs::VirtualFileSystem &vfs, std::string_view path) const;
    Status open_parent(gui::GuiCompositor &compositor, fs::VirtualFileSystem &vfs);
    Status render(gui::GuiCompositor &compositor, fs::VirtualFileSystem &vfs);

    gui::SurfaceId surface_id_{0};
    FixedString<96> path_{"/"};
    usize render_count_{0};
    usize selected_entry_{fs::max_child_nodes};
    usize file_scroll_{0};
    sched::ProcessId process_id_{0};
    user::Credentials credentials_{user::kernel_credentials()};
    u8 key_escape_state_{0};
};

} // namespace ok::apps
