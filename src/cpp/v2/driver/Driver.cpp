#include "Driver.hpp"
#include "ModuleLoader.hpp"
#include "../backend/Lowering.hpp"
#include "../backend/MIRCompiler.hpp"
#include "../hir/HirBuilder.hpp"
#include "../hir/Monomorphizer.hpp"
#include "../type/TypeChecker.hpp"
#include "../../vm/VM.hpp"
#include <iostream>

namespace loxis::v2 {

int Driver::compileAndRun(const std::string& file, int maxInstr, bool verbose) {
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
                std::cerr << "Type error: " << err << "\n";
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
        Chunk chunk = compiler.compile(output);

        if (verbose) {
            std::cout << "Running VM (max instructions: " << maxInstr << ")...\n";
        }

        VM vm(std::move(chunk), verbose);
        vm.run(maxInstr);

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace loxis::v2