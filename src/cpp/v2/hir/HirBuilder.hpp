#pragma once
#include "../AST.hpp"
#include "HIR.hpp"

namespace loxis::v2 {

class Scope; // fwd

class HirBuilder {
public:
    HirModule build(const Module& mod);
    void setScope(Scope* scope) { m_scope = scope; }

private:
    Scope* m_scope = nullptr;
    std::vector<std::string> m_genericParams;       // stack of active generic param names
    std::vector<size_t> m_genericDepthStack;         // counts per nesting level

    void pushGenerics(const std::vector<GenericParam>& params);
    void popGenerics();

    CoreTyPtr astType(const TypePtr& ty);
    CoreTyPtr literalType(const ExprLit& lit);

    static MirBinOp toMirBinOp(ExprBinary::Op op);
    static MirUnaryOp toMirUnaryOp(ExprUnary::Op op);
    static MirAssignOp toMirAssignOp(ExprAssign::Op op);

    HirExprPtr buildExpr(const ExprPtr& expr);
    HirStmtPtr buildStmt(const StmtPtr& stmt);
    HirPatPtr buildPat(const PatPtr& pat);

    HirFn buildFn(const ItemFun& fn);
    HirClass buildClass(const ItemClass& cls);

    HirExprPtr desugarFor(const ExprFor& e);
    HirExprPtr wrapWithPat(const PatPtr& pat, HirExprPtr elem, HirExprPtr body);
    HirExprPtr makeIntLit(int64_t v);
    HirExprPtr makeBin(MirBinOp op, HirExprPtr l, HirExprPtr r);
    HirExprPtr makeAssign(HirExprPtr lhs, HirExprPtr rhs);
    HirExprPtr makeLet(const std::string& name, HirExprPtr init, HirExprPtr body);
    HirExprPtr makeWhile(HirExprPtr cond, HirExprPtr body, const std::optional<std::string>& label);
    HirExprPtr makeMethodCall(HirExprPtr recv, const std::string& qualifiedName, std::vector<HirExprPtr> args);
    HirExprPtr makeIndex(HirExprPtr obj, HirExprPtr idx);
    HirExprPtr makeBlockWithTail(HirExprPtr first, HirExprPtr tail);
};

} // namespace loxis::v2