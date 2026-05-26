#pragma once

#include "ok/core/fixed.hpp"
#include "ok/gui/compositor.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/user/user.hpp"

#include <string_view>

namespace ok
{

class Kernel;

namespace apps
{

class KernelTaskManager final
{
  public:
    Status open(gui::GuiCompositor &compositor, Kernel &kernel, user::Credentials credentials,
                sched::ProcessId process_id, std::string_view title = "task-manager");
    Status refresh(gui::GuiCompositor &compositor, Kernel &kernel);
    Status handle_key(gui::GuiCompositor &compositor, Kernel &kernel, int key);
    Status scroll_processes(gui::GuiCompositor &compositor, Kernel &kernel, i32 rows);
    Status close(gui::GuiCompositor &compositor);
    void mark_closed();
    Status render_tui(Kernel &kernel, FixedString<4096> &out) const;

    [[nodiscard]] gui::SurfaceId surface_id() const
    {
        return surface_id_;
    }
    [[nodiscard]] sched::ProcessId process_id() const
    {
        return process_id_;
    }
    [[nodiscard]] const user::Credentials &credentials() const
    {
        return credentials_;
    }
    [[nodiscard]] usize render_count() const
    {
        return render_count_;
    }

  private:
    Status render(gui::GuiCompositor &compositor, Kernel &kernel);

    gui::SurfaceId surface_id_{0};
    sched::ProcessId process_id_{0};
    user::Credentials credentials_{user::kernel_credentials()};
    usize render_count_{0};
    usize process_scroll_{0};
    u8 key_escape_state_{0};
};

} // namespace apps
} // namespace ok
