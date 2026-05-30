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

add_executable(m3_link apps/m3_link.cpp)
target_link_libraries(m3_link PRIVATE retina_core)

add_executable(m4_compositor apps/m4_compositor.cpp)
target_link_libraries(m4_compositor PRIVATE retina_core)

add_executable(m5_rt_video apps/m5_rt_video.cpp)
target_link_libraries(m5_rt_video PRIVATE retina_core Threads::Threads)

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

add_executable(test_encoder tests/test_encoder.cpp)
target_link_libraries(test_encoder PRIVATE retina_core)
add_test(NAME encoder COMMAND test_encoder)

add_executable(test_packet tests/test_packet.cpp)
target_link_libraries(test_packet PRIVATE retina_core)
add_test(NAME packet COMMAND test_packet)

add_executable(test_channel tests/test_channel.cpp)
target_link_libraries(test_channel PRIVATE retina_core)
add_test(NAME channel COMMAND test_channel)

add_executable(test_decoder tests/test_decoder.cpp)
target_link_libraries(test_decoder PRIVATE retina_core)
add_test(NAME decoder COMMAND test_decoder)

add_executable(test_jitter_buffer tests/test_jitter_buffer.cpp)
target_link_libraries(test_jitter_buffer PRIVATE retina_core)
add_test(NAME jitter_buffer COMMAND test_jitter_buffer)

add_executable(test_fec tests/test_fec.cpp)
target_link_libraries(test_fec PRIVATE retina_core)
add_test(NAME fec COMMAND test_fec)

add_executable(test_feedback tests/test_feedback.cpp)
target_link_libraries(test_feedback PRIVATE retina_core)
add_test(NAME feedback COMMAND test_feedback)

add_executable(test_rt tests/test_rt.cpp)
target_link_libraries(test_rt PRIVATE retina_core Threads::Threads)
add_test(NAME rt COMMAND test_rt)

add_executable(test_canvas tests/test_canvas.cpp)
target_link_libraries(test_canvas PRIVATE retina_core)
add_test(NAME canvas COMMAND test_canvas)

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
