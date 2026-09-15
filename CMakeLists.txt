cmake_minimum_required(VERSION 3.26)
project(Loxis)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(loxis_vm
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
endif()

add_executable(LSPTest
        src/cpp/tools/LSPTest.cpp
)

add_executable(bench_gc
        src/cpp/tools/BenchGC.cpp
)

target_link_libraries(bench_gc PRIVATE loxis_v2)
