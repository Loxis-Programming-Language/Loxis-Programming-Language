#pragma once
#include "Type.hpp"
#include <unordered_map>
#include <vector>

namespace loxis::v2 {

inline std::unordered_map<std::string, TyPtr> makeSubstMap(
    const std::vector<std::string>& paramNames,
    const std::vector<TyPtr>& args)
{
    std::unordered_map<std::string, TyPtr> map;
    for (size_t i = 0; i < paramNames.size() && i < args.size(); ++i) {
        map[paramNames[i]] = args[i];
    }
    return map;
}

inline TyPtr substituteType(TyPtr ty, const std::unordered_map<std::string, TyPtr>& subst) {
    if (!ty) return ty;

    TyPtr result = std::visit([&](const auto& t) -> TyPtr {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, TyParam>) {
            auto it = subst.find(t.name);
            if (it != subst.end()) {
                if (!ty->nullable) return it->second;
                auto copy = *it->second;
                copy.nullable = true;
                return std::make_shared<Ty>(std::move(copy));
            }
            return ty;
        } else if constexpr (std::is_same_v<T, TyFn>) {
            std::vector<TyPtr> params;
            for (const auto& p : t.params) params.push_back(substituteType(p, subst));
            return std::make_shared<Ty>(TyFn{std::move(params), substituteType(t.ret, subst)});
        } else if constexpr (std::is_same_v<T, TyTuple>) {
            std::vector<TyPtr> elems;
            for (const auto& e : t.elems) elems.push_back(substituteType(e, subst));
            return std::make_shared<Ty>(TyTuple{std::move(elems)});
        } else if constexpr (std::is_same_v<T, TyArray>) {
            return std::make_shared<Ty>(TyArray{substituteType(t.elem, subst), t.len});
        } else if constexpr (std::is_same_v<T, TySlice>) {
            return std::make_shared<Ty>(TySlice{substituteType(t.elem, subst)});
        } else if constexpr (std::is_same_v<T, TyRef>) {
            return std::make_shared<Ty>(TyRef{t.mut, substituteType(t.elem, subst)});
        } else if constexpr (std::is_same_v<T, TyRawPtr>) {
            return std::make_shared<Ty>(TyRawPtr{t.mut, substituteType(t.elem, subst)});
        } else if constexpr (std::is_same_v<T, TyAdt>) {
            std::vector<TyPtr> args;
            for (const auto& a : t.args) args.push_back(substituteType(a, subst));
            return std::make_shared<Ty>(TyAdt{t.def, std::move(args)});
        } else if constexpr (std::is_same_v<T, TyClass>) {
            std::vector<TyPtr> args;
            for (const auto& a : t.args) args.push_back(substituteType(a, subst));
            return std::make_shared<Ty>(TyClass{t.def, std::move(args)});
        } else if constexpr (std::is_same_v<T, TyInterface>) {
            std::vector<TyPtr> args;
            for (const auto& a : t.args) args.push_back(substituteType(a, subst));
            return std::make_shared<Ty>(TyInterface{t.def, std::move(args)});
        } else {
            auto copy = *ty;
            return std::make_shared<Ty>(std::move(copy));
        }
    }, *ty);
    if (ty->nullable && result && !result->nullable) {
        result = std::make_shared<Ty>(*result);
        result->nullable = true;
    }
    return result;
}

inline TyPtr applyClassArgs(TyPtr base, const std::vector<TyPtr>& args) {
    if (!base || args.empty()) return base;
    if (base->kind() == TyKind::Class_) {
        auto& cls = std::get<TyClass>(*base);
        return std::make_shared<Ty>(TyClass{cls.def, args});
    }
    if (base->kind() == TyKind::Interface_) {
        auto& iface = std::get<TyInterface>(*base);
        return std::make_shared<Ty>(TyInterface{iface.def, args});
    }
    if (base->kind() == TyKind::Adt) {
        auto& adt = std::get<TyAdt>(*base);
        return std::make_shared<Ty>(TyAdt{adt.def, args});
    }
    return base;
}

inline std::unordered_map<std::string, TyPtr> classSubstMap(TyPtr classTy) {
    if (!classTy || classTy->kind() != TyKind::Class_) return {};
    auto& cls = std::get<TyClass>(*classTy);
    if (cls.args.empty()) return {};
    return makeSubstMap(cls.def->params, cls.args);
}

inline std::unordered_map<std::string, TyPtr> typeArgSubstMap(TyPtr ty) {
    if (!ty) return {};
    if (ty->kind() == TyKind::Class_) {
        auto& cls = std::get<TyClass>(*ty);
        if (cls.args.empty()) return {};
        return makeSubstMap(cls.def->params, cls.args);
    }
    if (ty->kind() == TyKind::Interface_) {
        auto& iface = std::get<TyInterface>(*ty);
        if (iface.args.empty()) return {};
        return makeSubstMap(iface.def->params, iface.args);
    }
    return {};
}

} // namespace
