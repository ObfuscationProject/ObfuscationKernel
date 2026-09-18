#pragma once

#include "ok/core/module.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/gui/gui.hpp"

namespace ok
{
class Kernel;
enum class ExtensionSurfaceAction : u8
{
    focus,
    refresh,
    close
};

// In-process SDK interface for a statically linked distribution component.
// This is not a stable binary module ABI. Build against the matching exported SDK.
// Callbacks run synchronously on the kernel GUI path; the owner outlives Kernel.
class KernelExtension
{
  public:
    virtual ~KernelExtension() = default;
    virtual Status self_test(Kernel &)
    {
        return Status::success();
    }
    virtual Status start(Kernel &)
    {
        return Status::success();
    }
    virtual Result<fs::FileBuffer> read_package(Kernel &, std::string_view)
    {
        return Status::not_found("extension package not found");
    }
    virtual Status load_package(Kernel &, std::string_view, const ModuleImageInfo &)
    {
        return Status::unsupported("no prelinked extension accepts this package");
    }
    virtual Result<bool> pointer_click(Kernel &, i32, i32)
    {
        return false;
    }
    virtual Result<bool> key(Kernel &, int)
    {
        return false;
    }
    virtual Status surface_action(Kernel &, gui::SurfaceId, ExtensionSurfaceAction)
    {
        return Status::not_found("surface is not extension-owned");
    }
    virtual Status process_exited(Kernel &, sched::ProcessId)
    {
        return Status::success();
    }
    virtual Status refresh(Kernel &, bool)
    {
        return Status::success();
    }
};
} // namespace ok
