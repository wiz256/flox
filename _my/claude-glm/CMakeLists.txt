cmake_minimum_required(VERSION 3.22)
project(flox_strategies LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FLOX source tree — adjust if your layout differs
set(FLOX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Bring in FLOX's static library
add_subdirectory(${FLOX_ROOT} ${CMAKE_BINARY_DIR}/flox)

# Grid search executable (requires backtest module)
add_executable(crush_grid
    grid_search_main.cpp
)

target_include_directories(crush_grid PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}          # strategies/ and csv_bar_reader.h
    ${FLOX_ROOT}/include                 # flox/*.h headers
)

target_link_libraries(crush_grid PRIVATE flox)

# macOS: don't complain about unused result from read()
if(APPLE)
    target_compile_options(crush_grid PRIVATE -Wno-unused-result)
endif()
