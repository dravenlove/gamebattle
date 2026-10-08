cmake_minimum_required(VERSION 3.20)

project(gamebattle_practice LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Reuse the real engine as a library; its own tests and tools are not needed here.
set(GAMEBATTLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GAMEBATTLE_BUILD_NIF OFF CACHE BOOL "" FORCE)
set(GAMEBATTLE_BUILD_CONFIG_COMPILER OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. gamebattle EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)

function(practice_program name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE gamebattle_core Threads::Threads)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

practice_program(battle_thread_pool battle_thread_pool.cpp)
practice_program(battle_bench battle_bench.cpp)
practice_program(encode_bench encode_bench.cpp)
practice_program(term_fuzz term_fuzz.cpp)

if(NOT WIN32)
    practice_program(battle_tcp_server battle_tcp_server.cpp)
endif()

# Lesson 20: standalone game-server data structures (no engine dependency).
foreach(demo rank_test timers aoi consistent_hash aos_soa)
    add_executable(ds_${demo} ds/${demo}.cpp)
endforeach()
