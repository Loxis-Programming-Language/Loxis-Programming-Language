#include "Scope.hpp"
#include <cstdio>

namespace loxis::v2 {

// ============================================================
// Scope
// ============================================================
Scope::Scope(Scope* parent) : parent_(parent) {}

void Scope::declare(const std::string& name, SymbolKind kind, void* node, TyPtr ty) {
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
            for (const auto& g : n.generics) def->params.push_back(g.name);
            for (const auto& s : n.supers) def->superNames.push_back(s.segs.back());
            TyPtr ty = std::make_shared<Ty>(TyInterface{def, {}});
            scope->declare(n.name, SymbolKind::Interface,
                const_cast<void*>(static_cast<const void*>(&n)), ty);
        } else if constexpr (std::is_same_v<T, ItemObject>) {
            auto def = std::make_shared<ClassDef>();
            def->name = n.name;
            if (n.superClass) def->superName = n.superClass->segs.back();
            for (const auto& iface : n.interfaces)
                def->interfaceNames.push_back(iface.segs.back());
            TyPtr ty = std::make_shared<Ty>(TyClass{def, {}});
            scope->declare(n.name, SymbolKind::Value,
                const_cast<void*>(static_cast<const void*>(&n)), ty);
        } else if constexpr (std::is_same_v<T, ItemVal>) {
            scope->declare(n.name, SymbolKind::Value,
                const_cast<void*>(static_cast<const void*>(&n)), nullptr);
        } else if constexpr (std::is_same_v<T, ItemVar>) {
            scope->declare(n.name, SymbolKind::Value,
                const_cast<void*>(static_cast<const void*>(&n)), nullptr);
        } else if constexpr (std::is_same_v<T, ItemConst>) {
            scope->declare(n.name, SymbolKind::Value,
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
        mi.isOpen = m.isOpen;
        mi.isAbstract = m.isAbstract;
        mi.isOverride = m.isOverride;

        // Check for override: if method exists in super, reuse vtable index
        if (mi.isOverride) {
            for (auto& existing : def->methods) {
                if (existing.name == mi.name) {
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
        checkInterfaceConformance(*def, scope);
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
            continue; // error handled by type checker
        }
        auto& ifaceTy = std::get<TyInterface>(*ifaceSym->ty);
        auto ifaceDef = ifaceTy.def;

        for (const auto& im : ifaceDef->methods) {
            bool found = false;
            for (const auto& cm : def.methods) {
                if (cm.name == im.name) { found = true; break; }
            }
            if (!found) {
                // error: class does not implement interface method
                return false;
            }
        }
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

    // Register own methods
    for (const auto& m : iface.methods) {
        MethodInfo mi;
        mi.name = m.name;
        mi.funcType = nullptr;
        mi.isAbstract = !m.defaultBody; // abstract if no default body
        def->methods.push_back(mi);
        is->declare(m.name, SymbolKind::Value, nullptr, nullptr);
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

    auto objScope = std::make_unique<Scope>(scope);
    Scope* os = objScope.get();
    os->setSelfTy(sym->ty);
    os->setClassDef(def);
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
        def->methods.push_back(mi);
        os->declare(m.name, SymbolKind::Value, nullptr, nullptr);
    }

    buildVTable(*def);
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

    // Register variant names
    uint32_t disc = 0;
    for (const auto& v : ec.variants) {
        FieldInfo fi;
        fi.name = v.name;
        fi.type = nullptr;
        fi.offset = disc++; // discriminant stored in offset field for variants
        fi.isVal = true;
        def->fields.push_back(fi);
        es->declare(v.name, SymbolKind::Value, nullptr, nullptr);
    }

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
    root->declare("unit", SymbolKind::Type, nullptr, mkUnit());
    root->declare("void", SymbolKind::Type, nullptr, mkUnit());
    root->declare("()", SymbolKind::Type, nullptr, mkUnit());
    root->declare("noreturn", SymbolKind::Type, nullptr, mkNever());
    root->declare("null", SymbolKind::Type, nullptr, mkNull());

    // Register built-in functions (prelude)
    // println/print: accept any type — use nullptr type to skip checking
    root->declare("println", SymbolKind::Value, nullptr, nullptr);
    root->declare("print", SymbolKind::Value, nullptr, nullptr);
    // error: (str) -> noreturn
    root->declare("error", SymbolKind::Value, nullptr,
        std::make_shared<Ty>(TyFn{{mkStr()}, mkNever()}));

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
