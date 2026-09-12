#pragma once
#include <vector>
#include <string>
#include <optional>
#include <unordered_map>
#include "../hir/HIR.hpp"
#include "../mir/MIR.hpp"
#include "../type/Type.hpp"

namespace loxis::v2 {

struct LoweredClass {
    std::string name;
    uint32_t typeId = ~0u;
    uint32_t vtableEntryCount = 0;
    std::vector<std::string> vtableMethodNames;
    std::vector<ItableEntry> itables;
};

struct LoweredGlobalInit {
    uint32_t slot = ~0u;
    std::string initLabel;
};

struct LoweringOutput {
    std::vector<MirBody> bodies;
    std::vector<LoweredClass> classes;
    std::vector<LoweredGlobalInit> globalInitializers;
};

class Lowering {
public:
    LoweringOutput lower(const HirModule& hir);

private:
    struct LoopContext {
        BlockId breakTarget;
        BlockId continueTarget;
        std::optional<std::string> label;
    };

    MirBody* m_body = nullptr;
    std::vector<LoopContext> m_loopStack;
    BlockId m_currentBlock = 0;
    BlockId m_exitBlock = 0;
    bool m_blockTerminated = false;
    std::unordered_map<std::string, LocalId> m_scope;
    std::optional<LocalId> m_closureEnvLocal;
    std::string m_currentClassName;

    LocalId newLocal(CoreTyPtr ty, const std::string& name = "", bool mut = false);
    BlockId newBlock();
    void emit(Statement stmt);
    void term(Terminator t);

    LocalId lowerExpr(const HirExpr& expr);
    void lowerStmt(const HirStmt& stmt);
    MirBody lowerFn(const HirFn& fn);
    MirBody lowerClassCtor(const HirClass& cls);
    MirBody lowerMethod(const HirClass& cls, const HirFn& method);

    LocalId lowerPlace(const HirExpr& expr);
    LocalId unitLocal();
};

} // namespace loxis::v2
