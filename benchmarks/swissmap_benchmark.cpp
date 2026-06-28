#include <swissmap/fixed_map.hpp>

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

namespace
{
    using aos_map = swiss::swissmap_aos<std::uint64_t, std::uint32_t>;
    using soa_map = swiss::swissmap_soa<std::uint64_t, std::uint32_t>;
    constexpr std::size_t group_width = aos_map::group_width;

    static_assert(sizeof(aos_map::group_type) > sizeof(soa_map::group_type));

    std::uint64_t random_key(std::uint64_t x) noexcept
    {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    std::vector<std::uint64_t> make_random_keys(std::size_t entries, std::uint64_t first_index)
    {
        std::vector<std::uint64_t> keys;
        keys.reserve(entries);

        for (std::size_t i = 0; i < entries; ++i)
        {
            keys.push_back(random_key(first_index + static_cast<std::uint64_t>(i)));
        }

        return keys;
    }

    // chooses a valid hashmap capacity for a requested number of entries
    // the capacity is the smallest valid capacity that can hold the requested number of entries at a load factor of 7/8
    std::size_t capacity_for_entries(std::size_t entries)
    {
        std::size_t groups = 1;
        while (groups * group_width * 7 / 8 < entries)
        {
            groups <<= 1;
        }
        return groups * group_width;
    }

    void set_layout_counters(benchmark::State &state, std::size_t capacity, std::size_t group_size)
    {
        state.counters["capacity"] = static_cast<double>(capacity);
        state.counters["group_bytes"] = static_cast<double>(group_size);
        state.counters["table_bytes"] = static_cast<double>(capacity / group_width * group_size);
    }

    template <class Map>
    void BM_Insert(benchmark::State &state)
    {
        const std::size_t entries = static_cast<std::size_t>(state.range(0));
        const std::size_t capacity = capacity_for_entries(entries);
        const std::vector<std::uint64_t> keys = make_random_keys(entries, 0);

        for (auto _ : state)
        {
            Map map(capacity);

            for (std::size_t i = 0; i < entries; ++i)
            {
                const auto value = static_cast<std::uint32_t>(i);

                if (!map.insert(keys[i], value))
                {
                    state.SkipWithError("insert failed");
                    break;
                }
            }

            benchmark::DoNotOptimize(map.size());
            benchmark::ClobberMemory();
        }

        state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entries));
        set_layout_counters(state, capacity, sizeof(typename Map::group_type));
    }

    template <class Map>
    void BM_FindHit(benchmark::State &state)
    {
        const std::size_t entries = static_cast<std::size_t>(state.range(0));
        const std::size_t capacity = capacity_for_entries(entries);
        const std::vector<std::uint64_t> keys = make_random_keys(entries, 0);

        Map map(capacity);
        for (std::size_t i = 0; i < entries; ++i)
        {
            benchmark::DoNotOptimize(map.insert(keys[i], static_cast<std::uint32_t>(i)));
        }

        for (auto _ : state)
        {
            std::uint64_t checksum = 0;

            for (std::size_t i = 0; i < entries; ++i)
            {
                const std::uint32_t *value = map.find(keys[i]);

                if (value == nullptr)
                {
                    state.SkipWithError("lookup failed");
                    break;
                }

                checksum += *value;
            }

            benchmark::DoNotOptimize(checksum);
        }

        state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entries));
        set_layout_counters(state, capacity, sizeof(typename Map::group_type));
    }

    template <class Map>
    void BM_FindMiss(benchmark::State &state)
    {
        const std::size_t entries = static_cast<std::size_t>(state.range(0));
        const std::size_t capacity = capacity_for_entries(entries);
        const std::vector<std::uint64_t> keys = make_random_keys(entries, 0);
        const std::vector<std::uint64_t> missing_keys = make_random_keys(entries, static_cast<std::uint64_t>(entries));

        Map map(capacity);
        for (std::size_t i = 0; i < entries; ++i)
        {
            benchmark::DoNotOptimize(map.insert(keys[i], static_cast<std::uint32_t>(i)));
        }

        for (auto _ : state)
        {
            std::uint64_t misses = 0;

            for (std::size_t i = 0; i < entries; ++i)
            {
                const std::uint32_t *value = map.find(missing_keys[i]);

                if (value != nullptr)
                {
                    state.SkipWithError("unexpected hit");
                    break;
                }

                misses += value == nullptr;
            }

            benchmark::DoNotOptimize(misses);
        }

        state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entries));
        set_layout_counters(state, capacity, sizeof(typename Map::group_type));
    }

    void benchmark_args(benchmark::internal::Benchmark *benchmark)
    {
        benchmark
            ->Args({1 << 20})
            ->Args({1 << 22})
            ->Args({1 << 23})
            ->Repetitions(10)
            ->ReportAggregatesOnly(true);
    }
}

BENCHMARK_TEMPLATE(BM_Insert, aos_map)->Name("aos/insert")->Apply(benchmark_args);
BENCHMARK_TEMPLATE(BM_Insert, soa_map)->Name("soa/insert")->Apply(benchmark_args);

BENCHMARK_TEMPLATE(BM_FindHit, aos_map)->Name("aos/find_hit")->Apply(benchmark_args);
BENCHMARK_TEMPLATE(BM_FindHit, soa_map)->Name("soa/find_hit")->Apply(benchmark_args);

BENCHMARK_TEMPLATE(BM_FindMiss, aos_map)->Name("aos/find_miss")->Apply(benchmark_args);
BENCHMARK_TEMPLATE(BM_FindMiss, soa_map)->Name("soa/find_miss")->Apply(benchmark_args);

BENCHMARK_MAIN();
