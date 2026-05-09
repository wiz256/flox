cmake_minimum_required(VERSION 3.20)
project(flox_my_strategies LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FLOX must be built first; these headers come from the parent FLOX tree
set(FLOX_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include")
set(FLOX_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/../build")

add_executable(run_all_grids
    grid_searches/run_all_grids.cpp
)

target_include_directories(run_all_grids PRIVATE
    ${FLOX_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(run_all_grids PRIVATE
    flox
)

# Individual strategy targets (for debugging/testing individual strategies)
# add_executable(test_donchian grid_searches/test_single.cpp)
# target_compile_definitions(test_donchian PRIVATE STRATEGY=DONCHIAN)
