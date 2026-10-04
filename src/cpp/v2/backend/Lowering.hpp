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
    uint32_t parentTypeId = ~0u;
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
        size_t finallyDepth = 0;
    };

    MirBody* m_body = nullptr;
    std::vector<LoopContext> m_loopStack;
    std::vector<HirExprPtr> m_finallyStack;
    std::vector<bool> m_finallyHandlerActive;
    BlockId m_currentBlock = 0;
    BlockId m_exitBlock = 0;
    bool m_blockTerminated = false;
    std::unordered_map<std::string, LocalId> m_scope;
    std::optional<LocalId> m_closureEnvLocal;
    std::string m_currentClassName;
    SourceLocation m_currentLoc;

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
    // Throwaway destination for statements that produce no value, such as a
    // global store. Allocated at most once per body, so a function with many
    // global assignments does not burn one of the 256 registers on each.
    std::optional<LocalId> m_scratchLocal;
    LocalId scratchLocal();
    bool lowerFinalizersTo(size_t depth);
};

} // namespace loxis::v2
