#include "Parser.hpp"
#include "Lexer.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace loxis::v2 {

namespace {

// Resolves the escape sequences in a string literal body. Shared by ordinary
// string literals and by the literal chunks of a string template -- the
// template path used to hand its raw text straight to the AST, so `$"a\nb"`
// carried a literal backslash-n into the output.
std::string unescapeString(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out.push_back(s[i]); continue; }
        char c = s[++i];
        switch (c) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '0': out.push_back('\0'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case '\'': out.push_back('\''); break;
            case '$': out.push_back('$'); break;
            case 'x': {
                if (i + 2 < s.size()) {
                    out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
                    i += 2;
                }
                break;
            }
            default: out.push_back(c); break;
        }
    }
    return out;
}

// Walks `count` characters of `text` forward from `loc`, so a fragment lexed on
// its own can start at a meaningful position. Treats both \n and \r as line
// breaks, matching Lexer::advance.
SourceLocation advanceLoc(SourceLocation loc, const std::string& text, size_t count) {
    for (size_t i = 0; i < count && i < text.size(); ++i) {
        if (text[i] == '\n' || text[i] == '\r') { loc.line++; loc.col = 1; }
        else loc.col++;
    }
    return loc;
}

} // namespace

// ---------- token helpers ----------
Token& Parser::peek(size_t off) { return toks[pos + off]; }
bool Parser::check(Tk k) { return pos < toks.size() && peek().kind == k; }
bool Parser::check(Tk k, size_t off) { return pos + off < toks.size() && toks[pos + off].kind == k; }
bool Parser::match(Tk k) { if (check(k)) { ++pos; return true; } return false; }
Token& Parser::expect(Tk k) {
    if (!check(k)) {
        error(std::string("expected ") + tkName(k));
        if (!atEnd()) return toks[pos++];
        return toks[pos];
    }
    return toks[pos++];
}
bool Parser::atEnd() { return check(Tk::Eof); }
void Parser::advance() { if (!atEnd()) ++pos; }
SourceLocation Parser::curloc() { return peek().loc; }

// ---------- basic helpers ----------
std::string Parser::expectIdent() {
    if (!check(Tk::Ident)) {
        error("expected identifier");
        if (!atEnd()) {
            ++pos;
        }
        return "";
    }
    std::string s = peek().lex;
    ++pos;
    return s;
}

bool Parser::isItemStart() {
    switch (peek().kind) {
        case Tk::KwFun: case Tk::KwVal: case Tk::KwVar: case Tk::KwConst:
        case Tk::KwClass: case Tk::KwInterface: case Tk::KwObject: case Tk::KwEnum:
        case Tk::KwImport: case Tk::KwFrom:
        case Tk::KwPublic: case Tk::KwInternal: case Tk::KwPrivate:
        case Tk::KwOpen: case Tk::KwAbstract: case Tk::KwData: case Tk::KwOverride:
            return true;
        default: return false;
    }
}

void Parser::skipToBoundary() {
    while (!atEnd()) {
        if (check(Tk::Semi) || check(Tk::RBrace)) { advance(); return; }
        if (isItemStart()) return;
        advance();
    }
}

void Parser::error(const std::string& msg) {
    if (!m_quietErrors) {
        const Token& found = peek();
        SourceLocation end = found.loc;
        end.col += static_cast<uint32_t>(std::max<size_t>(1, found.lex.size()));
        std::string label = msg;
        if (found.kind != Tk::Eof && !found.lex.empty()) label += ", found `" + found.lex + "`";
        throw FrontendError(Diagnostic{
            DiagnosticSeverity::Error, DiagnosticKind::Syntax, msg,
            LabeledSpan{SourceSpan{found.loc, end}, std::move(label)}
        });
    }
}

// Thrown instead of letting the recursion run out of native stack, which would
// abort the process with STATUS_STACK_OVERFLOW (0xC00000FD) and no output.
// Deliberately not routed through error(): that is a no-op while a speculative
// parse is running, and this has to unwind unconditionally.
[[noreturn]] void Parser::reportNestingTooDeep() {
    const Token& found = peek();
    SourceLocation end = found.loc;
    end.col += static_cast<uint32_t>(std::max<size_t>(1, found.lex.size()));
    throw FrontendError(Diagnostic{
        DiagnosticSeverity::Error, DiagnosticKind::Syntax,
        "expression is nested too deeply to parse",
        LabeledSpan{SourceSpan{found.loc, end},
            "stack overflow guard: nesting limit reached"}
    });
}

// ---------- constructor / entry ----------
Parser::Parser(std::vector<Token> tokens) {
    for (auto& t : tokens) {
        if (t.kind != Tk::Newline) toks.push_back(std::move(t));
    }
    if (toks.empty() || toks.back().kind != Tk::Eof)
        toks.emplace_back(Tk::Eof, "", SourceLocation{});
}

Module Parser::parseFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Lexer lex(src, path);
    Parser p(lex.tokenize());
    Module m = p.parse();
    size_t s = path.find_last_of("/\\");
    std::string name = (s == std::string::npos) ? path : path.substr(s + 1);
    size_t d = name.find_last_of('.');
    if (d != std::string::npos) name = name.substr(0, d);
    m.name = name;
    return m;
}

Module Parser::parse() {
    Module mod;
    mod.loc = curloc();
    if (match(Tk::KwPackage)) {
        mod.packageName = parseDottedPath();
        match(Tk::Semi);
    }
    while (!atEnd()) {
        size_t prev = pos;
        if (auto item = parseItem()) {
            mod.items.push_back(item);
        } else {
            if (pos == prev) advance();
            skipToBoundary();
        }
    }
    return mod;
}

// ---------- generic helpers ----------
std::vector<GenericParam> Parser::parseGenerics() {
    std::vector<GenericParam> out;
    if (!match(Tk::Lt)) return out;
    while (!check(Tk::Gt) && !check(Tk::Shr) && !atEnd()) {
        GenericParam gp;
        if (match(Tk::KwConst)) {
            gp.kind = GenericParam::Const;
            gp.name = expectIdent();
            expect(Tk::Colon);
            (void)parseType();
        } else {
            gp.kind = GenericParam::Type;
            gp.name = expectIdent();
        }
        if (match(Tk::Eq)) gp.default_ = parseType();
        out.push_back(std::move(gp));
        if (!match(Tk::Comma)) break;
    }
    expectGt();
    return out;
}

std::vector<WhereBound> Parser::parseWhere() {
    std::vector<WhereBound> out;
    if (!match(Tk::KwWhere)) return out;
    while (!atEnd() && !check(Tk::LBrace) && !check(Tk::Semi)) {
        TypePtr ty = parseType();
        expect(Tk::Colon);
        Path trait = parseTypePath();
        out.push_back({ty, trait});
        if (!match(Tk::Comma)) break;
    }
    return out;
}

TypePtr Parser::parseRet() {
    if (match(Tk::Arrow)) return parseType();
    if (match(Tk::Colon)) {
        return parseType();
    }
    return nullptr;
}

std::vector<std::pair<std::string,TypePtr>> Parser::parseFnParams(std::optional<size_t>* varargIndex,
                                                                            std::optional<size_t>* kwargsIndex) {
    std::vector<std::pair<std::string,TypePtr>> out;
    expect(Tk::LParen);
    while (!check(Tk::RParen) && !atEnd()) {
        const bool hasStar = match(Tk::Star);
        const bool isKwargs = hasStar && match(Tk::Star);
        const bool isVararg = hasStar && !isKwargs;
        if (isVararg) {
            if (!varargIndex) error("'*' parameter is not allowed here");
            if (varargIndex && *varargIndex) error("only one '*' parameter is allowed");
            if (varargIndex) *varargIndex = out.size();
        }
        if (isKwargs) {
            if (!kwargsIndex) error("'**' parameter is not allowed here");
            if (kwargsIndex && *kwargsIndex) error("only one '**' parameter is allowed");
            if (kwargsIndex) *kwargsIndex = out.size();
        }
        std::string name = expectIdent();
        for (const auto& param : out) if (param.first == name) error("duplicate parameter '" + name + "'");
        expect(Tk::Colon);
        TypePtr ty = parseType();
        out.emplace_back(name, ty);
        if (!match(Tk::Comma)) break;
        if (isKwargs && !check(Tk::RParen)) error("'**' parameter must be last");
        if (isVararg && !check(Tk::RParen) && !(check(Tk::Star) && check(Tk::Star, 1)))
            error("only a '**' parameter may follow a '*' parameter");
    }
    expect(Tk::RParen);
    return out;
}

std::vector<ParamDecl> Parser::parseClassParams() {
    std::vector<ParamDecl> out;
    expect(Tk::LParen);
    while (!check(Tk::RParen) && !atEnd()) {
        ParamDecl pd;
        if (match(Tk::KwVal)) {
            pd.isVal = true;
        } else if (match(Tk::KwVar)) {
            pd.isVar = true;
        }
        pd.name = expectIdent();
        expect(Tk::Colon);
        pd.ty = parseType();
        if (match(Tk::Eq)) pd.default_ = parseExpr();
        out.push_back(std::move(pd));
        if (!match(Tk::Comma)) break;
    }
    expect(Tk::RParen);
    return out;
}

Path Parser::parsePath() {
    Path p;
    if (check(Tk::Ident)) {
        p.segs.push_back(expectIdent());
    } else {
        error("expected identifier in path");
        return p;
    }
    while (match(Tk::DColon)) {
        if (check(Tk::Lt)) {
            auto args = parseGenericArgs();
            p.args.insert(p.args.end(), args.begin(), args.end());
        } else {
            p.segs.push_back(expectIdent());
        }
    }
    if (check(Tk::Lt)) {
        size_t save = pos;
        m_quietErrors = true;
        auto args = parseGenericArgs();
        m_quietErrors = false;
        if (args.empty()) {
            pos = save; // backtrack: < was comparison, not generic
        } else {
            p.args.insert(p.args.end(), args.begin(), args.end());
        }
    }
    return p;
}

Path Parser::parseTypePath() {
    Path p;
    p.segs.push_back(expectIdent());
    while (true) {
        if (match(Tk::DColon)) {
            if (check(Tk::Lt)) {
                auto args = parseGenericArgs();
                p.args.insert(p.args.end(), args.begin(), args.end());
            } else {
                p.segs.push_back(expectIdent());
            }
        } else if (check(Tk::Lt)) {
            auto args = parseGenericArgs();
            p.args.insert(p.args.end(), args.begin(), args.end());
        } else {
            break;
        }
    }
    return p;
}

std::vector<TypePtr> Parser::parseGenericArgs() {
    std::vector<TypePtr> out;
    expect(Tk::Lt);
    while (!check(Tk::Gt) && !check(Tk::Shr) && !check(Tk::Ge) && !atEnd()) {
        out.push_back(parseType());
        if (!match(Tk::Comma)) break;
    }
    if (!expectGt()) {
        return {}; // failure → caller should backtrack
    }
    return out;
}

bool Parser::expectGt() {
    if (match(Tk::Gt)) return true;
    if (check(Tk::Shr)) {
        Token t = peek();
        t.kind = Tk::Gt;
        t.lex = ">";
        toks[pos] = t;
        toks.insert(toks.begin() + pos + 1, Token{Tk::Gt, ">", t.loc});
        advance();
        return true;
    }
    error("expected '>'");
    return false;
}

Visibility Parser::parseVisibility() {
    if (match(Tk::KwPublic)) return Visibility::Public;
    if (match(Tk::KwPrivate)) return Visibility::Private;
    if (match(Tk::KwInternal)) return Visibility::Internal;
    return Visibility::Internal;
}

Path Parser::parseDottedPath() {
    Path p;
    p.segs.push_back(expectIdent());
    while (match(Tk::Dot)) {
        p.segs.push_back(expectIdent());
    }
    return p;
}

ClassModifier Parser::parseClassModifier() {
    if (match(Tk::KwOpen)) return ClassModifier::Open;
    if (match(Tk::KwAbstract)) return ClassModifier::Abstract;
    if (match(Tk::KwData)) return ClassModifier::Data;
    return ClassModifier::None;
}

// ---------- items ----------
ItemPtr Parser::parseItem() {
    if (check(Tk::KwImport)) return parseImport();
    if (check(Tk::KwFrom)) return parseFromImport();

    Visibility visibility = parseVisibility();
    ClassModifier modifier = parseClassModifier();
    // re-parse visibility if modifiers were in different order
    if (visibility == Visibility::Internal) visibility = parseVisibility();

    if (check(Tk::KwFun)) return std::make_shared<Item>(parseFun(visibility));
    if (check(Tk::KwVal)) return parseVal(visibility);
    if (check(Tk::KwVar)) return parseVar(visibility);
    if (check(Tk::KwConst)) return parseConst(visibility);
    if (check(Tk::KwClass)) return parseClass(visibility, modifier);
    if (check(Tk::KwInterface)) return parseInterface(visibility);
    if (check(Tk::KwObject)) return parseObject(visibility);
    if (check(Tk::KwEnum)) return parseEnumClass(visibility);
    if (check(Tk::KwType)) return parseTypeAlias(visibility);

    error("expected item");
    return nullptr;
}

// ---------- function ----------
ItemFun Parser::parseFun(Visibility vis) {
    expect(Tk::KwFun);

    // Check for extension function: fun Type.name(...) or just fun name(...)
    TypePtr receiverType = nullptr;
    std::string funcName;
    std::string first = expectIdent();

    if (match(Tk::Dot)) {
        // Extension function — first identifier is the receiver type
        Path recvPath;
        recvPath.segs.push_back(first);
        receiverType = std::make_shared<Type>(AstTyPath{recvPath});
        funcName = expectIdent();
    } else {
        funcName = first;
    }

    if (funcName == "operator") {
        if (match(Tk::Plus)) funcName = "operator+";
        else if (match(Tk::Minus)) funcName = "operator-";
        else if (match(Tk::Star)) funcName = "operator*";
        else if (match(Tk::Slash)) funcName = "operator/";
        else if (match(Tk::Percent)) funcName = "operator%";
        else if (match(Tk::EqEq)) funcName = "operator==";
        else if (match(Tk::Ne)) funcName = "operator!=";
        else if (match(Tk::Le)) funcName = "operator<=";
        else if (match(Tk::Lt)) funcName = "operator<";
        else if (match(Tk::Ge)) funcName = "operator>=";
        else if (match(Tk::Gt)) funcName = "operator>";
        else if (match(Tk::Bang)) funcName = "operator!";
        else error("expected operator token after 'operator'");
    }
    auto generics = parseGenerics();
    std::optional<size_t> varargIndex;
    std::optional<size_t> kwargsIndex;
    auto params = parseFnParams(&varargIndex, &kwargsIndex);
    TypePtr ret = parseRet();
    auto where_ = parseWhere();
    ExprPtr body = nullptr;
    if (match(Tk::Semi)) {
        // abstract or declaration only
    } else if (match(Tk::Eq)) {
        body = parseExpr();
    } else if (check(Tk::LBrace)) {
        body = parseBlock();
    }
    // If none matched, it's an abstract method with no body
    ItemFun result{funcName, vis, std::move(generics), std::move(params), ret, std::move(where_), body,
                   false, false, false, "", receiverType, "this"};
    result.varargIndex = varargIndex;
    result.kwargsIndex = kwargsIndex;
    return result;
}

// ---------- class ----------
ItemPtr Parser::parseClass(Visibility vis, ClassModifier mod) {
    expect(Tk::KwClass);
    std::string name = expectIdent();
    auto generics = parseGenerics();

    // primary constructor params
    std::vector<ParamDecl> primaryCtor;
    if (check(Tk::LParen)) {
        primaryCtor = parseClassParams();
    }

    // superclass + interfaces
    std::optional<Path> superClass;
    std::vector<ExprPtr> superClassArgs;
    std::vector<Path> interfaces;

    if (match(Tk::Colon)) {
        // first could be superclass (with optional args) or interface
        Path first = parseTypePath();
        if (check(Tk::LParen)) {
            // superclass with constructor args
            superClass = first;
            expect(Tk::LParen);
            while (!check(Tk::RParen) && !atEnd()) {
                superClassArgs.push_back(parseExpr());
                if (!match(Tk::Comma)) break;
            }
            expect(Tk::RParen);
        } else {
            interfaces.push_back(first);
        }
        // remaining are interfaces
        while (match(Tk::Comma)) {
            interfaces.push_back(parseTypePath());
        }
    }

    auto where_ = parseWhere();

    std::vector<FieldDecl> fields;
    std::vector<ItemFun> methods;
    std::vector<StmtPtr> initBlocks;

    if (match(Tk::LBrace)) {
        while (!check(Tk::RBrace) && !atEnd()) {
            // method modifiers: open, abstract, override
            bool isOpen = false, isAbstract = false, isOverride = false;
            while (match(Tk::KwOpen)) isOpen = true;
            while (match(Tk::KwAbstract)) isAbstract = true;
            while (match(Tk::KwOverride)) isOverride = true;

            if (check(Tk::KwVal) || check(Tk::KwVar)) {
                if (isOpen || isAbstract || isOverride) error("modifiers not applicable to field");
                FieldDecl fd;
                fd.isVal = match(Tk::KwVal);
                if (!fd.isVal) { match(Tk::KwVar); fd.isVal = false; }
                fd.name = expectIdent();
                expect(Tk::Colon);
                fd.ty = parseType();
                fd.initializer = nullptr;
                if (match(Tk::Eq)) fd.initializer = parseExpr();
                fields.push_back(std::move(fd));
            } else if (check(Tk::KwFun)) {
                Visibility mvis = parseVisibility();
                ItemFun m = parseFun(mvis);
                m.isOpen = isOpen;
                m.isAbstract = isAbstract;
                m.isOverride = isOverride;
                methods.push_back(std::move(m));
            } else if (match(Tk::KwInit)) {
                if (isOpen || isAbstract || isOverride) error("modifiers not applicable to init");
                initBlocks.push_back(std::make_shared<Stmt>(StmtExpr{parseBlock(), false}));
            } else {
                error("expected val, var, fun, or init in class body");
                skipToBoundary();
            }
        }
        expect(Tk::RBrace);
    }

    if (mod == ClassModifier::Data) {
        auto makeType = [](const std::string& typeName) -> TypePtr {
            return std::make_shared<Type>(AstTyPath{Path{{typeName}, {}}});
        };
        auto makePathExpr = [](const std::string& pathName) -> ExprPtr {
            return std::make_shared<Expr>(ExprPath{Path{{pathName}, {}}});
        };
        auto makeCall = [&](const std::string& callee, std::vector<ExprPtr> args) -> ExprPtr {
            return std::make_shared<Expr>(ExprCall{makePathExpr(callee), std::move(args)});
        };
        auto hasMethod = [&](const std::string& methodName) {
            return std::any_of(methods.begin(), methods.end(), [&](const ItemFun& method) {
                return method.name == methodName;
            });
        };
        auto addMethod = [&](const std::string& methodName,
                             std::vector<std::pair<std::string, TypePtr>> params,
                             TypePtr ret, ExprPtr body) {
            if (hasMethod(methodName)) return;
            methods.push_back(ItemFun{methodName, Visibility::Public, {}, std::move(params),
                std::move(ret), {}, std::move(body), false, false, false, "", nullptr, "this"});
        };
        auto fieldTypeName = [](const TypePtr& ty) -> std::string {
            if (!ty) return {};
            if (auto* pathTy = std::get_if<AstTyPath>(ty.get())) {
                if (!pathTy->path.segs.empty()) return pathTy->path.segs.back();
            }
            return {};
        };

        std::vector<ParamDecl> properties;
        for (const auto& param : primaryCtor) {
            if (param.isVal || param.isVar) properties.push_back(param);
        }
        if (properties.empty()) error("data class requires at least one val/var constructor property");

        for (size_t i = 0; i < properties.size(); ++i) {
            addMethod("component" + std::to_string(i + 1), {}, properties[i].ty,
                      makePathExpr(properties[i].name));
        }

        std::vector<std::pair<std::string, TypePtr>> copyParams;
        std::vector<ExprPtr> copyArgs;
        for (const auto& property : properties) {
            copyParams.push_back({property.name, property.ty});
            copyArgs.push_back(makePathExpr(property.name));
        }
        addMethod("copy", std::move(copyParams), makeType(name), makeCall(name, std::move(copyArgs)));

        auto makeEqualsBody = [&]() -> ExprPtr {
            ExprPtr result = std::make_shared<Expr>(ExprLit{true});
            for (auto it = properties.rbegin(); it != properties.rend(); ++it) {
                ExprPtr lhs = makePathExpr(it->name);
                ExprPtr rhs = std::make_shared<Expr>(ExprField{makePathExpr("other"), it->name});
                ExprPtr equal = std::make_shared<Expr>(ExprBinary{ExprBinary::Eq, lhs, rhs});
                result = std::make_shared<Expr>(ExprIf{
                    equal, result, std::make_shared<Expr>(ExprLit{false})
                });
            }
            return result;
        };
        std::vector<std::pair<std::string, TypePtr>> equalsParams{{"other", makeType(name)}};
        addMethod("equals", equalsParams, makeType("bool"), makeEqualsBody());
        addMethod("operator==", std::move(equalsParams), makeType("bool"), makeEqualsBody());

        ExprPtr hash = std::make_shared<Expr>(ExprLit{static_cast<int64_t>(1)});
        for (const auto& property : properties) {
            ExprPtr value = makePathExpr(property.name);
            const std::string typeName = fieldTypeName(property.ty);
            if (typeName == "str") value = makeCall("strlen", {value});
            else if (typeName == "float" || typeName == "double") value = makeCall("ftoi", {value});
            ExprPtr multiplied = std::make_shared<Expr>(ExprBinary{
                ExprBinary::Mul, hash, std::make_shared<Expr>(ExprLit{static_cast<int64_t>(31)})
            });
            hash = std::make_shared<Expr>(ExprBinary{ExprBinary::Add, multiplied, value});
        }
        addMethod("hashCode", {}, makeType("int"), hash);

        ExprPtr text = std::make_shared<Expr>(ExprLit{name + "("});
        for (size_t i = 0; i < properties.size(); ++i) {
            const auto& property = properties[i];
            std::string prefix = (i == 0 ? "" : ", ") + property.name + "=";
            text = std::make_shared<Expr>(ExprBinary{
                ExprBinary::Add, text, std::make_shared<Expr>(ExprLit{prefix})
            });
            ExprPtr value = makePathExpr(property.name);
            const std::string typeName = fieldTypeName(property.ty);
            if (typeName != "str") {
                if (typeName == "float" || typeName == "double") value = makeCall("ftoi", {value});
                value = makeCall("i2s", {value});
            }
            text = std::make_shared<Expr>(ExprBinary{ExprBinary::Add, text, value});
        }
        text = std::make_shared<Expr>(ExprBinary{
            ExprBinary::Add, text, std::make_shared<Expr>(ExprLit{std::string(")")})
        });
        addMethod("toString", {}, makeType("str"), text);
    }

    auto item = ItemClass{name, vis, mod, std::move(generics), std::move(primaryCtor),
                          superClass, std::move(superClassArgs), std::move(interfaces),
                          std::move(where_), std::move(fields), std::move(methods), std::move(initBlocks)};
    return std::make_shared<Item>(std::move(item));
}

// ---------- interface ----------
ItemPtr Parser::parseInterface(Visibility vis) {
    expect(Tk::KwInterface);
    std::string name = expectIdent();
    auto generics = parseGenerics();

    std::vector<Path> supers;
    if (match(Tk::Colon)) {
        while (!atEnd()) {
            supers.push_back(parseTypePath());
            if (!match(Tk::Comma)) break;
        }
    }

    expect(Tk::LBrace);
    std::vector<InterfaceMethodDecl> methods;
    while (!check(Tk::RBrace) && !atEnd()) {
        bool isOverride = match(Tk::KwOverride);
        if (!check(Tk::KwFun)) { error("expected fun in interface"); skipToBoundary(); continue; }
        advance();
        std::string mname = expectIdent();
        std::optional<size_t> varargIndex;
        std::optional<size_t> kwargsIndex;
        auto params = parseFnParams(&varargIndex, &kwargsIndex);
        TypePtr ret = parseRet();
        ExprPtr def = nullptr;
        if (match(Tk::Semi)) {
            // abstract — no body
        } else if (match(Tk::Eq)) {
            def = parseExpr();
        } else if (check(Tk::LBrace)) {
            def = parseBlock();
        }
        // If none matched, it's an abstract method with no body
        InterfaceMethodDecl method{mname, std::move(params), ret, def};
        method.varargIndex = varargIndex;
        method.kwargsIndex = kwargsIndex;
        methods.push_back(std::move(method));
    }
    expect(Tk::RBrace);
    return std::make_shared<Item>(ItemInterface{name, vis, std::move(generics), std::move(supers), std::move(methods)});
}

// ---------- enum class ----------
ItemPtr Parser::parseEnumClass(Visibility vis) {
    expect(Tk::KwEnum);
    expect(Tk::KwClass);
    std::string name = expectIdent();
    auto generics = parseGenerics();

    std::vector<Path> interfaces;
    if (match(Tk::Colon)) {
        while (!atEnd()) {
            interfaces.push_back(parseTypePath());
            if (!match(Tk::Comma)) break;
        }
    }

    expect(Tk::LBrace);
    std::vector<EnumVariant> variants;
    std::vector<FieldDecl> properties;
    std::vector<ItemFun> methods;

    while (!check(Tk::RBrace) && !atEnd()) {
        bool isOpen = false, isAbstract = false, isOverride = false;
        while (match(Tk::KwOpen)) isOpen = true;
        while (match(Tk::KwAbstract)) isAbstract = true;
        while (match(Tk::KwOverride)) isOverride = true;

        if (check(Tk::KwFun)) {
            Visibility mvis = parseVisibility();
            ItemFun m = parseFun(mvis);
            m.isOpen = isOpen;
            m.isAbstract = isAbstract;
            m.isOverride = isOverride;
            methods.push_back(std::move(m));
        } else if (check(Tk::KwVal) || check(Tk::KwVar)) {
            FieldDecl fd;
            fd.isVal = match(Tk::KwVal);
            if (!fd.isVal) { match(Tk::KwVar); fd.isVal = false; }
            fd.name = expectIdent();
            expect(Tk::Colon);
            fd.ty = parseType();
            fd.initializer = nullptr;
            if (match(Tk::Eq)) fd.initializer = parseExpr();
            properties.push_back(std::move(fd));
        } else if (check(Tk::Ident)) {
            EnumVariant v;
            v.name = expectIdent();
            if (check(Tk::LParen)) {
                v.isTuple = true;
                expect(Tk::LParen);
                while (!check(Tk::RParen) && !atEnd()) {
                    TypePtr ty = parseType();
                    v.fields.push_back({"", ty});
                    if (!match(Tk::Comma)) break;
                }
                expect(Tk::RParen);
            } else if (check(Tk::LBrace)) {
                v.isTuple = false;
                expect(Tk::LBrace);
                while (!check(Tk::RBrace) && !atEnd()) {
                    std::string fn = expectIdent();
                    expect(Tk::Colon);
                    TypePtr ty = parseType();
                    v.fields.push_back({fn, ty});
                    if (!match(Tk::Comma)) break;
                }
                expect(Tk::RBrace);
            }
            variants.push_back(std::move(v));
            if (!match(Tk::Comma)) break;
        } else {
            error("expected enum variant, fun, val, or var");
            skipToBoundary();
        }
    }
    expect(Tk::RBrace);

    return std::make_shared<Item>(ItemEnumClass{name, vis, std::move(generics), std::move(interfaces),
                                                std::move(variants), std::move(properties), std::move(methods)});
}

// ---------- object (singleton) ----------
ItemPtr Parser::parseObject(Visibility vis) {
    expect(Tk::KwObject);
    std::string name = expectIdent();

    std::optional<Path> superClass;
    std::vector<Path> interfaces;

    if (match(Tk::Colon)) {
        Path first = parseTypePath();
        if (check(Tk::LParen)) {
            superClass = first;
            expect(Tk::LParen);
            while (!check(Tk::RParen)) advance();
            expect(Tk::RParen);
        } else {
            interfaces.push_back(first);
        }
        while (match(Tk::Comma)) {
            interfaces.push_back(parseTypePath());
        }
    }

    std::vector<FieldDecl> fields;
    std::vector<ItemFun> methods;
    std::vector<StmtPtr> initBlocks;

    if (match(Tk::LBrace)) {
        while (!check(Tk::RBrace) && !atEnd()) {
            bool isOpen = false, isAbstract = false, isOverride = false;
            while (match(Tk::KwOpen)) isOpen = true;
            while (match(Tk::KwAbstract)) isAbstract = true;
            while (match(Tk::KwOverride)) isOverride = true;

            if (check(Tk::KwVal) || check(Tk::KwVar)) {
                if (isOpen || isAbstract || isOverride) error("modifiers not applicable to field");
                FieldDecl fd;
                fd.isVal = match(Tk::KwVal);
                if (!fd.isVal) { match(Tk::KwVar); fd.isVal = false; }
                fd.name = expectIdent();
                expect(Tk::Colon);
                fd.ty = parseType();
                fd.initializer = nullptr;
                if (match(Tk::Eq)) fd.initializer = parseExpr();
                fields.push_back(std::move(fd));
            } else if (check(Tk::KwFun)) {
                Visibility mvis = parseVisibility();
                ItemFun m = parseFun(mvis);
                m.isOpen = isOpen;
                m.isAbstract = isAbstract;
                m.isOverride = isOverride;
                methods.push_back(std::move(m));
            } else if (match(Tk::KwInit)) {
                if (isOpen || isAbstract || isOverride) error("modifiers not applicable to init");
                initBlocks.push_back(std::make_shared<Stmt>(StmtExpr{parseBlock(), false}));
            } else {
                error("expected val, var, fun, or init in object body");
                skipToBoundary();
            }
        }
        expect(Tk::RBrace);
    }

    return std::make_shared<Item>(ItemObject{name, vis, superClass, std::move(interfaces),
                                             std::move(fields), std::move(methods), std::move(initBlocks)});
}

// ---------- val / var / const ----------
ItemPtr Parser::parseVal(Visibility vis) {
    expect(Tk::KwVal);
    std::string name = expectIdent();
    TypePtr ty = nullptr;
    if (match(Tk::Colon)) ty = parseType();
    ExprPtr init = nullptr;
    if (match(Tk::Eq)) init = parseExpr();
    return std::make_shared<Item>(ItemVal{name, vis, ty, init});
}

ItemPtr Parser::parseVar(Visibility vis) {
    expect(Tk::KwVar);
    std::string name = expectIdent();
    TypePtr ty = nullptr;
    if (match(Tk::Colon)) ty = parseType();
    ExprPtr init = nullptr;
    if (match(Tk::Eq)) init = parseExpr();
    return std::make_shared<Item>(ItemVar{name, vis, ty, init});
}

ItemPtr Parser::parseConst(Visibility vis) {
    expect(Tk::KwConst);
    std::string name = expectIdent();
    expect(Tk::Colon);
    TypePtr ty = parseType();
    expect(Tk::Eq);
    ExprPtr init = parseExpr();
    return std::make_shared<Item>(ItemConst{name, vis, ty, init});
}

// ---------- type alias ----------
ItemPtr Parser::parseTypeAlias(Visibility vis) {
    expect(Tk::KwType);
    std::string name = expectIdent();
    expect(Tk::Eq);
    TypePtr underlying = parseType();
    return std::make_shared<Item>(ItemTypeAlias{name, vis, underlying});
}

// ---------- imports ----------
ItemPtr Parser::parseImport() {
    expect(Tk::KwImport);
    Path module = parseDottedPath();
    std::vector<ImportItem> items;
    if (match(Tk::KwAs)) {
        items.push_back({module.segs.back(), expectIdent()});
    }
    match(Tk::Semi);
    return std::make_shared<Item>(ItemImport{std::move(module), std::move(items), true});
}

ItemPtr Parser::parseFromImport() {
    expect(Tk::KwFrom);
    Path module = parseDottedPath();
    expect(Tk::KwImport);

    std::vector<ImportItem> items;
    if (match(Tk::LBrace)) {
        while (!check(Tk::RBrace) && !atEnd()) {
            std::string name = expectIdent();
            std::optional<std::string> alias;
            if (match(Tk::KwAs)) alias = expectIdent();
            items.push_back({name, alias});
            if (!match(Tk::Comma)) break;
        }
        expect(Tk::RBrace);
    } else {
        while (!check(Tk::Semi) && !atEnd()) {
            std::string name = expectIdent();
            std::optional<std::string> alias;
            if (match(Tk::KwAs)) alias = expectIdent();
            items.push_back({name, alias});
            if (!match(Tk::Comma)) break;
        }
    }
    match(Tk::Semi);
    return std::make_shared<Item>(ItemImport{std::move(module), std::move(items), false});
}

// ---------- statements ----------
StmtPtr Parser::parseStmt() {
    if (check(Tk::KwLet)) return parseLet();
    if (check(Tk::KwVal) || check(Tk::KwVar)) {
        // local val/var — parse as let-like
        bool isVal = match(Tk::KwVal);
        if (!isVal) match(Tk::KwVar);
        std::string name = expectIdent();
        TypePtr ty = nullptr;
        if (match(Tk::Colon)) ty = parseType();
        ExprPtr init = nullptr;
        if (match(Tk::Eq)) init = parseExpr();

        PatPtr pat = std::make_shared<Pat>(PatBind{name, !isVal, nullptr});
        return std::make_shared<Stmt>(StmtLet{pat, ty, init});
    }
    if (isItemStart()) return parseItemStmt();
    ExprPtr e = parseExpr();
    if (match(Tk::Semi)) return std::make_shared<Stmt>(StmtExpr{e, true});
    return std::make_shared<Stmt>(StmtExpr{e, false});
}

StmtPtr Parser::parseLet() {
    expect(Tk::KwLet);
    PatPtr pat = parsePat();
    TypePtr ty = nullptr;
    if (match(Tk::Colon)) ty = parseType();
    ExprPtr init = nullptr;
    if (match(Tk::Eq)) init = parseExpr();
    return std::make_shared<Stmt>(StmtLet{pat, ty, init});
}

StmtPtr Parser::parseItemStmt() {
    ItemPtr it = parseItem();
    return std::make_shared<Stmt>(StmtItem{it});
}

// ---------- expressions ----------
ExprPtr Parser::parseExpr() {
    DepthGuard guard(*this);
    SourceLocation start = curloc();
    ExprPtr expr = parseAssign();
    if (expr && expr->loc.file.empty()) {
        expr->loc = std::move(start);
        expr->endLoc = expr->loc;
        if (pos > 0) {
            const Token& last = toks[pos - 1];
            expr->endLoc = last.loc;
            expr->endLoc.col += static_cast<uint32_t>(std::max<size_t>(1, last.lex.size()));
        }
    }
    return expr;
}

ExprPtr Parser::parseAssign() {
    DepthGuard guard(*this);
    ExprPtr lhs = parseElvis();
    ExprAssign::Op op = ExprAssign::Set;
    bool ok = false;
    if (match(Tk::Eq)) { ok = true; op = ExprAssign::Set; }
    else if (match(Tk::PlusEq)) { ok = true; op = ExprAssign::AddEq; }
    else if (match(Tk::MinusEq)) { ok = true; op = ExprAssign::SubEq; }
    else if (match(Tk::StarEq)) { ok = true; op = ExprAssign::MulEq; }
    else if (match(Tk::SlashEq)) { ok = true; op = ExprAssign::DivEq; }
    else if (match(Tk::PercentEq)) { ok = true; op = ExprAssign::RemEq; }
    else if (match(Tk::AmpEq)) { ok = true; op = ExprAssign::AmpEq; }
    else if (match(Tk::PipeEq)) { ok = true; op = ExprAssign::PipeEq; }
    else if (match(Tk::CaretEq)) { ok = true; op = ExprAssign::CaretEq; }
    else if (match(Tk::ShlEq)) { ok = true; op = ExprAssign::ShlEq; }
    else if (match(Tk::ShrEq)) { ok = true; op = ExprAssign::ShrEq; }
    if (ok) {
        ExprPtr rhs = parseAssign();
        return std::make_shared<Expr>(ExprAssign{op, lhs, rhs});
    }
    return lhs;
}

ExprPtr Parser::parseElvis() {
    ExprPtr lhs = parseRange();
    while (match(Tk::Question)) {
        if (match(Tk::Colon)) {
            // a ?: b  (elvis)
            ExprPtr rhs = parseRange();
            lhs = std::make_shared<Expr>(ExprElvis{lhs, rhs});
        } else {
            // a? (try/error propagation)
            lhs = std::make_shared<Expr>(ExprTry{lhs});
        }
    }
    return lhs;
}

ExprPtr Parser::parseRange() {
    ExprPtr lhs = parseOr();
    if (match(Tk::DotDot)) {
        ExprPtr rhs = parseOr();
        return std::make_shared<Expr>(ExprRange{lhs, rhs, false});
    }
    if (match(Tk::DotDotEq)) {
        ExprPtr rhs = parseOr();
        return std::make_shared<Expr>(ExprRange{lhs, rhs, true});
    }
    return lhs;
}

ExprPtr Parser::parseOr() {
    ExprPtr lhs = parseAnd();
    while (match(Tk::OrOr)) {
        ExprPtr rhs = parseAnd();
        lhs = std::make_shared<Expr>(ExprBinary{ExprBinary::Or, lhs, rhs});
    }
    return lhs;
}

ExprPtr Parser::parseAnd() {
    ExprPtr lhs = parseCmp();
    while (match(Tk::AndAnd)) {
        ExprPtr rhs = parseCmp();
        lhs = std::make_shared<Expr>(ExprBinary{ExprBinary::And, lhs, rhs});
    }
    return lhs;
}

ExprPtr Parser::parseCmp() {
    ExprPtr lhs = parseBitOr();
    while (true) {
        // NOTE: tryAttachGenericArgs disabled in comparisons — < is always comparison here.
        // Generic args in expressions (like foo<Int>(x)) use parsePostfix path instead.
        ExprBinary::Op op;
        if (match(Tk::EqEq)) op = ExprBinary::Eq;
        else if (match(Tk::Ne)) op = ExprBinary::Ne;
        else if (match(Tk::Lt)) op = ExprBinary::Lt;
        else if (match(Tk::Gt)) op = ExprBinary::Gt;
        else if (match(Tk::Le)) op = ExprBinary::Le;
        else if (match(Tk::Ge)) op = ExprBinary::Ge;
        else if (check(Tk::KwIs)) {
            match(Tk::KwIs);
            if (match(Tk::Bang)) {
                TypePtr ty = parseType();
                lhs = std::make_shared<Expr>(ExprIsNot{lhs, ty});
            } else {
                TypePtr ty = parseType();
                lhs = std::make_shared<Expr>(ExprIs{lhs, ty});
            }
            continue;
        } else if (match(Tk::Bang) && check(Tk::KwIs)) {
            expect(Tk::KwIs);
            TypePtr ty = parseType();
            lhs = std::make_shared<Expr>(ExprIsNot{lhs, ty});
            continue;
        } else if (match(Tk::KwIn)) {
            ExprPtr rhs = parseBitOr();
            lhs = std::make_shared<Expr>(ExprBinary{static_cast<ExprBinary::Op>(92), lhs, rhs});
            continue;
        } else if (match(Tk::KwAs)) {
            TypePtr ty = parseType();
            lhs = std::make_shared<Expr>(ExprCast{lhs, ty});
            continue;
        } else break;
        ExprPtr rhs = parseBitOr();
        lhs = std::make_shared<Expr>(ExprBinary{op, lhs, rhs});
    }
    return lhs;
}

ExprPtr Parser::parseBitOr() {
    ExprPtr lhs = parseBitXor();
    while (match(Tk::Pipe)) {
        ExprPtr rhs = parseBitXor();
        lhs = std::make_shared<Expr>(ExprBinary{ExprBinary::BitOr, lhs, rhs});
    }
    return lhs;
}

ExprPtr Parser::parseBitXor() {
    ExprPtr lhs = parseBitAnd();
    while (match(Tk::Caret)) {
        ExprPtr rhs = parseBitAnd();
        lhs = std::make_shared<Expr>(ExprBinary{ExprBinary::BitXor, lhs, rhs});
    }
    return lhs;
}

ExprPtr Parser::parseBitAnd() {
    ExprPtr lhs = parseShift();
    while (match(Tk::Amp)) {
        ExprPtr rhs = parseShift();
        lhs = std::make_shared<Expr>(ExprBinary{ExprBinary::BitAnd, lhs, rhs});
    }
    return lhs;
}

ExprPtr Parser::parseShift() {
    ExprPtr lhs = parseAdd();
    while (true) {
        ExprBinary::Op op;
        if (match(Tk::Shl)) op = ExprBinary::Shl;
        else if (match(Tk::Shr)) op = ExprBinary::Shr;
        else break;
        ExprPtr rhs = parseAdd();
        lhs = std::make_shared<Expr>(ExprBinary{op, lhs, rhs});
    }
    return lhs;
}

ExprPtr Parser::parseAdd() {
    ExprPtr lhs = parseMul();
    while (true) {
        ExprBinary::Op op;
        if (match(Tk::Plus)) op = ExprBinary::Add;
        else if (match(Tk::Minus)) op = ExprBinary::Sub;
        else break;
        ExprPtr rhs = parseMul();
        lhs = std::make_shared<Expr>(ExprBinary{op, lhs, rhs});
    }
    return lhs;
}

ExprPtr Parser::parseMul() {
    ExprPtr lhs = parsePrefix();
    while (true) {
        ExprBinary::Op op;
        if (match(Tk::Star)) op = ExprBinary::Mul;
        else if (match(Tk::Slash)) op = ExprBinary::Div;
        else if (match(Tk::Percent)) op = ExprBinary::Rem;
        else break;
        ExprPtr rhs = parsePrefix();
        lhs = std::make_shared<Expr>(ExprBinary{op, lhs, rhs});
    }
    return lhs;
}

ExprPtr Parser::parsePrefix() {
    if (check(Tk::Bang)) {
        if (check(Tk::KwIs, 1)) {
            return parsePostfix();
        }
        advance(); // consume !
        ExprPtr op = parsePrefix();
        return std::make_shared<Expr>(ExprUnary{ExprUnary::Not, op});
    }
    if (match(Tk::Minus)) {
        ExprPtr op = parsePrefix();
        return std::make_shared<Expr>(ExprUnary{ExprUnary::Neg, op});
    }
    if (match(Tk::Star)) {
        ExprPtr op = parsePrefix();
        return std::make_shared<Expr>(ExprUnary{ExprUnary::Deref, op});
    }
    if (match(Tk::Amp)) {
        ExprPtr op = parsePrefix();
        return std::make_shared<Expr>(ExprUnary{ExprUnary::Ref, op});
    }
    return parsePostfix();
}

bool Parser::tryAttachGenericArgs(ExprPtr& expr) {
    if (!expr || !std::holds_alternative<ExprPath>(*expr)) return false;
    if (!check(Tk::Lt)) return false;
    // A '<' that opens a new line is not a generic argument list; attaching it
    // would swallow the start of the next statement.
    if (atLineStart()) return false;

    size_t save = pos;
    m_quietErrors = true;
    auto args = parseGenericArgs();
    m_quietErrors = false;
    if (args.empty()) {
        pos = save;
        return false;
    }

    auto& path = std::get<ExprPath>(*expr).path;
    path.args.insert(path.args.end(), args.begin(), args.end());
    return true;
}

bool Parser::atLineStart() const {
    if (pos == 0 || pos >= toks.size()) return false;
    const uint32_t here = toks[pos].loc.line;
    const uint32_t previous = toks[pos - 1].loc.line;
    if (here == 0 || previous == 0) return false;   // synthetic token, no line
    return here != previous;
}

ExprPtr Parser::parsePostfix() {
    // Anchor a postfix node on the token that actually names it. Without this
    // the node inherits whatever parseExpr stamped on the whole chain -- the
    // receiver's position -- so "field 'forEach' not found" pointed at the
    // receiver instead of at `forEach`.
    auto stamp = [](ExprPtr node, const SourceLocation& loc, size_t lexLen) -> ExprPtr {
        node->loc = loc;
        node->endLoc = loc;
        node->endLoc.col += static_cast<uint32_t>(std::max<size_t>(1, lexLen));
        return node;
    };

    ExprPtr expr = parsePrimary();
    bool justParsedCall = false;
    while (true) {
        if (tryAttachGenericArgs(expr)) {
            continue;
        }
        // Trailing lambda: `foo(a) { x -> body }` / `obj.method() { x -> body }`
        // passes the closure as the final argument. Disabled while parsing a
        // control-flow head (`if (foo()) { … }`) so the brace stays a block.
        if (justParsedCall && check(Tk::LBrace) && !atLineStart() && !m_parsingBlockHead) {
            ExprPtr lambda = parseBlockOrClosure();
            if (auto* call = std::get_if<ExprCall>(expr.get())) {
                call->args.push_back(std::move(lambda));
                call->argNames.push_back(std::nullopt);
            } else if (auto* mc = std::get_if<ExprMethodCall>(expr.get())) {
                mc->args.push_back(std::move(lambda));
                mc->argNames.push_back(std::nullopt);
            } else if (auto* sc = std::get_if<ExprSafeCall>(expr.get())) {
                sc->args.push_back(std::move(lambda));
            }
            justParsedCall = false;
            continue;
        }
        // A '(' that opens a new source line begins a new statement rather than
        // an argument list (Kotlin reads it the same way). Without this guard
        // `val x = f()` followed by a line starting with '(' parsed as `f()(…)`
        // -- and the receiver of whatever followed the parens became f's type.
        if (check(Tk::LParen) && !atLineStart()) {
            const SourceLocation parenLoc = peek().loc;
            std::vector<ExprPtr> args;
            std::vector<std::optional<std::string>> argNames;
            advance();
            while (!check(Tk::RParen) && !atEnd()) {
                std::optional<std::string> argName;
                if (match(Tk::Star)) {
                    argName = match(Tk::Star) ? "**" : "*";
                } else if (check(Tk::Ident) && check(Tk::Eq, 1)) {
                    argName = peek().lex;
                    pos += 2;
                }
                args.push_back(parseExpr());
                argNames.push_back(std::move(argName));
                if (!match(Tk::Comma)) break;
            }
            expect(Tk::RParen);
            auto call = ExprCall{expr, std::move(args)};
            call.argNames = std::move(argNames);
            expr = stamp(std::make_shared<Expr>(std::move(call)), parenLoc, 1);
            justParsedCall = true;
        } else if (match(Tk::Dot)) {
            if (check(Tk::Ident)) {
                const SourceLocation nameLoc = peek().loc;
                std::string name = peek().lex;
                advance();
                std::vector<TypePtr> methodTypeArgs;
                if (check(Tk::Lt)) {
                    size_t genericSave = pos;
                    m_quietErrors = true;
                    auto parsedArgs = parseGenericArgs();
                    m_quietErrors = false;
                    if (!parsedArgs.empty() && check(Tk::LParen) && !atLineStart()) methodTypeArgs = std::move(parsedArgs);
                    else pos = genericSave;
                }
                if (check(Tk::LParen) && !atLineStart()) {
                    std::vector<ExprPtr> args;
                    std::vector<std::optional<std::string>> argNames;
                    advance();
                    while (!check(Tk::RParen) && !atEnd()) {
                        std::optional<std::string> argName;
                        if (match(Tk::Star)) {
                            argName = match(Tk::Star) ? "**" : "*";
                        } else if (check(Tk::Ident) && check(Tk::Eq, 1)) {
                            argName = peek().lex;
                            pos += 2;
                        }
                        args.push_back(parseExpr());
                        argNames.push_back(std::move(argName));
                        if (!match(Tk::Comma)) break;
                    }
                    expect(Tk::RParen);
                    auto call = ExprMethodCall{expr, name, std::move(args)};
                    call.typeArgs = std::move(methodTypeArgs);
                    call.argNames = std::move(argNames);
                    expr = stamp(std::make_shared<Expr>(std::move(call)), nameLoc, name.size());
                    justParsedCall = true;
                } else if (check(Tk::LBrace) && !atLineStart() && !m_parsingBlockHead) {
                    // Parentheses-elided trailing lambda: `xs.map { x -> ... }`
                    ExprPtr lambda = parseBlockOrClosure();
                    std::vector<ExprPtr> args;
                    args.push_back(std::move(lambda));
                    auto call = ExprMethodCall{expr, name, std::move(args)};
                    call.typeArgs = std::move(methodTypeArgs);
                    call.argNames.push_back(std::nullopt);
                    expr = stamp(std::make_shared<Expr>(std::move(call)), nameLoc, name.size());
                    justParsedCall = false;
                } else {
                    if (!methodTypeArgs.empty()) error("generic member arguments require a method call");
                    expr = stamp(std::make_shared<Expr>(ExprField{expr, name}), nameLoc, name.size());
                    justParsedCall = false;
                }
            } else {
                error("expected identifier after '.'");
                break;
            }
        } else if (match(Tk::Question)) {
            // ?. safe call/field, or ?? (elvis is ?: handled in parseElvis)
            if (match(Tk::Dot)) {
                if (check(Tk::Ident)) {
                    const SourceLocation nameLoc = peek().loc;
                    std::string name = peek().lex;
                    advance();
                    if (check(Tk::LParen) && !atLineStart()) {
                        std::vector<ExprPtr> args;
                        advance();
                        while (!check(Tk::RParen) && !atEnd()) {
                            if ((check(Tk::Ident) && check(Tk::Eq, 1)) || check(Tk::Star))
                                error("named and spread arguments require an ordinary method call; unwrap the receiver first");
                            args.push_back(parseExpr());
                            if (!match(Tk::Comma)) break;
                        }
                        expect(Tk::RParen);
                        expr = stamp(std::make_shared<Expr>(ExprSafeCall{expr, name, std::move(args)}),
                                     nameLoc, name.size());
                        justParsedCall = true;
                    } else {
                        expr = stamp(std::make_shared<Expr>(ExprSafeField{expr, name}),
                                     nameLoc, name.size());
                        justParsedCall = false;
                    }
                } else {
                    error("expected identifier after '?.'");
                    break;
                }
            } else {
                // standalone ? was handled in parseElvis (a ?: b or a?)
                --pos;
                return expr;
            }
        } else if (match(Tk::Bang)) {
            if (check(Tk::KwIs)) {
                --pos; // put back the ! so parseCmp can handle !is
                return expr;
            }
            // !! force unwrap — second ! follows
            if (match(Tk::Bang)) {
                expr = stamp(std::make_shared<Expr>(ExprForceUnwrap{expr}), toks[pos - 1].loc, 2);
            } else {
                error("expected '!!' for force unwrap");
                break;
            }
        } else if (check(Tk::LBracket) && !atLineStart()) {
            const SourceLocation bracketLoc = peek().loc;
            advance();
            ExprPtr idx = parseExpr();
            expect(Tk::RBracket);
            expr = stamp(std::make_shared<Expr>(ExprIndex{expr, idx}), bracketLoc, 1);
            justParsedCall = false;
        } else {
            break;
        }
    }
    return expr;
}

ExprPtr Parser::parsePrimary() {
    if (check(Tk::IntLit) || check(Tk::FloatLit) || check(Tk::StringLit) || check(Tk::CharLit)
        || check(Tk::KwTrue) || check(Tk::KwFalse))
        return parseLiteral();

    if (match(Tk::KwNull)) return parseNull();

    // Labeled loop, `name: for ...`. The label comes *before* the keyword, so
    // this has to run before the plain-identifier branch below -- testing for an
    // identifier once the loop keyword is already known to be next can never
    // match, which is why labels used to be unparseable.
    if (check(Tk::Ident) && check(Tk::Colon, 1) &&
        (check(Tk::KwWhile, 2) || check(Tk::KwFor, 2) || check(Tk::KwLoop, 2))) {
        std::string label = peek().lex;
        pos += 2;
        if (match(Tk::KwWhile)) return parseWhile(label);
        if (match(Tk::KwFor)) return parseFor(label);
        if (match(Tk::KwLoop)) return parseLoop(label);
    }

    if (check(Tk::Ident)) {
        Path path = parsePath();
        if (check(Tk::LBrace) && !m_parsingBlockHead) return parseStructExpr(path);
        return std::make_shared<Expr>(ExprPath{path});
    }

    if (check(Tk::LParen)) {
        advance();
        if (match(Tk::RParen)) return std::make_shared<Expr>(ExprTuple{std::vector<ExprPtr>{}});
        ExprPtr first = parseExpr();
        if (match(Tk::Comma)) {
            std::vector<ExprPtr> elems = {first};
            while (!check(Tk::RParen) && !atEnd()) {
                elems.push_back(parseExpr());
                if (!match(Tk::Comma)) break;
            }
            expect(Tk::RParen);
            return std::make_shared<Expr>(ExprTuple{std::move(elems)});
        }
        expect(Tk::RParen);
        return first;
    }

    if (check(Tk::LBracket)) {
        advance();
        std::vector<ExprPtr> elems;
        while (!check(Tk::RBracket) && !atEnd()) {
            elems.push_back(parseExpr());
            if (!match(Tk::Comma)) break;
        }
        expect(Tk::RBracket);
        return std::make_shared<Expr>(ExprArray{std::move(elems)});
    }

    if (check(Tk::LBrace)) {
        if (looksLikeMapLiteral()) return parseMapLiteral();
        return parseBlockOrClosure();
    }

    if (check(Tk::KwWhile) || check(Tk::KwFor) || check(Tk::KwLoop)) {
        if (match(Tk::KwWhile)) return parseWhile(std::nullopt);
        if (match(Tk::KwFor)) return parseFor(std::nullopt);
        if (match(Tk::KwLoop)) return parseLoop(std::nullopt);
    }

    if (match(Tk::KwIf)) return parseIf();
    if (match(Tk::KwWhen)) return parseWhen();
    if (match(Tk::KwReturn)) return parseReturn();
    if (match(Tk::KwThrow)) return parseThrow();
    if (match(Tk::KwTry)) return parseTryCatch();
    if (match(Tk::KwBreak)) return parseBreak();
    if (match(Tk::KwContinue)) return parseContinue();
    // string template: $"hello $name"
    // `$"..."` is the old spelling. Every string interpolates now, so point
    // anyone still writing the prefix at the plain form.
    if (match(Tk::Dollar)) {
        if (check(Tk::StringLit)) {
            error("remove the '$' before this string; every string interpolates now");
        } else {
            error("unexpected '$'");
        }
        return std::make_shared<Expr>(ExprLit{std::string("")});
    }

    error("unexpected token in expression");
    if (!atEnd()) advance();
    return std::make_shared<Expr>(ExprLit{0});
}

bool Parser::looksLikeMapLiteral() {
    if (!check(Tk::LBrace) || check(Tk::RBrace, 1)) return false;
    Tk first = toks[pos + 1].kind;
    if (first == Tk::KwVal || first == Tk::KwVar || first == Tk::KwLet ||
        first == Tk::KwFun || first == Tk::KwReturn || first == Tk::KwIf ||
        first == Tk::KwWhen || first == Tk::KwWhile || first == Tk::KwFor ||
        first == Tk::KwLoop) return false;
    size_t depth = 0;
    bool sawColon = false;
    for (size_t i = pos + 1; i < toks.size(); ++i) {
        Tk kind = toks[i].kind;
        if (kind == Tk::LParen || kind == Tk::LBracket || kind == Tk::LBrace) { depth++; continue; }
        if (kind == Tk::RParen || kind == Tk::RBracket || kind == Tk::RBrace) {
            if (depth == 0) return sawColon;
            depth--;
            continue;
        }
        if (depth == 0) {
            if (kind == Tk::Arrow || kind == Tk::Semi) return false;
            if (kind == Tk::Colon) sawColon = true;
        }
    }
    return false;
}

ExprPtr Parser::parseMapLiteral() {
    expect(Tk::LBrace);
    std::vector<ExprMapEntry> entries;
    while (!check(Tk::RBrace) && !atEnd()) {
        ExprPtr key = parseExpr();
        expect(Tk::Colon);
        ExprPtr value = parseExpr();
        entries.push_back({key, value});
        if (!match(Tk::Comma)) break;
    }
    expect(Tk::RBrace);
    return std::make_shared<Expr>(ExprMap{std::move(entries)});
}

// Parse the content between { and } — returns ExprBlock or single expression.
ExprPtr Parser::parseBlockBody() {
    std::vector<StmtPtr> stmts;
    ExprPtr tail = nullptr;
    while (!check(Tk::RBrace) && !atEnd()) {
        if (check(Tk::KwLet)) {
            stmts.push_back(parseLet());
        } else if (check(Tk::KwVal) || check(Tk::KwVar)) {
            bool isVal = match(Tk::KwVal);
            if (!isVal) match(Tk::KwVar);
            std::string name = expectIdent();
            TypePtr ty = nullptr;
            if (match(Tk::Colon)) ty = parseType();
            ExprPtr init = nullptr;
            if (match(Tk::Eq)) init = parseExpr();
            match(Tk::Semi);
            PatPtr pat = std::make_shared<Pat>(PatBind{name, !isVal, nullptr});
            stmts.push_back(std::make_shared<Stmt>(StmtLet{pat, ty, init}));
        } else if (isItemStart()) {
            stmts.push_back(parseItemStmt());
        } else {
            ExprPtr e = parseExpr();
            bool isAssign = std::holds_alternative<ExprAssign>(*e);
            if (match(Tk::Semi) || isAssign) {
                stmts.push_back(std::make_shared<Stmt>(StmtExpr{e, true}));
            } else if (check(Tk::RBrace)) {
                tail = e;
                break;
            } else {
                stmts.push_back(std::make_shared<Stmt>(StmtExpr{e, true}));
            }
        }
    }
    if (stmts.empty() && tail) return tail;
    return std::make_shared<Expr>(ExprBlock{std::move(stmts), tail});
}

ExprPtr Parser::parseBlock() {
    expect(Tk::LBrace);
    ExprPtr body = parseBlockBody();
    expect(Tk::RBrace);
    return body; // always an ExprBlock (or single expr)
}

// Expression-level { ... } — Kotlin-style: always a closure.
// { }             → no-param closure returning unit
// { -> body }     → no-param closure (explicit)
// { x, y -> body }→ closure with params
// { stmts }       → no-param closure (implicit, like Kotlin)
ExprPtr Parser::parseBlockOrClosure() {
    expect(Tk::LBrace);

    // Empty: {} — no-param closure, unit body
    if (match(Tk::RBrace)) {
        auto unitBody = std::make_shared<Expr>(ExprBlock{std::vector<StmtPtr>{}, nullptr});
        return std::make_shared<Expr>(ExprClosure{
            std::vector<std::pair<std::string,TypePtr>>{}, nullptr, unitBody});
    }

    // No-param closure (explicit arrow): { -> body }
    if (check(Tk::Arrow)) {
        advance();
        ExprPtr body = parseBlockBody();
        expect(Tk::RBrace);
        return std::make_shared<Expr>(ExprClosure{
            std::vector<std::pair<std::string,TypePtr>>{}, nullptr, body});
    }

    // Param-ful closure: { ident [: type] [, ident [: type]]* -> body }
    if (check(Tk::Ident)) {
        size_t save = pos;
        m_quietErrors = true;

        std::vector<std::pair<std::string,TypePtr>> params;
        bool isClosure = true;

        std::string name = expectIdent();
        TypePtr ty = nullptr;
        if (match(Tk::Colon)) ty = parseType();
        params.emplace_back(name, ty);

        while (match(Tk::Comma)) {
            if (!check(Tk::Ident)) { isClosure = false; break; }
            name = expectIdent();
            ty = nullptr;
            if (match(Tk::Colon)) ty = parseType();
            params.emplace_back(name, ty);
        }

        m_quietErrors = false;

        if (isClosure && check(Tk::Arrow)) {
            advance();
            ExprPtr body = parseBlockBody();
            expect(Tk::RBrace);
            return std::make_shared<Expr>(ExprClosure{std::move(params), nullptr, body});
        }

        // Not a closure — backtrack to start of body
        pos = save;
    }

    // Implicit no-param closure: { stmts; tail }
    ExprPtr body = parseBlockBody();
    expect(Tk::RBrace);
    return std::make_shared<Expr>(ExprClosure{
        std::vector<std::pair<std::string,TypePtr>>{}, nullptr, body});
}

ExprPtr Parser::parseIf() {
    bool savedBlockHead = m_parsingBlockHead;
    m_parsingBlockHead = true;
    ExprPtr cond = parseExpr();
    m_parsingBlockHead = savedBlockHead;
    ExprPtr then_ = parseBlock();
    ExprPtr else_ = nullptr;
    if (match(Tk::KwElse)) {
        if (check(Tk::KwIf)) { advance(); else_ = parseIf(); }
        else else_ = parseBlock();
    }
    return std::make_shared<Expr>(ExprIf{cond, then_, else_});
}

ExprPtr Parser::parseWhile(std::optional<std::string> label) {
    bool savedBlockHead = m_parsingBlockHead;
    m_parsingBlockHead = true;
    ExprPtr cond = parseExpr();
    m_parsingBlockHead = savedBlockHead;
    ExprPtr body = parseBlock();
    return std::make_shared<Expr>(ExprWhile{cond, body, label});
}

ExprPtr Parser::parseFor(std::optional<std::string> label) {
    PatPtr pat = parsePat();
    if (check(Tk::KwIn)) advance();
    else error("expected 'in' in for loop");
    bool savedBlockHead = m_parsingBlockHead;
    m_parsingBlockHead = true;
    ExprPtr iter = parseExpr();
    m_parsingBlockHead = savedBlockHead;
    ExprPtr body = parseBlock();
    return std::make_shared<Expr>(ExprFor{pat, iter, body, label});
}

ExprPtr Parser::parseLoop(std::optional<std::string> label) {
    ExprPtr body = parseBlock();
    return std::make_shared<Expr>(ExprLoop{body, label});
}

ExprPtr Parser::parseWhen() {
    bool hasScrut = false;
    ExprPtr scrut = nullptr;
    if (match(Tk::LParen)) {
        hasScrut = true;
        scrut = parseExpr();
        expect(Tk::RParen);
    }
    expect(Tk::LBrace);
    std::vector<WhenArm> arms;
    while (!check(Tk::RBrace) && !atEnd()) {
        WhenArm arm;
        if (match(Tk::KwElse)) {
            arm.isElse = true;
        } else {
            if (hasScrut) {
                arm.condition = parsePat();
            } else {
                ExprPtr cond = parseExpr();
                if (match(Tk::KwIs)) {
                    TypePtr ty = parseType();
                    arm.condition = std::make_shared<Expr>(ExprIs{cond, ty});
                } else if (match(Tk::KwIn)) {
                    ExprPtr range = parseExpr();
                    arm.condition = std::make_shared<Expr>(ExprBinary{static_cast<ExprBinary::Op>(92), cond, range});
                } else {
                    arm.condition = cond;
                }
            }
        }
        expect(Tk::Arrow);
        arm.body = check(Tk::LBrace) ? parseBlock() : parseExpr();
        arms.push_back(std::move(arm));
        match(Tk::Comma); // optional comma between arms
    }
    expect(Tk::RBrace);
    return std::make_shared<Expr>(ExprWhen{scrut, std::move(arms)});
}

ExprPtr Parser::parseBreak() {
    std::optional<std::string> label;
    if (check(Tk::Ident)) { label = peek().lex; advance(); }
    ExprPtr e = nullptr;
    if (!check(Tk::Semi) && !check(Tk::RBrace) && !check(Tk::RBracket) && !check(Tk::RParen) && !check(Tk::Comma) && !atEnd())
        e = parseExpr();
    return std::make_shared<Expr>(ExprBreak{label, e});
}

ExprPtr Parser::parseContinue() {
    std::optional<std::string> label;
    if (check(Tk::Ident)) { label = peek().lex; advance(); }
    return std::make_shared<Expr>(ExprContinue{label});
}

ExprPtr Parser::parseReturn() {
    ExprPtr e = nullptr;
    if (!check(Tk::Semi) && !check(Tk::RBrace) && !check(Tk::RBracket) && !check(Tk::RParen) && !check(Tk::Comma) && !atEnd())
        e = parseExpr();
    return std::make_shared<Expr>(ExprReturn{e});
}

ExprPtr Parser::parseThrow() {
    if (check(Tk::Semi) || check(Tk::Newline) || check(Tk::RBrace) || atEnd()) {
        error("throw requires an exception value");
        return std::make_shared<Expr>(ExprThrow{nullptr});
    }
    return std::make_shared<Expr>(ExprThrow{parseExpr()});
}

ExprPtr Parser::parseTryCatch() {
    if (!check(Tk::LBrace)) error("try requires a block");
    ExprPtr body = parseBlock();
    std::vector<CatchClause> catches;
    ExprPtr finallyBody;
    auto skipSeparators = [&]() {
        while (match(Tk::Newline) || match(Tk::Semi)) {}
    };
    skipSeparators();
    while (match(Tk::KwCatch)) {
        expect(Tk::LParen);
        std::string name = expectIdent();
        expect(Tk::Colon);
        TypePtr ty = parseType();
        expect(Tk::RParen);
        if (!check(Tk::LBrace)) error("catch requires a block");
        ExprPtr catchBody = parseBlock();
        catches.push_back(CatchClause{std::move(name), std::move(ty), std::move(catchBody)});
        skipSeparators();
    }
    if (match(Tk::KwFinally)) {
        if (!check(Tk::LBrace)) error("finally requires a block");
        finallyBody = parseBlock();
    }
    if (catches.empty() && !finallyBody) error("try requires catch or finally");
    return std::make_shared<Expr>(ExprTryCatch{std::move(body), std::move(catches), std::move(finallyBody)});
}

ExprPtr Parser::parseStructExpr(Path path) {
    expect(Tk::LBrace);
    std::vector<ExprFieldInit> fields;
    bool rest = false;
    while (!check(Tk::RBrace) && !atEnd()) {
        if (match(Tk::DotDot)) {
            rest = true;
            if (!check(Tk::RBrace)) parseExpr();
            break;
        }
        std::string name = expectIdent();
        ExprPtr val = nullptr;
        if (match(Tk::Colon)) val = parseExpr();
        else val = std::make_shared<Expr>(ExprPath{Path{{name}, {}}});
        fields.push_back({name, val});
        if (!match(Tk::Comma)) break;
    }
    expect(Tk::RBrace);
    return std::make_shared<Expr>(ExprStruct{std::move(path), std::move(fields), rest});
}

ExprPtr Parser::parseLiteral() {
    Token t = peek();
    advance();
    switch (t.kind) {
        case Tk::KwTrue: return std::make_shared<Expr>(ExprLit{true});
        case Tk::KwFalse: return std::make_shared<Expr>(ExprLit{false});
        case Tk::IntLit: {
            std::string s = t.lex;
            s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
            int base = 10;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s = s.substr(2); }
            else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s = s.substr(2); }
            else if (s.size() > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) { base = 8; s = s.substr(2); }
            int64_t v = std::stoll(s, nullptr, base);
            return std::make_shared<Expr>(ExprLit{v});
        }
        case Tk::FloatLit: {
            std::string s = t.lex;
            s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
            double v = std::stod(s);
            return std::make_shared<Expr>(ExprLit{v});
        }
        case Tk::StringLit:
            // Interpolation is a property of the content, not a separate literal
            // syntax: any string may carry `$name` / `${expr}`.
            if (t.lex.find('$') != std::string::npos) return parseStringTemplate(t);
            return std::make_shared<Expr>(ExprLit{unescapeString(t.lex.substr(1, t.lex.size()-2))});
        case Tk::CharLit: {
            std::string s = t.lex.substr(1, t.lex.size()-2);
            char c = 0;
            if (!s.empty()) {
                if (s[0] == '\\' && s.size() > 1) {
                    switch (s[1]) {
                        case 'n': c = '\n'; break;
                        case 't': c = '\t'; break;
                        case 'r': c = '\r'; break;
                        case '0': c = '\0'; break;
                        case '\\': c = '\\'; break;
                        case '\'': c = '\''; break;
                        case 'x': {
                            if (s.size() > 3) {
                                std::string hex = s.substr(2, 2);
                                c = static_cast<char>(std::stoi(hex, nullptr, 16));
                            }
                            break;
                        }
                        default: c = s[1]; break;
                    }
                } else {
                    c = s[0];
                }
            }
            return std::make_shared<Expr>(ExprLit{c});
        }
        default: return std::make_shared<Expr>(ExprLit{0});
    }
}

ExprPtr Parser::parseNull() {
    return std::make_shared<Expr>(ExprNull{});
}

ExprPtr Parser::parseEmbeddedExpr() {
    ExprPtr e = parseExpr();
    // A fragment that does not lex down to a single expression is a mistake in
    // the template, not something to silently truncate.
    if (!atEnd()) error("unexpected token after interpolation expression");
    return e;
}

// Turns `$"..."` into ExprStringTemplate. The parts that are plain text become
// strings; everything else is parsed as a real expression, so `${a + b}` means
// addition rather than a variable literally named "a + b".
ExprPtr Parser::parseStringTemplate(const Token& tok) {
    // tok.lex is the raw lexeme: quotes included, escapes not yet resolved --
    // the same form parseLiteral consumes.
    std::string raw = tok.lex.substr(1, tok.lex.size() - 2);
    ExprStringTemplate tmpl;
    std::string lit;

    // The body starts one column to the right of the opening quote.
    SourceLocation contentStart = tok.loc;
    contentStart.col += 1;

    auto flushLit = [&]() {
        if (!lit.empty()) { tmpl.parts.push_back(unescapeString(lit)); lit.clear(); }
    };
    auto embedded = [&](const std::string& src, size_t offset) -> ExprPtr {
        SourceLocation start = advanceLoc(contentStart, raw, offset);
        Lexer subLexer(src, start.file, start);
        Parser subParser(subLexer.tokenize());
        // A template can be reached while speculatively parsing, and a fresh
        // parser is not quiet by default; without this it would throw out of a
        // parse the caller intends to back out of.
        subParser.m_quietErrors = m_quietErrors;
        return subParser.parseEmbeddedExpr();
    };

    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '$') { lit.push_back(raw[i]); ++i; continue; }
        if (i + 1 >= raw.size()) { lit.push_back('$'); ++i; continue; }

        char next = raw[i + 1];
        if (next == '$') { lit.push_back('$'); i += 2; continue; }  // `$$` is a literal `$`

        if (next == '{') {
            // Scan to the matching brace. Nested braces and nested string
            // literals both have to be stepped over, or the first `}` or `"`
            // inside them would end the fragment early.
            size_t j = i + 2;
            int depth = 1;
            while (j < raw.size() && depth > 0) {
                char c = raw[j];
                if (c == '\\') { j += 2; continue; }
                if (c == '"') {
                    ++j;
                    while (j < raw.size() && raw[j] != '"') j += (raw[j] == '\\') ? 2 : 1;
                    ++j;
                    continue;
                }
                if (c == '{') { ++depth; ++j; continue; }
                if (c == '}') { --depth; if (depth == 0) break; }
                ++j;
            }
            if (depth != 0) {
                error("unterminated '${' in string template");
                return std::make_shared<Expr>(ExprLit{std::string("")});
            }
            flushLit();
            tmpl.parts.push_back(embedded(raw.substr(i + 2, j - i - 2), i + 2));
            i = j + 1;
            continue;
        }

        // `$name` shorthand for `${name}`.
        if (std::isalpha(static_cast<unsigned char>(next)) || next == '_') {
            size_t j = i + 1;
            while (j < raw.size() &&
                   (std::isalnum(static_cast<unsigned char>(raw[j])) || raw[j] == '_')) ++j;
            flushLit();
            auto path = std::make_shared<Expr>(ExprPath{Path{{raw.substr(i + 1, j - i - 1)}, {}}});
            path->loc = advanceLoc(contentStart, raw, i + 1);
            path->endLoc = path->loc;
            tmpl.parts.push_back(path);
            i = j;
            continue;
        }

        lit.push_back('$');
        ++i;
    }
    flushLit();

    if (tmpl.parts.empty()) return std::make_shared<Expr>(ExprLit{std::string("")});
    if (tmpl.parts.size() == 1 && std::holds_alternative<std::string>(tmpl.parts[0]))
        return std::make_shared<Expr>(ExprLit{std::get<std::string>(tmpl.parts[0])});
    return std::make_shared<Expr>(tmpl);
}

// ---------- patterns ----------
PatPtr Parser::parsePat() {
    DepthGuard guard(*this);
    if (match(Tk::Underscore)) return std::make_shared<Pat>(PatWild{});
    if (match(Tk::Amp)) {
        PatPtr sub = parsePat();
        return std::make_shared<Pat>(PatRef{false, sub});
    }
    if (check(Tk::LParen)) {
        advance();
        if (match(Tk::RParen)) return std::make_shared<Pat>(PatTuple{std::vector<PatPtr>{}});
        PatPtr first = parsePat();
        if (match(Tk::Comma)) {
            std::vector<PatPtr> elems = {first};
            while (!check(Tk::RParen) && !atEnd()) {
                elems.push_back(parsePat());
                if (!match(Tk::Comma)) break;
            }
            expect(Tk::RParen);
            return std::make_shared<Pat>(PatTuple{std::move(elems)});
        }
        expect(Tk::RParen);
        return first;
    }
    if (check(Tk::IntLit) || check(Tk::FloatLit) || check(Tk::StringLit) || check(Tk::CharLit)
        || check(Tk::KwTrue) || check(Tk::KwFalse))
        return std::make_shared<Pat>(PatLit{parseLiteral()});

    if (check(Tk::Ident)) {
        Path path;
        path.segs.push_back(expectIdent());
        while (match(Tk::DColon)) {
            if (check(Tk::LParen) || check(Tk::LBrace)) break;
            path.segs.push_back(expectIdent());
        }
        if (check(Tk::LParen)) {
            std::vector<PatPtr> args;
            advance();
            while (!check(Tk::RParen) && !atEnd()) {
                args.push_back(parsePat());
                if (!match(Tk::Comma)) break;
            }
            expect(Tk::RParen);
            return std::make_shared<Pat>(PatEnum{std::move(path), std::move(args)});
        }
        if (check(Tk::LBrace)) {
            advance();
            std::vector<PatField> fields;
            bool rest = false;
            while (!check(Tk::RBrace) && !atEnd()) {
                if (match(Tk::DotDot)) { rest = true; break; }
                std::string name = expectIdent();
                PatPtr p = nullptr;
                if (match(Tk::Colon)) p = parsePat();
                else p = std::make_shared<Pat>(PatBind{name, false, nullptr});
                fields.push_back({name, p});
                if (!match(Tk::Comma)) break;
            }
            expect(Tk::RBrace);
            return std::make_shared<Pat>(PatStruct{std::move(path), std::move(fields), rest});
        }
        if (path.segs.size() == 1)
            return std::make_shared<Pat>(PatBind{path.segs[0], false, nullptr});
        return std::make_shared<Pat>(PatEnum{std::move(path), std::vector<PatPtr>{}});
    }
    error("unexpected token in pattern");
    return std::make_shared<Pat>(PatWild{});
}

// ---------- types ----------
TypePtr Parser::parseType() {
    DepthGuard guard(*this);
    TypePtr base = nullptr;

    // fn type: fun(params) -> ret
    if (match(Tk::KwFun)) {
        expect(Tk::LParen);
        std::vector<TypePtr> params;
        while (!check(Tk::RParen) && !atEnd()) {
            params.push_back(parseType());
            if (!match(Tk::Comma)) break;
        }
        expect(Tk::RParen);
        TypePtr ret = nullptr;
        if (match(Tk::Arrow)) ret = parseType();
        else ret = std::make_shared<Type>(AstTyTuple{std::vector<TypePtr>{}});
        base = std::make_shared<Type>(AstTyFn{std::move(params), ret});
    } else if (match(Tk::Amp)) {
        TypePtr elem = parseType();
        base = std::make_shared<Type>(AstTyRef{false, elem});
    } else if (match(Tk::Star)) {
        TypePtr elem = parseType();
        base = std::make_shared<Type>(AstTyPtr{false, elem});
    } else if (check(Tk::LBracket)) {
        advance();
        TypePtr elem = parseType();
        if (match(Tk::Semi)) {
            ExprPtr size = parseExpr();
            expect(Tk::RBracket);
            base = std::make_shared<Type>(AstTyArray{elem, size});
        } else {
            expect(Tk::RBracket);
            base = std::make_shared<Type>(AstTySlice{elem});
        }
    } else if (check(Tk::LParen)) {
        advance();
        if (match(Tk::RParen)) base = std::make_shared<Type>(AstTyTuple{std::vector<TypePtr>{}});
        else {
            std::vector<TypePtr> elems;
            elems.push_back(parseType());
            while (match(Tk::Comma)) {
                if (check(Tk::RParen)) break;
                elems.push_back(parseType());
            }
            expect(Tk::RParen);
            if (elems.size() == 1) base = elems[0];
            else base = std::make_shared<Type>(AstTyTuple{std::move(elems)});
        }
    } else {
        Path p = parseTypePath();
        base = std::make_shared<Type>(AstTyPath{p});
    }

    // nullable suffix: T?
    while (match(Tk::Question)) {
        base = std::make_shared<Type>(AstTyNullable{base});
    }

    return base;
}

} // namespace loxis::v2
