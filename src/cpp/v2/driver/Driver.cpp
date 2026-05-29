#include "Driver.hpp"
#include "../AST.hpp"
#include "../backend/Lowering.hpp"
#include "../backend/MIRCompiler.hpp"
#include "../frontend/Parser.hpp"
#include "../type/TypeChecker.hpp"
#include "../../vm/VM.hpp"
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

namespace loxis::v2 {

namespace {

namespace fs = std::filesystem;

fs::path resolveModulePath(const Path& module, const fs::path& baseDir) {
    fs::path rel;
    for (const auto& seg : module.segs) {
        rel /= seg;
    }

    fs::path lx = baseDir / rel;
    lx.replace_extension(".lx");
    if (fs::exists(lx)) return fs::canonical(lx);

    fs::path lxs = baseDir / rel;
    lxs.replace_extension(".lxs");
    if (fs::exists(lxs)) return fs::canonical(lxs);

    return {};
}

void appendImportedModules(
    Module& out,
    const fs::path& file,
    std::set<fs::path>& loading,
    std::set<fs::path>& loaded
) {
    fs::path canonical = fs::canonical(file);
    if (loaded.contains(canonical)) return;
    if (loading.contains(canonical)) {
        throw std::runtime_error("cyclic import involving " + canonical.string());
    }

    loading.insert(canonical);
    Module mod = Parser::parseFile(canonical.string());
    fs::path baseDir = canonical.parent_path();

    for (const auto& item : mod.items) {
        if (!item || !std::holds_alternative<ItemImport>(*item)) continue;
        const auto& import = std::get<ItemImport>(*item);
        fs::path imported = resolveModulePath(import.module, baseDir);
        if (imported.empty()) {
            throw std::runtime_error("cannot resolve import '" +
                (import.module.segs.empty() ? std::string{} : import.module.segs.back()) +
                "' from " + canonical.string());
        }
        appendImportedModules(out, imported, loading, loaded);
    }

    for (const auto& item : mod.items) {
        if (!item || std::holds_alternative<ItemImport>(*item)) continue;
        out.items.push_back(item);
    }

    loading.erase(canonical);
    loaded.insert(canonical);
}

Module loadModuleGraph(const std::string& file) {
    fs::path root = fs::canonical(file);
    Module combined;
    combined.name = root.stem().string();
    combined.loc.file = root.string();
    std::set<fs::path> loading;
    std::set<fs::path> loaded;
    appendImportedModules(combined, root, loading, loaded);
    return combined;
}

} // namespace

int Driver::compileAndRun(const std::string& file, int maxInstr, bool verbose) {
    try {
        if (verbose) {
            std::cout << "Parsing Loxis v2 source...\n";
        }

        Module mod = loadModuleGraph(file);

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

        VM vm(std::move(chunk), verbose);
        vm.run(maxInstr);

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace loxis::v2
