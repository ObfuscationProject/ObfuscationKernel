#pragma once

#include "ok/core/types.hpp"

#include <concepts>
#include <span>
#include <string_view>
#include <type_traits>

namespace ok {

template <typename T>
concept ByteLike = std::same_as<std::remove_cv_t<T>, std::byte> ||
                   std::same_as<std::remove_cv_t<T>, u8> ||
                   std::same_as<std::remove_cv_t<T>, char>;

template <typename T>
concept KernelService = requires(T service) {
    { service.name() } -> std::convertible_to<std::string_view>;
    { service.initialize() } -> std::same_as<Status>;
};

template <typename T>
concept TriviallySerializable = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

template <typename T>
concept PageSized = requires(T value) {
    { value.page_size() } -> std::convertible_to<usize>;
};

} // namespace ok

