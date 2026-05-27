#pragma once

#include "ok/core/fixed.hpp"
#include "ok/gui/compositor.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/user/user.hpp"

#include <array>
#include <string_view>

namespace ok
{

class Kernel;

namespace apps
{

enum class TaskMonitorProgram : u8
{
    task_manager,
    top,
};

class KernelTaskManager final
{
  public:
    Status open(gui::GuiCompositor &compositor, Kernel &kernel, user::Credentials credentials,
                sched::ProcessId process_id, TaskMonitorProgram program = TaskMonitorProgram::task_manager);
    Status refresh(gui::GuiCompositor &compositor, Kernel &kernel);
    Status handle_key(gui::GuiCompositor &compositor, Kernel &kernel, int key);
    Status scroll_processes(gui::GuiCompositor &compositor, Kernel &kernel, i32 rows);
    Status close(gui::GuiCompositor &compositor);
    void mark_closed();
    Status render_tui(Kernel &kernel, FixedString<4096> &out) const;
    Status render_top_tui(Kernel &kernel, FixedString<4096> &out) const;

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
    [[nodiscard]] usize process_scroll() const
    {
        return process_scroll_;
    }
    [[nodiscard]] u8 sampled_cpu_usage_percent(smp::CpuId cpu) const;
    [[nodiscard]] u8 sampled_process_usage_percent(sched::ProcessId pid) const;

  private:
    struct ProcessUsageSample
    {
        sched::ProcessId pid{0};
        u64 accounted_ticks{0};
        u8 usage_percent{0};
    };

    void update_usage_sample(const sched::Scheduler &scheduler);
    [[nodiscard]] u8 cpu_usage_percent(smp::CpuId cpu) const;
    [[nodiscard]] u8 process_usage_percent(sched::ProcessId pid) const;
    Status render(gui::GuiCompositor &compositor, Kernel &kernel);

    gui::SurfaceId surface_id_{0};
    sched::ProcessId process_id_{0};
    user::Credentials credentials_{user::kernel_credentials()};
    TaskMonitorProgram program_{TaskMonitorProgram::task_manager};
    usize render_count_{0};
    usize process_scroll_{0};
    u64 last_total_dispatches_{0};
    std::array<u64, smp::max_cpus> last_cpu_dispatches_{};
    std::array<u64, smp::max_cpus> last_cpu_busy_dispatches_{};
    std::array<u8, smp::max_cpus> cpu_usage_percent_{};
    std::array<ProcessUsageSample, sched::max_processes> process_usage_samples_{};
    std::array<ProcessUsageSample, sched::max_processes> last_process_usage_samples_{};
    usize process_usage_sample_count_{0};
    usize last_process_usage_sample_count_{0};
    u8 key_escape_state_{0};
    bool has_usage_sample_{false};
};

} // namespace apps
} // namespace ok
