cmake_minimum_required(VERSION 4.0)

project(aurhub LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

set(_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
find_package(ZLIB REQUIRED)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${_saved_suffixes})
find_package(CLI11 REQUIRED)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v4.6.4
    GIT_SHALLOW TRUE
)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(simdjson)

add_library(aurhub_core
    src/generation.cpp
    src/search.cpp
    src/snapshot.cpp
    src/srcinfo.cpp
)
target_include_directories(aurhub_core PUBLIC src)
target_link_libraries(aurhub_core PUBLIC ZLIB::ZLIB simdjson::simdjson)
target_link_options(aurhub_core PUBLIC -static)
target_compile_options(aurhub_core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

add_executable(aurhub-indexer src/indexer.cpp)
target_link_libraries(aurhub-indexer PRIVATE aurhub_core CLI11::CLI11)
target_compile_options(aurhub-indexer PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

add_executable(aurhubd src/server.cpp)
target_link_libraries(aurhubd PRIVATE aurhub_core CLI11::CLI11)
target_compile_options(aurhubd PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

add_executable(aurhub-search-bench src/search_bench.cpp)
target_link_libraries(aurhub-search-bench PRIVATE aurhub_core CLI11::CLI11)
target_compile_options(aurhub-search-bench PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

add_executable(aurhub-query-bench src/query_bench.cpp)
target_link_libraries(aurhub-query-bench PRIVATE aurhub_core CLI11::CLI11)
target_compile_options(aurhub-query-bench PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

add_executable(aurhub-generation-compare src/generation_compare.cpp)
target_link_libraries(aurhub-generation-compare PRIVATE aurhub_core)
target_compile_options(aurhub-generation-compare PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

add_executable(aurhub-package-dump src/package_dump.cpp)
target_link_libraries(aurhub-package-dump PRIVATE aurhub_core)
target_compile_options(aurhub-package-dump PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

add_executable(aurhub-srcinfo-test tests/srcinfo_test.cpp)
target_link_libraries(aurhub-srcinfo-test PRIVATE aurhub_core)
target_compile_options(aurhub-srcinfo-test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

enable_testing()
add_test(NAME aurhub-srcinfo COMMAND aurhub-srcinfo-test)
add_test(
    NAME aurhub-diagnostics
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics.sh
            $<TARGET_FILE:aurhub-indexer>
            $<TARGET_FILE:aurhub-query-bench>
)
add_test(
    NAME aurhub-smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.sh
            $<TARGET_FILE:aurhub-indexer>
            $<TARGET_FILE:aurhubd>
            $<TARGET_FILE:aurhub-search-bench>
            $<TARGET_FILE:aurhub-generation-compare>
)

add_test(
    NAME aurhub-generation
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/generation.sh
            $<TARGET_FILE:aurhub-indexer>
            $<TARGET_FILE:aurhub-query-bench>
            $<TARGET_FILE:aurhub-generation-compare>
)
