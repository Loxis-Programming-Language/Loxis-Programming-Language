#include "Driver.hpp"
#include "../AST.hpp"
#include "../backend/Lowering.hpp"
#include "../backend/MIRCompiler.hpp"
#include "../frontend/Parser.hpp"
#include "../type/TypeChecker.hpp"
#include "../../vm/VM.hpp"
#include <iostream>
#include <stdexcept>

namespace loxis::v2 {

int Driver::compileAndRun(const std::string& file, int maxInstr, bool verbose) {
    try {
        if (verbose) {
            std::cout << "Parsing Loxis v2 source...\n";
        }

        Module mod = Parser::parseFile(file);

        if (verbose) {
            std::cout << "Checking types...\n";
        }

        TypeChecker checker;
        if (!checker.checkModule(mod)) {
            for (const auto& err : checker.getErrors()) {
                std::cerr << "Type error: " << err << "\n";
            }
            return 1;
        }

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
