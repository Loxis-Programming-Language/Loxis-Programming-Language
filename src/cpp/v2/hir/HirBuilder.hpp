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
    std::vector<HirFn> m_generatedFns;
    uint32_t m_literalCounter = 0;

    void pushGenerics(const std::vector<GenericParam>& params);
    void popGenerics();

    CoreTyPtr astType(const TypePtr& ty);
    CoreTyPtr literalType(const ExprLit& lit);

    static MirBinOp toMirBinOp(ExprBinary::Op op);
    static MirUnaryOp toMirUnaryOp(ExprUnary::Op op);
    static MirAssignOp toMirAssignOp(ExprAssign::Op op);
    static std::string binaryOperatorName(ExprBinary::Op op);
    static std::string unaryOperatorName(ExprUnary::Op op);

    HirExprPtr buildExpr(const ExprPtr& expr);
    HirStmtPtr buildStmt(const StmtPtr& stmt);
    HirPatPtr buildPat(const PatPtr& pat);

    HirFn buildFn(const ItemFun& fn);
    HirClass buildClass(const ItemClass& cls);
    HirClass buildEnumClass(const ItemEnumClass& cls);
    HirClass buildObject(const ItemObject& obj);
    HirFn buildInterfaceMethod(const std::string& ifaceName, const InterfaceMethodDecl& method, const std::vector<GenericParam>& generics);

    HirExprPtr desugarFor(const ExprFor& e);
    HirExprPtr wrapWithPat(const PatPtr& pat, HirExprPtr elem, HirExprPtr body);
    HirExprPtr makeIntLit(int64_t v);
    HirExprPtr makeBin(MirBinOp op, HirExprPtr l, HirExprPtr r);
    HirExprPtr makeAssign(HirExprPtr lhs, HirExprPtr rhs);
    HirExprPtr makeLet(const std::string& name, HirExprPtr init, HirExprPtr body, CoreTyPtr ty = nullptr);
    HirExprPtr makeWhile(HirExprPtr cond, HirExprPtr body, const std::optional<std::string>& label);
    HirExprPtr makeMethodCall(HirExprPtr recv, const std::string& qualifiedName, std::vector<HirExprPtr> args);
    HirExprPtr makeIndex(HirExprPtr obj, HirExprPtr idx);
    HirExprPtr makeBlockWithTail(HirExprPtr first, HirExprPtr tail);
    HirExprPtr makeSeqBlock(std::vector<HirExprPtr> stmts, HirExprPtr tail);
    HirExprPtr makeBreakIf(HirExprPtr cond);
    // Builds one rotated `for` body: step the index at the top, leave once the
    // index has passed the end, then run the user's body with the pattern bound.
    HirExprPtr makeForLoopBody(HirExprPtr idxPath, HirExprPtr incIdx, HirExprPtr exitCond,
                               const PatPtr& pat, HirExprPtr elem, HirExprPtr body,
                               const std::optional<std::string>& label);
};

} // namespace loxis::v2
