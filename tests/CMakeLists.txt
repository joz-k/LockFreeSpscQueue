# Find the native threading library for the current platform (e.g., pthreads)
# This command is REQUIRED because our tests use std::jthread.
find_package(Threads REQUIRED)

# --- Test: Single Item Transfer ---
add_executable(test_single_item test_single_item.cpp)
target_link_libraries(test_single_item PRIVATE spsc_queue Threads::Threads)
add_test(NAME SingleItemTransfer COMMAND test_single_item)

# --- Test: Batch Transfer ---
add_executable(test_batch_transfer test_batch_transfer.cpp)
target_link_libraries(test_batch_transfer PRIVATE spsc_queue Threads::Threads)
add_test(NAME BatchTransfer COMMAND test_batch_transfer)

# --- Test: Convenience API ---
add_executable(test_convenience_api test_convenience_api.cpp)
target_link_libraries(test_convenience_api PRIVATE spsc_queue Threads::Threads)
add_test(NAME ConvenienceApi COMMAND test_convenience_api)

# --- Test: Stress Test Consumer - Producer ---
add_executable(stress_test stress_test.cpp)
target_link_libraries(stress_test PRIVATE spsc_queue Threads::Threads)
add_test(NAME StressTest COMMAND stress_test)

# --- Test for Move Semantics ---
add_executable(test_move_semantics test_move_semantics.cpp)
target_link_libraries(test_move_semantics PRIVATE spsc_queue Threads::Threads)
add_test(NAME MoveSemantics COMMAND test_move_semantics)

# Add a label to the MoveSemantics test so it can be disabled
# This annotates the test, allowing us to specifically exclude it in certain
# environments (like GitHub worker)
set_tests_properties(MoveSemantics PROPERTIES LABELS "ci_flaky")

