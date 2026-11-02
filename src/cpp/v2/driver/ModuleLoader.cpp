#include "ModuleLoader.hpp"
#include "../frontend/Parser.hpp"
#include <cstdlib>
#include <set>
#include <stdexcept>

namespace loxis::v2 {

namespace fs = std::filesystem;

namespace {

fs::path tryLxFile(const fs::path& base) {
    fs::path lx = base;
    lx.replace_extension(".lx");
    if (fs::exists(lx)) return fs::absolute(lx).lexically_normal();

    fs::path lxs = base;
    lxs.replace_extension(".lxs");
    if (fs::exists(lxs)) return fs::absolute(lxs).lexically_normal();

    // Directory index files: pkg/mod.lx, pkg/package.lx, pkg/index.lx
    if (fs::is_directory(base)) {
        for (const auto& entry : {"mod.lx", "package.lx", "index.lx"}) {
            fs::path candidate = base / entry;
            if (fs::exists(candidate)) return fs::absolute(candidate).lexically_normal();
        }
    }

    return {};
}

fs::path pathFromSegments(const fs::path& root, const Path& module) {
    fs::path rel;
    for (const auto& seg : module.segs) {
        rel /= seg;
    }
    return root / rel;
}

} // namespace

ModuleLoader::ModuleLoader(fs::path entryFile, fs::path stdlibRoot)
    : m_entryFile(fs::absolute(std::move(entryFile)).lexically_normal())
    , m_entryDir(m_entryFile.parent_path())
    , m_configuredStdlibRoot(std::move(stdlibRoot))
    , m_stdlibRoot(findStdlibRoot())
{}

fs::path ModuleLoader::findStdlibRoot() const {
    if (!m_configuredStdlibRoot.empty() &&
        fs::exists(m_configuredStdlibRoot / "loxis" / "prelude.lx")) {
        return fs::absolute(m_configuredStdlibRoot).lexically_normal();
    }

    if (const char* env = std::getenv("LOXIS_STDLIB")) {
        fs::path fromEnv(env);
        if (fs::exists(fromEnv / "loxis" / "prelude.lx")) {
            return fs::absolute(fromEnv).lexically_normal();
        }
    }

    fs::path dir = m_entryDir;
    for (int depth = 0; depth < 24 && !dir.empty(); ++depth) {
        fs::path candidate = dir / "stdlib";
        if (fs::exists(candidate / "loxis" / "prelude.lx")) {
            return fs::absolute(candidate).lexically_normal();
        }
        dir = dir.parent_path();
    }

    fs::path cwd = fs::current_path();
    if (fs::exists(cwd / "stdlib" / "loxis" / "prelude.lx")) {
        return fs::absolute(cwd / "stdlib").lexically_normal();
    }

    return {};
}

fs::path ModuleLoader::resolveStdlibModule(const Path& module) const {
    if (m_stdlibRoot.empty() || module.segs.empty()) return {};
    if (module.segs[0] != "loxis") return {};

    if (module.segs.size() == 1) {
        return tryLxFile(m_stdlibRoot / "loxis" / "prelude");
    }

    fs::path rel;
    for (size_t i = 1; i < module.segs.size(); ++i) {
        rel /= module.segs[i];
    }
    return tryLxFile(m_stdlibRoot / "loxis" / rel);
}

fs::path ModuleLoader::resolveSourceModule(
    const Path& module,
    const fs::path& nearDir) const
{
    if (module.segs.empty()) return {};

    fs::path rel;
    for (const auto& seg : module.segs) {
        rel /= seg;
    }

    fs::path dir = nearDir;
    for (int depth = 0; depth < 24 && !dir.empty(); ++depth) {
        if (auto found = tryLxFile(dir / rel); !found.empty()) {
            return found;
        }
        dir = dir.parent_path();
    }

    dir = m_entryDir;
    for (int depth = 0; depth < 24 && !dir.empty(); ++depth) {
        if (auto found = tryLxFile(dir / rel); !found.empty()) {
            return found;
        }
        dir = dir.parent_path();
    }

    return {};
}

fs::path ModuleLoader::resolveModulePath(
    const Path& module,
    const fs::path& importingDir) const
{
    if (module.segs.empty()) return {};

    if (auto stdlib = resolveStdlibModule(module); !stdlib.empty()) {
        return stdlib;
    }

    if (auto local = resolveSourceModule(module, importingDir); !local.empty()) {
        return local;
    }

    return tryLxFile(pathFromSegments(importingDir, module));
}

bool ModuleLoader::isExported(const Item& item) const {
    return std::visit([&](const auto& n) -> bool {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ItemFun> ||
                      std::is_same_v<T, ItemClass> ||
                      std::is_same_v<T, ItemEnumClass> ||
                      std::is_same_v<T, ItemInterface> ||
                      std::is_same_v<T, ItemObject> ||
                      std::is_same_v<T, ItemVal> ||
                      std::is_same_v<T, ItemVar> ||
                      std::is_same_v<T, ItemConst> ||
                      std::is_same_v<T, ItemTypeAlias>) {
            return n.visibility != Visibility::Private;
        } else {
            return false;
        }
    }, item);
}

bool ModuleLoader::matchesImportList(const Item& item, const ItemImport& import) const {
    if (import.importModule || import.items.empty()) return true;
    return std::visit([&](const auto& n) -> bool {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ItemFun> ||
                      std::is_same_v<T, ItemClass> ||
                      std::is_same_v<T, ItemEnumClass> ||
                      std::is_same_v<T, ItemInterface> ||
                      std::is_same_v<T, ItemObject> ||
                      std::is_same_v<T, ItemVal> ||
                      std::is_same_v<T, ItemVar> ||
                      std::is_same_v<T, ItemConst> ||
                      std::is_same_v<T, ItemTypeAlias>) {
            for (const auto& imported : import.items) {
                if (imported.name == n.name) return true;
            }
        }
        return false;
    }, item);
}

void ModuleLoader::appendModule(
    Module& out,
    const fs::path& file,
    std::set<fs::path>& loading,
    std::set<fs::path>& loaded,
    const ItemImport* importedBy)
{
    fs::path canonical = fs::absolute(file).lexically_normal();
    if (loaded.contains(canonical)) return;
    if (loading.contains(canonical)) {
        throw std::runtime_error("cyclic import involving " + canonical.string());
    }

    loading.insert(canonical);
    Module mod = Parser::parseFile(canonical.string());
    const auto relative = canonical.lexically_relative(m_stdlibRoot);
    bool intrinsicSource = !m_stdlibRoot.empty() && !relative.empty() &&
        *relative.begin() != "..";
    for (const auto& item : mod.items) {
        if (auto* import = std::get_if<ItemImport>(item.get()); import &&
            import->module.segs == std::vector<std::string>{"loxis", "unsafe"}) intrinsicSource = true;
    }
    if (intrinsicSource) out.intrinsicSources.push_back(canonical.string());
    fs::path baseDir = canonical.parent_path();

    for (const auto& item : mod.items) {
        if (!item || !std::holds_alternative<ItemImport>(*item)) continue;
        const auto& import = std::get<ItemImport>(*item);
        fs::path imported = resolveModulePath(import.module, baseDir);
        if (imported.empty()) {
            std::string name = import.module.segs.empty()
                ? std::string{}
                : import.module.segs.back();
            throw std::runtime_error(
                "cannot resolve import '" + name + "' from " + canonical.string());
        }
        appendModule(out, imported, loading, loaded, &import);
    }

    for (const auto& item : mod.items) {
        if (!item || std::holds_alternative<ItemImport>(*item)) continue;
        if (importedBy) {
            if (!isExported(*item)) continue;
            if (!matchesImportList(*item, *importedBy)) continue;
        }
        out.items.push_back(item);
    }

    loading.erase(canonical);
    loaded.insert(canonical);
    m_loadOrder.push_back(canonical);
    if (m_trace) m_trace->module(canonical.string());
}

Module ModuleLoader::load() {
    Module combined;
    combined.name = m_entryFile.stem().string();
    combined.loc.file = m_entryFile.string();

    std::set<fs::path> loading;
    std::set<fs::path> loaded;

    if (!m_stdlibRoot.empty()) {
        fs::path prelude = m_stdlibRoot / "loxis" / "prelude.lx";
        if (fs::exists(prelude)) {
            appendModule(combined, prelude, loading, loaded, nullptr);
        }
    }

    appendModule(combined, m_entryFile, loading, loaded, nullptr);
    return combined;
}

} // namespace loxis::v2
