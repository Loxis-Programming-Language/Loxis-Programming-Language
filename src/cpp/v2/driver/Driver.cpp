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
        out = compiler.compile(output);
        return 0;
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
        vm.run(opts.maxInstr);

        if (opts.gcStats) {
            vm.finishGC();
            vm.dumpGCStats(std::cerr);
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace loxis::v2
