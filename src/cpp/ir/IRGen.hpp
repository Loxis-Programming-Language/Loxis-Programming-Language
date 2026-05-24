#pragma once

#include <string>
#include <unordered_map>
#include "../frontend/AST.hpp"
#include "OpCode.hpp"

class IRGen {
public:
	IRProgram generate(const AST &ast);

private:
	IRProgram m_program;

	// Symbol tables
	std::unordered_map <std::string, uint8_t> m_varRegs;
	std::unordered_map <std::string, std::string> m_varTypes;   // name → type
	std::unordered_map <std::string, std::string> m_funRetTypes; // fun name → return type
	std::string m_currentFun;  // currently compiling function name
	uint8_t m_nextVarReg = 16;

	uint8_t allocVarReg(const std::string &name);

	// Expression type tracking (register → type string)
	std::unordered_map <uint8_t, std::string> m_exprType;

	void setExprType(uint8_t reg, const std::string &t) { m_exprType[reg] = t; }

	std::string getExprType(uint8_t reg) const {
		auto it = m_exprType.find(reg);
		return it != m_exprType.end() ? it->second : "int";
	}

	// Scratch registers for expression evaluation (allocated from var reg range,
	// so each function gets its own scratch space — safe for recursion)
	uint8_t allocTmp();

	void resetTmps();

	// Label generation
	int m_labelId = 0;
	// Class field info: "ClassName.fieldName" -> {typeId, byteOffset}
	std::unordered_map <std::string, std::pair<uint32_t, uint32_t>> m_classFields;
	// Method registry: "ClassName.methodName" -> desugar function name
	std::unordered_map <std::string, std::string> m_classMethods;

	void genClassDecl(const ClassDeclNode &cls);

	uint8_t genMemberExpr(const MemberExprNode &n);

	void genMemberAssign(const MemberAssignStmtNode &n);

	uint8_t genMethodCall(const MethodCallNode &n);

	std::string newLabel(const std::string &prefix);

	// Current basic block being built
	BasicBlock *m_curBlock = nullptr;

	void startBlock(const std::string &label);

	void emit(OpCode op, std::vector <Value> operands);

	void emitTerm(OpCode op, std::vector <Value> operands, std::vector <std::string> succs);

	// Code generation
	void genFun(const FunDeclNode &fun);

	void genStmt(const Stmt &stmt);

	void genVarDecl(const VarDeclNode &n);

	void truncateI32(uint8_t reg);

	void genAssign(const AssignStmtNode &n);

	void genIf(const IfStmtNode &n);

	void genWhile(const WhileStmtNode &n);

	void genReturn(const ReturnStmtNode &n);

	void genPrint(const PrintStmtNode &n);

	void genExprStmt(const ExprStmtNode &n);

	// Expression compilation → returns register holding result
	uint8_t genExpr(const Expr &expr);

	uint8_t genIntLit(const IntLitNode &n);

	uint8_t genStrLit(const StrLitNode &n);

	uint8_t genFloatLit(const FloatLitNode &n);

	uint8_t genDoubleLit(const DoubleLitNode &n);

	uint8_t genVarExpr(const VarExprNode &n);

	uint8_t genBinExpr(const BinExprNode &n);

	uint8_t genUnaryExpr(const UnaryExprNode &n);

	uint8_t genCallExpr(const CallExprNode &n);

	// Labels for branch targets in current function
	uint8_t x0() const { return 0; } // zero register / return value
};
