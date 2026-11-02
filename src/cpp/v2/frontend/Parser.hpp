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

    // Parses exactly one expression from this parser's token stream. String
    // templates use it to turn the text inside `${...}` into a real expression
    // tree rather than an opaque name.
    ExprPtr parseEmbeddedExpr();

private:
    std::vector<Token> toks;
    size_t pos = 0;
    bool m_quietErrors = false;     // suppress error output during speculative parse
    bool m_parsingBlockHead = false;  // disambiguate control-flow heads from struct literals

    // Recursive-descent parsing has no natural depth bound. Without one, a
    // pathological input (deeply nested parens, types or patterns) walks off the
    // end of the native stack and the compiler dies with no diagnostic at all.
    // Tuned against the measured cost of the deepest per-level recursion: a
    // parenthesised sub-expression costs ~15 native frames (~11 KB in a Debug
    // build, where frames are largest). Each level costs 2 counts here (parseExpr
    // + parseAssign), so 256 fires at ~128 nested parens -- and the 8 MB stack
    // reserve set in CMakeLists.txt covers that with room to spare (a 1 MB stack
    // would die at ~90). Keep the two in step if either changes.
    static constexpr size_t kMaxNestingDepth = 256;
    size_t m_depth = 0;

    // Counts nesting, not calls: a flat binary chain re-enters these functions
    // once per operand but never nests, so it must not trip the limit.
    struct DepthGuard {
        Parser& parser;
        explicit DepthGuard(Parser& p) : parser(p) {
            if (++parser.m_depth > kMaxNestingDepth) {
                --parser.m_depth;  // the constructor throws, so ~DepthGuard never runs
                parser.reportNestingTooDeep();
            }
        }
        ~DepthGuard() { --parser.m_depth; }
    };
    [[noreturn]] void reportNestingTooDeep();

    // token helpers
    Token& peek(size_t off = 0);
    bool check(Tk k);
    bool check(Tk k, size_t off);
    bool match(Tk k);
    Token& expect(Tk k);
    bool atEnd();
    void advance();
    SourceLocation curloc();

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
    std::vector<std::pair<std::string,TypePtr>> parseFnParams(std::optional<size_t>* varargIndex = nullptr,
                                                          std::optional<size_t>* kwargsIndex = nullptr);
    Path parsePath();
    Path parseTypePath();
    std::vector<TypePtr> parseGenericArgs();
    bool expectGt();
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
    ItemPtr parseTypeAlias(Visibility vis);
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
    bool tryAttachGenericArgs(ExprPtr& expr);

    // True when the token at `pos` opens a new source line. The constructor
    // drops every Tk::Newline, so comparing line numbers is the only way the
    // parser can still see a line break.
    bool atLineStart() const;
    ExprPtr parsePrimary();
    ExprPtr parseMapLiteral();
    bool looksLikeMapLiteral();
    ExprPtr parseBlockOrClosure();
    ExprPtr parseBlockBody();
    ExprPtr parseBlock();
    ExprPtr parseIf();
    ExprPtr parseWhile(std::optional<std::string> label);
    ExprPtr parseFor(std::optional<std::string> label);
    ExprPtr parseLoop(std::optional<std::string> label);
    ExprPtr parseWhen();
    ExprPtr parseBreak();
    ExprPtr parseContinue();
    ExprPtr parseReturn();
    ExprPtr parseThrow();
    ExprPtr parseTryCatch();
    ExprPtr parseStructExpr(Path path);
    ExprPtr parseLiteral();
    ExprPtr parseNull();
    // `tok` is the StringLit token that followed the `$`.
    ExprPtr parseStringTemplate(const Token& tok);

    // patterns
    PatPtr parsePat();

    // types
    TypePtr parseType();
};

} // namespace loxis::v2
