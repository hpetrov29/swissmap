#include <swissmap/fixed_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace
{
    constexpr std::size_t hash_for_group(std::size_t wanted_group)
    {
        std::size_t candidate = 0;
        while ((swiss::detail::split_hash(candidate).h1 & 1U) != wanted_group)
        {
            ++candidate;
        }
        return candidate;
    }

    struct two_group_hash
    {
        std::size_t operator()(int key) const noexcept
        {
            constexpr std::size_t group_zero_hash = hash_for_group(0);
            constexpr std::size_t group_one_hash = hash_for_group(1);
            return key < 1000 ? group_zero_hash : group_one_hash;
        }
    };

    template <class Map>
    void check_basic_erase()
    {
        Map map(16);

        REQUIRE(map.try_emplace(1001, "first order"));
        REQUIRE(map.try_emplace(1002, "second order"));

        CHECK(map.erase(1001));
        CHECK(map.size() == 1);
        CHECK_FALSE(map.contains(1001));
        CHECK(map.contains(1002));
        CHECK_FALSE(map.erase(1001));
    }
}

TEST_CASE("a new map is empty")
{
    swiss::swissmap<int, int> map(16);

    CHECK(map.empty());
    CHECK(map.size() == 0);
    CHECK(map.capacity() == 16);
    CHECK(map.max_load() == 14);
}

TEST_CASE("a probe sequence visits every group exactly once")
{
    for (std::size_t group_count = 1; group_count <= 16; group_count *= 2)
    {
        for (std::size_t start = 0; start < group_count; ++start)
        {
            swiss::detail::probe_seq seq(start, group_count - 1);
            unsigned int visited = 0;
            std::size_t visited_count = 0;

            do
            {
                const unsigned int group_bit = 1U << seq.group_index();
                CHECK((visited & group_bit) == 0);
                visited |= group_bit;
                ++visited_count;
            } while (seq.advance());

            CHECK(visited_count == group_count);
        }
    }
}

TEST_CASE("values can be inserted and found")
{
    swiss::swissmap<int, std::string> map(16);

    REQUIRE(map.insert(1001, std::string{"first order"}));
    REQUIRE(map.try_emplace(1002, "second order"));

    REQUIRE(map.find(1001) != nullptr);
    CHECK(*map.find(1001) == "first order");
    CHECK(map.contains(1002));
    CHECK(map.find(9999) == nullptr);
}

TEST_CASE("inserting a duplicate key keeps the original value")
{
    swiss::swissmap<int, int> map(16);

    REQUIRE(map.insert(42, 7));
    CHECK_FALSE(map.insert(42, 99));

    REQUIRE(map.find(42) != nullptr);
    CHECK(*map.find(42) == 7);
    CHECK(map.size() == 1);
}

TEST_CASE("entries can be erased from both layouts")
{
    SECTION("AoS")
    {
        check_basic_erase<swiss::swissmap_aos<int, std::string>>();
    }

    SECTION("SoA")
    {
        check_basic_erase<swiss::swissmap_soa<int, std::string>>();
    }
}

TEST_CASE("a tombstone preserves lookup of a displaced key")
{
    swiss::swissmap<int, int, two_group_hash> map(32);

    for (int key = 0; key < 17; ++key)
    {
        REQUIRE(map.insert(key, key * 10));
    }

    REQUIRE(map.erase(0));

    REQUIRE(map.find(16) != nullptr);
    CHECK(*map.find(16) == 160);

    REQUIRE(map.insert(17, 170));
    CHECK(map.contains(17));
}

TEST_CASE("probing terminates and reuses tombstones when no empty lanes remain")
{
    swiss::swissmap<int, int, two_group_hash> map(32);

    for (int key = 0; key < 16; ++key)
    {
        REQUIRE(map.insert(key, key));
    }
    for (int key = 1000; key < 1012; ++key)
    {
        REQUIRE(map.insert(key, key));
    }

    for (int key = 0; key < 4; ++key)
    {
        REQUIRE(map.erase(key));
    }
    for (int key = 1012; key < 1016; ++key)
    {
        REQUIRE(map.insert(key, key));
    }

    // The table now has 28 live entries, four tombstones, and no EMPTY lanes.
    REQUIRE(map.erase(4));
    REQUIRE(map.insert_unique_unchecked(2000, 2000));

    REQUIRE(map.erase(1000));
    CHECK(map.find(999) == nullptr);
    REQUIRE(map.insert(20, 20));
    CHECK(map.contains(20));
}

TEST_CASE("insertion stops at the maximum load")
{
    swiss::swissmap<int, int> map(16);

    for (std::size_t i = 0; i < map.max_load(); ++i)
    {
        REQUIRE(map.insert(static_cast<int>(i), static_cast<int>(i * 10)));
    }

    CHECK(map.full());
    CHECK_FALSE(map.insert(100, 1000));
}

TEST_CASE("capacity must contain a power-of-two number of groups")
{
    CHECK_THROWS_AS((swiss::swissmap<int, int>{15}), std::invalid_argument);
    CHECK_THROWS_AS((swiss::swissmap<int, int>{48}), std::invalid_argument);
    CHECK_NOTHROW((swiss::swissmap<int, int>{32}));
}

TEST_CASE("explicit layouts report the selected layout")
{
    using aos_map = swiss::swissmap_aos<int, int>;
    using soa_map = swiss::swissmap_soa<int, int>;

    STATIC_CHECK(aos_map::effective_layout() == swiss::bucket_layout::aos);
    STATIC_CHECK(soa_map::effective_layout() == swiss::bucket_layout::soa);
}
