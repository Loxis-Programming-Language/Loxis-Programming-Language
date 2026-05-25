#pragma once
#include <cstdint>
#include <string>

namespace loxis::v2 {

struct SourceLoc {
    std::string file;
    uint32_t line = 0, col = 0;
    std::string fmt() const { return file + ":" + std::to_string(line) + ":" + std::to_string(col); }
};

enum class Tk : uint16_t {
    Eof,
    // literals
    Ident, IntLit, FloatLit, StringLit, CharLit,
    // keywords
    KwMod, KwUse, KwFn, KwLet, KwMut, KwStruct, KwEnum, KwTrait, KwImpl, KwFor, KwWhile, KwIf, KwElse, KwMatch,
    KwReturn, KwBreak, KwContinue, KwLoop, KwTrue, KwFalse, KwSelf, KwSuper, KwExtern, KwUnsafe, KwConst, KwStatic,
    KwAs, KwWhere, KwType, KwRef, KwBox, KwPub, KwPriv, KwPubCrate,
    // punct
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Dot, DotDot, DotDotEq, Colon, DColon, Semi, Arrow, FatArrow, Pound, Dollar, At,
    Plus, Minus, Star, Slash, Percent,
    Amp, Pipe, Caret, Tilde, Bang, Question,
    Lt, Gt, Le, Ge, Eq, EqEq, Ne,
    AndAnd, OrOr,
    Shl, Shr,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq, AmpEq, PipeEq, CaretEq, ShlEq, ShrEq,
    // newline (significant whitespace for v1 compat, but v2 uses semi mostly)
    Newline,
    // misc
    Underscore,
};

struct Token {
    Tk kind;
    std::string lex;
    SourceLoc loc;
    Token() : kind(Tk::Eof) {}
    Token(Tk k, std::string l, SourceLoc s) : kind(k), lex(std::move(l)), loc(std::move(s)) {}
};

inline const char* tkName(Tk k) {
    switch(k) {
    case Tk::Eof: return "EOF";
    case Tk::Ident: return "identifier";
    case Tk::IntLit: return "integer";
    case Tk::FloatLit: return "float";
    case Tk::StringLit: return "string";
    case Tk::CharLit: return "char";
    case Tk::KwMod: return "'mod'";
    case Tk::KwUse: return "'use'";
    case Tk::KwFn: return "'fn'";
    case Tk::KwLet: return "'let'";
    case Tk::KwMut: return "'mut'";
    case Tk::KwStruct: return "'struct'";
    case Tk::KwEnum: return "'enum'";
    case Tk::KwTrait: return "'trait'";
    case Tk::KwImpl: return "'impl'";
    case Tk::KwFor: return "'for'";
    case Tk::KwWhile: return "'while'";
    case Tk::KwIf: return "'if'";
    case Tk::KwElse: return "'else'";
    case Tk::KwMatch: return "'match'";
    case Tk::KwReturn: return "'return'";
    case Tk::KwBreak: return "'break'";
    case Tk::KwContinue: return "'continue'";
    case Tk::KwLoop: return "'loop'";
    case Tk::KwTrue: return "'true'";
    case Tk::KwFalse: return "'false'";
    case Tk::KwSelf: return "'self'";
    case Tk::KwSuper: return "'super'";
    case Tk::KwExtern: return "'extern'";
    case Tk::KwUnsafe: return "'unsafe'";
    case Tk::KwConst: return "'const'";
    case Tk::KwStatic: return "'static'";
    case Tk::KwAs: return "'as'";
    case Tk::KwWhere: return "'where'";
    case Tk::KwType: return "'type'";
    case Tk::KwRef: return "'ref'";
    case Tk::KwBox: return "'box'";
    case Tk::KwPub: return "'pub'";
    case Tk::KwPriv: return "'priv'";
    case Tk::KwPubCrate: return "'pub(crate)'";
    case Tk::LParen: return "'('";
    case Tk::RParen: return "')'";
    case Tk::LBrace: return "'{'";
    case Tk::RBrace: return "'}'";
    case Tk::LBracket: return "'['";
    case Tk::RBracket: return "']'";
    case Tk::Comma: return "','";
    case Tk::Dot: return "'.'";
    case Tk::DotDot: return "'..'";
    case Tk::DotDotEq: return "'..='";
    case Tk::Colon: return "':'";
    case Tk::DColon: return "'::'";
    case Tk::Semi: return "';'";
    case Tk::Arrow: return "'->'";
    case Tk::FatArrow: return "'=>'";
    case Tk::Pound: return "'#'";
    case Tk::Dollar: return "'$'";
    case Tk::At: return "'@'";
    case Tk::Plus: return "'+'";
    case Tk::Minus: return "'-'";
    case Tk::Star: return "'*'";
    case Tk::Slash: return "'/'";
    case Tk::Percent: return "'%'";
    case Tk::Amp: return "'&'";
    case Tk::Pipe: return "'|'";
    case Tk::Caret: return "'^'";
    case Tk::Tilde: return "'~'";
    case Tk::Bang: return "'!'";
    case Tk::Question: return "'?'";
    case Tk::Lt: return "'<'";
    case Tk::Gt: return "'>'";
    case Tk::Le: return "'<='";
    case Tk::Ge: return "'>='";
    case Tk::Eq: return "'='";
    case Tk::EqEq: return "'=='";
    case Tk::Ne: return "'!='";
    case Tk::AndAnd: return "'&&'";
    case Tk::OrOr: return "'||'";
    case Tk::Shl: return "'<<'";
    case Tk::Shr: return "'>>'";
    case Tk::PlusEq: return "'+='";
    case Tk::MinusEq: return "'-='";
    case Tk::StarEq: return "'*='";
    case Tk::SlashEq: return "'/='";
    case Tk::PercentEq: return "'%='";
    case Tk::AmpEq: return "'&='";
    case Tk::PipeEq: return "'|='";
    case Tk::CaretEq: return "'^='";
    case Tk::ShlEq: return "'<<='";
    case Tk::ShrEq: return "'>>='";
    case Tk::Newline: return "newline";
    case Tk::Underscore: return "'_'";
    }
    return "<?>";
}

} // namespace
