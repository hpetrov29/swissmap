#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace swiss::detail
{
    template <class T>
    struct slot_storage
    {
        alignas(T) std::byte storage[sizeof(T)];

        constexpr void *raw_ptr() noexcept
        {
            return storage;
        }

        constexpr const void *raw_ptr() const noexcept
        {
            return storage;
        }

        constexpr T *ptr() noexcept
        {
            return std::launder(reinterpret_cast<T *>(storage));
        }

        constexpr const T *ptr() const noexcept
        {
            return std::launder(reinterpret_cast<const T *>(storage));
        }

        template <class... Args>
        constexpr T &construct(Args &&...args)
        {
            return *std::construct_at(reinterpret_cast<T *>(raw_ptr()), std::forward<Args>(args)...);
        }

        constexpr void destroy() noexcept(std::is_nothrow_destructible_v<T>)
        {
            std::destroy_at(ptr());
        }
    };
}
