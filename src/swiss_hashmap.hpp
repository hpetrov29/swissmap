#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <optional>

#if defined(__SSE2__) || \
    (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#define SWISS_TABLE_HAS_SSE2 1
#else
#define SWISS_TABLE_HAS_SSE2 0
#endif

namespace swiss
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

#if SWISS_TABLE_HAS_SSE2
        __m128i _ctrl;
#else
        const ctrl_t *ctrl;
#endif

        explicit control_word(const ctrl_t *p) noexcept
#if SWISS_TABLE_HAS_SSE2
            : _ctrl(_mm_loadu_si128(reinterpret_cast<const __m128i *>(p))) // laods 16 control bytes in the 128-bit SIMD register _ctrl
#else
            : ctrl(p)
#endif
        {
        }

        bit_mask match_h2(std::uint8_t h2) const noexcept
        {
#if SWISS_TABLE_HAS_SSE2
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
#if SWISS_TABLE_HAS_SSE2
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
#if SWISS_TABLE_HAS_SSE2
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

    template <class Key, class Value>
    struct map_slot
    {
        using KeyType = Key;
        using ValueType = Value;
        Key key;
        Value value;

        template <class K, class... Args>
            requires std::constructible_from<Key, K &&> &&
                         std::constructible_from<Value, Args &&...>
        constexpr map_slot(K &&k, Args &&...args)
            : key(std::forward<K>(k)), value(std::forward<Args>(args)...)
        {
        }
    };

    struct slot_position
    {
        std::size_t group_index;
        std::size_t lane;
    };

    enum class bucket_layout
    {
        automatic,
        aos,
        soa,
    };

    template <class Slot>
    struct aos_bucket_group
    {

        using Key = typename Slot::KeyType;
        using Value = typename Slot::ValueType;

        alignas(control_word::width) ctrl_t ctrl_bytes[control_word::width];
        slot_storage<Slot> slots[control_word::width];

        constexpr aos_bucket_group() noexcept
        {
            reset_control_bytes();
        }

        constexpr void reset_control_bytes() noexcept
        {
#if SWISS_TABLE_HAS_SSE2
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

        void destroy(std::size_t lane) noexcept(std::is_nothrow_destructible_v<Slot>)
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

    template <class Slot>
    struct soa_bucket_group
    {
        using Key = typename Slot::KeyType;
        using Value = typename Slot::ValueType;

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
#if SWISS_TABLE_HAS_SSE2
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

    template <
        class Key,
        class Value,
        class Hash = std::hash<Key>,
        class Eq = std::equal_to<Key>,
        bucket_layout Layout = bucket_layout::automatic>
    class swissmap
    {
    public:
        using slot_type = map_slot<Key, Value>;
        static constexpr bool slot_has_padding = sizeof(slot_type) > sizeof(Key) + sizeof(Value);
        static constexpr bool use_soa_layout =
            Layout == bucket_layout::soa ||
            (Layout == bucket_layout::automatic && slot_has_padding);
        using group_type = std::conditional_t<use_soa_layout, soa_bucket_group<slot_type>, aos_bucket_group<slot_type>>;
        static constexpr std::size_t group_width = control_word::width;

        explicit swissmap(std::size_t capacity)
        {
            if (!is_valid_capacity(capacity))
            {
                throw std::invalid_argument("swissmap capacity must be at least 16, a multiple of 16, and have a power-of-two group count");
            }

            m_capacity = capacity;
            m_group_count = capacity / group_width;
            m_group_mask = m_group_count - 1;
            m_max_load = capacity * 7 / 8;
            m_groups = std::make_unique<group_type[]>(m_group_count);
        }
        swissmap(const swissmap &) = delete;
        swissmap &operator=(const swissmap &) = delete;
        ~swissmap()
        {
            destroy_filled_slots();
        }

    public:
        [[nodiscard]] std::size_t size() const noexcept { return m_size; }
        [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
        [[nodiscard]] std::size_t max_load() const noexcept { return m_max_load; }

        [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
        [[nodiscard]] bool full() const noexcept { return m_size >= m_max_load; }

        [[nodiscard]] double load_factor() const noexcept
        {
            return static_cast<double>(m_size) / static_cast<double>(m_capacity);
        }

    public:
        Value *find(const Key &key)
        {
            std::optional<slot_position> pos = lookup_slot(key);
            return pos ? &m_groups[pos->group_index].value_at(pos->lane) : nullptr;
        }

        const Value *find(const Key &key) const
        {
            std::optional<slot_position> pos = lookup_slot(key);
            return pos ? &m_groups[pos->group_index].value_at(pos->lane) : nullptr;
        }

        [[nodiscard]] bool contains(const Key &key) const
        {
            return lookup_slot(key).has_value();
        }

    public:
        template <class K, class... Args>
            requires std::constructible_from<Key, K &&> && std::constructible_from<Value, Args &&...>
        bool try_emplace(K &&key, Args &&...args)
        {
            if (full())
            {
                return false;
            }

            const hash_parts parts = hash_key(key);
            probe_seq seq(parts.h1, m_group_mask);
            slot_position insert_pos{};
            bool has_insert_pos = false;

            while (true)
            {
                const std::size_t group_index = seq.group_index();
                group_type &bucket = m_groups[group_index];
                const control_word word = bucket.get_control_word();
                bit_mask matches = word.match_h2(parts.h2);

                while (matches.any()) // 1 in 128 chance we have a duplicate h2, need to check keys
                {
                    const std::size_t lane = matches.pop_lowest();

                    if (m_eq(bucket.key_at(lane), key))
                    { // key already exists, do not insert
                        return false;
                    }
                }

                bit_mask reusable = word.match_empty_or_deleted();
                if (reusable.any() && !has_insert_pos)
                {
                    insert_pos.group_index = group_index;
                    insert_pos.lane = reusable.pop_lowest();
                    has_insert_pos = true;
                }

                if (word.match_empty().any())
                {
                    group_type &target = m_groups[insert_pos.group_index];

                    target.construct(
                        insert_pos.lane,
                        std::forward<K>(key),
                        std::forward<Args>(args)...);

                    target.set_control_byte(insert_pos.lane, static_cast<ctrl_t>(parts.h2));
                    ++m_size;
                    return true;
                }

                seq.next();
            }
        }

        template <class K, class V>
            requires std::constructible_from<Key, K &&> &&
                     std::constructible_from<Value, V &&>
        bool insert(K &&key, V &&value)
        {
            return try_emplace(std::forward<K>(key), std::forward<V>(value));
        }

    public:
        template <class K, class... Args>
            requires std::constructible_from<Key, K &&> &&
                     std::constructible_from<Value, Args &&...>
        bool emplace_unique_unchecked(K &&key, Args &&...args)
        {
            if (full())
            {
                return false;
            }

            const hash_parts parts = hash_key(key);
            probe_seq seq(parts.h1, m_group_mask);

            while (true)
            {
                const std::size_t group_index = seq.group_index();
                group_type &bucket = m_groups[group_index];
                const control_word word = bucket.get_control_word();

                bit_mask reusable = word.match_empty_or_deleted();
                if (reusable.any())
                {
                    const std::size_t lane = reusable.pop_lowest();
                    bucket.construct(lane, std::forward<K>(key), std::forward<Args>(args)...);
                    bucket.set_control_byte(lane, static_cast<ctrl_t>(parts.h2));
                    ++m_size;
                    return true;
                }

                seq.next();
            }
        }

        template <class K, class V>
            requires std::constructible_from<Key, K &&> &&
                     std::constructible_from<Value, V &&>
        bool insert_unique_unchecked(K &&key, V &&value)
        {
            return emplace_unique_unchecked(std::forward<K>(key), std::forward<V>(value));
        }

    private:
        constexpr hash_parts hash_key(const Key &key) const
            noexcept(noexcept(std::declval<const Hash &>()(key)))
        {
            return split_hash(m_hash(key));
        }

        std::optional<slot_position> lookup_slot(const Key &key)
        {
            return std::as_const(*this).lookup_slot(key);
        }

        std::optional<slot_position> lookup_slot(const Key &key) const
        {
            const hash_parts parts = hash_key(key);
            probe_seq seq(parts.h1, m_group_mask);

            while (true)
            {
                const std::size_t group_index = seq.group_index();
                const group_type &bucket = m_groups[group_index];
                const control_word word = bucket.get_control_word();

                bit_mask matches = word.match_h2(parts.h2);
                while (matches.any())
                {
                    const std::size_t lane = matches.pop_lowest();

                    if (m_eq(bucket.key_at(lane), key))
                    {
                        return slot_position{group_index, lane};
                    }
                }

                if (word.match_empty().any())
                {
                    return std::nullopt;
                }

                seq.next();
            }
        }

        void reset_control_bytes() noexcept
        {
            for (std::size_t i = 0; i < m_group_count; ++i)
            {
                m_groups[i].reset_control_bytes();
            }
        }

        void destroy_filled_slots() noexcept(std::is_nothrow_destructible_v<slot_type>)
        {
            if (m_groups == nullptr)
            {
                return;
            }

            for (std::size_t group_index = 0; group_index < m_group_count; ++group_index)
            {
                group_type &bucket = m_groups[group_index];
                for (std::size_t lane = 0; lane < group_width; ++lane)
                {
                    if (bucket.is_filled(lane))
                    {
                        bucket.destroy(lane);
                    }
                }
            }
        }

        static constexpr bool is_valid_capacity(std::size_t capacity) noexcept
        {
            if (capacity < group_width || capacity % group_width != 0)
            {
                return false;
            }

            return std::has_single_bit(capacity / group_width);
        }

    private:
        std::size_t m_size = 0;
        std::size_t m_capacity = 0;
        std::size_t m_group_count = 0;
        std::size_t m_group_mask = 0;
        std::size_t m_max_load = 0;
        [[no_unique_address]] Hash m_hash{};
        [[no_unique_address]] Eq m_eq{};
        std::unique_ptr<group_type[]> m_groups;
    };

    template <
        class Key,
        class Value,
        class Hash = std::hash<Key>,
        class Eq = std::equal_to<Key>>
    using swissmap_aos = swissmap<Key, Value, Hash, Eq, bucket_layout::aos>;

    template <
        class Key,
        class Value,
        class Hash = std::hash<Key>,
        class Eq = std::equal_to<Key>>
    using swissmap_soa = swissmap<Key, Value, Hash, Eq, bucket_layout::soa>;

}

#undef SWISS_TABLE_HAS_SSE2
