#pragma once
#include <string_view>

namespace loxis::v2 {
enum class NativeReturn { Int, String, NullableString, Double };
struct NativeBuiltin {
    std::string_view name;
    int syscall;
    NativeReturn result;
    int arity;
};
// Shared by semantic analysis, HIR construction and bytecode emission.
inline constexpr NativeBuiltin nativeBuiltins[] = {
    {"nativeFileExists", 44, NativeReturn::Int, 1},
    {"nativeTryReadText", 45, NativeReturn::NullableString, 1},
    {"nativeIoError", 46, NativeReturn::String, 0},
    {"nativeIsDirectory", 47, NativeReturn::Int, 1},
    {"nativeCreateDirectories", 48, NativeReturn::Int, 1},
    {"nativeRemoveDirectory", 49, NativeReturn::Int, 1},
    {"nativePathJoin", 50, NativeReturn::String, 2},
    {"nativePathNormalize", 51, NativeReturn::String, 1},
    {"nativePathParent", 52, NativeReturn::String, 1},
    {"nativePathName", 53, NativeReturn::String, 1},
    {"nativePathExtension", 54, NativeReturn::String, 1},
    {"nativeCurrentDirectory", 55, NativeReturn::String, 0},
    {"nativeListDirectory", 56, NativeReturn::Int, 1},
    {"nativeDirectoryEntry", 57, NativeReturn::String, 1},
    {"nativeEnvironment", 60, NativeReturn::NullableString, 1},
    {"nativeArgumentCount", 61, NativeReturn::Int, 0},
    {"nativeArgument", 62, NativeReturn::String, 1},
    {"nativeUnixMillis", 63, NativeReturn::Int, 0},
    {"nativeMonotonicNanos", 64, NativeReturn::Int, 0},
    {"nativeSqrt", 70, NativeReturn::Double, 1},
    {"nativeFloor", 71, NativeReturn::Double, 1},
    {"nativeCeil", 72, NativeReturn::Double, 1},
    {"nativeRound", 73, NativeReturn::Double, 1},
    {"nativeSin", 74, NativeReturn::Double, 1},
    {"nativeCos", 75, NativeReturn::Double, 1},
    {"nativeLog", 76, NativeReturn::Double, 1},
    {"nativeExp", 77, NativeReturn::Double, 1},
    {"nativePow", 78, NativeReturn::Double, 2},
    {"nativeIsFinite", 79, NativeReturn::Int, 1},
    {"nativeDoubleString", 80, NativeReturn::String, 1},
    {"nativeParseDouble", 81, NativeReturn::Double, 1},
    {"nativeValidDouble", 82, NativeReturn::Int, 1},
    {"nativeTcpConnect", 83, NativeReturn::Int, 2},
    {"nativeTcpBind", 84, NativeReturn::Int, 2},
    {"nativeTcpAccept", 85, NativeReturn::Int, 1},
    {"nativeSocketRead", 86, NativeReturn::NullableString, 2},
    {"nativeSocketWrite", 87, NativeReturn::Int, 2},
    {"nativeSocketClose", 88, NativeReturn::Int, 1},
    {"nativeNetError", 89, NativeReturn::String, 0},
    {"nativeSocketLocalPort", 90, NativeReturn::Int, 1},
};
inline const NativeBuiltin* findNativeBuiltin(std::string_view name) {
    for (const auto& builtin : nativeBuiltins)
        if (builtin.name == name) return &builtin;
    return nullptr;
}
}
