#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include "Token.hpp"
#include "AST.hpp"

class Parser {
public:
	static AST parseFile(const std::string &path);

	explicit Parser(std::vector <Token> tokens);

	AST parse();

private:
	std::vector <Token> m_tokens;
	size_t m_pos = 0;

	Token peek() const;

	Token peekAhead(size_t offset) const;

	Token advance();

	bool match(TokenKind kind);

	void expect(TokenKind kind);

	bool isKeyword(const std::string &kw) const;

	// Newline handling: newlines are statement separators, but inside {} they're whitespace
	void skipNewlines();

	// Check if current token is a statement-starter keyword
	bool isStmtStart() const;

	// Scope tracking — which variables are declared in current scope
	std::unordered_set <std::string> m_declaredVars;

	// Top level
	FunDeclNode parseFunDecl();

	ClassDeclNode parseClassDecl();

	// Statements
	std::shared_ptr <Stmt> parseStatement();

	std::shared_ptr <Stmt> parseVarDecl();

	std::shared_ptr <Stmt> parseIfStmt();

	std::shared_ptr <Stmt> parseWhileStmt();

	std::shared_ptr <Stmt> parseReturnStmt();

	std::shared_ptr <Stmt> parsePrintStmt();

	std::shared_ptr <Stmt> parseAssignOrExpr();

	std::vector <std::shared_ptr<Stmt>> parseBlock();

	// Expressions (Pratt parser)
	std::shared_ptr <Expr> parseExpr();

	std::shared_ptr <Expr> parseExprPrec(int minPrec);

	std::shared_ptr <Expr> parsePrimary();

	std::shared_ptr <Expr> parseCall(const std::string &name, SourceLocation loc);

	// Precedence helpers
	static int precOf(TokenKind kind);
};
