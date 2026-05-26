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
                sched::ProcessId process_id);
    Status refresh(gui::GuiCompositor &compositor, Kernel &kernel);
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
};

} // namespace apps
} // namespace ok
