add_executable(cracking_lsm_tests cracking_lsm_test.cpp)

target_link_libraries(cracking_lsm_tests PRIVATE cracking-lsm::cracking-lsm)

add_test(NAME cracking_lsm_tests COMMAND cracking_lsm_tests)

add_executable(cracking_lsm_gtests
    kv_entry_test.cpp native_codec_test.cpp block_format_test.cpp run_test.cpp memory_accounting_test.cpp
    memtable_test.cpp)
target_link_libraries(cracking_lsm_gtests PRIVATE cracking-lsm::cracking-lsm GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cracking_lsm_gtests)
