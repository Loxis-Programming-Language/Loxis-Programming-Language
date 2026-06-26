#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "../mir/MIR.hpp"
#include "../type/Type.hpp"

namespace loxis::v2 {

using HirExprPtr = std::shared_ptr<struct HirExpr>;
using HirStmtPtr = std::shared_ptr<struct HirStmt>;
using HirPatPtr  = std::shared_ptr<struct HirPat>;

// ============================================================
// Patterns
// ============================================================
struct HirPatWild {};
struct HirPatBind { std::string name; bool mut = false; HirPatPtr sub; };

struct HirPat : std::variant<HirPatWild, HirPatBind> {
    using variant::variant;
};

// ============================================================
// Expressions (typed, resolved — ready for CFG lowering)
// ============================================================
struct HirLit {
    CoreTyPtr ty;
    std::variant<int64_t,uint64_t,double,bool,char,std::string> val;
};
struct HirPath { std::string name; std::vector<CoreTyPtr> typeArgs; CoreTyPtr ty; };
struct HirTuple { std::vector<HirExprPtr> elems; CoreTyPtr ty; };
struct HirArray { std::vector<HirExprPtr> elems; CoreTyPtr ty; };
struct HirFieldInit { std::string name; HirExprPtr expr; };
struct HirStruct {
    std::string typeName;
    std::vector<HirFieldInit> fields;
    std::vector<CoreTyPtr> typeArgs;
    uint32_t totalFieldSize = 0;
    uint32_t typeId = ~0u;
    CoreTyPtr ty;
};
struct HirCall {
    HirExprPtr func;
    std::optional<std::string> staticName;
    std::string ctorLabel;
    std::vector<HirExprPtr> args;
    std::vector<CoreTyPtr> typeArgs;
    CoreTyPtr ty;
};
struct HirMethodCall {
    HirExprPtr recv;
    std::string method;
    std::string qualifiedName;
    std::vector<HirExprPtr> args;
    uint32_t vtableIndex = ~0u;
    bool isInterfaceDispatch = false;
    uint32_t ifaceTypeId = ~0u;
    uint32_t ifaceMethodIndex = 0;
    CoreTyPtr ty;
};
struct HirField { HirExprPtr obj; std::string field; uint32_t fieldOffset = 0; CoreTyPtr ty; };
struct HirIndex { HirExprPtr obj; HirExprPtr idx; CoreTyPtr ty; };
struct HirUnary { MirUnaryOp op; HirExprPtr operand; CoreTyPtr ty; };
struct HirBinary { MirBinOp op; HirExprPtr l, r; CoreTyPtr ty; };
struct HirAssign { MirAssignOp op; HirExprPtr lhs, rhs; CoreTyPtr ty; };
struct HirBlock { std::vector<HirStmtPtr> stmts; HirExprPtr tail; CoreTyPtr ty; };
struct HirIf { HirExprPtr cond; HirExprPtr then_; HirExprPtr else_; CoreTyPtr ty; };
struct HirWhile { HirExprPtr cond; HirExprPtr body; std::optional<std::string> label; CoreTyPtr ty; };
struct HirFor { HirPatPtr pat; HirExprPtr iter; HirExprPtr body; std::optional<std::string> label; CoreTyPtr ty; };
struct HirLoop { HirExprPtr body; std::optional<std::string> label; CoreTyPtr ty; };
struct HirWhenArm {
    std::variant<HirExprPtr, HirPatPtr, std::monostate> condition;
    HirExprPtr body;
    bool isElse = false;
};
struct HirWhen { HirExprPtr scrut; std::vector<HirWhenArm> arms; CoreTyPtr ty; };
struct HirBreak { std::optional<std::string> label; HirExprPtr expr; CoreTyPtr ty; };
struct HirContinue { std::optional<std::string> label; CoreTyPtr ty; };
struct HirReturn { HirExprPtr expr; CoreTyPtr ty; };
struct HirRef { bool mut = false; HirExprPtr expr; CoreTyPtr ty; };
struct HirDeref { HirExprPtr expr; CoreTyPtr ty; };
struct HirTry { HirExprPtr expr; CoreTyPtr ty; };
struct HirCast { HirExprPtr expr; CoreTyPtr targetTy; CoreTyPtr ty; };
struct HirSafeCall { HirExprPtr recv; std::string method; std::vector<HirExprPtr> args; CoreTyPtr ty; };
struct HirSafeField { HirExprPtr recv; std::string field; uint32_t fieldOffset = 0; CoreTyPtr ty; };
struct HirElvis { HirExprPtr lhs; HirExprPtr rhs; CoreTyPtr ty; };
struct HirForceUnwrap { HirExprPtr expr; CoreTyPtr ty; };
struct HirIs { HirExprPtr expr; uint32_t targetTypeId = ~0u; CoreTyPtr ty; };
struct HirIsNot { HirExprPtr expr; uint32_t targetTypeId = ~0u; CoreTyPtr ty; };
struct HirNull { CoreTyPtr ty; };

struct HirExpr : std::variant<
    HirLit, HirPath, HirTuple, HirArray, HirStruct, HirCall, HirMethodCall,
    HirField, HirIndex, HirUnary, HirBinary, HirAssign, HirBlock, HirIf,
    HirWhile, HirFor, HirLoop, HirWhen, HirBreak, HirContinue, HirReturn,
    HirRef, HirDeref, HirTry, HirCast,
    HirSafeCall, HirSafeField, HirElvis, HirForceUnwrap,
    HirIs, HirIsNot, HirNull
> {
    using variant::variant;
};

// ============================================================
// Statements
// ============================================================
struct HirStmtLet { HirPatPtr pat; CoreTyPtr ty; HirExprPtr init; };
struct HirStmtExpr { HirExprPtr expr; };

struct HirStmt : std::variant<HirStmtLet, HirStmtExpr> {
    using variant::variant;
};

// ============================================================
// Items
// ============================================================
struct HirFn {
    std::string name;
    std::vector<std::string> generics;
    std::vector<std::pair<std::string, CoreTyPtr>> params;
    CoreTyPtr ret;
    HirExprPtr body;
};

struct HirCtorParam {
    std::string name;
    CoreTyPtr ty;
    bool isVal = false;
    bool isVar = false;
};

struct HirClass {
    std::string name;
    std::vector<std::string> generics;
    uint32_t typeId = ~0u;
    uint32_t totalFieldSize = 0;
    uint32_t vtableEntryCount = 0;
    std::vector<std::string> vtableMethodNames;
    std::vector<ItableEntry> itables;
    std::vector<HirCtorParam> ctorParams;
    std::vector<HirFn> methods;
};

struct HirItem : std::variant<HirFn, HirClass> {
    using variant::variant;
};

struct HirModule {
    std::vector<HirItem> items;
};

} // namespace