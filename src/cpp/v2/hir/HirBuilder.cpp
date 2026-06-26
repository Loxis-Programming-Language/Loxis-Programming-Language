#include "HirBuilder.hpp"
#include "../type/Scope.hpp"

namespace loxis::v2 {

void HirBuilder::pushGenerics(const std::vector<GenericParam>& params) {
    size_t n = params.size();
    for (const auto& g : params) {
        m_genericParams.push_back(g.name);
    }
    m_genericDepthStack.push_back(n);
}

void HirBuilder::popGenerics() {
    if (!m_genericDepthStack.empty()) {
        size_t n = m_genericDepthStack.back();
        m_genericDepthStack.pop_back();
        for (size_t i = 0; i < n && !m_genericParams.empty(); ++i) {
            m_genericParams.pop_back();
        }
    }
}

MirBinOp HirBuilder::toMirBinOp(ExprBinary::Op op) {
    switch (op) {
        case ExprBinary::Add: return MirBinOp::Add;
        case ExprBinary::Sub: return MirBinOp::Sub;
        case ExprBinary::Mul: return MirBinOp::Mul;
        case ExprBinary::Div: return MirBinOp::Div;
        case ExprBinary::Rem: return MirBinOp::Rem;
        case ExprBinary::Shl: return MirBinOp::Shl;
        case ExprBinary::Shr: return MirBinOp::Shr;
        case ExprBinary::BitAnd: return MirBinOp::BitAnd;
        case ExprBinary::BitOr: return MirBinOp::BitOr;
        case ExprBinary::BitXor: return MirBinOp::BitXor;
        case ExprBinary::And: return MirBinOp::And;
        case ExprBinary::Or: return MirBinOp::Or;
        case ExprBinary::Eq: return MirBinOp::Eq;
        case ExprBinary::Ne: return MirBinOp::Ne;
        case ExprBinary::Lt: return MirBinOp::Lt;
        case ExprBinary::Le: return MirBinOp::Le;
        case ExprBinary::Gt: return MirBinOp::Gt;
        case ExprBinary::Ge: return MirBinOp::Ge;
    }
    return MirBinOp::Add;
}

MirUnaryOp HirBuilder::toMirUnaryOp(ExprUnary::Op op) {
    switch (op) {
        case ExprUnary::Neg: return MirUnaryOp::Neg;
        case ExprUnary::Not: return MirUnaryOp::Not;
        case ExprUnary::Deref: return MirUnaryOp::Deref;
        case ExprUnary::Ref: return MirUnaryOp::Ref;
        case ExprUnary::RefMut: return MirUnaryOp::RefMut;
    }
    return MirUnaryOp::Neg;
}

MirAssignOp HirBuilder::toMirAssignOp(ExprAssign::Op op) {
    switch (op) {
        case ExprAssign::Set: return MirAssignOp::Set;
        case ExprAssign::AddEq: return MirAssignOp::AddEq;
        case ExprAssign::SubEq: return MirAssignOp::SubEq;
        case ExprAssign::MulEq: return MirAssignOp::MulEq;
        case ExprAssign::DivEq: return MirAssignOp::DivEq;
        case ExprAssign::RemEq: return MirAssignOp::RemEq;
        case ExprAssign::AmpEq: return MirAssignOp::AmpEq;
        case ExprAssign::PipeEq: return MirAssignOp::PipeEq;
        case ExprAssign::CaretEq: return MirAssignOp::CaretEq;
        case ExprAssign::ShlEq: return MirAssignOp::ShlEq;
        case ExprAssign::ShrEq: return MirAssignOp::ShrEq;
    }
    return MirAssignOp::Set;
}

CoreTyPtr HirBuilder::astType(const TypePtr& ty) {
    if (!ty) return mkUnit();
    return std::visit([&](const auto& t) -> CoreTyPtr {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, AstTyPath>) {
            if (t.path.segs.empty()) return mkError();
            const auto& name = t.path.segs.back();

            // 1. Check active generic parameters (e.g., T in a generic function body)
            for (size_t i = 0; i < m_genericParams.size(); ++i) {
                if (m_genericParams[i] == name) {
                    return std::make_shared<Ty>(TyParam{name, static_cast<uint32_t>(i)});
                }
            }

            // 2. Check built-in types
            if (name == "int") return mkInt();
            if (name == "long") return mkLong();
            if (name == "float") return mkFloat();
            if (name == "double") return mkDouble();
            if (name == "bool") return mkBool();
            if (name == "char") return mkChar();
            if (name == "str") return mkStr();
            if (name == "unit" || name == "void" || name == "()" || name == "None") return mkUnit();
            if (name == "noreturn") return mkNever();

            // 3. Check scope for user-defined types
            if (m_scope) {
                Symbol* sym = m_scope->lookup(name);
                if (sym && sym->ty &&
                    (sym->kind == SymbolKind::Type || sym->kind == SymbolKind::Interface)) {
                    CoreTyPtr base = sym->ty;
                    if (!t.path.args.empty()) {
                        // Apply generic arguments
                        std::vector<CoreTyPtr> argTys;
                        for (const auto& arg : t.path.args) {
                            argTys.push_back(astType(arg));
                        }
                        if (base->kind() == TyKind::Class_) {
                            auto& cls = std::get<TyClass>(*base);
                            return std::make_shared<Ty>(TyClass{cls.def, std::move(argTys)});
                        }
                        if (base->kind() == TyKind::Interface_) {
                            auto& iface = std::get<TyInterface>(*base);
                            return std::make_shared<Ty>(TyInterface{iface.def, std::move(argTys)});
                        }
                    }
                    return base;
                }
            }

            return mkInt(); // Fallback: unknown type
        } else if constexpr (std::is_same_v<T, AstTyTuple>) {
            std::vector<TyPtr> elems;
            for (const auto& elem : t.elems) elems.push_back(astType(elem));
            return std::make_shared<Ty>(TyTuple{std::move(elems)});
        } else if constexpr (std::is_same_v<T, AstTyArray>) {
            return std::make_shared<Ty>(TyArray{astType(t.elem), 0});
        } else if constexpr (std::is_same_v<T, AstTySlice>) {
            return std::make_shared<Ty>(TySlice{astType(t.elem)});
        } else if constexpr (std::is_same_v<T, AstTyRef>) {
            return std::make_shared<Ty>(TyRef{t.mut, astType(t.elem)});
        } else if constexpr (std::is_same_v<T, AstTyPtr>) {
            return std::make_shared<Ty>(TyRawPtr{t.mut, astType(t.elem)});
        } else if constexpr (std::is_same_v<T, AstTyFn>) {
            std::vector<TyPtr> params;
            for (const auto& param : t.params) params.push_back(astType(param));
            return std::make_shared<Ty>(TyFn{std::move(params), astType(t.ret)});
        } else if constexpr (std::is_same_v<T, AstTyNullable>) {
            return makeNullable(astType(t.inner));
        } else {
            return mkInt();
        }
    }, *ty);
}

CoreTyPtr HirBuilder::literalType(const ExprLit& lit) {
    return std::visit([](const auto& v) -> CoreTyPtr {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) return mkBool();
        else if constexpr (std::is_same_v<T, char>) return mkChar();
        else if constexpr (std::is_same_v<T, double>) return mkDouble();
        else if constexpr (std::is_same_v<T, std::string>) return mkStr();
        else return mkInt();
    }, lit.val);
}

HirPatPtr HirBuilder::buildPat(const PatPtr& pat) {
    if (!pat) return std::make_shared<HirPat>(HirPatWild{});
    return std::visit([&](const auto& p) -> HirPatPtr {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, PatBind>) {
            return std::make_shared<HirPat>(HirPatBind{p.name, p.mut, buildPat(p.sub)});
        } else {
            return std::make_shared<HirPat>(HirPatWild{});
        }
    }, *pat);
}

HirStmtPtr HirBuilder::buildStmt(const StmtPtr& stmt) {
    if (!stmt) return std::make_shared<HirStmt>(HirStmtExpr{nullptr});
    return std::visit([&](const auto& s) -> HirStmtPtr {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, StmtLet>) {
            return std::make_shared<HirStmt>(HirStmtLet{
                buildPat(s.pat), astType(s.ty), buildExpr(s.init)
            });
        } else if constexpr (std::is_same_v<T, StmtExpr>) {
            return std::make_shared<HirStmt>(HirStmtExpr{buildExpr(s.expr)});
        } else {
            return std::make_shared<HirStmt>(HirStmtExpr{nullptr});
        }
    }, *stmt);
}

HirExprPtr HirBuilder::buildExpr(const ExprPtr& expr) {
    if (!expr) return std::make_shared<HirExpr>(HirNull{mkUnit()});
    return std::visit([&](const auto& e) -> HirExprPtr {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, ExprLit>) {
            return std::make_shared<HirExpr>(HirLit{literalType(e), e.val});
        } else if constexpr (std::is_same_v<T, ExprPath>) {
            std::string name = e.path.segs.empty() ? "" : e.path.segs.back();
            std::vector<CoreTyPtr> typeArgs;
            for (const auto& arg : e.path.args) typeArgs.push_back(astType(arg));
            CoreTyPtr ty = e.resolvedTy ? e.resolvedTy : mkInt();
            if (e.isField) {
                auto selfExpr = std::make_shared<HirExpr>(HirPath{"self", {}, mkInt()});
                return std::make_shared<HirExpr>(HirField{selfExpr, name, e.fieldOffset, ty});
            }
            return std::make_shared<HirExpr>(HirPath{name, std::move(typeArgs), ty});
        } else if constexpr (std::is_same_v<T, ExprTuple>) {
            std::vector<HirExprPtr> elems;
            for (const auto& elem : e.elems) elems.push_back(buildExpr(elem));
            return std::make_shared<HirExpr>(HirTuple{std::move(elems), mkInt()});
        } else if constexpr (std::is_same_v<T, ExprArray>) {
            std::vector<HirExprPtr> elems;
            for (const auto& elem : e.elems) elems.push_back(buildExpr(elem));
            return std::make_shared<HirExpr>(HirArray{std::move(elems), mkInt()});
        } else if constexpr (std::is_same_v<T, ExprStruct>) {
            std::vector<HirFieldInit> fields;
            for (const auto& f : e.fields) {
                fields.push_back({f.name, buildExpr(f.expr)});
            }
            std::string typeName = e.path.segs.empty() ? "" : e.path.segs.back();
            std::vector<CoreTyPtr> typeArgs;
            for (const auto& arg : e.path.args) typeArgs.push_back(astType(arg));
            if (typeArgs.empty()) typeArgs = e.resolvedTypeArgs;
            return std::make_shared<HirExpr>(HirStruct{
                typeName, std::move(fields), std::move(typeArgs),
                e.totalFieldSize, e.typeId, mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprCall>) {
            std::optional<std::string> staticName;
            std::vector<CoreTyPtr> typeArgs;
            if (e.func && std::holds_alternative<ExprPath>(*e.func)) {
                const auto& path = std::get<ExprPath>(*e.func);
                if (!path.path.segs.empty()) staticName = path.path.segs.back();
                for (const auto& arg : path.path.args) {
                    typeArgs.push_back(astType(arg));
                }
            }
            if (typeArgs.empty()) typeArgs = e.resolvedTypeArgs;
            std::vector<HirExprPtr> args;
            for (const auto& a : e.args) args.push_back(buildExpr(a));
            return std::make_shared<HirExpr>(HirCall{
                buildExpr(e.func), staticName, e.ctorLabel, std::move(args),
                std::move(typeArgs), mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprMethodCall>) {
            std::vector<HirExprPtr> args;
            for (const auto& a : e.args) args.push_back(buildExpr(a));
            return std::make_shared<HirExpr>(HirMethodCall{
                buildExpr(e.recv), e.method, e.qualifiedName, std::move(args),
                e.vtableIndex, e.isInterfaceDispatch, e.ifaceTypeId, e.ifaceMethodIndex, mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprField>) {
            return std::make_shared<HirExpr>(HirField{
                buildExpr(e.obj), e.field, e.fieldOffset, mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprIndex>) {
            return std::make_shared<HirExpr>(HirIndex{
                buildExpr(e.obj), buildExpr(e.idx), mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprUnary>) {
            return std::make_shared<HirExpr>(HirUnary{
                toMirUnaryOp(e.op), buildExpr(e.operand), mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprBinary>) {
            return std::make_shared<HirExpr>(HirBinary{
                toMirBinOp(e.op), buildExpr(e.l), buildExpr(e.r), mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprAssign>) {
            return std::make_shared<HirExpr>(HirAssign{
                toMirAssignOp(e.op), buildExpr(e.lhs), buildExpr(e.rhs), mkUnit()
            });
        } else if constexpr (std::is_same_v<T, ExprBlock>) {
            std::vector<HirStmtPtr> stmts;
            for (const auto& s : e.stmts) stmts.push_back(buildStmt(s));
            return std::make_shared<HirExpr>(HirBlock{
                std::move(stmts), buildExpr(e.tail), mkUnit()
            });
        } else if constexpr (std::is_same_v<T, ExprIf>) {
            return std::make_shared<HirExpr>(HirIf{
                buildExpr(e.cond), buildExpr(e.then_), buildExpr(e.else_), mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprWhile>) {
            return std::make_shared<HirExpr>(HirWhile{
                buildExpr(e.cond), buildExpr(e.body), e.label, mkUnit()
            });
        } else if constexpr (std::is_same_v<T, ExprRange>) {
            return std::make_shared<HirExpr>(HirNull{mkUnit()});
        } else if constexpr (std::is_same_v<T, ExprFor>) {
            return desugarFor(e);
        } else if constexpr (std::is_same_v<T, ExprLoop>) {
            return std::make_shared<HirExpr>(HirLoop{
                buildExpr(e.body), e.label, mkUnit()
            });
        } else if constexpr (std::is_same_v<T, ExprWhen>) {
            std::vector<HirWhenArm> arms;
            for (const auto& arm : e.arms) {
                HirWhenArm ha;
                ha.body = buildExpr(arm.body);
                ha.isElse = arm.isElse;
                if (arm.isElse) {
                    ha.condition = std::monostate{};
                } else if (std::holds_alternative<PatPtr>(arm.condition) && std::get<PatPtr>(arm.condition)) {
                    ha.condition = buildPat(std::get<PatPtr>(arm.condition));
                } else if (std::holds_alternative<ExprPtr>(arm.condition)) {
                    ha.condition = buildExpr(std::get<ExprPtr>(arm.condition));
                } else {
                    ha.condition = std::monostate{};
                }
                arms.push_back(std::move(ha));
            }
            return std::make_shared<HirExpr>(HirWhen{
                buildExpr(e.scrut), std::move(arms), mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprBreak>) {
            return std::make_shared<HirExpr>(HirBreak{e.label, buildExpr(e.expr), mkUnit()});
        } else if constexpr (std::is_same_v<T, ExprContinue>) {
            return std::make_shared<HirExpr>(HirContinue{e.label, mkUnit()});
        } else if constexpr (std::is_same_v<T, ExprReturn>) {
            return std::make_shared<HirExpr>(HirReturn{buildExpr(e.expr), mkUnit()});
        } else if constexpr (std::is_same_v<T, ExprRef>) {
            return std::make_shared<HirExpr>(HirRef{e.mut, buildExpr(e.expr), mkInt()});
        } else if constexpr (std::is_same_v<T, ExprDeref>) {
            return std::make_shared<HirExpr>(HirDeref{buildExpr(e.expr), mkInt()});
        } else if constexpr (std::is_same_v<T, ExprTry>) {
            return std::make_shared<HirExpr>(HirTry{buildExpr(e.expr), mkInt()});
        } else if constexpr (std::is_same_v<T, ExprCast>) {
            CoreTyPtr target = astType(e.ty);
            return std::make_shared<HirExpr>(HirCast{buildExpr(e.expr), target, target});
        } else if constexpr (std::is_same_v<T, ExprSafeCall>) {
            std::vector<HirExprPtr> args;
            for (const auto& a : e.args) args.push_back(buildExpr(a));
            return std::make_shared<HirExpr>(HirSafeCall{
                buildExpr(e.recv), e.method, std::move(args), mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprSafeField>) {
            return std::make_shared<HirExpr>(HirSafeField{
                buildExpr(e.recv), e.field, 0, mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprElvis>) {
            return std::make_shared<HirExpr>(HirElvis{
                buildExpr(e.lhs), buildExpr(e.rhs), mkInt()
            });
        } else if constexpr (std::is_same_v<T, ExprForceUnwrap>) {
            return std::make_shared<HirExpr>(HirForceUnwrap{buildExpr(e.expr), mkInt()});
        } else if constexpr (std::is_same_v<T, ExprIs>) {
            return std::make_shared<HirExpr>(HirIs{buildExpr(e.expr), ~0u, mkBool()});
        } else if constexpr (std::is_same_v<T, ExprIsNot>) {
            return std::make_shared<HirExpr>(HirIsNot{buildExpr(e.expr), ~0u, mkBool()});
        } else if constexpr (std::is_same_v<T, ExprNull>) {
            return std::make_shared<HirExpr>(HirNull{mkInt()});
        } else if constexpr (std::is_same_v<T, ExprStringTemplate>) {
            if (e.parts.empty()) {
                return std::make_shared<HirExpr>(HirLit{mkStr(), std::string{}});
            }
            // Convert each part to a HirExpr:
            // - string → HirLit (string literal)
            // - ExprPtr → HirUnary::ToStr (convert to string)
            HirExprPtr result = nullptr;
            for (const auto& part : e.parts) {
                HirExprPtr partExpr;
                if (std::holds_alternative<std::string>(part)) {
                    partExpr = std::make_shared<HirExpr>(HirLit{
                        mkStr(), std::get<std::string>(part)
                    });
                } else {
                    auto expr = buildExpr(std::get<ExprPtr>(part));
                    partExpr = std::make_shared<HirExpr>(HirUnary{
                        MirUnaryOp::ToStr, expr, mkStr()
                    });
                }
                if (!result) {
                    result = partExpr;
                } else {
                    result = std::make_shared<HirExpr>(HirBinary{
                        MirBinOp::Add, result, partExpr, mkStr()
                    });
                }
            }
            return result ? result : std::make_shared<HirExpr>(HirLit{mkStr(), std::string{}});
        } else {
            return std::make_shared<HirExpr>(HirNull{mkUnit()});
        }
    }, *expr);
}

HirExprPtr HirBuilder::makeIntLit(int64_t v) {
    return std::make_shared<HirExpr>(HirLit{mkInt(), v});
}

HirExprPtr HirBuilder::makeBin(MirBinOp op, HirExprPtr l, HirExprPtr r) {
    return std::make_shared<HirExpr>(HirBinary{op, std::move(l), std::move(r), mkBool()});
}

HirExprPtr HirBuilder::makeAssign(HirExprPtr lhs, HirExprPtr rhs) {
    return std::make_shared<HirExpr>(HirAssign{
        MirAssignOp::Set, std::move(lhs), std::move(rhs), mkUnit()
    });
}

HirExprPtr HirBuilder::makeLet(const std::string& name, HirExprPtr init, HirExprPtr body) {
    auto pat = std::make_shared<HirPat>(HirPatBind{name, false, nullptr});
    auto stmt = std::make_shared<HirStmt>(HirStmtLet{pat, mkInt(), std::move(init)});
    std::vector<HirStmtPtr> stmts = {stmt};
    return std::make_shared<HirExpr>(HirBlock{std::move(stmts), std::move(body), mkUnit()});
}

HirExprPtr HirBuilder::makeWhile(HirExprPtr cond, HirExprPtr body, const std::optional<std::string>& label) {
    return std::make_shared<HirExpr>(HirWhile{std::move(cond), std::move(body), label, mkUnit()});
}

HirExprPtr HirBuilder::makeMethodCall(HirExprPtr recv, const std::string& qualifiedName, std::vector<HirExprPtr> args) {
    auto dot = qualifiedName.rfind('.');
    std::string method = dot == std::string::npos ? qualifiedName : qualifiedName.substr(dot + 1);
    return std::make_shared<HirExpr>(HirMethodCall{
        std::move(recv), method, qualifiedName, std::move(args), ~0u, false, ~0u, 0, mkInt()
    });
}

HirExprPtr HirBuilder::makeIndex(HirExprPtr obj, HirExprPtr idx) {
    return std::make_shared<HirExpr>(HirIndex{std::move(obj), std::move(idx), mkInt()});
}

HirExprPtr HirBuilder::makeBlockWithTail(HirExprPtr first, HirExprPtr tail) {
    std::vector<HirStmtPtr> stmts;
    if (first) {
        stmts.push_back(std::make_shared<HirStmt>(HirStmtExpr{std::move(first)}));
    }
    return std::make_shared<HirExpr>(HirBlock{std::move(stmts), std::move(tail), mkUnit()});
}

HirExprPtr HirBuilder::wrapWithPat(const PatPtr& pat, HirExprPtr elem, HirExprPtr body) {
    if (!pat) return body;
    return std::visit([&](const auto& p) -> HirExprPtr {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, PatWild>) {
            return body;
        } else if constexpr (std::is_same_v<T, PatBind>) {
            return makeLet(p.name, elem, body);
        } else {
            return body;
        }
    }, *pat);
}

HirExprPtr HirBuilder::desugarFor(const ExprFor& e) {
    HirExprPtr one = makeIntLit(1);
    HirExprPtr iterExpr = buildExpr(e.iter);
    HirExprPtr body = buildExpr(e.body);
    HirExprPtr idxPath = std::make_shared<HirExpr>(HirPath{"__for_i", {}, mkInt()});

    auto incIdx = [&]() {
        return makeAssign(idxPath, makeBin(MirBinOp::Add, idxPath, one));
    };

    if (e.iterKind == ExprFor::IterKind::IntRange ||
        e.iterKind == ExprFor::IterKind::IntRangeInclusive) {
        const auto& range = std::get<ExprRange>(*e.iter);
        HirExprPtr lo = buildExpr(range.start);
        HirExprPtr hi = buildExpr(range.end);
        MirBinOp cmp = e.iterKind == ExprFor::IterKind::IntRangeInclusive
            ? MirBinOp::Le : MirBinOp::Lt;
        HirExprPtr cond = makeBin(cmp, idxPath, hi);
        HirExprPtr loopBody = wrapWithPat(e.pat, idxPath, makeBlockWithTail(body, incIdx()));
        return makeLet("__for_i", lo, makeWhile(cond, loopBody, e.label));
    }

    if (e.iterKind == ExprFor::IterKind::Array) {
        HirExprPtr lenLit = makeIntLit(static_cast<int64_t>(e.arrayLen));
        HirExprPtr arrPath = std::make_shared<HirExpr>(HirPath{"__for_arr", {}, mkInt()});
        HirExprPtr elem = makeIndex(arrPath, idxPath);
        HirExprPtr cond = makeBin(MirBinOp::Lt, idxPath, lenLit);
        HirExprPtr loopBody = wrapWithPat(e.pat, elem, makeBlockWithTail(body, incIdx()));
        HirExprPtr withIdx = makeLet("__for_i", makeIntLit(0), makeWhile(cond, loopBody, e.label));
        return makeLet("__for_arr", iterExpr, withIdx);
    }

    if (e.iterKind == ExprFor::IterKind::Iterable) {
        HirExprPtr itPath = std::make_shared<HirExpr>(HirPath{"__for_it", {}, mkInt()});
        std::string qn = e.iterClassName.empty() ? "size" : e.iterClassName + ".size";
        HirExprPtr sizeCall = makeMethodCall(itPath, qn, {});
        HirExprPtr elem = makeMethodCall(itPath, e.iterClassName + ".get", {idxPath});
        HirExprPtr cond = makeBin(MirBinOp::Lt, idxPath, sizeCall);
        HirExprPtr loopBody = wrapWithPat(e.pat, elem, makeBlockWithTail(body, incIdx()));
        HirExprPtr withIdx = makeLet("__for_i", makeIntLit(0), makeWhile(cond, loopBody, e.label));
        return makeLet("__for_it", iterExpr, withIdx);
    }

    return body;
}

HirFn HirBuilder::buildFn(const ItemFun& fn) {
    HirFn out;
    out.name = fn.name;
    for (const auto& g : fn.generics) {
        out.generics.push_back(g.name);
    }
    pushGenerics(fn.generics);
    out.ret = astType(fn.ret);
    for (const auto& p : fn.params) {
        out.params.push_back({p.first, astType(p.second)});
    }
    out.body = buildExpr(fn.body);
    popGenerics();
    return out;
}

HirClass HirBuilder::buildClass(const ItemClass& cls) {
    HirClass out;
    out.name = cls.name;
    for (const auto& g : cls.generics) {
        out.generics.push_back(g.name);
    }
    out.typeId = cls.typeId;
    out.totalFieldSize = cls.totalFieldSize;
    out.vtableEntryCount = cls.vtableEntryCount;
    out.vtableMethodNames = cls.vtableMethodNames;
    out.itables = cls.itables;
    pushGenerics(cls.generics);
    for (const auto& pd : cls.primaryCtor) {
        out.ctorParams.push_back({
            pd.name, astType(pd.ty), pd.isVal, pd.isVar
        });
    }
    for (const auto& m : cls.methods) {
        out.methods.push_back(buildFn(m));
    }
    popGenerics();
    return out;
}

HirModule HirBuilder::build(const Module& mod) {
    HirModule hir;
    for (const auto& item : mod.items) {
        if (!item) continue;
        std::visit([&](const auto& i) {
            using T = std::decay_t<decltype(i)>;
            if constexpr (std::is_same_v<T, ItemFun>) {
                hir.items.push_back(buildFn(i));
            } else if constexpr (std::is_same_v<T, ItemClass>) {
                hir.items.push_back(buildClass(i));
            }
        }, *item);
    }
    return hir;
}

} // namespace loxis::v2