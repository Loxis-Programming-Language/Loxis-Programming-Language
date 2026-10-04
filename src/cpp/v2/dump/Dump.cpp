#include "Dump.hpp"

#include "../type/Scope.hpp"

#include <ostream>

namespace loxis::v2::dump {

void Printer::line(const std::string& text) {
    for (int i = 0; i < m_indent * m_width; ++i) m_os << ' ';
    m_os << text << '\n';
}

void Printer::open(const std::string& text) {
    line(text + " {");
    ++m_indent;
}

void Printer::close(const std::string& text) {
    if (m_indent > 0) --m_indent;
    line(text);
}

void Printer::field(const std::string& name, const std::string& value) {
    line(name + ": " + value);
}

void dumpScopes(const Scope& root, std::ostream& os) {
    // Scope::dump() prints through printf() rather than a stream, so it cannot
    // be captured here. Walk the public accessors instead and keep the same
    // shape: one line per symbol, one bracketed header per nested scope.
    Printer p(os);

    const auto emit = [&](const Scope& scope, auto&& self) -> void {
        for (const auto& [name, sym] : scope.symbols()) {
            const char* kind = sym.kind == SymbolKind::Type ? "type" :
                               sym.kind == SymbolKind::Interface ? "iface" : "val";
            p.line(std::string(kind) + " " + name + " : " + (sym.ty ? sym.ty->toString() : "?"));
        }
        for (const auto& [name, child] : scope.children()) {
            std::string header = "scope " + name;
            if (child->selfTy()) header += " self=" + child->selfTy()->toString();
            if (auto cd = child->classDef()) {
                header += " fields=" + std::to_string(cd->fields.size())
                        + " methods=" + std::to_string(cd->methods.size())
                        + " vtable=" + std::to_string(cd->vtable.size())
                        + " size=" + std::to_string(cd->totalFieldSize);
            }
            p.open(header);
            self(*child, self);
            p.close();
        }
    };
    emit(root, emit);
}

} // namespace loxis::v2::dump
