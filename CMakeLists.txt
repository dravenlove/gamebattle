cmake_minimum_required(VERSION 3.20)

project(gamebattle VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GAMEBATTLE_BUILD_TESTS "Build the C++ battle-core tests" ON)
option(GAMEBATTLE_BUILD_NIF "Build the Erlang NIF adapter" OFF)
option(GAMEBATTLE_BUILD_CONFIG_COMPILER "Build the standalone battle config compiler" OFF)

if(GAMEBATTLE_BUILD_CONFIG_COMPILER)
    add_executable(gamebattle_config_compiler tools/config_compiler.cpp)
    target_compile_features(gamebattle_config_compiler PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(gamebattle_config_compiler PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(gamebattle_config_compiler PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

add_library(gamebattle_core STATIC
    src/battle_state.cpp
    src/config_store.cpp
    src/effect_system.cpp
    src/engine.cpp
    src/target_selector.cpp
    src/term.cpp
    src/wire.cpp
)
set_target_properties(gamebattle_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(gamebattle_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(gamebattle_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(gamebattle_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(gamebattle_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(gamebattle_port src/port_main.cpp)
target_link_libraries(gamebattle_port PRIVATE gamebattle_core)

if(GAMEBATTLE_BUILD_NIF)
    if(NOT ERLANG_ERTS_INCLUDE_DIR)
        message(FATAL_ERROR "Set ERLANG_ERTS_INCLUDE_DIR to the directory containing erl_nif.h")
    endif()
    add_library(gamebattle_nif SHARED src/nif.cpp)
    target_include_directories(gamebattle_nif PRIVATE ${ERLANG_ERTS_INCLUDE_DIR})
    target_link_libraries(gamebattle_nif PRIVATE gamebattle_core)
    set_target_properties(gamebattle_nif PROPERTIES PREFIX "" OUTPUT_NAME "gamebattle_nif")
endif()

if(GAMEBATTLE_BUILD_TESTS)
    enable_testing()
    add_executable(gamebattle_tests tests/engine_test.cpp)
    target_link_libraries(gamebattle_tests PRIVATE gamebattle_core)
    add_test(NAME gamebattle_tests COMMAND gamebattle_tests)

    if(GAMEBATTLE_BUILD_CONFIG_COMPILER)
        set(GAMEBATTLE_TEST_CONFIG_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated-config)
        set(GAMEBATTLE_TEST_CONFIG ${GAMEBATTLE_TEST_CONFIG_DIR}/example.gbcfg)
        add_custom_command(
            OUTPUT ${GAMEBATTLE_TEST_CONFIG}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${GAMEBATTLE_TEST_CONFIG_DIR}
            COMMAND $<TARGET_FILE:gamebattle_config_compiler>
                --input-dir ${CMAKE_CURRENT_SOURCE_DIR}/config/example
                --output ${GAMEBATTLE_TEST_CONFIG}
            DEPENDS
                gamebattle_config_compiler
                tools/config_compiler.cpp
                config/example/buffs.csv
                config/example/buff_modifiers.csv
                config/example/buff_reactions.csv
                config/example/effects.csv
                config/example/skills.csv
                config/example/passives.csv
            VERBATIM
        )
        add_custom_target(gamebattle_test_config DEPENDS ${GAMEBATTLE_TEST_CONFIG})
        add_executable(gamebattle_config_tests tests/config_store_test.cpp)
        add_dependencies(gamebattle_config_tests gamebattle_test_config)
        target_link_libraries(gamebattle_config_tests PRIVATE gamebattle_core)
        target_compile_definitions(gamebattle_config_tests PRIVATE
            GAMEBATTLE_TEST_CONFIG_PATH="${GAMEBATTLE_TEST_CONFIG}")
        add_test(NAME gamebattle_config_tests COMMAND gamebattle_config_tests)

        add_executable(gamebattle_config_cycle_loader_test
            tests/config_cycle_loader_test.cpp)
        target_link_libraries(gamebattle_config_cycle_loader_test PRIVATE gamebattle_core)
        add_test(NAME gamebattle_config_loader_invalid_buff_cycle
            COMMAND gamebattle_config_cycle_loader_test)

        add_test(NAME gamebattle_config_compiler_valid
            COMMAND gamebattle_config_compiler
                --input-dir ${CMAKE_CURRENT_SOURCE_DIR}/config/example
                --check-only)
        add_test(NAME gamebattle_config_compiler_invalid_reference
            COMMAND gamebattle_config_compiler
                --input-dir ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/config_invalid_reference
                --check-only)
        add_test(NAME gamebattle_config_compiler_invalid_buff_cycle
            COMMAND gamebattle_config_compiler
                --input-dir ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/config_invalid_buff_cycle
                --check-only)
        set_tests_properties(
            gamebattle_config_compiler_invalid_reference
            gamebattle_config_compiler_invalid_buff_cycle
            PROPERTIES WILL_FAIL TRUE)
    endif()
endif()

install(TARGETS gamebattle_port RUNTIME DESTINATION priv COMPONENT BattleRuntime)
if(GAMEBATTLE_BUILD_NIF)
    install(TARGETS gamebattle_nif
        RUNTIME DESTINATION priv COMPONENT BattleRuntime
        LIBRARY DESTINATION priv COMPONENT BattleRuntime)
endif()
if(GAMEBATTLE_BUILD_CONFIG_COMPILER)
    install(TARGETS gamebattle_config_compiler
        RUNTIME DESTINATION bin
        COMPONENT ConfigTools)
endif()
