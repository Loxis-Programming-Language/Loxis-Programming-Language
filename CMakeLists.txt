cmake_minimum_required(VERSION 3.26)
project(Loinx)

set(CMAKE_CXX_STANDARD 20)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# Original v1 compiler
add_executable(Loinx
        src/cpp/main.cpp
        src/cpp/frontend/Lexer.cpp
        src/cpp/frontend/Parser.cpp
        src/cpp/ir/IRGen.cpp
        src/cpp/backend/Compiler.cpp
        src/cpp/vm/VM.cpp)

# V2 compiler (next-gen)
file(GLOB_RECURSE V2_SRC src/cpp/v2/*.cpp)
add_executable(LoinxV2
        src/cpp/main_v2.cpp
        ${V2_SRC}
        src/cpp/vm/VM.cpp
        src/cpp/backend/Chunk.hpp)
