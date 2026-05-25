#include "Scope.hpp"

namespace loxis::v2 {

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

Scope* Scope::parent() const { return parent_; }
bool Scope::isRoot() const { return parent_ == nullptr; }

Scope* Scope::childMod(const std::string& name) const {
    auto it = children_.find(name);
    return it != children_.end() ? it->second.get() : nullptr;
}

void Scope::addChildMod(const std::string& name, std::unique_ptr<Scope> child) {
    children_[name] = std::move(child);
}

std::unique_ptr<Scope> ScopeBuilder::build(Module& mod) {
    auto root = std::make_unique<Scope>(nullptr);
    root->declare("i32", SymbolKind::Type, nullptr, mkI32());
    root->declare("i64", SymbolKind::Type, nullptr, mkI64());
    root->declare("u32", SymbolKind::Type, nullptr, mkU32());
    root->declare("u64", SymbolKind::Type, nullptr, mkU64());
    root->declare("f32", SymbolKind::Type, nullptr, mkF32());
    root->declare("f64", SymbolKind::Type, nullptr, mkF64());
    root->declare("bool", SymbolKind::Type, nullptr, mkBool());
    root->declare("str", SymbolKind::Type, nullptr, mkStr());
    root->declare("String", SymbolKind::Type, nullptr, mkString());
    root->declare("()", SymbolKind::Type, nullptr, mkUnit());
    root->declare("!", SymbolKind::Type, nullptr, mkNever());
    processItems(mod.items, root.get());
    return root;
}

void ScopeBuilder::processItems(const std::vector<ItemPtr>& items, Scope* scope) {
    for (const auto& item : items) {
        std::visit([&](auto&& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ItemFn>) {
                scope->declare(n.name, SymbolKind::Value,
                    const_cast<void*>(static_cast<const void*>(&n)), nullptr);
            } else if constexpr (std::is_same_v<T, ItemStruct>) {
                auto def = std::make_shared<AdtDef>();
                def->name = n.name;
                for (const auto& g : n.generics) def->params.push_back(g.name);
                TyPtr ty = std::make_shared<Ty>(TyAdt{def, {}});
                scope->declare(n.name, SymbolKind::Type,
                    const_cast<void*>(static_cast<const void*>(&n)), ty);
            } else if constexpr (std::is_same_v<T, ItemEnum>) {
                auto def = std::make_shared<AdtDef>();
                def->name = n.name;
                for (const auto& g : n.generics) def->params.push_back(g.name);
                TyPtr ty = std::make_shared<Ty>(TyAdt{def, {}});
                scope->declare(n.name, SymbolKind::Type,
                    const_cast<void*>(static_cast<const void*>(&n)), ty);
                for (const auto& v : n.vars) {
                    scope->declare(v.name, SymbolKind::Value,
                        const_cast<void*>(static_cast<const void*>(&n)), ty);
                }
            } else if constexpr (std::is_same_v<T, ItemTrait>) {
                TyPtr ty = std::make_shared<Ty>(TyTrait{n.name, {}});
                scope->declare(n.name, SymbolKind::Trait,
                    const_cast<void*>(static_cast<const void*>(&n)), ty);
            } else if constexpr (std::is_same_v<T, ItemImpl>) {
            } else if constexpr (std::is_same_v<T, ItemMod>) {
                auto child = std::make_unique<Scope>(scope);
                Scope* childPtr = child.get();
                processItems(n.items, childPtr);
                scope->addChildMod(n.name, std::move(child));
            } else if constexpr (std::is_same_v<T, ItemUse>) {
                processUse(n, scope);
            } else if constexpr (std::is_same_v<T, ItemStatic>) {
                scope->declare(n.name, SymbolKind::Value,
                    const_cast<void*>(static_cast<const void*>(&n)), nullptr);
            } else if constexpr (std::is_same_v<T, ItemConst>) {
                scope->declare(n.name, SymbolKind::Value,
                    const_cast<void*>(static_cast<const void*>(&n)), nullptr);
            }
        }, *item);
    }
}

void ScopeBuilder::processUse(const ItemUse& use, Scope* scope) {
    std::function<void(const UseTree&, const std::vector<std::string>&)> walk;
    walk = [&](const UseTree& tree, const std::vector<std::string>& prefix) {
        std::vector<std::string> segs = prefix;
        for (const auto& s : tree.path.segs) segs.push_back(s);
        if (tree.kind == UseTree::Glob) {
        } else if (tree.kind == UseTree::Nested) {
            for (const auto& sub : tree.nested) walk(sub, segs);
        } else {
            Symbol* sym = nullptr;
            Scope* cur = scope;
            for (size_t i = 0; i < segs.size(); ++i) {
                sym = nullptr;
                Scope* search = cur;
                while (search) {
                    sym = search->lookup(segs[i]);
                    if (sym) break;
                    search = search->parent();
                }
                if (!sym) break;
                if (i + 1 < segs.size()) {
                    Scope* next = nullptr;
                    search = cur;
                    while (search) {
                        next = search->childMod(segs[i]);
                        if (next) break;
                        search = search->parent();
                    }
                    if (next) {
                        cur = next;
                    } else {
                        sym = nullptr;
                        break;
                    }
                }
            }
            if (sym) {
                std::string alias = tree.rename ? *tree.rename : segs.back();
                scope->declare(alias, sym->kind, sym->node, sym->ty);
            }
        }
    };
    walk(use.tree, {});
}

} // namespace
