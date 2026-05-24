cmake_minimum_required(VERSION 3.26)
project(Loinx)

set(CMAKE_CXX_STANDARD 20)

add_executable(Loinx
        src/cpp/main.cpp
        src/cpp/frontend/Lexer.cpp
        src/cpp/frontend/Parser.cpp
        src/cpp/ir/IRGen.cpp
        src/cpp/backend/Compiler.cpp
        src/cpp/vm/VM.cpp)
