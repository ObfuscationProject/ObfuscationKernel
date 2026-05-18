#pragma once

#include "ok/arch/arch.hpp"

#include <concepts>

namespace ok::arch {

template <typename T>
concept ArchitectureProfile = requires {
    { T::architecture } -> std::convertible_to<Architecture>;
    { T::name } -> std::convertible_to<std::string_view>;
    { T::page_size } -> std::convertible_to<usize>;
    { T::register_count } -> std::convertible_to<usize>;
    { T::endianness } -> std::convertible_to<Endianness>;
};

template <>
struct ArchTraits<Architecture::i386> {
    static constexpr Architecture architecture = Architecture::i386;
    static constexpr std::string_view name = "i386";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 8;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 2;
};

template <>
struct ArchTraits<Architecture::x86_64> {
    static constexpr Architecture architecture = Architecture::x86_64;
    static constexpr std::string_view name = "x86_64";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 16;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 4;
};

template <>
struct ArchTraits<Architecture::aarch64> {
    static constexpr Architecture architecture = Architecture::aarch64;
    static constexpr std::string_view name = "aarch64";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 31;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 4;
};

template <>
struct ArchTraits<Architecture::arm32> {
    static constexpr Architecture architecture = Architecture::arm32;
    static constexpr std::string_view name = "arm32";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 16;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 2;
};

template <>
struct ArchTraits<Architecture::rv64> {
    static constexpr Architecture architecture = Architecture::rv64;
    static constexpr std::string_view name = "rv64";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 32;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 4;
};

template <>
struct ArchTraits<Architecture::rv32> {
    static constexpr Architecture architecture = Architecture::rv32;
    static constexpr std::string_view name = "rv32";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 32;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 2;
};

template <>
struct ArchTraits<Architecture::loongarch64> {
    static constexpr Architecture architecture = Architecture::loongarch64;
    static constexpr std::string_view name = "loongarch64";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 32;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 4;
};

static_assert(ArchitectureProfile<ArchTraits<Architecture::i386>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::x86_64>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::aarch64>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::arm32>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::rv64>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::rv32>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::loongarch64>>);

} // namespace ok::arch
