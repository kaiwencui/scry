cmake_minimum_required(VERSION 3.20)
project(scry VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SCRY_NATIVE_ARCH "Enable -march=native SIMD optimizations" ON)

add_library(scry_core STATIC
    src/gitignore.cpp
    src/io_uring_reader.cpp
    src/regex_engine.cpp
    src/posting.cpp
    src/symbols.cpp
    src/indexer.cpp
    src/delta.cpp
    src/searcher.cpp
    src/daemon.cpp
    src/tui.cpp
    src/cli.cpp
)

target_include_directories(scry_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(scry_core PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter -fno-rtti
    $<$<CONFIG:Release>:-O3>
    $<$<AND:$<CONFIG:Release>,$<BOOL:${SCRY_NATIVE_ARCH}>>:-march=native>
)

find_package(Threads REQUIRED)
target_link_libraries(scry_core PUBLIC Threads::Threads)
if(APPLE)
    target_link_libraries(scry_core PUBLIC "-framework CoreServices")
endif()

add_executable(scry src/main.cpp)
target_link_libraries(scry PRIVATE scry_core)
install(TARGETS scry RUNTIME DESTINATION bin)

add_executable(scry-test tests/test_main.cpp)
target_link_libraries(scry-test PRIVATE scry_core)

add_executable(scry-bench tests/bench_engine.cpp)
target_link_libraries(scry-bench PRIVATE scry_core)

enable_testing()
add_test(NAME scry_unit_tests COMMAND scry-test)
