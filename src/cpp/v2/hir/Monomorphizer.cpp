#include "Monomorphizer.hpp"
#include "../type/TypeSubst.hpp"
#include <sstream>

namespace loxis::v2 {

// ============================================================
// Name mangling
// ============================================================

std::string Monomorphizer::typeSig(CoreTyPtr ty) {
    if (!ty) return "?";
    switch (ty->kind()) {
        case TyKind::Int: return "i";
        case TyKind::Long: return "l";
        case TyKind::Float: return "f";
        case TyKind::Double: return "d";
        case TyKind::Bool: return "b";
        case TyKind::Char: return "c";
        case TyKind::Str: return "s";
        case TyKind::Unit: return "u";
        case TyKind::Never: return "n";
        case TyKind::Null: return "0";
        case TyKind::Class_: {
            auto& c = std::get<TyClass>(*ty);
            std::string sig = c.def->name;
            for (const auto& a : c.args) sig += "$" + typeSig(a);
            return sig;
        }
        case TyKind::Interface_: {
            auto& iface = std::get<TyInterface>(*ty);
            std::string sig = iface.def->name;
            for (const auto& a : iface.args) sig += "$" + typeSig(a);
            return sig;
        }
        case TyKind::Param: return std::get<TyParam>(*ty).name;
        default: return "x";
    }
}

std::string Monomorphizer::mangle(const std::string& base,
                                   const std::vector<CoreTyPtr>& typeArgs) {
    std::string result = base;
    for (const auto& ta : typeArgs) {
        result += "$" + typeSig(ta);
    }
    return result;
}

std::string Monomorphizer::typeSigVec(const std::vector<CoreTyPtr>& types) {
    std::string result;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i > 0) result += "_";
        result += typeSig(types[i]);
    }
    return result;
}

Monomorphizer::SubstMap Monomorphizer::makeSubst(
    const std::vector<std::string>& paramNames,
    const std::vector<CoreTyPtr>& typeArgs)
{
    SubstMap subst;
    for (size_t i = 0; i < paramNames.size() && i < typeArgs.size(); ++i) {
        subst[paramNames[i]] = typeArgs[i];
    }
    return subst;
}

// ============================================================
// Type substitution
// ============================================================

CoreTyPtr Monomorphizer::substTy(CoreTyPtr ty, const SubstMap& subst) {
    if (!ty) return nullptr;
    if (ty->kind() == TyKind::Param) {
        auto& param = std::get<TyParam>(*ty);
        auto it = subst.find(param.name);
        if (it != subst.end()) return it->second;
    }
    // For composite types with args (TyClass, TyInterface), substitute args
    if (ty->kind() == TyKind::Class_) {
        auto& c = std::get<TyClass>(*ty);
        std::vector<CoreTyPtr> newArgs;
        for (const auto& a : c.args) newArgs.push_back(substTy(a, subst));
        return std::make_shared<Ty>(TyClass{c.def, std::move(newArgs)});
    }
    if (ty->kind() == TyKind::Interface_) {
        auto& iface = std::get<TyInterface>(*ty);
        std::vector<CoreTyPtr> newArgs;
        for (const auto& a : iface.args) newArgs.push_back(substTy(a, subst));
        return std::make_shared<Ty>(TyInterface{iface.def, std::move(newArgs)});
    }
    if (ty->kind() == TyKind::Fn) {
        auto& fn = std::get<TyFn>(*ty);
        std::vector<CoreTyPtr> newParams;
        for (const auto& p : fn.params) newParams.push_back(substTy(p, subst));
        return std::make_shared<Ty>(TyFn{std::move(newParams), substTy(fn.ret, subst)});
    }
    if (ty->kind() == TyKind::Ref) {
        auto& r = std::get<TyRef>(*ty);
        return std::make_shared<Ty>(TyRef{r.mut, substTy(r.elem, subst)});
    }
    if (ty->kind() == TyKind::Array) {
        auto& a = std::get<TyArray>(*ty);
        return std::make_shared<Ty>(TyArray{substTy(a.elem, subst), a.len});
    }
    if (ty->kind() == TyKind::Slice) {
        auto& s = std::get<TySlice>(*ty);
        return std::make_shared<Ty>(TySlice{substTy(s.elem, subst)});
    }
    if (ty->kind() == TyKind::Tuple) {
        auto& tup = std::get<TyTuple>(*ty);
        std::vector<CoreTyPtr> newElems;
        for (const auto& e : tup.elems) newElems.push_back(substTy(e, subst));
        return std::make_shared<Ty>(TyTuple{std::move(newElems)});
    }
    return ty;
}

// ============================================================
// Expression / statement / function substitution
// ============================================================

void Monomorphizer::substExpr(HirExprPtr& expr, const SubstMap& subst) {
    if (!expr) return;
    std::visit([&](auto& e) {
        using T = std::decay_t<decltype(e)>;
        e.ty = substTy(e.ty, subst);

        if constexpr (std::is_same_v<T, HirPath>) {
            for (auto& ta : e.typeArgs) ta = substTy(ta, subst);
        }
        else if constexpr (std::is_same_v<T, HirCall>) {
            for (auto& ta : e.typeArgs) ta = substTy(ta, subst);
            for (auto& a : e.args) substExpr(a, subst);
            substExpr(e.func, subst);
        }
        else if constexpr (std::is_same_v<T, HirStruct>) {
            for (auto& ta : e.typeArgs) ta = substTy(ta, subst);
            for (auto& f : e.fields) substExpr(f.expr, subst);
        }
        else if constexpr (std::is_same_v<T, HirTuple>) {
            for (auto& elem : e.elems) substExpr(elem, subst);
        }
        else if constexpr (std::is_same_v<T, HirArray>) {
            for (auto& elem : e.elems) substExpr(elem, subst);
        }
        else if constexpr (std::is_same_v<T, HirField>) {
            substExpr(e.obj, subst);
        }
        else if constexpr (std::is_same_v<T, HirIndex>) {
            substExpr(e.obj, subst);
            substExpr(e.idx, subst);
        }
        else if constexpr (std::is_same_v<T, HirUnary>) {
            substExpr(e.operand, subst);
        }
        else if constexpr (std::is_same_v<T, HirBinary>) {
            substExpr(e.l, subst);
            substExpr(e.r, subst);
        }
        else if constexpr (std::is_same_v<T, HirAssign>) {
            substExpr(e.lhs, subst);
            substExpr(e.rhs, subst);
        }
        else if constexpr (std::is_same_v<T, HirMethodCall>) {
            substExpr(e.recv, subst);
            for (auto& a : e.args) substExpr(a, subst);
        }
        else if constexpr (std::is_same_v<T, HirBlock>) {
            for (auto& s : e.stmts) scanStmt(s);  // Use scanStmt for recursion
            if (e.tail) substExpr(e.tail, subst);
        }
        else if constexpr (std::is_same_v<T, HirIf>) {
            substExpr(e.cond, subst);
            substExpr(e.then_, subst);
            if (e.else_) substExpr(e.else_, subst);
        }
        else if constexpr (std::is_same_v<T, HirWhile>) {
            substExpr(e.cond, subst);
            substExpr(e.body, subst);
        }
        else if constexpr (std::is_same_v<T, HirFor>) {
            substExpr(e.iter, subst);
            substExpr(e.body, subst);
        }
        else if constexpr (std::is_same_v<T, HirLoop>) {
            substExpr(e.body, subst);
        }
        else if constexpr (std::is_same_v<T, HirWhen>) {
            if (e.scrut) substExpr(e.scrut, subst);
            for (auto& arm : e.arms) substExpr(arm.body, subst);
        }
        else if constexpr (std::is_same_v<T, HirBreak>) {
            if (e.expr) substExpr(e.expr, subst);
        }
        else if constexpr (std::is_same_v<T, HirReturn>) {
            substExpr(e.expr, subst);
        }
        else if constexpr (std::is_same_v<T, HirRef>) {
            substExpr(e.expr, subst);
        }
        else if constexpr (std::is_same_v<T, HirDeref>) {
            substExpr(e.expr, subst);
        }
        else if constexpr (std::is_same_v<T, HirTry>) {
            substExpr(e.expr, subst);
        }
        else if constexpr (std::is_same_v<T, HirCast>) {
            substExpr(e.expr, subst);
            e.targetTy = substTy(e.targetTy, subst);
        }
        else if constexpr (std::is_same_v<T, HirSafeCall>) {
            substExpr(e.recv, subst);
            for (auto& a : e.args) substExpr(a, subst);
        }
        else if constexpr (std::is_same_v<T, HirSafeField>) {
            substExpr(e.recv, subst);
        }
        else if constexpr (std::is_same_v<T, HirElvis>) {
            substExpr(e.lhs, subst);
            substExpr(e.rhs, subst);
        }
        else if constexpr (std::is_same_v<T, HirForceUnwrap>) {
            substExpr(e.expr, subst);
        }
        else if constexpr (std::is_same_v<T, HirIs>) {
            substExpr(e.expr, subst);
        }
        else if constexpr (std::is_same_v<T, HirIsNot>) {
            substExpr(e.expr, subst);
        }
    }, *expr);
}

void Monomorphizer::substFn(HirFn& fn, const SubstMap& subst) {
    fn.ret = substTy(fn.ret, subst);
    for (auto& p : fn.params) {
        p.second = substTy(p.second, subst);
    }
    if (fn.body) substExpr(fn.body, subst);
}

void Monomorphizer::substClass(HirClass& cls, const SubstMap& subst) {
    for (auto& cp : cls.ctorParams) {
        cp.ty = substTy(cp.ty, subst);
    }
    for (auto& m : cls.methods) {
        substFn(m, subst);
    }
}

// ============================================================
// Scanning for instantiation sites
// ============================================================

void Monomorphizer::scanStmt(const HirStmtPtr& stmt) {
    if (!stmt) return;
    std::visit([&](auto& s) {
        using ST = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<ST, HirStmtLet>) {
            if (s.init) scanExpr(s.init);
        } else if constexpr (std::is_same_v<ST, HirStmtExpr>) {
            if (s.expr) scanExpr(s.expr);
        }
    }, *stmt);
}

void Monomorphizer::scanExpr(HirExprPtr expr) {
    if (!expr) return;
    std::visit([&](auto& e) {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, HirCall>) {
            if (!e.typeArgs.empty()) {
                std::string fnName;
                if (e.staticName) fnName = *e.staticName;
                else if (auto* path = std::get_if<HirPath>(e.func.get())) {
                    fnName = path->name;
                }

                // Check if this calls a generic function
                auto fit = m_fnByName.find(fnName);
                if (fit != m_fnByName.end() && hasGenerics(*fit->second)) {
                    std::string mangled = monoFn(*fit->second, e.typeArgs);
                    e.staticName = mangled;
                }

                // Check if this is a constructor call to a generic class
                if (!e.ctorLabel.empty()) {
                    // ctorLabel is like "Box.init" — extract class name
                    std::string className = e.ctorLabel;
                    auto dotPos = className.rfind('.');
                    if (dotPos != std::string::npos) {
                        className = className.substr(0, dotPos);
                    }
                    auto cit = m_classByName.find(className);
                    if (cit != m_classByName.end() && hasGenerics(*cit->second)) {
                        std::string mangled = monoClass(*cit->second, e.typeArgs);
                        e.ctorLabel = mangled + ".init";
                    }
                }
            }
            for (auto& a : e.args) scanExpr(a);
            scanExpr(e.func);
        }
        else if constexpr (std::is_same_v<T, HirStruct>) {
            if (!e.typeArgs.empty()) {
                auto it = m_classByName.find(e.typeName);
                if (it != m_classByName.end() && hasGenerics(*it->second)) {
                    std::string mangled = monoClass(*it->second, e.typeArgs);
                    e.typeName = mangled;
                    e.typeArgs.clear();
                }
            }
            for (auto& f : e.fields) scanExpr(f.expr);
        }
        else if constexpr (std::is_same_v<T, HirMethodCall>) {
            scanExpr(e.recv);
            for (auto& a : e.args) scanExpr(a);
        }
        else if constexpr (std::is_same_v<T, HirTuple>) {
            for (auto& elem : e.elems) scanExpr(elem);
        }
        else if constexpr (std::is_same_v<T, HirArray>) {
            for (auto& elem : e.elems) scanExpr(elem);
        }
        else if constexpr (std::is_same_v<T, HirField>) {
            scanExpr(e.obj);
        }
        else if constexpr (std::is_same_v<T, HirIndex>) {
            scanExpr(e.obj); scanExpr(e.idx);
        }
        else if constexpr (std::is_same_v<T, HirUnary>) {
            scanExpr(e.operand);
        }
        else if constexpr (std::is_same_v<T, HirBinary>) {
            scanExpr(e.l); scanExpr(e.r);
        }
        else if constexpr (std::is_same_v<T, HirAssign>) {
            scanExpr(e.lhs); scanExpr(e.rhs);
        }
        else if constexpr (std::is_same_v<T, HirBlock>) {
            for (auto& s : e.stmts) scanStmt(s);
            if (e.tail) scanExpr(e.tail);
        }
        else if constexpr (std::is_same_v<T, HirIf>) {
            scanExpr(e.cond); scanExpr(e.then_);
            if (e.else_) scanExpr(e.else_);
        }
        else if constexpr (std::is_same_v<T, HirWhile>) {
            scanExpr(e.cond); scanExpr(e.body);
        }
        else if constexpr (std::is_same_v<T, HirFor>) {
            scanExpr(e.iter); scanExpr(e.body);
        }
        else if constexpr (std::is_same_v<T, HirLoop>) {
            scanExpr(e.body);
        }
        else if constexpr (std::is_same_v<T, HirWhen>) {
            if (e.scrut) scanExpr(e.scrut);
            for (auto& arm : e.arms) scanExpr(arm.body);
        }
        else if constexpr (std::is_same_v<T, HirBreak>) {
            if (e.expr) scanExpr(e.expr);
        }
        else if constexpr (std::is_same_v<T, HirReturn>) {
            scanExpr(e.expr);
        }
        else if constexpr (std::is_same_v<T, HirRef>) {
            scanExpr(e.expr);
        }
        else if constexpr (std::is_same_v<T, HirDeref>) {
            scanExpr(e.expr);
        }
        else if constexpr (std::is_same_v<T, HirTry>) {
            scanExpr(e.expr);
        }
        else if constexpr (std::is_same_v<T, HirCast>) {
            scanExpr(e.expr);
        }
        else if constexpr (std::is_same_v<T, HirSafeCall>) {
            scanExpr(e.recv);
            for (auto& a : e.args) scanExpr(a);
        }
        else if constexpr (std::is_same_v<T, HirSafeField>) {
            scanExpr(e.recv);
        }
        else if constexpr (std::is_same_v<T, HirElvis>) {
            scanExpr(e.lhs); scanExpr(e.rhs);
        }
        else if constexpr (std::is_same_v<T, HirForceUnwrap>) {
            scanExpr(e.expr);
        }
        else if constexpr (std::is_same_v<T, HirIs>) {
            scanExpr(e.expr);
        }
        else if constexpr (std::is_same_v<T, HirIsNot>) {
            scanExpr(e.expr);
        }
    }, *expr);
}

void Monomorphizer::scanFn(const HirFn& fn) {
    if (fn.body) scanExpr(fn.body);
}

// ============================================================
// Monomorphization
// ============================================================

std::string Monomorphizer::monoFn(const HirFn& genericFn,
                                   const std::vector<CoreTyPtr>& typeArgs) {
    std::string mangled = mangle(genericFn.name, typeArgs);

    SpecKey key{genericFn.name, typeSigVec(typeArgs)};
    auto it = m_specMap.find(key);
    if (it != m_specMap.end()) return it->second;

    m_specMap[key] = mangled;

    // Build substitution map
    SubstMap subst = makeSubst(genericFn.generics, typeArgs);

    // Clone and substitute
    HirFn specialized = genericFn;
    specialized.name = mangled;
    specialized.generics.clear();
    substFn(specialized, subst);

    // Add to worklist for recursive scanning
    m_worklist.push_back(key);

    m_output.items.push_back(specialized);
    return mangled;
}

std::string Monomorphizer::monoClass(const HirClass& genericCls,
                                      const std::vector<CoreTyPtr>& typeArgs) {
    std::string mangled = mangle(genericCls.name, typeArgs);

    SpecKey key{genericCls.name, typeSigVec(typeArgs)};
    auto it = m_specMap.find(key);
    if (it != m_specMap.end()) return it->second;

    m_specMap[key] = mangled;

    SubstMap subst = makeSubst(genericCls.generics, typeArgs);

    HirClass specialized = genericCls;
    specialized.name = mangled;
    specialized.generics.clear();
    specialized.typeId = m_nextTypeId++;
    substClass(specialized, subst);

    // Also monomorphize the constructor
    if (!specialized.ctorParams.empty()) {
        // Methods are already subst'd by substClass
    }

    m_worklist.push_back(key);
    m_output.items.push_back(specialized);
    return mangled;
}

// ============================================================
// Main entry point
// ============================================================

HirModule Monomorphizer::monomorphize(const HirModule& hir) {
    m_specMap.clear();
    m_fnByName.clear();
    m_classByName.clear();
    m_worklist.clear();
    m_output.items.clear();

    // Compute starting typeId for specializations
    m_nextTypeId = 1;
    for (const auto& item : hir.items) {
        std::visit([&](const auto& i) {
            using T = std::decay_t<decltype(i)>;
            if constexpr (std::is_same_v<T, HirClass>) {
                if (i.typeId != ~0u && i.typeId >= m_nextTypeId)
                    m_nextTypeId = i.typeId + 1;
            }
        }, item);
    }

    // Index all items by name
    for (const auto& item : hir.items) {
        std::visit([&](const auto& i) {
            using T = std::decay_t<decltype(i)>;
            if constexpr (std::is_same_v<T, HirFn>) {
                m_fnByName[i.name] = &i;
            } else if constexpr (std::is_same_v<T, HirClass>) {
                m_classByName[i.name] = &i;
                // Also index constructor
                std::string ctorName = i.name + ".init";
                // Constructor is not a separate HIR item yet, skip
            }
        }, item);
    }

    // First pass: copy items, scan ALL function bodies for instantiation sites
    for (const auto& item : hir.items) {
        std::visit([&](const auto& i) {
            using T = std::decay_t<decltype(i)>;
            if constexpr (std::is_same_v<T, HirFn>) {
                if (hasGenerics(i)) {
                    scanFn(i);  // scan generic template body too
                } else {
                    scanFn(i);  // scan non-generic functions for calls to generics
                    m_output.items.push_back(item);
                }
            } else if constexpr (std::is_same_v<T, HirClass>) {
                if (hasGenerics(i)) {
                    for (const auto& m : i.methods) scanFn(m);
                } else {
                    for (const auto& m : i.methods) scanFn(m);
                    m_output.items.push_back(item);
                }
            }
        }, item);
    }

    // Process worklist: scan newly created specializations for nested generics
    while (!m_worklist.empty()) {
        auto key = m_worklist.back();
        m_worklist.pop_back();

        std::string mangled = m_specMap[key];
        // Find the specialized item and scan it
        for (auto& item : m_output.items) {
            std::visit([&](auto& i) {
                using T = std::decay_t<decltype(i)>;
                if constexpr (std::is_same_v<T, HirFn>) {
                    if (i.name == mangled) { scanFn(i); }
                } else if constexpr (std::is_same_v<T, HirClass>) {
                    if (i.name == mangled) {
                        for (auto& m : i.methods) scanFn(m);
                    }
                }
            }, item);
        }
    }

    return std::move(m_output);
}

} // namespace loxis::v2
