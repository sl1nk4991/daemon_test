cmake_minimum_required(VERSION 3.5)

project(daemon_test)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "-Wall -O0 -fsanitize=address")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB_RECURSE sources src/main.cpp)

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PUBLIC ${sources})

target_include_directories(${PROJECT_NAME} PUBLIC include)

