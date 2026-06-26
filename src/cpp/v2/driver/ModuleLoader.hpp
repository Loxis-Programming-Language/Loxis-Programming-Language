#pragma once
#include "../AST.hpp"
#include <filesystem>
#include <set>
#include <string>

namespace loxis::v2 {

class ModuleLoader {
public:
    explicit ModuleLoader(std::filesystem::path entryFile);

    Module load();

private:
    std::filesystem::path m_entryFile;
    std::filesystem::path m_entryDir;
    std::filesystem::path m_stdlibRoot;

    std::filesystem::path findStdlibRoot() const;
    std::filesystem::path resolveStdlibModule(const Path& module) const;
    std::filesystem::path resolveSourceModule(
        const Path& module,
        const std::filesystem::path& nearDir) const;
    std::filesystem::path resolveModulePath(
        const Path& module,
        const std::filesystem::path& importingDir) const;

    void appendModule(
        Module& out,
        const std::filesystem::path& file,
        std::set<std::filesystem::path>& loading,
        std::set<std::filesystem::path>& loaded);
};

} // namespace loxis::v2