#include "Parser.hpp"
#include "Lexer.hpp"
#include <fstream>
#include <sstream>

AST Parser::parseFile(const std::string &path) {
	std::ifstream file(path);
	if (!file.is_open())
		throw ParseError("cannot open file: " + path);
	std::stringstream buffer;
	buffer << file.rdbuf();
	std::string source = buffer.str();
	Lexer lexer(source, path);
	auto tokens = lexer.tokenize();
	Parser parser(tokens);
	return parser.parse();
}

Parser::Parser(std::vector <Token> tokens)
		: m_tokens(std::move(tokens)), m_pos(0) {}

AST Parser::parse() {
	AST ast;
	skipNewlines();
	while (peek().kind != TokenKind::Eof) {
		if (peek().kind == TokenKind::Ident && peek().lexeme == "fun")
			ast.nodes.push_back(parseFunDecl());
		else if (peek().kind == TokenKind::Ident && peek().lexeme == "class")
			ast.nodes.push_back(parseClassDecl());
		else
			throw ParseError("expected 'fun' or 'class' declaration", peek().loc);
		skipNewlines();
	}
	return ast;
}

Token Parser::peek() const {
	return (m_pos < m_tokens.size()) ? m_tokens[m_pos] : Token(TokenKind::Eof, "", {});
}

Token Parser::peekAhead(size_t offset) const {
	size_t idx = m_pos + offset;
	return (idx < m_tokens.size()) ? m_tokens[idx] : Token(TokenKind::Eof, "", {});
}

Token Parser::advance() {
	Token current = peek();
	if (m_pos < m_tokens.size()) m_pos++;
	return current;
}

bool Parser::match(TokenKind kind) {
	if (peek().kind == kind) {
		advance();
		return true;
	}
	return false;
}

void Parser::expect(TokenKind kind) {
	if (!match(kind))
		throw ParseError("expected kind " + std::to_string((int) kind) +
		                 ", got '" + peek().lexeme + "' (kind " + std::to_string((int) peek().kind) + ")",
		                 peek().loc);
}

bool Parser::isKeyword(const std::string &kw) const {
	return peek().kind == TokenKind::Ident && peek().lexeme == kw;
}

void Parser::skipNewlines() {
	while (peek().kind == TokenKind::Newline) advance();
}

bool Parser::isStmtStart() const {
	if (peek().kind == TokenKind::Ident) {
		const auto &kw = peek().lexeme;
		return kw == "var" || kw == "val" || kw == "if" || kw == "while" ||
		       kw == "return" || kw == "print";
	}
	return false;
}

// ==================== Top Level ====================

FunDeclNode Parser::parseFunDecl() {
	advance();  // consume "fun"
	SourceLocation loc = peek().loc;
	if (peek().kind != TokenKind::Ident)
		throw ParseError("expected function name", peek().loc);
	std::string name = advance().lexeme;

	expect(TokenKind::OpenParen);
	std::vector <ParamDecl> params;
	if (peek().kind != TokenKind::CloseParen) {
		do {
			if (peek().kind != TokenKind::Ident)
				throw ParseError("expected parameter name", peek().loc);
			ParamDecl p;
			p.name = advance().lexeme;
			// mandatory type annotation
			if (!match(TokenKind::Colon))
				throw ParseError("parameter '" + p.name + "' requires type annotation", peek().loc);
			if (peek().kind != TokenKind::Ident)
				throw ParseError("expected type after ':'", peek().loc);
			p.type = advance().lexeme;
			params.push_back(std::move(p));
		} while (match(TokenKind::Comma));
	}
	expect(TokenKind::CloseParen);

	FunDeclNode node;
	m_declaredVars.clear();
	node.name = name;
	node.params = std::move(params);
	node.loc = loc;

	// mandatory return type
	if (!match(TokenKind::Arrow))
		throw ParseError("function '" + name + "' requires return type annotation", peek().loc);
	if (peek().kind != TokenKind::Ident)
		throw ParseError("expected return type", peek().loc);
	node.returnType = advance().lexeme;

	if (match(TokenKind::Equals)) {
		node.body.push_back(std::make_shared<Stmt>(ReturnStmtNode{parseExpr(), loc}));
	} else {
		expect(TokenKind::OpenBrace);
		skipNewlines();
		while (peek().kind != TokenKind::CloseBrace && peek().kind != TokenKind::Eof) {
			node.body.push_back(parseStatement());
			skipNewlines();
		}
		expect(TokenKind::CloseBrace);
	}
	return node;
}

ClassDeclNode Parser::parseClassDecl() {
	advance();  // consume "class"
	SourceLocation loc = peek().loc;
	if (peek().kind != TokenKind::Ident)
		throw ParseError("expected class name", peek().loc);
	std::string name = advance().lexeme;

	ClassDeclNode node;
	m_declaredVars.clear();
	node.name = name;
	node.loc = loc;

	expect(TokenKind::OpenBrace);
	skipNewlines();

	// Parse class body: fields (var/val without initializer) and methods (fun)
	while (peek().kind != TokenKind::CloseBrace && peek().kind != TokenKind::Eof) {
		if (peek().kind == TokenKind::Ident) {
			const auto &kw = peek().lexeme;
			if (kw == "var" || kw == "val") {
				// Field: var name: type or var name: type = default
				advance();  // consume var/val
				if (peek().kind != TokenKind::Ident)
					throw ParseError("expected field name", peek().loc);
				FieldDecl field;
				field.name = advance().lexeme;
				if (match(TokenKind::Colon)) {
					if (peek().kind != TokenKind::Ident)
						throw ParseError("expected type after ':'", peek().loc);
					field.type = advance().lexeme;
				}
				if (match(TokenKind::Equals))
					field.init = parseExpr();
				node.fields.push_back(std::move(field));
			} else if (kw == "fun") {
				node.methods.push_back(parseFunDecl());
			} else {
				throw ParseError("expected field or method in class body", peek().loc);
			}
		} else {
			throw ParseError("expected field or method in class body", peek().loc);
		}
		skipNewlines();
	}
	expect(TokenKind::CloseBrace);

	return node;
}

// ==================== Statements ====================

std::shared_ptr <Stmt> Parser::parseStatement() {
	skipNewlines();
	Token t = peek();
	if (t.kind == TokenKind::Ident) {
		const auto &kw = t.lexeme;
		if (kw == "var" || kw == "val") return parseVarDecl();
		if (kw == "if") return parseIfStmt();
		if (kw == "while") return parseWhileStmt();
		if (kw == "return") return parseReturnStmt();
		if (kw == "print") return parsePrintStmt();
		return parseAssignOrExpr();
	}
	throw ParseError("expected statement", t.loc);
}

std::shared_ptr <Stmt> Parser::parseVarDecl() {
	bool mut = advance().lexeme == "var";
	SourceLocation loc = peek().loc;
	if (peek().kind != TokenKind::Ident)
		throw ParseError("expected variable name", peek().loc);
	std::string name = advance().lexeme;

	std::string typeAnnot;
	if (match(TokenKind::Colon)) {
		if (peek().kind != TokenKind::Ident)
			throw ParseError("expected type after ':'", peek().loc);
		typeAnnot = advance().lexeme;
	}

	auto node = std::make_shared<VarDeclNode>();
	node->mut = mut;
	node->name = name;
	node->typeAnnot = typeAnnot;
	m_declaredVars.insert(name);
	node->loc = loc;
	if (match(TokenKind::Equals))
		node->init = parseExpr();
	return std::make_shared<Stmt>(std::move(*node));
}

std::shared_ptr <Stmt> Parser::parseIfStmt() {
	advance();
	SourceLocation loc = peek().loc;
	auto cond = parseExpr();
	expect(TokenKind::OpenBrace);
	auto thenBody = parseBlock();

	std::vector <std::shared_ptr<Stmt>> elseBody;
	if (isKeyword("else")) {
		advance();
		if (isKeyword("if"))
			elseBody.push_back(parseIfStmt());
		else {
			expect(TokenKind::OpenBrace);
			elseBody = parseBlock();
		}
	}

	auto node = std::make_shared<IfStmtNode>();
	node->cond = std::move(cond);
	node->thenBody = std::move(thenBody);
	node->elseBody = std::move(elseBody);
	node->loc = loc;
	return std::make_shared<Stmt>(std::move(*node));
}

std::shared_ptr <Stmt> Parser::parseWhileStmt() {
	advance();
	SourceLocation loc = peek().loc;
	auto cond = parseExpr();
	expect(TokenKind::OpenBrace);
	auto body = parseBlock();

	auto node = std::make_shared<WhileStmtNode>();
	node->cond = std::move(cond);
	node->body = std::move(body);
	node->loc = loc;
	return std::make_shared<Stmt>(std::move(*node));
}

std::shared_ptr <Stmt> Parser::parseReturnStmt() {
	advance();
	SourceLocation loc = peek().loc;
	std::shared_ptr <Expr> value;
	if (peek().kind != TokenKind::Newline && peek().kind != TokenKind::CloseBrace)
		value = parseExpr();
	auto node = std::make_shared<ReturnStmtNode>();
	node->value = std::move(value);
	node->loc = loc;
	return std::make_shared<Stmt>(std::move(*node));
}

std::shared_ptr <Stmt> Parser::parsePrintStmt() {
	advance();
	SourceLocation loc = peek().loc;
	expect(TokenKind::OpenParen);
	auto value = parseExpr();
	expect(TokenKind::CloseParen);
	auto node = std::make_shared<PrintStmtNode>();
	node->value = std::move(value);
	node->loc = loc;
	return std::make_shared<Stmt>(std::move(*node));
}

std::shared_ptr <Stmt> Parser::parseAssignOrExpr() {
	std::string name = advance().lexeme;
	SourceLocation loc = peek().loc;

	// Member access chain: obj.field or obj.field.field2
	std::vector <std::string> fieldChain;
	while (match(TokenKind::Dot)) {
		if (peek().kind != TokenKind::Ident)
			throw ParseError("expected field name after '.'", peek().loc);
		fieldChain.push_back(advance().lexeme);
	}

	if (peek().kind == TokenKind::Equals || peek().kind == TokenKind::PlusEq ||
	    peek().kind == TokenKind::MinusEq || peek().kind == TokenKind::StarEq ||
	    peek().kind == TokenKind::SlashEq) {

		AssignStmtNode::Op op;
		switch (advance().kind) {
			case TokenKind::Equals:
				op = AssignStmtNode::Set;
				break;
			case TokenKind::PlusEq:
				op = AssignStmtNode::AddEq;
				break;
			case TokenKind::MinusEq:
				op = AssignStmtNode::SubEq;
				break;
			case TokenKind::StarEq:
				op = AssignStmtNode::MulEq;
				break;
			case TokenKind::SlashEq:
				op = AssignStmtNode::DivEq;
				break;
			default:
				throw ParseError("internal error", peek().loc);
		}
		auto value = parseExpr();
		if (!fieldChain.empty()) {
			auto obj = std::make_shared<VarExprNode>();
			obj->name = name;
			obj->loc = loc;
			auto base = std::make_shared<Expr>(std::move(*obj));
			for (size_t i = 0; i + 1 < fieldChain.size(); i++) {
				auto mem = std::make_shared<MemberExprNode>();
				mem->object = base;
				mem->field = fieldChain[i];
				mem->loc = loc;
				base = std::make_shared<Expr>(std::move(*mem));
			}
			auto node = std::make_shared<MemberAssignStmtNode>();
			node->op = (MemberAssignStmtNode::Op) op;
			node->object = base;
			node->field = fieldChain.back();
			node->value = std::move(value);
			node->loc = loc;
			return std::make_shared<Stmt>(std::move(*node));
		}
		if (fieldChain.empty() && m_declaredVars.find(name) == m_declaredVars.end())
			throw ParseError("undeclared variable -- use val or var to declare", loc);
		auto node = std::make_shared<AssignStmtNode>();
		node->name = name;
		node->op = op;
		node->value = std::move(value);
		node->loc = loc;
		return std::make_shared<Stmt>(std::move(*node));
	}
	if (!fieldChain.empty() && match(TokenKind::OpenParen)) {
		auto obj = std::make_shared<VarExprNode>();
		obj->name = name;
		obj->loc = loc;
		auto base = std::make_shared<Expr>(std::move(*obj));
		for (size_t i = 0; i + 1 < fieldChain.size(); i++) {
			auto mem = std::make_shared<MemberExprNode>();
			mem->object = base;
			mem->field = fieldChain[i];
			mem->loc = loc;
			base = std::make_shared<Expr>(std::move(*mem));
		}
		std::string method = fieldChain.back();
		std::vector <std::shared_ptr<Expr>> args;
		if (peek().kind != TokenKind::CloseParen) {
			do { args.push_back(parseExpr()); } while (match(TokenKind::Comma));
		}
		expect(TokenKind::CloseParen);
		auto call = std::make_shared<MethodCallNode>();
		call->object = base;
		call->method = method;
		call->args = std::move(args);
		call->loc = loc;
		auto node = std::make_shared<ExprStmtNode>();
		node->expr = std::make_shared<Expr>(std::move(*call));
		node->loc = loc;
		return std::make_shared<Stmt>(std::move(*node));
	}
	if (match(TokenKind::OpenParen)) {
		auto call = parseCall(name, loc);
		auto node = std::make_shared<ExprStmtNode>();
		node->expr = std::move(call);
		node->loc = loc;
		return std::make_shared<Stmt>(std::move(*node));
	}
	throw ParseError("expected = or (+=, etc) or (", peek().loc);
}

std::vector <std::shared_ptr<Stmt>> Parser::parseBlock() {
	skipNewlines();
	std::vector <std::shared_ptr<Stmt>> stmts;
	while (peek().kind != TokenKind::CloseBrace && peek().kind != TokenKind::Eof) {
		stmts.push_back(parseStatement());
		skipNewlines();
	}
	expect(TokenKind::CloseBrace);
	return stmts;
}

int Parser::precOf(TokenKind kind) {
	switch (kind) {
		case TokenKind::EqualsEquals:
		case TokenKind::NotEquals:
		case TokenKind::Less:
		case TokenKind::Greater:
		case TokenKind::LessEq:
		case TokenKind::GreaterEq:
			return 1;
		case TokenKind::Plus:
		case TokenKind::Minus:
			return 2;
		case TokenKind::StarOp:
		case TokenKind::Slash:
			return 3;
		default:
			return -1;
	}
}

std::shared_ptr <Expr> Parser::parseExpr() { return parseExprPrec(0); }

std::shared_ptr <Expr> Parser::parseExprPrec(int minPrec) {
	auto left = parsePrimary();
	while (true) {
		Token opToken = peek();
		int p = precOf(opToken.kind);
		if (p < minPrec) break;
		advance();
		auto right = parseExprPrec(p + 1);
		BinExprNode::Op op;
		switch (opToken.kind) {
			case TokenKind::Plus:
				op = BinExprNode::Add;
				break;
			case TokenKind::Minus:
				op = BinExprNode::Sub;
				break;
			case TokenKind::StarOp:
				op = BinExprNode::Mul;
				break;
			case TokenKind::Slash:
				op = BinExprNode::Div;
				break;
			case TokenKind::EqualsEquals:
				op = BinExprNode::Eq;
				break;
			case TokenKind::NotEquals:
				op = BinExprNode::Neq;
				break;
			case TokenKind::Less:
				op = BinExprNode::Lt;
				break;
			case TokenKind::Greater:
				op = BinExprNode::Gt;
				break;
			case TokenKind::LessEq:
				op = BinExprNode::Le;
				break;
			case TokenKind::GreaterEq:
				op = BinExprNode::Ge;
				break;
			default:
				throw ParseError("unexpected operator", opToken.loc);
		}
		auto node = std::make_shared<BinExprNode>();
		node->op = op;
		node->left = std::move(left);
		node->right = std::move(right);
		node->loc = opToken.loc;
		left = std::make_shared<Expr>(std::move(*node));
	}
	return left;
}

std::shared_ptr <Expr> Parser::parsePrimary() {
	Token t = peek();
	if (t.kind == TokenKind::Minus) {
		advance();
		auto operand = parsePrimary();
		auto node = std::make_shared<UnaryExprNode>();
		node->op = UnaryExprNode::Neg;
		node->operand = std::move(operand);
		node->loc = t.loc;
		return std::make_shared<Expr>(std::move(*node));
	}
	if (t.kind == TokenKind::Integer) {
		advance();
		auto node = std::make_shared<IntLitNode>();
		node->value = std::stoll(t.lexeme);
		node->loc = t.loc;
		return std::make_shared<Expr>(std::move(*node));
	}
	if (t.kind == TokenKind::FloatLit) {
		advance();
		auto node = std::make_shared<FloatLitNode>();
		node->value = std::stod(t.lexeme);
		node->loc = t.loc;
		return std::make_shared<Expr>(std::move(*node));
	}
	if (t.kind == TokenKind::DoubleLit) {
		advance();
		auto node = std::make_shared<DoubleLitNode>();
		node->value = std::stod(t.lexeme);
		node->loc = t.loc;
		return std::make_shared<Expr>(std::move(*node));
	}
	if (t.kind == TokenKind::String) {
		advance();
		auto node = std::make_shared<StrLitNode>();
		node->value = t.lexeme;
		node->loc = t.loc;
		return std::make_shared<Expr>(std::move(*node));
	}
	if (t.kind == TokenKind::Ident) {
		std::string name = advance().lexeme;
		SourceLocation loc = t.loc;
		if (match(TokenKind::OpenParen)) return parseCall(name, loc);
		auto node = std::make_shared<VarExprNode>();
		node->name = name;
		node->loc = loc;
		auto result = std::make_shared<Expr>(std::move(*node));
		while (match(TokenKind::Dot)) {
			if (peek().kind != TokenKind::Ident) throw ParseError("expected field name after '.'", peek().loc);
			std::string field = advance().lexeme;
			if (match(TokenKind::OpenParen)) {
				std::vector <std::shared_ptr<Expr>> args;
				if (peek().kind != TokenKind::CloseParen) {
					do { args.push_back(parseExpr()); } while (match(TokenKind::Comma));
				}
				expect(TokenKind::CloseParen);
				auto call = std::make_shared<MethodCallNode>();
				call->object = result;
				call->method = field;
				call->args = std::move(args);
				call->loc = loc;
				result = std::make_shared<Expr>(std::move(*call));
			} else {
				auto mem = std::make_shared<MemberExprNode>();
				mem->object = result;
				mem->field = field;
				mem->loc = loc;
				result = std::make_shared<Expr>(std::move(*mem));
			}
		}
		return result;
		return result;
	}
	if (match(TokenKind::OpenParen)) {
		auto expr = parseExpr();
		expect(TokenKind::CloseParen);
		return expr;
	}
	throw ParseError("expected expression", t.loc);
}

std::shared_ptr <Expr> Parser::parseCall(const std::string &name, SourceLocation loc) {
	std::vector <std::shared_ptr<Expr>> args;
	if (peek().kind != TokenKind::CloseParen) {
		do { args.push_back(parseExpr()); } while (match(TokenKind::Comma));
	}
	expect(TokenKind::CloseParen);
	auto node = std::make_shared<CallExprNode>();
	node->name = name;
	node->args = std::move(args);
	node->loc = loc;
	return std::make_shared<Expr>(std::move(*node));
}
