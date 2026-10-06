cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(assembly-line C)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

file(GLOB SOURCES ${CMAKE_SOURCE_DIR}/src/*.c)
add_executable(assembly_line ${SOURCES})
