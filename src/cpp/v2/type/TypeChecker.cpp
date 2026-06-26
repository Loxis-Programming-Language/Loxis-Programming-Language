#include "TypeChecker.hpp"
#include "TypeSubst.hpp"
#include <variant>
#include <cassert>
#include <cstdio>

namespace loxis::v2 {

TyPtr TypeChecker::freshInfer() { return mkInfer(inferCounter++); }
void TypeChecker::error(const std::string& msg) { errors.push_back(msg); }

bool TypeChecker::isNumeric(TyPtr t) {
    if (!t) return false;
    TyKind k = t->kind();
    return k == TyKind::Int || k == TyKind::Long || k == TyKind::Float || k == TyKind::Double;
}
bool TypeChecker::isInteger(TyPtr t) {
    if (!t) return false;
    TyKind k = t->kind();
    return k == TyKind::Int || k == TyKind::Long;
}
bool TypeChecker::isBool(TyPtr t) { return t && t->kind() == TyKind::Bool; }

TyPtr TypeChecker::unwrapNullable(TyPtr t) {
    if (!t) return nullptr;
    if (t->nullable) {
        auto copy = *t;
        copy.nullable = false;
        return std::make_shared<Ty>(std::move(copy));
    }
    return t;
}

bool TypeChecker::isSubtype(TyPtr a, TyPtr b) {
    if (!a || !b) return false;
    if (sameType(a, b)) return true;
    if (b->kind() == TyKind::Param || a->kind() == TyKind::Param) return true;
    // noreturn <: T (bottom)
    if (a->isNever()) return true;
    // null <: T? (null literal is subtype of any nullable)
    if (a->isNull() && b->nullable) return true;
    // T <: T? (non-null is subtype of nullable version)
    if (b->nullable && !a->nullable) {
        auto aNon = *a; aNon.nullable = false;
        auto bNon = *b; bNon.nullable = false;
        return sameType(std::make_shared<Ty>(aNon), std::make_shared<Ty>(bNon));
    }
    // Class A extends B: A <: B
    if (a->kind() == TyKind::Class_ && b->kind() == TyKind::Class_) {
        auto& ca = std::get<TyClass>(*a);
        auto& cb = std::get<TyClass>(*b);
        // Walk superclass chain of A looking for B
        auto def = ca.def;
        while (def) {
            if (def->name == cb.def->name) return true;
            if (!def->superName) break;
            // superclass resolution would need a scope; simplified for now
            break;
        }
    }
    // Interface I <: J if I extends J
    if (a->kind() == TyKind::Interface_ && b->kind() == TyKind::Interface_) {
        auto& ia = std::get<TyInterface>(*a);
        auto& ib = std::get<TyInterface>(*b);
        for (const auto& sn : ia.def->superNames) {
            if (sn == ib.def->name) return true;
        }
    }
    // Class A implements interface I
    if (a->kind() == TyKind::Class_ && b->kind() == TyKind::Interface_) {
        auto& ca = std::get<TyClass>(*a);
        auto& ib = std::get<TyInterface>(*b);
        for (const auto& iname : ca.def->interfaceNames) {
            if (iname == ib.def->name) return true;
        }
    }
    return false;
}

bool TypeChecker::sameType(TyPtr a, TyPtr b) {
    if (!a || !b) return false;
    if (a->isError() || b->isError()) return true;
    if (a->nullable != b->nullable) return false;
    if (a->kind() != b->kind()) return false;
    switch (a->kind()) {
        case TyKind::Infer:
            return std::get<InferVar>(*a).id == std::get<InferVar>(*b).id;
        case TyKind::Fn: {
            auto& fa = std::get<TyFn>(*a);
            auto& fb = std::get<TyFn>(*b);
            if (fa.params.size() != fb.params.size()) return false;
            for (size_t i = 0; i < fa.params.size(); ++i)
                if (!sameType(fa.params[i], fb.params[i])) return false;
            return sameType(fa.ret, fb.ret);
        }
        case TyKind::Tuple: {
            auto& ta = std::get<TyTuple>(*a);
            auto& tb = std::get<TyTuple>(*b);
            if (ta.elems.size() != tb.elems.size()) return false;
            for (size_t i = 0; i < ta.elems.size(); ++i)
                if (!sameType(ta.elems[i], tb.elems[i])) return false;
            return true;
        }
        case TyKind::Array: {
            auto& aa = std::get<TyArray>(*a);
            auto& ab = std::get<TyArray>(*b);
            return aa.len == ab.len && sameType(aa.elem, ab.elem);
        }
        case TyKind::Slice:
            return sameType(std::get<TySlice>(*a).elem, std::get<TySlice>(*b).elem);
        case TyKind::Ref: {
            auto& ra = std::get<TyRef>(*a);
            auto& rb = std::get<TyRef>(*b);
            return ra.mut == rb.mut && sameType(ra.elem, rb.elem);
        }
        case TyKind::RawPtr: {
            auto& ra = std::get<TyRawPtr>(*a);
            auto& rb = std::get<TyRawPtr>(*b);
            return ra.mut == rb.mut && sameType(ra.elem, rb.elem);
        }
        case TyKind::Adt: {
            auto& aa = std::get<TyAdt>(*a);
            auto& ab = std::get<TyAdt>(*b);
            if (aa.def->name != ab.def->name) return false;
            if (aa.args.size() != ab.args.size()) return false;
            for (size_t i = 0; i < aa.args.size(); ++i)
                if (!sameType(aa.args[i], ab.args[i])) return false;
            return true;
        }
        case TyKind::Class_: {
            auto& ca = std::get<TyClass>(*a);
            auto& cb = std::get<TyClass>(*b);
            if (ca.def->name != cb.def->name) return false;
            if (ca.args.size() != cb.args.size()) return false;
            for (size_t i = 0; i < ca.args.size(); ++i)
                if (!sameType(ca.args[i], cb.args[i])) return false;
            return true;
        }
        case TyKind::Interface_: {
            auto& ia = std::get<TyInterface>(*a);
            auto& ib = std::get<TyInterface>(*b);
            if (ia.def->name != ib.def->name) return false;
            if (ia.args.size() != ib.args.size()) return false;
            for (size_t i = 0; i < ia.args.size(); ++i)
                if (!sameType(ia.args[i], ib.args[i])) return false;
            return true;
        }
        case TyKind::Param:
            return std::get<TyParam>(*a).name == std::get<TyParam>(*b).name;
        case TyKind::Null:
            return true;
        default:
            return true;
    }
}

bool TypeChecker::isPrimitiveCastable(TyPtr from, TyPtr to) {
    return (isNumeric(from) && isNumeric(to)) ||
           (isInteger(from) && isBool(to)) ||
           (isBool(from) && isInteger(to)) ||
           (from->kind() == TyKind::Char && isInteger(to)) ||
           (isInteger(from) && to->kind() == TyKind::Char);
}

// ============================================================
// Class/Interface helpers
// ============================================================
const MethodInfo* TypeChecker::lookupMethod(TyPtr recv, const std::string& name) {
    if (!recv) return nullptr;
    // Strip nullable for lookup
    TyPtr inner = recv->nullable ? unwrapNullable(recv) : recv;
    if (inner->kind() == TyKind::Class_) {
        auto& cls = std::get<TyClass>(*inner);
        for (const auto& m : cls.def->methods) {
            if (m.name == name) return &m;
        }
    } else if (inner->kind() == TyKind::Interface_) {
        auto& iface = std::get<TyInterface>(*inner);
        for (const auto& m : iface.def->methods) {
            if (m.name == name) return &m;
        }
    }
    return nullptr;
}

const FieldInfo* TypeChecker::lookupField(TyPtr recv, const std::string& name) {
    if (!recv) return nullptr;
    TyPtr inner = recv->nullable ? unwrapNullable(recv) : recv;
    if (inner->kind() == TyKind::Class_) {
        auto& cls = std::get<TyClass>(*inner);
        for (const auto& f : cls.def->fields) {
            if (f.name == name) return &f;
        }
    }
    return nullptr;
}

bool TypeChecker::isMutablePlace(ExprPtr e, Scope* sc) {
    if (!e) return false;
    return std::visit([&](auto&& n) -> bool {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ExprPath>) {
            return true;
        } else if constexpr (std::is_same_v<T, ExprDeref>) {
            TyPtr t = checkExpr(n.expr, sc);
            if (t->kind() == TyKind::Ref) return std::get<TyRef>(*t).mut;
            if (t->kind() == TyKind::RawPtr) return std::get<TyRawPtr>(*t).mut;
            return false;
        } else if constexpr (std::is_same_v<T, ExprField>) {
            return isMutablePlace(n.obj, sc);
        } else if constexpr (std::is_same_v<T, ExprIndex>) {
            return isMutablePlace(n.obj, sc);
        } else {
            return false;
        }
    }, *e);
}

// ============================================================
// Type resolution
// ============================================================
TyPtr TypeChecker::resolvePathType(const Path& path, Scope* sc) {
    if (path.segs.empty()) { error("empty type path"); return mkError(); }
    Symbol* sym = nullptr;
    Scope* search = sc;
    for (const auto& s : path.segs) {
        sym = nullptr;
        while (search) {
            sym = search->lookup(s);
            if (sym) break;
            search = search->parent();
        }
        if (!sym) { error("unresolved type path: " + s); return mkError(); }
        if (sym->kind != SymbolKind::Type && sym->kind != SymbolKind::Interface) {
            error("path does not refer to a type: " + s); return mkError();
        }
    }
    if (!sym || !sym->ty) return mkError();

    if (path.args.empty()) return sym->ty;

    std::vector<TyPtr> argTys;
    for (const auto& arg : path.args) {
        argTys.push_back(resolveType(*arg, sc));
    }

    TyPtr base = sym->ty;
    if (base->kind() == TyKind::Class_) {
        auto& cls = std::get<TyClass>(*base);
        if (argTys.size() != cls.def->params.size()) {
            error("generic argument count mismatch for " + cls.def->name);
            return mkError();
        }
        return std::make_shared<Ty>(TyClass{cls.def, argTys});
    }
    if (base->kind() == TyKind::Interface_) {
        auto& iface = std::get<TyInterface>(*base);
        if (argTys.size() != iface.def->params.size()) {
            error("generic argument count mismatch for " + iface.def->name);
            return mkError();
        }
        return std::make_shared<Ty>(TyInterface{iface.def, argTys});
    }
    error("type arguments on non-generic type");
    return mkError();
}

TyPtr TypeChecker::resolveType(const Type& surfaceType, Scope* sc) {
    return std::visit([&](auto&& n) -> TyPtr {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, AstTyPath>) {
            return resolvePathType(n.path, sc);
        } else if constexpr (std::is_same_v<T, AstTyTuple>) {
            std::vector<TyPtr> elems;
            for (const auto& e : n.elems) elems.push_back(resolveType(*e, sc));
            return std::make_shared<Ty>(TyTuple{elems});
        } else if constexpr (std::is_same_v<T, AstTyArray>) {
            TyPtr elem = resolveType(*n.elem, sc);
            return std::make_shared<Ty>(TyArray{elem, 0});
        } else if constexpr (std::is_same_v<T, AstTySlice>) {
            TyPtr elem = resolveType(*n.elem, sc);
            return std::make_shared<Ty>(TySlice{elem});
        } else if constexpr (std::is_same_v<T, AstTyRef>) {
            TyPtr elem = resolveType(*n.elem, sc);
            return std::make_shared<Ty>(TyRef{n.mut, elem});
        } else if constexpr (std::is_same_v<T, AstTyPtr>) {
            TyPtr elem = resolveType(*n.elem, sc);
            return std::make_shared<Ty>(TyRawPtr{n.mut, elem});
        } else if constexpr (std::is_same_v<T, AstTyFn>) {
            std::vector<TyPtr> params;
            for (const auto& p : n.params) params.push_back(resolveType(*p, sc));
            TyPtr ret = resolveType(*n.ret, sc);
            return std::make_shared<Ty>(TyFn{params, ret});
        } else if constexpr (std::is_same_v<T, AstTyInfer>) {
            return freshInfer();
        } else if constexpr (std::is_same_v<T, AstTyNullable>) {
            TyPtr inner = resolveType(*n.inner, sc);
            return makeNullable(inner);
        } else {
            return mkError();
        }
    }, surfaceType);
}

// ============================================================
// Patterns
// ============================================================
TyPtr TypeChecker::checkPat(PatPtr p, TyPtr expected, Scope* sc) {
    if (!p) return mkError();
    return std::visit([&](auto&& n) -> TyPtr {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, PatWild>) {
            return expected ? expected : freshInfer();
        } else if constexpr (std::is_same_v<T, PatLit>) {
            TyPtr t = checkExpr(n.expr, sc);
            if (expected && !sameType(t, expected)) error("literal pattern type mismatch");
            return t;
        } else if constexpr (std::is_same_v<T, PatBind>) {
            TyPtr t = expected ? expected : freshInfer();
            sc->declare(n.name, SymbolKind::Value, nullptr, t);
            return t;
        } else if constexpr (std::is_same_v<T, PatTuple>) {
            std::vector<TyPtr> elemTys;
            if (expected && expected->kind() == TyKind::Tuple) {
                auto& tup = std::get<TyTuple>(*expected);
                if (tup.elems.size() != n.elems.size()) error("tuple pattern length mismatch");
                for (size_t i = 0; i < n.elems.size(); ++i) {
                    TyPtr et = (i < tup.elems.size()) ? tup.elems[i] : nullptr;
                    elemTys.push_back(checkPat(n.elems[i], et, sc));
                }
            } else {
                for (const auto& el : n.elems) elemTys.push_back(checkPat(el, nullptr, sc));
            }
            return std::make_shared<Ty>(TyTuple{elemTys});
        } else if constexpr (std::is_same_v<T, PatStruct>) {
            return expected ? expected : mkError();
        } else if constexpr (std::is_same_v<T, PatEnum>) {
            return expected ? expected : mkError();
        } else if constexpr (std::is_same_v<T, PatRef>) {
            if (expected && expected->kind() == TyKind::Ref) {
                TyPtr inner = std::get<TyRef>(*expected).elem;
                return std::make_shared<Ty>(TyRef{n.mut, checkPat(n.pat, inner, sc)});
            }
            TyPtr inner = checkPat(n.pat, nullptr, sc);
            return std::make_shared<Ty>(TyRef{n.mut, inner});
        } else if constexpr (std::is_same_v<T, PatRest>) {
            return expected ? expected : mkError();
        } else {
            return mkError();
        }
    }, *p);
}

// ============================================================
// Expressions — the core
// ============================================================
TyPtr TypeChecker::checkExpr(ExprPtr e, Scope* sc) {
    if (!e) { error("null expression"); return mkError(); }
    return std::visit([&](auto&& n) -> TyPtr {
        using T = std::decay_t<decltype(n)>;

        if constexpr (std::is_same_v<T, ExprLit>) {
            return std::visit([&](auto&& v) -> TyPtr {
                using VT = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<VT, int64_t> || std::is_same_v<VT, uint64_t>) return mkInt();
                else if constexpr (std::is_same_v<VT, double>) return mkDouble();
                else if constexpr (std::is_same_v<VT, bool>) return mkBool();
                else if constexpr (std::is_same_v<VT, char>) return std::make_shared<Ty>(TyChar{});
                else if constexpr (std::is_same_v<VT, std::string>) return mkStr();
                else { error("unknown literal"); return mkError(); }
            }, n.val);

        } else if constexpr (std::is_same_v<T, ExprPath>) {
            // Resolve name — may be value (var/val/fun) or type (for constructor)
            Symbol* sym = nullptr;
            Scope* search = sc;
            for (const auto& s : n.path.segs) {
                sym = nullptr;
                while (search) {
                    sym = search->lookup(s);
                    if (sym) break;
                    search = search->parent();
                }
                if (!sym) {
                    // Fallback: check if this is a field on 'this' in enclosing class scope
                    Scope* fs = sc;
                    while (fs) {
                        if (auto cd = fs->classDef()) {
                            for (const auto& f : cd->fields) {
                                if (f.name == s) {
                                    n.isField = true;
                                    n.fieldOffset = f.offset;
                                    return f.type ? f.type : mkUnit();
                                }
                            }
                        }
                        fs = fs->parent();
                    }
                    error("unresolved path: " + s); return mkError();
                }
            }
            if (sym->kind == SymbolKind::Type || sym->kind == SymbolKind::Interface) {
                TyPtr ty = sym->ty ? sym->ty : mkError();
                if (!n.path.args.empty()) {
                    std::vector<TyPtr> argTys;
                    for (const auto& arg : n.path.args) {
                        argTys.push_back(resolveType(*arg, sc));
                    }
                    if (ty->kind() == TyKind::Class_) {
                        auto& cls = std::get<TyClass>(*ty);
                        if (argTys.size() != cls.def->params.size()) {
                            error("generic argument count mismatch for " + cls.def->name);
                            return mkError();
                        }
                        ty = std::make_shared<Ty>(TyClass{cls.def, argTys});
                    } else if (ty->kind() == TyKind::Interface_) {
                        auto& iface = std::get<TyInterface>(*ty);
                        if (argTys.size() != iface.def->params.size()) {
                            error("generic argument count mismatch for " + iface.def->name);
                            return mkError();
                        }
                        ty = std::make_shared<Ty>(TyInterface{iface.def, argTys});
                    }
                }
                return ty;
            }
            if (sym->kind == SymbolKind::Value) {
                TyPtr ty = sym->ty ? sym->ty : mkUnit();
                // Check if this is a class field — mark for field access lowering
                if (n.path.segs.size() == 1 && n.path.args.empty()) {
                    Scope* fs = sc;
                    while (fs) {
                        if (auto cd = fs->classDef()) {
                            for (const auto& f : cd->fields) {
                                if (f.name == n.path.segs[0]) {
                                    n.isField = true;
                                    n.fieldOffset = f.offset;
                                    if (!sym->ty && f.type) ty = f.type;
                                    break;
                                }
                            }
                        }
                        fs = fs->parent();
                    }
                }
                if (!n.path.args.empty() && sym->node) {
                    if (auto* fun = static_cast<ItemFun*>(sym->node)) {
                        if (!fun->generics.empty()) {
                            std::vector<TyPtr> typeArgs;
                            for (const auto& ta : n.path.args) {
                                typeArgs.push_back(resolveType(*ta, sc));
                            }
                            if (typeArgs.size() != fun->generics.size()) {
                                error("generic argument count mismatch for " + fun->name);
                                return mkError();
                            }
                            std::vector<std::string> names;
                            for (const auto& g : fun->generics) names.push_back(g.name);
                            ty = substituteType(ty, makeSubstMap(names, typeArgs));
                        }
                    }
                }
                n.resolvedTy = ty;
                return ty;
            }
            error("path did not resolve to value or type"); return mkError();

        } else if constexpr (std::is_same_v<T, ExprTuple>) {
            std::vector<TyPtr> elems;
            for (const auto& el : n.elems) elems.push_back(checkExpr(el, sc));
            return std::make_shared<Ty>(TyTuple{elems});

        } else if constexpr (std::is_same_v<T, ExprArray>) {
            if (n.elems.empty()) return std::make_shared<Ty>(TyArray{mkError(), 0});
            TyPtr first = checkExpr(n.elems[0], sc);
            for (size_t i = 1; i < n.elems.size(); ++i) {
                TyPtr et = checkExpr(n.elems[i], sc);
                if (!sameType(et, first)) error("array element type mismatch");
            }
            return std::make_shared<Ty>(TyArray{first, static_cast<uint64_t>(n.elems.size())});

        } else if constexpr (std::is_same_v<T, ExprStruct>) {
            // Struct/class literal: resolve type, check fields
            TyPtr structTy = resolvePathType(n.path, sc);
            if (structTy->isError()) return mkError();
            if (structTy->kind() == TyKind::Class_) {
                auto& cls = std::get<TyClass>(*structTy);
                for (const auto& f : n.fields) {
                    const FieldInfo* fi = lookupField(structTy, f.name);
                    if (!fi) { error("unknown field '" + f.name + "' in " + cls.def->name); }
                    else if (f.expr) {
                        TyPtr ft = checkExpr(f.expr, sc);
                        TyPtr expected = fi->type;
                        auto subst = classSubstMap(structTy);
                        if (!subst.empty() && expected) expected = substituteType(expected, subst);
                        if (expected && !sameType(ft, expected)) error("field type mismatch for '" + f.name + "'");
                    }
                }
                // Annotate AST node for lowering
                n.totalFieldSize = cls.def->totalFieldSize;
                n.typeId = cls.def->typeId;
                n.resolvedTypeArgs = cls.args; // Concrete generic args
                n.vtableEntryCount = static_cast<uint32_t>(cls.def->vtable.size());
                for (uint32_t vi : cls.def->vtable) {
                    if (vi < cls.def->methods.size())
                        n.vtableMethodNames.push_back(cls.def->name + "." + cls.def->methods[vi].name);
                    else
                        n.vtableMethodNames.push_back("");
                }
            }
            return structTy;

        } else if constexpr (std::is_same_v<T, ExprCall>) {
            TyPtr callee = checkExpr(n.func, sc);
            if (!callee || callee->isError()) return mkError();

            // Built-in untyped calls (print, println)
            if (callee->isUnit()) {
                for (const auto& a : n.args) checkExpr(a, sc);
                return mkUnit();
            }

            // Constructor call: callee is a class type
            if (callee->kind() == TyKind::Class_) {
                auto& cls = std::get<TyClass>(*callee);
                for (const auto& a : n.args) checkExpr(a, sc);
                n.ctorLabel = cls.def->name + ".init";
                n.resolvedTypeArgs = cls.args; // Concrete generic args
                n.resolvedTy = callee;
                return callee;
            }

            // Function call
            if (callee->kind() != TyKind::Fn) { error("call on non-function"); return mkError(); }
            auto& fn = std::get<TyFn>(*callee);
            if (fn.params.size() != n.args.size()) { error("call arity mismatch"); return mkError(); }
            for (size_t i = 0; i < n.args.size(); ++i) {
                TyPtr argTy = checkExpr(n.args[i], sc);
                if (!isSubtype(argTy, fn.params[i])) error("call argument type mismatch");
            }
            // Store resolved generic type args from the path
            if (auto* pe = std::get_if<ExprPath>(n.func.get())) {
                if (!pe->path.args.empty()) {
                    for (const auto& arg : pe->path.args) {
                        n.resolvedTypeArgs.push_back(resolveType(*arg, sc));
                    }
                }
            }
            n.resolvedTy = fn.ret;
            return fn.ret;

        } else if constexpr (std::is_same_v<T, ExprMethodCall>) {
            TyPtr recv = checkExpr(n.recv, sc);
            if (!recv || recv->isError()) return mkError();

            // Determine dispatch kind for the backend
            TyPtr inner = recv->nullable ? unwrapNullable(recv) : recv;
            if (inner->kind() == TyKind::Interface_) {
                n.isInterfaceDispatch = true;
                auto& iface = std::get<TyInterface>(*inner);
                n.ifaceTypeId = iface.def->typeId;
                // Find method index within the interface
                for (size_t i = 0; i < iface.def->methods.size(); ++i) {
                    if (iface.def->methods[i].name == n.method) {
                        n.ifaceMethodIndex = static_cast<uint32_t>(i);
                        break;
                    }
                }
            }

            const MethodInfo* mi = lookupMethod(recv, n.method);
            if (!mi) {
                error("method '" + n.method + "' not found on type " + recv->toString());
                return mkError();
            }

            n.vtableIndex = mi->vtableIndex;
            if (inner->kind() == TyKind::Class_) {
                n.qualifiedName = std::get<TyClass>(*inner).def->name + "." + n.method;
            }

            if (mi->funcType && mi->funcType->kind() == TyKind::Fn) {
                TyPtr fnTy = mi->funcType;
                auto subst = classSubstMap(recv);
                if (!subst.empty()) fnTy = substituteType(fnTy, subst);
                auto& mfn = std::get<TyFn>(*fnTy);
                size_t selfOffset = 1;
                if (n.args.size() + selfOffset != mfn.params.size())
                    error("method call arity mismatch for '" + n.method + "'");
                else {
                    for (size_t i = 0; i < n.args.size(); ++i) {
                        TyPtr argTy = checkExpr(n.args[i], sc);
                        if (!isSubtype(argTy, mfn.params[i + selfOffset]))
                            error("method argument type mismatch for '" + n.method + "'");
                    }
                }
                return mfn.ret;
            }
            // Method type not yet resolved — accept any args
            for (const auto& a : n.args) checkExpr(a, sc);
            return freshInfer();

        } else if constexpr (std::is_same_v<T, ExprField>) {
            TyPtr base = checkExpr(n.obj, sc);
            if (!base || base->isError()) return mkError();

            const FieldInfo* fi = lookupField(base, n.field);
            if (fi) {
                n.fieldOffset = fi->offset;
                TyPtr ft = fi->type ? fi->type : freshInfer();
                auto subst = classSubstMap(base);
                if (!subst.empty()) ft = substituteType(ft, subst);
                return ft;
            }
            error("field '" + n.field + "' not found on type " + base->toString());
            return mkError();

        } else if constexpr (std::is_same_v<T, ExprIndex>) {
            TyPtr base = checkExpr(n.obj, sc);
            TyPtr idx = checkExpr(n.idx, sc);
            if (!isInteger(idx)) error("index must be integer");
            if (base->kind() == TyKind::Array) return std::get<TyArray>(*base).elem;
            if (base->kind() == TyKind::Slice) return std::get<TySlice>(*base).elem;
            error("index on non-array/slice"); return mkError();

        } else if constexpr (std::is_same_v<T, ExprUnary>) {
            TyPtr inner = checkExpr(n.operand, sc);
            switch (n.op) {
                case ExprUnary::Neg:
                    if (!isNumeric(inner)) { error("neg on non-numeric"); return mkError(); }
                    return inner;
                case ExprUnary::Not:
                    if (!isBool(inner)) { error("not on non-bool"); return mkError(); }
                    return mkBool();
                case ExprUnary::Deref:
                    if (inner->kind() == TyKind::Ref) return std::get<TyRef>(*inner).elem;
                    if (inner->kind() == TyKind::RawPtr) return std::get<TyRawPtr>(*inner).elem;
                    error("deref on non-reference"); return mkError();
                case ExprUnary::Ref:
                    return std::make_shared<Ty>(TyRef{false, inner});
                case ExprUnary::RefMut:
                    if (!isMutablePlace(n.operand, sc)) { error("cannot take &mut of immutable place"); return mkError(); }
                    return std::make_shared<Ty>(TyRef{true, inner});
                default:
                    error("unknown unary op"); return mkError();
            }

        } else if constexpr (std::is_same_v<T, ExprBinary>) {
            TyPtr l = checkExpr(n.l, sc);
            TyPtr r = checkExpr(n.r, sc);
            switch (n.op) {
                case ExprBinary::Add:
                    // String concatenation
                    if (l->isStr() && r->isStr()) return mkStr();
                    // Numeric
                    if (!isNumeric(l) || !isNumeric(r)) { error("numeric op on non-numeric"); return mkError(); }
                    if (!sameType(l, r)) { error("numeric op type mismatch"); return mkError(); }
                    return l;
                case ExprBinary::Sub: case ExprBinary::Mul:
                case ExprBinary::Div: case ExprBinary::Rem:
                    if (!isNumeric(l) || !isNumeric(r)) { error("numeric op on non-numeric"); return mkError(); }
                    if (!sameType(l, r)) { error("numeric op type mismatch"); return mkError(); }
                    return l;
                case ExprBinary::Shl: case ExprBinary::Shr:
                case ExprBinary::BitAnd: case ExprBinary::BitOr: case ExprBinary::BitXor:
                    if (!isInteger(l) || !isInteger(r)) { error("bitwise op on non-integer"); return mkError(); }
                    if (!sameType(l, r)) { error("bitwise op type mismatch"); return mkError(); }
                    return l;
                case ExprBinary::Eq: case ExprBinary::Ne:
                    // Allow comparison with null: T? == null, null == T?
                    if (l->isNull() || r->isNull()) {
                        if (!l->isNull() && !l->nullable) error("cannot compare non-nullable with null");
                        if (!r->isNull() && !r->nullable) error("cannot compare non-nullable with null");
                        return mkBool();
                    }
                    if (!sameType(l, r)) {
                        if (!isSubtype(l, r) && !isSubtype(r, l))
                            error("comparison type mismatch");
                    }
                    return mkBool();
                case ExprBinary::Lt: case ExprBinary::Le:
                case ExprBinary::Gt: case ExprBinary::Ge:
                    if (!sameType(l, r)) { error("comparison type mismatch"); return mkError(); }
                    return mkBool();
                case ExprBinary::And: case ExprBinary::Or:
                    if (!isBool(l) || !isBool(r)) { error("logical op on non-bool"); return mkError(); }
                    return mkBool();
                default:
                    error("unknown binary op"); return mkError();
            }

        } else if constexpr (std::is_same_v<T, ExprAssign>) {
            if (!isMutablePlace(n.lhs, sc)) { error("assignment to immutable place"); return mkError(); }
            TyPtr lhs = checkExpr(n.lhs, sc);
            TyPtr rhs = checkExpr(n.rhs, sc);
            switch (n.op) {
                case ExprAssign::Set:
                    if (!isSubtype(rhs, lhs)) error("assignment type mismatch");
                    break;
                default:
                    if (!isNumeric(lhs) || !sameType(lhs, rhs)) error("compound assignment type error");
                    break;
            }
            return mkUnit();

        } else if constexpr (std::is_same_v<T, ExprBlock>) {
            for (const auto& stmt : n.stmts) checkStmt(stmt, sc);
            if (n.tail) return checkExpr(n.tail, sc);
            return mkUnit();

        } else if constexpr (std::is_same_v<T, ExprIf>) {
            TyPtr cond = checkExpr(n.cond, sc);
            if (!isBool(cond)) error("if condition must be bool");

            // Smart casting: extract type refinements from the condition
            bool invertRefinements = false;
            auto refinements = extractRefinements(*n.cond, sc, invertRefinements);

            // Then branch: use refinements (non-inverted)
            Scope thenScope(sc);
            if (!invertRefinements) {
                applyRefinements(&thenScope, refinements);
            }
            TyPtr t = checkExpr(n.then_, &thenScope);

            if (n.else_) {
                // Else branch: use inverted refinements
                Scope elseScope(sc);
                if (invertRefinements) {
                    applyRefinements(&elseScope, refinements);
                }
                TyPtr el = checkExpr(n.else_, &elseScope);
                if (!sameType(t, el)) { error("if/else branch type mismatch"); return mkError(); }
                return t;
            }
            return mkUnit();

        } else if constexpr (std::is_same_v<T, ExprWhile>) {
            TyPtr cond = checkExpr(n.cond, sc);
            if (!isBool(cond)) error("while condition must be bool");
            loopStack.push_back({mkUnit(), n.label});
            checkExpr(n.body, sc);
            loopStack.pop_back();
            return mkUnit();

        } else if constexpr (std::is_same_v<T, ExprRange>) {
            TyPtr start = checkExpr(n.start, sc);
            TyPtr end = checkExpr(n.end, sc);
            if (!isInteger(start) || !isInteger(end)) error("range bounds must be integers");
            return mkInt();

        } else if constexpr (std::is_same_v<T, ExprFor>) {
            TyPtr iter = checkExpr(n.iter, sc);
            TyPtr elemTy = mkInt();

            if (std::holds_alternative<ExprRange>(*n.iter)) {
                const auto& range = std::get<ExprRange>(*n.iter);
                n.iterKind = range.inclusive
                    ? ExprFor::IterKind::IntRangeInclusive
                    : ExprFor::IterKind::IntRange;
            } else if (iter->kind() == TyKind::Array) {
                n.iterKind = ExprFor::IterKind::Array;
                elemTy = std::get<TyArray>(*iter).elem;
                if (std::holds_alternative<ExprArray>(*n.iter)) {
                    n.arrayLen = std::get<ExprArray>(*n.iter).elems.size();
                } else {
                    n.arrayLen = std::get<TyArray>(*iter).len;
                }
            } else if (lookupMethod(iter, "size") && lookupMethod(iter, "get")) {
                n.iterKind = ExprFor::IterKind::Iterable;
                if (iter->kind() == TyKind::Class_) {
                    n.iterClassName = std::get<TyClass>(*iter).def->name;
                }
                const MethodInfo* getM = lookupMethod(iter, "get");
                if (getM && getM->funcType && getM->funcType->kind() == TyKind::Fn) {
                    TyPtr ret = std::get<TyFn>(*getM->funcType).ret;
                    auto subst = classSubstMap(iter);
                    elemTy = subst.empty() ? ret : substituteType(ret, subst);
                }
            } else {
                error("for-in requires integer range, array literal, or iterable with size()/get()");
                return mkError();
            }

            Scope bodyScope(sc);
            checkPat(n.pat, elemTy, &bodyScope);
            loopStack.push_back({mkUnit(), n.label});
            checkExpr(n.body, &bodyScope);
            loopStack.pop_back();
            return mkUnit();

        } else if constexpr (std::is_same_v<T, ExprLoop>) {
            loopStack.push_back({mkUnit(), n.label});
            checkExpr(n.body, sc);
            loopStack.pop_back();
            return mkUnit();

        } else if constexpr (std::is_same_v<T, ExprWhen>) {
            TyPtr scrut = n.scrut ? checkExpr(n.scrut, sc) : nullptr;
            TyPtr result = nullptr;
            for (const auto& arm : n.arms) {
                Scope armScope(sc);
                if (auto* cond = std::get_if<ExprPtr>(&arm.condition)) {
                    if (*cond) {
                        TyPtr condTy = checkExpr(*cond, sc);
                        if (scrut && !sameType(condTy, scrut))
                            error("when condition type does not match scrutinee");
                        // Apply smart casting refinements from the arm condition
                        bool invert = false;
                        auto refs = extractRefinements(**cond, sc, invert);
                        if (!invert) applyRefinements(&armScope, refs);
                    }
                }
                TyPtr armTy = arm.body ? checkExpr(arm.body, &armScope) : mkUnit();
                if (!result) result = armTy;
                else if (!sameType(result, armTy)) error("when arm type mismatch");
            }
            return result ? result : mkError();

        } else if constexpr (std::is_same_v<T, ExprBreak>) {
            if (loopStack.empty()) { error("break outside loop"); return mkError(); }
            if (n.expr) checkExpr(n.expr, sc);
            return mkNever();

        } else if constexpr (std::is_same_v<T, ExprContinue>) {
            if (loopStack.empty()) { error("continue outside loop"); return mkError(); }
            return mkNever();

        } else if constexpr (std::is_same_v<T, ExprReturn>) {
            if (!currentRetTy) { error("return outside function"); return mkError(); }
            if (n.expr) {
                TyPtr ret = checkExpr(n.expr, sc);
                if (!isSubtype(ret, currentRetTy)) error("return type mismatch");
            } else {
                if (!currentRetTy->isUnit()) error("return unit in non-unit function");
            }
            return mkNever();

        } else if constexpr (std::is_same_v<T, ExprClosure>) {
            std::vector<TyPtr> paramTys;
            for (size_t i = 0; i < n.params.size(); ++i) paramTys.push_back(freshInfer());
            TyPtr bodyTy = checkExpr(n.body, sc);
            TyPtr retTy = n.ret ? resolveType(*n.ret, sc) : bodyTy;
            return std::make_shared<Ty>(TyFn{paramTys, retTy});

        } else if constexpr (std::is_same_v<T, ExprRef>) {
            TyPtr inner = checkExpr(n.expr, sc);
            return std::make_shared<Ty>(TyRef{n.mut, inner});

        } else if constexpr (std::is_same_v<T, ExprDeref>) {
            TyPtr inner = checkExpr(n.expr, sc);
            if (inner->kind() == TyKind::Ref) return std::get<TyRef>(*inner).elem;
            if (inner->kind() == TyKind::RawPtr) return std::get<TyRawPtr>(*inner).elem;
            error("deref on non-reference"); return mkError();

        } else if constexpr (std::is_same_v<T, ExprTry>) {
            TyPtr inner = checkExpr(n.expr, sc);
            (void)inner;
            return freshInfer();

        } else if constexpr (std::is_same_v<T, ExprCast>) {
            TyPtr from = checkExpr(n.expr, sc);
            TyPtr to = resolveType(*n.ty, sc);
            if (!isPrimitiveCastable(from, to)) error("invalid cast");
            return to;

        } else if constexpr (std::is_same_v<T, ExprSafeCall>) {
            TyPtr recv = checkExpr(n.recv, sc);
            if (!recv->nullable) error("safe call on non-nullable type");
            TyPtr inner = unwrapNullable(recv);
            const MethodInfo* mi = lookupMethod(inner, n.method);
            if (mi && mi->funcType && mi->funcType->kind() == TyKind::Fn) {
                TyPtr ret = std::get<TyFn>(*mi->funcType).ret;
                return makeNullable(ret);
            }
            return makeNullable(freshInfer());

        } else if constexpr (std::is_same_v<T, ExprSafeField>) {
            TyPtr recv = checkExpr(n.recv, sc);
            if (!recv->nullable) error("safe field access on non-nullable type");
            TyPtr inner = unwrapNullable(recv);
            const FieldInfo* fi = lookupField(inner, n.field);
            if (fi && fi->type) return makeNullable(fi->type);
            return makeNullable(freshInfer());

        } else if constexpr (std::is_same_v<T, ExprElvis>) {
            TyPtr lhs = checkExpr(n.lhs, sc);
            if (!lhs->nullable) error("elvis on non-nullable type");
            TyPtr rhs = checkExpr(n.rhs, sc);
            TyPtr inner = unwrapNullable(lhs);
            // Result is the common type — non-nullable unwrapped version
            if (sameType(inner, rhs)) return inner;
            return rhs; // simplified: just return rhs type

        } else if constexpr (std::is_same_v<T, ExprForceUnwrap>) {
            TyPtr inner = checkExpr(n.expr, sc);
            if (!inner->nullable) error("force unwrap on non-nullable type");
            return unwrapNullable(inner);

        } else if constexpr (std::is_same_v<T, ExprIs>) {
            TyPtr exprTy = checkExpr(n.expr, sc);
            TyPtr ty = resolveType(*n.ty, sc);
            (void)exprTy; (void)ty;
            return mkBool();

        } else if constexpr (std::is_same_v<T, ExprIsNot>) {
            TyPtr exprTy = checkExpr(n.expr, sc);
            TyPtr ty = resolveType(*n.ty, sc);
            (void)exprTy; (void)ty;
            return mkBool();

        } else if constexpr (std::is_same_v<T, ExprNull>) {
            return mkNull();

        } else if constexpr (std::is_same_v<T, ExprStringTemplate>) {
            return mkStr();

        } else {
            error("unhandled expression kind"); return mkError();
        }
    }, *e);
}

// ============================================================
// Statements
// ============================================================
void TypeChecker::checkStmt(StmtPtr s, Scope* sc) {
    if (!s) return;
    std::visit([&](auto&& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, StmtLet>) {
            TyPtr initTy = n.init ? checkExpr(n.init, sc) : mkUnit();
            TyPtr patTy = n.ty ? resolveType(*n.ty, sc) : initTy;
            if (!isSubtype(initTy, patTy)) error("let init type mismatch");
            checkPat(n.pat, patTy, sc);
        } else if constexpr (std::is_same_v<T, StmtExpr>) {
            checkExpr(n.expr, sc);
        } else if constexpr (std::is_same_v<T, StmtItem>) {
            checkItem(n.item, sc);
        }
    }, *s);
}

// ============================================================
// Items
// ============================================================
void TypeChecker::checkItem(ItemPtr i, Scope* sc) {
    if (!i) return;
    std::visit([&](auto&& n) {
        using T = std::decay_t<decltype(n)>;

        if constexpr (std::is_same_v<T, ItemFun>) {
            Scope fnScope(sc);
            for (size_t gi = 0; gi < n.generics.size(); ++gi) {
                fnScope.declare(
                    n.generics[gi].name,
                    SymbolKind::Type,
                    nullptr,
                    std::make_shared<Ty>(TyParam{n.generics[gi].name, static_cast<uint32_t>(gi)}));
            }

            std::vector<TyPtr> paramTys;
            for (const auto& p : n.params)
                paramTys.push_back(resolveType(*p.second, &fnScope));

            TyPtr retTy = n.ret ? resolveType(*n.ret, &fnScope) : mkUnit();
            TyPtr fnTy = std::make_shared<Ty>(TyFn{paramTys, retTy});

            Symbol* sym = sc->lookup(n.name);
            if (sym) sym->ty = fnTy;

            TyPtr saved = currentRetTy;
            currentRetTy = retTy;

            for (size_t i = 0; i < n.params.size(); ++i)
                fnScope.declare(n.params[i].first, SymbolKind::Value, nullptr, paramTys[i]);

            if (n.body) checkExpr(n.body, &fnScope);
            currentRetTy = saved;

        } else if constexpr (std::is_same_v<T, ItemClass>) {
            // Type-check in class child scope
            Scope* classScope = sc->childMod(n.name);
            if (!classScope) classScope = sc;
            auto classDef = classScope->classDef();

            for (const auto& pd : n.primaryCtor) {
                if (!pd.isVal && !pd.isVar) continue;
                TyPtr paramTy = pd.ty ? resolveType(*pd.ty, classScope) : mkError();
                if (classDef) {
                    for (auto& field : classDef->fields) {
                        if (field.name == pd.name) {
                            field.type = paramTy;
                            break;
                        }
                    }
                }
                if (Symbol* sym = classScope->lookupLocal(pd.name)) sym->ty = paramTy;
            }

            // Check field initializers in class scope
            for (const auto& fd : n.fields) {
                TyPtr fieldTy = fd.ty ? resolveType(*fd.ty, classScope) : nullptr;
                if (classDef) {
                    for (auto& field : classDef->fields) {
                        if (field.name == fd.name) {
                            field.type = fieldTy;
                            break;
                        }
                    }
                }
                if (fd.initializer) {
                    TyPtr initTy = checkExpr(fd.initializer, classScope);
                    if (!fieldTy) fieldTy = initTy;
                    if (classDef) {
                        for (auto& field : classDef->fields) {
                            if (field.name == fd.name) {
                                field.type = fieldTy;
                                break;
                            }
                        }
                    }
                    if (fd.ty) {
                        if (!isSubtype(initTy, fieldTy)) error("field initializer type mismatch for '" + fd.name + "'");
                    }
                    // Update scope symbol with resolved field type
                    if (Symbol* fsym = classScope->lookupLocal(fd.name)) {
                        fsym->ty = fieldTy;
                    }
                }
            }

            // Check methods in class scope
            for (const auto& m : n.methods) {
                if (classDef) {
                    std::vector<TyPtr> params;
                    params.push_back(classScope->selfTy() ? classScope->selfTy() : mkError());
                    for (const auto& p : m.params) {
                        params.push_back(resolveType(*p.second, classScope));
                    }
                    TyPtr retTy = m.ret ? resolveType(*m.ret, classScope) : mkUnit();
                    TyPtr methodTy = std::make_shared<Ty>(TyFn{std::move(params), retTy});
                    for (auto& method : classDef->methods) {
                        if (method.name == m.name) {
                            method.funcType = methodTy;
                            break;
                        }
                    }
                    if (Symbol* sym = classScope->lookupLocal(m.name)) sym->ty = methodTy;
                }
                checkItem(std::make_shared<Item>(m), classScope);
            }

            // Populate ItemClass annotations for backend code generation
            if (classDef) {
                n.totalFieldSize = classDef->totalFieldSize;
                n.typeId = classDef->typeId;
                n.itables = classDef->itables;
                n.vtableEntryCount = static_cast<uint32_t>(classDef->vtable.size());
                for (uint32_t vi : classDef->vtable) {
                    if (vi < classDef->methods.size())
                        n.vtableMethodNames.push_back(n.name + "." + classDef->methods[vi].name);
                    else
                        n.vtableMethodNames.push_back("");
                }
            }

        } else if constexpr (std::is_same_v<T, ItemEnumClass>) {
            Scope* enumScope = sc->childMod(n.name);
            if (!enumScope) enumScope = sc;
            for (const auto& m : n.methods)
                checkItem(std::make_shared<Item>(m), enumScope);

        } else if constexpr (std::is_same_v<T, ItemInterface>) {
            // Interface methods don't have bodies to check
            // (default bodies are checked when implemented)

        } else if constexpr (std::is_same_v<T, ItemObject>) {
            Scope* objScope = sc->childMod(n.name);
            if (!objScope) objScope = sc;
            for (const auto& fd : n.fields) {
                if (fd.initializer) checkExpr(fd.initializer, objScope);
            }
            for (const auto& m : n.methods)
                checkItem(std::make_shared<Item>(m), objScope);

        } else if constexpr (std::is_same_v<T, ItemImport>) {
            // handled by ModuleLoader

        } else if constexpr (std::is_same_v<T, ItemVal>) {
            TyPtr init = checkExpr(n.init, sc);
            TyPtr finalTy = init;
            if (n.ty) {
                TyPtr ty = resolveType(*n.ty, sc);
                if (!isSubtype(init, ty)) error("val type mismatch");
                finalTy = ty;
            }
            if (Symbol* sym = sc->lookupLocal(n.name)) sym->ty = finalTy;

        } else if constexpr (std::is_same_v<T, ItemVar>) {
            TyPtr init = checkExpr(n.init, sc);
            TyPtr finalTy = init;
            if (n.ty) {
                TyPtr ty = resolveType(*n.ty, sc);
                if (!isSubtype(init, ty)) error("var type mismatch");
                finalTy = ty;
            }
            if (Symbol* sym = sc->lookupLocal(n.name)) sym->ty = finalTy;

        } else if constexpr (std::is_same_v<T, ItemConst>) {
            TyPtr ty = resolveType(*n.ty, sc);
            TyPtr init = checkExpr(n.init, sc);
            if (!isSubtype(init, ty)) error("const type mismatch");
            if (Symbol* sym = sc->lookupLocal(n.name)) sym->ty = ty;
        }
    }, *i);
}

// ============================================================
// Smart casting / flow typing
// ============================================================
std::vector<TypeChecker::TypeRefinement> TypeChecker::extractRefinements(const Expr& cond, Scope* sc, bool& isInvert) {
    isInvert = false;
    return std::visit([&](const auto& e) -> std::vector<TypeRefinement> {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, ExprIs>) {
            // x is Type → refine x to Type in then-branch
            if (auto* path = std::get_if<ExprPath>(e.expr.get())) {
                std::string varName = path->path.segs.empty() ? "" : path->path.segs.back();
                if (!varName.empty()) {
                    TyPtr targetTy = e.ty ? resolveType(*e.ty, sc) : nullptr;
                    return {{varName, targetTy}};
                }
            }
        }
        else if constexpr (std::is_same_v<T, ExprIsNot>) {
            // x !is Type → refine x to Type in else-branch (invert)
            isInvert = true;
            if (auto* path = std::get_if<ExprPath>(e.expr.get())) {
                std::string varName = path->path.segs.empty() ? "" : path->path.segs.back();
                if (!varName.empty()) {
                    TyPtr targetTy = e.ty ? resolveType(*e.ty, sc) : nullptr;
                    return {{varName, targetTy}};
                }
            }
        }
        else if constexpr (std::is_same_v<T, ExprBinary>) {
            if (e.op == ExprBinary::Ne) {
                // x != null → refine T? to T in then-branch
                if (auto* path = std::get_if<ExprPath>(e.l.get())) {
                    if (std::holds_alternative<ExprNull>(*e.r)) {
                        std::string varName = path->path.segs.empty() ? "" : path->path.segs.back();
                        return {{varName, nullptr}}; // nullptr means "unwrap nullable"
                    }
                }
                // null != x → refine T? to T in then-branch
                if (auto* path = std::get_if<ExprPath>(e.r.get())) {
                    if (std::holds_alternative<ExprNull>(*e.l)) {
                        std::string varName = path->path.segs.empty() ? "" : path->path.segs.back();
                        return {{varName, nullptr}};
                    }
                }
            }
        }
        else if constexpr (std::is_same_v<T, ExprUnary>) {
            if (e.op == ExprUnary::Not) {
                // !(x is Type) → invert: then/else swap
                bool innerInvert = false;
                auto inner = extractRefinements(*e.operand, sc, innerInvert);
                isInvert = !innerInvert; // invert the invert flag
                return inner;
            }
        }

        return {};
    }, cond);
}

void TypeChecker::applyRefinements(Scope* target, const std::vector<TypeRefinement>& refs) {
    for (const auto& r : refs) {
        if (r.varName.empty()) continue;
        Symbol* sym = target->lookupLocal(r.varName);
        if (!sym) {
            // Try parent scope
            sym = target->lookup(r.varName);
        }
        if (sym && sym->ty) {
            TyPtr refined;
            // nullptr means "unwrap nullable"
            if (!r.refinedType && sym->ty->nullable) {
                refined = unwrapNullable(sym->ty);
            } else if (r.refinedType) {
                refined = r.refinedType;
            }
            if (refined) {
                target->declare(r.varName, sym->kind, sym->node, refined);
            }
        }
    }
}

bool TypeChecker::checkModule(Module& mod) {
    ScopeBuilder builder;
    rootOwner = builder.build(mod);
    rootScope = rootOwner.get();
    for (const auto& item : mod.items)
        checkItem(item, rootScope);
    return errors.empty();
}

} // namespace
