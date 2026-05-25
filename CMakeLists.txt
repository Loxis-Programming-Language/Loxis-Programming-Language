cmake_minimum_required(VERSION 3.26)
project(Loxis)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(loxis_vm
        src/cpp/vm/VM.cpp)

target_include_directories(loxis_vm PUBLIC
        src/cpp)

add_library(loxis_v2
        src/cpp/v2/backend/Lowering.cpp
        src/cpp/v2/backend/MIRCompiler.cpp
        src/cpp/v2/driver/Driver.cpp
        src/cpp/v2/frontend/Lexer.cpp
        src/cpp/v2/frontend/Parser.cpp
        src/cpp/v2/type/Scope.cpp
        src/cpp/v2/type/TypeChecker.cpp)

target_include_directories(loxis_v2 PUBLIC
        src/cpp)

target_link_libraries(loxis_v2 PUBLIC loxis_vm)

# v2 is the default Loxis compiler.
add_executable(Loxis
        src/cpp/main_v2.cpp
)

target_link_libraries(Loxis PRIVATE loxis_v2)

# v1 is kept only as a deprecated compatibility target.
option(LOXIS_BUILD_V1_DEPRECATED "Build the deprecated v1 compiler target" OFF)
if(LOXIS_BUILD_V1_DEPRECATED)
    message(WARNING "LoxisV1 is deprecated; the default Loxis target uses v2.")
    add_executable(LoxisV1
            src/cpp/main.cpp
            src/cpp/frontend/Lexer.cpp
            src/cpp/frontend/Parser.cpp
            src/cpp/ir/IRGen.cpp
            src/cpp/backend/Compiler.cpp)
    target_link_libraries(LoxisV1 PRIVATE loxis_vm)
endif()
