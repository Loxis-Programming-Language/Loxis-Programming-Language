#pragma once
#include <string>
#include <vector>
#include "../Token.hpp"

namespace loxis::v2 {

class Lexer {
public:
    Lexer(std::string source, std::string filename);
    std::vector<Token> tokenize();
private:
    std::string src, file;
    size_t pos = 0;
    uint32_t line = 1, col = 1;

    bool atEnd() const;
    char peek(size_t off = 0) const;
    char advance();
    SourceLoc loc() const;

    void skipSpace();
    bool match(char c);
    bool isIdStart(char c) const;
    bool isIdCont(char c) const;
    Tk keyword(const std::string& s) const;
    bool trySuffix();
    std::string literalStr();
    std::string literalChar();
};

} // namespace loxis::v2
