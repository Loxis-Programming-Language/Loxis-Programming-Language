#include "Dump.hpp"

#include "../AST.hpp"

#include <sstream>
#include <variant>

namespace loxis::v2::dump {
namespace {

std::string quoted(const std::string& text) { return "\"" + text + "\""; }

// "@line:col", or empty when the node carries no location. Postfix nodes only
// gained their own location recently; an empty suffix here is what an
// un-anchored node looks like.
std::string locSuffix(const SourceLocation& loc) {
    if (loc.file.empty() && loc.line == 0) return "";
    return "@" + std::to_string(loc.line) + ":" + std::to_string(loc.col);
}

std::string pathText(const Path& path) {
    std::ostringstream out;
    for (size_t i = 0; i < path.segs.size(); ++i) {
        if (i) out << ".";
        out << path.segs[i];
    }
    return out.str();
}

const char* unaryOpName(ExprUnary::Op op) {
    switch (op) {
        case ExprUnary::Neg:    return "Neg";
        case ExprUnary::Not:    return "Not";
        case ExprUnary::Deref:  return "Deref";
        case ExprUnary::Ref:    return "Ref";
        case ExprUnary::RefMut: return "RefMut";
    }
    return "?";
}

const char* binaryOpName(ExprBinary::Op op) {
    switch (op) {
        case ExprBinary::Add:    return "Add";
        case ExprBinary::Sub:    return "Sub";
        case ExprBinary::Mul:    return "Mul";
        case ExprBinary::Div:    return "Div";
        case ExprBinary::Rem:    return "Rem";
        case ExprBinary::Shl:    return "Shl";
        case ExprBinary::Shr:    return "Shr";
        case ExprBinary::BitAnd: return "BitAnd";
        case ExprBinary::BitOr:  return "BitOr";
        case ExprBinary::BitXor: return "BitXor";
        case ExprBinary::And:    return "And";
        case ExprBinary::Or:     return "Or";
        case ExprBinary::Eq:     return "Eq";
        case ExprBinary::Ne:     return "Ne";
        case ExprBinary::Lt:     return "Lt";
        case ExprBinary::Le:     return "Le";
        case ExprBinary::Gt:     return "Gt";
        case ExprBinary::Ge:     return "Ge";
    }
    return "?";
}

const char* assignOpName(ExprAssign::Op op) {
    switch (op) {
        case ExprAssign::Set:     return "Set";
        case ExprAssign::AddEq:   return "AddEq";
        case ExprAssign::SubEq:   return "SubEq";
        case ExprAssign::MulEq:   return "MulEq";
        case ExprAssign::DivEq:   return "DivEq";
        case ExprAssign::RemEq:   return "RemEq";
        case ExprAssign::AmpEq:   return "AmpEq";
        case ExprAssign::PipeEq:  return "PipeEq";
        case ExprAssign::CaretEq: return "CaretEq";
        case ExprAssign::ShlEq:   return "ShlEq";
        case ExprAssign::ShrEq:   return "ShrEq";
    }
    return "?";
}

const char* visibilityName(Visibility v) {
    switch (v) {
        case Visibility::Private:  return "private";
        case Visibility::Internal: return "internal";
        case Visibility::Public:   return "public";
    }
    return "?";
}

const char* classModifierName(ClassModifier m) {
    switch (m) {
        case ClassModifier::None:     return "none";
        case ClassModifier::Open:     return "open";
        case ClassModifier::Abstract: return "abstract";
        case ClassModifier::Data:     return "data";
    }
    return "?";
}

std::string literalText(const ExprLit& lit) {
    return std::visit([](const auto& value) -> std::string {
        using V = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<V, std::string>) return quoted(value);
        else if constexpr (std::is_same_v<V, bool>)    return value ? "true" : "false";
        else if constexpr (std::is_same_v<V, char>)    return "'" + std::string(1, value) + "'";
        else if constexpr (std::is_same_v<V, double>)  return std::to_string(value);
        else                                            return std::to_string(value);
    }, lit.val);
}

void dumpExpr(Printer& p, const ExprPtr& expr);
void dumpStmt(Printer& p, const StmtPtr& stmt);
void dumpPat(Printer& p, const PatPtr& pat);
void dumpType(Printer& p, const TypePtr& type);
void dumpItem(Printer& p, const ItemPtr& item);

// ---- types ----
struct TypePrinter {
    Printer& p;

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, AstTyPath>) {
            p.open(std::string("TyPath ") + pathText(n.path));
            for (const auto& arg : n.path.args) dumpType(p, arg);
            p.close();
        } else if constexpr (std::is_same_v<T, AstTyTuple>) {
            p.open("TyTuple");
            for (const auto& elem : n.elems) dumpType(p, elem);
            p.close();
        } else if constexpr (std::is_same_v<T, AstTyArray>) {
            p.open("TyArray");
            p.field("size", "");
            dumpType(p, n.elem);
            p.close();
        } else if constexpr (std::is_same_v<T, AstTySlice>) {
            p.open("TySlice");
            dumpType(p, n.elem);
            p.close();
        } else if constexpr (std::is_same_v<T, AstTyRef>) {
            p.open(n.mut ? "TyRef mut" : "TyRef");
            dumpType(p, n.elem);
            p.close();
        } else if constexpr (std::is_same_v<T, AstTyPtr>) {
            p.open(n.mut ? "TyPtr mut" : "TyPtr");
            dumpType(p, n.elem);
            p.close();
        } else if constexpr (std::is_same_v<T, AstTyFn>) {
            p.open("TyFn");
            for (const auto& param : n.params) dumpType(p, param);
            p.open("ret");
            dumpType(p, n.ret);
            p.close();
            p.close();
        } else if constexpr (std::is_same_v<T, AstTyInfer>) {
            p.line("TyInfer");
        } else if constexpr (std::is_same_v<T, AstTyNullable>) {
            p.open("TyNullable");
            dumpType(p, n.inner);
            p.close();
        }
    }
};

void dumpType(Printer& p, const TypePtr& type) {
    if (!type) { p.line("Ty <none>"); return; }
    std::visit(TypePrinter{p}, *type);
}

// ---- patterns ----
struct PatPrinter {
    Printer& p;

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, PatWild>) {
            p.line("PatWild _");
        } else if constexpr (std::is_same_v<T, PatBind>) {
            p.open(std::string("PatBind ") + n.name + (n.mut ? " (mut)" : ""));
            if (n.sub) dumpPat(p, n.sub);
            p.close();
        } else if constexpr (std::is_same_v<T, PatLit>) {
            p.open("PatLit");
            dumpExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, PatTuple>) {
            p.open("PatTuple");
            for (const auto& elem : n.elems) dumpPat(p, elem);
            p.close();
        } else if constexpr (std::is_same_v<T, PatField>) {
            p.open(std::string("PatField ") + n.name);
            if (n.pat) dumpPat(p, n.pat);
            p.close();
        } else if constexpr (std::is_same_v<T, PatStruct>) {
            p.open(std::string("PatStruct ") + pathText(n.path) + (n.rest ? " .." : ""));
            for (const auto& field : n.fields) {
                p.open("field " + field.name);
                if (field.pat) dumpPat(p, field.pat);
                p.close();
            }
            p.close();
        } else if constexpr (std::is_same_v<T, PatEnum>) {
            p.open(std::string("PatEnum ") + pathText(n.path));
            for (const auto& elem : n.elems) dumpPat(p, elem);
            p.close();
        } else if constexpr (std::is_same_v<T, PatRef>) {
            p.open(n.mut ? "PatRef mut" : "PatRef");
            dumpPat(p, n.pat);
            p.close();
        } else if constexpr (std::is_same_v<T, PatRest>) {
            p.line("PatRest ..");
        }
    }
};

void dumpPat(Printer& p, const PatPtr& pat) {
    if (!pat) { p.line("Pat <none>"); return; }
    std::visit(PatPrinter{p}, *pat);
}

// ---- expressions ----
struct ExprPrinter {
    Printer& p;
    const Expr& self;

    void open(const std::string& kind) const { p.open(kind + locSuffix(self.loc)); }

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, ExprLit>) {
            p.line("ExprLit " + literalText(n));
        } else if constexpr (std::is_same_v<T, ExprPath>) {
            open("ExprPath " + pathText(n.path));
            p.close();
        } else if constexpr (std::is_same_v<T, ExprTuple>) {
            open("ExprTuple");
            for (const auto& elem : n.elems) dumpExpr(p, elem);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprArray>) {
            open("ExprArray");
            for (const auto& elem : n.elems) dumpExpr(p, elem);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprMap>) {
            open("ExprMap");
            for (const auto& entry : n.entries) {
                p.open("entry");
                dumpExpr(p, entry.key);
                dumpExpr(p, entry.value);
                p.close();
            }
            p.close();
        } else if constexpr (std::is_same_v<T, ExprStruct>) {
            open("ExprStruct " + pathText(n.path) + (n.rest ? " .." : ""));
            for (const auto& field : n.fields) {
                p.open("field " + field.name);
                dumpExpr(p, field.expr);
                p.close();
            }
            p.close();
        } else if constexpr (std::is_same_v<T, ExprCall>) {
            open("ExprCall");
            p.open("callee");
            dumpExpr(p, n.func);
            p.close();
            for (const auto& arg : n.args) dumpExpr(p, arg);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprMethodCall>) {
            open("ExprMethodCall " + quoted(n.method));
            p.open("recv");
            dumpExpr(p, n.recv);
            p.close();
            for (size_t i = 0; i < n.args.size(); ++i) {
                p.open("arg " + std::to_string(i));
                dumpExpr(p, n.args[i]);
                p.close();
            }
            if (n.qualifiedName.size()) p.field("resolved", n.qualifiedName);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprField>) {
            open("ExprField " + quoted(n.field));
            p.open("recv");
            dumpExpr(p, n.obj);
            p.close();
            p.close();
        } else if constexpr (std::is_same_v<T, ExprIndex>) {
            open("ExprIndex");
            dumpExpr(p, n.obj);
            dumpExpr(p, n.idx);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprUnary>) {
            open(std::string("ExprUnary ") + unaryOpName(n.op));
            dumpExpr(p, n.operand);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprBinary>) {
            open(std::string("ExprBinary ") + binaryOpName(n.op));
            dumpExpr(p, n.l);
            dumpExpr(p, n.r);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprAssign>) {
            open(std::string("ExprAssign ") + assignOpName(n.op));
            dumpExpr(p, n.lhs);
            dumpExpr(p, n.rhs);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprBlock>) {
            open("ExprBlock");
            for (const auto& stmt : n.stmts) dumpStmt(p, stmt);
            if (n.tail) {
                p.open("tail");
                dumpExpr(p, n.tail);
                p.close();
            }
            p.close();
        } else if constexpr (std::is_same_v<T, ExprIf>) {
            open("ExprIf");
            p.open("cond"); dumpExpr(p, n.cond); p.close();
            p.open("then"); dumpExpr(p, n.then_); p.close();
            if (n.else_) { p.open("else"); dumpExpr(p, n.else_); p.close(); }
            p.close();
        } else if constexpr (std::is_same_v<T, ExprWhile>) {
            open(std::string("ExprWhile") + (n.label ? " label=" + *n.label : ""));
            p.open("cond"); dumpExpr(p, n.cond); p.close();
            p.open("body"); dumpExpr(p, n.body); p.close();
            p.close();
        } else if constexpr (std::is_same_v<T, ExprRange>) {
            open(n.inclusive ? "ExprRange ..=" : "ExprRange ..");
            dumpExpr(p, n.start);
            dumpExpr(p, n.end);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprFor>) {
            open(std::string("ExprFor") + (n.label ? " label=" + *n.label : ""));
            p.open("pat"); dumpPat(p, n.pat); p.close();
            p.open("iter"); dumpExpr(p, n.iter); p.close();
            p.open("body"); dumpExpr(p, n.body); p.close();
            p.close();
        } else if constexpr (std::is_same_v<T, ExprLoop>) {
            open(std::string("ExprLoop") + (n.label ? " label=" + *n.label : ""));
            dumpExpr(p, n.body);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprWhen>) {
            open("ExprWhen");
            p.open("scrut"); dumpExpr(p, n.scrut); p.close();
            for (const auto& arm : n.arms) {
                p.open(arm.isElse ? "arm else" : "arm");
                if (auto expr = std::get_if<ExprPtr>(&arm.condition); expr && *expr) {
                    p.open("cond"); dumpExpr(p, *expr); p.close();
                } else if (auto pat = std::get_if<PatPtr>(&arm.condition); pat && *pat) {
                    p.open("pat"); dumpPat(p, *pat); p.close();
                }
                p.open("body"); dumpExpr(p, arm.body); p.close();
                p.close();
            }
            p.close();
        } else if constexpr (std::is_same_v<T, ExprBreak>) {
            open(std::string("ExprBreak") + (n.label ? " label=" + *n.label : ""));
            if (n.expr) dumpExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprContinue>) {
            p.line(std::string("ExprContinue") + (n.label ? " label=" + *n.label : "")
                   + locSuffix(self.loc));
        } else if constexpr (std::is_same_v<T, ExprReturn>) {
            open("ExprReturn");
            if (n.expr) dumpExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprThrow>) {
            open("ExprThrow");
            if (n.expr) dumpExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprTryCatch>) {
            open("ExprTryCatch");
            p.open("body"); dumpExpr(p, n.body); p.close();
            for (const auto& clause : n.catches) {
                p.open("catch " + clause.name);
                if (clause.ty) dumpType(p, clause.ty);
                dumpExpr(p, clause.body);
                p.close();
            }
            if (n.finallyBody) { p.open("finally"); dumpExpr(p, n.finallyBody); p.close(); }
            p.close();
        } else if constexpr (std::is_same_v<T, ExprClosure>) {
            open("ExprClosure");
            for (const auto& [name, ty] : n.params) {
                p.open("param " + name);
                if (ty) dumpType(p, ty);
                p.close();
            }
            if (n.ret) { p.open("ret"); dumpType(p, n.ret); p.close(); }
            p.open("body"); dumpExpr(p, n.body); p.close();
            p.close();
        } else if constexpr (std::is_same_v<T, ExprRef>) {
            open(n.mut ? "ExprRef mut" : "ExprRef");
            dumpExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprDeref>) {
            open("ExprDeref");
            dumpExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprTry>) {
            open("ExprTry");
            dumpExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprCast>) {
            open("ExprCast");
            dumpExpr(p, n.expr);
            dumpType(p, n.ty);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprSafeCall>) {
            open("ExprSafeCall " + quoted(n.method));
            p.open("recv"); dumpExpr(p, n.recv); p.close();
            for (const auto& arg : n.args) dumpExpr(p, arg);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprSafeField>) {
            open("ExprSafeField " + quoted(n.field));
            p.open("recv"); dumpExpr(p, n.recv); p.close();
            p.close();
        } else if constexpr (std::is_same_v<T, ExprElvis>) {
            open("ExprElvis");
            dumpExpr(p, n.lhs);
            dumpExpr(p, n.rhs);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprForceUnwrap>) {
            open("ExprForceUnwrap");
            dumpExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprIs>) {
            open("ExprIs");
            dumpExpr(p, n.expr);
            dumpType(p, n.ty);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprIsNot>) {
            open("ExprIsNot");
            dumpExpr(p, n.expr);
            dumpType(p, n.ty);
            p.close();
        } else if constexpr (std::is_same_v<T, ExprNull>) {
            p.line("ExprNull" + locSuffix(self.loc));
        } else if constexpr (std::is_same_v<T, ExprStringTemplate>) {
            open("ExprStringTemplate");
            for (const auto& part : n.parts) {
                if (auto text = std::get_if<std::string>(&part)) {
                    p.line("text " + quoted(*text));
                } else {
                    p.open("expr");
                    dumpExpr(p, std::get<ExprPtr>(part));
                    p.close();
                }
            }
            p.close();
        }
    }
};

void dumpExpr(Printer& p, const ExprPtr& expr) {
    if (!expr) { p.line("Expr <none>"); return; }
    std::visit(ExprPrinter{p, *expr}, *expr);
}

// ---- statements ----
struct StmtPrinter {
    Printer& p;

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, StmtLet>) {
            p.open("StmtLet");
            if (n.pat) dumpPat(p, n.pat);
            if (n.ty) { p.open("ty"); dumpType(p, n.ty); p.close(); }
            if (n.init) { p.open("init"); dumpExpr(p, n.init); p.close(); }
            p.close();
        } else if constexpr (std::is_same_v<T, StmtExpr>) {
            p.open("StmtExpr");
            dumpExpr(p, n.expr);
            p.close();
        } else if constexpr (std::is_same_v<T, StmtItem>) {
            p.open("StmtItem");
            dumpItem(p, n.item);
            p.close();
        }
    }
};

void dumpStmt(Printer& p, const StmtPtr& stmt) {
    if (!stmt) { p.line("Stmt <none>"); return; }
    std::visit(StmtPrinter{p}, *stmt);
}

// ---- items ----
void dumpParams(Printer& p, const std::vector<std::pair<std::string, TypePtr>>& params);

struct ItemPrinter {
    Printer& p;

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, ItemFun>) {
            p.open(std::string("ItemFun ") + n.name + " (" + visibilityName(n.visibility) + ")");
            dumpParams(p, n.params);
            if (n.ret) { p.open("ret"); dumpType(p, n.ret); p.close(); }
            if (n.receiverType) { p.open("extension receiver"); dumpType(p, n.receiverType); p.close(); }
            if (n.body) { p.open("body"); dumpExpr(p, n.body); p.close(); }
            p.close();
        } else if constexpr (std::is_same_v<T, ItemClass>) {
            p.open(std::string("ItemClass ") + n.name + " (" + classModifierName(n.modifier) + ")");
            if (n.superClass) p.field("super", pathText(*n.superClass));
            for (const auto& iface : n.interfaces) p.field("implements", pathText(iface));
            for (const auto& field : n.fields) {
                p.open("field " + field.name + (field.isVal ? " (val)" : " (var)"));
                if (field.ty) dumpType(p, field.ty);
                if (field.initializer) dumpExpr(p, field.initializer);
                p.close();
            }
            for (const auto& method : n.methods) {
                p.open("method " + method.name);
                dumpParams(p, method.params);
                if (method.ret) { p.open("ret"); dumpType(p, method.ret); p.close(); }
                if (method.body) { p.open("body"); dumpExpr(p, method.body); p.close(); }
                p.close();
            }
            p.close();
        } else if constexpr (std::is_same_v<T, ItemEnumClass>) {
            p.open("ItemEnumClass " + n.name);
            for (const auto& variant : n.variants) {
                p.open(std::string("variant ") + variant.name + (variant.isTuple ? " (tuple)" : ""));
                dumpParams(p, variant.fields);
                p.close();
            }
            p.close();
        } else if constexpr (std::is_same_v<T, ItemInterface>) {
            p.open("ItemInterface " + n.name);
            for (const auto& sup : n.supers) p.field("extends", pathText(sup));
            for (const auto& method : n.methods) {
                p.open("method " + method.name);
                dumpParams(p, method.params);
                if (method.ret) { p.open("ret"); dumpType(p, method.ret); p.close(); }
                if (method.defaultBody) { p.open("default"); dumpExpr(p, method.defaultBody); p.close(); }
                p.close();
            }
            p.close();
        } else if constexpr (std::is_same_v<T, ItemObject>) {
            p.open("ItemObject " + n.name);
            if (n.superClass) p.field("super", pathText(*n.superClass));
            for (const auto& method : n.methods) {
                p.open("method " + method.name);
                dumpParams(p, method.params);
                if (method.body) { p.open("body"); dumpExpr(p, method.body); p.close(); }
                p.close();
            }
            p.close();
        } else if constexpr (std::is_same_v<T, ItemImport>) {
            p.open("ItemImport " + pathText(n.module));
            for (const auto& item : n.items) {
                p.line("item " + item.name + (item.alias ? " as " + *item.alias : ""));
            }
            p.close();
        } else if constexpr (std::is_same_v<T, ItemVal>) {
            p.open("ItemVal " + n.name);
            if (n.ty) { p.open("ty"); dumpType(p, n.ty); p.close(); }
            if (n.init) { p.open("init"); dumpExpr(p, n.init); p.close(); }
            p.close();
        } else if constexpr (std::is_same_v<T, ItemVar>) {
            p.open("ItemVar " + n.name);
            if (n.ty) { p.open("ty"); dumpType(p, n.ty); p.close(); }
            if (n.init) { p.open("init"); dumpExpr(p, n.init); p.close(); }
            p.close();
        } else if constexpr (std::is_same_v<T, ItemConst>) {
            p.open("ItemConst " + n.name);
            if (n.ty) { p.open("ty"); dumpType(p, n.ty); p.close(); }
            if (n.init) { p.open("init"); dumpExpr(p, n.init); p.close(); }
            p.close();
        } else if constexpr (std::is_same_v<T, ItemTypeAlias>) {
            p.open("ItemTypeAlias " + n.name);
            if (n.underlying) dumpType(p, n.underlying);
            p.close();
        }
    }
};

void dumpParams(Printer& p, const std::vector<std::pair<std::string, TypePtr>>& params) {
    for (const auto& [name, ty] : params) {
        p.open("param " + name);
        if (ty) dumpType(p, ty);
        p.close();
    }
}

void dumpItem(Printer& p, const ItemPtr& item) {
    if (!item) { p.line("Item <none>"); return; }
    std::visit(ItemPrinter{p}, *item);
}

} // namespace

void dumpAst(const Module& mod, std::ostream& os) {
    Printer p(os);
    p.open("Module " + quoted(mod.name));
    for (const auto& item : mod.items) dumpItem(p, item);
    p.close();
}

} // namespace loxis::v2::dump
