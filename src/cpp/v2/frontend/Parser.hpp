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
    bool isItemStart();
    void skipToBoundary();
    void error(const std::string& msg);

    // generic helpers
    std::vector<GenericParam> parseGenerics();
    std::vector<WhereBound> parseWhere();
    TypePtr parseRet();
    std::vector<ParamDecl> parseClassParams();
    std::vector<std::pair<std::string,TypePtr>> parseFnParams();
    Path parsePath();
    Path parseTypePath();
    std::vector<TypePtr> parseGenericArgs();
    void expectGt();
    Visibility parseVisibility();
    Path parseDottedPath();

    // class modifiers
    ClassModifier parseClassModifier();

    // items
    ItemPtr parseItem();
    ItemFun parseFun(Visibility vis);
    ItemPtr parseClass(Visibility vis, ClassModifier mod);
    ItemPtr parseInterface(Visibility vis);
    ItemPtr parseEnumClass(Visibility vis);
    ItemPtr parseObject(Visibility vis);
    ItemPtr parseVal(Visibility vis);
    ItemPtr parseVar(Visibility vis);
    ItemPtr parseConst(Visibility vis);
    ItemPtr parseImport();
    ItemPtr parseFromImport();

    // statements
    StmtPtr parseStmt();
    StmtPtr parseLet();
    StmtPtr parseItemStmt();

    // expressions
    ExprPtr parseExpr();
    ExprPtr parseAssign();
    ExprPtr parseElvis();
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
    ExprPtr parseWhen();
    ExprPtr parseBreak();
    ExprPtr parseContinue();
    ExprPtr parseReturn();
    ExprPtr parseClosure();
    ExprPtr parseStructExpr(Path path);
    ExprPtr parseLiteral();
    ExprPtr parseNull();

    // patterns
    PatPtr parsePat();

    // types
    TypePtr parseType();
};

} // namespace loxis::v2
