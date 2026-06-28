#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

#if defined(__SSE2__) || \
    (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#define SWISSMAP_DETAIL_HAS_SSE2 1
#else
#define SWISSMAP_DETAIL_HAS_SSE2 0
#endif

namespace swiss::detail
{
    using ctrl_t = std::int8_t;

    struct ctrl
    {
        // Each group has 16 slots and 16 control bytes.
        // A filled slot's control byte is 0xxxxxxx: high bit 0, low 7 bits store h2.
        // Special states are negative, so one sign-bit test can reject non-filled slots.

        // empty:      10000000  // -128
        static constexpr ctrl_t empty = static_cast<ctrl_t>(-128);
        // deleted:    11111110  // -2
        static constexpr ctrl_t deleted = static_cast<ctrl_t>(-2);

        static constexpr bool is_filled(ctrl_t c) noexcept { return c >= 0; }
        static constexpr bool is_empty_or_deleted(ctrl_t c) noexcept { return c < 0; }
        static constexpr bool is_empty(ctrl_t c) noexcept { return c == empty; }
        static constexpr bool is_deleted(ctrl_t c) noexcept { return c == deleted; }
    };

    struct hash_parts
    {
        std::size_t h1;
        std::uint8_t h2;
    };

    inline constexpr std::uint64_t avalanche_hash(std::uint64_t x) noexcept
    {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    }

    inline constexpr hash_parts split_hash(std::size_t hash) noexcept
    {
        const auto mixed = avalanche_hash(static_cast<std::uint64_t>(hash));
        return {
            static_cast<std::size_t>(mixed >> 7),
            static_cast<std::uint8_t>(mixed & 0x7f),
        };
    }

    struct bit_mask
    {
        std::uint32_t bits = 0;

        constexpr bool any() const noexcept { return bits != 0; }

        constexpr std::size_t lowest_bit_index() const noexcept
        {
            return static_cast<std::size_t>(std::countr_zero(bits));
        }

        constexpr std::size_t pop_lowest() noexcept
        {
            const auto index = lowest_bit_index();
            bits &= bits - 1;
            return index;
        }
    };

    struct control_word
    {
        static constexpr std::size_t width = 16;

#if SWISSMAP_DETAIL_HAS_SSE2
        __m128i _ctrl;
#else
        const ctrl_t *ctrl;
#endif

        explicit control_word(const ctrl_t *p) noexcept
#if SWISSMAP_DETAIL_HAS_SSE2
            : _ctrl(_mm_loadu_si128(reinterpret_cast<const __m128i *>(p)))
#else
            : ctrl(p)
#endif
        {
        }

        bit_mask match_h2(std::uint8_t h2) const noexcept
        {
#if SWISSMAP_DETAIL_HAS_SSE2
            // sets the 16 signed 8-bit integer values to h2
            // copies the same 8-bit pattern into all 16 lanes of the __m128i
            const __m128i _h2 = _mm_set1_epi8(static_cast<char>(h2));
            // compares the 16 8-bit integers in _ctrl and _h2 for equality elementwise
            // each result lane is 0b11111111 if equal, 0b00000000 otherwise
            const __m128i _eq = _mm_cmpeq_epi8(_ctrl, _h2);
            // creates a 16-bit mask from the most significant bits of the 16 8-bit integers in _eq
            // upper bits are zero
            return {static_cast<std::uint32_t>(_mm_movemask_epi8(_eq))};
#else
            std::uint32_t bits = 0;
            for (std::size_t i = 0; i < width; ++i)
            {
                if (ctrl[i] == static_cast<ctrl_t>(h2))
                {
                    bits |= 1u << i;
                }
            }
            return {bits};
#endif
        }

        bit_mask match_empty() const noexcept
        {
#if SWISSMAP_DETAIL_HAS_SSE2
            // sets the 16 signed 8-bit integer values to ctrl::empty (0b10000000)
            // copies the same 8-bit pattern into all 16 lanes of the __m128i
            const auto _empty = _mm_set1_epi8(static_cast<char>(ctrl::empty));
            // compares the 16 8-bit integers in _ctrl and _empty for equality elementwise
            // each result lane is 0b11111111 if equal, 0b00000000 otherwise
            const auto eq = _mm_cmpeq_epi8(_ctrl, _empty);
            // creates a 16-bit mask from the most significant bits of the 16 8-bit integers in _eq
            // upper bits are zero
            return {static_cast<std::uint32_t>(_mm_movemask_epi8(eq))};
#else
            std::uint32_t bits = 0;
            for (std::size_t i = 0; i < width; ++i)
            {
                if (ctrl[i] == ctrl::empty)
                {
                    bits |= 1u << i;
                }
            }
            return {bits};
#endif
        }

        bit_mask match_empty_or_deleted() const noexcept
        {
#if SWISSMAP_DETAIL_HAS_SSE2
            return {static_cast<std::uint32_t>(_mm_movemask_epi8(_ctrl))};
#else
            std::uint32_t bits = 0;
            for (std::size_t i = 0; i < width; ++i)
            {
                if (ctrl::is_empty_or_deleted(ctrl[i]))
                {
                    bits |= 1u << i;
                }
            }
            return {bits};
#endif
        }
    };

    struct probe_seq
    {
        std::size_t mask;
        std::size_t index;
        std::size_t stride = 0;

        constexpr probe_seq(std::size_t h1, std::size_t group_mask) noexcept
            : mask(group_mask), index(h1 & group_mask) {}

        constexpr std::size_t group_index() const noexcept { return index; }

        constexpr void next() noexcept
        {
            ++stride;
            index = (index + stride) & mask;
        }
    };
}

#undef SWISSMAP_DETAIL_HAS_SSE2
