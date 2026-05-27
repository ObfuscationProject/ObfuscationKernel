#include "ok/apps/task_manager.hpp"

#include "ok/core/kernel.hpp"

namespace ok::apps
{
namespace
{

using gui::GuiCompositor;
using gui::Rect;
using gui::SurfaceId;
using gui::WindowState;
using gui::gui_glyph_height;
using gui::gui_glyph_width;
using gui::taskbar_height;

constexpr Rect task_manager_bounds{.x = 52, .y = 28, .width = 356, .height = 196};
constexpr u32 background_color = 0xff101820u;
constexpr u32 panel_color = 0xff172331u;
constexpr u32 title_color = 0xff12313du;
constexpr u32 header_color = 0xff203a49u;
constexpr u32 bar_cpu_color = 0xff44aa88u;
constexpr u32 bar_io_color = 0xffffcc66u;
constexpr u32 text_color = 0xffd8f3ffu;
constexpr u32 muted_text_color = 0xff9fc6d2u;

template <usize Capacity> Status append_decimal(FixedString<Capacity> &out, u64 value)
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

template <usize Capacity> Status append_padded(FixedString<Capacity> &out, std::string_view text, usize width)
{
    const auto count = text.size() < width ? text.size() : width;
    if (auto status = out.append(text.substr(0, count)); !status.ok())
    {
        return status;
    }
    for (usize i = count; i < width; ++i)
    {
        if (auto status = out.append(' '); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

template <usize Capacity> Status append_padded_decimal(FixedString<Capacity> &out, u64 value, usize width)
{
    FixedString<32> number;
    if (auto status = append_decimal(number, value); !status.ok())
    {
        return status;
    }
    for (usize i = number.size(); i < width; ++i)
    {
        if (auto status = out.append(' '); !status.ok())
        {
            return status;
        }
    }
    return out.append(number.view());
}

template <usize Capacity> Status append_percent(FixedString<Capacity> &out, u64 value)
{
    if (auto status = append_decimal(out, value); !status.ok())
    {
        return status;
    }
    return out.append('%');
}

std::string_view process_state_label(sched::ProcessState state)
{
    switch (state)
    {
    case sched::ProcessState::created:
        return "new";
    case sched::ProcessState::runnable:
        return "run";
    case sched::ProcessState::running:
        return "cpu";
    case sched::ProcessState::blocked:
        return "wait";
    case sched::ProcessState::exited:
        return "exit";
    }
    return "?";
}

std::string_view scheduler_mode_label(sched::SchedulingMode mode)
{
    switch (mode)
    {
    case sched::SchedulingMode::cooperative:
        return "cooperative";
    case sched::SchedulingMode::round_robin:
        return "round-robin";
    case sched::SchedulingMode::per_cpu_round_robin:
        return "per-cpu-rr";
    }
    return "unknown";
}

template <usize Capacity> Status append_process_line(FixedString<Capacity> &out, const sched::Scheduler &scheduler,
                                                     const sched::ProcessControlBlock &process)
{
    if (auto status = append_padded_decimal(out, process.pid(), 5); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(' '); !status.ok())
    {
        return status;
    }
    if (auto status = append_padded(out, process_state_label(process.state()), 4); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(' '); !status.ok())
    {
        return status;
    }
    if (auto status = append_padded_decimal(out, scheduler.process_usage_percent(process.pid()), 3); !status.ok())
    {
        return status;
    }
    if (auto status = out.append("% "); !status.ok())
    {
        return status;
    }
    if (auto status = append_padded_decimal(out, process.priority(), 3); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(' '); !status.ok())
    {
        return status;
    }
    if (auto status = append_padded_decimal(out, process.threads().size(), 3); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(' '); !status.ok())
    {
        return status;
    }
    if (auto status = append_padded(out, process.name(), 24); !status.ok())
    {
        return status;
    }
    return out.append('\n');
}

Status draw_line(GuiCompositor &compositor, SurfaceId surface, u32 row, std::string_view text, u32 foreground,
                 u32 background)
{
    return compositor.draw_text(surface, 2, row, text, foreground, background);
}

usize visible_process_rows(const gui::SurfaceInfo &info)
{
    const auto rows = info.bounds.height / gui_glyph_height;
    return rows > 15 ? rows - 15 : static_cast<usize>(0);
}

usize max_process_scroll(Kernel &kernel, const gui::SurfaceInfo &info)
{
    const auto visible = visible_process_rows(info);
    const auto count = kernel.scheduler().process_count();
    return count > visible ? count - visible : static_cast<usize>(0);
}

Status fill_rect_if_non_empty(GuiCompositor &compositor, SurfaceId surface, Rect rect, u32 rgba)
{
    if (rect.width == 0 || rect.height == 0)
    {
        return Status::success();
    }
    return compositor.fill_rect(surface, rect, rgba);
}

} // namespace

Status KernelTaskManager::open(GuiCompositor &compositor, Kernel &kernel, user::Credentials credentials,
                               sched::ProcessId process_id, TaskMonitorProgram program)
{
    credentials_ = credentials;
    process_id_ = process_id;
    program_ = program;
    const auto title = program_ == TaskMonitorProgram::top ? std::string_view{"top"} : std::string_view{"task-manager"};
    if (surface_id_ != 0 && !compositor.surface_info(surface_id_))
    {
        surface_id_ = 0;
    }
    if (surface_id_ == 0)
    {
        auto bounds = task_manager_bounds;
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
        auto surface = compositor.create_surface(bounds, title);
        if (!surface)
        {
            return surface.status();
        }
        surface_id_ = surface.value();
    }
    else if (auto status = compositor.set_title(surface_id_, title); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.raise_surface(surface_id_); !status.ok())
    {
        return status;
    }
    return render(compositor, kernel);
}

Status KernelTaskManager::refresh(GuiCompositor &compositor, Kernel &kernel)
{
    if (surface_id_ == 0)
    {
        return Status::not_initialized("task manager surface is not open");
    }
    return render(compositor, kernel);
}

Status KernelTaskManager::handle_key(GuiCompositor &compositor, Kernel &kernel, int key)
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
    if (key_escape_state_ == 2)
    {
        key_escape_state_ = 0;
        if (key == 'A')
        {
            return scroll_processes(compositor, kernel, gui::scroll_rows(gui::ScrollDirection::previous));
        }
        if (key == 'B')
        {
            return scroll_processes(compositor, kernel, gui::scroll_rows(gui::ScrollDirection::next));
        }
    }
    return Status::success();
}

Status KernelTaskManager::scroll_processes(GuiCompositor &compositor, Kernel &kernel, i32 rows)
{
    if (surface_id_ == 0 || rows == 0)
    {
        return Status::success();
    }
    auto info = compositor.surface_info(surface_id_);
    if (!info)
    {
        mark_closed();
        return Status::success();
    }
    const auto max_scroll = max_process_scroll(kernel, info.value());
    if (process_scroll_ > max_scroll)
    {
        process_scroll_ = max_scroll;
    }
    const auto command = gui::scroll_command_from_rows(rows);
    if (command.direction == gui::ScrollDirection::next)
    {
        const auto room = max_scroll - process_scroll_;
        process_scroll_ += command.rows > room ? room : command.rows;
    }
    else if (command.rows >= process_scroll_)
    {
        process_scroll_ = 0;
    }
    else
    {
        process_scroll_ -= command.rows;
    }
    return render(compositor, kernel);
}

Status KernelTaskManager::close(GuiCompositor &compositor)
{
    if (surface_id_ == 0)
    {
        process_id_ = 0;
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

void KernelTaskManager::mark_closed()
{
    surface_id_ = 0;
    process_id_ = 0;
    program_ = TaskMonitorProgram::task_manager;
    process_scroll_ = 0;
    key_escape_state_ = 0;
}

Status KernelTaskManager::render_tui(Kernel &kernel, FixedString<4096> &out) const
{
    out.clear();
    auto &scheduler = kernel.scheduler();
    const auto net = kernel.network().stats();
    const auto disk = kernel.disk().io_stats();
    const auto disk_geometry = kernel.disk().geometry();
    const auto dispatches = scheduler.total_dispatches();

    if (auto status = out.append("TASK MANAGER\n"); !status.ok())
    {
        return status;
    }
    if (auto status = out.append("scheduler="); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(scheduler_mode_label(scheduler.mode())); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" processes="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, scheduler.process_count()); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" dispatches="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, dispatches); !status.ok())
    {
        return status;
    }
    if (auto status = out.append("\n\nCPU\n"); !status.ok())
    {
        return status;
    }
    for (usize i = 0; i < scheduler.cpu_count(); ++i)
    {
        const auto cpu = static_cast<smp::CpuId>(i);
        const auto stats = scheduler.cpu_stats(cpu);
        if (auto status = out.append("  cpu"); !status.ok())
        {
            return status;
        }
        if (auto status = append_decimal(out, i); !status.ok())
        {
            return status;
        }
        if (auto status = out.append(" use="); !status.ok())
        {
            return status;
        }
        if (auto status = append_percent(out, scheduler.cpu_usage_percent(cpu)); !status.ok())
        {
            return status;
        }
        if (auto status = out.append(" dispatches="); !status.ok())
        {
            return status;
        }
        if (auto status = append_decimal(out, stats.dispatches); !status.ok())
        {
            return status;
        }
        if (auto status = out.append(" current="); !status.ok())
        {
            return status;
        }
        if (auto status = append_decimal(out, stats.current_pid); !status.ok())
        {
            return status;
        }
        if (auto status = out.append('/'); !status.ok())
        {
            return status;
        }
        if (auto status = append_decimal(out, stats.current_tid); !status.ok())
        {
            return status;
        }
        if (auto status = out.append('\n'); !status.ok())
        {
            return status;
        }
    }

    if (auto status = out.append("\nNET tx_bytes="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, net.bytes_tx); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" rx_bytes="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, net.bytes_rx); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" ipv4="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, net.ipv4_tx); !status.ok())
    {
        return status;
    }
    if (auto status = out.append('/'); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, net.ipv4_rx); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" udp="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, net.udp_tx); !status.ok())
    {
        return status;
    }
    if (auto status = out.append('/'); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, net.udp_rx); !status.ok())
    {
        return status;
    }

    if (auto status = out.append("\nDISK "); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(kernel.disk_name()); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" blocks="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, disk_geometry.block_count); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" read_bytes="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, disk.bytes_read); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" write_bytes="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, disk.bytes_written); !status.ok())
    {
        return status;
    }

    if (auto info = kernel.simplefs().info())
    {
        if (auto status = out.append(" files="); !status.ok())
        {
            return status;
        }
        if (auto status = append_decimal(out, info.value().file_count); !status.ok())
        {
            return status;
        }
    }

    if (auto status = out.append("\n\n  PID STAT CPU PRI THR COMMAND\n"); !status.ok())
    {
        return status;
    }
    for (const auto &process : scheduler.processes())
    {
        if (auto status = append_process_line(out, scheduler, process); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status KernelTaskManager::render_top_tui(Kernel &kernel, FixedString<4096> &out) const
{
    out.clear();
    auto &scheduler = kernel.scheduler();
    const auto dispatches = scheduler.total_dispatches();

    if (auto status = out.append("top - "); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, scheduler.cpu_count()); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" cpus, "); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, scheduler.process_count()); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" tasks, "); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, dispatches); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" dispatches\n"); !status.ok())
    {
        return status;
    }

    if (auto status = out.append("%Cpu(s): "); !status.ok())
    {
        return status;
    }
    for (usize i = 0; i < scheduler.cpu_count(); ++i)
    {
        if (i != 0)
        {
            if (auto status = out.append("  "); !status.ok())
            {
                return status;
            }
        }
        if (auto status = out.append("cpu"); !status.ok())
        {
            return status;
        }
        if (auto status = append_decimal(out, i); !status.ok())
        {
            return status;
        }
        if (auto status = out.append("="); !status.ok())
        {
            return status;
        }
        if (auto status = append_percent(out, scheduler.cpu_usage_percent(static_cast<smp::CpuId>(i))); !status.ok())
        {
            return status;
        }
    }
    if (auto status = out.append("\nMem: page="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, kernel.memory().frames().page_size()); !status.ok())
    {
        return status;
    }
    if (auto status = out.append(" free_frames="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(out, kernel.memory().frames().free_frames()); !status.ok())
    {
        return status;
    }
    if (auto status = out.append("\n\n  PID USER    STAT CPU PRI THR COMMAND\n"); !status.ok())
    {
        return status;
    }
    for (const auto &process : scheduler.processes())
    {
        if (auto status = append_padded_decimal(out, process.pid(), 5); !status.ok())
        {
            return status;
        }
        if (auto status = out.append(' '); !status.ok())
        {
            return status;
        }
        std::string_view user_label = "user";
        if (process.credentials().kernel_space)
        {
            user_label = "kernel";
        }
        else if (const auto *account = kernel.user_space().users().find_by_uid(process.credentials().euid);
                 account != nullptr)
        {
            user_label = account->name.view();
        }
        if (auto status = append_padded(out, user_label, 7); !status.ok())
        {
            return status;
        }
        if (auto status = out.append(' '); !status.ok())
        {
            return status;
        }
        if (auto status = append_padded(out, process_state_label(process.state()), 4); !status.ok())
        {
            return status;
        }
        if (auto status = out.append(' '); !status.ok())
        {
            return status;
        }
        if (auto status = append_padded_decimal(out, scheduler.process_usage_percent(process.pid()), 3); !status.ok())
        {
            return status;
        }
        if (auto status = out.append("% "); !status.ok())
        {
            return status;
        }
        if (auto status = append_padded_decimal(out, process.priority(), 3); !status.ok())
        {
            return status;
        }
        if (auto status = out.append(' '); !status.ok())
        {
            return status;
        }
        if (auto status = append_padded_decimal(out, process.threads().size(), 3); !status.ok())
        {
            return status;
        }
        if (auto status = out.append(' '); !status.ok())
        {
            return status;
        }
        if (auto status = out.append(process.name()); !status.ok())
        {
            return status;
        }
        if (auto status = out.append('\n'); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status KernelTaskManager::render(GuiCompositor &compositor, Kernel &kernel)
{
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

    const auto width = info.value().bounds.width;
    const auto height = info.value().bounds.height;
    if (auto status = compositor.fill(surface_id_, background_color); !status.ok())
    {
        return status;
    }
    if (auto status = fill_rect_if_non_empty(
            compositor, surface_id_,
            Rect{.x = 2, .y = 2, .width = width > 4 ? width - 4 : 0, .height = gui_glyph_height * 2},
            title_color);
        !status.ok())
    {
        return status;
    }
    if (auto status = fill_rect_if_non_empty(
            compositor, surface_id_,
            Rect{.x = 2, .y = static_cast<i32>(gui_glyph_height * 3), .width = width > 4 ? width - 4 : 0,
                 .height = height > gui_glyph_height * 3 + 4 ? height - gui_glyph_height * 3 - 4 : 0},
            panel_color);
        !status.ok())
    {
        return status;
    }

    const auto title_text = program_ == TaskMonitorProgram::top ? std::string_view{"TOP"} : std::string_view{"TASK MANAGER"};
    if (auto status = draw_line(compositor, surface_id_, 1, title_text, text_color, title_color); !status.ok())
    {
        return status;
    }

    auto &scheduler = kernel.scheduler();
    FixedString<160> summary;
    if (auto status = summary.assign("CPU cores="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(summary, scheduler.cpu_count()); !status.ok())
    {
        return status;
    }
    if (auto status = summary.append(" procs="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(summary, scheduler.process_count()); !status.ok())
    {
        return status;
    }
    if (auto status = summary.append(" dispatches="); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(summary, scheduler.total_dispatches()); !status.ok())
    {
        return status;
    }
    if (auto status = draw_line(compositor, surface_id_, 3, summary.view(), text_color, panel_color); !status.ok())
    {
        return status;
    }

    const u32 bar_left = 76;
    const u32 bar_width = width > bar_left + 14 ? width - bar_left - 14 : 0;
    const auto max_cpu_rows = scheduler.cpu_count() < 6 ? scheduler.cpu_count() : static_cast<usize>(6);
    for (usize i = 0; i < max_cpu_rows; ++i)
    {
        const auto row = static_cast<u32>(5 + i);
        const auto cpu = static_cast<smp::CpuId>(i);
        const auto usage = scheduler.cpu_usage_percent(cpu);
        FixedString<64> line;
        if (auto status = line.assign("cpu"); !status.ok())
        {
            return status;
        }
        if (auto status = append_decimal(line, i); !status.ok())
        {
            return status;
        }
        if (auto status = line.append(" "); !status.ok())
        {
            return status;
        }
        if (auto status = append_percent(line, usage); !status.ok())
        {
            return status;
        }
        const auto y = static_cast<i32>(row * gui_glyph_height + 3);
        if (auto status = fill_rect_if_non_empty(
                compositor, surface_id_, Rect{.x = static_cast<i32>(bar_left), .y = y, .width = bar_width, .height = 5},
                0xff0d141cu);
            !status.ok())
        {
            return status;
        }
        if (auto status = fill_rect_if_non_empty(
                compositor, surface_id_,
                Rect{.x = static_cast<i32>(bar_left), .y = y,
                     .width = static_cast<u32>((static_cast<u64>(bar_width) * usage) / 100u), .height = 5},
                bar_cpu_color);
            !status.ok())
        {
            return status;
        }
        if (auto status = draw_line(compositor, surface_id_, row, line.view(), text_color, panel_color); !status.ok())
        {
            return status;
        }
    }

    const auto net = kernel.network().stats();
    const auto disk = kernel.disk().io_stats();
    FixedString<160> io_line;
    if (auto status = io_line.assign("NET tx/rx "); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(io_line, net.bytes_tx); !status.ok())
    {
        return status;
    }
    if (auto status = io_line.append("/"); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(io_line, net.bytes_rx); !status.ok())
    {
        return status;
    }
    if (auto status = io_line.append("  DISK r/w "); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(io_line, disk.bytes_read); !status.ok())
    {
        return status;
    }
    if (auto status = io_line.append("/"); !status.ok())
    {
        return status;
    }
    if (auto status = append_decimal(io_line, disk.bytes_written); !status.ok())
    {
        return status;
    }
    if (auto status = fill_rect_if_non_empty(
            compositor, surface_id_,
            Rect{.x = 2, .y = static_cast<i32>(12 * gui_glyph_height), .width = width > 4 ? width - 4 : 0,
                 .height = gui_glyph_height},
            header_color);
        !status.ok())
    {
        return status;
    }
    if (auto status = fill_rect_if_non_empty(
            compositor, surface_id_,
            Rect{.x = static_cast<i32>(width > 94 ? width - 94 : 4), .y = static_cast<i32>(12 * gui_glyph_height + 3),
                 .width = 74, .height = 5},
            bar_io_color);
        !status.ok())
    {
        return status;
    }
    if (auto status = draw_line(compositor, surface_id_, 12, io_line.view(), text_color, header_color); !status.ok())
    {
        return status;
    }

    if (auto status = draw_line(compositor, surface_id_, 14, "  PID STAT CPU PRI THR COMMAND", muted_text_color,
                                panel_color);
        !status.ok())
    {
        return status;
    }
    const auto rows = height / gui_glyph_height;
    const auto visible = visible_process_rows(info.value());
    const auto max_scroll = max_process_scroll(kernel, info.value());
    if (process_scroll_ > max_scroll)
    {
        process_scroll_ = max_scroll;
    }
    usize rendered = 0;
    usize skipped = 0;
    for (const auto &process : scheduler.processes())
    {
        if (skipped < process_scroll_)
        {
            ++skipped;
            continue;
        }
        if (rendered >= visible)
        {
            break;
        }
        const auto row = static_cast<u32>(15 + rendered);
        if (row >= rows)
        {
            break;
        }
        FixedString<160> process_line;
        if (auto status = append_process_line(process_line, scheduler, process); !status.ok())
        {
            return status;
        }
        if (!process_line.empty())
        {
            process_line.pop_back();
        }
        const auto row_color = (rendered % 2) == 0 ? 0xff111c24u : panel_color;
        if (auto status = fill_rect_if_non_empty(
                compositor, surface_id_,
                Rect{.x = 2, .y = static_cast<i32>(row * gui_glyph_height), .width = width > 4 ? width - 4 : 0,
                     .height = gui_glyph_height},
                row_color);
            !status.ok())
        {
            return status;
        }
        if (auto status = draw_line(compositor, surface_id_, row, process_line.view(), text_color, row_color);
            !status.ok())
        {
            return status;
        }
        ++rendered;
    }

    if (auto status = compositor.present(); !status.ok())
    {
        return status;
    }
    ++render_count_;
    return Status::success();
}

} // namespace ok::apps
