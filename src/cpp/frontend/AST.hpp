#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <memory>
#include "../Error.hpp"

struct Expr;
struct Stmt;

struct IntLitNode {
	int64_t value;
	SourceLocation loc;
};

struct StrLitNode {
	std::string value;
	SourceLocation loc;
};

struct FloatLitNode {
	double value;       // stored as f64, truncated to f32 on assignment
	SourceLocation loc;
};

struct DoubleLitNode {
	double value;
	SourceLocation loc;
};

struct VarExprNode {
	std::string name;
	SourceLocation loc;
};

struct BinExprNode {
	enum Op {
		Add, Sub, Mul, Div, Eq, Neq, Lt, Le, Gt, Ge
	};
	Op op;
	std::shared_ptr <Expr> left;
	std::shared_ptr <Expr> right;
	SourceLocation loc;

	~BinExprNode();
};

struct UnaryExprNode {
	enum Op {
		Neg
	};
	Op op;
	std::shared_ptr <Expr> operand;
	SourceLocation loc;

	~UnaryExprNode();
};

struct CallExprNode {
	std::string name;
	std::vector <std::shared_ptr<Expr>> args;
	SourceLocation loc;

	~CallExprNode();
};

struct ArrayLitNode {
	std::vector <std::shared_ptr<Expr>> elements;
	SourceLocation loc;
};

struct IndexExprNode {
	std::shared_ptr <Expr> base;
	std::shared_ptr <Expr> index;
	SourceLocation loc;
};

struct MemberExprNode {
	std::shared_ptr <Expr> object;
	std::string field;
	SourceLocation loc;
};

struct MethodCallNode {
	std::shared_ptr <Expr> object;
	std::string method;
	std::vector <std::shared_ptr<Expr>> args;
	SourceLocation loc;
};

using _ExprVariant = std::variant<
		IntLitNode,
		StrLitNode,
		VarExprNode,
		BinExprNode,
		UnaryExprNode,
		CallExprNode,
		ArrayLitNode,
		IndexExprNode,
		FloatLitNode,
		DoubleLitNode,
		MemberExprNode,
		MethodCallNode
>;

struct Expr : _ExprVariant {
	using _ExprVariant::variant;

	Expr() = default;

	// Explicit constructors for each alternative (avoids SFINAE issues)
	Expr(IntLitNode v) : _ExprVariant(std::move(v)) {}

	Expr(StrLitNode v) : _ExprVariant(std::move(v)) {}

	Expr(VarExprNode v) : _ExprVariant(std::move(v)) {}

	Expr(BinExprNode v) : _ExprVariant(std::move(v)) {}

	Expr(UnaryExprNode v) : _ExprVariant(std::move(v)) {}

	Expr(CallExprNode v) : _ExprVariant(std::move(v)) {}

	Expr(ArrayLitNode v) : _ExprVariant(std::move(v)) {}

	Expr(IndexExprNode v) : _ExprVariant(std::move(v)) {}

	Expr(FloatLitNode v) : _ExprVariant(std::move(v)) {}

	Expr(DoubleLitNode v) : _ExprVariant(std::move(v)) {}

	Expr(MemberExprNode v) : _ExprVariant(std::move(v)) {}

	Expr(MethodCallNode v) : _ExprVariant(std::move(v)) {}
};

struct ParamDecl {
	std::string name;
	std::string type;  // "Int", "String", or "" if omitted
};

struct VarDeclNode {
	bool mut;
	std::string name;
	std::string typeAnnot;  // optional: "Int", "String", or ""
	std::shared_ptr <Expr> init;
	SourceLocation loc;
};

struct AssignStmtNode {
	std::string name;
	enum Op {
		Set, AddEq, SubEq, MulEq, DivEq
	};
	Op op;
	std::shared_ptr <Expr> value;
	SourceLocation loc;
};

struct IfStmtNode {
	std::shared_ptr <Expr> cond;
	std::vector <std::shared_ptr<Stmt>> thenBody;
	std::vector <std::shared_ptr<Stmt>> elseBody;
	SourceLocation loc;

	~IfStmtNode();
};

struct WhileStmtNode {
	std::shared_ptr <Expr> cond;
	std::vector <std::shared_ptr<Stmt>> body;
	SourceLocation loc;

	~WhileStmtNode();
};

struct ReturnStmtNode {
	std::shared_ptr <Expr> value;
	SourceLocation loc;
};

struct PrintStmtNode {
	std::shared_ptr <Expr> value;
	SourceLocation loc;
};

struct IndexAssignStmtNode {
	std::shared_ptr <Expr> base;
	std::shared_ptr <Expr> index;
	std::shared_ptr <Expr> value;
	SourceLocation loc;
};

struct MemberAssignStmtNode {
	enum Op {
		Set, AddEq, SubEq, MulEq, DivEq
	};
	Op op = Set;
	std::shared_ptr <Expr> object;
	std::string field;
	std::shared_ptr <Expr> value;
	SourceLocation loc;
};

struct ExprStmtNode {
	std::shared_ptr <Expr> expr;
	SourceLocation loc;
};

using _StmtVariant = std::variant<
		VarDeclNode,
		AssignStmtNode,
		IfStmtNode,
		WhileStmtNode,
		IndexAssignStmtNode,
		MemberAssignStmtNode,
		ReturnStmtNode,
		PrintStmtNode,
		ExprStmtNode
>;

struct Stmt : _StmtVariant {
	using _StmtVariant::variant;

	Stmt() = default;

	Stmt(VarDeclNode v) : _StmtVariant(std::move(v)) {}

	Stmt(AssignStmtNode v) : _StmtVariant(std::move(v)) {}

	Stmt(IfStmtNode v) : _StmtVariant(std::move(v)) {}

	Stmt(WhileStmtNode v) : _StmtVariant(std::move(v)) {}

	Stmt(ReturnStmtNode v) : _StmtVariant(std::move(v)) {}

	Stmt(PrintStmtNode v) : _StmtVariant(std::move(v)) {}

	Stmt(ExprStmtNode v) : _StmtVariant(std::move(v)) {}

	Stmt(MemberAssignStmtNode v) : _StmtVariant(std::move(v)) {}

	Stmt(IndexAssignStmtNode v) : _StmtVariant(std::move(v)) {}
};

struct FunDeclNode {
	std::string name;
	std::vector <ParamDecl> params;
	std::string returnType;    // "Int", "String", or ""
	std::vector <std::shared_ptr<Stmt>> body;
	SourceLocation loc;

	~FunDeclNode();
};

struct FieldDecl {
	std::string name;
	std::string type;
	std::shared_ptr <Expr> init;  // default value (nullable)
};

struct ClassDeclNode {
	std::string name;
	std::vector <FieldDecl> fields;
	std::vector <FunDeclNode> methods;
	SourceLocation loc;
};

using TopLevelNode = std::variant<FunDeclNode, ClassDeclNode>;

struct AST {
	std::vector <TopLevelNode> nodes;
};

// Destructors — must come after Expr/Stmt are complete
inline BinExprNode::~BinExprNode() = default;

inline UnaryExprNode::~UnaryExprNode() = default;

inline CallExprNode::~CallExprNode() = default;

inline IfStmtNode::~IfStmtNode() = default;

inline WhileStmtNode::~WhileStmtNode() = default;

inline FunDeclNode::~FunDeclNode() = default;
