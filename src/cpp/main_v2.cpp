#include "v2/driver/Driver.hpp"
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

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
    std::printf(kUsage, program, program, program, program);
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

        for (size_t i = fileIndex + 1; i < args.size(); ++i) {
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
                    return 1;
                }
            } else if (arg.rfind("--slots=", 0) == 0) {
                opts.heapSlots = static_cast<uint32_t>(std::stoul(arg.substr(8)));
            } else if (arg.rfind("--dump=", 0) == 0) {
                if (!selectDumpStage(arg.substr(7), opts.dumps)) {
                    std::cerr << "Unknown dump stage: " << arg.substr(7) << "\n";
                    return 1;
                }
            } else if (arg.rfind("--dump-", 0) == 0) {
                if (!selectDumpStage(arg.substr(7), opts.dumps)) {
                    std::cerr << "Unknown dump flag: " << arg << "\n";
                    return 1;
                }
            } else if (arg == "verbose" || arg == "true" || arg == "yes" || arg == "1") {
                // Legacy positional form: `Loxis demo.lx 100000 verbose`.
                opts.verbose = true;
            } else if (isUnsignedNumber(arg)) {
                opts.maxInstr = std::stoi(arg);
            } else {
                // Silently feeding this to stoi is what used to turn a typo into
                // "Fatal error: stoi".
                std::cerr << "Unknown argument: " << arg << "\n\n";
                printUsage(argv[0]);
                return 1;
            }
        }

        loxis::v2::Driver driver;
        return driver.compileAndRun(opts);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
