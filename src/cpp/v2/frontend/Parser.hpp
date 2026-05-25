#pragma once
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include "../Token.hpp"
#include "../AST.hpp"

namespace loxis::v2 {

class Parser {
public:
    static Module parseFile(const std::string& path);
    explicit Parser(std::vector<Token> tokens);
    Module parse();

private:
    std::vector<Token> toks;
    size_t pos = 0;

    // token helpers
    Token& peek(size_t off = 0);
    bool check(Tk k);
    bool check(Tk k, size_t off);
    bool match(Tk k);
    Token& expect(Tk k);
    bool atEnd();
    void advance();
    SourceLoc curloc();

    // basic helpers
    std::string expectIdent();
    std::string expectIdentOrKw();
    bool isItemStart();
    void skipToBoundary();
    void error(const std::string& msg);

    // generic helpers
    std::vector<GenericParam> parseGenerics();
    std::vector<WhereBound> parseWhere();
    TypePtr parseRet();
    std::vector<std::pair<std::string,TypePtr>> parseFnParams();
    Path parsePath();
    Path parseTypePath();
    std::vector<TypePtr> parseGenericArgs();
    void expectGt();

    // items
    ItemPtr parseItem();
    ItemFn parseFnBody(bool pub, bool unsafe, bool ext);
    ItemPtr parseStruct(bool pub);
    ItemPtr parseEnum(bool pub);
    ItemPtr parseTrait(bool pub);
    ItemPtr parseImpl();
    ItemPtr parseMod(bool pub);
    ItemPtr parseUse(bool pub);
    ItemPtr parseConst(bool pub);
    ItemPtr parseStatic(bool pub);
    UseTree parseUseTree();

    // statements
    StmtPtr parseStmt();
    StmtPtr parseLet();
    StmtPtr parseItemStmt();

    // expressions
    ExprPtr parseExpr();
    ExprPtr parseAssign();
    ExprPtr parseRange();
    ExprPtr parseOr();
    ExprPtr parseAnd();
    ExprPtr parseCmp();
    ExprPtr parseBitOr();
    ExprPtr parseBitXor();
    ExprPtr parseBitAnd();
    ExprPtr parseShift();
    ExprPtr parseAdd();
    ExprPtr parseMul();
    ExprPtr parseCast();
    ExprPtr parsePrefix();
    ExprPtr parsePostfix();
    ExprPtr parsePrimary();
    ExprPtr parseBlock();
    ExprPtr parseIf();
    ExprPtr parseWhile(std::optional<std::string> label);
    ExprPtr parseFor(std::optional<std::string> label);
    ExprPtr parseLoop(std::optional<std::string> label);
    ExprPtr parseMatch();
    ExprPtr parseBreak();
    ExprPtr parseContinue();
    ExprPtr parseReturn();
    ExprPtr parseClosure();
    ExprPtr parseStructExpr(Path path);
    ExprPtr parseLiteral();

    // patterns
    PatPtr parsePat();

    // types
    TypePtr parseType();
};

} // namespace loxis::v2
