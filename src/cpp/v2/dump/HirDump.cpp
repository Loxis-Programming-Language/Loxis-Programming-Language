#include "Dump.hpp"

#include "../hir/HIR.hpp"

#include <sstream>
#include <variant>

namespace loxis::v2::dump {
namespace {

std::string quoted(const std::string& text) { return "\"" + text + "\""; }

std::string tyText(const CoreTyPtr& ty) { return ty ? ty->toString() : "?"; }

std::string locText(const SourceLocation& loc) {
    if (loc.file.empty() && loc.line == 0) return "";
    return "@" + std::to_string(loc.line) + ":" + std::to_string(loc.col);
}

const char* binOpName(MirBinOp op) {
    switch (op) {
        case MirBinOp::Add: return "+";   case MirBinOp::Sub: return "-";
        case MirBinOp::Mul: return "*";   case MirBinOp::Div: return "/";
        case MirBinOp::Rem: return "%";   case MirBinOp::Shl: return "<<";
        case MirBinOp::Shr: return ">>";  case MirBinOp::BitAnd: return "&";
        case MirBinOp::BitOr: return "|"; case MirBinOp::BitXor: return "^";
        case MirBinOp::And: return "&&";  case MirBinOp::Or: return "||";
        case MirBinOp::Eq: return "==";   case MirBinOp::Ne: return "!=";
        case MirBinOp::Lt: return "<";    case MirBinOp::Le: return "<=";
        case MirBinOp::Gt: return ">";    case MirBinOp::Ge: return ">=";
    }
    return "?";
}

const char* unaryOpName(MirUnaryOp op) {
    switch (op) {
        case MirUnaryOp::Neg:    return "-";
        case MirUnaryOp::Not:    return "!";
        case MirUnaryOp::Deref:  return "*";
        case MirUnaryOp::Ref:    return "&";
        case MirUnaryOp::RefMut: return "&mut";
        case MirUnaryOp::ToStr:  return "to-str";
    }
    return "?";
}

const char* assignOpName(MirAssignOp op) {
    switch (op) {
        case MirAssignOp::Set: return "=";      case MirAssignOp::AddEq: return "+=";
        case MirAssignOp::SubEq: return "-=";   case MirAssignOp::MulEq: return "*=";
        case MirAssignOp::DivEq: return "/=";   case MirAssignOp::RemEq: return "%=";
        case MirAssignOp::AmpEq: return "&=";   case MirAssignOp::PipeEq: return "|=";
        case MirAssignOp::CaretEq: return "^="; case MirAssignOp::ShlEq: return "<<=";
        case MirAssignOp::ShrEq: return ">>=";
    }
    return "?";
}

std::string litText(const HirLit& lit) {
    return std::visit([](const auto& value) -> std::string {
        using V = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<V, std::string>) return quoted(value);
        else if constexpr (std::is_same_v<V, bool>)   return value ? "true" : "false";
        else if constexpr (std::is_same_v<V, char>)   return "'" + std::string(1, value) + "'";
        else if constexpr (std::is_same_v<V, double>) return std::to_string(value);
        else return std::to_string(value);
    }, lit.val);
}

void dumpHExpr(Printer& p, const HirExprPtr& expr);
void dumpHStmt(Printer& p, const HirStmtPtr& stmt);
void dumpHPat(Printer& p, const HirPatPtr& pat);
void dumpHFn(Printer& p, const HirFn& fn);

struct HExprPrinter {
    Printer& p;
    const HirExpr& self;

    void open(const std::string& kind) const { p.open(kind + locText(self.loc)); }

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, HirLit>) {
            p.line("HirLit " + litText(n) + " : " + tyText(n.ty) + locText(self.loc));
        } else if constexpr (std::is_same_v<T, HirPath>) {
            open("HirPath " + n.name + " : " + tyText(n.ty));
            if (n.globalSlot != ~0u) p.field("globalSlot", std::to_string(n.globalSlot));
            if (n.captureOffset != ~0u) p.field("captureOffset", std::to_string(n.captureOffset));
            p.close();
        } else if constexpr (std::is_same_v<T, HirTuple>) {
            open("HirTuple : " + tyText(n.ty));
            for (const auto& elem : n.elems) dumpHExpr(p, elem);
            p.close();
        } else if constexpr (std::is_same_v<T, HirArray>) {
            open("HirArray : " + tyText(n.ty));
            for (const auto& elem : n.elems) dumpHExpr(p, elem);
            p.close();
        } else if constexpr (std::is_same_v<T, HirStruct>) {
            open("HirStruct " + n.typeName + " : " + tyText(n.ty));
            p.field("typeId", std::to_string(n.typeId));
            p.field("size", std::to_string(n.totalFieldSize));
            for (const auto& field : n.fields) {
                p.open("field " + field.name);
                dumpHExpr(p, field.expr);
                p.close();
            }
            p.close();
        } else if constexpr (std::is_same_v<T, HirCall>) {
            open("HirCall : " + tyText(n.ty));
            if (n.staticName) p.field("staticName", *n.staticName);
            if (n.ctorLabel.size()) p.field("ctorLabel", n.ctorLabel);
            p.open("callee"); dumpHExpr(p, n.func); p.close();
            for (const auto& arg : n.args) dumpHExpr(p, arg);
            p.close();
        } else if constexpr (std::is_same_v<T, HirMethodCall>) {
            open("HirMethodCall " + quoted(n.method) + " : " + tyText(n.ty));
            if (n.qualifiedName.size()) p.field("qualified", n.qualifiedName);
            if (n.vtableIndex != ~0u) p.field("vtableIndex", std::to_string(n.vtableIndex));
            if (n.isInterfaceDispatch) p.field("interface", std::to_string(n.ifaceTypeId)
                                                          + "#" + std::to_string(n.ifaceMethodIndex));
            p.open("recv"); dumpHExpr(p, n.recv); p.close();
            for (const auto& arg : n.args) dumpHExpr(p, arg);
            p.close();
        } else if constexpr (std::is_same_v<T, HirField>) {
            open("HirField " + quoted(n.field) + " : " + tyText(n.ty));
            p.field("offset", std::to_string(n.fieldOffset));
            p.open("obj"); dumpHExpr(p, n.obj); p.close();
            p.close();
        } else if constexpr (std::is_same_v<T, HirIndex>) {
            open("HirIndex : " + tyText(n.ty));
            dumpHExpr(p, n.obj);
            dumpHExpr(p, n.idx);
            p.close();
        } else if constexpr (std::is_same_v<T, HirUnary>) {
            open(std::string("HirUnary ") + unaryOpName(n.op) + " : " + tyText(n.ty));
            dumpHExpr(p, n.operand);
            p.close();
        } else if constexpr (std::is_same_v<T, HirBinary>) {
            open(std::string("HirBinary ") + binOpName(n.op) + " : " + tyText(n.ty));
            dumpHExpr(p, n.l);
            dumpHExpr(p, n.r);
            p.close();
        } else if constexpr (std::is_same_v<T, HirAssign>) {
            open(std::string("HirAssign ") + assignOpName(n.op) + " : " + tyText(n.ty));
            dumpHExpr(p, n.lhs);
            dumpHExpr(p, n.rhs);
            p.close();
        } else if constexpr (std::is_same_v<T, HirBlock>) {
            open("HirBlock : " + tyText(n.ty));
            for (const auto& stmt : n.stmts) dumpHStmt(p, stmt);
            if (n.tail) { p.open("tail"); dumpHExpr(p, n.tail); p.close(); }
            p.close();
        } else if constexpr (std::is_same_v<T, HirIf>) {
            open("HirIf : " + tyText(n.ty));
            p.open("cond"); dumpHExpr(p, n.cond); p.close();
            p.open("then"); dumpHExpr(p, n.then_); p.close();
            if (n.else_) { p.open("else"); dumpHExpr(p, n.else_); p.close(); }
            p.close();
        } else if constexpr (std::is_same_v<T, HirWhile>) {
            open(std::string("HirWhile") + (n.label ? " label=" + *n.label : ""));
            p.open("cond"); dumpHExpr(p, n.cond); p.close();
            p.open("body"); dumpHExpr(p, n.body); p.close();
            p.close();
        } else if constexpr (std::is_same_v<T, HirFor>) {
            open(std::string("HirFor") + (n.label ? " label=" + *n.label : ""));
            p.open("pat"); dumpHPat(p, n.pat); p.close();
            p.open("iter"); dumpHExpr(p, n.iter); p.close();
            p.open("body"); dumpHExpr(p, n.body); p.close();
            p.close();
        } else if constexpr (std::is_same_v<T, HirLoop>) {
            open(std::string("HirLoop") + (n.label ? " label=" + *n.label : ""));
            dumpHExpr(p, n.body);
            p.close();
        } else if constexpr (std::is_same_v<T, HirWhen>) {
            open("HirWhen : " + tyText(n.ty));
            p.open("scrut"); dumpHExpr(p, n.scrut); p.close();
            for (const auto& arm : n.arms) {
                p.open(arm.isElse ? "arm else" : "arm");
                if (auto expr = std::get_if<HirExprPtr>(&arm.condition); expr && *expr) {
                    p.open("cond"); dumpHExpr(p, *expr); p.close();
                } else if (auto pat = std::get_if<HirPatPtr>(&arm.condition); pat && *pat) {
                    p.open("pat"); dumpHPat(p, *pat); p.close();
                }
                p.open("body"); dumpHExpr(p, arm.body); p.close();
                p.close();
            }
            p.close();
        } else if constexpr (std::is_same_v<T, HirBreak>) {
            open(std::string("HirBreak") + (n.label ? " label=" + *n.label : ""));
            if (n.expr) dumpHExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, HirContinue>) {
            p.line(std::string("HirContinue") + (n.label ? " label=" + *n.label : "")
                   + locText(self.loc));
        } else if constexpr (std::is_same_v<T, HirReturn>) {
            open("HirReturn");
            if (n.expr) dumpHExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, HirThrow>) {
            open("HirThrow");
            if (n.expr) dumpHExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, HirTryCatch>) {
            open("HirTryCatch : " + tyText(n.ty));
            p.open("body"); dumpHExpr(p, n.body); p.close();
            for (const auto& clause : n.catches) {
                p.open("catch " + clause.name + " : " + tyText(clause.exceptionTy));
                dumpHExpr(p, clause.body);
                p.close();
            }
            if (n.finallyBody) { p.open("finally"); dumpHExpr(p, n.finallyBody); p.close(); }
            p.close();
        } else if constexpr (std::is_same_v<T, HirRef>) {
            open(n.mut ? "HirRef mut" : "HirRef");
            dumpHExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, HirDeref>) {
            open("HirDeref : " + tyText(n.ty));
            dumpHExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, HirTry>) {
            open("HirTry : " + tyText(n.ty));
            dumpHExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, HirCast>) {
            open("HirCast : " + tyText(n.ty) + " as " + tyText(n.targetTy));
            dumpHExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, HirSafeCall>) {
            open("HirSafeCall " + quoted(n.method) + " : " + tyText(n.ty));
            p.open("recv"); dumpHExpr(p, n.recv); p.close();
            for (const auto& arg : n.args) dumpHExpr(p, arg);
            p.close();
        } else if constexpr (std::is_same_v<T, HirSafeField>) {
            open("HirSafeField " + quoted(n.field) + " : " + tyText(n.ty));
            p.open("recv"); dumpHExpr(p, n.recv); p.close();
            p.close();
        } else if constexpr (std::is_same_v<T, HirElvis>) {
            open("HirElvis : " + tyText(n.ty));
            dumpHExpr(p, n.lhs);
            dumpHExpr(p, n.rhs);
            p.close();
        } else if constexpr (std::is_same_v<T, HirForceUnwrap>) {
            open("HirForceUnwrap : " + tyText(n.ty));
            dumpHExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, HirIs>) {
            open("HirIs : " + tyText(n.ty));
            p.field("targetTypeId", std::to_string(n.targetTypeId));
            dumpHExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, HirIsNot>) {
            open("HirIsNot : " + tyText(n.ty));
            p.field("targetTypeId", std::to_string(n.targetTypeId));
            dumpHExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, HirNull>) {
            p.line("HirNull : " + tyText(n.ty) + locText(self.loc));
        } else if constexpr (std::is_same_v<T, HirClosure>) {
            open("HirClosure " + n.functionName + " : " + tyText(n.ty));
            p.field("envTypeId", std::to_string(n.envTypeId));
            for (const auto& capture : n.captures) {
                p.line("capture " + capture.name + " : " + tyText(capture.ty)
                       + " @" + std::to_string(capture.offset));
            }
            p.close();
        } else if constexpr (std::is_same_v<T, HirEnumCtor>) {
            open("HirEnumCtor " + n.enumName + " : " + tyText(n.ty));
            p.field("typeId", std::to_string(n.typeId));
            p.field("variant", std::to_string(n.variantIndex));
            for (const auto& arg : n.args) dumpHExpr(p, arg);
            p.close();
        }
    }
};

void dumpHExpr(Printer& p, const HirExprPtr& expr) {
    if (!expr) { p.line("HExpr <none>"); return; }
    std::visit(HExprPrinter{p, *expr}, *expr);
}

struct HPatPrinter {
    Printer& p;

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, HirPatWild>) {
            p.line("HirPatWild _");
        } else if constexpr (std::is_same_v<T, HirPatBind>) {
            p.open(std::string("HirPatBind ") + n.name + (n.mut ? " (mut)" : ""));
            if (n.sub) dumpHPat(p, n.sub);
            p.close();
        } else if constexpr (std::is_same_v<T, HirPatEnum>) {
            p.open("HirPatEnum variant=" + std::to_string(n.variantIndex));
            for (const auto& elem : n.elems) dumpHPat(p, elem);
            p.close();
        }
    }
};

void dumpHPat(Printer& p, const HirPatPtr& pat) {
    if (!pat) { p.line("HPat <none>"); return; }
    std::visit(HPatPrinter{p}, *pat);
}

struct HStmtPrinter {
    Printer& p;

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, HirStmtLet>) {
            p.open("HirStmtLet : " + tyText(n.ty));
            if (n.pat) dumpHPat(p, n.pat);
            if (n.init) { p.open("init"); dumpHExpr(p, n.init); p.close(); }
            p.close();
        } else if constexpr (std::is_same_v<T, HirStmtExpr>) {
            p.open("HirStmtExpr");
            dumpHExpr(p, n.expr);
            p.close();
        }
    }
};

void dumpHStmt(Printer& p, const HirStmtPtr& stmt) {
    if (!stmt) { p.line("HStmt <none>"); return; }
    std::visit(HStmtPrinter{p}, *stmt);
}

void dumpHFn(Printer& p, const HirFn& fn) {
    p.open("HirFn " + fn.name + " -> " + tyText(fn.ret)
           + (fn.globalSlot != ~0u ? " globalSlot=" + std::to_string(fn.globalSlot) : ""));
    for (const auto& [name, ty] : fn.params) p.line("param " + name + " : " + tyText(ty));
    if (fn.body) { p.open("body"); dumpHExpr(p, fn.body); p.close(); }
    p.close();
}

} // namespace

void dumpHir(const HirModule& hir, std::ostream& os) {
    Printer p(os);
    os << "; " << hir.items.size() << " HIR items\n";
    for (const auto& item : hir.items) {
        if (const auto* fn = std::get_if<HirFn>(&item)) {
            dumpHFn(p, *fn);
        } else if (const auto* cls = std::get_if<HirClass>(&item)) {
            p.open("HirClass " + cls->name + " typeId=" + std::to_string(cls->typeId)
                   + " parent=" + std::to_string(cls->parentTypeId)
                   + (cls->isSingleton ? " singleton" : ""));
            for (const auto& field : cls->fields) {
                p.line("field " + field.name + " : " + tyText(field.ty)
                       + " +" + std::to_string(field.offset));
            }
            for (const auto& param : cls->ctorParams) {
                p.line("ctorParam " + param.name + " : " + tyText(param.ty));
            }
            for (const auto& method : cls->methods) dumpHFn(p, method);
            p.close();
        }
    }
}

} // namespace loxis::v2::dump
