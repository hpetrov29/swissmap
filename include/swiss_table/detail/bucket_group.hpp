#pragma once

#include "control.hpp"
#include "slot_storage.hpp"

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

#if defined(__SSE2__) || \
    (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#define SWISS_TABLE_DETAIL_HAS_SSE2 1
#else
#define SWISS_TABLE_DETAIL_HAS_SSE2 0
#endif

namespace swiss::detail
{
    template <class Key, class Value>
    struct aos_slot
    {
        Key key;
        Value value;

        template <class K, class... Args>
            requires std::constructible_from<Key, K &&> &&
                     std::constructible_from<Value, Args &&...>
        constexpr aos_slot(K &&k, Args &&...args)
            : key(std::forward<K>(k)), value(std::forward<Args>(args)...)
        {
        }
    };

    struct slot_position
    {
        std::size_t group_index;
        std::size_t lane;
    };

    template <class Key, class Value>
    struct aos_bucket_group
    {
        using slot_type = aos_slot<Key, Value>;

        alignas(control_word::width) ctrl_t ctrl_bytes[control_word::width];
        slot_storage<slot_type> slots[control_word::width];

        constexpr aos_bucket_group() noexcept
        {
            reset_control_bytes();
        }

        constexpr void reset_control_bytes() noexcept
        {
#if SWISS_TABLE_DETAIL_HAS_SSE2
            const __m128i empty = _mm_set1_epi8(static_cast<char>(ctrl::empty));
            _mm_store_si128(reinterpret_cast<__m128i *>(ctrl_bytes), empty);
#else
            for (std::size_t i = 0; i < control_word::width; ++i)
            {
                ctrl_bytes[i] = ctrl::empty;
            }
#endif
        }

        Key &key_at(std::size_t lane) noexcept
        {
            return slots[lane].ptr()->key;
        }

        const Key &key_at(std::size_t lane) const noexcept
        {
            return slots[lane].ptr()->key;
        }

        Value &value_at(std::size_t lane) noexcept
        {
            return slots[lane].ptr()->value;
        }

        const Value &value_at(std::size_t lane) const noexcept
        {
            return slots[lane].ptr()->value;
        }

        template <class K, class... Args>
            requires std::constructible_from<Key, K &&> &&
                     std::constructible_from<Value, Args &&...>
        void construct(std::size_t lane, K &&key, Args &&...args)
        {
            slots[lane].construct(std::forward<K>(key), std::forward<Args>(args)...);
        }

        void destroy(std::size_t lane) noexcept(std::is_nothrow_destructible_v<slot_type>)
        {
            slots[lane].destroy();
        }

        void set_control_byte(std::size_t lane, ctrl_t value) noexcept
        {
            ctrl_bytes[lane] = value;
        }

        control_word get_control_word() const noexcept
        {
            return control_word(ctrl_bytes);
        }

        bool is_filled(std::size_t lane) const noexcept
        {
            return ctrl::is_filled(ctrl_bytes[lane]);
        }
    };

    template <class Key, class Value>
    struct soa_bucket_group
    {
        // Split storage keeps the 16 keys and 16 values in separate arrays instead of
        // storing 16 {key, value} pairs. This removes per-slot padding when Key and
        // Value have different sizes/alignments.
        //
        // The group stays naturally friendly to 16-byte alignment: ctrl_bytes has
        // 16 bytes, and each key/value array has 16 elements, so each array contributes
        // 16 * sizeof(T) bytes. Since sizeof(T) is a multiple of alignof(T), the array
        // boundaries stay aligned, and the total group size remains a multiple of 16.

        alignas(control_word::width) ctrl_t ctrl_bytes[control_word::width];
        slot_storage<Key> keys[control_word::width];
        slot_storage<Value> values[control_word::width];

        constexpr soa_bucket_group() noexcept
        {
            reset_control_bytes();
        }

        constexpr void reset_control_bytes() noexcept
        {
#if SWISS_TABLE_DETAIL_HAS_SSE2
            const __m128i empty = _mm_set1_epi8(static_cast<char>(ctrl::empty));
            _mm_store_si128(reinterpret_cast<__m128i *>(ctrl_bytes), empty);
#else
            for (std::size_t i = 0; i < control_word::width; ++i)
            {
                ctrl_bytes[i] = ctrl::empty;
            }
#endif
        }

        Key &key_at(std::size_t lane) noexcept
        {
            return *keys[lane].ptr();
        }

        const Key &key_at(std::size_t lane) const noexcept
        {
            return *keys[lane].ptr();
        }

        Value &value_at(std::size_t lane) noexcept
        {
            return *values[lane].ptr();
        }

        const Value &value_at(std::size_t lane) const noexcept
        {
            return *values[lane].ptr();
        }

        template <class K, class... Args>
            requires std::constructible_from<Key, K &&> &&
                     std::constructible_from<Value, Args &&...>
        void construct(std::size_t lane, K &&key, Args &&...args)
        {
            keys[lane].construct(std::forward<K>(key));
            try
            {
                values[lane].construct(std::forward<Args>(args)...);
            }
            catch (...)
            {
                keys[lane].destroy();
                throw;
            }
        }

        void destroy(std::size_t lane) noexcept(std::is_nothrow_destructible_v<Key> &&
                                                std::is_nothrow_destructible_v<Value>)
        {
            values[lane].destroy();
            keys[lane].destroy();
        }

        void set_control_byte(std::size_t lane, ctrl_t value) noexcept
        {
            ctrl_bytes[lane] = value;
        }

        control_word get_control_word() const noexcept
        {
            return control_word(ctrl_bytes);
        }

        bool is_filled(std::size_t lane) const noexcept
        {
            return ctrl::is_filled(ctrl_bytes[lane]);
        }
    };
}

#undef SWISS_TABLE_DETAIL_HAS_SSE2
