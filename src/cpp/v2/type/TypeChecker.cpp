#include "TypeChecker.hpp"
#include <variant>
#include <cassert>

namespace loxis::v2 {

TyPtr TypeChecker::freshInfer() {
    return mkInfer(inferCounter++);
}

void TypeChecker::error(const std::string& msg) {
    errors.push_back(msg);
}

bool TypeChecker::isNumeric(TyPtr t) {
    if (!t) return false;
    TyKind k = t->kind();
    return k == TyKind::I8 || k == TyKind::I16 || k == TyKind::I32 || k == TyKind::I64 || k == TyKind::Isize ||
           k == TyKind::U8 || k == TyKind::U16 || k == TyKind::U32 || k == TyKind::U64 || k == TyKind::Usize ||
           k == TyKind::F32 || k == TyKind::F64;
}

bool TypeChecker::isInteger(TyPtr t) {
    if (!t) return false;
    TyKind k = t->kind();
    return k == TyKind::I8 || k == TyKind::I16 || k == TyKind::I32 || k == TyKind::I64 || k == TyKind::Isize ||
           k == TyKind::U8 || k == TyKind::U16 || k == TyKind::U32 || k == TyKind::U64 || k == TyKind::Usize;
}

bool TypeChecker::isBool(TyPtr t) {
    return t && t->kind() == TyKind::Bool;
}

bool TypeChecker::sameType(TyPtr a, TyPtr b) {
    if (!a || !b) return false;
    if (a->isError() || b->isError()) return true;
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

bool TypeChecker::isMutablePlace(ExprPtr e, Scope* sc) {
    if (!e) return false;
    return std::visit([&](auto&& n) -> bool {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ExprPath>) {
            return true; // simplified
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
        if (sym->kind != SymbolKind::Type && sym->kind != SymbolKind::Trait) {
            error("path does not refer to a type: " + s); return mkError();
        }
    }
    return sym && sym->ty ? sym->ty : mkError();
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
        } else if constexpr (std::is_same_v<T, AstTyNever>) {
            return mkNever();
        } else {
            return mkError();
        }
    }, surfaceType);
}

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
            TyPtr t;
            if (expected) {
                t = expected;
            } else {
                t = freshInfer();
            }
            sc->declare(n.name, SymbolKind::Value, nullptr, t);
            if (n.sub) {
                TyPtr subTy = checkPat(n.sub, t, sc);
                if (!sameType(t, subTy)) error("bind sub-pattern type mismatch");
            }
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
            if (expected && expected->kind() == TyKind::Adt) {
                // TODO: check fields
            }
            return expected ? expected : mkError();
        } else if constexpr (std::is_same_v<T, PatEnum>) {
            if (expected && expected->kind() == TyKind::Adt) {
                // TODO: check variant
            }
            return expected ? expected : mkError();
        } else if constexpr (std::is_same_v<T, PatRef>) {
            if (expected && expected->kind() == TyKind::Ref) {
                TyPtr inner = std::get<TyRef>(*expected).elem;
                return std::make_shared<Ty>(TyRef{n.mut, checkPat(n.pat, inner, sc)});
            } else {
                TyPtr inner = checkPat(n.pat, nullptr, sc);
                return std::make_shared<Ty>(TyRef{n.mut, inner});
            }
        } else if constexpr (std::is_same_v<T, PatRest>) {
            return expected ? expected : mkError();
        } else {
            return mkError();
        }
    }, *p);
}

TyPtr TypeChecker::checkExpr(ExprPtr e, Scope* sc) {
    if (!e) { error("null expression"); return mkError(); }
    return std::visit([&](auto&& n) -> TyPtr {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ExprLit>) {
            return std::visit([&](auto&& v) -> TyPtr {
                using VT = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<VT, int64_t> || std::is_same_v<VT, uint64_t>) return mkI32();
                else if constexpr (std::is_same_v<VT, double>) return mkF64();
                else if constexpr (std::is_same_v<VT, bool>) return mkBool();
                else if constexpr (std::is_same_v<VT, char>) return std::make_shared<Ty>(TyChar{});
                else if constexpr (std::is_same_v<VT, std::string>) return mkStr();
                else { error("unknown literal"); return mkError(); }
            }, n.val);
        } else if constexpr (std::is_same_v<T, ExprPath>) {
            Symbol* sym = nullptr;
            Scope* search = sc;
            for (const auto& s : n.path.segs) {
                sym = nullptr;
                while (search) {
                    sym = search->lookup(s);
                    if (sym) break;
                    search = search->parent();
                }
                if (!sym) { error("unresolved path: " + s); return mkError(); }
            }
            if (sym && sym->kind == SymbolKind::Value) return sym->ty ? sym->ty : mkError();
            error("path did not resolve to value"); return mkError();
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
            // TODO: check struct literal
            return freshInfer();
        } else if constexpr (std::is_same_v<T, ExprCall>) {
            TyPtr callee = checkExpr(n.func, sc);
            if (callee->kind() != TyKind::Fn) { error("call on non-function"); return mkError(); }
            auto& fn = std::get<TyFn>(*callee);
            if (fn.params.size() != n.args.size()) { error("call arity mismatch"); return mkError(); }
            for (size_t i = 0; i < n.args.size(); ++i) {
                TyPtr argTy = checkExpr(n.args[i], sc);
                if (!sameType(argTy, fn.params[i])) error("call argument type mismatch");
            }
            return fn.ret;
        } else if constexpr (std::is_same_v<T, ExprMethodCall>) {
            TyPtr recv = checkExpr(n.recv, sc);
            (void)recv;
            // simplified: unresolved for now
            return freshInfer();
        } else if constexpr (std::is_same_v<T, ExprField>) {
            TyPtr base = checkExpr(n.obj, sc);
            (void)base;
            // fields not populated yet
            return freshInfer();
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
                case ExprBinary::Add: case ExprBinary::Sub: case ExprBinary::Mul: case ExprBinary::Div: case ExprBinary::Rem:
                    if (!isNumeric(l) || !isNumeric(r)) { error("numeric op on non-numeric"); return mkError(); }
                    if (!sameType(l, r)) { error("numeric op type mismatch"); return mkError(); }
                    return l;
                case ExprBinary::Shl: case ExprBinary::Shr: case ExprBinary::BitAnd: case ExprBinary::BitOr: case ExprBinary::BitXor:
                    if (!isInteger(l) || !isInteger(r)) { error("bitwise op on non-integer"); return mkError(); }
                    if (!sameType(l, r)) { error("bitwise op type mismatch"); return mkError(); }
                    return l;
                case ExprBinary::Eq: case ExprBinary::Ne: case ExprBinary::Lt: case ExprBinary::Le: case ExprBinary::Gt: case ExprBinary::Ge:
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
                    if (!sameType(lhs, rhs)) error("assignment type mismatch");
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
            TyPtr t = checkExpr(n.then_, sc);
            if (n.else_) {
                TyPtr el = checkExpr(n.else_, sc);
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
        } else if constexpr (std::is_same_v<T, ExprFor>) {
            TyPtr iter = checkExpr(n.iter, sc);
            (void)iter;
            loopStack.push_back({mkUnit(), n.label});
            checkExpr(n.body, sc);
            loopStack.pop_back();
            return mkUnit();
        } else if constexpr (std::is_same_v<T, ExprLoop>) {
            loopStack.push_back({mkUnit(), n.label});
            checkExpr(n.body, sc);
            loopStack.pop_back();
            return mkUnit();
        } else if constexpr (std::is_same_v<T, ExprMatch>) {
            TyPtr scrut = checkExpr(n.scrut, sc);
            TyPtr result = nullptr;
            for (const auto& arm : n.arms) {
                checkPat(arm.first, scrut, sc);
                TyPtr armTy = checkExpr(arm.second, sc);
                if (!result) result = armTy;
                else if (!sameType(result, armTy)) error("match arm type mismatch");
            }
            return result ? result : mkError();
        } else if constexpr (std::is_same_v<T, ExprBreak>) {
            if (loopStack.empty()) { error("break outside loop"); return mkError(); }
            if (n.label) {
                bool found = false;
                for (auto it = loopStack.rbegin(); it != loopStack.rend(); ++it) {
                    if (it->label && *it->label == *n.label) { found = true; break; }
                }
                if (!found) error("break label not found");
            }
            if (n.expr) checkExpr(n.expr, sc);
            return mkNever();
        } else if constexpr (std::is_same_v<T, ExprContinue>) {
            if (loopStack.empty()) { error("continue outside loop"); return mkError(); }
            if (n.label) {
                bool found = false;
                for (auto it = loopStack.rbegin(); it != loopStack.rend(); ++it) {
                    if (it->label && *it->label == *n.label) { found = true; break; }
                }
                if (!found) error("continue label not found");
            }
            return mkNever();
        } else if constexpr (std::is_same_v<T, ExprReturn>) {
            if (!currentRetTy) { error("return outside function"); return mkError(); }
            if (n.expr) {
                TyPtr ret = checkExpr(n.expr, sc);
                if (!sameType(ret, currentRetTy)) { error("return type mismatch"); return mkError(); }
            } else {
                if (!currentRetTy->isUnit()) error("return unit in non-unit function");
            }
            return mkNever();
        } else if constexpr (std::is_same_v<T, ExprClosure>) {
            std::vector<TyPtr> paramTys;
            for (size_t i = 0; i < n.params.size(); ++i) {
                (void)n.params[i];
                paramTys.push_back(freshInfer());
            }
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
        } else {
            error("unhandled expression kind"); return mkError();
        }
    }, *e);
}

void TypeChecker::checkStmt(StmtPtr s, Scope* sc) {
    if (!s) return;
    std::visit([&](auto&& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, StmtLet>) {
            TyPtr initTy = checkExpr(n.init, sc);
            TyPtr patTy = n.ty ? resolveType(*n.ty, sc) : initTy;
            if (!sameType(initTy, patTy)) error("let init type mismatch");
            checkPat(n.pat, patTy, sc);
        } else if constexpr (std::is_same_v<T, StmtExpr>) {
            checkExpr(n.expr, sc);
        } else if constexpr (std::is_same_v<T, StmtItem>) {
            checkItem(n.item, sc);
        }
    }, *s);
}

void TypeChecker::checkItem(ItemPtr i, Scope* sc) {
    if (!i) return;
    std::visit([&](auto&& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ItemFn>) {
            std::vector<TyPtr> paramTys;
            for (const auto& p : n.params) {
                paramTys.push_back(resolveType(*p.second, sc));
            }
            TyPtr retTy = n.ret ? resolveType(*n.ret, sc) : mkUnit();
            TyPtr fnTy = std::make_shared<Ty>(TyFn{paramTys, retTy});
            Symbol* sym = sc->lookup(n.name);
            if (sym) sym->ty = fnTy;
            TyPtr saved = currentRetTy;
            currentRetTy = retTy;
            checkExpr(n.body, sc);
            currentRetTy = saved;
        } else if constexpr (std::is_same_v<T, ItemStruct>) {
            // TODO: populate fields
        } else if constexpr (std::is_same_v<T, ItemEnum>) {
            // TODO: populate variants
        } else if constexpr (std::is_same_v<T, ItemTrait>) {
        } else if constexpr (std::is_same_v<T, ItemImpl>) {
            for (const auto& m : n.methods) {
                checkItem(std::make_shared<Item>(m), sc);
            }
        } else if constexpr (std::is_same_v<T, ItemMod>) {
            Scope* child = sc->childMod(n.name);
            if (!child) child = sc;
            for (const auto& it : n.items) checkItem(it, child);
        } else if constexpr (std::is_same_v<T, ItemUse>) {
        } else if constexpr (std::is_same_v<T, ItemStatic>) {
            TyPtr ty = n.ty ? resolveType(*n.ty, sc) : nullptr;
            TyPtr init = checkExpr(n.init, sc);
            if (ty && !sameType(ty, init)) error("static type mismatch");
        } else if constexpr (std::is_same_v<T, ItemConst>) {
            TyPtr ty = n.ty ? resolveType(*n.ty, sc) : nullptr;
            TyPtr init = checkExpr(n.init, sc);
            if (ty && !sameType(ty, init)) error("const type mismatch");
        }
    }, *i);
}

bool TypeChecker::checkModule(Module& mod) {
    ScopeBuilder builder;
    rootOwner = builder.build(mod);
    rootScope = rootOwner.get();
    for (const auto& item : mod.items) {
        checkItem(item, rootScope);
    }
    return errors.empty();
}

} // namespace
