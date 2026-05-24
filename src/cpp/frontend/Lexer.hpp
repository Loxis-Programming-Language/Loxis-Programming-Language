#pragma once

#include <string>
#include <vector>
#include "Token.hpp"

class Lexer {
public:
	Lexer(std::string source, std::string filename);

	std::vector <Token> tokenize();

private:
	std::string m_source;
	std::string m_filename;
	size_t m_pos = 0;
	uint32_t m_line = 1;
	uint32_t m_col = 0;

	char currentChar() const;

	char peekChar(size_t offset = 1) const;

	void advance();

	void skipLineComment();

	void skipBlockComment();

	Token lexString();

	Token lexNumberOrFloat();

	Token lexIdentOrKeyword();

	SourceLocation currentLocation() const;
};
