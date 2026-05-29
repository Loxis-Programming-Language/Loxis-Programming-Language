#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <optional>

namespace loxis::v2 {

struct Ty;
using TyPtr = std::shared_ptr<Ty>;
using CoreTyPtr = TyPtr;

enum class TyKind {
    Infer, Never, Unit, Bool, Char,
    Int, Long,
    Float, Double,
    Str,
    Fn, Tuple, Array, Slice,
    Ref, RawPtr,
    Adt,
    Class_,
    Interface_,
    Param,
    Projection,
    Error,
    Null,
};

struct InferVar { uint32_t id; };

struct AdtDef {
    std::string name;
    std::vector<std::string> params;
};

struct FieldInfo {
    std::string name;
    TyPtr type;
    uint32_t offset = 0;       // byte offset from payload start (populated by ScopeBuilder)
    bool isVal = true;
};

struct MethodInfo {
    std::string name;
    TyPtr funcType;             // (self, params...) -> ret
    uint32_t vtableIndex = ~0u; // ~0u = static/final dispatch
    bool isOpen = false;
    bool isAbstract = false;
    bool isOverride = false;
};

struct InterfaceDef {
    std::string name;
    std::vector<std::string> params;
    std::vector<MethodInfo> methods;     // declared methods
    std::vector<std::string> superNames; // super-interfaces
};

struct ClassDef {
    std::string name;
    std::vector<std::string> params;
    std::vector<FieldInfo> fields;     // own fields (offset from own payload start)
    std::vector<MethodInfo> methods;   // own methods (includes overrides)
    std::vector<uint32_t> vtable;      // indices into methods[] for virtual dispatch
    std::optional<std::string> superName;
    std::vector<std::string> interfaceNames;
    bool isOpen = false;
    bool isAbstract = false;
    bool isData = false;
    uint32_t totalFieldSize = 0;       // total size of own fields (fields * 9 bytes)
};

struct TyNever {};
struct TyUnit {};
struct TyBool {};
struct TyChar {};
struct TyInt { enum Width { W32, W64 } w; };      // int, long
struct TyFloat{ enum Width { W32, W64 } w; };      // float, double
struct TyStr {};
struct TyNull {};
struct TyFn { std::vector<TyPtr> params; TyPtr ret; };
struct TyTuple { std::vector<TyPtr> elems; };
struct TyArray { TyPtr elem; uint64_t len; };
struct TySlice { TyPtr elem; };
struct TyRef { bool mut = false; TyPtr elem; };
struct TyRawPtr { bool mut = false; TyPtr elem; };
struct TyAdt { std::shared_ptr<AdtDef> def; std::vector<TyPtr> args; };
struct TyClass { std::shared_ptr<ClassDef> def; std::vector<TyPtr> args; };
struct TyInterface { std::shared_ptr<InterfaceDef> def; std::vector<TyPtr> args; };
struct TyParam { std::string name; uint32_t idx; };
struct TyProjection { TyPtr base; std::string assoc; };
struct TyError {};

struct Ty : std::variant<
    InferVar,
    TyNever,
    TyUnit,
    TyBool, TyChar,
    TyInt, TyFloat,
    TyStr, TyNull,
    TyFn, TyTuple, TyArray, TySlice,
    TyRef, TyRawPtr,
    TyAdt, TyClass, TyInterface, TyParam, TyProjection,
    TyError
> {
    using variant::variant;
    bool nullable = false;
    TyKind kind() const;
    std::string toString() const;
    bool isUnit() const { return kind()==TyKind::Unit; }
    bool isNever() const { return kind()==TyKind::Never; }
    bool isError() const { return kind()==TyKind::Error; }
    bool isNull() const { return kind()==TyKind::Null; }
};

inline TyKind Ty::kind() const {
    struct V {
        TyKind r;
        void operator()(const InferVar&) { r=TyKind::Infer; }
        void operator()(const TyNever&) { r=TyKind::Never; }
        void operator()(const TyUnit&) { r=TyKind::Unit; }
        void operator()(const TyBool&) { r=TyKind::Bool; }
        void operator()(const TyChar&) { r=TyKind::Char; }
        void operator()(const TyInt&) { r=TyKind::Int; }
        void operator()(const TyFloat&){ r=TyKind::Float; }
        void operator()(const TyStr&) { r=TyKind::Str; }
        void operator()(const TyNull&){ r=TyKind::Null; }
        void operator()(const TyFn&) { r=TyKind::Fn; }
        void operator()(const TyTuple&){ r=TyKind::Tuple; }
        void operator()(const TyArray&){ r=TyKind::Array; }
        void operator()(const TySlice&){ r=TyKind::Slice; }
        void operator()(const TyRef&) { r=TyKind::Ref; }
        void operator()(const TyRawPtr&){ r=TyKind::RawPtr; }
        void operator()(const TyAdt&) { r=TyKind::Adt; }
        void operator()(const TyClass&){ r=TyKind::Class_; }
        void operator()(const TyInterface&){ r=TyKind::Interface_; }
        void operator()(const TyParam&){ r=TyKind::Param; }
        void operator()(const TyProjection&){ r=TyKind::Projection; }
        void operator()(const TyError&){ r=TyKind::Error; }
    } v{TyKind::Error};
    std::visit(v, *this);
    if (std::holds_alternative<TyInt>(*this)) {
        return std::get<TyInt>(*this).w == TyInt::W64 ? TyKind::Long : TyKind::Int;
    }
    if (std::holds_alternative<TyFloat>(*this)) {
        return std::get<TyFloat>(*this).w == TyFloat::W64 ? TyKind::Double : TyKind::Float;
    }
    return v.r;
}

inline std::string Ty::toString() const {
    std::string suffix = nullable ? "?" : "";
    switch(kind()) {
    case TyKind::Infer: return "_" + suffix;
    case TyKind::Never: return suffix.empty() ? "noreturn" : "null";
    case TyKind::Unit: return "()" + suffix;
    case TyKind::Bool: return "bool" + suffix;
    case TyKind::Char: return "char" + suffix;
    case TyKind::Int: return "int" + suffix;
    case TyKind::Long: return "long" + suffix;
    case TyKind::Float: return "float" + suffix;
    case TyKind::Double: return "double" + suffix;
    case TyKind::Str: return "str" + suffix;
    case TyKind::Null: return "null";
    case TyKind::Fn: return "fun(..)" + suffix;
    case TyKind::Tuple: { auto&t=std::get<TyTuple>(*this); std::string s="("; for(size_t i=0;i<t.elems.size();++i){s+=t.elems[i]->toString();if(i+1<t.elems.size())s+=",";} s+=")"+suffix; return s;}
    case TyKind::Array: { auto&t=std::get<TyArray>(*this); return "["+t.elem->toString()+";"+std::to_string(t.len)+"]"+suffix; }
    case TyKind::Slice: return "["+std::get<TySlice>(*this).elem->toString()+"]"+suffix;
    case TyKind::Ref: { auto&t=std::get<TyRef>(*this); return std::string("&")+(t.mut?"mut ":"")+t.elem->toString(); }
    case TyKind::RawPtr: { auto&t=std::get<TyRawPtr>(*this); return std::string("*")+(t.mut?"mut ":"const ")+t.elem->toString(); }
    case TyKind::Adt: return std::get<TyAdt>(*this).def->name + suffix;
    case TyKind::Class_: return std::get<TyClass>(*this).def->name + suffix;
    case TyKind::Interface_: return std::get<TyInterface>(*this).def->name + suffix;
    case TyKind::Param: return std::get<TyParam>(*this).name + suffix;
    case TyKind::Projection: return "_" + suffix;
    case TyKind::Error: return "<error>";
    }
    return "?" + suffix;
}

// Factory functions
inline TyPtr mkInfer(uint32_t id) { return std::make_shared<Ty>(InferVar{id}); }
inline TyPtr mkUnit() { return std::make_shared<Ty>(TyUnit{}); }
inline TyPtr mkBool() { return std::make_shared<Ty>(TyBool{}); }
inline TyPtr mkChar() { return std::make_shared<Ty>(TyChar{}); }
inline TyPtr mkInt() { return std::make_shared<Ty>(TyInt{TyInt::W32}); }
inline TyPtr mkLong() { return std::make_shared<Ty>(TyInt{TyInt::W64}); }
inline TyPtr mkFloat() { return std::make_shared<Ty>(TyFloat{TyFloat::W32}); }
inline TyPtr mkDouble() { return std::make_shared<Ty>(TyFloat{TyFloat::W64}); }
inline TyPtr mkStr() { return std::make_shared<Ty>(TyStr{}); }
inline TyPtr mkNull() { return std::make_shared<Ty>(TyNull{}); }
inline TyPtr mkNever() { return std::make_shared<Ty>(TyNever{}); }
inline TyPtr mkError() { return std::make_shared<Ty>(TyError{}); }

inline TyPtr makeNullable(TyPtr inner) {
    auto copy = *inner;
    copy.nullable = true;
    return std::make_shared<Ty>(std::move(copy));
}

} // namespace
