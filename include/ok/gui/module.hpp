#pragma once

#include "ok/core/module.hpp"
#include "ok/gui/compositor.hpp"

namespace ok::gui
{

class GuiModule final : public KernelModule, public KernelService
{
  public:
    GuiModule() = default;
    explicit GuiModule(driver::FramebufferDisplayDriver &display)
    {
        bind_display(display);
    }

    void bind_display(driver::FramebufferDisplayDriver &display)
    {
        display_ = &display;
    }

    [[nodiscard]] ModuleManifest manifest() const override;
    [[nodiscard]] std::string_view service_id() const override
    {
        return gui_service_id;
    }
    Status start(ServiceRegistry &) override;
    Status stop() override;
    Status shutdown() override;

    [[nodiscard]] GuiCompositor &compositor()
    {
        return compositor_;
    }
    [[nodiscard]] const GuiCompositor &compositor() const
    {
        return compositor_;
    }

  private:
    driver::FramebufferDisplayDriver *display_{nullptr};
    GuiCompositor compositor_{};
};

class GuiSupervisor final
{
  public:
    GuiSupervisor(ModuleManager &modules, GuiModule &module) : modules_(modules), module_(module)
    {
    }

    Status tick();
    [[nodiscard]] usize restart_attempts() const
    {
        return restart_attempts_;
    }

  private:
    ModuleManager &modules_;
    GuiModule &module_;
    usize restart_attempts_{0};
};

} // namespace ok::gui
