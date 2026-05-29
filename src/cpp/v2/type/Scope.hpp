#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "../AST.hpp"
#include "Type.hpp"

namespace loxis::v2 {

enum class SymbolKind { Value, Type, Interface };

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
    Symbol* lookupLocal(const std::string& name);

    Scope* parent() const;
    bool isRoot() const;

    // class scope support
    TyPtr selfTy() const { return selfTy_; }
    void setSelfTy(TyPtr ty) { selfTy_ = ty; }
    std::shared_ptr<ClassDef> classDef() const { return classDef_; }
    void setClassDef(std::shared_ptr<ClassDef> def) { classDef_ = def; }

    // child modules
    Scope* childMod(const std::string& name) const;
    void addChildMod(const std::string& name, std::unique_ptr<Scope> child);

    // debug: dump scope tree
    void dump(int indent = 0) const;

    // access for debug
    const std::unordered_map<std::string, Symbol>& symbols() const { return symbols_; }
    const std::unordered_map<std::string, std::unique_ptr<Scope>>& children() const { return children_; }

private:
    Scope* parent_;
    std::unordered_map<std::string, Symbol> symbols_;
    std::unordered_map<std::string, std::unique_ptr<Scope>> children_;
    TyPtr selfTy_ = nullptr;
    std::shared_ptr<ClassDef> classDef_ = nullptr;
};

class ScopeBuilder {
public:
    std::unique_ptr<Scope> build(Module& mod);

private:
    // Pass 1: declare names in scope
    void declareItem(ItemPtr item, Scope* scope);
    // Pass 2: resolve internals (class fields/methods, interface methods, inheritance)
    void resolveItem(ItemPtr item, Scope* scope);

    // class/interface/object analysis helpers
    void resolveClass(ItemClass& cls, Scope* scope);
    void resolveInterface(ItemInterface& iface, Scope* scope);
    void resolveObject(ItemObject& obj, Scope* scope);
    void resolveEnumClass(ItemEnumClass& ec, Scope* scope);

    // Inherit fields/methods from superclass (flatten into ClassDef)
    void inheritFromSuper(ClassDef& def, Scope* scope);
    // Build vtable indices for virtual methods
    void buildVTable(ClassDef& def);
    // Check that all interface methods are implemented
    bool checkInterfaceConformance(ClassDef& def, Scope* scope);
};

} // namespace
