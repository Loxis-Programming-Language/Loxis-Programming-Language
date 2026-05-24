#include "IRGen.hpp"
#include <stdexcept>
#include "Type.hpp"

IRProgram IRGen::generate(const AST &ast) {
	m_program.blocks.clear();
	m_funRetTypes.clear();
	m_classMethods.clear();

	// Look for a "main" function — will be called from entry point
	bool hasMain = false;
	for (const auto &node: ast.nodes) {
		if (std::holds_alternative<FunDeclNode>(node)) {
			if (std::get<FunDeclNode>(node).name == "main") {
				hasMain = true;
			}
		}
	}

	// Synthetic entry point: call main() then halt
	if (hasMain) {
		startBlock("_entry");
		emit(OpCode::Call, {LabelRef{"main"}});
		emitTerm(OpCode::Halt, {}, {});
	}

	// First pass: register class types
	m_classFields.clear();
	for (const auto &node: ast.nodes) {
		if (std::holds_alternative<ClassDeclNode>(node)) {
			auto &cls = std::get<ClassDeclNode>(node);
			TypeNode classType(TypeKind::Class, cls.name);
			std::vector <uint8_t> buf;
			classType.serialize(buf);
			uint32_t typeId = (uint32_t) m_program.typePool.size();
			for (size_t i = 0; i < m_program.typePool.size(); i++) {
				if (m_program.typePool[i] == buf) {
					typeId = (uint32_t) i;
					break;
				}
			}
			if (typeId == m_program.typePool.size())
				m_program.typePool.push_back(std::move(buf));
			for (size_t i = 0; i < cls.fields.size(); i++) {
				m_classFields[cls.name + "." + cls.fields[i].name] = {
						typeId, static_cast<uint32_t>(i * 9)
				};
			}
		}
	}

	// Generate class constructors and methods
	for (const auto &node: ast.nodes) {
		if (std::holds_alternative<ClassDeclNode>(node)) {
			genClassDecl(std::get<ClassDeclNode>(node));
		}
	}

	for (const auto &node: ast.nodes) {
		if (std::holds_alternative<FunDeclNode>(node)) {
			genFun(std::get<FunDeclNode>(node));
		}
	}

	return m_program;
}

// ==================== Helpers ====================

uint8_t IRGen::allocVarReg(const std::string &name) {
	auto it = m_varRegs.find(name);
	if (it != m_varRegs.end()) return it->second;
	uint8_t reg = m_nextVarReg++;
	m_varRegs[name] = reg;
	return reg;
}

uint8_t IRGen::allocTmp() {
	return m_nextVarReg++;
}

void IRGen::resetTmps() {
	// Scratch regs are per-function (allocated from var reg range).
	// Recursive calls get their own scratch regs starting from 16.
}

std::string IRGen::newLabel(const std::string &prefix) {
	return prefix + std::to_string(m_labelId++);
}

void IRGen::startBlock(const std::string &label) {
	m_program.blocks.emplace_back();
	m_curBlock = &m_program.blocks.back();
	m_curBlock->label = label;
}

void IRGen::emit(OpCode op, std::vector <Value> operands) {
	m_curBlock->instructions.push_back({op, std::move(operands), {}});
}

void IRGen::emitTerm(OpCode op, std::vector <Value> operands,
                     std::vector <std::string> succs) {
	m_curBlock->terminator = IRInstruction{op, std::move(operands), {}};
	m_curBlock->successors = std::move(succs);
}

// ==================== Function ====================

void IRGen::genFun(const FunDeclNode &fun) {
	m_varRegs.clear();
	m_varTypes.clear();
	m_exprType.clear();
	m_nextVarReg = 16;
	m_currentFun = fun.name;

	// Explicit return type or infer later
	if (!fun.returnType.empty())
		m_funRetTypes[fun.name] = fun.returnType;

	for (size_t i = 0; i < fun.params.size() && i < 8; i++) {
		m_varRegs[fun.params[i].name] = static_cast<uint8_t>(128 + i);
		// Infer param type from annotation
		if (!fun.params[i].type.empty())
			m_varTypes[fun.params[i].name] = fun.params[i].type;
	}

	startBlock(fun.name);

	for (const auto &stmt: fun.body) {
		genStmt(*stmt);
	}

	if (!m_curBlock->terminator) {
		emit(OpCode::Mov, {RegId{0}, static_cast<int64_t>(0)});
		emitTerm(OpCode::Ret, {}, {});
	}
}

// ==================== Statements ====================

void IRGen::genStmt(const Stmt &stmt) {
	resetTmps();

	if (std::holds_alternative<VarDeclNode>(stmt))
		genVarDecl(std::get<VarDeclNode>(stmt));
	else if (std::holds_alternative<AssignStmtNode>(stmt))
		genAssign(std::get<AssignStmtNode>(stmt));
	else if (std::holds_alternative<IfStmtNode>(stmt))
		genIf(std::get<IfStmtNode>(stmt));
	else if (std::holds_alternative<WhileStmtNode>(stmt))
		genWhile(std::get<WhileStmtNode>(stmt));
	else if (std::holds_alternative<ReturnStmtNode>(stmt))
		genReturn(std::get<ReturnStmtNode>(stmt));
	else if (std::holds_alternative<PrintStmtNode>(stmt))
		genPrint(std::get<PrintStmtNode>(stmt));
	else if (std::holds_alternative<ExprStmtNode>(stmt))
		genExprStmt(std::get<ExprStmtNode>(stmt));
	else if (std::holds_alternative<MemberAssignStmtNode>(stmt))
		genMemberAssign(std::get<MemberAssignStmtNode>(stmt));
}

// Truncate register value to i32 (sign-extend from 32 bits)
void IRGen::truncateI32(uint8_t reg) {
	uint8_t tmp = allocTmp();
	emit(OpCode::Shl, {RegId{reg}, static_cast<int64_t>(32), RegId{tmp}});
	emit(OpCode::Shr, {RegId{tmp}, static_cast<int64_t>(32), RegId{reg}});
}

void IRGen::genVarDecl(const VarDeclNode &n) {
	uint8_t dstReg = allocVarReg(n.name);
	std::string inferredType;
	if (n.init) {
		uint8_t valReg = genExpr(*n.init);
		emit(OpCode::Mov, {RegId{dstReg}, RegId{valReg}});
		inferredType = getExprType(valReg);
		setExprType(dstReg, inferredType);
	}
	// Type annotation overrides inference
	std::string varType = n.typeAnnot.empty() ? inferredType : n.typeAnnot;
	if (!varType.empty())
		m_varTypes[n.name] = varType;
	// i32 truncation
	if (varType == "int")
		truncateI32(dstReg);
	// f32 truncation: cast to float and back
	if (varType == "float") {
		uint8_t tmp = allocTmp();
		// store as f32: convert to float, back to double
		emit(OpCode::FNeg, {RegId{dstReg}, RegId{tmp}});
		// FNeg gives us TAG_FLOAT. Then truncate by doing nothing extra (f32 fits in f64 mantissa).
		// Proper f32 truncation would need an F32ToF64 op. For now, tag-only.
	}
}

void IRGen::genAssign(const AssignStmtNode &n) {
	uint8_t dstReg = allocVarReg(n.name);
	uint8_t valReg = genExpr(*n.value);

	switch (n.op) {
		case AssignStmtNode::Set:
			emit(OpCode::Mov, {RegId{dstReg}, RegId{valReg}});
			break;
		case AssignStmtNode::AddEq:
			emit(OpCode::Add, {RegId{dstReg}, RegId{dstReg}, RegId{valReg}});
			break;
		case AssignStmtNode::SubEq:
			emit(OpCode::Sub, {RegId{dstReg}, RegId{valReg}, RegId{dstReg}});
			break;
		case AssignStmtNode::MulEq:
			emit(OpCode::Mul, {RegId{dstReg}, RegId{valReg}, RegId{dstReg}});
			break;
		case AssignStmtNode::DivEq:
			emit(OpCode::Div, {RegId{dstReg}, RegId{valReg}, RegId{dstReg}});
			break;
	}
	auto ti = m_varTypes.find(n.name);
	if (ti != m_varTypes.end() && ti->second == "int") {
		truncateI32(dstReg);
	}
}

void IRGen::genIf(const IfStmtNode &n) {
	uint8_t condReg = genExpr(*n.cond);

	std::string thenLabel = newLabel(".then");
	std::string elseLabel = newLabel(".else");
	std::string endLabel = newLabel(".endif");

	emit(OpCode::Cmp, {RegId{condReg}, RegId{x0()}});
	emitTerm(OpCode::Br,
	         {CondRef{BranchCond::NonZero}, LabelRef{thenLabel}, LabelRef{elseLabel}},
	         {thenLabel, elseLabel});

	startBlock(thenLabel);
	for (const auto &s: n.thenBody) genStmt(*s);
	if (!m_curBlock->terminator) {
		emitTerm(OpCode::Br,
		         {CondRef{BranchCond::Always}, LabelRef{endLabel}},
		         {endLabel});
	}

	startBlock(elseLabel);
	for (const auto &s: n.elseBody) genStmt(*s);
	if (!m_curBlock->terminator) {
		emitTerm(OpCode::Br,
		         {CondRef{BranchCond::Always}, LabelRef{endLabel}},
		         {endLabel});
	}

	startBlock(endLabel);
}

void IRGen::genWhile(const WhileStmtNode &n) {
	std::string condLabel = newLabel(".loop_cond");
	std::string bodyLabel = newLabel(".loop_body");
	std::string endLabel = newLabel(".loop_end");

	emitTerm(OpCode::Br,
	         {CondRef{BranchCond::Always}, LabelRef{condLabel}},
	         {condLabel});

	startBlock(condLabel);
	uint8_t condReg = genExpr(*n.cond);
	emit(OpCode::Cmp, {RegId{condReg}, RegId{x0()}});
	emitTerm(OpCode::Br,
	         {CondRef{BranchCond::NonZero}, LabelRef{bodyLabel}, LabelRef{endLabel}},
	         {bodyLabel, endLabel});

	startBlock(bodyLabel);
	for (const auto &s: n.body) genStmt(*s);
	if (!m_curBlock->terminator) {
		emitTerm(OpCode::Br,
		         {CondRef{BranchCond::Always}, LabelRef{condLabel}},
		         {condLabel});
	}

	startBlock(endLabel);
}

void IRGen::genReturn(const ReturnStmtNode &n) {
	if (n.value) {
		uint8_t valReg = genExpr(*n.value);
		emit(OpCode::Mov, {RegId{x0()}, RegId{valReg}});
		// Infer return type if not explicitly annotated
		if (m_funRetTypes.find(m_currentFun) == m_funRetTypes.end()) {
			m_funRetTypes[m_currentFun] = getExprType(valReg);
		}
	}
	emitTerm(OpCode::Ret, {}, {});
}

void IRGen::genPrint(const PrintStmtNode &n) {
	uint8_t valReg = genExpr(*n.value);
	emit(OpCode::Print, {RegId{valReg}});
}

void IRGen::genExprStmt(const ExprStmtNode &n) {
	genExpr(*n.expr);
}

// ==================== Expressions ====================

uint8_t IRGen::genExpr(const Expr &expr) {
	if (std::holds_alternative<IntLitNode>(expr))
		return genIntLit(std::get<IntLitNode>(expr));
	if (std::holds_alternative<StrLitNode>(expr))
		return genStrLit(std::get<StrLitNode>(expr));
	if (std::holds_alternative<VarExprNode>(expr))
		return genVarExpr(std::get<VarExprNode>(expr));
	if (std::holds_alternative<BinExprNode>(expr))
		return genBinExpr(std::get<BinExprNode>(expr));
	if (std::holds_alternative<UnaryExprNode>(expr))
		return genUnaryExpr(std::get<UnaryExprNode>(expr));
	if (std::holds_alternative<CallExprNode>(expr))
		return genCallExpr(std::get<CallExprNode>(expr));
	if (std::holds_alternative<FloatLitNode>(expr))
		return genFloatLit(std::get<FloatLitNode>(expr));
	if (std::holds_alternative<DoubleLitNode>(expr))
		return genDoubleLit(std::get<DoubleLitNode>(expr));
	if (std::holds_alternative<MemberExprNode>(expr))
		return genMemberExpr(std::get<MemberExprNode>(expr));
	if (std::holds_alternative<MethodCallNode>(expr))
		return genMethodCall(std::get<MethodCallNode>(expr));
	return 0;
}

uint8_t IRGen::genIntLit(const IntLitNode &n) {
	uint8_t reg = allocTmp();
	emit(OpCode::Mov, {RegId{reg}, static_cast<int64_t>(n.value)});
	setExprType(reg, "int");
	return reg;
}

uint8_t IRGen::genStrLit(const StrLitNode &n) {
	uint8_t reg = allocTmp();
	emit(OpCode::Push, {RegId{reg}, std::string(n.value)});
	setExprType(reg, "str");
	return reg;
}

uint8_t IRGen::genFloatLit(const FloatLitNode &n) {
	float f = static_cast<float>(n.value);
	double d = static_cast<double>(f);
	uint8_t reg = allocTmp();
	emit(OpCode::Mov, {RegId{reg}, std::bit_cast<int64_t>(d)});
	setExprType(reg, "float");
	return reg;
}

uint8_t IRGen::genDoubleLit(const DoubleLitNode &n) {
	uint8_t reg = allocTmp();
	emit(OpCode::Mov, {RegId{reg}, std::bit_cast<int64_t>(n.value)});
	setExprType(reg, "double");
	return reg;
}

uint8_t IRGen::genVarExpr(const VarExprNode &n) {
	uint8_t srcReg = allocVarReg(n.name);
	uint8_t dstReg = allocTmp();
	emit(OpCode::Push, {RegId{dstReg}, RegId{srcReg}});
	auto ti = m_varTypes.find(n.name);
	setExprType(dstReg, ti != m_varTypes.end() ? ti->second : "int");
	return dstReg;
}

uint8_t IRGen::genBinExpr(const BinExprNode &n) {
	// Arithmetic operators — type-aware: int vs float/double
	if (n.op <= BinExprNode::Div) {
		uint8_t lhs = genExpr(*n.left);
		uint8_t rhs = genExpr(*n.right);
		uint8_t result = allocTmp();

		std::string lt = getExprType(lhs);
		std::string rt = getExprType(rhs);
		bool useFloat = (lt == "float" || lt == "double" || rt == "float" || rt == "double");

		if (useFloat) {
			if (lt == "int") {
				uint8_t c = allocTmp();
				emit(OpCode::ItoF, {RegId{lhs}, RegId{c}});
				lhs = c;
			}
			if (rt == "int") {
				uint8_t c = allocTmp();
				emit(OpCode::ItoF, {RegId{rhs}, RegId{c}});
				rhs = c;
			}
			switch (n.op) {
				case BinExprNode::Add:
					emit(OpCode::FAdd, {RegId{result}, RegId{lhs}, RegId{rhs}});
					break;
				case BinExprNode::Sub:
					emit(OpCode::FSub, {RegId{lhs}, RegId{rhs}, RegId{result}});
					break;
				case BinExprNode::Mul:
					emit(OpCode::FMul, {RegId{lhs}, RegId{rhs}, RegId{result}});
					break;
				case BinExprNode::Div:
					emit(OpCode::FDiv, {RegId{lhs}, RegId{rhs}, RegId{result}});
					break;
				default:
					break;
			}
			setExprType(result, "double");
		} else {
			switch (n.op) {
				case BinExprNode::Add:
					emit(OpCode::Add, {RegId{result}, RegId{lhs}, RegId{rhs}});
					break;
				case BinExprNode::Sub:
					emit(OpCode::Sub, {RegId{lhs}, RegId{rhs}, RegId{result}});
					break;
				case BinExprNode::Mul:
					emit(OpCode::Mul, {RegId{lhs}, RegId{rhs}, RegId{result}});
					break;
				case BinExprNode::Div:
					emit(OpCode::Div, {RegId{lhs}, RegId{rhs}, RegId{result}});
					break;
				default:
					break;
			}
			setExprType(result, "int");
		}
		return result;
	}

	// Comparison operators: produce 0 or 1 in result register
	uint8_t lhs = genExpr(*n.left);
	uint8_t rhs = genExpr(*n.right);
	uint8_t result = allocTmp();
	emit(OpCode::Mov, {RegId{result}, static_cast<int64_t>(1)});
	emit(OpCode::Cmp, {RegId{lhs}, RegId{rhs}});

	std::string doneLabel = newLabel(".cmp_done");
	std::string falseLabel = newLabel(".cmp_false");

	switch (n.op) {
		case BinExprNode::Eq:
			emit(OpCode::Jz, {LabelRef{doneLabel}});
			emit(OpCode::Mov, {RegId{result}, static_cast<int64_t>(0)});
			break;

		case BinExprNode::Neq:
			emit(OpCode::Jnz, {LabelRef{doneLabel}});
			emit(OpCode::Mov, {RegId{result}, static_cast<int64_t>(0)});
			break;

		case BinExprNode::Lt:
			emit(OpCode::Jz, {LabelRef{falseLabel}});
			emit(OpCode::Jn, {LabelRef{doneLabel}});
			emitTerm(OpCode::Br, {CondRef{BranchCond::Always}, LabelRef{falseLabel}}, {falseLabel});
			startBlock(falseLabel);
			emit(OpCode::Mov, {RegId{result}, static_cast<int64_t>(0)});
			break;

		case BinExprNode::Le:
			emit(OpCode::Jz, {LabelRef{doneLabel}});
			emit(OpCode::Jn, {LabelRef{doneLabel}});
			emit(OpCode::Mov, {RegId{result}, static_cast<int64_t>(0)});
			break;

		case BinExprNode::Gt:
			emit(OpCode::Jz, {LabelRef{falseLabel}});
			emit(OpCode::Jn, {LabelRef{falseLabel}});
			emitTerm(OpCode::Br, {CondRef{BranchCond::Always}, LabelRef{doneLabel}}, {doneLabel});
			startBlock(falseLabel);
			emit(OpCode::Mov, {RegId{result}, static_cast<int64_t>(0)});
			break;

		case BinExprNode::Ge:
			emit(OpCode::Jz, {LabelRef{doneLabel}});
			emit(OpCode::Jn, {LabelRef{falseLabel}});
			emitTerm(OpCode::Br, {CondRef{BranchCond::Always}, LabelRef{doneLabel}}, {doneLabel});
			startBlock(falseLabel);
			emit(OpCode::Mov, {RegId{result}, static_cast<int64_t>(0)});
			break;

		default:
			break;
	}

	emitTerm(OpCode::Br,
	         {CondRef{BranchCond::Always}, LabelRef{doneLabel}},
	         {doneLabel});

	startBlock(doneLabel);
	return result;
}

uint8_t IRGen::genUnaryExpr(const UnaryExprNode &n) {
	if (n.op == UnaryExprNode::Neg) {
		uint8_t operand = genExpr(*n.operand);
		uint8_t result = allocTmp();
		uint8_t zero = allocTmp();
		emit(OpCode::Mov, {RegId{zero}, static_cast<int64_t>(0)});
		emit(OpCode::Sub, {RegId{zero}, RegId{operand}, RegId{result}});
		return result;
	}
	return 0;
}

uint8_t IRGen::genCallExpr(const CallExprNode &n) {
	// Heap builtins
	if (n.name == "alloc" && n.args.size() >= 2) {
		// alloc("type", count) — typed heap allocation (no erasure)
		std::string typeStr;
		if (!n.args.empty() && std::holds_alternative<StrLitNode>(*n.args[0]))
			typeStr = std::get<StrLitNode>(*n.args[0]).value;
		TypeNode type = TypeNode::fromString(typeStr);
		std::vector <uint8_t> buf;
		type.serialize(buf);
		uint32_t typeId = (uint32_t) m_program.typePool.size();
		for (size_t i = 0; i < m_program.typePool.size(); i++) {
			if (m_program.typePool[i] == buf) {
				typeId = (uint32_t) i;
				break;
			}
		}
		if (typeId == m_program.typePool.size())
			m_program.typePool.push_back(std::move(buf));

		uint8_t cntReg = genExpr(*n.args[1]);
		uint8_t nineReg = allocTmp();
		emit(OpCode::Mov, {RegId{nineReg}, static_cast<int64_t>(9)});
		uint8_t sizeReg = allocTmp();
		emit(OpCode::Mul, {RegId{cntReg}, RegId{nineReg}, RegId{sizeReg}});

		uint8_t result = allocTmp();
		emit(OpCode::Alloc, {static_cast<int64_t>(typeId), RegId{sizeReg}, RegId{result}});
		return result;
	}
	if (n.name == "hstore") {
		uint8_t baseReg = genExpr(*n.args[0]);
		uint8_t offReg = genExpr(*n.args[1]);
		uint8_t valReg = genExpr(*n.args[2]);
		emit(OpCode::HStore, {RegId{baseReg}, RegId{offReg}, RegId{valReg}});
		uint8_t dummy = allocTmp();
		emit(OpCode::Mov, {RegId{dummy}, static_cast<int64_t>(0)});
		return dummy;
	}
	if (n.name == "hload") {
		uint8_t baseReg = genExpr(*n.args[0]);
		uint8_t offReg = genExpr(*n.args[1]);
		uint8_t result = allocTmp();
		emit(OpCode::HLoad, {RegId{baseReg}, RegId{offReg}, RegId{result}});
		return result;
	}
	if (n.name == "hfree") {
		uint8_t baseReg = genExpr(*n.args[0]);
		emit(OpCode::HFree, {RegId{baseReg}});
		uint8_t dummy = allocTmp();
		emit(OpCode::Mov, {RegId{dummy}, static_cast<int64_t>(0)});
		return dummy;
	}

	// FP builtins
	if (n.name == "itof") {
		uint8_t src = genExpr(*n.args[0]);
		uint8_t res = allocTmp();
		emit(OpCode::ItoF, {RegId{src}, RegId{res}});
		setExprType(res, "double");
		return res;
	}
	if (n.name == "ftoi") {
		uint8_t src = genExpr(*n.args[0]);
		uint8_t res = allocTmp();
		emit(OpCode::FtoI, {RegId{src}, RegId{res}});
		setExprType(res, "int");
		return res;
	}
	if (n.name == "fadd") {
		uint8_t a = genExpr(*n.args[0]);
		uint8_t b = genExpr(*n.args[1]);
		uint8_t r = allocTmp();
		emit(OpCode::FAdd, {RegId{r}, RegId{a}, RegId{b}});
		setExprType(r, "double");
		return r;
	}
	if (n.name == "fsub") {
		uint8_t a = genExpr(*n.args[0]);
		uint8_t b = genExpr(*n.args[1]);
		uint8_t r = allocTmp();
		emit(OpCode::FSub, {RegId{a}, RegId{b}, RegId{r}});
		setExprType(r, "double");
		return r;
	}
	if (n.name == "fmul") {
		uint8_t a = genExpr(*n.args[0]);
		uint8_t b = genExpr(*n.args[1]);
		uint8_t r = allocTmp();
		emit(OpCode::FMul, {RegId{a}, RegId{b}, RegId{r}});
		setExprType(r, "double");
		return r;
	}
	if (n.name == "fdiv") {
		uint8_t a = genExpr(*n.args[0]);
		uint8_t b = genExpr(*n.args[1]);
		uint8_t r = allocTmp();
		emit(OpCode::FDiv, {RegId{a}, RegId{b}, RegId{r}});
		setExprType(r, "double");
		return r;
	}
	if (n.name == "fneg") {
		uint8_t src = genExpr(*n.args[0]);
		uint8_t res = allocTmp();
		emit(OpCode::FNeg, {RegId{src}, RegId{res}});
		setExprType(res, "double");
		return res;
	}

	size_t argCount = std::min(n.args.size(), size_t(8));
	for (size_t i = 0; i < argCount; i++) {
		uint8_t valReg = genExpr(*n.args[i]);
		emit(OpCode::Mov, {RegId{static_cast<uint8_t>(128 + i)}, RegId{valReg}});
	}

	// Save scratch registers (x16..xN) so recursive calls don't clobber them
	uint8_t savedLimit = m_nextVarReg;
	for (uint8_t r = 16; r < savedLimit; r++) {
		emit(OpCode::PushR, {RegId{r}});
	}

	emit(OpCode::Call, {LabelRef{n.name}});

	// Restore scratch registers in reverse order
	for (uint8_t r = savedLimit; r > 16;) {
		r--;
		emit(OpCode::PopR, {RegId{r}});
	}

	uint8_t result = allocTmp();
	emit(OpCode::Push, {RegId{result}, RegId{x0()}});
	// Propagate return type from called function
	auto rit = m_funRetTypes.find(n.name);
	if (rit != m_funRetTypes.end())
		setExprType(result, rit->second);
	return result;
}

// ==================== Classes ====================

void IRGen::genClassDecl(const ClassDeclNode &cls) {
	// Allocate type_id
	TypeNode classType(TypeKind::Class, cls.name);
	std::vector <uint8_t> buf;
	classType.serialize(buf);
	uint32_t typeId = (uint32_t) m_program.typePool.size();
	for (size_t i = 0; i < m_program.typePool.size(); i++)
		if (m_program.typePool[i] == buf) {
			typeId = (uint32_t) i;
			break;
		}

	// Constructor: ClassName_new() -> alloc + init fields
	std::string ctorName = cls.name + "_new";
	m_funRetTypes[ctorName] = cls.name;
	m_varRegs.clear();
	m_varTypes.clear();
	m_exprType.clear();
	m_nextVarReg = 16;
	m_labelId = 0;
	startBlock(ctorName);

	uint32_t totalSize = (uint32_t) cls.fields.size() * 9;
	uint8_t sizeReg = allocTmp();
	emit(OpCode::Mov, {RegId{sizeReg}, static_cast<int64_t>(totalSize)});
	uint8_t objReg = allocTmp();
	emit(OpCode::Alloc, {static_cast<int64_t>(typeId), RegId{sizeReg}, RegId{objReg}});

	// Initialize fields
	for (size_t i = 0; i < cls.fields.size(); i++) {
		if (cls.fields[i].init) {
			uint8_t valReg = genExpr(*cls.fields[i].init);
			uint8_t offReg = allocTmp();
			emit(OpCode::Mov, {RegId{offReg}, static_cast<int64_t>(i * 9)});
			emit(OpCode::HStore, {RegId{objReg}, RegId{offReg}, RegId{valReg}});
		}
	}

	emit(OpCode::Mov, {RegId{x0()}, RegId{objReg}});
	emitTerm(OpCode::Ret, {}, {});

	// Generate methods (desugared to ClassName_methodName)
	for (const auto &m: cls.methods) {
		std::string mname = cls.name + "_" + m.name;
		m_varRegs.clear();
		m_varTypes.clear();
		m_exprType.clear();
		m_nextVarReg = 16;
		m_classMethods[cls.name + "." + m.name] = mname;
		// Map this as first param, then method params
		m_varTypes["this"] = cls.name;
		m_varRegs["this"] = 128;
		for (size_t i = 0; i < m.params.size() && i < 7; i++)
			m_varRegs[m.params[i].name] = static_cast<uint8_t>(129 + i);
		startBlock(mname);
		for (const auto &s: m.body)
			genStmt(*s);
		if (!m_curBlock->terminator) {
			emit(OpCode::Mov, {RegId{0}, static_cast<int64_t>(0)});
			emitTerm(OpCode::Ret, {}, {});
		}
	}
}

uint8_t IRGen::genMemberExpr(const MemberExprNode &n) {
	uint8_t objReg = genExpr(*n.object);
	// Lookup field offset from class info
	// We need the object's class. For now, try to find field from the expression type
	// Fallback: use the field name directly (assumes compiler knows the class)
	uint32_t offset = 0;
	// Search all class fields for a matching field name
	for (const auto &[key, info]: m_classFields) {
		// key format: "ClassName.fieldName"
		auto dotPos = key.find('.');
		if (dotPos != std::string::npos && key.substr(dotPos + 1) == n.field) {
			offset = info.second;
			break;
		}
	}
	uint8_t offReg = allocTmp();
	emit(OpCode::Mov, {RegId{offReg}, static_cast<int64_t>(offset)});
	uint8_t result = allocTmp();
	emit(OpCode::HLoad, {RegId{objReg}, RegId{offReg}, RegId{result}});
	return result;
}

void IRGen::genMemberAssign(const MemberAssignStmtNode &n) {
	uint8_t objReg = genExpr(*n.object);
	uint8_t valReg = genExpr(*n.value);
	uint32_t offset = 0;
	for (const auto &[key, info]: m_classFields) {
		auto dotPos = key.find('.');
		if (dotPos != std::string::npos && key.substr(dotPos + 1) == n.field) {
			offset = info.second;
			break;
		}
	}
	uint8_t offReg = allocTmp();
	emit(OpCode::Mov, {RegId{offReg}, static_cast<int64_t>(offset)});
	if (n.op != MemberAssignStmtNode::Set) {
		// Compound: load old value, apply op, then store
		uint8_t oldVal = allocTmp();
		emit(OpCode::HLoad, {RegId{objReg}, RegId{offReg}, RegId{oldVal}});
		uint8_t newVal = allocTmp();
		switch (n.op) {
			case MemberAssignStmtNode::AddEq:
				emit(OpCode::Add, {RegId{newVal}, RegId{oldVal}, RegId{valReg}});
				break;
			case MemberAssignStmtNode::SubEq:
				emit(OpCode::Sub, {RegId{oldVal}, RegId{valReg}, RegId{newVal}});
				break;
			case MemberAssignStmtNode::MulEq:
				emit(OpCode::Mul, {RegId{oldVal}, RegId{valReg}, RegId{newVal}});
				break;
			case MemberAssignStmtNode::DivEq:
				emit(OpCode::Div, {RegId{oldVal}, RegId{valReg}, RegId{newVal}});
				break;
			default:
				break;
		}
		valReg = newVal;
	}
	emit(OpCode::HStore, {RegId{objReg}, RegId{offReg}, RegId{valReg}});
}

uint8_t IRGen::genMethodCall(const MethodCallNode &n) {
	// Evaluate object
	uint8_t objReg = genExpr(*n.object);

	// Find ClassName from object's type
	std::string objType = getExprType(objReg);
	std::string key = objType + "." + n.method;
	auto it = m_classMethods.find(key);
	std::string target;
	if (it != m_classMethods.end()) {
		target = it->second;  // Point.move → Point_move
	} else {
		// fallback: try method name directly
		target = n.method;
	}

	// Set up args: arg0 = this, arg1.. = method args
	emit(OpCode::Mov, {RegId{128}, RegId{objReg}});  // this = object
	size_t argCount = std::min(n.args.size(), size_t(7));
	for (size_t i = 0; i < argCount; i++) {
		uint8_t v = genExpr(*n.args[i]);
		emit(OpCode::Mov, {RegId{static_cast<uint8_t>(129 + i)}, RegId{v}});
	}

	// Save scratch regs
	uint8_t savedLimit = m_nextVarReg;
	for (uint8_t r = 16; r < savedLimit; r++)
		emit(OpCode::PushR, {RegId{r}});

	emit(OpCode::Call, {LabelRef{target}});

	// Restore
	for (uint8_t r = savedLimit; r > 16;) {
		r--;
		emit(OpCode::PopR, {RegId{r}});
	}

	uint8_t result = allocTmp();
	emit(OpCode::Push, {RegId{result}, RegId{x0()}});
	auto rit = m_funRetTypes.find(target);
	if (rit != m_funRetTypes.end())
		setExprType(result, rit->second);
	return result;
}
