# Find the threading library, as it's required for this example.
find_package(Threads REQUIRED)

# --- Example: Basic Usage ---
add_executable(basic_usage basic_usage.cpp)

# Link the example to our queue library and the threading library.
target_link_libraries(basic_usage PRIVATE spsc_queue Threads::Threads)

# --- Example: Advanced Usage ---
add_executable(advanced_usage advanced_usage.cpp)

# Link the example to our queue library and the threading library.
target_link_libraries(advanced_usage PRIVATE spsc_queue Threads::Threads)

# --- Example for Ranges API ---
add_executable(ranges_example ranges_example.cpp)

target_link_libraries(ranges_example PRIVATE spsc_queue Threads::Threads)

