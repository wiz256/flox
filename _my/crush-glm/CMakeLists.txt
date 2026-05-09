cmake_minimum_required(VERSION 3.22)
project(crush_grid LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(FLOX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(FLOX_INCLUDE "${FLOX_DIR}/include")
set(FLOX_BUILD "${FLOX_DIR}/build")

find_package(fmt REQUIRED)

add_executable(crush_grid
    src/main.cpp
    src/csv_reader.cpp
)

target_include_directories(crush_grid PRIVATE
    ${FLOX_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(crush_grid PRIVATE
    flox
    fmt::fmt
)

target_link_directories(crush_grid PRIVATE
    ${FLOX_BUILD}
)

target_compile_options(crush_grid PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

target_compile_options(crush_grid PRIVATE
    $<$<CONFIG:Debug>:-O0 -g -fsanitize=address -fno-omit-frame-pointer>
    $<$<CONFIG:Release>:-O2>
)
target_link_options(crush_grid PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address>
)
