#include "VM.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <bit>
#include "../Error.hpp"
#include "../ir/OpCode.hpp"

static const char *traceOpcodeName(OpCode op) {
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

#define runtime_error(msg) do { \
    std::cerr << "Runtime error: " << msg << std::endl; \
    std::cerr << "At " << __FILE__ << ":" << __LINE__ << std::endl; \
    throw RuntimeError(msg); \
} while (0)


VM::VM(Chunk chunk, bool trace)
		: m_chunk(std::move(chunk)), m_flags(0), m_trace(trace) {
	std::fill(std::begin(m_regs), std::end(m_regs), 0);
	std::fill(std::begin(m_rtag), std::end(m_rtag), 0);
}

int VM::run(int maxExecuteCount) {
	// Build JIT on first run, then execute via JIT
	if (!m_jitReady) {
		if (!tryBuildJit()) {
			runtime_error("failed to build JIT code");
		}
	}
	return runJit(maxExecuteCount);
}

// ---- JIT Implementation ----

bool VM::decodeOperandAt(uint32_t &ip, JitOperand &out) const {
	if (ip >= m_chunk.code.size()) return false;

	uint8_t tag = m_chunk.code[ip++];
	out.tag = static_cast<OperandTag>(tag);

	switch (out.tag) {
		case OperandTag::Reg:
			if (ip >= m_chunk.code.size()) return false;
			out.intVal = m_chunk.code[ip++];
			break;

		case OperandTag::Imm64:
			if (ip + 7 >= m_chunk.code.size()) return false;
			{
				uint64_t value = 0;
				value |= (static_cast<uint64_t>(m_chunk.code[ip++]) << 0);
				value |= (static_cast<uint64_t>(m_chunk.code[ip++]) << 8);
				value |= (static_cast<uint64_t>(m_chunk.code[ip++]) << 16);
				value |= (static_cast<uint64_t>(m_chunk.code[ip++]) << 24);
				value |= (static_cast<uint64_t>(m_chunk.code[ip++]) << 32);
				value |= (static_cast<uint64_t>(m_chunk.code[ip++]) << 40);
				value |= (static_cast<uint64_t>(m_chunk.code[ip++]) << 48);
				value |= (static_cast<uint64_t>(m_chunk.code[ip++]) << 56);
				out.intVal = static_cast<int64_t>(value);
			}
			break;

		case OperandTag::StrIdx:
			if (ip + 3 >= m_chunk.code.size()) return false;
			{
				uint32_t value = 0;
				value |= (static_cast<uint32_t>(m_chunk.code[ip++]) << 0);
				value |= (static_cast<uint32_t>(m_chunk.code[ip++]) << 8);
				value |= (static_cast<uint32_t>(m_chunk.code[ip++]) << 16);
				value |= (static_cast<uint32_t>(m_chunk.code[ip++]) << 24);
				out.strIdx = value;
			}
			break;

		case OperandTag::Addr:
			if (ip + 3 >= m_chunk.code.size()) return false;
			{
				uint32_t value = 0;
				value |= (static_cast<uint32_t>(m_chunk.code[ip++]) << 0);
				value |= (static_cast<uint32_t>(m_chunk.code[ip++]) << 8);
				value |= (static_cast<uint32_t>(m_chunk.code[ip++]) << 16);
				value |= (static_cast<uint32_t>(m_chunk.code[ip++]) << 24);
				out.intVal = value;  // bytecode offset stored in intVal
			}
			break;

		default:
			return false;
	}

	return true;
}

bool VM::tryBuildJit() {
	if (m_jitReady) return true;

	m_jitCode.clear();
	m_jitIpMap.clear();

	uint32_t ip = 0;
	while (ip < m_chunk.code.size()) {
		size_t jitIdx = m_jitCode.size();
		m_jitIpMap[ip] = jitIdx;

		JitInstruction instr;
		instr.byteOffset = ip;

		uint8_t opcode = m_chunk.code[ip++];
		instr.op = static_cast<OpCode>(opcode);

		int operandCount = 0;
		switch (instr.op) {
			case OpCode::Nop:
			case OpCode::Ret:
			case OpCode::Halt:
				operandCount = 0;
				break;
			case OpCode::Jnz:
			case OpCode::Jz:
			case OpCode::Jn:
			case OpCode::Call:
			case OpCode::Print:
			case OpCode::PushR:
			case OpCode::PopR:
				operandCount = 1;
				break;
			case OpCode::Push:
			case OpCode::Mov:
			case OpCode::Cmp:
			case OpCode::Jmp:
				operandCount = 2;
				break;
			case OpCode::HFree:
				operandCount = 1;
				break;
			case OpCode::Add:
			case OpCode::Sub:
			case OpCode::Mul:
			case OpCode::Div:
			case OpCode::HStore:
			case OpCode::HLoad:
			case OpCode::Alloc:
			case OpCode::Shl:
			case OpCode::Shr:
			case OpCode::FAdd:
			case OpCode::FSub:
			case OpCode::FMul:
			case OpCode::FDiv:
				operandCount = 3;
				break;
			case OpCode::ItoF:
			case OpCode::FtoI:
			case OpCode::FNeg:
				operandCount = 2;
				break;
			case OpCode::Skip:
				// 0 or 1 operands: peek if next byte is an Addr tag
				if (ip < m_chunk.code.size() &&
				    m_chunk.code[ip] == static_cast<uint8_t>(OperandTag::Addr)) {
					operandCount = 1;
				} else {
					operandCount = 0;
				}
				break;
			default:
				return false;
		}

		for (int i = 0; i < operandCount; i++) {
			JitOperand op;
			if (!decodeOperandAt(ip, op)) return false;
			instr.operands.push_back(op);
		}

		m_jitCode.push_back(std::move(instr));
	}

	m_jitReady = true;
	return true;
}

bool VM::resolveJitTarget(uint32_t byteOffset, size_t &target) const {
	auto it = m_jitIpMap.find(byteOffset);
	if (it == m_jitIpMap.end()) return false;
	target = it->second;
	return true;
}

int64_t VM::readIntOperand(const JitOperand &operand) const {
	if (operand.tag == OperandTag::Reg) {
		return m_regs[static_cast<uint8_t>(operand.intVal)];
	}
	if (operand.tag == OperandTag::Imm64) {
		return operand.intVal;
	}
	return 0;
}

std::string VM::readStringOperand(const JitOperand &operand) const {
	if (operand.tag == OperandTag::StrIdx) {
		if (operand.strIdx < m_chunk.strings.size()) {
			return m_chunk.strings[operand.strIdx];
		}
	} else if (operand.tag == OperandTag::Reg) {
		uint8_t reg = static_cast<uint8_t>(operand.intVal);
		if (m_rtag[reg] == 1) {
			uint32_t strIdx = static_cast<uint32_t>(m_regs[reg]);
			if (strIdx < m_chunk.strings.size()) {
				return m_chunk.strings[strIdx];
			}
		}
	}
	return {};
}

void VM::execJitInstruction(const JitInstruction &instr, bool &halted) {
	switch (instr.op) {
		case OpCode::Nop:
			m_jitIp++;
			break;

		case OpCode::Push: {
			auto &dst = instr.operands[0];
			auto &val = instr.operands[1];
			uint8_t dstReg = static_cast<uint8_t>(dst.intVal);
			if (val.tag == OperandTag::Reg) {
				m_regs[dstReg] = m_regs[static_cast<uint8_t>(val.intVal)];
				m_rtag[dstReg] = m_rtag[static_cast<uint8_t>(val.intVal)];
			} else if (val.tag == OperandTag::Imm64) {
				m_regs[dstReg] = val.intVal;
				m_rtag[dstReg] = 0;
			} else if (val.tag == OperandTag::StrIdx) {
				m_regs[dstReg] = val.strIdx;
				m_rtag[dstReg] = 1;
			}
			m_jitIp++;
			break;
		}

		case OpCode::Pop:
			m_jitIp++;
			break;

		case OpCode::PushR: {
			auto &reg = instr.operands[0];
			uint8_t r = static_cast<uint8_t>(reg.intVal);
			m_dataStack.push({m_regs[r], m_rtag[r]});
			m_jitIp++;
			break;
		}

		case OpCode::PopR: {
			auto &reg = instr.operands[0];
			uint8_t r = static_cast<uint8_t>(reg.intVal);
			if (!m_dataStack.empty()) {
				auto [val, tag] = m_dataStack.top();
				m_dataStack.pop();
				m_regs[r] = val;
				m_rtag[r] = tag;
			}
			m_jitIp++;
			break;
		}

		case OpCode::Alloc: {
			// operands[0] = type_id (Imm64), [1] = size_reg, [2] = result_reg
			uint32_t typeId = static_cast<uint32_t>(instr.operands[0].intVal);
			uint32_t size = static_cast<uint32_t>(readIntOperand(instr.operands[1]));
			uint8_t resReg = static_cast<uint8_t>(instr.operands[2].intVal);
			uint32_t offset = m_heap.alloc(size, typeId);
			m_regs[resReg] = static_cast<int64_t>(offset);
			m_rtag[resReg] = TAG_HEAP;
			m_jitIp++;
			break;
		}

		case OpCode::HFree: {
			auto &baseOp = instr.operands[0];
			uint8_t baseReg = static_cast<uint8_t>(baseOp.intVal);
			m_heap.free(static_cast<uint32_t>(m_regs[baseReg]));
			m_jitIp++;
			break;
		}

		case OpCode::HStore: {
			// operands[0]=base_reg, [1]=offset_reg, [2]=value_reg
			auto &baseOp = instr.operands[0];
			auto &offOp = instr.operands[1];
			auto &valOp = instr.operands[2];
			uint32_t base = static_cast<uint32_t>(m_regs[static_cast<uint8_t>(baseOp.intVal)]);
			uint32_t off = static_cast<uint32_t>(m_regs[static_cast<uint8_t>(offOp.intVal)]);
			uint8_t valReg = static_cast<uint8_t>(valOp.intVal);
			m_heap.store(base + off, m_regs[valReg], m_rtag[valReg]);
			m_jitIp++;
			break;
		}

		case OpCode::HLoad: {
			// operands[0]=base_reg, [1]=offset_reg, [2]=result_reg
			auto &baseOp = instr.operands[0];
			auto &offOp = instr.operands[1];
			auto &resultOp = instr.operands[2];
			uint32_t base = static_cast<uint32_t>(m_regs[static_cast<uint8_t>(baseOp.intVal)]);
			uint32_t off = static_cast<uint32_t>(m_regs[static_cast<uint8_t>(offOp.intVal)]);
			uint8_t resultReg = static_cast<uint8_t>(resultOp.intVal);
			auto [val, tag] = m_heap.load(base + off);
			m_regs[resultReg] = val;
			m_rtag[resultReg] = tag;
			m_jitIp++;
			break;
		}

		case OpCode::Mov: {
			auto &dst = instr.operands[0];
			auto &val = instr.operands[1];
			uint8_t dstReg = static_cast<uint8_t>(dst.intVal);
			if (val.tag == OperandTag::Imm64) {
				m_regs[dstReg] = val.intVal;
				m_rtag[dstReg] = 0;
			} else if (val.tag == OperandTag::Reg) {
				m_regs[dstReg] = m_regs[static_cast<uint8_t>(val.intVal)];
				m_rtag[dstReg] = m_rtag[static_cast<uint8_t>(val.intVal)];
			}
			m_jitIp++;
			break;
		}
		case OpCode::Add: {
			auto &dst = instr.operands[0];
			auto &src = instr.operands[1];
			auto &val = instr.operands[2];
			uint8_t srcReg = static_cast<uint8_t>(src.intVal);
			uint8_t dstReg = static_cast<uint8_t>(dst.intVal);
			int64_t operand = readIntOperand(val);
			m_regs[dstReg] = m_regs[srcReg] + operand;
			m_rtag[dstReg] = 0;
			m_jitIp++;
			break;
		}

		case OpCode::Sub: {
			auto &src = instr.operands[0];
			auto &val = instr.operands[1];
			auto &dst = instr.operands[2];
			uint8_t srcReg = static_cast<uint8_t>(src.intVal);
			uint8_t dstReg = static_cast<uint8_t>(dst.intVal);
			int64_t operand = readIntOperand(val);
			m_regs[dstReg] = m_regs[srcReg] - operand;
			m_rtag[dstReg] = 0;
			m_jitIp++;
			break;
		}

		case OpCode::Mul: {
			auto &src = instr.operands[0];
			auto &val = instr.operands[1];
			auto &dst = instr.operands[2];
			uint8_t srcReg = static_cast<uint8_t>(src.intVal);
			uint8_t dstReg = static_cast<uint8_t>(dst.intVal);
			int64_t operand = readIntOperand(val);
			m_regs[dstReg] = m_regs[srcReg] * operand;
			m_rtag[dstReg] = 0;
			m_jitIp++;
			break;
		}

		case OpCode::Div: {
			auto &src = instr.operands[0];
			auto &val = instr.operands[1];
			auto &dst = instr.operands[2];
			uint8_t srcReg = static_cast<uint8_t>(src.intVal);
			uint8_t dstReg = static_cast<uint8_t>(dst.intVal);
			int64_t operand = readIntOperand(val);
			if (operand == 0) {
				runtime_error("division by zero");
			}
			m_regs[dstReg] = m_regs[srcReg] / operand;
			m_rtag[dstReg] = 0;
			m_jitIp++;
			break;
		}

		case OpCode::Shl: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			int64_t shift = readIntOperand(instr.operands[1]);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			m_regs[dstReg] = m_regs[srcReg] << shift;
			m_rtag[dstReg] = 0;
			m_jitIp++;
			break;
		}

		case OpCode::Shr: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			int64_t shift = readIntOperand(instr.operands[1]);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			m_regs[dstReg] = m_regs[srcReg] >> shift;
			m_rtag[dstReg] = 0;
			m_jitIp++;
			break;
		}

		case OpCode::FAdd: {
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[1].intVal);
			uint8_t valReg = static_cast<uint8_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			m_regs[dstReg] = std::bit_cast<int64_t>(a + b);
			m_rtag[dstReg] = TAG_FLOAT;
			m_jitIp++;
			break;
		}

		case OpCode::FSub: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t valReg = static_cast<uint8_t>(instr.operands[1].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			m_regs[dstReg] = std::bit_cast<int64_t>(a - b);
			m_rtag[dstReg] = TAG_FLOAT;
			m_jitIp++;
			break;
		}

		case OpCode::FMul: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t valReg = static_cast<uint8_t>(instr.operands[1].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			m_regs[dstReg] = std::bit_cast<int64_t>(a * b);
			m_rtag[dstReg] = TAG_FLOAT;
			m_jitIp++;
			break;
		}

		case OpCode::FDiv: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t valReg = static_cast<uint8_t>(instr.operands[1].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			m_regs[dstReg] = std::bit_cast<int64_t>(a / b);
			m_rtag[dstReg] = TAG_FLOAT;
			m_jitIp++;
			break;
		}

		case OpCode::ItoF: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[1].intVal);
			double d = static_cast<double>(m_regs[srcReg]);
			m_regs[dstReg] = std::bit_cast<int64_t>(d);
			m_rtag[dstReg] = TAG_FLOAT;
			m_jitIp++;
			break;
		}

		case OpCode::FtoI: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[1].intVal);
			double d = std::bit_cast<double>(m_regs[srcReg]);
			m_regs[dstReg] = static_cast<int64_t>(d);
			m_rtag[dstReg] = 0;
			m_jitIp++;
			break;
		}

		case OpCode::FNeg: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[1].intVal);
			double d = std::bit_cast<double>(m_regs[srcReg]);
			m_regs[dstReg] = std::bit_cast<int64_t>(-d);
			m_rtag[dstReg] = TAG_FLOAT;
			m_jitIp++;
			break;
		}

		case OpCode::Cmp: {
			auto &r0 = instr.operands[0];
			auto &r1 = instr.operands[1];
			uint8_t reg0 = static_cast<uint8_t>(r0.intVal);
			uint8_t reg1 = static_cast<uint8_t>(r1.intVal);
			int64_t val0 = m_regs[reg0];
			int64_t val1 = m_regs[reg1];
			m_flags = 0;
			if (val0 == val1) m_flags |= FLAG_ZERO;
			if (val0 < val1) m_flags |= FLAG_NEG;
			m_jitIp++;
			break;
		}

		case OpCode::Jmp: {
			auto &addrTrue = instr.operands[0];
			auto &addrFalse = instr.operands[1];
			uint32_t targetOffset;
			if (m_flags & FLAG_ZERO) {
				targetOffset = static_cast<uint32_t>(addrTrue.intVal);
			} else {
				targetOffset = static_cast<uint32_t>(addrFalse.intVal);
			}
			size_t target;
			if (!resolveJitTarget(targetOffset, target)) {
				halted = true;
				return;
			}
			m_jitIp = target;
			break;
		}

		case OpCode::Jnz: {
			auto &addr = instr.operands[0];
			if (!(m_flags & FLAG_ZERO)) {
				size_t target;
				if (!resolveJitTarget(static_cast<uint32_t>(addr.intVal), target)) {
					halted = true;
					return;
				}
				m_jitIp = target;
			} else {
				m_jitIp++;
			}
			break;
		}

		case OpCode::Jz: {
			auto &addr = instr.operands[0];
			if (m_flags & FLAG_ZERO) {
				size_t target;
				if (!resolveJitTarget(static_cast<uint32_t>(addr.intVal), target)) {
					halted = true;
					return;
				}
				m_jitIp = target;
			} else {
				m_jitIp++;
			}
			break;
		}

		case OpCode::Jn: {
			auto &addr = instr.operands[0];
			if (m_flags & FLAG_NEG) {
				size_t target;
				if (!resolveJitTarget(static_cast<uint32_t>(addr.intVal), target)) {
					halted = true;
					return;
				}
				m_jitIp = target;
			} else {
				m_jitIp++;
			}
			break;
		}

		case OpCode::Skip: {
			if (!instr.operands.empty()) {
				size_t target;
				if (!resolveJitTarget(static_cast<uint32_t>(instr.operands[0].intVal), target)) {
					halted = true;
					return;
				}
				m_jitIp = target;
			} else {
				halted = true;
			}
			break;
		}

		case OpCode::Call: {
			auto &addr = instr.operands[0];
			size_t target;
			if (!resolveJitTarget(static_cast<uint32_t>(addr.intVal), target)) {
				halted = true;
				return;
			}
			m_callStack.push(m_jitIp + 1);
			m_jitIp = target;
			break;
		}

		case OpCode::Ret: {
			if (m_callStack.empty()) {
				halted = true;
				return;
			}
			m_jitIp = m_callStack.top();
			m_callStack.pop();
			break;
		}

		case OpCode::Print: {
			auto &val = instr.operands[0];
			if (val.tag == OperandTag::Imm64) {
				std::cout << val.intVal << "\n";
			} else if (val.tag == OperandTag::StrIdx) {
				if (val.strIdx < m_chunk.strings.size()) {
					std::cout << m_chunk.strings[val.strIdx] << "\n";
				}
			} else if (val.tag == OperandTag::Reg) {
				uint8_t reg = static_cast<uint8_t>(val.intVal);
				if (m_rtag[reg] == 0) {
					std::cout << m_regs[reg] << "\n";
				} else if (m_rtag[reg] == 1) {
					uint32_t strIdx = static_cast<uint32_t>(m_regs[reg]);
					if (strIdx < m_chunk.strings.size()) {
						std::cout << m_chunk.strings[strIdx] << "\n";
					}
				}
			}
			m_jitIp++;
			break;
		}

		case OpCode::Halt:
			halted = true;
			break;

		default:
			halted = true;
			break;
	}
}

static std::string formatTraceOperand(const VM::JitOperand &op, const Chunk &chunk) {
	std::ostringstream oss;
	switch (op.tag) {
		case OperandTag::Reg:
			oss << "r" << (int)op.intVal;
			break;
		case OperandTag::Imm64:
			oss << "#" << op.intVal;
			break;
		case OperandTag::StrIdx:
			oss << "str[" << op.strIdx << "]";
			if (op.strIdx < chunk.strings.size())
				oss << "(\"" << chunk.strings[op.strIdx] << "\")";
			break;
		case OperandTag::Addr:
			oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << (uint32_t)op.intVal << std::dec;
			break;
	}
	return oss.str();
}

int VM::runJit(int maxExecuteCount) {
	int executeCount = 0;
	bool halted = false;
	m_jitIp = 0;

	while (m_jitIp < m_jitCode.size() && executeCount < maxExecuteCount && !halted) {
		executeCount++;
		const auto &instr = m_jitCode[m_jitIp];

		if (m_trace) {
			std::cerr << "[" << std::dec << std::setw(6) << std::setfill(' ') << executeCount << "] "
			          << "0x" << std::hex << std::setw(6) << std::setfill('0') << instr.byteOffset << std::dec << " " << std::setfill(' ')
			          << std::setw(7) << std::left << traceOpcodeName(instr.op);
			for (size_t i = 0; i < instr.operands.size(); i++) {
				if (i > 0) std::cerr << ",";
				std::cerr << " " << formatTraceOperand(instr.operands[i], m_chunk);
			}
			std::cerr << std::endl;
		}

		execJitInstruction(instr, halted);
	}

	return 0;
}

int VM::run() {
	constexpr int unlimited = 2147483647;
	return run(unlimited);
}
