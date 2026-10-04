#include "Lexer.hpp"
#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace loxis::v2 {

Lexer::Lexer(std::string source, std::string filename, SourceLocation start)
    : src(std::move(source)), file(std::move(filename)) {
    // A default-constructed SourceLocation has line == 0; that means "no base given".
    if (start.line != 0) { line = start.line; col = start.col; }
}

bool Lexer::atEnd() const { return pos >= src.size(); }
char Lexer::peek(size_t off) const { return pos + off < src.size() ? src[pos + off] : '\0'; }
char Lexer::advance() {
    char c = src[pos++];
    if (c == '\n' || c == '\r') { ++line; col = 1; }
    else { ++col; }
    return c;
}

SourceLocation Lexer::loc() const { return {file, line, col}; }

void Lexer::skipSpace() {
    while (!atEnd() && (peek() == ' ' || peek() == '\t')) advance();
}

bool Lexer::match(char c) {
    if (peek() == c) { advance(); return true; }
    return false;
}

bool Lexer::isIdStart(char c) const { return c == '_' || std::isalpha(static_cast<unsigned char>(c)); }
bool Lexer::isIdCont(char c) const { return c == '_' || std::isalnum(static_cast<unsigned char>(c)); }

Tk Lexer::keyword(const std::string& s) const {
    static const std::unordered_map<std::string, Tk> kw = {
        {"package", Tk::KwPackage}, {"import", Tk::KwImport}, {"from", Tk::KwFrom},
        {"fun", Tk::KwFun}, {"let", Tk::KwLet}, {"var", Tk::KwVar}, {"val", Tk::KwVal},
        {"vararg", Tk::KwVararg},
        {"class", Tk::KwClass}, {"interface", Tk::KwInterface}, {"object", Tk::KwObject}, {"enum", Tk::KwEnum},
        {"open", Tk::KwOpen}, {"abstract", Tk::KwAbstract}, {"data", Tk::KwData}, {"override", Tk::KwOverride},
        {"init", Tk::KwInit}, {"companion", Tk::KwCompanion},
        {"for", Tk::KwFor}, {"while", Tk::KwWhile}, {"if", Tk::KwIf},
        {"else", Tk::KwElse}, {"when", Tk::KwWhen}, {"return", Tk::KwReturn},
        {"break", Tk::KwBreak}, {"continue", Tk::KwContinue}, {"loop", Tk::KwLoop},
        {"throw", Tk::KwThrow}, {"try", Tk::KwTry}, {"catch", Tk::KwCatch}, {"finally", Tk::KwFinally},
        {"true", Tk::KwTrue}, {"false", Tk::KwFalse}, {"null", Tk::KwNull},
        {"is", Tk::KwIs}, {"in", Tk::KwIn}, {"as", Tk::KwAs},
        {"where", Tk::KwWhere}, {"type", Tk::KwType}, {"const", Tk::KwConst},
        {"public", Tk::KwPublic}, {"internal", Tk::KwInternal},
        {"private", Tk::KwPrivate},
    };
    auto it = kw.find(s);
    return it != kw.end() ? it->second : Tk::Ident;
}

// Scans a string lexeme. It cannot simply stop at the first `"`, because
// `${...}` may contain real expressions -- including string literals and braces
// of their own -- that must not terminate the outer string. Every string is
// scanned this way; whether the result actually interpolates is for the parser
// to decide, since only it knows how to parse the code parts.
std::string Lexer::literalStr() {
    size_t start = pos;
    SourceLocation startLoc = loc();
    advance(); // opening "
    while (!atEnd()) {
        char c = peek();
        if (c == '\\') { advance(); if (!atEnd()) advance(); continue; }
        if (c == '"') { advance(); return src.substr(start, pos - start); }
        if (c == '$' && peek(1) == '{') {
            advance(); advance();  // "$" and "{"
            int depth = 1;
            while (!atEnd() && depth > 0) {
                char d = peek();
                if (d == '{') { ++depth; advance(); continue; }
                if (d == '}') { --depth; advance(); continue; }
                // Nested string literals are skipped whole, so a brace or quote
                // inside one cannot throw off the depth count.
                if (d == '"') { advance(); while (!atEnd() && peek() != '"') { if (peek() == '\\') advance(); if (!atEnd()) advance(); } if (!atEnd()) advance(); continue; }
                if (d == '\\') { advance(); if (!atEnd()) advance(); continue; }
                advance();
            }
            continue;
        }
        advance();
    }
    throw FrontendError(Diagnostic{DiagnosticSeverity::Error, DiagnosticKind::Syntax,
        "unterminated string literal", {{startLoc, loc()}, "string starts here"}});
}

std::string Lexer::literalChar() {
    size_t start = pos;
    SourceLocation startLoc = loc();
    advance(); // opening '
    if (!atEnd() && peek() == '\\') { advance(); if (!atEnd()) advance(); }
    else if (!atEnd() && peek() != '\'' && peek() != '\n' && peek() != '\r') advance();
    if (atEnd() || peek() != '\'') {
        throw FrontendError(Diagnostic{DiagnosticSeverity::Error, DiagnosticKind::Syntax,
            "unterminated character literal", {{startLoc, loc()}, "character literal starts here"}});
    }
    advance();
    return src.substr(start, pos - start);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    // UTF-8 BOM is transport metadata, not source text.
    if (pos == 0 && src.size() >= 3 &&
        static_cast<unsigned char>(src[0]) == 0xEF &&
        static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF) {
        advance(); advance(); advance();
    }
    while (!atEnd()) {
        skipSpace();
        if (atEnd()) break;
        SourceLocation l = loc();
        size_t start = pos;
        char c = peek();
        // newlines
        if (c == '\n' || c == '\r') {
            advance();
            if (c == '\r' && !atEnd() && peek() == '\n') advance();
            out.emplace_back(Tk::Newline, "\n", l);
            continue;
        }
        // comments
        if (c == '/' && peek(1) == '/') {
            advance(); advance();
            while (!atEnd() && peek() != '\n' && peek() != '\r') advance();
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            advance(); advance();
            while (!atEnd() && !(peek() == '*' && peek(1) == '/')) advance();
            if (atEnd()) {
                throw FrontendError(Diagnostic{DiagnosticSeverity::Error, DiagnosticKind::Syntax,
                    "unterminated block comment", {{l, loc()}, "comment starts here"}});
            }
            advance(); advance();
            continue;
        }
        // identifier / keyword / underscore
        if (isIdStart(c)) {
            while (!atEnd() && isIdCont(peek())) advance();
            std::string lex = src.substr(start, pos - start);
            Tk kind = (lex == "_") ? Tk::Underscore : keyword(lex);
            out.emplace_back(kind, lex, l);
            continue;
        }
        // numbers
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
            bool isFloat = false;
            if (c == '0' && (peek(1) == 'x' || peek(1) == 'b' || peek(1) == 'o')) {
                advance(); advance();
                while (!atEnd() && (std::isxdigit(static_cast<unsigned char>(peek())) || peek() == '_')) advance();

                out.emplace_back(Tk::IntLit, src.substr(start, pos - start), l);
                continue;
            }
            while (!atEnd() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')) advance();
            if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
                isFloat = true; advance();
                while (!atEnd() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')) advance();
            }
            if (peek() == 'e' || peek() == 'E') {
                isFloat = true;
                size_t epos = pos;
                advance();
                if (peek() == '+' || peek() == '-') advance();
                if (!std::isdigit(static_cast<unsigned char>(peek()))) { pos = epos; col = l.col + static_cast<uint32_t>(epos - start); }
                else while (!atEnd() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')) advance();
            }
            if (peek() == 'f' || peek() == 'F') {
                isFloat = true;
                advance();
            }
            if (isFloat) {

                out.emplace_back(Tk::FloatLit, src.substr(start, pos - start), l);
            } else {

                out.emplace_back(Tk::IntLit, src.substr(start, pos - start), l);
            }
            continue;
        }
        // string
        if (c == '"') {
            out.emplace_back(Tk::StringLit, literalStr(), l);
            continue;
        }
        // char
        if (c == '\'') {
            out.emplace_back(Tk::CharLit, literalChar(), l);
            continue;
        }
        // operators / punctuation
        auto emit1 = [&](Tk k) { advance(); out.emplace_back(k, src.substr(start, 1), l); };
        auto emit2 = [&](Tk k) { advance(); advance(); out.emplace_back(k, src.substr(start, 2), l); };
        auto emit3 = [&](Tk k) { advance(); advance(); advance(); out.emplace_back(k, src.substr(start, 3), l); };
        switch (c) {
            case '+': (peek(1) == '=') ? emit2(Tk::PlusEq) : emit1(Tk::Plus); break;
            case '-':
                if (peek(1) == '>') emit2(Tk::Arrow);
                else if (peek(1) == '=') emit2(Tk::MinusEq);
                else emit1(Tk::Minus);
                break;
            case '*': (peek(1) == '=') ? emit2(Tk::StarEq) : emit1(Tk::Star); break;
            case '/': (peek(1) == '=') ? emit2(Tk::SlashEq) : emit1(Tk::Slash); break;
            case '%': (peek(1) == '=') ? emit2(Tk::PercentEq) : emit1(Tk::Percent); break;
            case '&':
                if (peek(1) == '&') emit2(Tk::AndAnd);
                else if (peek(1) == '=') emit2(Tk::AmpEq);
                else emit1(Tk::Amp);
                break;
            case '|':
                if (peek(1) == '|') emit2(Tk::OrOr);
                else if (peek(1) == '=') emit2(Tk::PipeEq);
                else emit1(Tk::Pipe);
                break;
            case '^': (peek(1) == '=') ? emit2(Tk::CaretEq) : emit1(Tk::Caret); break;
            case '~': emit1(Tk::Tilde); break;
            case '!': (peek(1) == '=') ? emit2(Tk::Ne) : emit1(Tk::Bang); break;
            case '<':
                if (peek(1) == '<') (peek(2) == '=') ? emit3(Tk::ShlEq) : emit2(Tk::Shl);
                else if (peek(1) == '=') emit2(Tk::Le);
                else emit1(Tk::Lt);
                break;
            case '>':
                if (peek(1) == '>') (peek(2) == '=') ? emit3(Tk::ShrEq) : emit2(Tk::Shr);
                else if (peek(1) == '=') emit2(Tk::Ge);
                else emit1(Tk::Gt);
                break;
            case '=':
                if (peek(1) == '=') emit2(Tk::EqEq);
                else if (peek(1) == '>') emit2(Tk::FatArrow);
                else emit1(Tk::Eq);
                break;
            case '.':
                if (peek(1) == '.') (peek(2) == '=') ? emit3(Tk::DotDotEq) : emit2(Tk::DotDot);
                else emit1(Tk::Dot);
                break;
            case ':': (peek(1) == ':') ? emit2(Tk::DColon) : emit1(Tk::Colon); break;
            case ';': emit1(Tk::Semi); break;
            case ',': emit1(Tk::Comma); break;
            case '(': emit1(Tk::LParen); break;
            case ')': emit1(Tk::RParen); break;
            case '{': emit1(Tk::LBrace); break;
            case '}': emit1(Tk::RBrace); break;
            case '[': emit1(Tk::LBracket); break;
            case ']': emit1(Tk::RBracket); break;
            case '#': emit1(Tk::Pound); break;
            case '$': emit1(Tk::Dollar); break;
            case '@': emit1(Tk::At); break;
            case '?': emit1(Tk::Question); break;
            default:
                SourceLocation end = l;
                ++end.col;
                throw FrontendError(Diagnostic{DiagnosticSeverity::Error, DiagnosticKind::Syntax,
                    "unexpected character `" + std::string(1, c) + "`", {{l, end}, "unexpected token"}});
        }
    }
    out.emplace_back(Tk::Eof, "", SourceLocation{file, line, col});
    return out;
}

} // namespace loxis::v2
