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
    I8, I16, I32, I64, Isize,
    U8, U16, U32, U64, Usize,
    F32, F64,
    Str, String,
    Fn, Tuple, Array, Slice,
    Ref, RawPtr,
    Adt,
    Trait,
    Param,
    Projection,
    Error,
};

struct InferVar { uint32_t id; };

struct AdtDef {
    std::string name;
    std::vector<std::string> params;
};

struct TyNever {};
struct TyUnit {};
struct TyBool {};
struct TyChar {};
struct TyInt { enum Width { W8,W16,W32,W64,WSize } w; };
struct TyUint{ enum Width { W8,W16,W32,W64,WSize } w; };
struct TyFloat{ enum Width { W32,W64 } w; };
struct TyStr {};
struct TyString {};
struct TyFn { std::vector<TyPtr> params; TyPtr ret; };
struct TyTuple { std::vector<TyPtr> elems; };
struct TyArray { TyPtr elem; uint64_t len; };
struct TySlice { TyPtr elem; };
struct TyRef { bool mut = false; TyPtr elem; };
struct TyRawPtr { bool mut = false; TyPtr elem; };
struct TyAdt { std::shared_ptr<AdtDef> def; std::vector<TyPtr> args; };
struct TyTrait { std::string name; std::vector<TyPtr> args; };
struct TyParam { std::string name; uint32_t idx; };
struct TyProjection { TyPtr base; std::string assoc; };
struct TyError {};

struct Ty : std::variant<
    InferVar,
    TyNever,
    TyUnit,
    TyBool, TyChar,
    TyInt, TyUint, TyFloat,
    TyStr, TyString,
    TyFn, TyTuple, TyArray, TySlice,
    TyRef, TyRawPtr,
    TyAdt, TyTrait, TyParam, TyProjection,
    TyError
> {
    using variant::variant;
    TyKind kind() const;
    std::string toString() const;
    bool isUnit() const { return kind()==TyKind::Unit; }
    bool isNever() const { return kind()==TyKind::Never; }
    bool isError() const { return kind()==TyKind::Error; }
};

inline TyKind Ty::kind() const {
    struct V {
        TyKind r;
        void operator()(const InferVar&) { r=TyKind::Infer; }
        void operator()(const TyNever&) { r=TyKind::Never; }
        void operator()(const TyUnit&) { r=TyKind::Unit; }
        void operator()(const TyBool&) { r=TyKind::Bool; }
        void operator()(const TyChar&) { r=TyKind::Char; }
        void operator()(const TyInt&) { r=TyKind::I8; }
        void operator()(const TyUint&){ r=TyKind::U8; }
        void operator()(const TyFloat&){ r=TyKind::F32; }
        void operator()(const TyStr&) { r=TyKind::Str; }
        void operator()(const TyString&){ r=TyKind::String; }
        void operator()(const TyFn&) { r=TyKind::Fn; }
        void operator()(const TyTuple&){ r=TyKind::Tuple; }
        void operator()(const TyArray&){ r=TyKind::Array; }
        void operator()(const TySlice&){ r=TyKind::Slice; }
        void operator()(const TyRef&) { r=TyKind::Ref; }
        void operator()(const TyRawPtr&){ r=TyKind::RawPtr; }
        void operator()(const TyAdt&) { r=TyKind::Adt; }
        void operator()(const TyTrait&){ r=TyKind::Trait; }
        void operator()(const TyParam&){ r=TyKind::Param; }
        void operator()(const TyProjection&){ r=TyKind::Projection; }
        void operator()(const TyError&){ r=TyKind::Error; }
    } v{TyKind::Error};
    std::visit(v, *this);
    if (std::holds_alternative<TyInt>(*this)) {
        switch(std::get<TyInt>(*this).w) { case TyInt::W8:return TyKind::I8; case TyInt::W16:return TyKind::I16; case TyInt::W32:return TyKind::I32; case TyInt::W64:return TyKind::I64; case TyInt::WSize:return TyKind::Isize; }
    }
    if (std::holds_alternative<TyUint>(*this)) {
        switch(std::get<TyUint>(*this).w) { case TyUint::W8:return TyKind::U8; case TyUint::W16:return TyKind::U16; case TyUint::W32:return TyKind::U32; case TyUint::W64:return TyKind::U64; case TyUint::WSize:return TyKind::Usize; }
    }
    if (std::holds_alternative<TyFloat>(*this)) {
        switch(std::get<TyFloat>(*this).w) { case TyFloat::W32:return TyKind::F32; case TyFloat::W64:return TyKind::F64; }
    }
    return v.r;
}

inline std::string Ty::toString() const {
    switch(kind()) {
    case TyKind::Infer: return "_";
    case TyKind::Never: return "!";
    case TyKind::Unit: return "()";
    case TyKind::Bool: return "bool";
    case TyKind::Char: return "char";
    case TyKind::I8: case TyKind::I16: case TyKind::I32: case TyKind::I64: case TyKind::Isize: {
        auto w=std::get<TyInt>(*this).w;
        return w==TyInt::W8?"i8":w==TyInt::W16?"i16":w==TyInt::W32?"i32":w==TyInt::W64?"i64":"isize";
    }
    case TyKind::U8: case TyKind::U16: case TyKind::U32: case TyKind::U64: case TyKind::Usize: {
        auto w=std::get<TyUint>(*this).w;
        return w==TyUint::W8?"u8":w==TyUint::W16?"u16":w==TyUint::W32?"u32":w==TyUint::W64?"u64":"usize";
    }
    case TyKind::F32: case TyKind::F64: return std::get<TyFloat>(*this).w==TyFloat::W32?"f32":"f64";
    case TyKind::Str: return "str";
    case TyKind::String: return "String";
    case TyKind::Fn: return "fn(..)";
    case TyKind::Tuple: { auto&t=std::get<TyTuple>(*this); std::string s="("; for(size_t i=0;i<t.elems.size();++i){s+=t.elems[i]->toString();if(i+1<t.elems.size())s+=",";} s += ")"; return s;}
    case TyKind::Array: { auto&t=std::get<TyArray>(*this); return "["+t.elem->toString()+"; "+std::to_string(t.len)+"]"; }
    case TyKind::Slice: return "["+std::get<TySlice>(*this).elem->toString()+"]";
    case TyKind::Ref: { auto&t=std::get<TyRef>(*this); return std::string("&")+(t.mut?"mut ":"")+t.elem->toString(); }
    case TyKind::RawPtr: { auto&t=std::get<TyRawPtr>(*this); return std::string("*")+(t.mut?"mut ":"const ")+t.elem->toString(); }
    case TyKind::Adt: return std::get<TyAdt>(*this).def->name;
    case TyKind::Trait: return std::get<TyTrait>(*this).name;
    case TyKind::Param: return std::get<TyParam>(*this).name;
    case TyKind::Projection: return "_";
    case TyKind::Error: return "<error>";
    }
    return "?";
}

inline TyPtr mkInfer(uint32_t id) { return std::make_shared<Ty>(InferVar{id}); }
inline TyPtr mkUnit() { return std::make_shared<Ty>(TyUnit{}); }
inline TyPtr mkBool() { return std::make_shared<Ty>(TyBool{}); }
inline TyPtr mkI32() { return std::make_shared<Ty>(TyInt{TyInt::W32}); }
inline TyPtr mkI64() { return std::make_shared<Ty>(TyInt{TyInt::W64}); }
inline TyPtr mkU32() { return std::make_shared<Ty>(TyUint{TyUint::W32}); }
inline TyPtr mkU64() { return std::make_shared<Ty>(TyUint{TyUint::W64}); }
inline TyPtr mkF32() { return std::make_shared<Ty>(TyFloat{TyFloat::W32}); }
inline TyPtr mkF64() { return std::make_shared<Ty>(TyFloat{TyFloat::W64}); }
inline TyPtr mkStr() { return std::make_shared<Ty>(TyStr{}); }
inline TyPtr mkString() { return std::make_shared<Ty>(TyString{}); }
inline TyPtr mkNever() { return std::make_shared<Ty>(TyNever{}); }
inline TyPtr mkError() { return std::make_shared<Ty>(TyError{}); }

} // namespace
