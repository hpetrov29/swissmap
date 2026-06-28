#include <swissmap/fixed_map.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>

TEST_CASE("a new map is empty")
{
    swiss::swissmap<int, int> map(16);

    CHECK(map.empty());
    CHECK(map.size() == 0);
    CHECK(map.capacity() == 16);
    CHECK(map.max_load() == 14);
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
