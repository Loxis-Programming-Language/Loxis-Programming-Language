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
Token& Parser::expect(Tk k) { if (!check(k)) error(std::string("expected ") + tkName(k)); return toks[pos++]; }
bool Parser::atEnd() { return check(Tk::Eof); }
void Parser::advance() { if (!atEnd()) ++pos; }
SourceLoc Parser::curloc() { return peek().loc; }

// ---------- basic helpers ----------
std::string Parser::expectIdent() {
    if (!check(Tk::Ident)) error("expected identifier");
    std::string s = peek().lex;
    ++pos;
    return s;
}
std::string Parser::expectIdentOrKw() {
    if (check(Tk::Ident) || check(Tk::KwSelf) || check(Tk::KwSuper)) {
        std::string s = peek().lex;
        ++pos;
        return s;
    }
    error("expected identifier");
    return "";
}
bool Parser::isItemStart() {
    switch (peek().kind) {
        case Tk::KwFn: case Tk::KwStruct: case Tk::KwEnum: case Tk::KwTrait:
        case Tk::KwImpl: case Tk::KwMod: case Tk::KwUse: case Tk::KwConst:
        case Tk::KwStatic: case Tk::KwUnsafe: case Tk::KwExtern:
        case Tk::KwPub: case Tk::KwPriv: case Tk::KwPubCrate:
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
            (void)parseType(); // const param type not stored in AST
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

Path Parser::parsePath() {
    Path p;
    p.segs.push_back(expectIdentOrKw());
    while (match(Tk::DColon)) {
        if (check(Tk::Lt)) {
            auto args = parseGenericArgs();
            p.args.insert(p.args.end(), args.begin(), args.end());
        } else {
            p.segs.push_back(expectIdentOrKw());
        }
    }
    return p;
}

Path Parser::parseTypePath() {
    Path p;
    p.segs.push_back(expectIdentOrKw());
    while (true) {
        if (match(Tk::DColon)) {
            if (check(Tk::Lt)) {
                auto args = parseGenericArgs();
                p.args.insert(p.args.end(), args.begin(), args.end());
            } else {
                p.segs.push_back(expectIdentOrKw());
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

// ---------- items ----------
ItemPtr Parser::parseItem() {
    bool pub = false;
    if (match(Tk::KwPub)) pub = true;
    else if (match(Tk::KwPubCrate)) pub = true;
    else if (match(Tk::KwPriv)) pub = false;

    bool unsafe = false, ext = false;
    if (match(Tk::KwUnsafe)) unsafe = true;
    if (match(Tk::KwExtern)) ext = true;
    if (!unsafe && match(Tk::KwUnsafe)) unsafe = true;

    if (check(Tk::KwFn)) return std::make_shared<Item>(parseFnBody(pub, unsafe, ext));
    if (check(Tk::KwStruct)) return parseStruct(pub);
    if (check(Tk::KwEnum)) return parseEnum(pub);
    if (check(Tk::KwTrait)) return parseTrait(pub);
    if (check(Tk::KwImpl)) return parseImpl();
    if (check(Tk::KwMod)) return parseMod(pub);
    if (check(Tk::KwUse)) return parseUse(pub);
    if (check(Tk::KwConst)) return parseConst(pub);
    if (check(Tk::KwStatic)) return parseStatic(pub);
    error("expected item");
    return nullptr;
}

ItemFn Parser::parseFnBody(bool pub, bool unsafe, bool ext) {
    expect(Tk::KwFn);
    std::string name = expectIdent();
    auto generics = parseGenerics();
    auto params = parseFnParams();
    TypePtr ret = parseRet();
    auto where_ = parseWhere();
    ExprPtr body = nullptr;
    if (match(Tk::Semi)) {
        // no body
    } else {
        body = parseBlock();
    }
    return ItemFn{name, pub, std::move(generics), std::move(params), ret, std::move(where_), body, unsafe, ext};
}

ItemPtr Parser::parseStruct(bool pub) {
    expect(Tk::KwStruct);
    std::string name = expectIdent();
    auto generics = parseGenerics();
    auto where_ = parseWhere();
    expect(Tk::LBrace);
    std::vector<Field> fields;
    while (!check(Tk::RBrace) && !atEnd()) {
        std::string fn = expectIdent();
        expect(Tk::Colon);
        TypePtr ty = parseType();
        std::optional<ExprPtr> def;
        if (match(Tk::Eq)) def = parseExpr();
        fields.push_back({fn, ty, def});
        if (!match(Tk::Comma)) break;
    }
    expect(Tk::RBrace);
    return std::make_shared<Item>(ItemStruct{name, pub, std::move(generics), std::move(fields), std::move(where_)});
}

ItemPtr Parser::parseEnum(bool pub) {
    expect(Tk::KwEnum);
    std::string name = expectIdent();
    auto generics = parseGenerics();
    auto where_ = parseWhere();
    expect(Tk::LBrace);
    std::vector<Variant> vars;
    while (!check(Tk::RBrace) && !atEnd()) {
        std::string vn = expectIdent();
        bool tuple = false;
        std::vector<Field> fields;
        if (check(Tk::LParen)) {
            tuple = true;
            expect(Tk::LParen);
            while (!check(Tk::RParen) && !atEnd()) {
                TypePtr ty = parseType();
                fields.push_back({"", ty, std::nullopt});
                if (!match(Tk::Comma)) break;
            }
            expect(Tk::RParen);
        } else if (check(Tk::LBrace)) {
            tuple = false;
            expect(Tk::LBrace);
            while (!check(Tk::RBrace) && !atEnd()) {
                std::string fn = expectIdent();
                expect(Tk::Colon);
                TypePtr ty = parseType();
                std::optional<ExprPtr> def;
                if (match(Tk::Eq)) def = parseExpr();
                fields.push_back({fn, ty, def});
                if (!match(Tk::Comma)) break;
            }
            expect(Tk::RBrace);
        }
        vars.push_back({vn, std::move(fields), tuple});
        if (!match(Tk::Comma)) break;
    }
    expect(Tk::RBrace);
    return std::make_shared<Item>(ItemEnum{name, pub, std::move(generics), std::move(vars), std::move(where_)});
}

ItemPtr Parser::parseTrait(bool pub) {
    expect(Tk::KwTrait);
    std::string name = expectIdent();
    auto generics = parseGenerics();
    std::vector<Path> supers;
    if (match(Tk::Colon)) {
        while (!atEnd()) {
            supers.push_back(parseTypePath());
            if (!match(Tk::Plus)) break;
        }
    }
    expect(Tk::LBrace);
    std::vector<TraitMethod> methods;
    while (!check(Tk::RBrace) && !atEnd()) {
        if (!check(Tk::KwFn)) { error("expected fn in trait"); skipToBoundary(); continue; }
        advance(); // fn
        std::string mname = expectIdent();
        auto params = parseFnParams();
        TypePtr ret = parseRet();
        ExprPtr def = nullptr;
        if (match(Tk::Semi)) {
            // no body
        } else {
            def = parseBlock();
        }
        methods.push_back({mname, std::move(params), ret, def});
    }
    expect(Tk::RBrace);
    return std::make_shared<Item>(ItemTrait{name, pub, std::move(generics), std::move(methods), std::move(supers)});
}

ItemPtr Parser::parseImpl() {
    expect(Tk::KwImpl);
    auto generics = parseGenerics();
    TypePtr ty1 = parseType();
    std::optional<Path> trait;
    TypePtr selfTy = nullptr;
    if (match(Tk::KwFor)) {
        if (std::holds_alternative<AstTyPath>(*ty1)) {
            trait = std::get<AstTyPath>(*ty1).path;
        } else {
            error("trait in impl must be a path");
        }
        selfTy = parseType();
    } else {
        selfTy = ty1;
    }
    auto where_ = parseWhere();
    expect(Tk::LBrace);
    std::vector<ItemFn> methods;
    while (!check(Tk::RBrace) && !atEnd()) {
        bool pub = false;
        if (match(Tk::KwPub)) pub = true;
        else if (match(Tk::KwPubCrate)) pub = true;
        else if (match(Tk::KwPriv)) pub = false;
        bool unsafe = match(Tk::KwUnsafe);
        bool ext = match(Tk::KwExtern);
        if (!check(Tk::KwFn)) { error("expected fn in impl"); skipToBoundary(); continue; }
        methods.push_back(parseFnBody(pub, unsafe, ext));
    }
    expect(Tk::RBrace);
    return std::make_shared<Item>(ItemImpl{std::move(generics), selfTy, trait, std::move(where_), std::move(methods)});
}

ItemPtr Parser::parseMod(bool pub) {
    expect(Tk::KwMod);
    std::string name = expectIdent();
    if (match(Tk::Semi)) {
        return std::make_shared<Item>(ItemMod{name, pub, {}, false});
    }
    expect(Tk::LBrace);
    std::vector<ItemPtr> items;
    while (!check(Tk::RBrace) && !atEnd()) {
        if (auto it = parseItem()) items.push_back(it);
        else skipToBoundary();
    }
    expect(Tk::RBrace);
    return std::make_shared<Item>(ItemMod{name, pub, std::move(items), true});
}

ItemPtr Parser::parseUse(bool pub) {
    expect(Tk::KwUse);
    UseTree tree = parseUseTree();
    expect(Tk::Semi);
    return std::make_shared<Item>(ItemUse{pub, tree});
}

UseTree Parser::parseUseTree() {
    Path path;
    path.segs.push_back(expectIdentOrKw());
    while (match(Tk::DColon)) {
        if (check(Tk::Star)) {
            advance();
            return UseTree{UseTree::Glob, std::move(path)};
        }
        if (check(Tk::LBrace)) {
            advance();
            std::vector<UseTree> nested;
            while (!check(Tk::RBrace) && !atEnd()) {
                nested.push_back(parseUseTree());
                if (!match(Tk::Comma)) break;
            }
            expect(Tk::RBrace);
            return UseTree{UseTree::Nested, std::move(path), std::move(nested)};
        }
        path.segs.push_back(expectIdentOrKw());
    }
    if (match(Tk::KwAs)) {
        std::string rename = expectIdent();
        return UseTree{UseTree::Simple, std::move(path), {}, rename};
    }
    return UseTree{UseTree::Simple, std::move(path)};
}

ItemPtr Parser::parseConst(bool pub) {
    expect(Tk::KwConst);
    std::string name = expectIdent();
    expect(Tk::Colon);
    TypePtr ty = parseType();
    expect(Tk::Eq);
    ExprPtr init = parseExpr();
    expect(Tk::Semi);
    return std::make_shared<Item>(ItemConst{name, pub, ty, init});
}

ItemPtr Parser::parseStatic(bool pub) {
    expect(Tk::KwStatic);
    bool mut = match(Tk::KwMut);
    std::string name = expectIdent();
    expect(Tk::Colon);
    TypePtr ty = parseType();
    expect(Tk::Eq);
    ExprPtr init = parseExpr();
    expect(Tk::Semi);
    return std::make_shared<Item>(ItemStatic{name, pub, ty, init, mut});
}

// ---------- statements ----------
StmtPtr Parser::parseStmt() {
    if (check(Tk::KwLet)) return parseLet();
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
    if (!match(Tk::Semi)) {
        if (!check(Tk::RBrace)) error("expected ';' after let");
    }
    return std::make_shared<Stmt>(StmtLet{pat, ty, init});
}

StmtPtr Parser::parseItemStmt() {
    ItemPtr it = parseItem();
    return std::make_shared<Stmt>(StmtItem{it});
}

// ---------- expressions ----------
ExprPtr Parser::parseExpr() { return parseAssign(); }

ExprPtr Parser::parseAssign() {
    ExprPtr lhs = parseRange();
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

ExprPtr Parser::parseRange() {
    ExprPtr lhs = parseOr();
    if (match(Tk::DotDot)) {
        ExprPtr rhs = parseOr();
        // AST lacks ExprRange; reuse ExprBinary with out-of-range op id
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
        else break;
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
    ExprPtr lhs = parseCast();
    while (true) {
        ExprBinary::Op op;
        if (match(Tk::Star)) op = ExprBinary::Mul;
        else if (match(Tk::Slash)) op = ExprBinary::Div;
        else if (match(Tk::Percent)) op = ExprBinary::Rem;
        else break;
        ExprPtr rhs = parseCast();
        lhs = std::make_shared<Expr>(ExprBinary{op, lhs, rhs});
    }
    return lhs;
}
ExprPtr Parser::parseCast() {
    ExprPtr lhs = parsePrefix();
    while (match(Tk::KwAs)) {
        TypePtr ty = parseType();
        lhs = std::make_shared<Expr>(ExprCast{lhs, ty});
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
        bool mut = match(Tk::KwMut);
        ExprPtr op = parsePrefix();
        return std::make_shared<Expr>(ExprUnary{mut ? ExprUnary::RefMut : ExprUnary::Ref, op});
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
        } else if (check(Tk::LBracket)) {
            advance();
            ExprPtr idx = parseExpr();
            expect(Tk::RBracket);
            expr = std::make_shared<Expr>(ExprIndex{expr, idx});
        } else if (match(Tk::Question)) {
            expr = std::make_shared<Expr>(ExprTry{expr});
        } else {
            break;
        }
    }
    return expr;
}

ExprPtr Parser::parsePrimary() {
    if (check(Tk::IntLit) || check(Tk::FloatLit) || check(Tk::StringLit) || check(Tk::CharLit) || check(Tk::KwTrue) || check(Tk::KwFalse))
        return parseLiteral();

    if (check(Tk::Ident) || check(Tk::KwSelf) || check(Tk::KwSuper)) {
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
    if (match(Tk::KwMatch)) return parseMatch();
    if (match(Tk::KwReturn)) return parseReturn();
    if (match(Tk::KwBreak)) return parseBreak();
    if (match(Tk::KwContinue)) return parseContinue();
    if (check(Tk::Pipe)) return parseClosure();
    if (match(Tk::KwUnsafe)) return parseBlock();

    error("unexpected token in expression");
    return std::make_shared<Expr>(ExprLit{0});
}

ExprPtr Parser::parseBlock() {
    expect(Tk::LBrace);
    std::vector<StmtPtr> stmts;
    ExprPtr tail = nullptr;
    while (!check(Tk::RBrace) && !atEnd()) {
        if (check(Tk::KwLet)) {
            stmts.push_back(parseLet());
        } else if (isItemStart()) {
            stmts.push_back(parseItemStmt());
        } else {
            ExprPtr e = parseExpr();
            if (match(Tk::Semi)) {
                stmts.push_back(std::make_shared<Stmt>(StmtExpr{e, true}));
            } else {
                tail = e;
                break;
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
    if (check(Tk::Ident) && peek().lex == "in") advance();
    else error("expected 'in' in for loop");
    ExprPtr iter = parseExpr();
    ExprPtr body = parseBlock();
    return std::make_shared<Expr>(ExprFor{pat, iter, body, label});
}

ExprPtr Parser::parseLoop(std::optional<std::string> label) {
    ExprPtr body = parseBlock();
    return std::make_shared<Expr>(ExprLoop{body, label});
}

ExprPtr Parser::parseMatch() {
    ExprPtr scrut = parseExpr();
    expect(Tk::LBrace);
    std::vector<std::pair<PatPtr, ExprPtr>> arms;
    while (!check(Tk::RBrace) && !atEnd()) {
        PatPtr pat = parsePat();
        expect(Tk::FatArrow);
        ExprPtr body = parseExpr();
        arms.emplace_back(pat, body);
        if (!match(Tk::Comma)) break;
    }
    expect(Tk::RBrace);
    return std::make_shared<Expr>(ExprMatch{scrut, std::move(arms)});
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
        bool mut = match(Tk::KwMut);
        std::string name = expectIdent();
        params.emplace_back(name, mut);
        if (match(Tk::Colon)) parseType(); // discard type annotation
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
            if (!check(Tk::RBrace)) parseExpr(); // discard base expr
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
            static const char* suffs[] = {"i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","isize","usize"};
            for (const char* su : suffs) {
                size_t n = std::strlen(su);
                if (s.size() > n && s.compare(s.size()-n, n, su) == 0) {
                    s = s.substr(0, s.size()-n);
                    break;
                }
            }
            s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
            int base = 10;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s = s.substr(2); }
            else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s = s.substr(2); }
            else if (s.size() > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) { base = 8; s = s.substr(2); }
            bool u = false;
            for (const char* su : suffs) if (t.lex.size() >= std::strlen(su) && t.lex.compare(t.lex.size()-std::strlen(su), std::strlen(su), su) == 0) { if (su[0]=='u') u=true; break; }
            if (u) {
                uint64_t v = std::stoull(s, nullptr, base);
                return std::make_shared<Expr>(ExprLit{v});
            } else {
                int64_t v = std::stoll(s, nullptr, base);
                return std::make_shared<Expr>(ExprLit{v});
            }
        }
        case Tk::FloatLit: {
            std::string s = t.lex;
            if (s.size() > 3 && (s.compare(s.size()-3, 3, "f32") == 0 || s.compare(s.size()-3, 3, "f64") == 0))
                s = s.substr(0, s.size()-3);
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

// ---------- patterns ----------
PatPtr Parser::parsePat() {
    if (match(Tk::Underscore)) return std::make_shared<Pat>(PatWild{});
    if (match(Tk::KwMut)) {
        std::string name = expectIdent();
        return std::make_shared<Pat>(PatBind{name, true, nullptr});
    }
    if (match(Tk::Amp)) {
        bool mut = match(Tk::KwMut);
        PatPtr sub = parsePat();
        return std::make_shared<Pat>(PatRef{mut, sub});
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
    if (check(Tk::IntLit) || check(Tk::FloatLit) || check(Tk::StringLit) || check(Tk::CharLit) || check(Tk::KwTrue) || check(Tk::KwFalse))
        return std::make_shared<Pat>(PatLit{parseLiteral()});

    if (check(Tk::Ident) || check(Tk::KwSelf) || check(Tk::KwSuper)) {
        Path path;
        path.segs.push_back(expectIdentOrKw());
        while (match(Tk::DColon)) {
            if (check(Tk::LParen) || check(Tk::LBrace)) break;
            path.segs.push_back(expectIdentOrKw());
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
    if (match(Tk::Bang)) return std::make_shared<Type>(AstTyNever{});
    if (match(Tk::Amp)) {
        bool mut = match(Tk::KwMut);
        TypePtr elem = parseType();
        return std::make_shared<Type>(AstTyRef{mut, elem});
    }
    if (match(Tk::Star)) {
        bool mut = false;
        if (match(Tk::KwMut)) mut = true;
        else if (match(Tk::KwConst)) mut = false;
        TypePtr elem = parseType();
        return std::make_shared<Type>(AstTyPtr{mut, elem});
    }
    if (check(Tk::LBracket)) {
        advance();
        TypePtr elem = parseType();
        if (match(Tk::Semi)) {
            ExprPtr size = parseExpr();
            expect(Tk::RBracket);
            return std::make_shared<Type>(AstTyArray{elem, size});
        }
        expect(Tk::RBracket);
        return std::make_shared<Type>(AstTySlice{elem});
    }
    if (check(Tk::LParen)) {
        advance();
        if (match(Tk::RParen)) return std::make_shared<Type>(AstTyTuple{std::vector<TypePtr>{}});
        std::vector<TypePtr> elems;
        elems.push_back(parseType());
        while (match(Tk::Comma)) {
            if (check(Tk::RParen)) break;
            elems.push_back(parseType());
        }
        expect(Tk::RParen);
        if (elems.size() == 1) return elems[0];
        return std::make_shared<Type>(AstTyTuple{std::move(elems)});
    }
    if (match(Tk::KwFn)) {
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
        return std::make_shared<Type>(AstTyFn{std::move(params), ret});
    }
    Path p = parseTypePath();
    return std::make_shared<Type>(AstTyPath{p});
}

} // namespace loxis::v2
