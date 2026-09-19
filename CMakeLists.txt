cmake_minimum_required(VERSION 3.14)

project(gcra_ipc
    VERSION 1.0.0
    DESCRIPTION "Interprocess tick-less GCRA rate shaper in shared memory"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(gcra_ipc src/gcra_ipc.cpp)
add_library(gcra_ipc::gcra_ipc ALIAS gcra_ipc)

target_include_directories(gcra_ipc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

include(CheckLibraryExists)
check_library_exists(rt shm_open "" GCRA_IPC_HAVE_LIBRT)
if(GCRA_IPC_HAVE_LIBRT)
    target_link_libraries(gcra_ipc PRIVATE rt)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(gcra_ipc PRIVATE -Wall -Wextra)
endif()

option(GCRA_IPC_BUILD_TESTS "Build gcra_ipc tests" ON)
if(GCRA_IPC_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
        URL_HASH SHA256=f179ec217f9b3b3f3c6e8b02d3e7eda997b49e4ce26d6b235c9053bec9c0bf9f
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(gcra_ipc_test tests/gcra_ipc_test.cpp)
    target_compile_options(gcra_ipc_test PRIVATE -Wall -Wextra)
    target_link_libraries(gcra_ipc_test PRIVATE gcra_ipc GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(gcra_ipc_test)
endif()
