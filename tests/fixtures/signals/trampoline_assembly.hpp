#pragma once
#include <array>
#include <cstddef>
namespace trampoline_assembly {
inline constexpr std::array<std::byte, 16> rv32{{std::byte{0x93}, std::byte{0x08}, std::byte{0xb0}, std::byte{0x08}, std::byte{0x73}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x73}, std::byte{0x00}, std::byte{0x10}, std::byte{0x00}, std::byte{0x73}, std::byte{0x00}, std::byte{0x10}, std::byte{0x00}}};
inline constexpr std::array<std::byte, 16> rv64{{std::byte{0x93}, std::byte{0x08}, std::byte{0xb0}, std::byte{0x08}, std::byte{0x73}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x73}, std::byte{0x00}, std::byte{0x10}, std::byte{0x00}, std::byte{0x73}, std::byte{0x00}, std::byte{0x10}, std::byte{0x00}}};
inline constexpr std::array<std::byte, 32> mips{{std::byte{0x24}, std::byte{0x02}, std::byte{0x10}, std::byte{0x61}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0c}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0d}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0d}, std::byte{0x24}, std::byte{0x02}, std::byte{0x10}, std::byte{0x17}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0c}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0d}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0d}}};
inline constexpr std::array<std::byte, 16> mips64{{std::byte{0x24}, std::byte{0x02}, std::byte{0x14}, std::byte{0x5b}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0c}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0d}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0d}}};
inline constexpr std::array<std::byte, 16> loongarch64{{std::byte{0x0b}, std::byte{0x2c}, std::byte{0x82}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x2b}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x2a}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x2a}, std::byte{0x00}}};
}
