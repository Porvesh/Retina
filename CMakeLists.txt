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
