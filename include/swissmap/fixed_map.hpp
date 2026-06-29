#pragma once

#include "detail/bucket_group.hpp"

#include <bit>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace swiss
{
    enum class bucket_layout
    {
        automatic,
        aos,
        soa,
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
        using slot_type = detail::aos_slot<Key, Value>;
        static constexpr bool slot_has_padding = sizeof(slot_type) > sizeof(Key) + sizeof(Value);
        static constexpr bool use_soa_layout = (Layout == bucket_layout::soa) || (Layout == bucket_layout::automatic && slot_has_padding);

        using group_type = std::conditional_t<use_soa_layout, detail::soa_bucket_group<Key, Value>, detail::aos_bucket_group<Key, Value>>;
        static constexpr std::size_t group_width = detail::control_word::width;

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

        [[nodiscard]] static constexpr bucket_layout effective_layout() noexcept
        {
            return use_soa_layout ? bucket_layout::soa : bucket_layout::aos;
        }

    public:
        Value *find(const Key &key)
        {
            std::optional<detail::slot_position> pos = lookup_slot(key);
            return pos ? &m_groups[pos->group_index].value_at(pos->lane) : nullptr;
        }

        const Value *find(const Key &key) const
        {
            std::optional<detail::slot_position> pos = lookup_slot(key);
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

            const detail::hash_parts parts = hash_key(key);
            detail::probe_seq seq(parts.h1, m_group_mask);
            detail::slot_position insert_pos{};
            bool has_insert_pos = false;

            do
            {
                const std::size_t group_index = seq.group_index();
                group_type &bucket = m_groups[group_index];
                const detail::control_word word = bucket.get_control_word();
                detail::bit_mask matches = word.match_h2(parts.h2);

                while (matches.any()) // 1 in 128 chance we have a duplicate h2, need to check keys
                {
                    const std::size_t lane = matches.pop_lowest();

                    if (m_eq(bucket.key_at(lane), key))
                    { // key already exists, do not insert
                        return false;
                    }
                }

                detail::bit_mask reusable = word.match_empty_or_deleted();
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

                    target.set_control_byte(insert_pos.lane, static_cast<detail::ctrl_t>(parts.h2));
                    ++m_size;
                    return true;
                }

            } while (seq.advance());

            if (!has_insert_pos)
            {
                return false;
            }

            group_type &target = m_groups[insert_pos.group_index];
            target.construct(
                insert_pos.lane,
                std::forward<K>(key),
                std::forward<Args>(args)...);

            target.set_control_byte(insert_pos.lane, static_cast<detail::ctrl_t>(parts.h2));
            ++m_size;
            return true;
        }

        template <class K, class V>
            requires std::constructible_from<Key, K &&> &&
                     std::constructible_from<Value, V &&>
        bool insert(K &&key, V &&value)
        {
            return try_emplace(std::forward<K>(key), std::forward<V>(value));
        }

        bool erase(const Key &key)
        {
            const std::optional<detail::slot_position> pos = lookup_slot(key);

            if (!pos)
            {
                return false;
            }

            erase_slot(*pos);
            return true;
        }

        std::optional<Value> remove(const Key &key)
            requires std::move_constructible<Value>
        {
            const std::optional<detail::slot_position> pos = lookup_slot(key);

            if (!pos)
            {
                return std::nullopt;
            }

            group_type &group = m_groups[pos->group_index];
            std::optional<Value> removed(std::move(group.value_at(pos->lane)));

            erase_slot(*pos);
            return removed;
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

            const detail::hash_parts parts = hash_key(key);
            detail::probe_seq seq(parts.h1, m_group_mask);

            do
            {
                const std::size_t group_index = seq.group_index();
                group_type &bucket = m_groups[group_index];
                const detail::control_word word = bucket.get_control_word();

                detail::bit_mask reusable = word.match_empty_or_deleted();
                if (reusable.any())
                {
                    const std::size_t lane = reusable.pop_lowest();
                    bucket.construct(lane, std::forward<K>(key), std::forward<Args>(args)...);
                    bucket.set_control_byte(lane, static_cast<detail::ctrl_t>(parts.h2));
                    ++m_size;
                    return true;
                }

            } while (seq.advance());

            return false;
        }

        template <class K, class V>
            requires std::constructible_from<Key, K &&> &&
                     std::constructible_from<Value, V &&>
        bool insert_unique_unchecked(K &&key, V &&value)
        {
            return emplace_unique_unchecked(std::forward<K>(key), std::forward<V>(value));
        }

    private:
        constexpr detail::hash_parts hash_key(const Key &key) const
            noexcept(noexcept(std::declval<const Hash &>()(key)))
        {
            return detail::split_hash(m_hash(key));
        }

        std::optional<detail::slot_position> lookup_slot(const Key &key)
        {
            return std::as_const(*this).lookup_slot(key);
        }

        std::optional<detail::slot_position> lookup_slot(const Key &key) const
        {
            const detail::hash_parts parts = hash_key(key);
            detail::probe_seq seq(parts.h1, m_group_mask);

            do
            {
                const std::size_t group_index = seq.group_index();
                const group_type &bucket = m_groups[group_index];
                const detail::control_word word = bucket.get_control_word();

                detail::bit_mask matches = word.match_h2(parts.h2);
                while (matches.any())
                {
                    const std::size_t lane = matches.pop_lowest();

                    if (m_eq(bucket.key_at(lane), key))
                    {
                        return detail::slot_position{group_index, lane};
                    }
                }

                if (word.match_empty().any())
                {
                    return std::nullopt;
                }
            } while (seq.advance());

            return std::nullopt;
        }

        void erase_slot(detail::slot_position pos) noexcept(std::is_nothrow_destructible_v<slot_type>)
        {
            group_type &group = m_groups[pos.group_index];

            // any EMPTY found in the same group as the entry we're erasing
            // belongs to another lane in the same aligned group.
            const bool group_already_has_empty = group.get_control_word().match_empty().any();

            group.destroy(pos.lane);

            group.set_control_byte(pos.lane, group_already_has_empty ? detail::ctrl::empty : detail::ctrl::deleted);

            --m_size;
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
