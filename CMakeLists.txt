cmake_minimum_required(VERSION 3.26)
project(Loxis VERSION 0.1.0 LANGUAGES CXX)
set(LOXIS_VERSION "${PROJECT_VERSION}-alpha.2")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# <winsock2.h> and <windows.h> define min/max as function-like macros unless
# NOMINMAX is set. Those macros corrupt any `std::min(...)`, `std::max(...)` or
# `std::numeric_limits<T>::max()` call in a translation unit that includes the
# Windows headers -- the argument list is rewritten before the compiler sees it.
add_compile_definitions(NOMINMAX)

add_library(loxis_vm
        src/cpp/backend/Disasm.cpp
        src/cpp/vm/VM.cpp
        src/cpp/vm/GC.cpp)

target_include_directories(loxis_vm PUBLIC
        src/cpp
        src/aarc)

if(WIN32)
    target_link_libraries(loxis_vm PUBLIC ws2_32)
endif()

add_library(loxis_v2
        src/cpp/v2/backend/Lowering.cpp
        src/cpp/v2/backend/MIRCompiler.cpp
        src/cpp/v2/dump/AstDump.cpp
        src/cpp/v2/dump/Dump.cpp
        src/cpp/v2/dump/HirDump.cpp
        src/cpp/v2/dump/MirDump.cpp
        src/cpp/v2/dump/TokensDump.cpp
        src/cpp/v2/dump/Trace.cpp
        src/cpp/v2/hir/HirBuilder.cpp
        src/cpp/v2/hir/Monomorphizer.cpp
        src/cpp/v2/driver/ModuleLoader.cpp
        src/cpp/v2/driver/Driver.cpp
        src/cpp/v2/frontend/Lexer.cpp
        src/cpp/v2/frontend/Parser.cpp
        src/cpp/v2/type/Scope.cpp
        src/cpp/v2/type/TypeChecker.cpp)

target_include_directories(loxis_v2 PUBLIC
        src/cpp)

target_link_libraries(loxis_v2 PUBLIC loxis_vm)

add_executable(Loxis
        src/cpp/main_v2.cpp
)

target_link_libraries(Loxis PRIVATE loxis_v2)
target_compile_definitions(Loxis PRIVATE LOXIS_VERSION="${LOXIS_VERSION}")

# Recursive-descent parsing and the AST walkers that follow all run on the native
# stack. The default 1 MB reserve is what the nesting limits below are sized
# against; give them room so those limits are not forced down to single digits.
if(MSVC)
    target_link_options(Loxis PRIVATE /STACK:8388608)
endif()

enable_testing()

add_test(
        NAME loxis_smoke
        COMMAND powershell -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tests/run_smoke.ps1 -Exe $<TARGET_FILE:Loxis>
)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME loxis_stdlib
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_stdlib.py --exe $<TARGET_FILE:Loxis>)
    set_tests_properties(loxis_stdlib PROPERTIES TIMEOUT 300)
    add_test(NAME loxis_generic_inference
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_generic_inference.py --exe $<TARGET_FILE:Loxis>)
    add_test(NAME loxis_parser_regressions
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_parser_regressions.py --exe $<TARGET_FILE:Loxis>)
    add_test(NAME loxis_call_arguments
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_call_arguments.py --exe $<TARGET_FILE:Loxis>)
    add_test(NAME loxis_api_boundary
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_api_boundary.py --exe $<TARGET_FILE:Loxis>)
    add_test(NAME loxis_examples
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_examples.py --exe $<TARGET_FILE:Loxis>)
    add_test(NAME loxis_cli
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_cli.py --exe $<TARGET_FILE:Loxis>)
    add_test(NAME loxis_distribution
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_distribution.py --exe $<TARGET_FILE:Loxis>)
    add_test(NAME loxis_todo_cli
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_todo_cli.py --exe $<TARGET_FILE:Loxis>)
endif()

add_executable(LSPTest
        src/cpp/tools/LSPTest.cpp
)

add_executable(bench_gc
        src/cpp/tools/BenchGC.cpp
)

target_link_libraries(bench_gc PRIVATE loxis_v2)

# A release archive is deliberately flat: Loxis.exe sits beside stdlib/, so it
# can discover the bundled library regardless of the caller's working directory.
install(TARGETS Loxis RUNTIME DESTINATION .)
install(DIRECTORY stdlib DESTINATION .)
install(DIRECTORY examples DESTINATION .)
install(FILES README.md CHANGELOG.md LICENSE DESTINATION .)
install(DIRECTORY docs DESTINATION .)

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "Loxis")
set(CPACK_PACKAGE_VERSION "${LOXIS_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "Loxis-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
include(CPack)
