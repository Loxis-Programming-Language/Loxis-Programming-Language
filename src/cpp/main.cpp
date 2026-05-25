#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include "Error.hpp"
#include "frontend/Lexer.hpp"
#include "frontend/Parser.hpp"
#include "ir/IRGen.hpp"
#include "backend/Compiler.hpp"
#include "vm/VM.hpp"

#define time std::chrono::high_resolution_clock::now

auto diff(auto t1, auto t2) {
	return std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
}

struct Argument {
	std::string source;
	int maxExecuteCount = 2147483647;
	bool dumpTokens = false;
	bool dumpAst = false;
	bool dumpIr = false;
	bool dumpBytecode = false;
	bool runTrace = false;
	bool showTime = false;
};

Argument parse(int argc, char *argv[]) {
	Argument em;
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " <source_file> [max_instructions] [flags...]" << std::endl;
		std::cerr << "Flags: --token --ast --ir --bytecode --runtrace --time" << std::endl;
		throw std::runtime_error("missing source file.");
	}
	em.source = argv[1];

	for (int i = 2; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--token") em.dumpTokens = true;
		else if (arg == "--ast") em.dumpAst = true;
		else if (arg == "--ir") em.dumpIr = true;
		else if (arg == "--bytecode") em.dumpBytecode = true;
		else if (arg == "--runtrace") em.runTrace = true;
		else if (arg == "--time") em.showTime = true;
		else {
			// Try as max_instructions (backward compat)
			try {
				em.maxExecuteCount = std::stoi(arg);
			} catch (...) {
				std::cerr << "Unknown flag: " << arg << std::endl;
				throw std::runtime_error("unknown argument: " + arg);
			}
		}
	}
	return em;
}

// ---- Token dump ----

static const char *tokenKindName(TokenKind k) {
	switch (k) {
		case TokenKind::Ident:        return "Ident";
		case TokenKind::Integer:      return "Integer";
		case TokenKind::FloatLit:     return "FloatLit";
		case TokenKind::DoubleLit:    return "DoubleLit";
		case TokenKind::String:       return "String";
		case TokenKind::OpenParen:    return "OpenParen";
		case TokenKind::CloseParen:   return "CloseParen";
		case TokenKind::OpenBrace:    return "OpenBrace";
		case TokenKind::CloseBrace:   return "CloseBrace";
		case TokenKind::Comma:        return "Comma";
		case TokenKind::Plus:         return "Plus";
		case TokenKind::Minus:        return "Minus";
		case TokenKind::StarOp:       return "StarOp";
		case TokenKind::Slash:        return "Slash";
		case TokenKind::Equals:       return "Equals";
		case TokenKind::Colon:        return "Colon";
		case TokenKind::Dot:          return "Dot";
		case TokenKind::OpenBracket:  return "OpenBracket";
		case TokenKind::CloseBracket: return "CloseBracket";
		case TokenKind::EqualsEquals: return "EqualsEquals";
		case TokenKind::NotEquals:    return "NotEquals";
		case TokenKind::Less:         return "Less";
		case TokenKind::LessEq:       return "LessEq";
		case TokenKind::Greater:      return "Greater";
		case TokenKind::GreaterEq:    return "GreaterEq";
		case TokenKind::Arrow:        return "Arrow";
		case TokenKind::PlusEq:       return "PlusEq";
		case TokenKind::MinusEq:      return "MinusEq";
		case TokenKind::StarEq:       return "StarEq";
		case TokenKind::SlashEq:      return "SlashEq";
		case TokenKind::Newline:      return "Newline";
		case TokenKind::Eof:          return "Eof";
	}
	return "?";
}

static void dumpTokens(const std::vector<Token> &tokens) {
	std::cerr << "\n=== Tokens (" << tokens.size() << " tokens) ===" << std::endl;
	for (size_t i = 0; i < tokens.size(); i++) {
		const auto &t = tokens[i];
		std::cerr << "  " << std::setw(4) << i << ": "
		          << std::setw(13) << std::left << tokenKindName(t.kind)
		          << " \"" << t.lexeme << "\""
		          << "  @ " << t.loc.format() << std::endl;
	}
}

// ---- AST dump ----

static std::string indent(int d) { return std::string(d * 2, ' '); }

static const char *binOpName(BinExprNode::Op op) {
	switch (op) {
		case BinExprNode::Add: return "+";
		case BinExprNode::Sub: return "-";
		case BinExprNode::Mul: return "*";
		case BinExprNode::Div: return "/";
		case BinExprNode::Eq:  return "==";
		case BinExprNode::Neq: return "!=";
		case BinExprNode::Lt:  return "<";
		case BinExprNode::Le:  return "<=";
		case BinExprNode::Gt:  return ">";
		case BinExprNode::Ge:  return ">=";
	}
	return "?";
}

static void dumpExpr(const Expr &expr, int depth);

static void dumpStmt(const Stmt &stmt, int depth);

static void dumpExprNode(const IntLitNode &n, int d) {
	std::cerr << indent(d) << "IntLit(" << n.value << ")  " << n.loc.format() << std::endl;
}

static void dumpExprNode(const StrLitNode &n, int d) {
	std::cerr << indent(d) << "StrLit(\"" << n.value << "\")  " << n.loc.format() << std::endl;
}

static void dumpExprNode(const FloatLitNode &n, int d) {
	std::cerr << indent(d) << "FloatLit(" << n.value << "f)  " << n.loc.format() << std::endl;
}

static void dumpExprNode(const DoubleLitNode &n, int d) {
	std::cerr << indent(d) << "DoubleLit(" << n.value << ")  " << n.loc.format() << std::endl;
}

static void dumpExprNode(const VarExprNode &n, int d) {
	std::cerr << indent(d) << "Var(" << n.name << ")  " << n.loc.format() << std::endl;
}

static void dumpExprNode(const BinExprNode &n, int d) {
	std::cerr << indent(d) << "BinOp(" << binOpName(n.op) << ")  " << n.loc.format() << std::endl;
	std::cerr << indent(d) << "  left:" << std::endl;
	dumpExpr(*n.left, d + 2);
	std::cerr << indent(d) << "  right:" << std::endl;
	dumpExpr(*n.right, d + 2);
}

static void dumpExprNode(const UnaryExprNode &n, int d) {
	std::cerr << indent(d) << "UnaryOp(-)  " << n.loc.format() << std::endl;
	std::cerr << indent(d) << "  operand:" << std::endl;
	dumpExpr(*n.operand, d + 2);
}

static void dumpExprNode(const CallExprNode &n, int d) {
	std::cerr << indent(d) << "Call(" << n.name << ")  " << n.loc.format() << std::endl;
	for (size_t i = 0; i < n.args.size(); i++) {
		std::cerr << indent(d) << "  arg[" << i << "]:" << std::endl;
		dumpExpr(*n.args[i], d + 2);
	}
}

static void dumpExprNode(const ArrayLitNode &n, int d) {
	std::cerr << indent(d) << "ArrayLit[" << n.elements.size() << "]  " << n.loc.format() << std::endl;
	for (size_t i = 0; i < n.elements.size(); i++) {
		std::cerr << indent(d) << "  elem[" << i << "]:" << std::endl;
		dumpExpr(*n.elements[i], d + 2);
	}
}

static void dumpExprNode(const IndexExprNode &n, int d) {
	std::cerr << indent(d) << "IndexExpr  " << n.loc.format() << std::endl;
	std::cerr << indent(d) << "  base:" << std::endl;
	dumpExpr(*n.base, d + 2);
	std::cerr << indent(d) << "  index:" << std::endl;
	dumpExpr(*n.index, d + 2);
}

static void dumpExprNode(const MemberExprNode &n, int d) {
	std::cerr << indent(d) << "MemberExpr(." << n.field << ")  " << n.loc.format() << std::endl;
	std::cerr << indent(d) << "  object:" << std::endl;
	dumpExpr(*n.object, d + 2);
}

static void dumpExprNode(const MethodCallNode &n, int d) {
	std::cerr << indent(d) << "MethodCall(." << n.method << ")  " << n.loc.format() << std::endl;
	std::cerr << indent(d) << "  object:" << std::endl;
	dumpExpr(*n.object, d + 2);
	for (size_t i = 0; i < n.args.size(); i++) {
		std::cerr << indent(d) << "  arg[" << i << "]:" << std::endl;
		dumpExpr(*n.args[i], d + 2);
	}
}

static void dumpExpr(const Expr &expr, int depth) {
	std::visit([&](auto &&n) { dumpExprNode(n, depth); }, expr);
}

static void dumpStmtNode(const VarDeclNode &n, int d) {
	std::cerr << indent(d) << "VarDecl " << (n.mut ? "mut " : "") << n.name;
	if (!n.typeAnnot.empty()) std::cerr << ": " << n.typeAnnot;
	std::cerr << "  " << n.loc.format() << std::endl;
	if (n.init) {
		std::cerr << indent(d) << "  init:" << std::endl;
		dumpExpr(*n.init, d + 2);
	}
}

static const char *assignOpName(AssignStmtNode::Op op) {
	switch (op) {
		case AssignStmtNode::Set:   return "=";
		case AssignStmtNode::AddEq: return "+=";
		case AssignStmtNode::SubEq: return "-=";
		case AssignStmtNode::MulEq: return "*=";
		case AssignStmtNode::DivEq: return "/=";
	}
	return "?";
}

static void dumpStmtNode(const AssignStmtNode &n, int d) {
	std::cerr << indent(d) << "Assign " << n.name << " " << assignOpName(n.op) << "  " << n.loc.format() << std::endl;
	std::cerr << indent(d) << "  value:" << std::endl;
	dumpExpr(*n.value, d + 2);
}

static void dumpStmtNode(const IfStmtNode &n, int d) {
	std::cerr << indent(d) << "If  " << n.loc.format() << std::endl;
	std::cerr << indent(d) << "  cond:" << std::endl;
	dumpExpr(*n.cond, d + 2);
	std::cerr << indent(d) << "  then (" << n.thenBody.size() << " stmts):" << std::endl;
	for (auto &s : n.thenBody) dumpStmt(*s, d + 2);
	if (!n.elseBody.empty()) {
		std::cerr << indent(d) << "  else (" << n.elseBody.size() << " stmts):" << std::endl;
		for (auto &s : n.elseBody) dumpStmt(*s, d + 2);
	}
}

static void dumpStmtNode(const WhileStmtNode &n, int d) {
	std::cerr << indent(d) << "While  " << n.loc.format() << std::endl;
	std::cerr << indent(d) << "  cond:" << std::endl;
	dumpExpr(*n.cond, d + 2);
	std::cerr << indent(d) << "  body (" << n.body.size() << " stmts):" << std::endl;
	for (auto &s : n.body) dumpStmt(*s, d + 2);
}

static void dumpStmtNode(const ReturnStmtNode &n, int d) {
	std::cerr << indent(d) << "Return  " << n.loc.format() << std::endl;
	if (n.value) {
		std::cerr << indent(d) << "  value:" << std::endl;
		dumpExpr(*n.value, d + 2);
	}
}

static void dumpStmtNode(const PrintStmtNode &n, int d) {
	std::cerr << indent(d) << "Print  " << n.loc.format() << std::endl;
	std::cerr << indent(d) << "  value:" << std::endl;
	dumpExpr(*n.value, d + 2);
}

static void dumpStmtNode(const ExprStmtNode &n, int d) {
	std::cerr << indent(d) << "ExprStmt  " << n.loc.format() << std::endl;
	dumpExpr(*n.expr, d + 1);
}

static void dumpStmtNode(const IndexAssignStmtNode &n, int d) {
	std::cerr << indent(d) << "IndexAssign  " << n.loc.format() << std::endl;
	std::cerr << indent(d) << "  base:" << std::endl;
	dumpExpr(*n.base, d + 2);
	std::cerr << indent(d) << "  index:" << std::endl;
	dumpExpr(*n.index, d + 2);
	std::cerr << indent(d) << "  value:" << std::endl;
	dumpExpr(*n.value, d + 2);
}

static const char *memberAssignOpName(MemberAssignStmtNode::Op op) {
	switch (op) {
		case MemberAssignStmtNode::Set:   return "=";
		case MemberAssignStmtNode::AddEq: return "+=";
		case MemberAssignStmtNode::SubEq: return "-=";
		case MemberAssignStmtNode::MulEq: return "*=";
		case MemberAssignStmtNode::DivEq: return "/=";
	}
	return "?";
}

static void dumpStmtNode(const MemberAssignStmtNode &n, int d) {
	std::cerr << indent(d) << "MemberAssign ." << n.field << " " << memberAssignOpName(n.op) << "  " << n.loc.format() << std::endl;
	std::cerr << indent(d) << "  object:" << std::endl;
	dumpExpr(*n.object, d + 2);
	std::cerr << indent(d) << "  value:" << std::endl;
	dumpExpr(*n.value, d + 2);
}

static void dumpStmt(const Stmt &stmt, int depth) {
	std::visit([&](auto &&n) { dumpStmtNode(n, depth); }, stmt);
}

static void dumpAst(const AST &ast) {
	std::cerr << "\n=== AST (" << ast.nodes.size() << " top-level nodes) ===" << std::endl;
	for (size_t i = 0; i < ast.nodes.size(); i++) {
		std::visit([&](auto &&node) {
			using T = std::decay_t<decltype(node)>;
			if constexpr (std::is_same_v<T, FunDeclNode>) {
				std::cerr << "\nfun " << node.name;
				if (!node.returnType.empty()) std::cerr << ": " << node.returnType;
				std::cerr << "  " << node.loc.format() << std::endl;
				std::cerr << "  params (" << node.params.size() << "):" << std::endl;
				for (auto &p : node.params) {
					std::cerr << "    " << p.name;
					if (!p.type.empty()) std::cerr << ": " << p.type;
					std::cerr << std::endl;
				}
				std::cerr << "  body (" << node.body.size() << " stmts):" << std::endl;
				for (auto &s : node.body) dumpStmt(*s, 3);
			} else if constexpr (std::is_same_v<T, ClassDeclNode>) {
				std::cerr << "\nclass " << node.name << "  " << node.loc.format() << std::endl;
				std::cerr << "  fields (" << node.fields.size() << "):" << std::endl;
				for (auto &f : node.fields) {
					std::cerr << "    " << f.name << ": " << f.type;
					if (f.init) {
						std::cerr << " = ...";
					}
					std::cerr << std::endl;
				}
				std::cerr << "  methods (" << node.methods.size() << "):" << std::endl;
				for (auto &m : node.methods) {
					std::cerr << "    " << m.name << "(" << m.params.size() << " params)";
					if (!m.returnType.empty()) std::cerr << ": " << m.returnType;
					std::cerr << std::endl;
				}
			}
		}, ast.nodes[i]);
	}
}

// ---- IR dump ----

static const char *opcodeName(OpCode op) {
	switch (op) {
		case OpCode::Nop:   return "nop";
		case OpCode::Push:  return "push";
		case OpCode::Pop:   return "pop";
		case OpCode::Mov:   return "mov";
		case OpCode::PushR: return "pushr";
		case OpCode::PopR:  return "popr";
		case OpCode::Alloc: return "alloc";
		case OpCode::HFree: return "hfree";
		case OpCode::HStore:return "hstore";
		case OpCode::HLoad: return "hload";
		case OpCode::Shl:   return "shl";
		case OpCode::Shr:   return "shr";
		case OpCode::Add:   return "add";
		case OpCode::Sub:   return "sub";
		case OpCode::Mul:   return "mul";
		case OpCode::Div:   return "div";
		case OpCode::FAdd:  return "fadd";
		case OpCode::FSub:  return "fsub";
		case OpCode::FMul:  return "fmul";
		case OpCode::FDiv:  return "fdiv";
		case OpCode::ItoF:  return "itof";
		case OpCode::FtoI:  return "ftoi";
		case OpCode::FNeg:  return "fneg";
		case OpCode::Cmp:   return "cmp";
		case OpCode::Jmp:   return "jmp";
		case OpCode::Jnz:   return "jnz";
		case OpCode::Jz:    return "jz";
		case OpCode::Skip:  return "skip";
		case OpCode::Call:  return "call";
		case OpCode::Ret:   return "ret";
		case OpCode::Br:    return "br";
		case OpCode::Jn:    return "jn";
		case OpCode::Print: return "print";
		case OpCode::Halt:  return "halt";
	}
	return "?";
}

static void dumpValue(const Value &val) {
	if (std::holds_alternative<int64_t>(val))
		std::cerr << "#" << std::get<int64_t>(val);
	else if (std::holds_alternative<RegId>(val))
		std::cerr << "r" << (int)std::get<RegId>(val).id;
	else if (std::holds_alternative<LabelRef>(val))
		std::cerr << "@" << std::get<LabelRef>(val).name;
	else if (std::holds_alternative<std::string>(val))
		std::cerr << "\"" << std::get<std::string>(val) << "\"";
	else if (std::holds_alternative<CondRef>(val)) {
		auto cond = std::get<CondRef>(val).cond;
		if (cond == BranchCond::Always) std::cerr << "always";
		else if (cond == BranchCond::Zero) std::cerr << "zero";
		else std::cerr << "nonzero";
	}
}

static void dumpIR(const IRProgram &program) {
	std::cerr << "\n=== IR (" << program.blocks.size() << " blocks) ===" << std::endl;
	for (const auto &block : program.blocks) {
		std::cerr << "\n" << block.label << ":" << std::endl;
		for (const auto &instr : block.instructions) {
			std::cerr << "  " << opcodeName(instr.op);
			for (size_t i = 0; i < instr.operands.size(); i++) {
				if (i > 0) std::cerr << ",";
				std::cerr << " ";
				dumpValue(instr.operands[i]);
			}
			std::cerr << std::endl;
		}
		if (block.terminator) {
			std::cerr << "  [term] " << opcodeName(block.terminator->op);
			for (size_t i = 0; i < block.terminator->operands.size(); i++) {
				if (i > 0) std::cerr << ",";
				std::cerr << " ";
				dumpValue(block.terminator->operands[i]);
			}
			std::cerr << std::endl;
		}
		if (!block.successors.empty()) {
			std::cerr << "  ->";
			for (auto &s : block.successors) std::cerr << " " << s;
			std::cerr << std::endl;
		}
	}
}

// ---- Bytecode dump ----

static const char *byteOpcodeName(OpCode op) { return opcodeName(op); }

static void dumpBytecode(const Chunk &chunk) {
	std::cerr << "\n=== Bytecode (" << chunk.code.size() << " bytes) ===" << std::endl;

	uint32_t ip = 0;
	int instrCount = 0;
	while (ip < chunk.code.size()) {
		uint32_t start = ip;
		uint8_t opByte = chunk.code[ip++];
		auto op = static_cast<OpCode>(opByte);

		int operandCount = 0;
		switch (op) {
			case OpCode::Nop: case OpCode::Ret: case OpCode::Halt:
				operandCount = 0; break;
			case OpCode::Jnz: case OpCode::Jz: case OpCode::Jn:
			case OpCode::Call: case OpCode::Print:
			case OpCode::PushR: case OpCode::PopR:
				operandCount = 1; break;
			case OpCode::Push: case OpCode::Mov: case OpCode::Cmp:
			case OpCode::Jmp:
				operandCount = 2; break;
			case OpCode::HFree:
				operandCount = 1; break;
			case OpCode::Add: case OpCode::Sub: case OpCode::Mul: case OpCode::Div:
			case OpCode::HStore: case OpCode::HLoad: case OpCode::Alloc:
			case OpCode::Shl: case OpCode::Shr:
			case OpCode::FAdd: case OpCode::FSub: case OpCode::FMul: case OpCode::FDiv:
				operandCount = 3; break;
			case OpCode::ItoF: case OpCode::FtoI: case OpCode::FNeg:
				operandCount = 2; break;
			case OpCode::Skip:
				if (ip < chunk.code.size() &&
				    chunk.code[ip] == static_cast<uint8_t>(OperandTag::Addr))
					operandCount = 1;
				else
					operandCount = 0;
				break;
			default: break;
		}

		// Collect operand string representations
		std::ostringstream opsStr;
		for (int i = 0; i < operandCount; i++) {
			if (i > 0) opsStr << ", ";
			if (ip >= chunk.code.size()) break;
			uint8_t tag = chunk.code[ip++];
			switch (static_cast<OperandTag>(tag)) {
				case OperandTag::Reg: {
					if (ip >= chunk.code.size()) break;
					uint8_t reg = chunk.code[ip++];
					opsStr << "r" << (int)reg;
					break;
				}
				case OperandTag::Imm64: {
					if (ip + 7 >= chunk.code.size()) break;
					uint64_t v = 0;
					for (int j = 0; j < 8; j++)
						v |= static_cast<uint64_t>(chunk.code[ip++]) << (j * 8);
					opsStr << "#" << static_cast<int64_t>(v);
					break;
				}
				case OperandTag::StrIdx: {
					if (ip + 3 >= chunk.code.size()) break;
					uint32_t idx = 0;
					for (int j = 0; j < 4; j++)
						idx |= static_cast<uint32_t>(chunk.code[ip++]) << (j * 8);
					opsStr << "str[" << idx << "]";
					if (idx < chunk.strings.size())
						opsStr << "(\"" << chunk.strings[idx] << "\")";
					break;
				}
				case OperandTag::Addr: {
					if (ip + 3 >= chunk.code.size()) break;
					uint32_t addr = 0;
					for (int j = 0; j < 4; j++)
						addr |= static_cast<uint32_t>(chunk.code[ip++]) << (j * 8);
					opsStr << "0x" << std::hex << std::setw(8) << std::setfill('0') << addr << std::dec;
					break;
				}
				default: break;
			}
		}

		// Print hex bytes
		std::cerr << "  " << std::hex << std::setw(6) << std::setfill('0') << start << std::dec << ": ";
		std::ostringstream hexStr;
		for (uint32_t i = start; i < ip; i++)
			hexStr << " " << std::hex << std::setw(2) << std::setfill('0') << (int)chunk.code[i];
		std::cerr << std::setfill(' ') << std::setw(48) << std::left << hexStr.str();
		std::cerr << " " << byteOpcodeName(op) << "  " << opsStr.str() << std::endl;

		instrCount++;
	}

	// String pool
	if (!chunk.strings.empty()) {
		std::cerr << "\n--- String Pool (" << chunk.strings.size() << " strings) ---" << std::endl;
		for (size_t i = 0; i < chunk.strings.size(); i++)
			std::cerr << "  [" << i << "] \"" << chunk.strings[i] << "\"" << std::endl;
	}
}

// ---- Main ----

int main(int argc, char *argv[]) {
	auto em = parse(argc, argv);

	try {
		// --- Lex ---
		auto t0 = time();
		Lexer lexer = [&]() {
			std::ifstream f(em.source);
			if (!f) throw std::runtime_error("cannot open file: " + em.source);
			std::ostringstream ss;
			ss << f.rdbuf();
			return Lexer(ss.str(), em.source);
		}();
		auto tokens = lexer.tokenize();
		auto t1 = time();

		if (em.dumpTokens) dumpTokens(tokens);

		// --- Parse ---
		Parser parser(tokens);
		AST ast = parser.parse();
		auto t2 = time();

		if (em.dumpAst) dumpAst(ast);

		// --- IRGen ---
		IRGen irgen;
		IRProgram program = irgen.generate(ast);
		auto t3 = time();

		if (em.dumpIr) dumpIR(program);

		// --- Compile ---
		Compiler compiler;
		Chunk chunk = compiler.compile(program);
		auto t4 = time();

		if (em.dumpBytecode) dumpBytecode(chunk);

		// --- Run ---
		auto t5 = time();
		VM vm(chunk, em.runTrace);
		vm.run(em.maxExecuteCount);
		auto t6 = time();

		// --- Timing ---
		if (em.showTime) {
			std::cerr << "\n=== Timing ===" << std::endl;
			std::cerr << "Lexer:     " << diff(t0, t1) << " us" << std::endl;
			std::cerr << "Parser:    " << diff(t1, t2) << " us" << std::endl;
			std::cerr << "IRGen:     " << diff(t2, t3) << " us" << std::endl;
			std::cerr << "Compiler:  " << diff(t3, t4) << " us" << std::endl;
			std::cerr << "VM:        " << diff(t5, t6) << " us" << std::endl;
		}

		return 0;
	} catch (const RuntimeError &) {
		return 1;
	}
}
