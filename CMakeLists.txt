# --- Project Definition ---
cmake_minimum_required(VERSION 3.22)
project(
    LockFreeSpscQueue
    VERSION 1.0
    DESCRIPTION "A high-performance C++23 lock-free SPSC queue"
    LANGUAGES CXX
)

# --- C++ Standard and Properties ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Library Definition (Header-Only) ---
# Create an INTERFACE library for our header-only code.
# This makes it easy for other CMake projects to consume.
add_library(spsc_queue INTERFACE)
target_include_directories(spsc_queue INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include> # For when/if an install rule is added
)

# --- Optional Project Components ---
# These options allow users of this library (e.g., via add_subdirectory)
# to control whether they want to build our tests and examples.
option(SPSC_QUEUE_BUILD_TESTS "Build the project's tests" ON)
option(SPSC_QUEUE_BUILD_EXAMPLES "Build the project's examples" ON)
option(SPSC_QUEUE_BUILD_BENCHMARKS "Build the project's benchmarks" OFF)

# For MacOS
if(APPLE AND (SPSC_QUEUE_BUILD_TESTS OR SPSC_QUEUE_BUILD_EXAMPLES OR SPSC_QUEUE_BUILD_BENCHMARKS))
    # Enable experimental Clang stdlib features (for std::jthreads) when
    # building tests, examples or benchmark
    # As of Apple Clang version 17.0.0, this is still required
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>)
endif ()

if(SPSC_QUEUE_BUILD_TESTS)
    # Enable CTest if we're building any tests.
    enable_testing()
    add_subdirectory(tests)
endif()

if(SPSC_QUEUE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SPSC_QUEUE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()


# --- Installation ---
# This rule tells CMake to copy the 'include' directory to the destination
# specified by CMAKE_INSTALL_PREFIX when the user installs the project.
install(DIRECTORY include/ DESTINATION include)

# This rule makes the library's target available to other CMake projects
# that might use find_package() on an installed version of this library.
install(TARGETS spsc_queue EXPORT spsc_queue-targets)

install(EXPORT spsc_queue-targets
    FILE        spsc_queue-config.cmake
    NAMESPACE   spsc::
    DESTINATION lib/cmake/spsc_queue
)

