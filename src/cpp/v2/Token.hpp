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
    KwPackage, KwImport, KwFrom,
    KwFun, KwLet, KwVar, KwVal,
    KwClass, KwInterface, KwObject, KwEnum,
    KwOpen, KwAbstract, KwData, KwOverride,
    KwInit, KwCompanion,
    KwFor, KwWhile, KwIf, KwElse, KwWhen,
    KwReturn, KwBreak, KwContinue, KwLoop,
    KwTrue, KwFalse, KwNull,
    KwIs, KwIn, KwAs,
    KwWhere, KwType, KwConst,
    KwPublic, KwInternal, KwPrivate,
    // punct
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Dot, DotDot, DotDotEq, Colon, DColon, Semi, Arrow, FatArrow, Pound, Dollar, At,
    Plus, Minus, Star, Slash, Percent,
    Amp, Pipe, Caret, Tilde, Bang, Question,
    Lt, Gt, Le, Ge, Eq, EqEq, Ne,
    AndAnd, OrOr,
    Shl, Shr,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq, AmpEq, PipeEq, CaretEq, ShlEq, ShrEq,
    // newline
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
    case Tk::KwPackage: return "'package'";
    case Tk::KwImport: return "'import'";
    case Tk::KwFrom: return "'from'";
    case Tk::KwFun: return "'fun'";
    case Tk::KwLet: return "'let'";
    case Tk::KwVar: return "'var'";
    case Tk::KwVal: return "'val'";
    case Tk::KwClass: return "'class'";
    case Tk::KwInterface: return "'interface'";
    case Tk::KwObject: return "'object'";
    case Tk::KwEnum: return "'enum'";
    case Tk::KwOpen: return "'open'";
    case Tk::KwAbstract: return "'abstract'";
    case Tk::KwData: return "'data'";
    case Tk::KwOverride: return "'override'";
    case Tk::KwInit: return "'init'";
    case Tk::KwCompanion: return "'companion'";
    case Tk::KwFor: return "'for'";
    case Tk::KwWhile: return "'while'";
    case Tk::KwIf: return "'if'";
    case Tk::KwElse: return "'else'";
    case Tk::KwWhen: return "'when'";
    case Tk::KwReturn: return "'return'";
    case Tk::KwBreak: return "'break'";
    case Tk::KwContinue: return "'continue'";
    case Tk::KwLoop: return "'loop'";
    case Tk::KwTrue: return "'true'";
    case Tk::KwFalse: return "'false'";
    case Tk::KwNull: return "'null'";
    case Tk::KwIs: return "'is'";
    case Tk::KwIn: return "'in'";
    case Tk::KwAs: return "'as'";
    case Tk::KwWhere: return "'where'";
    case Tk::KwType: return "'type'";
    case Tk::KwConst: return "'const'";
    case Tk::KwPublic: return "'public'";
    case Tk::KwInternal: return "'internal'";
    case Tk::KwPrivate: return "'private'";
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
