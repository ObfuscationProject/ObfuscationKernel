#pragma once

#include "ok/core/types.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ok
{

template <usize Capacity> class FixedString final
{
  public:
    constexpr FixedString() = default;
    constexpr explicit FixedString(std::string_view value)
    {
        assign(value);
    }

    constexpr Status assign(std::string_view value)
    {
        if (value.size() >= Capacity)
        {
            return Status::overflow("fixed string capacity exceeded");
        }
        size_ = value.size();
        for (usize i = 0; i < size_; ++i)
        {
            data_[i] = value[i];
        }
        data_[size_] = '\0';
        return Status::success();
    }

    constexpr void clear()
    {
        size_ = 0;
        data_[0] = '\0';
    }

    constexpr Status append(char value)
    {
        if (size_ + 1 >= Capacity)
        {
            return Status::overflow("fixed string capacity exceeded");
        }
        data_[size_++] = value;
        data_[size_] = '\0';
        return Status::success();
    }

    constexpr Status append(std::string_view value)
    {
        if (size_ + value.size() >= Capacity)
        {
            return Status::overflow("fixed string capacity exceeded");
        }
        for (usize i = 0; i < value.size(); ++i)
        {
            data_[size_++] = value[i];
        }
        data_[size_] = '\0';
        return Status::success();
    }

    constexpr void pop_back()
    {
        if (size_ == 0)
        {
            return;
        }
        --size_;
        data_[size_] = '\0';
    }

    [[nodiscard]] constexpr std::string_view view() const
    {
        return {data_.data(), size_};
    }
    [[nodiscard]] constexpr const char *c_str() const
    {
        return data_.data();
    }
    [[nodiscard]] constexpr usize size() const
    {
        return size_;
    }
    [[nodiscard]] constexpr bool empty() const
    {
        return size_ == 0;
    }

  private:
    std::array<char, Capacity> data_{};
    usize size_{0};
};

template <typename T, usize Capacity> class StaticVector final
{
  public:
    constexpr StaticVector() = default;

    [[nodiscard]] constexpr usize size() const
    {
        return size_;
    }
    [[nodiscard]] constexpr usize capacity() const
    {
        return Capacity;
    }
    [[nodiscard]] constexpr bool empty() const
    {
        return size_ == 0;
    }
    [[nodiscard]] constexpr bool full() const
    {
        return size_ == Capacity;
    }
    constexpr void clear()
    {
        size_ = 0;
    }
    // Reserves an existing storage slot; the caller must fully initialize it.
    constexpr Result<T *> push_back_slot()
    {
        if (full())
        {
            return Status::overflow("static vector capacity exceeded");
        }
        return &values_[size_++];
    }

    constexpr Status push_back(const T &value)
    {
        if (full())
        {
            return Status::overflow("static vector capacity exceeded");
        }
        values_[size_++] = value;
        return Status::success();
    }

    constexpr Status erase_at(usize index)
    {
        if (index >= size_)
        {
            return Status::invalid_argument("static vector erase index out of range");
        }
        for (usize i = index; i + 1 < size_; ++i)
        {
            values_[i] = values_[i + 1];
        }
        --size_;
        return Status::success();
    }

    [[nodiscard]] constexpr T &operator[](usize index)
    {
        return values_[index];
    }
    [[nodiscard]] constexpr const T &operator[](usize index) const
    {
        return values_[index];
    }

    [[nodiscard]] constexpr T *begin()
    {
        return values_.data();
    }
    [[nodiscard]] constexpr T *end()
    {
        return values_.data() + size_;
    }
    [[nodiscard]] constexpr const T *begin() const
    {
        return values_.data();
    }
    [[nodiscard]] constexpr const T *end() const
    {
        return values_.data() + size_;
    }

  private:
    std::array<T, Capacity> values_{};
    usize size_{0};
};

template <typename T, usize Capacity> class StaticQueue final
{
  public:
    constexpr StaticQueue() = default;

    [[nodiscard]] constexpr usize size() const
    {
        return size_;
    }
    [[nodiscard]] constexpr bool empty() const
    {
        return size_ == 0;
    }
    [[nodiscard]] constexpr bool full() const
    {
        return size_ == Capacity;
    }

    constexpr Status push(const T &value)
    {
        if (full())
        {
            return Status::would_block("static queue full");
        }
        values_[tail_] = value;
        tail_ = (tail_ + 1) % Capacity;
        ++size_;
        return Status::success();
    }

    constexpr Result<T> pop()
    {
        if (empty())
        {
            return Status::would_block("static queue empty");
        }
        auto value = values_[head_];
        head_ = (head_ + 1) % Capacity;
        --size_;
        return value;
    }

  private:
    std::array<T, Capacity> values_{};
    usize head_{0};
    usize tail_{0};
    usize size_{0};
};

template <typename T, usize N> constexpr void fill_span(std::span<T, N> span, T value)
{
    for (auto &item : span)
    {
        item = value;
    }
}

template <typename T> constexpr void copy_span(std::span<T> destination, std::span<const T> source)
{
    const auto count = destination.size() < source.size() ? destination.size() : source.size();
    for (usize i = 0; i < count; ++i)
    {
        destination[i] = source[i];
    }
}

} // namespace ok
