#include "TypeChecker.hpp"
#include "../NativeBuiltins.hpp"
#include "TypeSubst.hpp"
#include <variant>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>

namespace loxis::v2 {

namespace {
std::string overloadBackendName(const std::string& name, const TyFn& signature) {
    return mangleFunctionSignature(name, signature);
}

std::string binaryOperatorName(ExprBinary::Op op) {
    switch (op) {
        case ExprBinary::Add: return "operator+";
        case ExprBinary::Sub: return "operator-";
        case ExprBinary::Mul: return "operator*";
        case ExprBinary::Div: return "operator/";
        case ExprBinary::Rem: return "operator%";
        case ExprBinary::Eq:  return "operator==";
        case ExprBinary::Ne:  return "operator!=";
        case ExprBinary::Lt:  return "operator<";
        case ExprBinary::Le:  return "operator<=";
        case ExprBinary::Gt:  return "operator>";
        case ExprBinary::Ge:  return "operator>=";
        default: return "";
    }
}

std::string unaryOperatorName(ExprUnary::Op op) {
    switch (op) {
        case ExprUnary::Neg: return "operator-";
        case ExprUnary::Not: return "operator!";
        default: return "";
    }
}

TyPtr callableParamType(TyPtr type, size_t index, std::optional<size_t> varargIndex) {
    if (varargIndex && *varargIndex == index)
        return std::make_shared<Ty>(TyArray{type, 0});
    return type;
}
}

TyPtr TypeChecker::freshInfer() { return mkInfer(inferCounter++); }

bool TypeChecker::canAccessIntrinsic(const std::string& name) {
    static const std::vector<std::string> internal = {
        "alloc", "halloc", "hread", "hload", "hset", "hstore", "hfree", "syscall",
        "strlen", "strchar", "strslice", "strindexof", "i2s", "s2i", "chr",
        "itof", "ftoi", "fadd", "fsub", "fmul", "fdiv", "fneg",
        "nativeFileReadText", "nativeFileWriteText", "nativeFileAppendText", "nativeFileRemove"
    };
    if (name != "Unsafe" && !name.starts_with("__loxis_") && !findNativeBuiltin(name) &&
        std::find(internal.begin(), internal.end(), name) == internal.end()) return true;
    if (checkingCompilerGenerated) return true;
    if (std::find(intrinsicSources.begin(), intrinsicSources.end(), currentLoc.file) != intrinsicSources.end()) return true;
    error("'" + name + "' is an internal runtime API; use the standard-library methods or explicitly import loxis.unsafe");
    return false;
}

TyPtr TypeChecker::parameterType(TyPtr type, size_t index, std::optional<size_t> varargIndex,
                                  std::optional<size_t> kwargsIndex, Scope* sc) {
    if (kwargsIndex && *kwargsIndex == index) {
        auto* map = sc->lookup("HashMap");
        if (!map || !map->ty || map->ty->kind() != TyKind::Class_) {
            error("'**' parameter requires import loxis.collections.map");
            return mkError();
        }
        return std::make_shared<Ty>(TyClass{std::get<TyClass>(*map->ty).def, {mkStr(), type}});
    }
    return callableParamType(type, index, varargIndex);
}

bool TypeChecker::bindArguments(const std::vector<ExprPtr>& args,
    const std::vector<std::optional<std::string>>& argNames,
    const std::vector<std::string>& paramNames, std::optional<size_t> varargIndex,
    std::optional<size_t> kwargsIndex, std::vector<ExprPtr>& arranged, bool diagnose) {
    arranged.assign(paramNames.size(), nullptr);
    ExprArray rest;
    ExprMap keywords;
    std::vector<std::string> seen;
    size_t positional = 0;
    bool named = false;
    const size_t fixed = varargIndex.value_or(kwargsIndex.value_or(paramNames.size()));
    auto fail = [&](const std::string& message) { if (diagnose) error(message); return false; };
    for (size_t i = 0; i < args.size(); ++i) {
        auto name = i < argNames.size() ? argNames[i] : std::nullopt;
        if (name && *name == "*") {
            if (named) return fail("positional spread cannot follow a named argument");
            if (!varargIndex || positional < fixed) return fail("positional spread requires a '*' parameter after all fixed positional arguments");
            rest.elems.push_back(args[i]); rest.spreads.push_back(true);
            continue;
        }
        if (name && *name == "**") {
            if (!kwargsIndex) return fail("keyword spread requires a '**' parameter");
            named = true;
            keywords.spreads.push_back(args[i]);
            continue;
        }
        if (name) {
            named = true;
            if (std::find(seen.begin(), seen.end(), *name) != seen.end())
                return fail("argument '" + *name + "' is supplied more than once");
            seen.push_back(*name);
            auto found = std::find(paramNames.begin(), paramNames.begin() + fixed, *name);
            if (found != paramNames.begin() + fixed) {
                size_t target = static_cast<size_t>(found - paramNames.begin());
                if (arranged[target]) return fail("argument '" + *name + "' is supplied more than once");
                arranged[target] = args[i];
            } else if (kwargsIndex) {
                keywords.entries.push_back({std::make_shared<Expr>(ExprLit{*name}), args[i]});
            } else return fail("unknown named argument '" + *name + "'");
        } else {
            if (named) return fail("positional argument cannot follow a named argument");
            if (positional < fixed) arranged[positional++] = args[i];
            else if (varargIndex) { rest.elems.push_back(args[i]); rest.spreads.push_back(false); }
            else return fail("too many positional arguments");
        }
    }
    if (varargIndex) arranged[*varargIndex] = std::make_shared<Expr>(std::move(rest));
    if (kwargsIndex) {
        const bool hasSpread = !keywords.spreads.empty();
        for (size_t i = 0; i < fixed; ++i)
            if (arranged[i]) keywords.reservedKeys.push_back(paramNames[i]);
        keywords.argumentPack = true;
        auto packed = std::make_shared<Expr>(std::move(keywords));
        arranged[*kwargsIndex] = packed;
        for (size_t i = 0; i < fixed; ++i) {
            if (arranged[i]) continue;
            if (!hasSpread) return fail("missing argument '" + paramNames[i] + "'");
            auto key = std::make_shared<Expr>(ExprLit{paramNames[i]});
            auto removed = std::make_shared<Expr>(ExprMethodCall{packed, "remove", {key}});
            auto present = std::make_shared<Expr>(ExprMethodCall{packed, "containsKey", {key}});
            auto message = std::make_shared<Expr>(ExprLit{std::string("missing argument '") + paramNames[i] + "' from keyword spread"});
            Path requirePath; requirePath.segs.push_back("require");
            auto check = std::make_shared<Expr>(ExprCall{std::make_shared<Expr>(ExprPath{requirePath}), {present, message}});
            auto value = std::make_shared<Expr>(ExprMethodCall{removed, "valueOrFail", {}});
            ExprBlock block;
            block.stmts.push_back(std::make_shared<Stmt>(StmtExpr{check, true}));
            block.tail = value;
            arranged[i] = std::make_shared<Expr>(std::move(block));
        }
    }
    for (size_t i = 0; i < fixed; ++i)
        if (!arranged[i]) return fail("missing argument '" + paramNames[i] + "'");
    return true;
}

void TypeChecker::error(const std::string& msg) {
    SourceLocation end = currentEndLoc;
    if (end.file.empty() || end.line != currentLoc.line || end.col <= currentLoc.col) {
        end = currentLoc;
        ++end.col;
    }
    errors.push_back(Diagnostic{
        DiagnosticSeverity::Error, DiagnosticKind::Type, msg,
        LabeledSpan{SourceSpan{currentLoc, end}, msg}
    });
}

void TypeChecker::error(const std::string& msg, std::string primaryLabel, std::string help) {
    SourceLocation end = currentEndLoc;
    if (end.file.empty() || end.line != currentLoc.line || end.col <= currentLoc.col) {
        end = currentLoc;
        ++end.col;
    }
    Diagnostic diagnostic{
        DiagnosticSeverity::Error, DiagnosticKind::Type, msg,
        LabeledSpan{SourceSpan{currentLoc, end}, std::move(primaryLabel)}
    };
    diagnostic.help = std::move(help);
    errors.push_back(std::move(diagnostic));
}

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

static TyPtr promotedNumericType(TyPtr a, TyPtr b) {
    if (!a || !b) return mkError();
    if (a->kind() == TyKind::Double || b->kind() == TyKind::Double) return mkDouble();
    if (a->kind() == TyKind::Float || b->kind() == TyKind::Float) return mkDouble();
    if (a->kind() == TyKind::Long || b->kind() == TyKind::Long) return mkLong();
    return mkInt();
}

static bool isBuiltinValueName(const std::string& name) {
    return findNativeBuiltin(name) || name == "print" || name == "println" || name == "write" ||
        name == "error" || name == "syscall" || name == "alloc" ||
        name == "halloc" || name == "hread" || name == "hload" ||
        name == "hset" || name == "hstore" || name == "hfree" ||
        name == "strlen" || name == "s2i" || name == "i2s" ||
        name == "strchar" || name == "strslice" || name == "strindexof" ||
        name == "nativeFileReadText" || name == "nativeFileWriteText" ||
        name == "nativeFileAppendText" || name == "nativeFileRemove" ||
        name == "itof" || name == "ftoi" || name == "fadd" ||
        name == "fsub" || name == "fmul" || name == "fdiv" ||
        name == "fneg";
}

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
    if (a->kind() == TyKind::Array && b->kind() == TyKind::Array) {
        const auto& actual = std::get<TyArray>(*a);
        const auto& expected = std::get<TyArray>(*b);
        return (expected.len == 0 || actual.len == expected.len) &&
               isSubtype(actual.elem, expected.elem);
    }
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
            def = def->superDef;
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
namespace {

// Every member name reachable on `ty`, walking the class's super chain. Only
// built when --trace is on: this is what turns "not found on type X" into
// something you can act on.
std::vector<std::string> memberNamesOf(const Ty& ty, bool wantMethods) {
    std::vector<std::string> names;
    if (ty.kind() == TyKind::Class_) {
        const auto& cls = std::get<TyClass>(ty);
        for (auto def = cls.def; def; def = def->superDef) {
            if (wantMethods) {
                for (const auto& m : def->methods) names.push_back(m.name);
            } else {
                for (const auto& f : def->fields) names.push_back(f.name);
            }
        }
    } else if (ty.kind() == TyKind::Interface_) {
        const auto& iface = std::get<TyInterface>(ty);
        if (wantMethods) {
            for (const auto& m : iface.def->methods) names.push_back(m.name);
        }
    }
    return names;
}

} // namespace

const MethodInfo* TypeChecker::lookupMethod(TyPtr recv, const std::string& name, size_t arity) {
    if (!recv) return nullptr;
    // Strip nullable for lookup
    TyPtr inner = recv->nullable ? unwrapNullable(recv) : recv;
    if (inner->kind() == TyKind::Class_) {
        auto& cls = std::get<TyClass>(*inner);
        for (const auto& m : cls.def->methods) {
            if (m.name == name && (m.arity == arity || m.varargIndex || m.kwargsIndex)) {
                if (m_trace)
                    m_trace->lookup("method " + name + "/" + std::to_string(arity),
                                    inner->toString(), memberNamesOf(*inner, true), true);
                return &m;
            }
        }
    } else if (inner->kind() == TyKind::Interface_) {
        auto& iface = std::get<TyInterface>(*inner);
        for (const auto& m : iface.def->methods) {
            if (m.name == name && (m.arity == arity || m.varargIndex || m.kwargsIndex)) {
                if (m_trace)
                    m_trace->lookup("method " + name + "/" + std::to_string(arity),
                                    inner->toString(), memberNamesOf(*inner, true), true);
                return &m;
            }
        }
    }
    if (m_trace)
        m_trace->lookup("method " + name + "/" + std::to_string(arity),
                        inner->toString(), memberNamesOf(*inner, true), false);
    return nullptr;
}

const FieldInfo* TypeChecker::lookupField(TyPtr recv, const std::string& name) {
    if (!recv) return nullptr;
    TyPtr inner = recv->nullable ? unwrapNullable(recv) : recv;
    if (inner->kind() == TyKind::Class_) {
        auto& cls = std::get<TyClass>(*inner);
        for (const auto& f : cls.def->fields) {
            if (f.name == name && f.type) {
                if (m_trace)
                    m_trace->lookup("field " + name, inner->toString(),
                                    memberNamesOf(*inner, false), true);
                return &f;
            }
        }
        auto super = cls.def->superDef;
        while (super) {
            for (const auto& f : super->fields) {
                if (f.name == name && f.type) {
                    if (m_trace)
                        m_trace->lookup("field " + name, inner->toString(),
                                        memberNamesOf(*inner, false), true);
                    return &f;
                }
            }
            super = super->superDef;
        }
    }
    if (m_trace)
        m_trace->lookup("field " + name, inner->toString(),
                        memberNamesOf(*inner, false), false);
    return nullptr;
}

bool TypeChecker::isMutablePlace(ExprPtr e, Scope* sc) {
    if (!e) return false;
    return std::visit([&](auto&& n) -> bool {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ExprPath>) {
            // A bare name is assignable only if it is bound by `var`. Returning
            // true unconditionally is what let `val x = 1; x = 2` compile and
            // silently rebind; it also let top-level `var` assignments reach
            // lowering, which had no store for them.
            if (n.path.segs.size() != 1 || !n.path.args.empty()) return false;
            const Symbol* sym = sc->lookup(n.path.segs[0]);
            return sym && sym->isMutable;
        } else if constexpr (std::is_same_v<T, ExprDeref>) {
            TyPtr t = checkExpr(n.expr, sc);
            if (t->kind() == TyKind::Ref) return std::get<TyRef>(*t).mut;
            if (t->kind() == TyKind::RawPtr) return std::get<TyRawPtr>(*t).mut;
            return false;
        } else if constexpr (std::is_same_v<T, ExprField>) {
            // `val` only makes the binding immutable, not the object it refers
            // to: `val o = Foo(); o.field = 1` and `val a = [1]; a[0] = 2` stay
            // legal. So the base has to be a *place*, but need not be mutable.
            return isPlace(n.obj, sc);
        } else if constexpr (std::is_same_v<T, ExprIndex>) {
            return isPlace(n.obj, sc);
        } else {
            return false;
        }
    }, *e);
}

bool TypeChecker::isPlace(ExprPtr e, Scope* sc) {
    if (!e) return false;
    return std::visit([&](auto&& n) -> bool {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ExprPath>) {
            return n.path.segs.size() == 1 && n.path.args.empty();
        } else if constexpr (std::is_same_v<T, ExprField>) {
            return isPlace(n.obj, sc);
        } else if constexpr (std::is_same_v<T, ExprIndex>) {
            return isPlace(n.obj, sc);
        } else if constexpr (std::is_same_v<T, ExprDeref>) {
            return true;
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
            // `n.mut` is set by the parser for `var`, so this is what makes a
            // `var` binding assignable and a `val` binding not.
            if (auto* sym = sc->lookupLocal(n.name)) sym->isMutable = n.mut;
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
            if (expected && expected->kind() == TyKind::Class_) {
                auto& cls = std::get<TyClass>(*expected);
                if (cls.def->isEnum) {
                    const std::string& variantName = n.path.segs.back();
                    for (const auto& variant : cls.def->variants) {
                        if (variant.name == variantName) {
                            n.variantIndex = variant.discriminant;
                            return expected;
                        }
                    }
                }
            }
            return expected ? expected : mkError();
        } else if constexpr (std::is_same_v<T, PatEnum>) {
            if (expected && expected->kind() == TyKind::Class_) {
                auto& cls = std::get<TyClass>(*expected);
                if (cls.def->isEnum) {
                    const std::string& variantName = n.path.segs.back();
                    for (const auto& variant : cls.def->variants) {
                        if (variant.name == variantName) {
                            n.variantIndex = variant.discriminant;
                            if (variant.fields.size() != n.elems.size()) {
                                error("enum pattern arity mismatch");
                                return expected;
                            }
                        for (size_t i = 0; i < n.elems.size(); ++i) {
                                TyPtr elemTy = variant.fields[i].type ? variant.fields[i].type : nullptr;
                                if (elemTy) elemTy = substituteType(elemTy, classSubstMap(expected));
                                checkPat(n.elems[i], elemTy, sc);
                            }
                            return expected;
                        }
                    }
                }
            }
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
TyPtr TypeChecker::checkClosure(ExprClosure& n, Scope* sc, TyPtr expectedFn) {
    const TyFn* expected = nullptr;
    if (expectedFn && expectedFn->kind() == TyKind::Fn) {
        expected = &std::get<TyFn>(*expectedFn);
        if (expected->params.size() != n.params.size()) {
            error("closure arity mismatch");
            expected = nullptr;
        }
    }

    Scope closureScope(sc);
    std::vector<TyPtr> paramTys;
    for (size_t i = 0; i < n.params.size(); ++i) {
        TyPtr paramTy = nullptr;
        if (expected && i < expected->params.size()) {
            paramTy = expected->params[i];
        } else if (n.params[i].second) {
            paramTy = resolveType(*n.params[i].second, sc);
        } else {
            paramTy = freshInfer();
        }
        paramTys.push_back(paramTy);
        closureScope.declare(n.params[i].first, SymbolKind::Value, nullptr, paramTy);
    }

    TyPtr annotatedRet = n.ret ? resolveType(*n.ret, sc) : nullptr;
    TyPtr expectedRet = expected ? expected->ret : nullptr;
    TyPtr targetRet = annotatedRet ? annotatedRet : expectedRet;

    n.captures.clear();
    n.envTypeId = ~0u;

    TyPtr savedRet = currentRetTy;
    currentRetTy = targetRet;
    closureScopes.push_back({&closureScope, &n});
    TyPtr bodyTy = n.body ? checkExpr(n.body, &closureScope) : mkUnit();
    auto returns = std::move(closureScopes.back().returns);
    closureScopes.pop_back();
    currentRetTy = savedRet;

    TyPtr retTy = targetRet ? targetRet : bodyTy;
    if (!targetRet && !returns.empty()) {
        retTy = returns.front();
        for (const auto& returned : returns)
            if (!sameType(returned, retTy)) error("closure return type mismatch");
        std::function<bool(const ExprPtr&)> exits = [&](const ExprPtr& expr) -> bool {
            if (!expr) return false;
            if (std::holds_alternative<ExprReturn>(*expr) || std::holds_alternative<ExprThrow>(*expr)) return true;
            if (auto* branch = std::get_if<ExprIf>(expr.get()))
                return exits(branch->then_) && exits(branch->else_);
            if (auto* block = std::get_if<ExprBlock>(expr.get())) {
                for (const auto& stmt : block->stmts)
                    if (auto* value = std::get_if<StmtExpr>(stmt.get()); value && exits(value->expr)) return true;
                return exits(block->tail);
            }
            return false;
        };
        if (bodyTy && !bodyTy->isNever() && !exits(n.body) && !isSubtype(bodyTy, retTy))
            error("closure return type mismatch");
    }
    if (targetRet && bodyTy && !bodyTy->isNever() && !isSubtype(bodyTy, targetRet)) {
        error("closure return type mismatch");
    }

    n.resolvedParamTys = paramTys;
    n.resolvedRetTy = retTy;
    if (!n.captures.empty()) {
        n.envTypeId = 0x40000000u + closureEnvTypeCounter++;
    }
    if (n.loweredName.empty()) {
        n.loweredName = "__lambda_" + std::to_string(closureCounter++);
    }
    return std::make_shared<Ty>(TyFn{paramTys, retTy});
}

bool TypeChecker::inferCall(TyFn& signature, const std::vector<std::string>& names,
                            const std::vector<ExprPtr>& args, Scope* sc,
                            std::vector<TyPtr>& typeArgs, TyPtr expectedTy, size_t offset) {
    if (signature.params.size() != args.size() + offset) return false;
    std::unordered_map<std::string, TyPtr> bindings;
    auto variable = [&](const std::string& name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    std::function<bool(TyPtr, TyPtr)> constrain;
    constrain = [&](TyPtr formal, TyPtr actual) -> bool {
        if (!formal || !actual || actual->isError()) return false;
        if (actual->isNever() || actual->isNull()) return true;
        if (formal->kind() == TyKind::Param && variable(std::get<TyParam>(*formal).name)) {
            TyPtr value = actual;
            if (formal->nullable) { value = std::make_shared<Ty>(*actual); value->nullable = false; }
            auto [it, inserted] = bindings.emplace(std::get<TyParam>(*formal).name, value);
            return inserted || sameType(it->second, value);
        }
        if (formal->kind() != actual->kind()) return isSubtype(actual, formal);
        return std::visit([&](const auto& f) -> bool {
            using F = std::decay_t<decltype(f)>;
            const auto* a = std::get_if<F>(actual.get());
            if (!a) return false;
            auto vectors = [&](const auto& fs, const auto& as) {
                if (fs.size() != as.size()) return false;
                for (size_t i = 0; i < fs.size(); ++i) if (!constrain(fs[i], as[i])) return false;
                return true;
            };
            if constexpr (std::is_same_v<F, TyClass> || std::is_same_v<F, TyInterface> || std::is_same_v<F, TyAdt>)
                return f.def == a->def && vectors(f.args, a->args);
            else if constexpr (std::is_same_v<F, TyFn>)
                return vectors(f.params, a->params) && constrain(f.ret, a->ret);
            else if constexpr (std::is_same_v<F, TyTuple>) return vectors(f.elems, a->elems);
            else if constexpr (std::is_same_v<F, TyArray>) return (f.len == 0 || f.len == a->len) && constrain(f.elem, a->elem);
            else if constexpr (std::is_same_v<F, TySlice>) return constrain(f.elem, a->elem);
            else if constexpr (std::is_same_v<F, TyRef> || std::is_same_v<F, TyRawPtr>)
                return f.mut == a->mut && constrain(f.elem, a->elem);
            else return isSubtype(actual, formal);
        }, *formal);
    };
    std::function<bool(TyPtr)> unresolved = [&](TyPtr ty) -> bool {
        if (!ty) return false;
        return std::visit([&](const auto& t) -> bool {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, TyParam>) return variable(t.name) && !bindings.count(t.name);
            else if constexpr (std::is_same_v<T, InferVar>) return true;
            else if constexpr (std::is_same_v<T, TyFn>) {
                for (auto p : t.params) if (unresolved(p)) return true;
                return unresolved(t.ret);
            } else if constexpr (std::is_same_v<T, TyClass> || std::is_same_v<T, TyInterface> || std::is_same_v<T, TyAdt>) {
                for (auto a : t.args) if (unresolved(a)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, TyTuple>) {
                for (auto e : t.elems) if (unresolved(e)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, TyArray> || std::is_same_v<T, TySlice> || std::is_same_v<T, TyRef> || std::is_same_v<T, TyRawPtr>)
                return unresolved(t.elem);
            else return false;
        }, *ty);
    };
    std::vector<TyPtr> actuals(args.size());
    auto emptyCollection = [](const ExprPtr& arg) {
        if (auto* array = std::get_if<ExprArray>(arg.get())) return array->elems.empty();
        if (auto* map = std::get_if<ExprMap>(arg.get())) return map->entries.empty() && map->spreads.empty();
        return false;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        if (emptyCollection(args[i])) continue;
        if (args[i] && !std::holds_alternative<ExprClosure>(*args[i])) {
            actuals[i] = checkExpr(args[i], sc, unresolved(signature.params[i + offset]) ? nullptr :
                                    substituteType(signature.params[i + offset], bindings));
            if (!constrain(signature.params[i + offset], actuals[i])) return false;
        }
    }
    // A discarded/void result carries no information about a generic argument.
    if (expectedTy && !expectedTy->isUnit() && unresolved(signature.ret) &&
        !constrain(signature.ret, expectedTy)) return false;
    // Infer ordinary arguments first, then contextual lambda inputs and outputs.
    for (size_t i = 0; i < args.size(); ++i) {
        if (actuals[i]) continue;
        if (emptyCollection(args[i])) continue;
        auto* closure = args[i] ? std::get_if<ExprClosure>(args[i].get()) : nullptr;
        TyPtr formal = signature.params[i + offset];
        if (!closure || formal->kind() != TyKind::Fn) return false;
        auto contextual = std::get<TyFn>(*substituteType(formal, bindings));
        if (contextual.params.size() != closure->params.size()) return false;
        for (size_t j = 0; j < contextual.params.size(); ++j) {
            if (closure->params[j].second) {
                auto annotation = resolveType(*closure->params[j].second, sc);
                if (!constrain(std::get<TyFn>(*formal).params[j], annotation)) return false;
                contextual.params[j] = annotation;
            } else if (unresolved(std::get<TyFn>(*formal).params[j])) return false;
        }
        if (unresolved(std::get<TyFn>(*formal).ret)) contextual.ret = nullptr;
        actuals[i] = checkClosure(*closure, sc, std::make_shared<Ty>(contextual));
        if (!constrain(formal, actuals[i])) return false;
    }
    typeArgs.clear();
    for (const auto& name : names) {
        auto it = bindings.find(name);
        if (it == bindings.end() || it->second->kind() == TyKind::Infer) return false;
        typeArgs.push_back(it->second);
    }
    signature = std::get<TyFn>(*substituteType(std::make_shared<Ty>(signature), bindings));
    for (size_t i = 0; i < args.size(); ++i) {
        if (!actuals[i]) actuals[i] = checkExpr(args[i], sc, signature.params[i + offset]);
        if (!isSubtype(actuals[i], signature.params[i + offset])) return false;
    }
    return true;
}

TyPtr TypeChecker::checkExpr(ExprPtr e, Scope* sc, TyPtr expectedTy) {
    if (!e) { error("null expression"); return mkError(); }
    if (m_checkDepth >= kMaxExprDepth) {
        if (!m_depthErrorReported) {
            m_depthErrorReported = true;
            error("expression is nested too deeply to type-check");
        }
        return mkError();
    }
    struct DepthGuard {
        size_t& depth;
        explicit DepthGuard(size_t& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
    } depthGuard(m_checkDepth);
    SourceLocation previousLoc = currentLoc;
    SourceLocation previousEndLoc = currentEndLoc;
    if (!e->loc.file.empty()) currentLoc = e->loc;
    if (!e->endLoc.file.empty()) currentEndLoc = e->endLoc;
    TyPtr result = std::visit([&](auto&& n) -> TyPtr {
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
            if (!n.path.segs.empty() && !canAccessIntrinsic(n.path.segs.back())) return mkError();
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
                    // Enum variant path: Enum::Variant
                    if (n.path.segs.size() >= 2) {
                        Scope* enumSearch = sc;
                        Symbol* enumSym = nullptr;
                        while (enumSearch) {
                            enumSym = enumSearch->lookup(n.path.segs[0]);
                            if (enumSym) break;
                            enumSearch = enumSearch->parent();
                        }
                        if (enumSym && enumSym->ty && enumSym->ty->kind() == TyKind::Class_) {
                            auto& enumTy = std::get<TyClass>(*enumSym->ty);
                            if (enumTy.def->isEnum) {
                                for (const auto& variant : enumTy.def->variants) {
                                    if (variant.name == n.path.segs.back()) {
                                        sym = enumSym;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    // Back-compat constructor alias: Point_new() -> Point()
                    if (!sym && n.path.segs.size() == 1 && s.size() > 4 && s.ends_with("_new")) {
                        std::string className = s.substr(0, s.size() - 4);
                        Scope* ctorSearch = sc;
                        while (ctorSearch) {
                            sym = ctorSearch->lookup(className);
                            if (sym) break;
                            ctorSearch = ctorSearch->parent();
                        }
                        if (sym && (sym->kind == SymbolKind::Type || sym->kind == SymbolKind::Interface)) {
                            break;
                        }
                        sym = nullptr;
                    }
                    // Fallback: check if this is a field on 'this' in enclosing class scope
                    Scope* fs = sc;
                    while (!sym && fs) {
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
                    if (!sym) {
                        error("unresolved path: " + s); return mkError();
                    }
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
                if (ty->kind() == TyKind::Class_) {
                    auto& cls = std::get<TyClass>(*ty);
                    if (cls.def->isEnum && n.path.segs.size() >= 2) {
                        const std::string& variantName = n.path.segs.back();
                        for (const auto& variant : cls.def->variants) {
                            if (variant.name == variantName) {
                                n.isEnumVariant = true;
                                n.variantIndex = variant.discriminant;
                                n.resolvedTy = ty;
                                return ty;
                            }
                        }
                    }
                }
                return ty;
            }
            if (sym->kind == SymbolKind::Value) {
                TyPtr ty = sym->ty ? sym->ty : mkUnit();
                if (ty && ty->kind() == TyKind::Class_ && n.path.segs.size() >= 2) {
                    auto& cls = std::get<TyClass>(*ty);
                    if (cls.def->isEnum) {
                        const std::string& variantName = n.path.segs.back();
                        for (const auto& variant : cls.def->variants) {
                            if (variant.name == variantName) {
                                n.isEnumVariant = true;
                                n.variantIndex = variant.discriminant;
                                break;
                            }
                        }
                    }
                }
                if (!closureScopes.empty() && n.path.segs.size() == 1) {
                    auto& closureCtx = closureScopes.back();
                    Scope* closureScope = closureCtx.scope;
                    const std::string& valueName = n.path.segs[0];
                    bool localToClosure = closureScope->lookupLocal(valueName) != nullptr;
                    if (!localToClosure && sym->node == nullptr && !isBuiltinValueName(valueName)) {
                        // Closure-convert the referenced variable: find the
                        // innermost closure that already owns it (as a local or an
                        // existing capture), then make every strictly inner closure
                        // capture/forward it down to the referencing one.
                        const int back = static_cast<int>(closureScopes.size()) - 1;
                        int ownerIdx = -1;
                        uint32_t sourceOffset = ~0u;
                        for (int k = back; k >= 0; --k) {
                            if (closureScopes[k].scope->lookupLocal(valueName)) {
                                ownerIdx = k;
                                sourceOffset = ~0u;
                                break;
                            }
                            for (const auto& cap : closureScopes[k].expr->captures) {
                                if (cap.name == valueName) {
                                    ownerIdx = k;
                                    sourceOffset = cap.offset;
                                    break;
                                }
                            }
                            if (ownerIdx == k) break;
                        }
                        for (int k = ownerIdx + 1; k <= back; ++k) {
                            ExprClosure* ce = closureScopes[k].expr;
                            uint32_t thisOffset;
                            auto existing = std::find_if(ce->captures.begin(), ce->captures.end(),
                                [&](const ClosureCapture& c) { return c.name == valueName; });
                            if (existing == ce->captures.end()) {
                                thisOffset = static_cast<uint32_t>(ce->captures.size()) * 9u;
                                ce->captures.push_back({valueName, ty, thisOffset, sourceOffset});
                            } else {
                                thisOffset = existing->offset;
                            }
                            sourceOffset = thisOffset;
                            if (k == back) n.captureOffset = thisOffset;
                        }
                        if (ownerIdx == back) {
                            // Already captured by the innermost closure by an
                            // earlier reference; just point at that slot.
                            for (const auto& cap : closureScopes[back].expr->captures)
                                if (cap.name == valueName) { n.captureOffset = cap.offset; break; }
                        }
                    }
                }
                n.isStaticFunction = sym->node && ty->kind() == TyKind::Fn;
                if (sym->globalSlot != ~0u) n.globalSlot = sym->globalSlot;
                // Check if this is a top-level val/var/const reference. A var with
                // a global slot is excluded: it is read with GLoad, not re-run
                // through its initializer.
                if (sym->isTopLevelProperty && !n.isStaticFunction && n.globalSlot == ~0u) {
                    n.isTopLevelVal = true;
                    n.topLevelGetter = "__val_" + n.path.segs[0];
                    // Note: could distinguish val/var/const prefix but they all
                    // use the same getter convention for now
                }
                // Check if this is a class field — mark for field access lowering
                // A function/local binding shadows a class field with the same name.
                if (n.path.segs.size() == 1 && n.path.args.empty() &&
                    sc->lookupLocal(n.path.segs[0]) == nullptr) {
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
            if (!n.elems.empty()) {
                error("tuple literals are not executable yet");
                return mkError();
            }
            return std::make_shared<Ty>(TyTuple{elems});

        } else if constexpr (std::is_same_v<T, ExprArray>) {
            if (n.elems.empty()) {
                if (expectedTy && expectedTy->kind() == TyKind::Array) {
                    n.resolvedTy = std::make_shared<Ty>(TyArray{std::get<TyArray>(*expectedTy).elem, 0});
                    return n.resolvedTy;
                }
                error("empty array literal requires an explicit type");
                return mkError();
            }
            TyPtr expectedElem = expectedTy && expectedTy->kind() == TyKind::Array
                ? std::get<TyArray>(*expectedTy).elem : nullptr;
            TyPtr first;
            bool spread = false;
            for (size_t i = 0; i < n.elems.size(); ++i) {
                bool expands = i < n.spreads.size() && n.spreads[i];
                TyPtr context = first ? first : expectedElem;
                if (expands && context) context = std::make_shared<Ty>(TyArray{context, 0});
                TyPtr et = checkExpr(n.elems[i], sc, context);
                if (expands) {
                    spread = true;
                    if (et->kind() != TyKind::Array || et->nullable) {
                        error("positional spread requires a non-null array"); return mkError();
                    }
                    et = std::get<TyArray>(*et).elem;
                }
                if (!first) first = et;
                else if (!sameType(et, first)) error("array element type mismatch");
            }
            n.resolvedTy = std::make_shared<Ty>(TyArray{first, spread ? 0 : static_cast<uint64_t>(n.elems.size())});
            return n.resolvedTy;

        } else if constexpr (std::is_same_v<T, ExprMap>) {
            TyPtr expectedKey, expectedValue;
            if (expectedTy && expectedTy->kind() == TyKind::Class_) {
                const auto& map = std::get<TyClass>(*expectedTy);
                if (map.def->name == "HashMap" && map.args.size() == 2) {
                    expectedKey = map.args[0]; expectedValue = map.args[1];
                }
            }
            TyPtr spreadKey, spreadValue;
            for (const auto& source : n.spreads) {
                auto ty = checkExpr(source, sc);
                if (ty->kind() != TyKind::Class_ || ty->nullable) { error("keyword spread requires HashMap<str, T>"); return mkError(); }
                const auto& map = std::get<TyClass>(*ty);
                if (map.def->name != "HashMap" || map.args.size() != 2 || map.args[0]->kind() != TyKind::Str) {
                    error("keyword spread requires HashMap<str, T>"); return mkError();
                }
                if (spreadValue && !sameType(spreadValue, map.args[1])) error("keyword spread value type mismatch");
                spreadKey = map.args[0]; spreadValue = map.args[1];
            }
            if (!expectedValue) { expectedKey = spreadKey; expectedValue = spreadValue; }
            if (n.entries.empty() && !expectedValue) { error("empty map literal requires an explicit type"); return mkError(); }
            TyPtr keyTy = n.entries.empty() ? expectedKey : checkExpr(n.entries[0].key, sc, expectedKey);
            TyPtr valueTy = n.entries.empty() ? expectedValue : checkExpr(n.entries[0].value, sc, expectedValue);
            if (spreadValue && !sameType(spreadValue, valueTy)) error("keyword spread value type mismatch");
            for (size_t i = 1; i < n.entries.size(); ++i) {
                TyPtr nextKey = checkExpr(n.entries[i].key, sc);
                TyPtr nextValue = checkExpr(n.entries[i].value, sc);
                if (!sameType(nextKey, keyTy)) error("map key type mismatch");
                if (!sameType(nextValue, valueTy)) error("map value type mismatch");
            }
            if (keyTy->kind() == TyKind::Str) {
                n.hashFunction = "hashString";
                n.equalFunction = "equalString";
            } else if (keyTy->kind() == TyKind::Int || keyTy->kind() == TyKind::Long) {
                n.hashFunction = "hashInt";
                n.equalFunction = "equalInt";
            } else {
                error("map literal keys currently require int or str");
                return mkError();
            }
            Symbol* mapSymbol = sc->lookup("HashMap");
            if (!mapSymbol || !mapSymbol->ty || mapSymbol->ty->kind() != TyKind::Class_) {
                error("map literal requires import loxis.collections.map");
                return mkError();
            }
            auto mapClass = std::get<TyClass>(*mapSymbol->ty);
            n.keyTy = keyTy;
            n.valueTy = valueTy;
            n.resolvedTy = std::make_shared<Ty>(TyClass{mapClass.def, {keyTy, valueTy}});
            return n.resolvedTy;

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
                    if (vi < cls.def->methods.size()) {
                        const auto& m = cls.def->methods[vi];
                        if (m.isInterfaceDefault)
                            n.vtableMethodNames.push_back(m.defaultIfaceName + "." + m.name);
                        else
                            n.vtableMethodNames.push_back(cls.def->name + "." + m.name);
                    } else
                        n.vtableMethodNames.push_back("");
                }
            }
            return structTy;

        } else if constexpr (std::is_same_v<T, ExprCall>) {
            n.resolvedTypeArgs.clear();
            std::string builtinName;
            if (n.func && std::holds_alternative<ExprPath>(*n.func)) {
                const auto& path = std::get<ExprPath>(*n.func).path;
                if (!path.segs.empty()) builtinName = path.segs.back();
            }

            auto checkBuiltinArgs = [&]() {
                for (const auto& a : n.args) checkExpr(a, sc);
            };
            if (!canAccessIntrinsic(builtinName)) return mkError();
            const std::vector<std::string> intrinsics = {
                "print", "println", "write", "syscall", "alloc", "halloc", "hread", "hload",
                "hset", "hstore", "hfree", "strlen", "s2i", "i2s", "chr", "strchar", "strslice",
                "strindexof", "itof", "ftoi", "fadd", "fsub", "fmul", "fdiv", "fneg"};
            if ((findNativeBuiltin(builtinName) || std::find(intrinsics.begin(), intrinsics.end(), builtinName) != intrinsics.end()) &&
                std::any_of(n.argNames.begin(), n.argNames.end(), [](const auto& name) { return name.has_value(); })) {
                error("compiler intrinsics accept positional arguments only"); return mkError();
            }

            if (const auto* native = findNativeBuiltin(builtinName)) {
                std::vector<TyPtr> argTypes;
                argTypes.reserve(n.args.size());
                for (const auto& arg : n.args) argTypes.push_back(checkExpr(arg, sc));
                if (n.args.size() != static_cast<size_t>(native->arity))
                    error("Wrong number of arguments to " + builtinName);
                auto requireNativeArg = [&](size_t index, TyKind expected, const char* expectedName) {
                    if (index >= argTypes.size()) return;
                    TyPtr actual = argTypes[index];
                    if (!actual || actual->nullable || actual->kind() != expected) {
                        error("argument " + std::to_string(index + 1) + " to " + builtinName +
                              " must be " + expectedName + ", got " +
                              (actual ? actual->toString() : std::string("<unknown>")));
                    }
                };
                switch (native->syscall) {
                    case 50:
                        requireNativeArg(1, TyKind::Str, "str");
                        [[fallthrough]];
                    case 44: case 45: case 47: case 48: case 49:
                    case 51: case 52: case 53: case 54: case 56: case 60:
                    case 81: case 82:
                        requireNativeArg(0, TyKind::Str, "str");
                        break;
                    case 83: case 84:
                        requireNativeArg(0, TyKind::Str, "str");
                        requireNativeArg(1, TyKind::Int, "int");
                        break;
                    case 85: case 88: case 90:
                        requireNativeArg(0, TyKind::Int, "int");
                        break;
                    case 86:
                        requireNativeArg(0, TyKind::Int, "int");
                        requireNativeArg(1, TyKind::Int, "int");
                        break;
                    case 87:
                        requireNativeArg(0, TyKind::Int, "int");
                        requireNativeArg(1, TyKind::Str, "str");
                        break;
                    case 57: case 62:
                        requireNativeArg(0, TyKind::Int, "int");
                        break;
                    case 78:
                        requireNativeArg(1, TyKind::Double, "double");
                        [[fallthrough]];
                    case 70: case 71: case 72: case 73: case 74:
                    case 75: case 76: case 77: case 79: case 80:
                        requireNativeArg(0, TyKind::Double, "double");
                        break;
                    default:
                        break;
                }
                TyPtr result = mkInt();
                if (native->result == NativeReturn::Double) result = mkDouble();
                if (native->result == NativeReturn::String || native->result == NativeReturn::NullableString)
                    result = mkStr();
                if (native->result == NativeReturn::NullableString) result->nullable = true;
                n.resolvedTy = result;
                return result;
            }
            if (builtinName == "print" || builtinName == "println" || builtinName == "write") {
                if (n.args.size() != 1) {
                    error("Wrong number of arguments to " + builtinName);
                } else {
                    TyPtr valueTy = checkExpr(n.args[0], sc);
                    const TyKind kind = valueTy ? valueTy->kind() : TyKind::Error;
                    const bool printable = valueTy && !valueTy->nullable &&
                        (kind == TyKind::Int || kind == TyKind::Long || kind == TyKind::Bool ||
                         kind == TyKind::Char || kind == TyKind::Str);
                    if (!printable) {
                        const std::string typeName = valueTy ? valueTy->toString() : std::string("<unknown>");
                        error("cannot " + builtinName + " `" + typeName + "`",
                              "`" + typeName + "` cannot be printed directly",
                              "convert it to `str` first");
                    }
                }
                n.resolvedTy = mkUnit();
                return mkUnit();
            }
            if (builtinName == "hset" || builtinName == "hstore" || builtinName == "hfree") {
                checkBuiltinArgs();
                n.resolvedTy = mkUnit();
                return mkUnit();
            }
            if (builtinName == "error") {
                checkBuiltinArgs();
                n.resolvedTy = mkNever();
                return mkNever();
            }
            if (builtinName == "syscall" || builtinName == "alloc" ||
                builtinName == "halloc" || builtinName == "hread" ||
                builtinName == "hload" || builtinName == "strlen" ||
                builtinName == "s2i" || builtinName == "ftoi" ||
                builtinName == "strchar" || builtinName == "strindexof" ||
                builtinName == "nativeFileWriteText" ||
                builtinName == "nativeFileAppendText" ||
                builtinName == "nativeFileRemove") {
                checkBuiltinArgs();
                n.resolvedTy = mkInt();
                return mkInt();
            }
            if (builtinName == "i2s" || builtinName == "strslice" ||
                builtinName == "chr" ||
                builtinName == "nativeFileReadText") {
                checkBuiltinArgs();
                n.resolvedTy = mkStr();
                return mkStr();
            }
            if (builtinName == "itof" || builtinName == "fadd" ||
                builtinName == "fsub" || builtinName == "fmul" ||
                builtinName == "fdiv" || builtinName == "fneg") {
                checkBuiltinArgs();
                n.resolvedTy = mkDouble();
                return mkDouble();
            }

            TyPtr callee = checkExpr(n.func, sc);
            if (!callee || callee->isError()) return mkError();

            // Built-in untyped calls (print, println)
            if (callee->isUnit()) {
                for (const auto& a : n.args) checkExpr(a, sc);
                return mkUnit();
            }

            // Constructor call: callee is a class type
            if (callee->kind() == TyKind::Class_) {
                auto cls = std::get<TyClass>(*callee);
                if (cls.def->isEnum && std::any_of(n.argNames.begin(), n.argNames.end(),
                                                  [](const auto& name) { return name.has_value(); })) {
                    error("enum variants accept positional arguments only"); return mkError();
                }
                if (!cls.def->isEnum && !n.argumentsBound) {
                    auto* symbol = sc->lookup(cls.def->name);
                    if (symbol && symbol->node) {
                        auto* item = static_cast<ItemClass*>(symbol->node);
                        std::vector<std::string> names;
                        for (const auto& param : item->primaryCtor) names.push_back(param.name);
                        std::vector<ExprPtr> bound;
                        if (!bindArguments(n.args, n.argNames, names, {}, {}, bound, true)) return mkError();
                        if (bound != n.args) n.evaluationArgs = n.args;
                        n.args = std::move(bound);
                        n.argumentsBound = true;
                    }
                }
                if (cls.args.empty() && !cls.def->params.empty() &&
                    std::holds_alternative<ExprPath>(*n.func) && std::get<ExprPath>(*n.func).path.args.empty()) {
                    std::vector<TyPtr> params;
                    auto* path = std::get_if<ExprPath>(n.func.get());
                    if (path && path->isEnumVariant && path->variantIndex < cls.def->variants.size()) {
                        for (const auto& field : cls.def->variants[path->variantIndex].fields)
                            params.push_back(field.type);
                    } else {
                        Symbol* symbol = sc->lookup(cls.def->name);
                        Scope* owner = sc;
                        while (owner && !owner->childMod(cls.def->name)) owner = owner->parent();
                        if (symbol && symbol->node && owner) {
                            auto* item = static_cast<ItemClass*>(symbol->node);
                            for (const auto& pd : item->primaryCtor)
                                params.push_back(pd.ty ? resolveType(*pd.ty, owner->childMod(cls.def->name)) : mkError());
                        }
                    }
                    std::vector<TyPtr> variables;
                    for (size_t i = 0; i < cls.def->params.size(); ++i)
                        variables.push_back(std::make_shared<Ty>(TyParam{cls.def->params[i], static_cast<uint32_t>(i)}));
                    TyFn ctor{params, std::make_shared<Ty>(TyClass{cls.def, variables})};
                    if (!inferCall(ctor, cls.def->params, n.args, sc, n.resolvedTypeArgs, expectedTy)) {
                        error("cannot infer generic type arguments for " + cls.def->name +
                              ": conflicting types or insufficient information; specify explicit type arguments");
                        return mkError();
                    }
                    callee = ctor.ret;
                    cls = std::get<TyClass>(*callee);
                    if (path) path->resolvedTy = callee;
                    n.resolvedTy = callee;
                    if (!cls.def->isEnum) n.ctorLabel = cls.def->name + ".init";
                    return callee;
                }
                if (n.func && std::holds_alternative<ExprPath>(*n.func)) {
                    auto& pathExpr = std::get<ExprPath>(*n.func);
                    if (pathExpr.isEnumVariant && cls.def->isEnum) {
                        const auto variantIndex = pathExpr.variantIndex;
                        if (variantIndex >= cls.def->variants.size()) {
                            error("invalid enum variant");
                            return mkError();
                        }
                        const auto& variant = cls.def->variants[variantIndex];
                        if (variant.fields.size() != n.args.size()) {
                            error("enum variant arity mismatch");
                            return mkError();
                        }
                        for (size_t i = 0; i < n.args.size(); ++i) {
                            TyPtr argTy = checkExpr(n.args[i], sc);
                            TyPtr fieldTy = variant.fields[i].type ? variant.fields[i].type : mkError();
                            fieldTy = substituteType(fieldTy, classSubstMap(callee));
                            if (!isSubtype(argTy, fieldTy)) {
                                error("enum variant argument type mismatch");
                            }
                        }
                        n.resolvedTypeArgs = cls.args;
                        n.resolvedTy = callee;
                        return callee;
                    }
                }
                for (const auto& a : n.args) checkExpr(a, sc);
                if (!cls.def->isEnum) {
                    n.ctorLabel = cls.def->name + ".init";
                }
                n.resolvedTypeArgs = cls.args; // Concrete generic args
                n.resolvedTy = callee;
                return callee;
            }

            // Function call — possibly overloaded
            if (callee->kind() != TyKind::Fn) { error("call on non-function"); return mkError(); }

            Symbol* callSym = nullptr;
            if (auto* pe = std::get_if<ExprPath>(n.func.get())) {
                Scope* search = sc;
                for (const auto& s : pe->path.segs) {
                    callSym = nullptr;
                    while (search) { callSym = search->lookup(s); if (callSym) break; search = search->parent(); }
                    if (!callSym) break;
                }
            }

            // Collect candidate function types (primary + overloads)
            std::vector<std::pair<TyFn, void*>> candidates;
            candidates.push_back({std::get<TyFn>(*callee), callSym ? callSym->node : nullptr});
            if (callSym) {
                for (auto& ov : callSym->overloads) {
                    if (ov.second && ov.second->kind() == TyKind::Fn) {
                        TyPtr candidateTy = ov.second;
                        if (auto* pe = std::get_if<ExprPath>(n.func.get());
                            pe && !pe->path.args.empty() && ov.first) {
                            auto* item = static_cast<ItemFun*>(ov.first);
                            if (item->generics.size() == pe->path.args.size()) {
                                std::vector<std::string> names;
                                std::vector<TyPtr> typeArgs;
                                for (const auto& generic : item->generics) names.push_back(generic.name);
                                for (const auto& arg : pe->path.args) typeArgs.push_back(resolveType(*arg, sc));
                                candidateTy = substituteType(candidateTy, makeSubstMap(names, typeArgs));
                            }
                        }
                        candidates.push_back({std::get<TyFn>(*candidateTy), ov.first});
                    }
                }
            }

            // Try each candidate; pick the first that matches arity
            TyFn* matchedFn = nullptr;
            void*  matchedNode = nullptr;
            bool inferred = false;
            bool triedInference = false;
            std::vector<TyPtr> knownArgTypes(n.args.size());
            std::vector<ExprPtr> matchedArgs;
            for (auto& cand : candidates) {
                std::vector<ExprPtr> candidateArgs = n.args;
                if (cand.second) {
                    auto* item = static_cast<ItemFun*>(cand.second);
                    if (item->receiverType) continue;
                    if (!n.argumentsBound) {
                        std::vector<std::string> names;
                        for (const auto& param : item->params) names.push_back(param.first);
                        if (!bindArguments(n.args, n.argNames, names, item->varargIndex,
                                           item->kwargsIndex, candidateArgs, candidates.size() == 1)) continue;
                    }
                } else if (!n.argumentsBound && !cand.first.paramNames.empty()) {
                    if (!bindArguments(n.args, n.argNames, cand.first.paramNames, cand.first.varargIndex,
                                       cand.first.kwargsIndex, candidateArgs, true)) continue;
                } else if (!n.argumentsBound && std::any_of(n.argNames.begin(), n.argNames.end(),
                                                           [](const auto& name) { return name.has_value(); })) {
                    error("named arguments require a declared function or method"); return mkError();
                }
                if (cand.first.params.size() != candidateArgs.size()) continue;
                knownArgTypes.assign(candidateArgs.size(), nullptr);
                if (cand.second && std::get_if<ExprPath>(n.func.get()) &&
                    std::get<ExprPath>(*n.func).path.args.empty()) {
                    auto* item = static_cast<ItemFun*>(cand.second);
                    if (!item->generics.empty()) {
                        triedInference = true;
                        std::vector<std::string> names;
                        for (const auto& g : item->generics) names.push_back(g.name);
                        const auto savedErrors = errors.size();
                        if (inferCall(cand.first, names, candidateArgs, sc, n.resolvedTypeArgs, expectedTy) &&
                            errors.size() == savedErrors) {
                            matchedFn = &cand.first;
                            matchedNode = cand.second;
                            inferred = true;
                            matchedArgs = std::move(candidateArgs);
                            break;
                        }
                        errors.resize(savedErrors);
                        n.resolvedTypeArgs.clear();
                        continue;
                    }
                }
                bool compatible = true;
                for (size_t i = 0; i < knownArgTypes.size(); ++i) {
                    if (candidateArgs[i] && !std::holds_alternative<ExprClosure>(*candidateArgs[i]))
                        knownArgTypes[i] = checkExpr(candidateArgs[i], sc, cand.first.params[i]);
                    if (knownArgTypes[i] && !isSubtype(knownArgTypes[i], cand.first.params[i])) {
                        compatible = false;
                        break;
                    }
                }
                if (compatible) {
                    matchedFn = &cand.first;
                    matchedNode = cand.second;
                    matchedArgs = std::move(candidateArgs);
                    break;
                }
            }
            if (!matchedFn) { error(triedInference ? "cannot infer generic type arguments: conflicting types or insufficient information; specify explicit type arguments" : "no matching overload for arity"); return mkError(); }
            if (!n.argumentsBound) {
                if (matchedArgs != n.args) n.evaluationArgs = n.args;
                for (const auto& arg : matchedArgs)
                    if (auto* map = std::get_if<ExprMap>(arg.get()); map && map->argumentPack && !map->spreads.empty())
                        n.evaluationArgs.push_back(arg);
                n.args = std::move(matchedArgs);
                n.argumentsBound = true;
            }

            // Mangle name for overloaded functions: <name>$<arity>
            if (callSym && !callSym->overloads.empty()) {
                n.mangledName = "";
                if (matchedNode) {
                    auto* matchedItem = static_cast<ItemFun*>(matchedNode);
                    n.mangledName = matchedItem->backendName;
                }
                if (n.mangledName.empty() && n.func && std::holds_alternative<ExprPath>(*n.func)) {
                    n.mangledName = overloadBackendName(
                        std::get<ExprPath>(*n.func).path.segs.back(), *matchedFn);
                }
            }

            if (!inferred) for (size_t i = 0; i < n.args.size(); ++i) {
                TyPtr argTy = nullptr;
                if (n.args[i] && std::holds_alternative<ExprClosure>(*n.args[i])) {
                    argTy = checkClosure(std::get<ExprClosure>(*n.args[i]), sc, matchedFn->params[i]);
                } else {
                    argTy = knownArgTypes[i];
                }
                if (!isSubtype(argTy, matchedFn->params[i])) error("call argument type mismatch");
            }
            // Store resolved generic type args from the path
            if (auto* pe = std::get_if<ExprPath>(n.func.get())) {
                if (!pe->path.args.empty()) {
                    for (const auto& arg : pe->path.args) {
                        n.resolvedTypeArgs.push_back(resolveType(*arg, sc));
                    }
                }
            }
            n.resolvedTy = matchedFn->ret;
            return matchedFn->ret;

        } else if constexpr (std::is_same_v<T, ExprMethodCall>) {
            TyPtr recv = checkExpr(n.recv, sc);
            if (!recv || recv->isError()) return mkError();
            if (recv->kind() == TyKind::Array && !recv->nullable && n.method == "size" &&
                n.args.empty() && n.typeArgs.empty()) {
                n.qualifiedName = "__loxis_arrayLen";
                n.resolvedTy = mkInt();
                return n.resolvedTy;
            }

            // Determine dispatch kind for the backend
            TyPtr inner = recv->nullable ? unwrapNullable(recv) : recv;
            if (inner->kind() == TyKind::Interface_) {
                n.isInterfaceDispatch = true;
                auto& iface = std::get<TyInterface>(*inner);
                n.ifaceTypeId = iface.def->typeId;
                for (size_t i = 0; i < iface.def->methods.size(); ++i) {
                    if (iface.def->methods[i].name == n.method) {
                        n.ifaceMethodIndex = static_cast<uint32_t>(i);
                        break;
                    }
                }
            }

            const MethodInfo* mi = lookupMethod(recv, n.method, n.args.size());
            if (mi) {
                if (!n.argumentsBound) {
                    std::vector<ExprPtr> bound;
                    if (!bindArguments(n.args, n.argNames, mi->paramNames, mi->varargIndex,
                                       mi->kwargsIndex, bound, true)) return mkError();
                    if (bound != n.args) n.evaluationArgs = n.args;
                    for (const auto& arg : bound)
                        if (auto* map = std::get_if<ExprMap>(arg.get()); map && map->argumentPack && !map->spreads.empty())
                            n.evaluationArgs.push_back(arg);
                    n.args = std::move(bound);
                    n.argumentsBound = true;
                }
                // Regular method dispatch
                n.vtableIndex = mi->vtableIndex;
                if (inner->kind() == TyKind::Class_) {
                    n.qualifiedName = std::get<TyClass>(*inner).def->name + "." + n.method;
                }

                if (mi->funcType && mi->funcType->kind() == TyKind::Fn) {
                    TyPtr fnTy = mi->funcType;
                    auto subst = typeArgSubstMap(inner);
                    if (!subst.empty()) fnTy = substituteType(fnTy, subst);
                    if (!n.typeArgs.empty()) {
                        if (n.typeArgs.size() != mi->genericParams.size()) {
                            error("generic argument count mismatch for method '" + n.method + "'");
                        } else {
                            n.resolvedTypeArgs.clear();
                            for (const auto& typeArg : n.typeArgs)
                                n.resolvedTypeArgs.push_back(resolveType(*typeArg, sc));
                            fnTy = substituteType(fnTy, makeSubstMap(mi->genericParams, n.resolvedTypeArgs));
                        }
                    } else if (!mi->genericParams.empty()) {
                        auto inferredFn = std::get<TyFn>(*fnTy);
                        if (!inferCall(inferredFn, mi->genericParams, n.args, sc,
                                       n.resolvedTypeArgs, expectedTy, 1)) {
                            error("cannot infer generic type arguments for method '" + n.method +
                                  "': conflicting types or insufficient information; specify explicit type arguments");
                            return mkError();
                        }
                        n.resolvedTy = inferredFn.ret;
                        return n.resolvedTy;
                    }
                    auto& mfn = std::get<TyFn>(*fnTy);
                    size_t selfOffset = 1;
                    if (n.args.size() + selfOffset != mfn.params.size())
                        error("method call arity mismatch for '" + n.method + "'");
                    else {
                        for (size_t i = 0; i < n.args.size(); ++i) {
                            TyPtr expected = mfn.params[i + selfOffset];
                            TyPtr argTy = nullptr;
                            if (n.args[i] && std::holds_alternative<ExprClosure>(*n.args[i])) {
                                argTy = checkClosure(std::get<ExprClosure>(*n.args[i]), sc, expected);
                            } else {
                                argTy = checkExpr(n.args[i], sc, expected);
                            }
                            if (!isSubtype(argTy, expected))
                                error("method argument type mismatch for '" + n.method + "'");
                        }
                    }
                    n.resolvedTy = mfn.ret;
                    return mfn.ret;
                }
                for (const auto& a : n.args) checkExpr(a, sc);
                n.resolvedTy = freshInfer();
                return n.resolvedTy;
            }

            if (const auto* field = lookupField(recv, n.method); field && field->type && field->type->kind() == TyKind::Fn) {
                auto callee = std::make_shared<Expr>(ExprField{n.recv, n.method});
                n.callableField = std::make_shared<Expr>(ExprCall{callee, n.args});
                std::get<ExprCall>(*n.callableField).argNames = n.argNames;
                n.resolvedTy = checkExpr(n.callableField, sc);
                return n.resolvedTy;
            }

            // Not a class method — try extension function lookup
            Symbol* extSym = sc->lookup(n.method);
            if (extSym) {
                // Search overloads for one whose first param matches the receiver
                std::vector<std::pair<void*, TyPtr>> candidates;
                if (extSym->ty && extSym->ty->kind() == TyKind::Fn) {
                    candidates.push_back({extSym->node, extSym->ty});
                }
                for (auto& ov : extSym->overloads) {
                    if (ov.second && ov.second->kind() == TyKind::Fn) {
                        candidates.push_back({ov.first, ov.second});
                    }
                }
                for (auto& cand : candidates) {
                    auto fnTy = std::get<TyFn>(*cand.second);
                    if (fnTy.params.empty()) continue;
                    auto* item = cand.first ? static_cast<ItemFun*>(cand.first) : nullptr;
                    if (!item || !item->receiverType) continue;
                    std::vector<ExprPtr> bound = n.args;
                    if (!n.argumentsBound) {
                        std::vector<std::string> names;
                        for (const auto& param : item->params) names.push_back(param.first);
                        if (!bindArguments(n.args, n.argNames, names, item->varargIndex, item->kwargsIndex, bound)) continue;
                    }
                    // First param is the receiver — check if it matches
                    size_t extArgs = bound.size() + 1; // +1 for receiver
                    if (fnTy.params.size() == extArgs &&
                        isSubtype(recv, fnTy.params[0])) {
                        bool inferredExtension = false;
                        if (!item->generics.empty()) {
                            std::vector<std::string> names;
                            for (const auto& param : item->generics) names.push_back(param.name);
                            if (!n.typeArgs.empty()) {
                                if (n.typeArgs.size() != names.size()) continue;
                                n.resolvedTypeArgs.clear();
                                for (const auto& type : n.typeArgs) n.resolvedTypeArgs.push_back(resolveType(*type, sc));
                                fnTy = std::get<TyFn>(*substituteType(std::make_shared<Ty>(fnTy), makeSubstMap(names, n.resolvedTypeArgs)));
                            } else {
                                const auto savedErrors = errors.size();
                                if (!inferCall(fnTy, names, bound, sc, n.resolvedTypeArgs, expectedTy, 1)) {
                                    errors.resize(savedErrors); continue;
                                }
                                inferredExtension = true;
                            }
                        }
                        if (!n.argumentsBound) {
                            if (bound != n.args) n.evaluationArgs = n.args;
                            for (const auto& arg : bound)
                                if (auto* map = std::get_if<ExprMap>(arg.get()); map && map->argumentPack && !map->spreads.empty())
                                    n.evaluationArgs.push_back(arg);
                            n.args = std::move(bound); n.argumentsBound = true;
                        }
                        // Found matching extension function!
                        // Use the mangled backend name from the ItemFun node
                        std::string backendName = n.method;
                        if (cand.first) {
                            auto* itemFn = static_cast<ItemFun*>(cand.first);
                            if (!itemFn->backendName.empty()) {
                                backendName = itemFn->backendName;
                            }
                        }
                        // Check arg types
                        if (!inferredExtension) for (size_t i = 0; i < extArgs && i < fnTy.params.size(); ++i) {
                            TyPtr argTy;
                            if (i == 0) argTy = recv; // already checked
                            else if (auto* closure = std::get_if<ExprClosure>(n.args[i - 1].get()))
                                argTy = checkClosure(*closure, sc, fnTy.params[i]);
                            else argTy = checkExpr(n.args[i - 1], sc, fnTy.params[i]);
                            if (!isSubtype(argTy, fnTy.params[i]))
                                error("extension function argument type mismatch for '" + n.method + "'");
                        }
                        n.resolvedTy = fnTy.ret;
                        n.qualifiedName = backendName;
                        n.vtableIndex = ~0u;
                        n.isInterfaceDispatch = false;
                        return fnTy.ret;
                    }
                }
            }

            error("method '" + n.method + "' not found on type " + recv->toString());
            return mkError();

        } else if constexpr (std::is_same_v<T, ExprField>) {
            TyPtr base = checkExpr(n.obj, sc);
            if (!base || base->isError()) return mkError();

            const FieldInfo* fi = lookupField(base, n.field);
            if (fi) {
                n.fieldOffset = fi->offset;
                TyPtr ft = fi->type ? fi->type : freshInfer();
                  auto subst = typeArgSubstMap(base);
                  if (!subst.empty()) ft = substituteType(ft, subst);
                  n.resolvedTy = ft;
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
            auto tryOverloadedUnary = [&]() -> TyPtr {
                std::string opName = unaryOperatorName(n.op);
                if (opName.empty()) return nullptr;
                const MethodInfo* mi = lookupMethod(inner, opName, 0);
                if (!mi || !mi->funcType || mi->funcType->kind() != TyKind::Fn) return nullptr;
                auto& fn = std::get<TyFn>(*mi->funcType);
                n.isOverloaded = true;
                n.vtableIndex = mi->vtableIndex;
                n.resolvedTy = fn.ret;
                TyPtr recvInner = inner->nullable ? unwrapNullable(inner) : inner;
                if (recvInner->kind() == TyKind::Interface_) {
                    n.isInterfaceDispatch = true;
                    auto& iface = std::get<TyInterface>(*recvInner);
                    n.ifaceTypeId = iface.def->typeId;
                    for (size_t i = 0; i < iface.def->methods.size(); ++i) {
                        if (iface.def->methods[i].name == opName && iface.def->methods[i].arity == 0) {
                            n.ifaceMethodIndex = static_cast<uint32_t>(i);
                            break;
                        }
                    }
                } else if (recvInner->kind() == TyKind::Class_) {
                    n.qualifiedName = std::get<TyClass>(*recvInner).def->name + "." + opName;
                }
                return n.resolvedTy;
            };
            switch (n.op) {
                case ExprUnary::Neg:
                    if (isNumeric(inner)) { n.resolvedTy = inner; return inner; }
                    if (TyPtr overloaded = tryOverloadedUnary()) return overloaded;
                    error("neg on non-numeric"); return mkError();
                case ExprUnary::Not:
                    if (isBool(inner)) { n.resolvedTy = mkBool(); return mkBool(); }
                    if (TyPtr overloaded = tryOverloadedUnary()) return overloaded;
                    error("not on non-bool"); return mkError();
                case ExprUnary::Deref:
                    if (inner->kind() == TyKind::Ref) { n.resolvedTy = std::get<TyRef>(*inner).elem; return n.resolvedTy; }
                    if (inner->kind() == TyKind::RawPtr) { n.resolvedTy = std::get<TyRawPtr>(*inner).elem; return n.resolvedTy; }
                    error("deref on non-reference"); return mkError();
                case ExprUnary::Ref:
                    n.resolvedTy = std::make_shared<Ty>(TyRef{false, inner});
                    return n.resolvedTy;
                case ExprUnary::RefMut:
                    if (!isMutablePlace(n.operand, sc)) { error("cannot take &mut of immutable place"); return mkError(); }
                    n.resolvedTy = std::make_shared<Ty>(TyRef{true, inner});
                    return n.resolvedTy;
                default:
                    error("unknown unary op"); return mkError();
            }

        } else if constexpr (std::is_same_v<T, ExprBinary>) {
            TyPtr l = checkExpr(n.l, sc);
            TyPtr r = checkExpr(n.r, sc);
            auto tryOverloadedBinary = [&]() -> TyPtr {
                std::string opName = binaryOperatorName(n.op);
                if (opName.empty()) return nullptr;
                const MethodInfo* mi = lookupMethod(l, opName, 1);
                if (!mi || !mi->funcType || mi->funcType->kind() != TyKind::Fn) return nullptr;
                auto& fn = std::get<TyFn>(*mi->funcType);
                if (fn.params.size() < 2) return nullptr;
                if (!isSubtype(r, fn.params[1])) return nullptr;
                n.isOverloaded = true;
                n.vtableIndex = mi->vtableIndex;
                n.resolvedTy = fn.ret;
                TyPtr recvInner = l->nullable ? unwrapNullable(l) : l;
                if (recvInner->kind() == TyKind::Interface_) {
                    n.isInterfaceDispatch = true;
                    auto& iface = std::get<TyInterface>(*recvInner);
                    n.ifaceTypeId = iface.def->typeId;
                    for (size_t i = 0; i < iface.def->methods.size(); ++i) {
                        if (iface.def->methods[i].name == opName && iface.def->methods[i].arity == 1) {
                            n.ifaceMethodIndex = static_cast<uint32_t>(i);
                            break;
                        }
                    }
                } else if (recvInner->kind() == TyKind::Class_) {
                    n.qualifiedName = std::get<TyClass>(*recvInner).def->name + "." + opName;
                }
                if ((n.op == ExprBinary::Eq || n.op == ExprBinary::Ne ||
                     n.op == ExprBinary::Lt || n.op == ExprBinary::Le ||
                     n.op == ExprBinary::Gt || n.op == ExprBinary::Ge) && !isBool(fn.ret)) {
                    error("operator comparison must return bool");
                    return mkError();
                }
                return n.resolvedTy;
            };
            switch (n.op) {
                case ExprBinary::Add:
                    // String concatenation
                    if (l->isStr() && r->isStr()) { n.resolvedTy = mkStr(); return n.resolvedTy; }
                    // Numeric
                    if (isNumeric(l) && isNumeric(r)) { n.resolvedTy = promotedNumericType(l, r); return n.resolvedTy; }
                    if (TyPtr overloaded = tryOverloadedBinary()) return overloaded;
                    error("numeric op on non-numeric"); return mkError();
                case ExprBinary::Sub: case ExprBinary::Mul:
                case ExprBinary::Div:
                    if (isNumeric(l) && isNumeric(r)) { n.resolvedTy = promotedNumericType(l, r); return n.resolvedTy; }
                    if (TyPtr overloaded = tryOverloadedBinary()) return overloaded;
                    error("numeric op on non-numeric"); return mkError();
                case ExprBinary::Rem:
                    if (isInteger(l) && isInteger(r) && sameType(l, r)) { n.resolvedTy = l; return n.resolvedTy; }
                    if (TyPtr overloaded = tryOverloadedBinary()) return overloaded;
                    if (!isInteger(l) || !isInteger(r)) { error("remainder op on non-integer"); return mkError(); }
                    error("remainder op type mismatch"); return mkError();
                case ExprBinary::Shl: case ExprBinary::Shr:
                case ExprBinary::BitAnd: case ExprBinary::BitOr: case ExprBinary::BitXor:
                    if (!isInteger(l) || !isInteger(r)) { error("bitwise op on non-integer"); return mkError(); }
                    if (!sameType(l, r)) { error("bitwise op type mismatch"); return mkError(); }
                    n.resolvedTy = l;
                    return l;
                case ExprBinary::Eq: case ExprBinary::Ne:
                    // Allow comparison with null: T? == null, null == T?
                    if (l->isNull() || r->isNull()) {
                        if (!l->isNull() && !l->nullable) error("cannot compare non-nullable with null");
                        if (!r->isNull() && !r->nullable) error("cannot compare non-nullable with null");
                        n.resolvedTy = mkBool();
                        return n.resolvedTy;
                    }
                    if (TyPtr overloaded = tryOverloadedBinary()) return overloaded;
                    if (sameType(l, r) || isSubtype(l, r) || isSubtype(r, l)) {
                        n.resolvedTy = mkBool();
                        return n.resolvedTy;
                    }
                    error("comparison type mismatch");
                    return mkError();
                case ExprBinary::Lt: case ExprBinary::Le:
                case ExprBinary::Gt: case ExprBinary::Ge:
                    if (TyPtr overloaded = tryOverloadedBinary()) return overloaded;
                    if (sameType(l, r)) { n.resolvedTy = mkBool(); return n.resolvedTy; }
                    error("comparison type mismatch"); return mkError();
                case ExprBinary::And: case ExprBinary::Or:
                    if (!isBool(l) || !isBool(r)) { error("logical op on non-bool"); return mkError(); }
                    return mkBool();
                default:
                    error("unknown binary op"); return mkError();
            }

        } else if constexpr (std::is_same_v<T, ExprAssign>) {
            if (!isMutablePlace(n.lhs, sc)) { error("assignment to immutable place"); return mkError(); }
            TyPtr lhs = checkExpr(n.lhs, sc);
            TyPtr rhs = checkExpr(n.rhs, sc, lhs);
            switch (n.op) {
                case ExprAssign::Set:
                    if (!isSubtype(rhs, lhs)) {
                        std::string target;
                        if (const auto* path = std::get_if<ExprPath>(n.lhs.get()); path && !path->path.segs.empty()) target = path->path.segs.back();
                        error("assignment type mismatch for " + target + ": " + rhs->toString() + " to " + lhs->toString());
                    }
                    break;
                default:
                    if (!isNumeric(lhs) || !sameType(lhs, rhs)) error("compound assignment type error");
                    break;
            }
            return mkUnit();

        } else if constexpr (std::is_same_v<T, ExprBlock>) {
            Scope blockScope(sc);
            for (const auto& stmt : n.stmts) checkStmt(stmt, &blockScope);
            if (n.tail) return checkExpr(n.tail, &blockScope, expectedTy);
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
            TyPtr t = checkExpr(n.then_, &thenScope, expectedTy);

            if (n.else_) {
                // Else branch: use inverted refinements
                Scope elseScope(sc);
                if (invertRefinements) {
                    applyRefinements(&elseScope, refinements);
                }
                TyPtr el = checkExpr(n.else_, &elseScope, expectedTy);
                if (isSubtype(t, el)) return el;
                if (!isSubtype(el, t)) { error("if/else branch type mismatch: " + t->toString() + " and " + el->toString()); return mkError(); }
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
            n.iterResolvedTy = iter;
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
            } else if (lookupMethod(iter, "next", 0)) {
                n.iteratorNext = std::make_shared<Expr>(ExprMethodCall{n.iter, "next", {}});
                TyPtr nextTy = checkExpr(n.iteratorNext, sc);
                if (!nextTy || nextTy->kind() != TyKind::Class_ ||
                    std::get<TyClass>(*nextTy).def->name != "Optional" ||
                    std::get<TyClass>(*nextTy).args.size() != 1) {
                    error("iterator next() must return Optional<T>");
                    return mkError();
                }
                n.iterKind = ExprFor::IterKind::Iterator;
                elemTy = std::get<TyClass>(*nextTy).args[0];
                n.iteratorElementTy = elemTy;
            } else if (lookupMethod(iter, "size", 0) && lookupMethod(iter, "get", 1)) {
                n.iterKind = ExprFor::IterKind::Iterable;
                if (iter->kind() == TyKind::Class_) {
                    n.iterClassName = std::get<TyClass>(*iter).def->name;
                }
                const MethodInfo* getM = lookupMethod(iter, "get", 1);
                if (getM && getM->funcType && getM->funcType->kind() == TyKind::Fn) {
                    TyPtr ret = std::get<TyFn>(*getM->funcType).ret;
                    auto subst = classSubstMap(iter);
                    elemTy = subst.empty() ? ret : substituteType(ret, subst);
                }
            } else {
                error("for-in requires range, array, next() iterator, or size()/get() collection");
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

            // Enum exhaustiveness. A `when` produces `result`, so an arm that is
            // missing simply falls off the end and hands back whatever the last
            // matched arm left behind -- how a function declared to return `str`
            // quietly returned 0. Only enforced where the value is consumed, so
            // `when` keeps working as an ordinary statement.
            const TyClass* scrutEnum = (scrut && scrut->kind() == TyKind::Class_)
                ? &std::get<TyClass>(*scrut) : nullptr;
            const bool isEnumScrut = scrutEnum && scrutEnum->def && scrutEnum->def->isEnum &&
                                     !scrutEnum->def->variants.empty();
            const bool valuePosition = expectedTy != nullptr || m_stmtExprDepth == 0;
            bool coversEverything = false;
            std::vector<bool> covered(isEnumScrut ? scrutEnum->def->variants.size() : 0, false);

            for (const auto& arm : n.arms) {
                Scope armScope(sc);
                if (auto* cond = std::get_if<ExprPtr>(&arm.condition)) {
                    if (*cond) {
                        TyPtr condTy = checkExpr(*cond, sc, scrut);
                        if (scrut && !sameType(condTy, scrut))
                            error("when condition type does not match scrutinee");
                        bool invert = false;
                        auto refs = extractRefinements(**cond, sc, invert);
                        if (!invert) applyRefinements(&armScope, refs);
                    }
                } else if (auto* pat = std::get_if<PatPtr>(&arm.condition)) {
                    if (*pat) {
                        checkPat(*pat, scrut, &armScope);
                        // checkPat has filled in variantIndex by now.
                        if (isEnumScrut) {
                            std::visit([&](auto&& p) {
                                using P = std::decay_t<decltype(p)>;
                                if constexpr (std::is_same_v<P, PatWild> || std::is_same_v<P, PatBind>) {
                                    coversEverything = true;
                                } else if constexpr (std::is_same_v<P, PatEnum> || std::is_same_v<P, PatStruct>) {
                                    if (p.variantIndex < covered.size()) covered[p.variantIndex] = true;
                                }
                            }, **pat);
                        }
                    }
                }
                if (arm.isElse) coversEverything = true;

                TyPtr armTy = arm.body ? checkExpr(arm.body, &armScope, expectedTy) : mkUnit();
                if (!result) result = armTy;
                else if (!sameType(result, armTy)) {
                    if (isSubtype(armTy, result)) {
                        // Keep the earlier, wider result type.
                    } else if (isSubtype(result, armTy)) {
                        result = armTy;
                    } else {
                        error("when arm type mismatch: " + result->toString() + " and " + armTy->toString());
                    }
                }
            }

            if (isEnumScrut && valuePosition && !coversEverything) {
                std::string missing;
                for (size_t i = 0; i < covered.size(); ++i) {
                    if (covered[i]) continue;
                    if (!missing.empty()) missing += ", ";
                    missing += scrutEnum->def->variants[i].name;
                }
                if (!missing.empty()) {
                    error("when is not exhaustive: `" + scrut->toString() + "` has no arm for " + missing,
                          "missing arm(s): " + missing,
                          "add the missing arm(s) or an `else` branch");
                }
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
            if (!currentRetTy && !closureScopes.empty()) {
                TyPtr returned = n.expr ? checkExpr(n.expr, sc) : mkUnit();
                closureScopes.back().returns.push_back(returned);
                return mkNever();
            }
            if (!currentRetTy) { error("return outside function"); return mkError(); }
            if (n.expr) {
                TyPtr ret = checkExpr(n.expr, sc, currentRetTy);
                if (!isSubtype(ret, currentRetTy)) error("return type mismatch");
            } else {
                if (!currentRetTy->isUnit()) error("return unit in non-unit function");
            }
            return mkNever();

        } else if constexpr (std::is_same_v<T, ExprThrow>) {
            if (!n.expr) return mkError();
            TyPtr thrown = checkExpr(n.expr, sc);
            Symbol* exceptionSym = sc->lookup("Exception");
            if (!exceptionSym || !exceptionSym->ty || exceptionSym->ty->kind() != TyKind::Class_) {
                error("throw requires the standard Exception type");
                return mkError();
            }
            if (!isSubtype(thrown, exceptionSym->ty))
                error("throw requires Exception, got " + thrown->toString());
            return mkNever();

        } else if constexpr (std::is_same_v<T, ExprTryCatch>) {
            Symbol* exceptionSym = sc->lookup("Exception");
            TyPtr result = checkExpr(n.body, sc);
            std::vector<TyPtr> priorCatchTypes;
            for (auto& clause : n.catches) {
                TyPtr catchTy = resolveType(*clause.ty, sc);
                clause.resolvedTy = catchTy;
                if (!exceptionSym || !exceptionSym->ty || !isSubtype(catchTy, exceptionSym->ty))
                    error("catch type must extend Exception");
                for (const auto& prior : priorCatchTypes) {
                    if (isSubtype(catchTy, prior)) {
                        error("unreachable catch for " + catchTy->toString() +
                              "; an earlier catch already handles it");
                        break;
                    }
                }
                priorCatchTypes.push_back(catchTy);
                if (catchTy && catchTy->kind() == TyKind::Class_)
                    clause.typeId = std::get<TyClass>(*catchTy).def->typeId;
                Scope catchScope(sc);
                catchScope.declare(clause.name, SymbolKind::Value, nullptr, catchTy);
                TyPtr arm = checkExpr(clause.body, &catchScope);
                if (result->isNever()) result = arm;
                else if (!arm->isNever() && !isSubtype(arm, result)) {
                    if (isSubtype(result, arm)) result = arm;
                    else error("try/catch branch type mismatch");
                }
            }
            if (n.finallyBody) checkExpr(n.finallyBody, sc);
            n.resolvedTy = result;
            return result;

        } else if constexpr (std::is_same_v<T, ExprClosure>) {
            return checkClosure(n, sc, expectedTy);

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
            const MethodInfo* mi = lookupMethod(inner, n.method, n.args.size());
            if (inner->kind() == TyKind::Interface_) {
                n.isInterfaceDispatch = true;
                auto& iface = std::get<TyInterface>(*inner);
                n.ifaceTypeId = iface.def->typeId;
                for (size_t i = 0; i < iface.def->methods.size(); ++i) {
                    if (iface.def->methods[i].name == n.method) {
                        n.ifaceMethodIndex = static_cast<uint32_t>(i);
                        break;
                    }
                }
            }
            if (mi && mi->funcType && mi->funcType->kind() == TyKind::Fn) {
                n.vtableIndex = mi->vtableIndex;
                if (inner->kind() == TyKind::Class_) {
                    n.qualifiedName = std::get<TyClass>(*inner).def->name + "." + n.method;
                }
                TyPtr ret = std::get<TyFn>(*mi->funcType).ret;
                n.resolvedTy = makeNullable(ret);
                return n.resolvedTy;
            }
            n.resolvedTy = makeNullable(freshInfer());
            return n.resolvedTy;

        } else if constexpr (std::is_same_v<T, ExprSafeField>) {
            TyPtr recv = checkExpr(n.recv, sc);
            if (!recv->nullable) error("safe field access on non-nullable type");
            TyPtr inner = unwrapNullable(recv);
            const FieldInfo* fi = lookupField(inner, n.field);
            if (fi && fi->type) {
                n.fieldOffset = fi->offset;
                n.resolvedTy = makeNullable(fi->type);
                return n.resolvedTy;
            }
            n.resolvedTy = makeNullable(freshInfer());
            return n.resolvedTy;

        } else if constexpr (std::is_same_v<T, ExprElvis>) {
            TyPtr lhs = checkExpr(n.lhs, sc);
            if (!lhs->nullable) error("elvis on non-nullable type");
            TyPtr rhs = checkExpr(n.rhs, sc);
            TyPtr inner = unwrapNullable(lhs);
            if (sameType(inner, rhs)) {
                n.resolvedTy = inner;
                return n.resolvedTy;
            }
            n.resolvedTy = rhs;
            return n.resolvedTy;

        } else if constexpr (std::is_same_v<T, ExprForceUnwrap>) {
            TyPtr inner = checkExpr(n.expr, sc);
            if (!inner->nullable) error("force unwrap on non-nullable type");
            n.resolvedTy = unwrapNullable(inner);
            return n.resolvedTy;

        } else if constexpr (std::is_same_v<T, ExprIs>) {
            TyPtr exprTy = checkExpr(n.expr, sc);
            TyPtr ty = resolveType(*n.ty, sc);
            if (ty && ty->kind() == TyKind::Class_) {
                n.targetTypeId = std::get<TyClass>(*ty).def->typeId;
            }
            (void)exprTy;
            return mkBool();

        } else if constexpr (std::is_same_v<T, ExprIsNot>) {
            TyPtr exprTy = checkExpr(n.expr, sc);
            TyPtr ty = resolveType(*n.ty, sc);
            if (ty && ty->kind() == TyKind::Class_) {
                n.targetTypeId = std::get<TyClass>(*ty).def->typeId;
            }
            (void)exprTy;
            return mkBool();

        } else if constexpr (std::is_same_v<T, ExprNull>) {
            return mkNull();

        } else if constexpr (std::is_same_v<T, ExprStringTemplate>) {
            // Each interpolated part is an ordinary expression and has to be
            // checked as one. Returning mkStr() without descending is what let
            // an unresolved name inside `$"...${...}"` compile and print a zero.
            for (const auto& part : n.parts) {
                const auto* sub = std::get_if<ExprPtr>(&part);
                if (!sub || !*sub) continue;
                TyPtr partTy = checkExpr(*sub, sc);
                if (!partTy || partTy->kind() == TyKind::Error) continue;  // already reported
                const TyKind kind = partTy->kind();
                const bool convertible = !partTy->nullable &&
                    (kind == TyKind::Int || kind == TyKind::Long || kind == TyKind::Bool ||
                     kind == TyKind::Char || kind == TyKind::Str);
                if (!convertible) {
                    const std::string typeName = partTy->toString();
                    error("cannot interpolate `" + typeName + "` into a string",
                          "`" + typeName + "` cannot be interpolated",
                          "convert it to `str` first");
                }
            }
            return mkStr();

        } else {
            error("unhandled expression kind"); return mkError();
        }
    }, *e);
    currentLoc = std::move(previousLoc);
    currentEndLoc = std::move(previousEndLoc);
    return result;
}

// ============================================================
// Statements
// ============================================================
void TypeChecker::checkStmt(StmtPtr s, Scope* sc) {
    if (!s) return;
    std::visit([&](auto&& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, StmtLet>) {
            TyPtr patTy = n.ty ? resolveType(*n.ty, sc) : nullptr;
            TyPtr initTy = mkUnit();
            if (n.init && std::holds_alternative<ExprClosure>(*n.init) &&
                patTy && patTy->kind() == TyKind::Fn) {
                initTy = checkClosure(std::get<ExprClosure>(*n.init), sc, patTy);
            } else {
                initTy = n.init ? checkExpr(n.init, sc, patTy) : mkUnit();
            }
            if (!patTy) patTy = initTy;
            if (!isSubtype(initTy, patTy)) error("let init type mismatch");
            n.resolvedTy = patTy;
            checkPat(n.pat, patTy, sc);
        } else if constexpr (std::is_same_v<T, StmtExpr>) {
            // The value is discarded here, which is what makes a non-exhaustive
            // `when` acceptable in statement position.
            struct StmtGuard {
                size_t& d;
                explicit StmtGuard(size_t& depth) : d(depth) { ++d; }
                ~StmtGuard() { --d; }
            } stmtGuard(m_stmtExprDepth);
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

        // if constexpr, not a runtime test: ItemImport is the one item without a
        // `name` member, so the body must not be instantiated for it.
        if constexpr (!std::is_same_v<T, ItemImport>) {
            if (m_trace) {
                const char* kind =
                    std::is_same_v<T, ItemFun>       ? "fun" :
                    std::is_same_v<T, ItemClass>     ? "class" :
                    std::is_same_v<T, ItemEnumClass> ? "enum" :
                    std::is_same_v<T, ItemInterface> ? "interface" :
                    std::is_same_v<T, ItemObject>    ? "object" :
                    std::is_same_v<T, ItemVal>       ? "val" :
                    std::is_same_v<T, ItemVar>       ? "var" :
                    std::is_same_v<T, ItemConst>     ? "const" : "typealias";
                m_trace->decl(kind, n.name);
            }
        }

        if constexpr (std::is_same_v<T, ItemFun>) {
            Scope fnScope(sc);
            for (size_t gi = 0; gi < n.generics.size(); ++gi) {
                fnScope.declare(
                    n.generics[gi].name,
                    SymbolKind::Type,
                    nullptr,
                    std::make_shared<Ty>(TyParam{n.generics[gi].name, static_cast<uint32_t>(gi)}));
            }

            // Resolve receiver type for extension functions
            TyPtr recvTy = nullptr;
            if (n.receiverType) {
                recvTy = resolveType(*n.receiverType, &fnScope);
            }

            std::vector<TyPtr> paramTys;
            n.resolvedParamTys.clear();
            // Extension functions: first param is the receiver
            if (recvTy) paramTys.push_back(recvTy);
            for (size_t pi = 0; pi < n.params.size(); ++pi) {
                TyPtr paramTy = parameterType(resolveType(*n.params[pi].second, &fnScope), pi, n.varargIndex, n.kwargsIndex, &fnScope);
                n.resolvedParamTys.push_back(paramTy);
                paramTys.push_back(paramTy);
            }

            TyPtr retTy = n.ret ? resolveType(*n.ret, &fnScope) : mkUnit();
            TyPtr fnTy = std::make_shared<Ty>(TyFn{paramTys, retTy});
            auto& callable = std::get<TyFn>(*fnTy);
            for (const auto& param : n.params) callable.paramNames.push_back(param.first);
            callable.varargIndex = n.varargIndex; callable.kwargsIndex = n.kwargsIndex;

            Symbol* sym = sc->lookup(n.name);
            if (sym) {
                if (!sym->overloads.empty()) {
                    n.backendName = overloadBackendName(n.receiverType ? n.name + "$extension" : n.name,
                                                        std::get<TyFn>(*fnTy));
                    for (auto& ov : sym->overloads) {
                        if (ov.first) {
                            auto* fn = static_cast<ItemFun*>(ov.first);
                            if (fn->backendName.empty()) {
                                auto ovFn = ov.second && ov.second->kind() == TyKind::Fn
                                    ? std::get<TyFn>(*ov.second) : TyFn{};
                                fn->backendName = overloadBackendName(
                                    fn->receiverType ? fn->name + "$extension" : fn->name, ovFn);
                            }
                        }
                    }
                } else {
                    n.backendName.clear();
                }

                // Update primary or overload type for this specific function
                void* myNode = const_cast<void*>(static_cast<const void*>(&n));
                bool updated = false;
                for (auto& ov : sym->overloads) {
                    if (ov.first == myNode) { ov.second = fnTy; updated = true; break; }
                }
                if (sym->node == myNode) sym->ty = fnTy;
                if (!updated) sym->ty = fnTy;
            }

            TyPtr saved = currentRetTy;
            currentRetTy = retTy;

            // Bind receiver for extension functions
            if (recvTy) {
                fnScope.declare(n.receiverName, SymbolKind::Value, nullptr, recvTy);
                fnScope.declare("this", SymbolKind::Value, nullptr, recvTy);
            }
            for (size_t i = 0; i < n.params.size(); ++i)
                fnScope.declare(n.params[i].first, SymbolKind::Value, nullptr, paramTys[i + (recvTy ? 1 : 0)]);

            const bool savedGenerated = checkingCompilerGenerated;
            checkingCompilerGenerated = n.compilerGenerated;
            if (n.body) checkExpr(n.body, &fnScope, retTy);
            checkingCompilerGenerated = savedGenerated;
            currentRetTy = saved;

        } else if constexpr (std::is_same_v<T, ItemClass>) {
            // Type-check in class child scope
            Scope* classScope = sc->childMod(n.name);
            if (!classScope) classScope = sc;
            auto classDef = classScope->classDef();

            if (n.modifier == ClassModifier::Data) {
                for (const auto& property : n.primaryCtor) {
                    if (!property.isVal && !property.isVar) continue;
                    TyPtr propertyTy = property.ty ? resolveType(*property.ty, classScope) : mkError();
                    const TyKind kind = propertyTy ? propertyTy->kind() : TyKind::Error;
                    const bool supported = kind == TyKind::Int || kind == TyKind::Str ||
                        kind == TyKind::Bool || kind == TyKind::Char || kind == TyKind::Double;
                    if (!supported) {
                        error(
                            "generated data-class members do not support property '" +
                                property.name + "' of type " +
                                (propertyTy ? propertyTy->toString() : std::string("<unknown>")),
                            "unsupported generated member type",
                            "use int, str, bool, char or double, or declare a regular class");
                        return;
                    }
                }
            }

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

            for (const auto& initBlock : n.initBlocks) {
                if (!initBlock) continue;
                if (auto* exprStmt = std::get_if<StmtExpr>(initBlock.get())) {
                    if (exprStmt->expr) checkExpr(exprStmt->expr, classScope);
                }
            }

            // Check methods in class scope
            for (const auto& m : n.methods) {
                if (classDef) {
                    Scope methodSignatureScope(classScope);
                    for (size_t gi = 0; gi < m.generics.size(); ++gi) {
                        methodSignatureScope.declare(
                            m.generics[gi].name,
                            SymbolKind::Type,
                            nullptr,
                            std::make_shared<Ty>(TyParam{m.generics[gi].name, static_cast<uint32_t>(gi)}));
                    }
                    std::vector<TyPtr> params;
                    params.push_back(classScope->selfTy() ? classScope->selfTy() : mkError());
                    for (size_t pi = 0; pi < m.params.size(); ++pi) {
                        params.push_back(parameterType(resolveType(*m.params[pi].second, &methodSignatureScope), pi, m.varargIndex, m.kwargsIndex, &methodSignatureScope));
                    }
                    TyPtr retTy = m.ret ? resolveType(*m.ret, &methodSignatureScope) : mkUnit();
                    TyPtr methodTy = std::make_shared<Ty>(TyFn{std::move(params), retTy});
                    for (auto& method : classDef->methods) {
                        if (method.name == m.name && method.arity == m.params.size()) {
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
                    if (vi < classDef->methods.size()) {
                        const auto& m = classDef->methods[vi];
                        if (m.isInterfaceDefault)
                            n.vtableMethodNames.push_back(m.defaultIfaceName + "." + m.name);
                        else
                            n.vtableMethodNames.push_back(n.name + "." + m.name);
                    } else
                        n.vtableMethodNames.push_back("");
                }
            }

        } else if constexpr (std::is_same_v<T, ItemEnumClass>) {
            Scope* enumScope = sc->childMod(n.name);
            if (!enumScope) enumScope = sc;
            auto enumDef = enumScope->classDef();
            for (const auto& m : n.methods) {
                if (enumDef) {
                    Scope methodSignatureScope(enumScope);
                    for (size_t gi = 0; gi < m.generics.size(); ++gi) {
                        methodSignatureScope.declare(
                            m.generics[gi].name,
                            SymbolKind::Type,
                            nullptr,
                            std::make_shared<Ty>(TyParam{m.generics[gi].name, static_cast<uint32_t>(gi)}));
                    }
                    std::vector<TyPtr> params;
                    params.push_back(enumScope->selfTy() ? enumScope->selfTy() : mkError());
                    for (size_t pi = 0; pi < m.params.size(); ++pi)
                        params.push_back(parameterType(resolveType(*m.params[pi].second, &methodSignatureScope), pi, m.varargIndex, m.kwargsIndex, &methodSignatureScope));
                    TyPtr retTy = m.ret ? resolveType(*m.ret, &methodSignatureScope) : mkUnit();
                    TyPtr methodTy = std::make_shared<Ty>(TyFn{std::move(params), retTy});
                    for (auto& method : enumDef->methods) {
                        if (method.name == m.name && method.arity == m.params.size()) {
                            method.funcType = methodTy;
                            break;
                        }
                    }
                    if (Symbol* methodSym = enumScope->lookupLocal(m.name)) methodSym->ty = methodTy;
                }
                checkItem(std::make_shared<Item>(m), enumScope);
            }

        } else if constexpr (std::is_same_v<T, ItemInterface>) {
            // Type-check default method bodies
            Scope* ifaceScope = sc->childMod(n.name);
            if (ifaceScope) {
                TyPtr selfTy = nullptr;
                if (Symbol* sym = sc->lookup(n.name))
                    selfTy = sym->ty;
                for (const auto& m : n.methods) {
                    if (m.defaultBody) {
                        Scope methodScope(ifaceScope);
                        methodScope.declare("self", SymbolKind::Value, nullptr, selfTy);
                        methodScope.declare("this", SymbolKind::Value, nullptr, selfTy);
                        for (size_t pi = 0; pi < m.params.size(); ++pi) {
                            const auto& p = m.params[pi];
                            TyPtr pt = parameterType(resolveType(*p.second, ifaceScope), pi, m.varargIndex, m.kwargsIndex, ifaceScope);
                            methodScope.declare(p.first, SymbolKind::Value, nullptr, pt);
                        }
                        TyPtr savedRet = currentRetTy;
                        currentRetTy = m.ret ? resolveType(*m.ret, ifaceScope) : mkUnit();
                        checkExpr(m.defaultBody, &methodScope);
                        currentRetTy = savedRet;
                    }
                }
            }

        } else if constexpr (std::is_same_v<T, ItemObject>) {
            Scope* objScope = sc->childMod(n.name);
            if (!objScope) objScope = sc;
            auto classDef = objScope->classDef();
            for (const auto& fd : n.fields) {
                TyPtr fieldTy = fd.ty ? resolveType(*fd.ty, objScope) : nullptr;
                TyPtr initTy = fd.initializer ? checkExpr(fd.initializer, objScope) : nullptr;
                if (!fieldTy) fieldTy = initTy;
                if (fd.ty && initTy && !isSubtype(initTy, fieldTy)) {
                    error("object field initializer type mismatch for '" + fd.name + "'");
                }
                if (classDef) {
                    for (auto& field : classDef->fields) {
                        if (field.name == fd.name) {
                            field.type = fieldTy;
                            break;
                        }
                    }
                }
                if (Symbol* fieldSymbol = objScope->lookupLocal(fd.name)) fieldSymbol->ty = fieldTy;
            }
            for (const auto& initBlock : n.initBlocks) {
                if (initBlock) {
                    if (auto* exprStmt = std::get_if<StmtExpr>(initBlock.get())) {
                        if (exprStmt->expr) checkExpr(exprStmt->expr, objScope);
                    }
                }
            }
            for (const auto& m : n.methods) {
                if (classDef) {
                    Scope methodSignatureScope(objScope);
                    for (size_t gi = 0; gi < m.generics.size(); ++gi) {
                        methodSignatureScope.declare(
                            m.generics[gi].name,
                            SymbolKind::Type,
                            nullptr,
                            std::make_shared<Ty>(TyParam{m.generics[gi].name, static_cast<uint32_t>(gi)}));
                    }
                    std::vector<TyPtr> params;
                    params.push_back(objScope->selfTy() ? objScope->selfTy() : mkError());
                    for (size_t pi = 0; pi < m.params.size(); ++pi)
                        params.push_back(parameterType(resolveType(*m.params[pi].second, &methodSignatureScope), pi, m.varargIndex, m.kwargsIndex, &methodSignatureScope));
                    TyPtr retTy = m.ret ? resolveType(*m.ret, &methodSignatureScope) : mkUnit();
                    TyPtr methodTy = std::make_shared<Ty>(TyFn{std::move(params), retTy});
                    for (auto& method : classDef->methods) {
                        if (method.name == m.name && method.arity == m.params.size()) {
                            method.funcType = methodTy;
                            break;
                        }
                    }
                    if (Symbol* methodSymbol = objScope->lookupLocal(m.name)) methodSymbol->ty = methodTy;
                }
                checkItem(std::make_shared<Item>(m), objScope);
            }
            if (classDef) {
                n.totalFieldSize = classDef->totalFieldSize;
                n.typeId = classDef->typeId;
                n.itables = classDef->itables;
                n.vtableEntryCount = static_cast<uint32_t>(classDef->vtable.size());
                n.vtableMethodNames.clear();
                for (uint32_t vi : classDef->vtable) {
                    if (vi < classDef->methods.size()) {
                        const auto& method = classDef->methods[vi];
                        if (method.isInterfaceDefault)
                            n.vtableMethodNames.push_back(method.defaultIfaceName + "." + method.name);
                        else
                            n.vtableMethodNames.push_back(n.name + "." + method.name);
                    } else {
                        n.vtableMethodNames.push_back("");
                    }
                }
            }

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
            if (Symbol* sym = sc->lookupLocal(n.name)) {
                sym->ty = finalTy;
                sym->node = const_cast<void*>(static_cast<const void*>(&n));
            }

        } else if constexpr (std::is_same_v<T, ItemVar>) {
            TyPtr init = checkExpr(n.init, sc);
            TyPtr finalTy = init;
            if (n.ty) {
                TyPtr ty = resolveType(*n.ty, sc);
                if (!isSubtype(init, ty)) error("var type mismatch");
                finalTy = ty;
            }
            if (Symbol* sym = sc->lookupLocal(n.name)) {
                sym->ty = finalTy;
                sym->node = const_cast<void*>(static_cast<const void*>(&n));
            }

        } else if constexpr (std::is_same_v<T, ItemConst>) {
            TyPtr ty = resolveType(*n.ty, sc);
            TyPtr init = checkExpr(n.init, sc);
            if (!isSubtype(init, ty)) error("const type mismatch");
            if (Symbol* sym = sc->lookupLocal(n.name)) {
                sym->ty = ty;
                sym->node = const_cast<void*>(static_cast<const void*>(&n));
            }

        } else if constexpr (std::is_same_v<T, ItemTypeAlias>) {
            TyPtr underlying = resolveType(*n.underlying, sc);
            if (Symbol* sym = sc->lookupLocal(n.name)) sym->ty = underlying;
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
    intrinsicSources = mod.intrinsicSources;
    ScopeBuilder builder;
    rootOwner = builder.build(mod);
    rootScope = rootOwner.get();
    // Resolve member signatures before any body can call a later member.
    for (const auto& item : mod.items) {
        std::visit([&](auto& owner) {
            using Owner = std::decay_t<decltype(owner)>;
            if constexpr (std::is_same_v<Owner, ItemClass> || std::is_same_v<Owner, ItemObject>) {
                auto* scope = rootScope->childMod(owner.name);
                if (!scope || !scope->classDef()) return;
                auto def = scope->classDef();
                for (const auto& method : owner.methods) {
                    Scope methodScope(scope);
                    for (size_t i = 0; i < method.generics.size(); ++i)
                        methodScope.declare(method.generics[i].name, SymbolKind::Type, nullptr,
                            std::make_shared<Ty>(TyParam{method.generics[i].name, static_cast<uint32_t>(i)}));
                    std::vector<TyPtr> params{scope->selfTy()};
                    for (size_t pi = 0; pi < method.params.size(); ++pi)
                        params.push_back(parameterType(resolveType(*method.params[pi].second, &methodScope), pi, method.varargIndex, method.kwargsIndex, &methodScope));
                    auto ret = method.ret ? resolveType(*method.ret, &methodScope) : mkUnit();
                    auto fn = std::make_shared<Ty>(TyFn{std::move(params), ret});
                    for (auto& info : def->methods)
                        if (info.name == method.name && info.arity == method.params.size()) info.funcType = fn;
                }
            } else if constexpr (std::is_same_v<Owner, ItemEnumClass>) {
                auto* scope = rootScope->childMod(owner.name);
                if (!scope || !scope->classDef()) return;
                auto def = scope->classDef();
                for (size_t v = 0; v < owner.variants.size(); ++v)
                    for (size_t f = 0; f < owner.variants[v].fields.size(); ++f)
                        def->variants[v].fields[f].type = resolveType(*owner.variants[v].fields[f].second, scope);
                for (const auto& method : owner.methods) {
                    Scope methodScope(scope);
                    for (size_t i = 0; i < method.generics.size(); ++i)
                        methodScope.declare(method.generics[i].name, SymbolKind::Type, nullptr,
                            std::make_shared<Ty>(TyParam{method.generics[i].name, static_cast<uint32_t>(i)}));
                    std::vector<TyPtr> params{scope->selfTy()};
                    for (size_t pi = 0; pi < method.params.size(); ++pi)
                        params.push_back(parameterType(resolveType(*method.params[pi].second, &methodScope), pi, method.varargIndex, method.kwargsIndex, &methodScope));
                    auto ret = method.ret ? resolveType(*method.ret, &methodScope) : mkUnit();
                    auto fn = std::make_shared<Ty>(TyFn{std::move(params), ret});
                    for (auto& info : def->methods)
                        if (info.name == method.name && info.arity == method.params.size()) info.funcType = fn;
                }
            } else if constexpr (std::is_same_v<Owner, ItemInterface>) {
                auto* scope = rootScope->childMod(owner.name);
                auto* symbol = rootScope->lookup(owner.name);
                if (!scope || !symbol || !symbol->ty) return;
                auto def = std::get<TyInterface>(*symbol->ty).def;
                for (const auto& method : owner.methods) {
                    std::vector<TyPtr> params{symbol->ty};
                    for (size_t pi = 0; pi < method.params.size(); ++pi)
                        params.push_back(parameterType(resolveType(*method.params[pi].second, scope), pi, method.varargIndex, method.kwargsIndex, scope));
                    auto ret = method.ret ? resolveType(*method.ret, scope) : mkUnit();
                    auto fn = std::make_shared<Ty>(TyFn{std::move(params), ret});
                    for (auto& info : def->methods)
                        if (info.name == method.name && info.arity == method.params.size()) info.funcType = fn;
                }
            }
        }, *item);
    }
    // Resolve signatures before bodies, including mutually recursive helpers.
    // Keep module-local declarations separately: imported/re-exported symbols may
    // legitimately appear more than once in the merged scope.
    struct LocalFunctionSignature {
        ItemFun* item;
        TyFn signature;
    };
    std::vector<LocalFunctionSignature> localSignatures;
    for (const auto& item : mod.items) {
        auto* fn = std::get_if<ItemFun>(item.get());
        if (!fn) continue;
        Scope signatureScope(rootScope);
        for (size_t i = 0; i < fn->generics.size(); ++i)
            signatureScope.declare(fn->generics[i].name, SymbolKind::Type, nullptr,
                std::make_shared<Ty>(TyParam{fn->generics[i].name, static_cast<uint32_t>(i)}));
        std::vector<TyPtr> params;
        if (fn->receiverType) params.push_back(resolveType(*fn->receiverType, &signatureScope));
        fn->resolvedParamTys.clear();
        for (size_t pi = 0; pi < fn->params.size(); ++pi) {
            TyPtr paramTy = parameterType(resolveType(*fn->params[pi].second, &signatureScope), pi, fn->varargIndex, fn->kwargsIndex, &signatureScope);
            fn->resolvedParamTys.push_back(paramTy);
            params.push_back(paramTy);
        }
        auto ret = fn->ret ? resolveType(*fn->ret, &signatureScope) : mkUnit();
        TyFn signature{std::move(params), ret};
        for (const auto& param : fn->params) signature.paramNames.push_back(param.first);
        signature.varargIndex = fn->varargIndex; signature.kwargsIndex = fn->kwargsIndex;
        auto type = std::make_shared<Ty>(signature);
        localSignatures.push_back({fn, signature});
        if (auto* symbol = rootScope->lookup(fn->name)) {
            if (symbol->node == fn) symbol->ty = type;
            for (auto& overload : symbol->overloads)
                if (overload.first == fn) overload.second = type;
        }
    }
    for (size_t i = 0; i < localSignatures.size(); ++i) {
        for (size_t j = i + 1; j < localSignatures.size(); ++j) {
            const auto& a = localSignatures[i];
            const auto& b = localSignatures[j];
            if (a.item->name != b.item->name) continue;
            if (static_cast<bool>(a.item->receiverType) != static_cast<bool>(b.item->receiverType)) continue;
            if (a.signature.params.size() != b.signature.params.size()) continue;
            bool sameParams = true;
            for (size_t p = 0; p < a.signature.params.size(); ++p) {
                if (mangleType(a.signature.params[p]) != mangleType(b.signature.params[p])) {
                    sameParams = false;
                    break;
                }
            }
            if (sameParams) {
                error("duplicate overload parameter signature for '" + a.item->name + "'");
            }
        }
    }
    for (const auto& item : mod.items)
        checkItem(item, rootScope);
    return errors.empty();
}

} // namespace
