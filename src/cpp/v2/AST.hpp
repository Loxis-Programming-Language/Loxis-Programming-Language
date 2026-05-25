#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>
#include "Token.hpp"

namespace loxis::v2 {

using ExprPtr = std::shared_ptr<struct Expr>;
using StmtPtr = std::shared_ptr<struct Stmt>;
using PatPtr  = std::shared_ptr<struct Pat>;
using TypePtr = std::shared_ptr<struct Type>;
using ItemPtr = std::shared_ptr<struct Item>;

// ============================================================
// Forward: Types
// ============================================================
struct Path {
    std::vector<std::string> segs;
    std::vector<TypePtr> args;
};

struct AstTyPath { Path path; };
struct AstTyTuple { std::vector<TypePtr> elems; };
struct AstTyArray { TypePtr elem; ExprPtr size; };
struct AstTySlice { TypePtr elem; };
struct AstTyRef  { bool mut = false; TypePtr elem; };
struct AstTyPtr { bool mut = false; TypePtr elem; };
struct AstTyFn   { std::vector<TypePtr> params; TypePtr ret; };
struct AstTyInfer {};
struct AstTyNever {};

struct Type : std::variant<AstTyPath, AstTyTuple, AstTyArray, AstTySlice, AstTyRef, AstTyPtr, AstTyFn, AstTyInfer, AstTyNever> {
    using variant::variant;
};

// ============================================================
// Forward: Patterns
// ============================================================
struct PatWild {};
struct PatBind { std::string name; bool mut = false; PatPtr sub; };
struct PatLit { ExprPtr expr; };
struct PatTuple { std::vector<PatPtr> elems; };
struct PatField { std::string name; PatPtr pat; };
struct PatStruct { Path path; std::vector<PatField> fields; bool rest = false; };
struct PatEnum { Path path; std::vector<PatPtr> elems; };
struct PatRef { bool mut = false; PatPtr pat; };
struct PatRest {};

struct Pat : std::variant<PatWild, PatBind, PatLit, PatTuple, PatStruct, PatEnum, PatRef, PatRest> {
    using variant::variant;
};

// ============================================================
// Forward: Expressions
// ============================================================
struct ExprLit {
    std::variant<int64_t,uint64_t,double,bool,char,std::string> val;
};
struct ExprPath { Path path; };
struct ExprTuple { std::vector<ExprPtr> elems; };
struct ExprArray { std::vector<ExprPtr> elems; };
struct ExprFieldInit { std::string name; ExprPtr expr; };
struct ExprStruct { Path path; std::vector<ExprFieldInit> fields; bool rest = false; };
struct ExprCall { ExprPtr func; std::vector<ExprPtr> args; };
struct ExprMethodCall { ExprPtr recv; std::string method; std::vector<ExprPtr> args; };
struct ExprField { ExprPtr obj; std::string field; };
struct ExprIndex { ExprPtr obj; ExprPtr idx; };
struct ExprUnary {
    enum Op { Neg, Not, Deref, Ref, RefMut } op;
    ExprPtr operand;
};
struct ExprBinary {
    enum Op { Add,Sub,Mul,Div,Rem,Shl,Shr,BitAnd,BitOr,BitXor,And,Or,Eq,Ne,Lt,Le,Gt,Ge } op;
    ExprPtr l, r;
};
struct ExprAssign {
    enum Op { Set,AddEq,SubEq,MulEq,DivEq,RemEq,AmpEq,PipeEq,CaretEq,ShlEq,ShrEq } op;
    ExprPtr lhs, rhs;
};
struct ExprBlock { std::vector<StmtPtr> stmts; ExprPtr tail; };
struct ExprIf { ExprPtr cond; ExprPtr then_; ExprPtr else_; };
struct ExprWhile { ExprPtr cond; ExprPtr body; std::optional<std::string> label; };
struct ExprFor { PatPtr pat; ExprPtr iter; ExprPtr body; std::optional<std::string> label; };
struct ExprLoop { ExprPtr body; std::optional<std::string> label; };
struct ExprMatch { ExprPtr scrut; std::vector<std::pair<PatPtr,ExprPtr>> arms; };
struct ExprBreak { std::optional<std::string> label; ExprPtr expr; };
struct ExprContinue { std::optional<std::string> label; };
struct ExprReturn { ExprPtr expr; };
struct ExprClosure { std::vector<std::pair<std::string,bool>> params; TypePtr ret; ExprPtr body; };
struct ExprRef { bool mut = false; ExprPtr expr; };
struct ExprDeref { ExprPtr expr; };
struct ExprTry { ExprPtr expr; };
struct ExprCast { ExprPtr expr; TypePtr ty; };

struct Expr : std::variant<
    ExprLit, ExprPath, ExprTuple, ExprArray, ExprStruct, ExprCall, ExprMethodCall,
    ExprField, ExprIndex, ExprUnary, ExprBinary, ExprAssign, ExprBlock, ExprIf,
    ExprWhile, ExprFor, ExprLoop, ExprMatch, ExprBreak, ExprContinue, ExprReturn,
    ExprClosure, ExprRef, ExprDeref, ExprTry, ExprCast
> {
    using variant::variant;
};

// ============================================================
// Forward: Statements
// ============================================================
struct StmtLet { PatPtr pat; TypePtr ty; ExprPtr init; };
struct StmtExpr { ExprPtr expr; bool semi; };
struct StmtItem { ItemPtr item; };

struct Stmt : std::variant<StmtLet, StmtExpr, StmtItem> {
    using variant::variant;
};

// ============================================================
// Forward: Items (top-level)
// ============================================================
struct GenericParam {
    enum Kind { Type, Const } kind;
    std::string name;
    TypePtr default_;
};

struct WhereBound {
    TypePtr ty;
    Path trait;
};

struct Field { std::string name; TypePtr ty; std::optional<ExprPtr> def; };
struct Variant { std::string name; std::vector<Field> fields; bool tuple = false; };
struct TraitMethod { std::string name; std::vector<std::pair<std::string,TypePtr>> params; TypePtr ret; ExprPtr def; };
struct UseTree {
    enum Kind { Simple, Nested, Glob } kind;
    Path path;
    std::vector<UseTree> nested;
    std::optional<std::string> rename;
};

struct ItemFn {
    std::string name; bool pub = false;
    std::vector<GenericParam> generics;
    std::vector<std::pair<std::string,TypePtr>> params;
    TypePtr ret;
    std::vector<WhereBound> where_;
    ExprPtr body;
    bool unsafe = false, ext = false;
};
struct ItemStruct { std::string name; bool pub = false; std::vector<GenericParam> generics; std::vector<Field> fields; std::vector<WhereBound> where_; };
struct ItemEnum { std::string name; bool pub = false; std::vector<GenericParam> generics; std::vector<Variant> vars; std::vector<WhereBound> where_; };
struct ItemTrait { std::string name; bool pub = false; std::vector<GenericParam> generics; std::vector<TraitMethod> methods; std::vector<Path> supers; };
struct ItemImpl { std::vector<GenericParam> generics; TypePtr ty; std::optional<Path> trait; std::vector<WhereBound> where_; std::vector<ItemFn> methods; };
struct ItemMod { std::string name; bool pub = false; std::vector<ItemPtr> items; bool inline_ = false; };
struct ItemUse { bool pub = false; UseTree tree; };
struct ItemStatic { std::string name; bool pub = false; TypePtr ty; ExprPtr init; bool mut = false; };
struct ItemConst { std::string name; bool pub = false; TypePtr ty; ExprPtr init; };

struct Item : std::variant<
    ItemFn, ItemStruct, ItemEnum, ItemTrait, ItemImpl, ItemMod, ItemUse, ItemStatic, ItemConst
> {
    using variant::variant;
};

// ============================================================
// Module (file root)
// ============================================================
struct Module {
    std::string name;
    std::vector<ItemPtr> items;
    SourceLoc loc;
};

} // namespace
