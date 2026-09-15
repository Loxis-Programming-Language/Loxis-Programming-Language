#pragma once
#include "HIR.hpp"
#include <unordered_map>
#include <string>

namespace loxis::v2 {

// Monomorphizer: rewrites a HirModule so that every generic function/class
// instantiation gets its own specialized copy with concrete types substituted.
class Monomorphizer {
public:
    HirModule monomorphize(const HirModule& hir);

private:
    using SubstMap = std::unordered_map<std::string, TyPtr>;

    struct SpecKey {
        std::string defName;
        std::string typeSig;  // mangled type signature
        bool operator==(const SpecKey& o) const = default;
    };
    struct SpecKeyHash {
        size_t operator()(const SpecKey& k) const {
            return std::hash<std::string>{}(k.defName + "|" + k.typeSig);
        }
    };

    // State
    std::unordered_map<SpecKey, std::string, SpecKeyHash> m_specMap;
    std::unordered_map<std::string, const HirFn*> m_fnByName;
    std::unordered_map<std::string, const HirClass*> m_classByName;

    std::vector<SpecKey> m_worklist;
    HirModule m_output;
    uint32_t m_nextTypeId = 1;

    // Walk expressions to find instantiation sites
    void scanExpr(HirExprPtr expr);
    void scanStmt(const HirStmtPtr& stmt);
    void scanFn(const HirFn& fn);

    // Create specialized copies
    std::string monoFn(const HirFn& genericFn, const std::vector<CoreTyPtr>& typeArgs);
    std::string monoClass(const HirClass& genericCls, const std::vector<CoreTyPtr>& typeArgs);
    std::string monoMethod(const HirClass& owner, const std::string& specializedOwner,
                           const std::vector<CoreTyPtr>& classArgs, const HirFn& method,
                           const std::vector<CoreTyPtr>& methodArgs);

    // Type substitution helpers
    void substFn(HirFn& fn, const SubstMap& subst);
    void substClass(HirClass& cls, const SubstMap& subst);
    void substExpr(HirExprPtr& expr, const SubstMap& subst);
    CoreTyPtr substTy(CoreTyPtr ty, const SubstMap& subst);

    // Name mangling
    std::string mangle(const std::string& base, const std::vector<CoreTyPtr>& typeArgs);
    static std::string typeSig(CoreTyPtr ty);
    static std::string typeSigVec(const std::vector<CoreTyPtr>& types);

    // Helpers
    bool hasGenerics(const HirFn& fn) const { return !fn.generics.empty(); }
    bool hasGenerics(const HirClass& cls) const { return !cls.generics.empty(); }
    SubstMap makeSubst(const std::vector<std::string>& paramNames,
                       const std::vector<CoreTyPtr>& typeArgs);
};

} // namespace loxis::v2
