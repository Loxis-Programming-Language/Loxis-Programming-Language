cmake_minimum_required(VERSION 3.26)
project(Loxis VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Unified output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# Include root for clean #include paths
include_directories(src/cpp)

# ------------------------------------------------------------------
# V1 compiler (original)
# ------------------------------------------------------------------
add_executable(Loinx
    src/cpp/main.cpp
    src/cpp/frontend/Lexer.cpp
    src/cpp/frontend/Parser.cpp
    src/cpp/ir/IRGen.cpp
    src/cpp/backend/Compiler.cpp
    src/cpp/vm/VM.cpp)

# ------------------------------------------------------------------
# V2 compiler (next-gen)
# ------------------------------------------------------------------
file(GLOB_RECURSE V2_SRC src/cpp/v2/*.cpp)
add_executable(LoinxV2
    src/cpp/main_v2.cpp
    ${V2_SRC}
    src/cpp/vm/VM.cpp
    src/cpp/backend/Chunk.hpp)

# ------------------------------------------------------------------
# Copy stdlib to build tree so binaries can find it at runtime
# ------------------------------------------------------------------
add_custom_target(stdlib_copy ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/std
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/std
    COMMENT "Copying standard library to build output"
)
add_dependencies(Loinx stdlib_copy)
add_dependencies(LoinxV2 stdlib_copy)

# ------------------------------------------------------------------
# Install targets
# ------------------------------------------------------------------
install(TARGETS Loinx LoinxV2
    RUNTIME DESTINATION bin)

install(DIRECTORY std/
    DESTINATION std
    FILES_MATCHING PATTERN "*.lxs")
