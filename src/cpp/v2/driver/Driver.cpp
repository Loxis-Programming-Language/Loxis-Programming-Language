#include "Driver.hpp"
#include "../AST.hpp"
#include "../backend/Lowering.hpp"
#include "../backend/MIRCompiler.hpp"
#include "../../vm/VM.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace loxis::v2 {

// Forward declarations for frontend components (to be linked later)
class Lexer;
class Parser;
class ScopeBuilder;
class TypeChecker;

int Driver::compileAndRun(const std::string& file, int maxInstr, bool verbose) {
    try {
        std::ifstream in(file);
        if (!in) {
            std::cerr << "Error: cannot open " << file << "\n";
            return 1;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string source = buffer.str();

        // TODO: enable when frontend is available
#if 0
        Lexer lexer(source);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        Module mod = parser.parseModule();
        ScopeBuilder sb;
        sb.build(mod);
        TypeChecker tc;
        auto errors = tc.check(mod);
        if (!errors.empty()) {
            for (const auto& err : errors) {
                std::cerr << "Type error: " << err.msg << "\n";
            }
            return 1;
        }
#else
        (void)source;
        Module mod;
        mod.name = file;
#endif

        if (verbose) {
            std::cout << "Lowering AST -> MIR...\n";
        }

        Lowering lowering;
        auto bodies = lowering.lowerModule(mod);

        if (verbose) {
            std::cout << "Compiling MIR -> bytecode...\n";
        }

        MIRCompiler compiler;
        Chunk chunk = compiler.compile(bodies);

        if (verbose) {
            std::cout << "Running VM (max instructions: " << maxInstr << ")...\n";
        }

        VM vm(std::move(chunk));
        vm.run(maxInstr);

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace loxis::v2
