#pragma once

#include "ok/core/types.hpp"
#include "virtio_input.hpp"

namespace ok::driver::qemu_virt
{

template <uptr PciEcamBase, uptr PciMmioBase> class VirtioPciInputDevice final
{
  public:
    bool initialize(VirtioInputKind kind)
    {
        if (initialized_)
        {
            return ready_;
        }
        initialized_ = true;
        for (u8 slot = first_pci_slot; slot < max_pci_slots; ++slot)
        {
            if (pci_config_read16(slot, pci_vendor_id) != virtio_vendor_id ||
                pci_config_read16(slot, pci_device_id) != virtio_modern_input_device_id)
            {
                continue;
            }
            configure_bars(slot);
            if (!load_caps(slot) || !matches_kind(kind))
            {
                continue;
            }
            ready_ = setup_queue();
            return ready_;
        }
        return false;
    }

    bool poll(VirtioInputEvent &event)
    {
        if (!ready_)
        {
            return false;
        }
        asm volatile("" ::: "memory");
        if (last_used_index_ == used_->idx)
        {
            return false;
        }
        const auto used_slot = static_cast<usize>(last_used_index_ % queue_size);
        const auto descriptor = static_cast<u16>(used_->ring[used_slot].id % queue_size);
        ++last_used_index_;
        event = events_[descriptor];
        recycle_descriptor(descriptor);
        return true;
    }

  private:
    static constexpr u8 first_pci_slot = 1;
    static constexpr u8 max_pci_slots = 32;
    static constexpr uptr pci_slot_stride = 0x10000;
    static constexpr uptr pci_bar1_offset = 0x0000;
    static constexpr uptr pci_bar4_offset = 0x8000;
    static constexpr u16 pci_vendor_id = 0x00;
    static constexpr u16 pci_device_id = 0x02;
    static constexpr u16 pci_command = 0x04;
    static constexpr u16 pci_status = 0x06;
    static constexpr u16 pci_capability_pointer = 0x34;
    static constexpr u16 pci_bar0 = 0x10;
    static constexpr u16 pci_bar1 = 0x14;
    static constexpr u16 pci_bar4 = 0x20;
    static constexpr u16 pci_bar5 = 0x24;
    static constexpr u16 pci_command_memory = 1u << 1;
    static constexpr u16 pci_command_bus_master = 1u << 2;
    static constexpr u16 pci_status_capability_list = 1u << 4;
    static constexpr u8 pci_cap_vendor_specific = 0x09;
    static constexpr u16 virtio_vendor_id = 0x1af4;
    static constexpr u16 virtio_modern_input_device_id = 0x1052;
    static constexpr u8 virtio_pci_cap_common_cfg = 1;
    static constexpr u8 virtio_pci_cap_notify_cfg = 2;
    static constexpr u8 virtio_pci_cap_isr_cfg = 3;
    static constexpr u8 virtio_pci_cap_device_cfg = 4;

    static constexpr uptr common_device_feature_select = 0x00;
    static constexpr uptr common_device_feature = 0x04;
    static constexpr uptr common_driver_feature_select = 0x08;
    static constexpr uptr common_driver_feature = 0x0c;
    static constexpr uptr common_device_status = 0x14;
    static constexpr uptr common_queue_select = 0x16;
    static constexpr uptr common_queue_size = 0x18;
    static constexpr uptr common_queue_enable = 0x1c;
    static constexpr uptr common_queue_notify_off = 0x1e;
    static constexpr uptr common_queue_desc = 0x20;
    static constexpr uptr common_queue_driver = 0x28;
    static constexpr uptr common_queue_device = 0x30;
    static constexpr uptr input_config_select = 0x00;
    static constexpr uptr input_config_subsel = 0x01;
    static constexpr uptr input_config_size = 0x02;
    static constexpr uptr input_config_data = 0x08;

    static constexpr u8 config_select_event_bits = 0x11;
    static constexpr u8 status_acknowledge = 1u;
    static constexpr u8 status_driver = 2u;
    static constexpr u8 status_driver_ok = 4u;
    static constexpr u8 status_features_ok = 8u;
    static constexpr u32 feature_version_1 = 1u << 0;
    static constexpr u16 descriptor_flag_write = 2u;
    static constexpr usize queue_size = 16;

    struct Descriptor
    {
        u64 address;
        u32 length;
        u16 flags;
        u16 next;
    };

    struct AvailableRing
    {
        u16 flags;
        u16 idx;
        u16 ring[queue_size];
        u16 used_event;
    };

    struct UsedElement
    {
        u32 id;
        u32 length;
    };

    struct UsedRing
    {
        u16 flags;
        volatile u16 idx;
        UsedElement ring[queue_size];
        u16 available_event;
    };

    static volatile u8 &mmio8(uptr address)
    {
        return *reinterpret_cast<volatile u8 *>(address);
    }

    static volatile u16 &mmio16(uptr address)
    {
        return *reinterpret_cast<volatile u16 *>(address);
    }

    static volatile u32 &mmio32(uptr address)
    {
        return *reinterpret_cast<volatile u32 *>(address);
    }

    static volatile u64 &mmio64(uptr address)
    {
        return *reinterpret_cast<volatile u64 *>(address);
    }

    static uptr pci_config_address(u8 slot, u16 offset)
    {
        return PciEcamBase + (static_cast<uptr>(slot) << 15) + offset;
    }

    static u8 pci_config_read8(u8 slot, u16 offset)
    {
        return mmio8(pci_config_address(slot, offset));
    }

    static u16 pci_config_read16(u8 slot, u16 offset)
    {
        return mmio16(pci_config_address(slot, offset));
    }

    static u32 pci_config_read32(u8 slot, u16 offset)
    {
        return mmio32(pci_config_address(slot, offset));
    }

    static void pci_config_write16(u8 slot, u16 offset, u16 value)
    {
        mmio16(pci_config_address(slot, offset)) = value;
    }

    static void pci_config_write32(u8 slot, u16 offset, u32 value)
    {
        mmio32(pci_config_address(slot, offset)) = value;
    }

    static uptr assigned_bar_base(u8 slot, u8 bar)
    {
        const auto slot_base = PciMmioBase + static_cast<uptr>(slot) * pci_slot_stride;
        if (bar == 4)
        {
            return slot_base + pci_bar4_offset;
        }
        if (bar == 1)
        {
            return slot_base + pci_bar1_offset;
        }
        return 0;
    }

    static void configure_bars(u8 slot)
    {
        pci_config_write32(slot, pci_bar0, 0);
        pci_config_write32(slot, pci_bar1, static_cast<u32>(assigned_bar_base(slot, 1)));
        pci_config_write32(slot, pci_bar4, static_cast<u32>(assigned_bar_base(slot, 4)));
        pci_config_write32(slot, pci_bar5, 0);
        pci_config_write16(slot, pci_command, pci_command_memory | pci_command_bus_master);
    }

    static u32 cap_read32(u8 slot, u8 cap, u8 offset)
    {
        return pci_config_read32(slot, static_cast<u16>(cap + offset));
    }

    bool load_caps(u8 slot)
    {
        common_cfg_ = 0;
        notify_cfg_ = 0;
        isr_cfg_ = 0;
        device_cfg_ = 0;
        notify_multiplier_ = 0;
        if ((pci_config_read16(slot, pci_status) & pci_status_capability_list) == 0)
        {
            return false;
        }
        u8 cap = pci_config_read8(slot, pci_capability_pointer) & 0xfcu;
        for (u8 guard = 0; cap != 0 && guard < 32; ++guard)
        {
            const u8 cap_id = pci_config_read8(slot, cap);
            const u8 next = pci_config_read8(slot, static_cast<u16>(cap + 1)) & 0xfcu;
            if (cap_id == pci_cap_vendor_specific)
            {
                const u8 cfg_type = pci_config_read8(slot, static_cast<u16>(cap + 3));
                const u8 bar = pci_config_read8(slot, static_cast<u16>(cap + 4));
                const u32 offset = cap_read32(slot, cap, 8);
                const auto base = assigned_bar_base(slot, bar);
                if (base != 0)
                {
                    const auto address = base + offset;
                    if (cfg_type == virtio_pci_cap_common_cfg)
                    {
                        common_cfg_ = address;
                    }
                    else if (cfg_type == virtio_pci_cap_notify_cfg)
                    {
                        notify_cfg_ = address;
                        notify_multiplier_ = cap_read32(slot, cap, 16);
                    }
                    else if (cfg_type == virtio_pci_cap_isr_cfg)
                    {
                        isr_cfg_ = address;
                    }
                    else if (cfg_type == virtio_pci_cap_device_cfg)
                    {
                        device_cfg_ = address;
                    }
                }
            }
            cap = next;
        }
        return common_cfg_ != 0 && notify_cfg_ != 0 && device_cfg_ != 0;
    }

    bool event_bit_supported(u16 event_type, u16 code)
    {
        mmio8(device_cfg_ + input_config_subsel) = static_cast<u8>(event_type);
        mmio8(device_cfg_ + input_config_select) = config_select_event_bits;
        asm volatile("" ::: "memory");
        const auto byte_index = static_cast<usize>(code / 8);
        if (byte_index >= mmio8(device_cfg_ + input_config_size))
        {
            return false;
        }
        const u8 bits = mmio8(device_cfg_ + input_config_data + byte_index);
        return ((bits >> (code % 8)) & 1u) != 0;
    }

    bool matches_kind(VirtioInputKind kind)
    {
        const bool has_keyboard_key =
            event_bit_supported(input_event_key, input_key_a) || event_bit_supported(input_event_key, input_key_enter);
        const bool has_relative_motion = event_bit_supported(input_event_relative, input_relative_x) ||
                                         event_bit_supported(input_event_relative, input_relative_y);
        const bool has_mouse_button = event_bit_supported(input_event_key, input_button_left);
        if (kind == VirtioInputKind::keyboard)
        {
            return has_keyboard_key && !has_relative_motion;
        }
        return has_relative_motion || has_mouse_button;
    }

    void reset_queue()
    {
        descriptors_ = descriptors_storage_;
        available_ = &available_storage_;
        used_ = &used_storage_;
        *available_ = {};
        *used_ = {};
        available_index_ = 0;
        last_used_index_ = 0;
        for (u16 i = 0; i < queue_size; ++i)
        {
            events_[i] = {};
            descriptors_[i] = Descriptor{
                .address = static_cast<u64>(reinterpret_cast<uptr>(&events_[i])),
                .length = sizeof(VirtioInputEvent),
                .flags = descriptor_flag_write,
                .next = 0,
            };
        }
    }

    void notify_queue()
    {
        mmio16(notify_cfg_ + queue_notify_offset_ * notify_multiplier_) = 0;
    }

    bool setup_queue()
    {
        mmio8(common_cfg_ + common_device_status) = 0;
        mmio8(common_cfg_ + common_device_status) = status_acknowledge | status_driver;
        mmio32(common_cfg_ + common_device_feature_select) = 0;
        const u32 ignored_device_features = mmio32(common_cfg_ + common_device_feature);
        static_cast<void>(ignored_device_features);
        mmio32(common_cfg_ + common_driver_feature_select) = 0;
        mmio32(common_cfg_ + common_driver_feature) = 0;
        mmio32(common_cfg_ + common_device_feature_select) = 1;
        if ((mmio32(common_cfg_ + common_device_feature) & feature_version_1) == 0)
        {
            return false;
        }
        mmio32(common_cfg_ + common_driver_feature_select) = 1;
        mmio32(common_cfg_ + common_driver_feature) = feature_version_1;
        mmio8(common_cfg_ + common_device_status) =
            static_cast<u8>(mmio8(common_cfg_ + common_device_status) | status_features_ok);
        if ((mmio8(common_cfg_ + common_device_status) & status_features_ok) == 0)
        {
            return false;
        }

        mmio16(common_cfg_ + common_queue_select) = 0;
        if (mmio16(common_cfg_ + common_queue_size) < queue_size)
        {
            return false;
        }
        mmio16(common_cfg_ + common_queue_size) = queue_size;
        queue_notify_offset_ = mmio16(common_cfg_ + common_queue_notify_off);
        reset_queue();
        mmio64(common_cfg_ + common_queue_desc) = reinterpret_cast<uptr>(descriptors_);
        mmio64(common_cfg_ + common_queue_driver) = reinterpret_cast<uptr>(available_);
        mmio64(common_cfg_ + common_queue_device) = reinterpret_cast<uptr>(used_);
        mmio16(common_cfg_ + common_queue_enable) = 1;

        for (u16 i = 0; i < queue_size; ++i)
        {
            recycle_descriptor(i);
        }
        mmio8(common_cfg_ + common_device_status) =
            static_cast<u8>(mmio8(common_cfg_ + common_device_status) | status_driver_ok);
        notify_queue();
        return true;
    }

    void recycle_descriptor(u16 descriptor)
    {
        available_->ring[available_index_ % queue_size] = descriptor;
        asm volatile("" ::: "memory");
        ++available_index_;
        available_->idx = available_index_;
        asm volatile("" ::: "memory");
        notify_queue();
        if (isr_cfg_ != 0)
        {
            const u8 ignored_isr = mmio8(isr_cfg_);
            static_cast<void>(ignored_isr);
        }
    }

    bool initialized_{false};
    bool ready_{false};
    uptr common_cfg_{0};
    uptr notify_cfg_{0};
    uptr isr_cfg_{0};
    uptr device_cfg_{0};
    u32 notify_multiplier_{0};
    u16 queue_notify_offset_{0};
    u16 available_index_{0};
    u16 last_used_index_{0};
    alignas(16) Descriptor descriptors_storage_[queue_size]{};
    alignas(16) AvailableRing available_storage_{};
    alignas(16) UsedRing used_storage_{};
    Descriptor *descriptors_{descriptors_storage_};
    AvailableRing *available_{&available_storage_};
    UsedRing *used_{&used_storage_};
    alignas(16) VirtioInputEvent events_[queue_size]{};
};

} // namespace ok::driver::qemu_virt
