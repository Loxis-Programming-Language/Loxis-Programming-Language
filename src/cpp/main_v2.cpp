#include "v2/driver/Driver.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

#ifndef LOXIS_VERSION
#define LOXIS_VERSION "dev"
#endif

const char* kUsage =
    "Loxis - a statically typed language with a register-based VM\n"
    "\n"
    "Usage:\n"
    "  %s run <file.lx> [options] [-- program args]\n"
    "  %s check <file.lx> [options]\n"
    "  %s test [path] [options] [-- test args]\n"
    "  %s <file.lx> [max_instructions] [options] [-- program args]\n"
    "  %s version\n"
    "\n"
    "Options:\n"
    "  -v, --verbose        Stage banners, loaded modules and per-stage timings\n"
    "      --trace          Fine-grained pipeline log: module loads, declarations,\n"
    "                       and every method/field lookup with its candidates\n"
    "      --dump-tokens    Print the lexer output and stop\n"
    "      --dump-ast       Print the parsed AST and stop\n"
    "      --dump-hir       Print the monomorphised HIR and stop\n"
    "      --dump-mir       Print the lowered MIR and stop\n"
    "      --dump-bytecode  Print the final bytecode and stop\n"
    "      --dump-scopes    Print the scope/symbol tree and stop\n"
    "      --dump=all       Print every stage above\n"
    "      --gc=<nogc|arc|aarc>   GC backend (default: nogc)\n"
    "      --slots=N        Heap slot count (0 = backend default)\n"
    "      --gc-stats       Print GC statistics after the run\n"
    "  -h, --help           Show this help\n"
    "      --version        Show the Loxis version\n"
    "\n"
    "Any --dump-* flag makes the compiler compile-only: the program is not run,\n"
    "so IR text and program output never interleave.\n"
    "\n"
    "The legacy positional forms still work: `file.lx 100000 verbose`.\n";

void printUsage(const char* program) {
    std::printf(kUsage, program, program, program, program, program);
}

bool selectDumpStage(const std::string& stage, loxis::v2::DumpOptions& dumps) {
    if (stage == "all") {
        dumps.tokens = dumps.ast = dumps.hir = true;
        dumps.mir = dumps.bytecode = dumps.scopes = true;
    } else if (stage == "tokens") {
        dumps.tokens = true;
    } else if (stage == "ast") {
        dumps.ast = true;
    } else if (stage == "hir") {
        dumps.hir = true;
    } else if (stage == "mir") {
        dumps.mir = true;
    } else if (stage == "bytecode") {
        dumps.bytecode = true;
    } else if (stage == "scopes") {
        dumps.scopes = true;
    } else {
        return false;
    }
    return true;
}

bool isUnsignedNumber(const std::string& text) {
    return !text.empty() && text.find_first_not_of("0123456789") == std::string::npos;
}

std::filesystem::path executableDirectory(const char* argv0) {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    std::error_code ec;
    const auto path = std::filesystem::absolute(argv0, ec);
    if (!ec) return path.parent_path();
    return {};
}

void configureBundledStdlib(loxis::v2::RunOptions& opts, const char* argv0) {
    const auto bundledStdlib = executableDirectory(argv0) / "stdlib";
    if (std::filesystem::exists(bundledStdlib / "loxis" / "prelude.lx")) {
        opts.stdlibRoot = bundledStdlib.string();
    }
}

bool parseOptions(
    const std::vector<std::string>& args,
    size_t start,
    loxis::v2::RunOptions& opts,
    const char* program)
{
    for (size_t i = start; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            for (++i; i < args.size(); ++i) opts.arguments.emplace_back(args[i]);
            break;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "--trace") {
            opts.trace = true;
        } else if (arg == "--gc-stats") {
            opts.gcStats = true;
        } else if (arg.rfind("--gc=", 0) == 0) {
            if (!parseGCMode(arg.substr(5), opts.gc)) {
                std::cerr << "Unknown GC backend: " << arg.substr(5)
                          << " (expected nogc, arc or aarc)\n";
                return false;
            }
        } else if (arg.rfind("--slots=", 0) == 0) {
            opts.heapSlots = static_cast<uint32_t>(std::stoul(arg.substr(8)));
        } else if (arg.rfind("--dump=", 0) == 0) {
            if (!selectDumpStage(arg.substr(7), opts.dumps)) {
                std::cerr << "Unknown dump stage: " << arg.substr(7) << "\n";
                return false;
            }
        } else if (arg.rfind("--dump-", 0) == 0) {
            if (!selectDumpStage(arg.substr(7), opts.dumps)) {
                std::cerr << "Unknown dump flag: " << arg << "\n";
                return false;
            }
        } else if (arg == "verbose" || arg == "true" || arg == "yes" || arg == "1") {
            opts.verbose = true;
        } else if (isUnsignedNumber(arg)) {
            opts.maxInstr = std::stoi(arg);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n\n";
            printUsage(program);
            return false;
        }
    }
    return true;
}

int runTests(
    const std::vector<std::string>& args,
    const char* argv0)
{
    namespace fs = std::filesystem;

    fs::path target = "tests";
    size_t optionStart = 1;
    if (args.size() > 1 && args[1] != "--" && args[1].rfind("-", 0) != 0) {
        target = args[1];
        optionStart = 2;
    }

    std::error_code ec;
    std::vector<fs::path> files;
    if (fs::is_regular_file(target, ec)) {
        if (target.extension() != ".lx") {
            std::cerr << "Test file must use the .lx extension: " << target.string() << "\n";
            return 1;
        }
        files.push_back(fs::absolute(target));
    } else if (fs::is_directory(target, ec)) {
        for (fs::recursive_directory_iterator it(target, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file() && it->path().extension() == ".lx") {
                files.push_back(fs::absolute(it->path()));
            }
        }
    } else {
        std::cerr << "Test path does not exist: " << target.string() << "\n";
        return 1;
    }
    if (ec) {
        std::cerr << "Could not scan test path " << target.string() << ": " << ec.message() << "\n";
        return 1;
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cerr << "No .lx tests found under: " << target.string() << "\n";
        return 1;
    }

    loxis::v2::RunOptions base;
    configureBundledStdlib(base, argv0);
    if (!parseOptions(args, optionStart, base, argv0)) return 1;
    if (base.dumps.any()) {
        std::cerr << "Dump options are not supported by 'Loxis test'\n";
        return 1;
    }

    size_t passed = 0;
    loxis::v2::Driver driver;
    for (const auto& file : files) {
        std::cout << "[ RUN  ] " << file.string() << "\n";
        auto opts = base;
        opts.file = file.string();
        const int result = driver.compileAndRun(opts);
        if (result == 0) {
            ++passed;
            std::cout << "[ PASS ] " << file.filename().string() << "\n";
        } else {
            std::cout << "[ FAIL ] " << file.filename().string() << "\n";
        }
    }

    const size_t failed = files.size() - passed;
    std::cout << passed << " passed, " << failed << " failed, "
              << files.size() << " total\n";
    return failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const std::vector<std::string> args(argv + 1, argv + argc);

        for (const std::string& arg : args) {
            if (arg == "-h" || arg == "--help") {
                printUsage(argv[0]);
                return 0;
            }
        }

        if (args.size() == 1 && (args[0] == "version" || args[0] == "--version")) {
            std::cout << "Loxis " << LOXIS_VERSION << "\n";
            return 0;
        }

        if (args.empty()) {
            printUsage(argv[0]);
            return 1;
        }

        if (args[0] == "test") {
            return runTests(args, argv[0]);
        }

        const bool hasCommand = args[0] == "run" || args[0] == "check";
        const size_t fileIndex = hasCommand ? 1 : 0;
        if (fileIndex >= args.size()) {
            std::cerr << "Missing .lx file after '" << args[0] << "'\n\n";
            printUsage(argv[0]);
            return 1;
        }
        if (args[fileIndex].rfind("-", 0) == 0) {
            std::cerr << "Expected a .lx file, got: " << args[fileIndex] << "\n\n";
            printUsage(argv[0]);
            return 1;
        }

        loxis::v2::RunOptions opts;
        opts.file = args[fileIndex];
        opts.compileOnly = hasCommand && args[0] == "check";
        configureBundledStdlib(opts, argv[0]);
        if (!parseOptions(args, fileIndex + 1, opts, argv[0])) return 1;

        loxis::v2::Driver driver;
        return driver.compileAndRun(opts);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
