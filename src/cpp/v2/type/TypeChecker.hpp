#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include "../AST.hpp"
#include "Scope.hpp"
#include "Type.hpp"

namespace loxis::v2 {

class TypeChecker {
public:
    bool checkModule(Module& mod);
    const std::vector<Diagnostic>& getErrors() const { return errors; }
    Scope* getScope() const { return rootScope; }
private:
    std::unique_ptr<Scope> rootOwner;
    Scope* rootScope = nullptr;
    uint32_t inferCounter = 0;
    std::vector<Diagnostic> errors;
    SourceLoc currentLoc;
    SourceLoc currentEndLoc;
    TyPtr currentRetTy = nullptr;
    uint32_t closureCounter = 0;
    uint32_t closureEnvTypeCounter = 0;

    struct ClosureContext {
        Scope* scope = nullptr;
        ExprClosure* expr = nullptr;
        std::vector<TyPtr> returns;
    };
    std::vector<ClosureContext> closureScopes;

    struct LoopCtx {
        TyPtr expected;
        std::optional<std::string> label;
    };
    std::vector<LoopCtx> loopStack;

    TyPtr freshInfer();
    void error(const std::string& msg);
    void error(const std::string& msg, std::string primaryLabel, std::string help);

    TyPtr checkExpr(ExprPtr e, Scope* sc, TyPtr expectedTy = nullptr);
    bool inferCall(TyFn& signature, const std::vector<std::string>& names,
                   const std::vector<ExprPtr>& args, Scope* sc,
                   std::vector<TyPtr>& typeArgs, TyPtr expectedTy = nullptr,
                   size_t offset = 0);
    bool arrangeArguments(const std::vector<ExprPtr>& args,
                          const std::vector<std::optional<std::string>>& argNames,
                          const std::vector<std::string>& paramNames,
                          std::optional<size_t> varargIndex,
                          std::vector<ExprPtr>& arranged, bool diagnose = false);
    TyPtr checkClosure(ExprClosure& n, Scope* sc, TyPtr expectedFn = nullptr);
    void checkStmt(StmtPtr s, Scope* sc);
    void checkItem(ItemPtr i, Scope* sc);

    TyPtr resolveType(const Type& surfaceType, Scope* sc);
    TyPtr resolvePathType(const Path& path, Scope* sc);

    TyPtr checkPat(PatPtr p, TyPtr expected, Scope* sc);

    bool isNumeric(TyPtr t);
    bool isInteger(TyPtr t);
    bool isBool(TyPtr t);
    bool sameType(TyPtr a, TyPtr b);
    bool isSubtype(TyPtr a, TyPtr b);
    bool isPrimitiveCastable(TyPtr from, TyPtr to);
    bool isMutablePlace(ExprPtr e, Scope* sc);

    // Class/interface helpers
    const MethodInfo* lookupMethod(TyPtr recv, const std::string& name, size_t arity);
    const FieldInfo*  lookupField(TyPtr recv, const std::string& name);
    TyPtr unwrapNullable(TyPtr t);

    // Smart casting / flow typing helpers
    struct TypeRefinement {
        std::string varName;
        TyPtr refinedType;
    };
    std::vector<TypeRefinement> extractRefinements(const Expr& cond, Scope* sc, bool& isInvert);
    void applyRefinements(Scope* target, const std::vector<TypeRefinement>& refs);
};

} // namespace
