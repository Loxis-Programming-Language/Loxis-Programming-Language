#include "Scope.hpp"
#include <cstdio>
#include <functional>

namespace loxis::v2 {

// ============================================================
// Scope
// ============================================================
Scope::Scope(Scope* parent) : parent_(parent) {}

void Scope::declare(const std::string& name, SymbolKind kind, void* node, TyPtr ty) {
    auto it = symbols_.find(name);
    if (it != symbols_.end() && it->second.kind == SymbolKind::Value && kind == SymbolKind::Value &&
        it->second.node != nullptr && node != nullptr) {
        // Both old and new are function-like declarations: push as overload
        if (it->second.overloads.empty()) {
            it->second.overloads.push_back({it->second.node, it->second.ty});
        }
        it->second.overloads.push_back({node, ty});
        // Update primary to the latest
        it->second.node = node;
        it->second.ty = ty;
        return;
    }
    symbols_[name] = Symbol{name, kind, node, std::move(ty)};
}

Symbol* Scope::lookup(const std::string& name) {
    auto it = symbols_.find(name);
    if (it != symbols_.end()) return &it->second;
    if (parent_) return parent_->lookup(name);
    return nullptr;
}

Symbol* Scope::lookupLocal(const std::string& name) {
    auto it = symbols_.find(name);
    return it != symbols_.end() ? &it->second : nullptr;
}

Scope* Scope::parent() const { return parent_; }
bool Scope::isRoot() const { return parent_ == nullptr; }

Scope* Scope::childMod(const std::string& name) const {
    auto it = children_.find(name);
    return it != children_.end() ? it->second.get() : nullptr;
}

void Scope::addChildMod(const std::string& name, std::unique_ptr<Scope> child) {
    children_[name] = std::move(child);
}

void Scope::dump(int indent) const {
    std::string pad(indent * 2, ' ');
    for (const auto& [name, sym] : symbols_) {
        const char* kind = sym.kind == SymbolKind::Type ? "type" :
                           sym.kind == SymbolKind::Interface ? "iface" : "val";
        std::string tyStr = sym.ty ? sym.ty->toString() : "?";
        printf("%s  %s %s : %s\n", pad.c_str(), kind, name.c_str(), tyStr.c_str());
    }
    for (const auto& [name, child] : children_) {
        printf("%s  [class/iface %s", pad.c_str(), name.c_str());
        if (child->selfTy()) printf(" self=%s", child->selfTy()->toString().c_str());
        if (auto cd = child->classDef()) {
            printf(" fields=%zu methods=%zu vtable=%zu size=%u",
                cd->fields.size(), cd->methods.size(), cd->vtable.size(), cd->totalFieldSize);
        }
        printf("]\n");
        child->dump(indent + 1);
    }
}

// ============================================================
// ScopeBuilder — Pass 1: declare names
// ============================================================
void ScopeBuilder::declareItem(ItemPtr item, Scope* scope) {
    std::visit([&](auto&& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ItemFun>) {
            scope->declare(n.name, SymbolKind::Value,
                const_cast<void*>(static_cast<const void*>(&n)), nullptr);
        } else if constexpr (std::is_same_v<T, ItemClass>) {
            auto def = std::make_shared<ClassDef>();
            def->name = n.name;
            def->typeId = m_nextClassTypeId++;
            for (const auto& g : n.generics) def->params.push_back(g.name);
            def->isOpen = (n.modifier == ClassModifier::Open);
            def->isAbstract = (n.modifier == ClassModifier::Abstract);
            def->isData = (n.modifier == ClassModifier::Data);
            if (n.superClass) def->superName = n.superClass->segs.back();
            for (const auto& iface : n.interfaces)
                def->interfaceNames.push_back(iface.segs.back());
            TyPtr ty = std::make_shared<Ty>(TyClass{def, {}});
            scope->declare(n.name, SymbolKind::Type,
                const_cast<void*>(static_cast<const void*>(&n)), ty);
        } else if constexpr (std::is_same_v<T, ItemEnumClass>) {
            auto def = std::make_shared<ClassDef>();
            def->name = n.name;
            def->typeId = m_nextClassTypeId++;
            def->isEnum = true;
            for (const auto& g : n.generics) def->params.push_back(g.name);
            TyPtr ty = std::make_shared<Ty>(TyClass{def, {}});
            scope->declare(n.name, SymbolKind::Type,
                const_cast<void*>(static_cast<const void*>(&n)), ty);
            for (const auto& v : n.variants) {
                scope->declare(v.name, SymbolKind::Value,
                    const_cast<void*>(static_cast<const void*>(&n)), ty);
            }
        } else if constexpr (std::is_same_v<T, ItemInterface>) {
            auto def = std::make_shared<InterfaceDef>();
            def->name = n.name;
            def->typeId = m_nextInterfaceTypeId++;
            for (const auto& g : n.generics) def->params.push_back(g.name);
            for (const auto& s : n.supers) def->superNames.push_back(s.segs.back());
            TyPtr ty = std::make_shared<Ty>(TyInterface{def, {}});
            scope->declare(n.name, SymbolKind::Interface,
                const_cast<void*>(static_cast<const void*>(&n)), ty);
        } else if constexpr (std::is_same_v<T, ItemObject>) {
            auto def = std::make_shared<ClassDef>();
            def->name = n.name;
            def->typeId = m_nextClassTypeId++;
            if (n.superClass) def->superName = n.superClass->segs.back();
            for (const auto& iface : n.interfaces)
                def->interfaceNames.push_back(iface.segs.back());
            TyPtr ty = std::make_shared<Ty>(TyClass{def, {}});
            scope->declare(n.name, SymbolKind::Value,
                const_cast<void*>(static_cast<const void*>(&n)), ty);
            if (auto* objectSymbol = scope->lookupLocal(n.name)) {
                objectSymbol->globalSlot = m_nextGlobalSlot++;
            }
        } else if constexpr (std::is_same_v<T, ItemVal>) {
            scope->declare(n.name, SymbolKind::Value,
                const_cast<void*>(static_cast<const void*>(&n)), nullptr);
            if (auto* sym = scope->lookupLocal(n.name)) sym->isTopLevelProperty = true;
        } else if constexpr (std::is_same_v<T, ItemVar>) {
            scope->declare(n.name, SymbolKind::Value,
                const_cast<void*>(static_cast<const void*>(&n)), nullptr);
            if (auto* sym = scope->lookupLocal(n.name)) sym->isTopLevelProperty = true;
        } else if constexpr (std::is_same_v<T, ItemConst>) {
            scope->declare(n.name, SymbolKind::Value,
                const_cast<void*>(static_cast<const void*>(&n)), nullptr);
            if (auto* sym = scope->lookupLocal(n.name)) sym->isTopLevelProperty = true;
        } else if constexpr (std::is_same_v<T, ItemTypeAlias>) {
            scope->declare(n.name, SymbolKind::Type,
                const_cast<void*>(static_cast<const void*>(&n)), nullptr);
        }
        // ItemImport handled by ModuleLoader
    }, *item);
}

// ============================================================
// ScopeBuilder — Pass 2: resolve internals
// ============================================================
void ScopeBuilder::resolveItem(ItemPtr item, Scope* scope) {
    std::visit([&](auto&& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ItemClass>) {
            resolveClass(const_cast<ItemClass&>(n), scope);
        } else if constexpr (std::is_same_v<T, ItemInterface>) {
            resolveInterface(const_cast<ItemInterface&>(n), scope);
        } else if constexpr (std::is_same_v<T, ItemObject>) {
            resolveObject(const_cast<ItemObject&>(n), scope);
        } else if constexpr (std::is_same_v<T, ItemEnumClass>) {
            resolveEnumClass(const_cast<ItemEnumClass&>(n), scope);
        }
    }, *item);
}

// ============================================================
// Class resolution
// ============================================================
void ScopeBuilder::resolveClass(ItemClass& cls, Scope* scope) {
    // Find the ClassDef that was registered in Pass 1
    Symbol* sym = scope->lookup(cls.name);
    if (!sym || !sym->ty || sym->ty->kind() != TyKind::Class_) return;
    auto& classTy = std::get<TyClass>(*sym->ty);
    auto def = classTy.def;

    // Create child scope for the class body
    auto classScope = std::make_unique<Scope>(scope);
    Scope* cs = classScope.get();

    // Set self type for 'this' binding
    TyPtr selfTy = sym->ty;
    cs->setSelfTy(selfTy);
    cs->setClassDef(def);
    cs->declare("this", SymbolKind::Value, nullptr, selfTy);
    scope->addChildMod(cls.name, std::move(classScope));

    // Register type parameters
    for (size_t i = 0; i < cls.generics.size(); ++i) {
        TyPtr paramTy = std::make_shared<Ty>(TyParam{cls.generics[i].name, static_cast<uint32_t>(i)});
        cs->declare(cls.generics[i].name, SymbolKind::Type, nullptr, paramTy);
    }

    // Inherit from superclass (populates def with inherited fields/methods)
    inheritFromSuper(*def, scope);

    // Starting field offset = super's totalFieldSize
    uint32_t fieldOffset = def->totalFieldSize;

    // Register primary constructor parameters (val/var become fields)
    for (const auto& pd : cls.primaryCtor) {
        if (pd.isVal || pd.isVar) {
            FieldInfo fi;
            fi.name = pd.name;
            fi.type = nullptr; // resolved during type checking
            fi.offset = fieldOffset;
            fi.isVal = pd.isVal;
            def->fields.push_back(fi);
            cs->declare(pd.name, SymbolKind::Value, nullptr, nullptr);
            fieldOffset += 9; // 9 bytes per field slot
        }
    }

    // Register body fields
    for (const auto& fd : cls.fields) {
        FieldInfo fi;
        fi.name = fd.name;
        fi.type = nullptr;
        fi.offset = fieldOffset;
        fi.isVal = fd.isVal;
        def->fields.push_back(fi);
        cs->declare(fd.name, SymbolKind::Value, nullptr, nullptr);
        fieldOffset += 9;
    }

    def->totalFieldSize = fieldOffset;

    // Register methods in class scope
    for (const auto& m : cls.methods) {
        MethodInfo mi;
        mi.name = m.name;
        mi.funcType = nullptr; // resolved during type checking
        mi.arity = static_cast<uint32_t>(m.params.size());
        mi.isOpen = m.isOpen;
        mi.isAbstract = m.isAbstract;
        mi.isOverride = m.isOverride;

        // Check for override: if method exists in super, reuse vtable index
        if (mi.isOverride) {
            for (auto& existing : def->methods) {
                if (existing.name == mi.name && existing.arity == mi.arity) {
                    mi.vtableIndex = existing.vtableIndex;
                    // Replace inherited method with overridden one
                    existing = mi;
                    break;
                }
            }
        }
        if (mi.vtableIndex == ~0u) {
            def->methods.push_back(mi);
        }

        cs->declare(m.name, SymbolKind::Value, nullptr, nullptr);
    }

    // Build vtable
    buildVTable(*def);

    // Check interface conformance
    if (!def->interfaceNames.empty()) {
        if (!checkInterfaceConformance(*def, scope)) {
            // conformance failure — errors reported inside checkInterfaceConformance
            // TypeChecker will catch missing methods during type checking
        }
    }
}

// ============================================================
// Inherit fields and methods from superclass
// ============================================================
void ScopeBuilder::inheritFromSuper(ClassDef& def, Scope* scope) {
    if (!def.superName) return;

    Symbol* superSym = scope->lookup(*def.superName);
    if (!superSym || !superSym->ty || superSym->ty->kind() != TyKind::Class_) return;

    auto& superTy = std::get<TyClass>(*superSym->ty);
    auto superDef = superTy.def;

    // Subclass fields start after super's total field size
    uint32_t baseOffset = superDef->totalFieldSize;

    // Prepend super's fields (already have correct offsets relative to object start)
    for (auto it = superDef->fields.rbegin(); it != superDef->fields.rend(); ++it) {
        def.fields.insert(def.fields.begin(), *it);
    }

    // Prepend non-overridden super methods
    for (auto it = superDef->methods.rbegin(); it != superDef->methods.rend(); ++it) {
        bool overridden = false;
        for (const auto& om : def.methods) {
            if (om.name == it->name && om.isOverride) { overridden = true; break; }
        }
        if (!overridden) {
            def.methods.insert(def.methods.begin(), *it);
        }
    }

    // Inherit vtable indices from super (prepend)
    for (auto it = superDef->vtable.rbegin(); it != superDef->vtable.rend(); ++it) {
        def.vtable.insert(def.vtable.begin(), *it);
    }

    // Inherit interfaces
    for (const auto& iname : superDef->interfaceNames) {
        bool already = false;
        for (const auto& i : def.interfaceNames) {
            if (i == iname) { already = true; break; }
        }
        if (!already) def.interfaceNames.push_back(iname);
    }

    // Subclass's own fields start after all inherited fields from super chain
    def.totalFieldSize = baseOffset;
}

// ============================================================
// Build vtable
// ============================================================
void ScopeBuilder::buildVTable(ClassDef& def) {
    // Virtual methods are those marked open, abstract, or override
    // plus data class methods (equals, hashCode, toString, copy, componentN)
    for (size_t i = 0; i < def.methods.size(); ++i) {
        auto& m = def.methods[i];
        if (m.isOpen || m.isAbstract || m.isOverride) {
            if (m.vtableIndex == ~0u) {
                m.vtableIndex = static_cast<uint32_t>(def.vtable.size());
                def.vtable.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    // Data class: auto-generate equals, hashCode, toString, copy, componentN
    if (def.isData) {
        // These would be added as virtual methods in a full implementation
        // For now, just mark that the vtable needs slots for them
    }
}

// ============================================================
// Interface conformance check
// ============================================================
bool ScopeBuilder::checkInterfaceConformance(ClassDef& def, Scope* scope) {
    for (const auto& iname : def.interfaceNames) {
        Symbol* ifaceSym = scope->lookup(iname);
        if (!ifaceSym || !ifaceSym->ty || ifaceSym->ty->kind() != TyKind::Interface_) {
            continue; // error will be reported by type checker
        }
        auto& ifaceTy = std::get<TyInterface>(*ifaceSym->ty);
        auto ifaceDef = ifaceTy.def;

        ItableEntry itable;
        itable.interfaceName = iname;
        itable.interfaceTypeId = ifaceDef->typeId;
        itable.slotMap.resize(ifaceDef->methods.size(), ~0u);

        for (size_t imIdx = 0; imIdx < ifaceDef->methods.size(); ++imIdx) {
            const auto& im = ifaceDef->methods[imIdx];
            bool found = false;

            for (size_t cmIdx = 0; cmIdx < def.methods.size(); ++cmIdx) {
                const auto& cm = def.methods[cmIdx];
                if (cm.name != im.name) continue;

                // Signature check: compare parameter counts (MVP)
                // Full structural comparison would check individual param types
                if (im.funcType && cm.funcType &&
                    im.funcType->kind() == TyKind::Fn && cm.funcType->kind() == TyKind::Fn) {
                    auto& imFn = std::get<TyFn>(*im.funcType);
                    auto& cmFn = std::get<TyFn>(*cm.funcType);
                    if (imFn.params.size() != cmFn.params.size()) {
                        continue; // arity mismatch — keep looking
                    }
                    // TODO: full structural type comparison for generics
                }

                // Map interface method index → class vtable slot (or method index for static)
                itable.slotMap[imIdx] = cm.vtableIndex != ~0u
                    ? cm.vtableIndex
                    : static_cast<uint32_t>(cmIdx);
                found = true;
                break;
            }

            if (!found) {
                if (!im.isAbstract) {
                    // Interface provides a default — inject into class vtable
                    MethodInfo defaultMi;
                    defaultMi.name = im.name;
                    defaultMi.funcType = im.funcType;
                    defaultMi.isInterfaceDefault = true;
                    defaultMi.defaultIfaceName = iname;
                    defaultMi.isOpen = true;
                    defaultMi.vtableIndex = static_cast<uint32_t>(def.vtable.size());
                    def.vtable.push_back(static_cast<uint32_t>(def.methods.size()));
                    def.methods.push_back(std::move(defaultMi));
                    itable.slotMap[imIdx] = defaultMi.vtableIndex;
                    found = true;
                } else {
                    return false;
                }
            }
        }

        def.itables.push_back(std::move(itable));
    }
    return true;
}

// ============================================================
// Interface resolution
// ============================================================
void ScopeBuilder::resolveInterface(ItemInterface& iface, Scope* scope) {
    Symbol* sym = scope->lookup(iface.name);
    if (!sym || !sym->ty || sym->ty->kind() != TyKind::Interface_) return;
    auto& ifaceTy = std::get<TyInterface>(*sym->ty);
    auto def = ifaceTy.def;

    // Create child scope
    auto ifaceScope = std::make_unique<Scope>(scope);
    Scope* is = ifaceScope.get();
    scope->addChildMod(iface.name, std::move(ifaceScope));

    // Register type parameters
    for (size_t i = 0; i < iface.generics.size(); ++i) {
        TyPtr paramTy = std::make_shared<Ty>(TyParam{iface.generics[i].name, static_cast<uint32_t>(i)});
        is->declare(iface.generics[i].name, SymbolKind::Type, nullptr, paramTy);
    }

    // Inherit super-interface methods
    for (const auto& superName : iface.supers) {
        Symbol* superSym = scope->lookup(superName.segs.back());
        if (superSym && superSym->ty && superSym->ty->kind() == TyKind::Interface_) {
            auto& superIface = std::get<TyInterface>(*superSym->ty);
            for (const auto& sm : superIface.def->methods) {
                def->methods.push_back(sm);
            }
            for (const auto& sn : superIface.def->superNames) {
                def->superNames.push_back(sn);
            }
        }
    }

    // Helper to resolve a simple AST type to a core TyPtr using scope lookup.
    // Uses std::function for recursive calls (nullable types, tuple types).
    std::function<TyPtr(const TypePtr&)> resolveSimpleType;
    resolveSimpleType = [&](const TypePtr& astTy) -> TyPtr {
        if (!astTy) return mkUnit();
        return std::visit([&](auto&& t) -> TyPtr {
            using TT = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<TT, AstTyPath>) {
                std::string name = t.path.segs.back();
                Symbol* sym = scope->lookup(name);
                if (sym && sym->ty) return sym->ty;
                // Check interface child scope
                Symbol* local = is->lookupLocal(name);
                if (local && local->ty) return local->ty;
                return nullptr;
            } else if constexpr (std::is_same_v<TT, AstTyNullable>) {
                TyPtr inner = resolveSimpleType(t.inner);
                return inner ? makeNullable(inner) : nullptr;
            } else if constexpr (std::is_same_v<TT, AstTyTuple>) {
                std::vector<TyPtr> elems;
                for (const auto& e : t.elems) {
                    TyPtr et = resolveSimpleType(e);
                    if (et) elems.push_back(et);
                }
                return std::make_shared<Ty>(TyTuple{elems});
            } else {
                return nullptr;
            }
        }, *astTy);
    };

    // Build TyFn for each interface method from its AST declaration
    TyPtr selfTy = sym->ty;
    for (const auto& m : iface.methods) {
        MethodInfo mi;
        mi.name = m.name;
        mi.arity = static_cast<uint32_t>(m.params.size());
        mi.isAbstract = !m.defaultBody; // abstract if no default body

        // Build funcType: (self: Interface, params...) -> ret
        std::vector<TyPtr> paramTypes;
        paramTypes.push_back(selfTy ? selfTy : mkUnit()); // self param
        for (const auto& p : m.params) {
            TyPtr pt = resolveSimpleType(p.second);
            if (!pt) pt = mkUnit(); // fallback: unresolved → unit (checked later)
            paramTypes.push_back(pt);
        }
        TyPtr retType = m.ret ? resolveSimpleType(m.ret) : mkUnit();
        if (!retType) retType = mkUnit();

        mi.funcType = std::make_shared<Ty>(TyFn{std::move(paramTypes), retType});
        def->methods.push_back(mi);
        is->declare(m.name, SymbolKind::Value, nullptr, mi.funcType);
    }
}

// ============================================================
// Object (singleton) resolution
// ============================================================
void ScopeBuilder::resolveObject(ItemObject& obj, Scope* scope) {
    Symbol* sym = scope->lookup(obj.name);
    if (!sym || !sym->ty || sym->ty->kind() != TyKind::Class_) return;
    auto& classTy = std::get<TyClass>(*sym->ty);
    auto def = classTy.def;
    obj.globalSlot = sym->globalSlot;

    auto objScope = std::make_unique<Scope>(scope);
    Scope* os = objScope.get();
    os->setSelfTy(sym->ty);
    os->setClassDef(def);
    os->declare("this", SymbolKind::Value, nullptr, sym->ty);
    scope->addChildMod(obj.name, std::move(objScope));

    // Inherit from superclass if any
    if (obj.superClass) {
        def->superName = obj.superClass->segs.back();
        inheritFromSuper(*def, scope);
    }

    uint32_t fieldOffset = def->totalFieldSize;

    for (const auto& fd : obj.fields) {
        FieldInfo fi;
        fi.name = fd.name;
        fi.type = nullptr;
        fi.offset = fieldOffset;
        fi.isVal = fd.isVal;
        def->fields.push_back(fi);
        os->declare(fd.name, SymbolKind::Value, nullptr, nullptr);
        fieldOffset += 9;
    }
    def->totalFieldSize = fieldOffset;

    for (const auto& m : obj.methods) {
        MethodInfo mi;
        mi.name = m.name;
        mi.funcType = nullptr;
        mi.arity = static_cast<uint32_t>(m.params.size());
        mi.isOpen = m.isOpen;
        mi.isAbstract = m.isAbstract;
        mi.isOverride = m.isOverride;
        def->methods.push_back(mi);
        os->declare(m.name, SymbolKind::Value, nullptr, nullptr);
    }

    buildVTable(*def);
    if (!def->interfaceNames.empty()) checkInterfaceConformance(*def, scope);
}

// ============================================================
// Enum class resolution
// ============================================================
void ScopeBuilder::resolveEnumClass(ItemEnumClass& ec, Scope* scope) {
    Symbol* sym = scope->lookup(ec.name);
    if (!sym || !sym->ty || sym->ty->kind() != TyKind::Class_) return;
    auto& classTy = std::get<TyClass>(*sym->ty);
    auto def = classTy.def;

    auto enumScope = std::make_unique<Scope>(scope);
    Scope* es = enumScope.get();
    es->setSelfTy(sym->ty);
    es->setClassDef(def);
    scope->addChildMod(ec.name, std::move(enumScope));

    for (size_t i = 0; i < ec.generics.size(); ++i) {
        TyPtr paramTy = std::make_shared<Ty>(TyParam{ec.generics[i].name, static_cast<uint32_t>(i)});
        es->declare(ec.generics[i].name, SymbolKind::Type, nullptr, paramTy);
    }

    // Register variant metadata
    uint32_t maxPayloadSize = 0;
    uint32_t disc = 0;
    for (const auto& v : ec.variants) {
        EnumVariantInfo info;
        info.name = v.name;
        info.isTuple = v.isTuple;
        info.discriminant = disc;

        uint32_t nextOffset = 9;
        for (const auto& field : v.fields) {
            FieldInfo fi;
            fi.name = field.first;
            fi.type = nullptr;
            if (field.second) {
                if (auto* pathTy = std::get_if<AstTyPath>(field.second.get())) {
                    const auto& tyName = pathTy->path.segs.back();
                    if (tyName == "int") fi.type = mkInt();
                    else if (tyName == "long") fi.type = mkLong();
                    else if (tyName == "bool") fi.type = mkBool();
                    else if (tyName == "char") fi.type = mkChar();
                    else if (tyName == "str") fi.type = mkStr();
                    else if (tyName == "float") fi.type = mkFloat();
                    else if (tyName == "double") fi.type = mkDouble();
                    else if (Symbol* local = es->lookupLocal(tyName)) fi.type = local->ty;
                    else if (Symbol* resolved = scope->lookup(tyName)) fi.type = resolved->ty;
                }
            }
            fi.offset = nextOffset;
            fi.isVal = true;
            info.fields.push_back(fi);
            nextOffset += 9;
        }
        info.payloadSize = nextOffset;
        if (info.payloadSize > maxPayloadSize) {
            maxPayloadSize = info.payloadSize;
        }

        FieldInfo marker;
        marker.name = v.name;
        marker.type = nullptr;
        marker.offset = disc;
        marker.isVal = true;
        def->fields.push_back(marker);
        def->variants.push_back(std::move(info));
        es->declare(v.name, SymbolKind::Value, nullptr, sym->ty);
        disc += 1;
    }
    def->totalFieldSize = maxPayloadSize == 0 ? 9 : maxPayloadSize;

    // Register methods
    for (const auto& m : ec.methods) {
        MethodInfo mi;
        mi.name = m.name;
        mi.funcType = nullptr;
        def->methods.push_back(mi);
        es->declare(m.name, SymbolKind::Value, nullptr, nullptr);
    }
}

// ============================================================
// Top-level build (two-pass)
// ============================================================
std::unique_ptr<Scope> ScopeBuilder::build(Module& mod) {
    auto root = std::make_unique<Scope>(nullptr);

    // Register built-in types
    root->declare("int", SymbolKind::Type, nullptr, mkInt());
    root->declare("long", SymbolKind::Type, nullptr, mkLong());
    root->declare("float", SymbolKind::Type, nullptr, mkFloat());
    root->declare("double", SymbolKind::Type, nullptr, mkDouble());
    root->declare("bool", SymbolKind::Type, nullptr, mkBool());
    root->declare("char", SymbolKind::Type, nullptr, mkChar());
    root->declare("str", SymbolKind::Type, nullptr, mkStr());
    root->declare("None", SymbolKind::Type, nullptr, mkUnit());
    root->declare("unit", SymbolKind::Type, nullptr, mkUnit());
    root->declare("void", SymbolKind::Type, nullptr, mkUnit());
    root->declare("()", SymbolKind::Type, nullptr, mkUnit());
    root->declare("noreturn", SymbolKind::Type, nullptr, mkNever());
    root->declare("null", SymbolKind::Type, nullptr, mkNull());

    // Register built-in functions
    // io: console
    root->declare("println", SymbolKind::Value, nullptr, nullptr);
    root->declare("print",   SymbolKind::Value, nullptr, nullptr);
    root->declare("write",   SymbolKind::Value, nullptr, nullptr);
    // mgm: program control
    root->declare("error",   SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkStr()}, mkNever()}));
    // mem: heap
    root->declare("halloc",  SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkInt()}, mkInt()}));
    root->declare("hread",   SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkInt(), mkInt()}, mkInt()}));
    root->declare("hset",    SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkInt(), mkInt(), mkInt()}, mkUnit()}));
    root->declare("hfree",   SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkInt()}, mkUnit()}));
    root->declare("alloc",   SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkInt()}, mkInt()}));
    root->declare("hload",   SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkInt(), mkInt()}, mkInt()}));
    root->declare("hstore",  SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkInt(), mkInt(), mkInt()}, mkUnit()}));
    // ptr: string ops
    root->declare("strlen",  SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkStr()}, mkInt()}));
    root->declare("i2s",     SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkInt()}, mkStr()}));
    root->declare("chr",     SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkInt()}, mkStr()}));
    root->declare("s2i",     SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkStr()}, mkInt()}));
    // sys: generic system call (unchecked — caller handles type)
    root->declare("syscall", SymbolKind::Value, nullptr, nullptr);
    // float primitives
    root->declare("itof",    SymbolKind::Value, nullptr, nullptr);
    root->declare("ftoi",    SymbolKind::Value, nullptr, nullptr);
    root->declare("fadd",    SymbolKind::Value, nullptr, nullptr);
    root->declare("fsub",    SymbolKind::Value, nullptr, nullptr);
    root->declare("fmul",    SymbolKind::Value, nullptr, nullptr);
    root->declare("fdiv",    SymbolKind::Value, nullptr, nullptr);
    root->declare("fneg",    SymbolKind::Value, nullptr, nullptr);

    // Pass 1: Declare all top-level names
    for (const auto& item : mod.items) {
        declareItem(item, root.get());
    }

    // Pass 2: Resolve class/interface/object internals
    for (const auto& item : mod.items) {
        resolveItem(item, root.get());
    }

    return root;
}

} // namespace
