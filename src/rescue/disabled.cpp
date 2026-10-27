// Builds without kernel_gui or debug test points do not compile the rescue
// applications. Kernel, its GUI paths and SDK consumers still call their public
// interfaces; these definitions behave like applications that were never opened.
#if defined(OK_DISABLE_RESCUE_APPS)
#include "ok/apps/file_manager.hpp"
#include "ok/apps/shell.hpp"
#include "ok/apps/task_manager.hpp"

namespace ok
{
namespace
{
Status not_built()
{
    return Status::unsupported("kernel rescue applications require kernel_gui or a debug build");
}
} // namespace

Status KernelDebugShell::attach(Kernel &) { return Status::success(); }
Result<std::string_view> KernelDebugShell::execute(std::string_view) { return not_built(); }
Status KernelDebugShell::show_gui() { return not_built(); }
Status KernelDebugShell::show_or_focus_gui() { return not_built(); }
Status KernelDebugShell::close_gui() { return Status::success(); }
Status KernelDebugShell::close_all_gui() { return Status::success(); }
Status KernelDebugShell::set_gui_input(std::string_view) { return Status::success(); }
Status KernelDebugShell::handle_key(int) { return Status::success(); }
Status KernelDebugShell::handle_key(gui::SurfaceId, int) { return Status::not_found("GUI shell surface not found"); }
Status KernelDebugShell::scroll_gui_history(i32) { return Status::success(); }
Status KernelDebugShell::tick() { return Status::success(); }
void KernelDebugShell::mark_gui_closed() {}
Status KernelDebugShell::reconcile_gui_windows() { return Status::success(); }
void KernelDebugShell::notify_process_exit(sched::ProcessId) {}
Status KernelDebugShell::close_process_window(sched::ProcessId) { return Status::not_found("GUI shell process not found"); }
Status KernelDebugShell::close_surface_window(gui::SurfaceId) { return Status::not_found("GUI shell surface not found"); }
Status KernelDebugShell::handle_surface_changed(gui::SurfaceId) { return Status::not_found("GUI shell surface not found"); }
Status KernelDebugShell::start_foreground_process(sched::ProcessId) { return not_built(); }
Status KernelDebugShell::interrupt_foreground_process() { return Status::success(); }
bool KernelDebugShell::owns_process(sched::ProcessId) const { return false; }
bool KernelDebugShell::owns_surface(gui::SurfaceId) const { return false; }
bool KernelDebugShell::gui_ready() { return false; }

namespace apps
{
Status KernelFileManager::open(gui::GuiCompositor &, fs::VirtualFileSystem &, std::string_view, user::Credentials,
                               sched::ProcessId)
{
    return not_built();
}
Status KernelFileManager::refresh(gui::GuiCompositor &, fs::VirtualFileSystem &) { return Status::success(); }
Status KernelFileManager::close(gui::GuiCompositor &) { return Status::success(); }
void KernelFileManager::mark_closed() {}
Status KernelFileManager::render_tui(fs::VirtualFileSystem &, std::string_view, user::Credentials,
                                     FixedString<4096> &) const
{
    return not_built();
}
Status KernelFileManager::handle_surface_changed(gui::GuiCompositor &, fs::VirtualFileSystem &)
{
    return Status::success();
}
Status KernelFileManager::handle_mouse(gui::GuiCompositor &, fs::VirtualFileSystem &, i32, i32, bool)
{
    return Status::success();
}
Status KernelFileManager::handle_key(gui::GuiCompositor &, fs::VirtualFileSystem &, int) { return Status::success(); }

Status KernelTaskManager::open(gui::GuiCompositor &, Kernel &, user::Credentials, sched::ProcessId,
                               TaskMonitorProgram)
{
    return not_built();
}
Status KernelTaskManager::refresh(gui::GuiCompositor &, Kernel &) { return Status::success(); }
Status KernelTaskManager::handle_key(gui::GuiCompositor &, Kernel &, int) { return Status::success(); }
Status KernelTaskManager::scroll_processes(gui::GuiCompositor &, Kernel &, i32) { return Status::success(); }
Status KernelTaskManager::close(gui::GuiCompositor &) { return Status::success(); }
void KernelTaskManager::mark_closed() {}
Status KernelTaskManager::render_tui(Kernel &, FixedString<4096> &) const { return not_built(); }
Status KernelTaskManager::render_top_tui(Kernel &, FixedString<4096> &) const { return not_built(); }
u8 KernelTaskManager::sampled_cpu_usage_percent(smp::CpuId) const { return 0; }
u8 KernelTaskManager::sampled_process_usage_percent(sched::ProcessId) const { return 0; }
} // namespace apps
} // namespace ok
#endif
