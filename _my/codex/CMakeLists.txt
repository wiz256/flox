cmake_minimum_required(VERSION 3.20)

project(flox_codex_mtf_grid LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep the local build small. The parent FLOX project still builds the core
# library and the replay/backtest components used by this example.
set(FLOX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLOX_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(FLOX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(FLOX_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(FLOX_BUILD_NODE OFF CACHE BOOL "" FORCE)
set(FLOX_BUILD_CAPI OFF CACHE BOOL "" FORCE)
set(FLOX_BUILD_CODON OFF CACHE BOOL "" FORCE)
set(FLOX_BUILD_QUICKJS OFF CACHE BOOL "" FORCE)
set(FLOX_ENABLE_BACKTEST ON CACHE BOOL "" FORCE)
set(FLOX_ENABLE_LZ4 ON CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/flox)

add_executable(codex_mtf_grid_backtest
  src/codex_mtf_grid_backtest.cpp
)

target_link_libraries(codex_mtf_grid_backtest PRIVATE flox::flox)
target_include_directories(codex_mtf_grid_backtest PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../include)

if(APPLE)
  target_compile_options(codex_mtf_grid_backtest PRIVATE -Wall -Wextra -Wpedantic)
else()
  target_compile_options(codex_mtf_grid_backtest PRIVATE -Wall -Wextra -Wpedantic)
endif()
