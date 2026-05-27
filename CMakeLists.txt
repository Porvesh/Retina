cmake_minimum_required(VERSION 3.16)
project(retina CXX)

# C++20 everywhere, no compiler-specific extensions, warnings on.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra)

# The core is header-only for now (Frame, and soon FrameBuffer/LatestValue).
# An INTERFACE library carries no compiled code — it just hands its include
# path to anything that links it. Every executable we add later links this.
add_library(retina_core INTERFACE)
target_include_directories(retina_core INTERFACE ${CMAKE_SOURCE_DIR}/include)

# Targets (apps, tests) get added here as we build them.

# --- apps (runnable demos) ---
find_package(Threads REQUIRED)

add_executable(m0_spine apps/m0_spine.cpp)
target_link_libraries(m0_spine PRIVATE retina_core Threads::Threads)

# --- tests ---
enable_testing()

# ThreadSanitizer for the concurrency tests. Off by default (it slows things and
# needs its own build dir); flip on with -DRETINA_TSAN=ON.
option(RETINA_TSAN "Build tests with ThreadSanitizer" OFF)

add_executable(test_latest_value tests/test_latest_value.cpp)
target_link_libraries(test_latest_value PRIVATE retina_core)
add_test(NAME latest_value COMMAND test_latest_value)

add_executable(test_latest_value_mt tests/test_latest_value_mt.cpp)
target_link_libraries(test_latest_value_mt PRIVATE retina_core)
find_package(Threads REQUIRED)
target_link_libraries(test_latest_value_mt PRIVATE Threads::Threads)
add_test(NAME latest_value_mt COMMAND test_latest_value_mt)

add_executable(test_latest_value_alloc tests/test_latest_value_alloc.cpp)
target_link_libraries(test_latest_value_alloc PRIVATE retina_core)
add_test(NAME latest_value_alloc COMMAND test_latest_value_alloc)

add_executable(test_spsc_ring tests/test_spsc_ring.cpp)
target_link_libraries(test_spsc_ring PRIVATE retina_core)
add_test(NAME spsc_ring COMMAND test_spsc_ring)

add_executable(test_spsc_ring_mt tests/test_spsc_ring_mt.cpp)
target_link_libraries(test_spsc_ring_mt PRIVATE retina_core Threads::Threads)
add_test(NAME spsc_ring_mt COMMAND test_spsc_ring_mt)

add_executable(test_double_triple tests/test_double_triple.cpp)
target_link_libraries(test_double_triple PRIVATE retina_core)
add_test(NAME double_triple COMMAND test_double_triple)

add_executable(test_camsim tests/test_camsim.cpp)
target_link_libraries(test_camsim PRIVATE retina_core)
add_test(NAME camsim COMMAND test_camsim)

add_executable(test_stream_aligner tests/test_stream_aligner.cpp)
target_link_libraries(test_stream_aligner PRIVATE retina_core)
add_test(NAME stream_aligner COMMAND test_stream_aligner)

add_executable(test_double_triple_mt tests/test_double_triple_mt.cpp)
target_link_libraries(test_double_triple_mt PRIVATE retina_core Threads::Threads)
add_test(NAME double_triple_mt COMMAND test_double_triple_mt)

if(RETINA_TSAN)
    target_compile_options(test_spsc_ring_mt      PRIVATE -fsanitize=thread -g -O1)
    target_link_options(test_spsc_ring_mt         PRIVATE -fsanitize=thread)
    target_compile_options(test_double_triple_mt  PRIVATE -fsanitize=thread -g -O1)
    target_link_options(test_double_triple_mt     PRIVATE -fsanitize=thread)
endif()

if(RETINA_TSAN)
    target_compile_options(test_latest_value_mt PRIVATE -fsanitize=thread -g -O1)
    target_link_options(test_latest_value_mt PRIVATE -fsanitize=thread)
endif()
