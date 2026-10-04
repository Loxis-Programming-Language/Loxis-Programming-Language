#pragma once
#include "../AST.hpp"
#include "../dump/Trace.hpp"
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace loxis::v2 {

class ModuleLoader {
public:
    explicit ModuleLoader(std::filesystem::path entryFile);

    Module load();

    // --trace: record every module appended, in dependency order.
    void setTrace(const Trace* trace) { m_trace = trace; }
    const std::vector<std::filesystem::path>& loadOrder() const { return m_loadOrder; }

private:
    std::filesystem::path m_entryFile;
    std::filesystem::path m_entryDir;
    std::filesystem::path m_stdlibRoot;
    const Trace* m_trace = nullptr;
    std::vector<std::filesystem::path> m_loadOrder;

    std::filesystem::path findStdlibRoot() const;
    std::filesystem::path resolveStdlibModule(const Path& module) const;
    std::filesystem::path resolveSourceModule(
        const Path& module,
        const std::filesystem::path& nearDir) const;
    std::filesystem::path resolveModulePath(
        const Path& module,
        const std::filesystem::path& importingDir) const;

    bool isExported(const Item& item) const;
    bool matchesImportList(const Item& item, const ItemImport& import) const;

    void appendModule(
        Module& out,
        const std::filesystem::path& file,
        std::set<std::filesystem::path>& loading,
        std::set<std::filesystem::path>& loaded,
        const ItemImport* importedBy = nullptr);
};

} // namespace loxis::v2