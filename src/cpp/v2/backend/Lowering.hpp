#pragma once
#include <vector>
#include <string>
#include <optional>
#include <unordered_map>
#include "../AST.hpp"
#include "../mir/MIR.hpp"
#include "../type/Type.hpp"

namespace loxis::v2 {

class Lowering {
public:
    std::vector<MirBody> lowerModule(const Module& mod);

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

    LocalId newLocal(CoreTyPtr ty, const std::string& name = "", bool mut = false);
    BlockId newBlock();
    void emit(Statement stmt);
    void term(Terminator t);

    LocalId lowerExpr(const Expr& expr);
    void lowerStmt(const Stmt& stmt);
    MirBody lowerFn(const ItemFun& fn);

    LocalId lowerPlace(const Expr& expr);
    LocalId unitLocal();
    CoreTyPtr lowerType(const TypePtr& ty);
    CoreTyPtr literalType(const ExprLit& lit);
};

} // namespace loxis::v2
