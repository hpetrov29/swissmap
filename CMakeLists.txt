cmake_minimum_required(VERSION 3.20)

project(swissmap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

add_library(swissmap INTERFACE)
add_library(swissmap::swissmap ALIAS swissmap)
target_include_directories(swissmap INTERFACE include)
target_compile_features(swissmap INTERFACE cxx_std_20)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    option(SWISSMAP_BUILD_TESTS "Build tests" ON)
    option(SWISSMAP_BUILD_BENCHMARKS "Build benchmarks" ON)
    option(SWISSMAP_BUILD_EXAMPLES "Build examples" ON)

    if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
else()
    option(SWISSMAP_BUILD_TESTS "Build tests" OFF)
    option(SWISSMAP_BUILD_BENCHMARKS "Build benchmarks" OFF)
    option(SWISSMAP_BUILD_EXAMPLES "Build examples" OFF)
endif()

if(SWISSMAP_BUILD_EXAMPLES)
    add_executable(basic_usage examples/basic_usage.cpp)
    target_link_libraries(basic_usage PRIVATE swissmap::swissmap)
endif()

if(SWISSMAP_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
    )

    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(swissmap_benchmark benchmarks/swissmap_benchmark.cpp)
    target_link_libraries(swissmap_benchmark PRIVATE swissmap::swissmap benchmark::benchmark)
endif()

if(SWISSMAP_BUILD_TESTS)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.11.0
    )

    FetchContent_MakeAvailable(Catch2)

    enable_testing()

    add_executable(swissmap_tests tests/fixed_map_tests.cpp)
    target_link_libraries(swissmap_tests PRIVATE swissmap::swissmap Catch2::Catch2WithMain)

    add_test(NAME swissmap_tests COMMAND swissmap_tests)
endif()
