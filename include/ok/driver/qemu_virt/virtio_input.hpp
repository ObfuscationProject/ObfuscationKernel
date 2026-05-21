#pragma once

#include "ok/core/types.hpp"

namespace ok::driver::qemu_virt
{

struct VirtioInputEvent
{
    u16 type;
    u16 code;
    u32 value;
};

inline constexpr u16 input_event_key = 0x01;
inline constexpr u16 input_event_relative = 0x02;
inline constexpr u16 input_relative_x = 0x00;
inline constexpr u16 input_relative_y = 0x01;
inline constexpr u16 input_key_enter = 28;
inline constexpr u16 input_key_a = 30;
inline constexpr u16 input_key_left_shift = 42;
inline constexpr u16 input_key_right_shift = 54;
inline constexpr u16 input_button_left = 0x110;

enum class VirtioInputKind : u8
{
    keyboard,
    mouse,
};

inline char map_linux_key_code(u16 code, bool shifted)
{
    constexpr char normal[] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8',
        '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
        't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
        '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',
        'm', ',', '.', '/', 0, '*', 0, ' ',
    };
    constexpr char shifted_map[] = {
        0, 0, '!', '@', '#', '$', '%', '^', '&', '*',
        '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
        'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
        '"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N',
        'M', '<', '>', '?', 0, '*', 0, ' ',
    };
    if (code >= sizeof(normal))
    {
        return 0;
    }
    return shifted ? shifted_map[code] : normal[code];
}

class VirtioInputDevice final
{
  public:
    bool initialize(const uptr *bases, usize base_count, VirtioInputKind kind)
    {
        if (initialized_)
        {
            return ready_;
        }
        initialized_ = true;
        uptr input_bases[4]{};
        usize input_count = 0;
        for (usize i = 0; i < base_count; ++i)
        {
            const auto base = bases[i];
            if (read32(base + magic_value) != virtio_magic || read32(base + version) == 0 ||
                read32(base + device_id) != input_device_id)
            {
                continue;
            }
            if (input_count < sizeof(input_bases) / sizeof(input_bases[0]))
            {
                input_bases[input_count++] = base;
            }
            if (!matches_kind(base, kind))
            {
                continue;
            }
            base_ = base;
            ready_ = setup_queue();
            return ready_;
        }
        if (const auto fallback = fallback_base(input_bases, input_count, kind); fallback != 0)
        {
            base_ = fallback;
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
    static constexpr usize queue_size = 16;
    static constexpr uptr magic_value = 0x000;
    static constexpr uptr version = 0x004;
    static constexpr uptr device_id = 0x008;
    static constexpr uptr device_features = 0x010;
    static constexpr uptr device_features_sel = 0x014;
    static constexpr uptr driver_features = 0x020;
    static constexpr uptr driver_features_sel = 0x024;
    static constexpr uptr guest_page_size = 0x028;
    static constexpr uptr queue_sel = 0x030;
    static constexpr uptr queue_num_max = 0x034;
    static constexpr uptr queue_num = 0x038;
    static constexpr uptr queue_align = 0x03c;
    static constexpr uptr queue_pfn = 0x040;
    static constexpr uptr queue_ready = 0x044;
    static constexpr uptr queue_notify = 0x050;
    static constexpr uptr interrupt_status = 0x060;
    static constexpr uptr interrupt_ack = 0x064;
    static constexpr uptr status = 0x070;
    static constexpr uptr queue_desc_low = 0x080;
    static constexpr uptr queue_desc_high = 0x084;
    static constexpr uptr queue_driver_low = 0x090;
    static constexpr uptr queue_driver_high = 0x094;
    static constexpr uptr queue_device_low = 0x0a0;
    static constexpr uptr queue_device_high = 0x0a4;
    static constexpr uptr config_select = 0x100;
    static constexpr uptr config_subsel = 0x101;
    static constexpr uptr config_size = 0x102;
    static constexpr uptr config_data = 0x108;
    static constexpr u32 virtio_magic = 0x74726976;
    static constexpr u32 input_device_id = 18;
    static constexpr u8 config_select_event_bits = 0x11;
    static constexpr u32 status_acknowledge = 1u;
    static constexpr u32 status_driver = 2u;
    static constexpr u32 status_driver_ok = 4u;
    static constexpr u32 status_features_ok = 8u;
    static constexpr u32 feature_version_1 = 1u << 0;
    static constexpr u32 legacy_queue_align = 4096;
    static constexpr u16 descriptor_flag_write = 2u;

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

    static constexpr usize legacy_descriptor_offset = 0;
    static constexpr usize legacy_available_offset = sizeof(Descriptor) * queue_size;
    static constexpr usize legacy_used_offset =
        ((legacy_available_offset + sizeof(AvailableRing) + legacy_queue_align - 1) / legacy_queue_align) *
        legacy_queue_align;
    static constexpr usize legacy_queue_size =
        ((legacy_used_offset + sizeof(UsedRing) + legacy_queue_align - 1) / legacy_queue_align) * legacy_queue_align;

    static volatile u32 &mmio32(uptr address)
    {
        return *reinterpret_cast<volatile u32 *>(address);
    }

    static volatile u8 &mmio8(uptr address)
    {
        return *reinterpret_cast<volatile u8 *>(address);
    }

    static u8 read8(uptr address)
    {
        return mmio8(address);
    }

    static u32 read32(uptr address)
    {
        return mmio32(address);
    }

    static void write8(uptr address, u8 value)
    {
        mmio8(address) = value;
    }

    static void write32(uptr address, u32 value)
    {
        mmio32(address) = value;
    }

    static void write64_split(uptr low_register, uptr high_register, uptr address)
    {
        const auto wide_address = static_cast<u64>(address);
        write32(low_register, static_cast<u32>(wide_address & 0xffff'ffffull));
        write32(high_register, static_cast<u32>((wide_address >> 32) & 0xffff'ffffull));
    }

    static bool event_bit_supported(uptr base, u16 event_type, u16 code)
    {
        write8(base + config_subsel, static_cast<u8>(event_type));
        write8(base + config_select, config_select_event_bits);
        asm volatile("" ::: "memory");
        const auto byte_index = static_cast<usize>(code / 8);
        if (byte_index >= read8(base + config_size))
        {
            return false;
        }
        const u8 bits = read8(base + config_data + byte_index);
        return ((bits >> (code % 8)) & 1u) != 0;
    }

    static bool matches_kind(uptr base, VirtioInputKind kind)
    {
        const bool has_keyboard_key = event_bit_supported(base, input_event_key, input_key_a) ||
                                      event_bit_supported(base, input_event_key, input_key_enter);
        const bool has_relative_motion = event_bit_supported(base, input_event_relative, input_relative_x) ||
                                         event_bit_supported(base, input_event_relative, input_relative_y);
        const bool has_mouse_button = event_bit_supported(base, input_event_key, input_button_left);
        if (kind == VirtioInputKind::keyboard)
        {
            return has_keyboard_key && !has_relative_motion;
        }
        return has_relative_motion || has_mouse_button;
    }

    static uptr fallback_base(const uptr *input_bases, usize input_count, VirtioInputKind kind)
    {
        if (input_count == 0)
        {
            return 0;
        }
        if (kind == VirtioInputKind::keyboard)
        {
            return input_bases[input_count - 1];
        }
        return input_count >= 2 ? input_bases[input_count - 2] : input_bases[0];
    }

    bool setup_queue()
    {
        legacy_ = read32(base_ + version) == 1;
        write32(base_ + status, 0);
        write32(base_ + status, status_acknowledge | status_driver);
        write32(base_ + device_features_sel, 0);
        static_cast<void>(read32(base_ + device_features));
        write32(base_ + driver_features_sel, 0);
        write32(base_ + driver_features, 0);
        if (!legacy_)
        {
            write32(base_ + device_features_sel, 1);
            if ((read32(base_ + device_features) & feature_version_1) == 0)
            {
                return false;
            }
            write32(base_ + driver_features_sel, 1);
            write32(base_ + driver_features, feature_version_1);
            write32(base_ + status, read32(base_ + status) | status_features_ok);
            if ((read32(base_ + status) & status_features_ok) == 0)
            {
                return false;
            }
        }

        write32(base_ + queue_sel, 0);
        if (read32(base_ + queue_num_max) < queue_size)
        {
            return false;
        }
        write32(base_ + queue_num, queue_size);
        reset_queue();
        if (legacy_)
        {
            write32(base_ + guest_page_size, legacy_queue_align);
            write32(base_ + queue_align, legacy_queue_align);
            write32(base_ + queue_pfn, static_cast<u32>(reinterpret_cast<uptr>(legacy_queue_) / legacy_queue_align));
        }
        else
        {
            write64_split(base_ + queue_desc_low, base_ + queue_desc_high, reinterpret_cast<uptr>(descriptors_));
            write64_split(base_ + queue_driver_low, base_ + queue_driver_high, reinterpret_cast<uptr>(available_));
            write64_split(base_ + queue_device_low, base_ + queue_device_high, reinterpret_cast<uptr>(used_));
            write32(base_ + queue_ready, 1);
        }

        for (u16 i = 0; i < queue_size; ++i)
        {
            recycle_descriptor(i);
        }
        write32(base_ + status, read32(base_ + status) | status_driver_ok);
        write32(base_ + queue_notify, 0);
        return true;
    }

    void reset_queue()
    {
        if (legacy_)
        {
            for (auto &byte : legacy_queue_)
            {
                byte = 0;
            }
            descriptors_ = reinterpret_cast<Descriptor *>(legacy_queue_ + legacy_descriptor_offset);
            available_ = reinterpret_cast<AvailableRing *>(legacy_queue_ + legacy_available_offset);
            used_ = reinterpret_cast<UsedRing *>(legacy_queue_ + legacy_used_offset);
        }
        else
        {
            descriptors_ = descriptors_storage_;
            available_ = &available_storage_;
            used_ = &used_storage_;
            *available_ = {};
            *used_ = {};
        }
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

    void recycle_descriptor(u16 descriptor)
    {
        available_->ring[available_index_ % queue_size] = descriptor;
        asm volatile("" ::: "memory");
        ++available_index_;
        available_->idx = available_index_;
        asm volatile("" ::: "memory");
        write32(base_ + queue_notify, 0);
        const auto pending = read32(base_ + interrupt_status);
        if (pending != 0)
        {
            write32(base_ + interrupt_ack, pending);
        }
    }

    bool initialized_{false};
    bool ready_{false};
    bool legacy_{false};
    uptr base_{0};
    u16 available_index_{0};
    u16 last_used_index_{0};
    alignas(16) Descriptor descriptors_storage_[queue_size]{};
    alignas(16) AvailableRing available_storage_{};
    alignas(16) UsedRing used_storage_{};
    alignas(4096) u8 legacy_queue_[legacy_queue_size]{};
    Descriptor *descriptors_{descriptors_storage_};
    AvailableRing *available_{&available_storage_};
    UsedRing *used_{&used_storage_};
    alignas(16) VirtioInputEvent events_[queue_size]{};
};

} // namespace ok::driver::qemu_virt
