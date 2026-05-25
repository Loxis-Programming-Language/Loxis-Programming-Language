#include "Lexer.hpp"
#include <cctype>
#include <utility>
#include <unordered_map>

#define panic(msg) throw LexError(msg, currentLocation());

Lexer::Lexer(std::string source, std::string filename)
		: m_source(std::move(source)), m_filename(std::move(filename)), m_pos(0), m_line(1), m_col(0) {}

std::vector <Token> Lexer::tokenize() {
	std::vector <Token> tokens;

	while (m_pos < m_source.size()) {
		char c = currentChar();

		// Whitespace (spaces/tabs) — skip
		if (c == ' ' || c == '\t') {
			advance();
			continue;
		}

		// Newline (handle LF, CR, CRLF)
		if (c == '\n') {
			advance();
			m_line++;
			m_col = 0;
			tokens.emplace_back(TokenKind::Newline, "\n", currentLocation());
			continue;
		}
		if (c == '\r') {
			advance();
			if (currentChar() == '\n') advance(); // CRLF
			m_line++;
			m_col = 0;
			tokens.emplace_back(TokenKind::Newline, "\n", currentLocation());
			continue;
		}

		// Line comment //
		if (c == '/' && peekChar() == '/') {
			skipLineComment();
			continue;
		}

		// Block comment /* */
		if (c == '/' && peekChar() == '*') {
			skipBlockComment();
			continue;
		}

		// Multi-char operators (must check before single-char)
		if (c == '-' && peekChar() == '>') {
			tokens.emplace_back(TokenKind::Arrow, "->", currentLocation());
			advance();
			advance();
			continue;
		}
		if (c == '=' && peekChar() == '=') {
			tokens.emplace_back(TokenKind::EqualsEquals, "==", currentLocation());
			advance();
			advance();
			continue;
		}
		if (c == '!' && peekChar() == '=') {
			tokens.emplace_back(TokenKind::NotEquals, "!=", currentLocation());
			advance();
			advance();
			continue;
		}
		if (c == '<' && peekChar() == '=') {
			tokens.emplace_back(TokenKind::LessEq, "<=", currentLocation());
			advance();
			advance();
			continue;
		}
		if (c == '>' && peekChar() == '=') {
			tokens.emplace_back(TokenKind::GreaterEq, ">=", currentLocation());
			advance();
			advance();
			continue;
		}
		if (c == '+' && peekChar() == '=') {
			tokens.emplace_back(TokenKind::PlusEq, "+=", currentLocation());
			advance();
			advance();
			continue;
		}
		if (c == '-' && peekChar() == '=') {
			tokens.emplace_back(TokenKind::MinusEq, "-=", currentLocation());
			advance();
			advance();
			continue;
		}
		if (c == '*' && peekChar() == '=') {
			tokens.emplace_back(TokenKind::StarEq, "*=", currentLocation());
			advance();
			advance();
			continue;
		}
		if (c == '/' && peekChar() == '=') {
			tokens.emplace_back(TokenKind::SlashEq, "/=", currentLocation());
			advance();
			advance();
			continue;
		}

		// Single-char symbols
		switch (c) {
			case '(':
				tokens.emplace_back(TokenKind::OpenParen, "(", currentLocation());
				advance();
				continue;
			case ')':
				tokens.emplace_back(TokenKind::CloseParen, ")", currentLocation());
				advance();
				continue;
			case '{':
				tokens.emplace_back(TokenKind::OpenBrace, "{", currentLocation());
				advance();
				continue;
			case '}':
				tokens.emplace_back(TokenKind::CloseBrace, "}", currentLocation());
				advance();
				continue;
			case ',':
				tokens.emplace_back(TokenKind::Comma, ",", currentLocation());
				advance();
				continue;
			case '+':
				tokens.emplace_back(TokenKind::Plus, "+", currentLocation());
				advance();
				continue;
			case '-':
				tokens.emplace_back(TokenKind::Minus, "-", currentLocation());
				advance();
				continue;
			case '*':
				tokens.emplace_back(TokenKind::StarOp, "*", currentLocation());
				advance();
				continue;
			case '/':
				tokens.emplace_back(TokenKind::Slash, "/", currentLocation());
				advance();
				continue;
			case '=':
				tokens.emplace_back(TokenKind::Equals, "=", currentLocation());
				advance();
				continue;
			case ':':
				tokens.emplace_back(TokenKind::Colon, ":", currentLocation());
				advance();
				continue;
			case '.':
				tokens.emplace_back(TokenKind::Dot, ".", currentLocation());
				advance();
				continue;
			case '[':
				tokens.emplace_back(TokenKind::OpenBracket, "[", currentLocation());
				advance();
				continue;
			case ']':
				tokens.emplace_back(TokenKind::CloseBracket, "]", currentLocation());
				advance();
				continue;
			case '<':
				tokens.emplace_back(TokenKind::Less, "<", currentLocation());
				advance();
				continue;
			case '>':
				tokens.emplace_back(TokenKind::Greater, ">", currentLocation());
				advance();
				continue;
		}

		// String
		if (c == '"') {
			tokens.push_back(lexString());
			continue;
		}

		// Number
		if (std::isdigit(c)) {
			tokens.push_back(lexNumberOrFloat());
			continue;
		}

		// Identifier or keyword
		if (std::isalpha(c) || c == '_') {
			tokens.push_back(lexIdentOrKeyword());
			continue;
		}

		panic("unexpected character: " + std::string(1, c));
	}

	tokens.emplace_back(TokenKind::Eof, "", currentLocation());
	return tokens;
}

char Lexer::currentChar() const {
	return (m_pos < m_source.size()) ? m_source[m_pos] : '\0';
}

char Lexer::peekChar(size_t offset) const {
	return (m_pos + offset < m_source.size()) ? m_source[m_pos + offset] : '\0';
}

void Lexer::advance() {
	if (m_pos < m_source.size()) {
		m_pos++;
		m_col++;
	}
}

void Lexer::skipLineComment() {
	advance();
	advance();  // skip //
	while (m_pos < m_source.size() && currentChar() != '\n') {
		advance();
	}
}

void Lexer::skipBlockComment() {
	advance();
	advance();  // skip /*
	while (m_pos < m_source.size()) {
		if (currentChar() == '*' && peekChar() == '/') {
			advance();
			advance();
			return;
		}
		if (currentChar() == '\n') {
			m_line++;
			m_col = 0;
		}
		advance();
	}
	panic("unterminated block comment");
}

Token Lexer::lexString() {
	SourceLocation loc = currentLocation();
	advance();  // skip opening "
	std::string value;

	while (m_pos < m_source.size() && currentChar() != '"') {
		char c = currentChar();
		if (c == '\n') {
			panic("unterminated string literal");
		}
		if (c == '\\' && m_pos + 1 < m_source.size()) {
			advance();
			char next = currentChar();
			switch (next) {
				case 'n':
					value += '\n';
					break;
				case 't':
					value += '\t';
					break;
				case '"':
					value += '"';
					break;
				case '\\':
					value += '\\';
					break;
				default:
					value += next;
					break;
			}
			advance();
		} else {
			value += c;
			advance();
		}
	}

	if (m_pos >= m_source.size()) panic("unterminated string");
	advance();  // skip closing "
	return {TokenKind::String, value, loc};
}

Token Lexer::lexNumberOrFloat() {
	SourceLocation loc = currentLocation();
	std::string value;
	bool hasDot = false;

	// Integer part (or leading digits before dot)
	while (m_pos < m_source.size() && std::isdigit(currentChar())) {
		value += currentChar();
		advance();
	}

	// Fractional part: .digits
	if (currentChar() == '.' && std::isdigit(peekChar())) {
		hasDot = true;
		value += currentChar();  // '.'
		advance();
		while (m_pos < m_source.size() && std::isdigit(currentChar())) {
			value += currentChar();
			advance();
		}
	}

	// Float suffix
	if (currentChar() == 'f' || currentChar() == 'F') {
		advance();
		return Token(TokenKind::FloatLit, value, loc);
	}

	if (hasDot)
		return Token(TokenKind::DoubleLit, value, loc);

	return Token(TokenKind::Integer, value, loc);
}

Token Lexer::lexIdentOrKeyword() {
	SourceLocation loc = currentLocation();
	std::string value;

	while (m_pos < m_source.size() && (std::isalnum(currentChar()) || currentChar() == '_')) {
		value += currentChar();
		advance();
	}

	static const std::unordered_map<std::string, TokenKind> keywords = {
		{"fun",    TokenKind::KwFun},
		{"class",  TokenKind::KwClass},
		{"var",    TokenKind::KwVar},
		{"val",    TokenKind::KwVal},
		{"if",     TokenKind::KwIf},
		{"else",   TokenKind::KwElse},
		{"while",  TokenKind::KwWhile},
		{"return", TokenKind::KwReturn},
		{"print",  TokenKind::KwPrint},
		{"true",   TokenKind::KwTrue},
		{"false",  TokenKind::KwFalse},
		{"None",   TokenKind::KwNone},
	};

	auto it = keywords.find(value);
	if (it != keywords.end()) return Token(it->second, value, loc);
	return Token(TokenKind::Ident, value, loc);
}

SourceLocation Lexer::currentLocation() const {
	return {m_filename, m_line, m_col};
}
