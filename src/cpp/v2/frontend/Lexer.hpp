#pragma once
#include <string>
#include <vector>
#include "../Token.hpp"

namespace loxis::v2 {

class Lexer {
public:
    // `start` lets a fragment be lexed as if it sat at that position in the
    // original file, so diagnostics point at real line/column numbers instead of
    // restarting at 1:1. It is used by string templates, which lex the text
    // inside `${...}` on its own. When unset the stream starts at 1:1.
    Lexer(std::string source, std::string filename, SourceLocation start = {});
    std::vector<Token> tokenize();
private:
    std::string src, file;
    size_t pos = 0;
    uint32_t line = 1, col = 1;

    bool atEnd() const;
    char peek(size_t off = 0) const;
    char advance();
    SourceLocation loc() const;

    void skipSpace();
    bool match(char c);
    bool isIdStart(char c) const;
    bool isIdCont(char c) const;
    Tk keyword(const std::string& s) const;

    std::string literalStr();
    std::string literalChar();
};

} // namespace loxis::v2
