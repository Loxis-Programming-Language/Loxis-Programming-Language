#include "Dump.hpp"

#include "../Token.hpp"
#include "../frontend/Lexer.hpp"

#include <ostream>
#include <vector>

namespace loxis::v2::dump {

namespace {

// A lexeme printed raw would put a literal newline inside the quotes for the
// newline token and wreck the column alignment of everything after it.
std::string escaped(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c;
        }
    }
    return out;
}

} // namespace

void dumpTokens(const std::string& source, const std::string& filename, std::ostream& os) {
    Lexer lexer(source, filename);
    const std::vector<Token> tokens = lexer.tokenize();

    os << "; " << tokens.size() << " tokens from " << filename << "\n";
    for (const Token& token : tokens) {
        os << "  " << token.loc.line << ":" << token.loc.col << "  " << tkName(token.kind);
        if (!token.lex.empty()) os << "  '" << escaped(token.lex) << "'";
        os << "\n";
    }
}

} // namespace loxis::v2::dump
