add_executable(cracking_lsm_tests cracking_lsm_test.cpp)

target_link_libraries(cracking_lsm_tests PRIVATE cracking-lsm::cracking-lsm)

add_test(NAME cracking_lsm_tests COMMAND cracking_lsm_tests)

add_executable(cracking_lsm_gtests
    kv_entry_test.cpp native_codec_test.cpp block_format_test.cpp run_test.cpp)
target_link_libraries(cracking_lsm_gtests PRIVATE cracking-lsm::cracking-lsm GTest::gtest_main)

find_package(Threads REQUIRED)
add_executable(cracking_lsm_memtable_tests
    memtable_test.cpp entry_comparator_test.cpp successor_result_test.cpp)
target_link_libraries(cracking_lsm_memtable_tests
    PRIVATE cracking-lsm::cracking-lsm GTest::gtest_main Threads::Threads)

include(GoogleTest)
gtest_discover_tests(cracking_lsm_gtests)
gtest_discover_tests(cracking_lsm_memtable_tests PROPERTIES TIMEOUT 60)
