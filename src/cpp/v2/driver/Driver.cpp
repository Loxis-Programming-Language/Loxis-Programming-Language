#include "Driver.hpp"
#include "ModuleLoader.hpp"
#include "../backend/Lowering.hpp"
#include "../backend/MIRCompiler.hpp"
#include "../hir/HirBuilder.hpp"
#include "../hir/Monomorphizer.hpp"
#include "../type/TypeChecker.hpp"
#include "../../vm/VM.hpp"
#include "../../Error.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
bool enableStderrColor() {
#ifdef _WIN32
    if (!_isatty(_fileno(stderr))) return false;
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) return false;
    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return isatty(fileno(stderr));
#endif
}

void printCompilerDiagnostic(const loxis::v2::Diagnostic& diagnostic) {
    const bool color = enableStderrColor();
    if (color) std::cerr << "\x1b[91m";
    const char* kind = diagnostic.kind == loxis::v2::DiagnosticKind::Syntax ? "SyntaxError" :
                       diagnostic.kind == loxis::v2::DiagnosticKind::Type ? "TypeError" : "CompileError";
    std::cerr << kind << ": " << diagnostic.message << "\n";
    const auto& loc = diagnostic.primary.span.start;
    if (!loc.file.empty() && loc.line > 0) {
        std::cerr << " --> " << loc.file << ":" << loc.line << ":" << loc.col << "\n"
                  << "  |\n";
        std::ifstream input(loc.file);
        std::string source;
        for (uint32_t line = 1; line <= loc.line && std::getline(input, source); ++line) {
            if (line != loc.line) continue;
            if (!source.empty() && source.back() == '\r') source.pop_back();
            const std::string lineNo = std::to_string(loc.line);
            std::cerr << lineNo << " | " << source << "\n"
                      << std::string(lineNo.size(), ' ') << " | ";
            const size_t start = loc.col > 0 ? loc.col - 1 : 0;
            for (size_t i = 0; i < start && i < source.size(); ++i)
                std::cerr << (source[i] == '\t' ? '\t' : ' ');
            size_t width = 1;
            if (diagnostic.primary.span.end.line == loc.line && diagnostic.primary.span.end.col > loc.col)
                width = diagnostic.primary.span.end.col - loc.col;
            std::cerr << std::string(std::min(width, std::max<size_t>(1, source.size() - std::min(start, source.size()))), '^');
            if (!diagnostic.primary.label.empty()) std::cerr << " " << diagnostic.primary.label;
            std::cerr << "\n";
        }
    }
    if (!diagnostic.notes.empty() || diagnostic.help) std::cerr << "  |\n";
    for (const auto& note : diagnostic.notes) std::cerr << "  = note: " << note << "\n";
    if (diagnostic.help) std::cerr << "  = help: " << *diagnostic.help << "\n";
    if (color) std::cerr << "\x1b[0m";
}

void printUncaughtTraceback(const std::string& traceback) {
    if (!enableStderrColor()) {
        std::cerr << traceback << "\n";
        return;
    }

    constexpr const char* red = "\x1b[91m";
    constexpr const char* boldRed = "\x1b[1;91m";
    constexpr const char* reset = "\x1b[0m";
    const size_t firstNewline = traceback.find('\n');
    const size_t lastNewline = traceback.rfind('\n');
    if (firstNewline == std::string::npos) {
        std::cerr << boldRed << traceback << reset << "\n";
        return;
    }
    std::cerr << boldRed << traceback.substr(0, firstNewline) << reset << "\n" << red;
    if (lastNewline > firstNewline) {
        std::cerr << traceback.substr(firstNewline + 1, lastNewline - firstNewline);
        std::cerr << reset << boldRed << traceback.substr(lastNewline + 1);
    } else {
        std::cerr << reset << boldRed << traceback.substr(firstNewline + 1);
    }
    std::cerr << reset << "\n";
}
}

namespace loxis::v2 {

int Driver::compileToChunk(const std::string& file, bool verbose, Chunk& out) {
    try {
        if (verbose) {
            std::cout << "Loading modules...\n";
        }

        ModuleLoader loader(file);
        Module mod = loader.load();

        if (verbose) {
            std::cout << "Checking types...\n";
        }

        TypeChecker checker;
        if (!checker.checkModule(mod)) {
            for (const auto& err : checker.getErrors()) {
                printCompilerDiagnostic(err);
            }
            return 1;
        }

        if (verbose) {
            std::cout << "Building HIR...\n";
        }

        HirBuilder hirBuilder;
        hirBuilder.setScope(checker.getScope());
        HirModule hir = hirBuilder.build(mod);

        if (verbose) {
            std::cout << "Monomorphizing...\n";
        }

        Monomorphizer mono;
        hir = mono.monomorphize(hir);

        if (verbose) {
            std::cout << "Lowering HIR -> MIR...\n";
        }

        Lowering lowering;
        auto output = lowering.lower(hir);

        if (verbose) {
            std::cout << "Compiling MIR -> bytecode...\n";
        }

        MIRCompiler compiler;
        out = compiler.compile(output);
        return 0;
    } catch (const FrontendError& e) {
        printCompilerDiagnostic(e.diagnostic);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

int Driver::compileAndRun(const RunOptions& opts) {
    Chunk chunk;
    if (compileToChunk(opts.file, opts.verbose, chunk) != 0) return 1;

    try {
        if (opts.verbose) {
            std::cout << "Running VM (max instructions: " << opts.maxInstr << ")...\n";
        }

        VM vm(std::move(chunk), opts.verbose, opts.gc, opts.heapSlots);
        vm.setArguments(opts.arguments);
        vm.run(opts.maxInstr);
        if (vm.truncated()) throw std::runtime_error("instruction limit exceeded");

        if (opts.gcStats) {
            vm.finishGC();
            vm.dumpGCStats(std::cerr);
        }

        return 0;
    } catch (const UncaughtLoxisException& e) {
        printUncaughtTraceback(e.what());
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace loxis::v2
