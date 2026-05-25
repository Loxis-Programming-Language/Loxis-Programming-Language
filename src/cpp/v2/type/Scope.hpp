#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include "../AST.hpp"
#include "Type.hpp"

namespace loxis::v2 {

enum class SymbolKind { Value, Type, Trait };

struct Symbol {
    std::string name;
    SymbolKind kind;
    void* node;
    TyPtr ty;
};

class Scope {
public:
    explicit Scope(Scope* parent = nullptr);
    void declare(const std::string& name, SymbolKind kind, void* node, TyPtr ty);
    Symbol* lookup(const std::string& name);
    Scope* parent() const;
    bool isRoot() const;
    Scope* childMod(const std::string& name) const;
    void addChildMod(const std::string& name, std::unique_ptr<Scope> child);
private:
    Scope* parent_;
    std::unordered_map<std::string, Symbol> symbols_;
    std::unordered_map<std::string, std::unique_ptr<Scope>> children_;
};

class ScopeBuilder {
public:
    std::unique_ptr<Scope> build(Module& mod);
private:
    void processItems(const std::vector<ItemPtr>& items, Scope* scope);
    void processUse(const ItemUse& use, Scope* scope);
};

} // namespace
