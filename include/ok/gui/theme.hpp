#pragma once
#include "ok/core/types.hpp"

namespace ok::gui::theme
{
// Shared mechanism-level visual tokens. Applications own layout and content.
inline constexpr u32 canvas = 0xffedf1f7u;
inline constexpr u32 surface = 0xffffffffu;
inline constexpr u32 subtle = 0xfff5f7fbu;
inline constexpr u32 border = 0xffd6deebu;
inline constexpr u32 text = 0xff24324au;
inline constexpr u32 muted = 0xff65748bu;
inline constexpr u32 accent = 0xff326bd8u;
inline constexpr u32 accent_soft = 0xffe7eefdu;
inline constexpr u32 positive = 0xff228271u;
inline constexpr u32 warning = 0xffa56b21u;
inline constexpr u32 danger = 0xffc84b5au;
inline constexpr u32 terminal = 0xff18263au;
inline constexpr u32 terminal_text = 0xffdbe7f7u;
inline constexpr u32 title_height = 18;
} // namespace ok::gui::theme
