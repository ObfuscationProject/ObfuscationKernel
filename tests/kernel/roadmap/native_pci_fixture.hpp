#pragma once

#include "ok/driver/abi.hpp"

namespace ok::driver::test
{

class NativeFakePciDriver final : public OkDriverModule
{
  public:
    [[nodiscard]] OkDriverManifest manifest() const override;
    Status probe(OkProbeContext &context) override;
    Status remove(OkProbeContext &context) override;
    [[nodiscard]] bool probed() const
    {
        return probed_;
    }
    [[nodiscard]] bool removed() const
    {
        return removed_;
    }

  private:
    bool probed_{false};
    bool removed_{false};
};

inline OkDriverManifest NativeFakePciDriver::manifest() const
{
    static const std::array ids{
        OkDeviceId{.vendor = 0x1af4, .device = 0x1000, .class_code = 0x02},
    };
    return OkDriverManifest{
        .name = "native-fake-pci",
        .version = "1",
        .bus = OkBusType::pci,
        .ids = ids,
    };
}

inline Status NativeFakePciDriver::probe(OkProbeContext &context)
{
    if (context.device == nullptr || context.mmio == nullptr || context.irq == nullptr)
    {
        return Status::invalid_argument("native fake PCI driver probe context is incomplete");
    }
    if (auto status = context.mmio->writel(0, 0x0badc0de); !status.ok())
    {
        return status;
    }
    if (!context.irq->registered)
    {
        return Status::not_initialized("native fake PCI driver IRQ is not registered");
    }
    probed_ = true;
    removed_ = false;
    return Status::success();
}

inline Status NativeFakePciDriver::remove(OkProbeContext &context)
{
    removed_ = true;
    if (context.device != nullptr)
    {
        context.device->attached = false;
    }
    return Status::success();
}

} // namespace ok::driver::test
