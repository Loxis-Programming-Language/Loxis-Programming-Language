#pragma once

#include <cstdint>
#include <string>
#include "../Error.hpp"

enum class TokenKind : uint8_t {
	// Literals
	Ident,        // identifier or keyword
	Integer,      // 42
	FloatLit,     // 3.14f or 3f
	DoubleLit,    // 3.14
	String,       // "string literal"

	// Single-char symbols
	OpenParen,    // (
	CloseParen,   // )
	OpenBrace,    // {
	CloseBrace,   // }
	Comma,        // ,
	Plus,         // +
	Minus,        // -
	StarOp,       // *
	Slash,        // /
	Equals,       // =
	Colon,        // :
	Dot,          // .
	OpenBracket,  // [
	CloseBracket, // ]

	// Multi-char operators
	EqualsEquals, // ==
	NotEquals,    // !=
	Less,         // <
	LessEq,       // <=
	Greater,      // >
	GreaterEq,    // >=
	Arrow,        // ->

	// Compound assignment
	PlusEq,       // +=
	MinusEq,      // -=
	StarEq,       // *=
	SlashEq,      // /=

	// Structural
	Newline,      // statement boundary
	Eof           // end of file
};

struct Token {
	TokenKind kind;
	std::string lexeme;
	SourceLocation loc;

	Token() : kind(TokenKind::Eof), lexeme(""), loc{"<unknown>", 0, 0} {}

	Token(TokenKind k, const std::string &lex, const SourceLocation &l)
			: kind(k), lexeme(lex), loc(l) {}
};
