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

enum class Visibility {
    Private,
    Internal,
    Public,
};

enum class ClassModifier { None, Open, Abstract, Data };

// ============================================================
// Types
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
struct AstTyNullable { TypePtr inner; };    // T?

struct Type : std::variant<AstTyPath, AstTyTuple, AstTyArray, AstTySlice, AstTyRef, AstTyPtr, AstTyFn, AstTyInfer, AstTyNullable> {
    using variant::variant;
};

// ============================================================
// Patterns
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
// Expressions
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

// when expression (replaces match)
struct WhenArm {
    std::variant<ExprPtr, PatPtr, std::monostate> condition; // expr, pattern, or empty (else)
    ExprPtr body;
    bool isElse = false;
};
struct ExprWhen { ExprPtr scrut; std::vector<WhenArm> arms; };

struct ExprBreak { std::optional<std::string> label; ExprPtr expr; };
struct ExprContinue { std::optional<std::string> label; };
struct ExprReturn { ExprPtr expr; };
struct ExprClosure { std::vector<std::pair<std::string,bool>> params; TypePtr ret; ExprPtr body; };
struct ExprRef { bool mut = false; ExprPtr expr; };
struct ExprDeref { ExprPtr expr; };
struct ExprTry { ExprPtr expr; };
struct ExprCast { ExprPtr expr; TypePtr ty; };

// Nullable & type test expressions
struct ExprSafeCall { ExprPtr recv; std::string method; std::vector<ExprPtr> args; };
struct ExprSafeField { ExprPtr recv; std::string field; };
struct ExprElvis { ExprPtr lhs; ExprPtr rhs; };
struct ExprForceUnwrap { ExprPtr expr; };
struct ExprIs { ExprPtr expr; TypePtr ty; };
struct ExprIsNot { ExprPtr expr; TypePtr ty; };
struct ExprNull {};
struct ExprStringTemplate { std::vector<std::variant<std::string, ExprPtr>> parts; };

struct Expr : std::variant<
    ExprLit, ExprPath, ExprTuple, ExprArray, ExprStruct, ExprCall, ExprMethodCall,
    ExprField, ExprIndex, ExprUnary, ExprBinary, ExprAssign, ExprBlock, ExprIf,
    ExprWhile, ExprFor, ExprLoop, ExprWhen, ExprBreak, ExprContinue, ExprReturn,
    ExprClosure, ExprRef, ExprDeref, ExprTry, ExprCast,
    ExprSafeCall, ExprSafeField, ExprElvis, ExprForceUnwrap,
    ExprIs, ExprIsNot, ExprNull, ExprStringTemplate
> {
    using variant::variant;
};

// ============================================================
// Statements
// ============================================================
struct StmtLet { PatPtr pat; TypePtr ty; ExprPtr init; };
struct StmtExpr { ExprPtr expr; bool semi; };
struct StmtItem { ItemPtr item; };

struct Stmt : std::variant<StmtLet, StmtExpr, StmtItem> {
    using variant::variant;
};

// ============================================================
// Items (top-level)
// ============================================================
struct GenericParam {
    enum Kind { Type, Const } kind;
    std::string name;
    TypePtr default_;
};

struct WhereBound {
    TypePtr ty;
    Path path;  // type:path for bound (was "trait", now generic)
};

struct FieldDecl {
    std::string name;
    TypePtr ty;
    bool isVal = true;     // val (immutable) or var (mutable)
    ExprPtr initializer;
};

struct ParamDecl {
    std::string name;
    TypePtr ty;
    bool isVal = false;    // val → property
    bool isVar = false;    // var → mutable property
    ExprPtr default_;
};

// Import
struct ImportItem { std::string name; std::optional<std::string> alias; };
struct ItemImport { Path module; std::vector<ImportItem> items; bool importModule = true; };

// Function
struct ItemFun {
    std::string name;
    Visibility visibility = Visibility::Public;
    std::vector<GenericParam> generics;
    std::vector<std::pair<std::string,TypePtr>> params;
    TypePtr ret;
    std::vector<WhereBound> where_;
    ExprPtr body;
    bool isOpen = false;
    bool isOverride = false;
    bool isAbstract = false;
};

// Class
struct ItemClass {
    std::string name;
    Visibility visibility = Visibility::Public;
    ClassModifier modifier = ClassModifier::None;
    std::vector<GenericParam> generics;
    std::vector<ParamDecl> primaryCtor;
    std::optional<Path> superClass;
    std::vector<ExprPtr> superClassArgs;
    std::vector<Path> interfaces;
    std::vector<WhereBound> where_;
    // Body
    std::vector<FieldDecl> fields;
    std::vector<ItemFun> methods;
    std::vector<StmtPtr> initBlocks;
};

// Interface
struct InterfaceMethodDecl {
    std::string name;
    std::vector<std::pair<std::string,TypePtr>> params;
    TypePtr ret;
    ExprPtr defaultBody;
};

struct ItemInterface {
    std::string name;
    Visibility visibility = Visibility::Public;
    std::vector<GenericParam> generics;
    std::vector<Path> supers;
    std::vector<InterfaceMethodDecl> methods;
};

// Enum class
struct EnumVariant {
    std::string name;
    std::vector<std::pair<std::string,TypePtr>> fields;
    bool isTuple = false;
};

struct ItemEnumClass {
    std::string name;
    Visibility visibility = Visibility::Public;
    std::vector<GenericParam> generics;
    std::vector<Path> interfaces;
    std::vector<EnumVariant> variants;
    std::vector<FieldDecl> properties;
    std::vector<ItemFun> methods;
};

// Object (singleton)
struct ItemObject {
    std::string name;
    Visibility visibility = Visibility::Public;
    std::optional<Path> superClass;
    std::vector<Path> interfaces;
    std::vector<FieldDecl> fields;
    std::vector<ItemFun> methods;
    std::vector<StmtPtr> initBlocks;
};

// Top-level properties
struct ItemVal {
    std::string name;
    Visibility visibility = Visibility::Public;
    TypePtr ty;
    ExprPtr init;
};

struct ItemVar {
    std::string name;
    Visibility visibility = Visibility::Public;
    TypePtr ty;
    ExprPtr init;
};

struct ItemConst {
    std::string name;
    Visibility visibility = Visibility::Public;
    TypePtr ty;
    ExprPtr init;
};

struct Item : std::variant<
    ItemFun, ItemClass, ItemEnumClass, ItemInterface, ItemObject,
    ItemImport, ItemVal, ItemVar, ItemConst
> {
    using variant::variant;
};

// ============================================================
// Module (file root)
// ============================================================
struct Module {
    std::string name;
    std::optional<Path> packageName;
    std::vector<ItemPtr> items;
    SourceLoc loc;
};

} // namespace
