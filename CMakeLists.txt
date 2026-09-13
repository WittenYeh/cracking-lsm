cmake_minimum_required(VERSION 3.20)

project(cracking_lsm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(third-party/abseil-cpp)
add_subdirectory(third-party/emds-toolkit)

add_library(cracking_lsm INTERFACE)
add_library(cracking-lsm::cracking-lsm ALIAS cracking_lsm)

target_include_directories(
    cracking_lsm
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(cracking_lsm INTERFACE cxx_std_23)
target_link_libraries(cracking_lsm INTERFACE absl::btree emds-toolkit::emds-toolkit)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(CRACKING_LSM_BUILD_TESTS_DEFAULT ON)
else()
    set(CRACKING_LSM_BUILD_TESTS_DEFAULT OFF)
endif()

option(
    CRACKING_LSM_BUILD_TESTS
    "Build cracking-lsm tests"
    ${CRACKING_LSM_BUILD_TESTS_DEFAULT}
)

unset(CRACKING_LSM_BUILD_TESTS_DEFAULT)

if(CRACKING_LSM_BUILD_TESTS)
    if(NOT TARGET GTest::gtest_main)
        include(FetchContent)

        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
            URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )

        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    enable_testing()
    add_subdirectory(tests)
endif()
