#include "Parser.hpp"
#include "Lexer.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace loxis::v2 {

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
SourceLoc Parser::curloc() { return peek().loc; }

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
    std::cerr << "parse error: " << msg << " at " << peek().loc.fmt() << "\n";
}

// ---------- constructor / entry ----------
Parser::Parser(std::vector<Token> tokens) {
    for (auto& t : tokens) {
        if (t.kind != Tk::Newline) toks.push_back(std::move(t));
    }
    if (toks.empty() || toks.back().kind != Tk::Eof)
        toks.emplace_back(Tk::Eof, "", SourceLoc{});
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
        error("expected '->' before return type");
        return parseType();
    }
    return nullptr;
}

std::vector<std::pair<std::string,TypePtr>> Parser::parseFnParams() {
    std::vector<std::pair<std::string,TypePtr>> out;
    expect(Tk::LParen);
    while (!check(Tk::RParen) && !atEnd()) {
        std::string name = expectIdent();
        expect(Tk::Colon);
        TypePtr ty = parseType();
        out.emplace_back(name, ty);
        if (!match(Tk::Comma)) break;
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
    expectGt();
    return out;
}

void Parser::expectGt() {
    if (match(Tk::Gt)) return;
    if (check(Tk::Shr)) {
        Token t = peek();
        t.kind = Tk::Gt;
        t.lex = ">";
        toks[pos] = t;
        toks.insert(toks.begin() + pos + 1, Token{Tk::Gt, ">", t.loc});
        advance();
        return;
    }
    error("expected '>'");
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

    error("expected item");
    return nullptr;
}

// ---------- function ----------
ItemFun Parser::parseFun(Visibility vis) {
    expect(Tk::KwFun);
    std::string name = expectIdent();
    auto generics = parseGenerics();
    auto params = parseFnParams();
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
    return ItemFun{name, vis, std::move(generics), std::move(params), ret, std::move(where_), body, false, false, false};
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
        auto params = parseFnParams();
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
        methods.push_back({mname, std::move(params), ret, def});
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
ExprPtr Parser::parseExpr() { return parseAssign(); }

ExprPtr Parser::parseAssign() {
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
        return std::make_shared<Expr>(ExprBinary{static_cast<ExprBinary::Op>(90), lhs, rhs});
    }
    if (match(Tk::DotDotEq)) {
        ExprPtr rhs = parseOr();
        return std::make_shared<Expr>(ExprBinary{static_cast<ExprBinary::Op>(91), lhs, rhs});
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
        ExprBinary::Op op;
        if (match(Tk::EqEq)) op = ExprBinary::Eq;
        else if (match(Tk::Ne)) op = ExprBinary::Ne;
        else if (match(Tk::Lt)) op = ExprBinary::Lt;
        else if (match(Tk::Gt)) op = ExprBinary::Gt;
        else if (match(Tk::Le)) op = ExprBinary::Le;
        else if (match(Tk::Ge)) op = ExprBinary::Ge;
        else if (match(Tk::KwIs)) {
            if (match(Tk::Bang)) {
                TypePtr ty = parseType();
                lhs = std::make_shared<Expr>(ExprIsNot{lhs, ty});
            } else {
                TypePtr ty = parseType();
                lhs = std::make_shared<Expr>(ExprIs{lhs, ty});
            }
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
    if (match(Tk::Bang)) {
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

ExprPtr Parser::parsePostfix() {
    ExprPtr expr = parsePrimary();
    while (true) {
        if (check(Tk::LParen)) {
            std::vector<ExprPtr> args;
            advance();
            while (!check(Tk::RParen) && !atEnd()) {
                args.push_back(parseExpr());
                if (!match(Tk::Comma)) break;
            }
            expect(Tk::RParen);
            expr = std::make_shared<Expr>(ExprCall{expr, std::move(args)});
        } else if (match(Tk::Dot)) {
            if (check(Tk::Ident)) {
                std::string name = peek().lex;
                advance();
                if (check(Tk::LParen)) {
                    std::vector<ExprPtr> args;
                    advance();
                    while (!check(Tk::RParen) && !atEnd()) {
                        args.push_back(parseExpr());
                        if (!match(Tk::Comma)) break;
                    }
                    expect(Tk::RParen);
                    expr = std::make_shared<Expr>(ExprMethodCall{expr, name, std::move(args)});
                } else {
                    expr = std::make_shared<Expr>(ExprField{expr, name});
                }
            } else {
                error("expected identifier after '.'");
                break;
            }
        } else if (match(Tk::Question)) {
            // ?. safe call/field, or ?? (elvis is ?: handled in parseElvis)
            if (match(Tk::Dot)) {
                if (check(Tk::Ident)) {
                    std::string name = peek().lex;
                    advance();
                    if (check(Tk::LParen)) {
                        std::vector<ExprPtr> args;
                        advance();
                        while (!check(Tk::RParen) && !atEnd()) {
                            args.push_back(parseExpr());
                            if (!match(Tk::Comma)) break;
                        }
                        expect(Tk::RParen);
                        expr = std::make_shared<Expr>(ExprSafeCall{expr, name, std::move(args)});
                    } else {
                        expr = std::make_shared<Expr>(ExprSafeField{expr, name});
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
            // !! force unwrap — second ! follows
            if (match(Tk::Bang)) {
                expr = std::make_shared<Expr>(ExprForceUnwrap{expr});
            } else {
                error("expected '!!' for force unwrap");
                break;
            }
        } else if (check(Tk::LBracket)) {
            advance();
            ExprPtr idx = parseExpr();
            expect(Tk::RBracket);
            expr = std::make_shared<Expr>(ExprIndex{expr, idx});
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

    if (check(Tk::Ident)) {
        Path path = parsePath();
        if (check(Tk::LBrace)) return parseStructExpr(path);
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

    if (check(Tk::LBrace)) return parseBlock();

    if (check(Tk::KwWhile) || check(Tk::KwFor) || check(Tk::KwLoop)) {
        std::optional<std::string> label;
        if (check(Tk::Ident) && check(Tk::Colon, 1)) {
            label = peek().lex;
            pos += 2;
        }
        if (match(Tk::KwWhile)) return parseWhile(label);
        if (match(Tk::KwFor)) return parseFor(label);
        if (match(Tk::KwLoop)) return parseLoop(label);
    }

    if (match(Tk::KwIf)) return parseIf();
    if (match(Tk::KwWhen)) return parseWhen();
    if (match(Tk::KwReturn)) return parseReturn();
    if (match(Tk::KwBreak)) return parseBreak();
    if (match(Tk::KwContinue)) return parseContinue();
    if (check(Tk::Pipe)) return parseClosure();

    // string template: $"hello $name"
    if (match(Tk::Dollar)) {
        if (check(Tk::StringLit)) {
            // simple string with $ interpolation
            std::string s = peek().lex;
            advance();
            // parse as string template
            ExprStringTemplate tmpl;
            std::string content = s.substr(1, s.size() - 2); // remove quotes
            size_t i = 0;
            std::string lit;
            while (i < content.size()) {
                if (content[i] == '$' && i + 1 < content.size()) {
                    if (content[i+1] == '{') {
                        // ${expr}
                        if (!lit.empty()) { tmpl.parts.push_back(lit); lit.clear(); }
                        size_t j = i + 2;
                        int braceDepth = 1;
                        size_t exprStart = j;
                        while (j < content.size() && braceDepth > 0) {
                            if (content[j] == '{') braceDepth++;
                            else if (content[j] == '}') braceDepth--;
                            j++;
                        }
                        std::string exprStr = content.substr(exprStart, j - exprStart - 1);
                        // can't parse from string here — store as literal for now
                        tmpl.parts.push_back(exprStr);
                        i = j;
                        continue;
                    } else if (std::isalpha(static_cast<unsigned char>(content[i+1])) || content[i+1] == '_') {
                        if (!lit.empty()) { tmpl.parts.push_back(lit); lit.clear(); }
                        size_t j = i + 1;
                        while (j < content.size() && (std::isalnum(static_cast<unsigned char>(content[j])) || content[j] == '_')) j++;
                        tmpl.parts.push_back(content.substr(i + 1, j - i - 1));
                        i = j;
                        continue;
                    }
                }
                lit += content[i];
                i++;
            }
            if (!lit.empty()) tmpl.parts.push_back(lit);
            if (tmpl.parts.size() == 1 && std::holds_alternative<std::string>(tmpl.parts[0]))
                return std::make_shared<Expr>(ExprLit{std::get<std::string>(tmpl.parts[0])});
            return std::make_shared<Expr>(tmpl);
        }
        error("expected string literal after '$'");
        return std::make_shared<Expr>(ExprLit{std::string("")});
    }

    error("unexpected token in expression");
    if (!atEnd()) advance();
    return std::make_shared<Expr>(ExprLit{0});
}

ExprPtr Parser::parseBlock() {
    expect(Tk::LBrace);
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
            // Assignment or call without semicolon is still a statement
            bool isAssign = std::holds_alternative<ExprAssign>(*e);
            if (match(Tk::Semi) || isAssign) {
                stmts.push_back(std::make_shared<Stmt>(StmtExpr{e, true}));
            } else if (check(Tk::RBrace)) {
                tail = e;
                break;
            } else {
                // No semicolon but more content on next line — treat as statement
                stmts.push_back(std::make_shared<Stmt>(StmtExpr{e, true}));
            }
        }
    }
    expect(Tk::RBrace);
    return std::make_shared<Expr>(ExprBlock{std::move(stmts), tail});
}

ExprPtr Parser::parseIf() {
    ExprPtr cond = parseExpr();
    ExprPtr then_ = parseBlock();
    ExprPtr else_ = nullptr;
    if (match(Tk::KwElse)) {
        if (check(Tk::KwIf)) { advance(); else_ = parseIf(); }
        else else_ = parseBlock();
    }
    return std::make_shared<Expr>(ExprIf{cond, then_, else_});
}

ExprPtr Parser::parseWhile(std::optional<std::string> label) {
    ExprPtr cond = parseExpr();
    ExprPtr body = parseBlock();
    return std::make_shared<Expr>(ExprWhile{cond, body, label});
}

ExprPtr Parser::parseFor(std::optional<std::string> label) {
    PatPtr pat = parsePat();
    if (check(Tk::KwIn)) advance();
    else error("expected 'in' in for loop");
    ExprPtr iter = parseExpr();
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
            // parse condition: expr, is Type, in expr
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
        if (hasScrut && !arm.isElse) {
            // condition is a pattern-like expression for comparison
        }
        expect(Tk::Arrow);
        arm.body = parseExpr();
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

ExprPtr Parser::parseClosure() {
    expect(Tk::Pipe);
    std::vector<std::pair<std::string,bool>> params;
    while (!check(Tk::Pipe) && !atEnd()) {
        std::string name = expectIdent();
        params.emplace_back(name, false);
        if (match(Tk::Colon)) parseType();
        if (!match(Tk::Comma)) break;
    }
    expect(Tk::Pipe);
    TypePtr ret = nullptr;
    if (match(Tk::Arrow)) ret = parseType();
    ExprPtr body = parseExpr();
    return std::make_shared<Expr>(ExprClosure{std::move(params), ret, body});
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
        case Tk::StringLit: {
            std::string s = t.lex.substr(1, t.lex.size()-2);
            std::string out;
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\\' && i+1 < s.size()) {
                    char c = s[++i];
                    switch (c) {
                        case 'n': out.push_back('\n'); break;
                        case 't': out.push_back('\t'); break;
                        case 'r': out.push_back('\r'); break;
                        case '0': out.push_back('\0'); break;
                        case '\\': out.push_back('\\'); break;
                        case '"': out.push_back('"'); break;
                        case 'x': {
                            if (i+2 < s.size()) {
                                std::string hex = s.substr(i+1, 2);
                                out.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
                                i += 2;
                            }
                            break;
                        }
                        default: out.push_back(c); break;
                    }
                } else {
                    out.push_back(s[i]);
                }
            }
            return std::make_shared<Expr>(ExprLit{std::move(out)});
        }
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

// ---------- patterns ----------
PatPtr Parser::parsePat() {
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
