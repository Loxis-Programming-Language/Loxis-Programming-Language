#pragma once

#include <iosfwd>
#include <string>

namespace loxis::v2 {

struct Module;
struct HirModule;
struct Scope;
struct LoweringOutput;

namespace dump {

// Indenting writer used by every --dump-* printer. Brace style mirrors what the
// rest of the compiler emits for a tree, e.g.
//
//   ExprMethodCall {
//     recv: ExprPath { Demo }
//     method: "forEach"
//   }
class Printer {
public:
    explicit Printer(std::ostream& os, int indentWidth = 2)
        : m_os(os), m_width(indentWidth) {}

    // One indented line.
    void line(const std::string& text);

    // `text {` followed by a deeper indent; pair with close().
    void open(const std::string& text);

    // Back to the outer indent, then `text` (default "}").
    void close(const std::string& text = "}");

    // `name: value` on its own line.
    void field(const std::string& name, const std::string& value);

    std::ostream& stream() { return m_os; }

private:
    std::ostream& m_os;
    int m_indent = 0;
    int m_width;
};

// --dump-tokens: lexes `source` and prints one token per line with its
// file:line:col and raw lexeme.
void dumpTokens(const std::string& source, const std::string& filename, std::ostream& os);

// --dump-ast: the parsed module tree.
void dumpAst(const Module& mod, std::ostream& os);

// --dump-hir: the lowered, monomorphised HIR module.
void dumpHir(const HirModule& hir, std::ostream& os);

// --dump-mir: every MirBody produced by Lowering, plus class/global metadata.
void dumpMir(const LoweringOutput& output, std::ostream& os);

// --dump-scopes: the scope/symbol tree built by ScopeBuilder. Writes to `os`.
void dumpScopes(const Scope& root, std::ostream& os);

} // namespace dump
} // namespace loxis::v2
