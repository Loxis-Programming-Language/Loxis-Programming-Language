#include "VM.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <cstdio>
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
		case OpCode::Rem:   return "rem";
		case OpCode::BitAnd:return "band";
		case OpCode::BitOr: return "bor";
		case OpCode::BitXor:return "bxor";
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
		case OpCode::GLoad: return "gload";
		case OpCode::GStore:return "gstore";
		case OpCode::Cmp:   return "cmp";
		case OpCode::Jmp:   return "jmp";
		case OpCode::Jnz:   return "jnz";
		case OpCode::Jz:    return "jz";
		case OpCode::Skip:  return "skip";
		case OpCode::Call:  return "call";
		case OpCode::Ret:   return "ret";
		case OpCode::Br:    return "br";
		case OpCode::Jn:    return "jn";
		case OpCode::NullChk: return "nullchk";
		case OpCode::CallInd: return "callind";
		case OpCode::VCall:  return "vcall";
		case OpCode::ICall:  return "icall";
		case OpCode::TypeOf: return "typeof";
		case OpCode::Syscall: return "syscall";
		case OpCode::Write:  return "write";
		case OpCode::Print:  return "print";
		case OpCode::StrLen: return "strlen";
		case OpCode::ToStr:  return "tostr";
		case OpCode::Halt:   return "halt";
	}
	return "?";
}

#define runtime_error(msg) do { \
    std::cerr << "Runtime error: " << msg << std::endl; \
    std::cerr << "At " << __FILE__ << ":" << __LINE__ << std::endl; \
    throw RuntimeError(msg); \
} while (0)


VM::VM(Chunk chunk, bool trace, GCMode gcMode, uint32_t heapSlots)
		: m_chunk(std::move(chunk)), m_flags(0), m_trace(trace) {
	if (gcMode == GCMode::AARCLocal) {
		m_heap.reserveSlots(heapSlots ? heapSlots : (1u << 20));
	}
	m_gcOwned = makeGCBackend(gcMode, m_heap);
	m_gc = m_gcOwned.get();
	std::fill(std::begin(m_regs), std::end(m_regs), 0);
	std::fill(std::begin(m_rtag), std::end(m_rtag), 0);
	std::fill(std::begin(m_globals), std::end(m_globals), 0);
	std::fill(std::begin(m_globalTags), std::end(m_globalTags), 0);
}

// Acquire before release.  The reverse order is not equivalent: if the value
// being installed is reachable only through the reference being dropped (a
// register spill restored after a call, or an object loaded out of a dying
// object's field), remove-then-add lets the count hit zero in between and the
// collector reclaims the object we were about to root -- which then comes back
// as a dangling handle.
void VM::setReg(uint8_t reg, int64_t value, uint8_t tag) {
	const uint8_t oldTag = m_rtag[reg];
	const int64_t oldValue = m_regs[reg];
	if (oldTag == TAG_HEAP && tag == TAG_HEAP && oldValue == value) return;
	if (tag == TAG_HEAP) m_gc->rootAdd(static_cast<uint32_t>(value));
	m_regs[reg] = value;
	m_rtag[reg] = tag;
	if (oldTag == TAG_HEAP) m_gc->rootRemove(static_cast<uint32_t>(oldValue));
}

void VM::setGlobal(uint8_t slot, int64_t value, uint8_t tag) {
	const uint8_t oldTag = m_globalTags[slot];
	const int64_t oldValue = m_globals[slot];
	if (oldTag == TAG_HEAP && tag == TAG_HEAP && oldValue == value) return;
	if (tag == TAG_HEAP) m_gc->rootAdd(static_cast<uint32_t>(value));
	m_globals[slot] = value;
	m_globalTags[slot] = tag;
	if (oldTag == TAG_HEAP) m_gc->rootRemove(static_cast<uint32_t>(oldValue));
}

void VM::finishGC() {
	for (uint16_t r = 0; r < 256; r++) setReg(static_cast<uint8_t>(r), 0, TAG_INT);
	for (uint16_t g = 0; g < 256; g++) setGlobal(static_cast<uint8_t>(g), 0, TAG_INT);
	while (!m_dataStack.empty()) {
		auto [val, tag] = m_dataStack.top();
		m_dataStack.pop();
		if (tag == TAG_HEAP) m_gc->rootRemove(static_cast<uint32_t>(val));
	}
	m_gc->shutdown();
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
	uint32_t lastByteOffset = 0;
	OpCode lastOp = OpCode::Nop;
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
			case OpCode::Write:
				case OpCode::Print:
			case OpCode::PushR:
			case OpCode::PopR:
				operandCount = 1;
				break;
			case OpCode::Push:
			case OpCode::Mov:
			case OpCode::Cmp:
			case OpCode::Jmp:
			case OpCode::GLoad:
			case OpCode::GStore:
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
			case OpCode::Rem:
			case OpCode::BitAnd:
			case OpCode::BitOr:
			case OpCode::BitXor:
			case OpCode::FAdd:
			case OpCode::FSub:
			case OpCode::FMul:
			case OpCode::FDiv:
				operandCount = 3;
				break;
			case OpCode::ItoF:
			case OpCode::FtoI:
			case OpCode::FNeg:
			case OpCode::NullChk:
			case OpCode::TypeOf:
			case OpCode::StrLen:
			case OpCode::ToStr:
				operandCount = 2;
				break;
			case OpCode::CallInd:
				operandCount = 2;
				break;
			case OpCode::Syscall:
				operandCount = 1; // syscall number in reg
				break;
			case OpCode::VCall:
				operandCount = 4; // obj_reg, slot_imm, nargs_imm, return_addr
				break;
			case OpCode::ICall:
				operandCount = 5; // obj_reg, iface_typeid_imm, method_idx_imm, nargs_imm, return_addr
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
			if (!decodeOperandAt(ip, op)) {
				if (m_trace) {
					std::cerr << "Failed to decode operand " << i
					          << " for opcode " << traceOpcodeName(instr.op)
					          << " at byte offset " << instr.byteOffset
					          << "; previous opcode " << traceOpcodeName(lastOp)
					          << " at byte offset " << lastByteOffset << std::endl;
				}
				return false;
			}
			instr.operands.push_back(op);
		}

		m_jitCode.push_back(std::move(instr));
		lastByteOffset = m_jitCode.back().byteOffset;
		lastOp = m_jitCode.back().op;
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
			if (strIdx < m_runtimeStrings.size()) {
				return m_runtimeStrings[strIdx];
			}
			if (strIdx < m_chunk.strings.size()) {
				return m_chunk.strings[strIdx];
			}
		}
	}
	return {};
}

uint32_t VM::internRuntimeString(const std::string& s) {
	uint32_t idx = static_cast<uint32_t>(m_runtimeStrings.size());
	m_runtimeStrings.push_back(s);
	return idx;
}

void VM::invokeSyscall(uint8_t op) {
	enum { S_READCHAR=0, S_S2I=1, S_TYPEOF=2, S_CHR=3, S_WRITE=10, S_PRINT=11,
	       S_HALLOC=20, S_HREAD=21, S_HSET=22, S_HFREE=23,
	       S_STRLEN=30, S_I2S=31, S_STRCHAR=32, S_STRSLICE=33, S_STRINDEXOF=34,
	       S_FILEREAD=40, S_FILEWRITE=41, S_FILEAPPEND=42, S_FILEREMOVE=43,
	       S_ERROR=99 };
	auto regString = [&](uint8_t reg) -> std::string {
		uint32_t si=static_cast<uint32_t>(m_regs[reg]);
		if(si<m_runtimeStrings.size())return m_runtimeStrings[si];
		if(si<m_chunk.strings.size())return m_chunk.strings[si];
		return {};
	};
	switch (op) {
	case S_READCHAR: { int c=std::cin.get(); setReg(0, c, TAG_INT); break; }
	case S_S2I: {
		uint32_t si=static_cast<uint32_t>(m_regs[1]); std::string s;
		if(si<m_runtimeStrings.size())s=m_runtimeStrings[si];
		else if(si<m_chunk.strings.size())s=m_chunk.strings[si];
		int64_t parsed=0; try{parsed=std::stoll(s);}catch(...){parsed=0;}
		setReg(0, parsed, TAG_INT); break; }
	case S_TYPEOF: { setReg(0, m_heap.getTypeId(static_cast<uint32_t>(m_regs[1])), TAG_INT); break; }
		case S_CHR: { setReg(0, internRuntimeString(std::string(1,static_cast<char>(static_cast<int>(m_regs[1])))), TAG_STR); break; }
	case S_WRITE: {
		int64_t v=m_regs[1];
		if(m_rtag[1]==0)std::cout<<v;
		else if(m_rtag[1]==1){uint32_t si=static_cast<uint32_t>(v);
		if(si<m_runtimeStrings.size())std::cout<<m_runtimeStrings[si];
		else if(si<m_chunk.strings.size())std::cout<<m_chunk.strings[si];} break; }
	case S_PRINT: {
		int64_t v=m_regs[1];
		if(m_rtag[1]==0)std::cout<<v<<"\n";
		else if(m_rtag[1]==1){uint32_t si=static_cast<uint32_t>(v);
		if(si<m_runtimeStrings.size())std::cout<<m_runtimeStrings[si]<<"\n";
		else if(si<m_chunk.strings.size())std::cout<<m_chunk.strings[si]<<"\n";} break; }
	case S_HALLOC: {
		uint32_t h=m_heap.alloc(static_cast<uint32_t>(m_regs[1]),0);
		if (h==Heap::INVALID) runtime_error("heap exhausted (slot space full)");
		m_gc->onAlloc(h); setReg(0, h, TAG_HEAP); break; }
	case S_HREAD: {
		auto c=m_heap.loadCell(static_cast<uint32_t>(m_regs[1]), static_cast<uint32_t>(m_regs[2]));
		setReg(0, c.first, c.second); break; }
	case S_HSET: {
		uint32_t base=static_cast<uint32_t>(m_regs[1]);
		uint32_t off=static_cast<uint32_t>(m_regs[2]);
		auto old=m_heap.loadCell(base, off);
		const uint8_t newTag=m_rtag[3];
		if (old.second==TAG_HEAP && newTag==TAG_HEAP && old.first==m_regs[3]) break;
		if (old.second==TAG_HEAP) m_gc->edgeRemove(base, static_cast<uint32_t>(old.first));
		m_heap.storeCell(base, off, m_regs[3], newTag);
		if (newTag==TAG_HEAP) m_gc->edgeAdd(base, static_cast<uint32_t>(m_regs[3]));
		break; }
	case S_HFREE: { m_gc->onExplicitFree(static_cast<uint32_t>(m_regs[1])); break; }
	case S_STRLEN: {
		uint32_t si=static_cast<uint32_t>(m_regs[1]); int64_t len=0;
		if(si<m_runtimeStrings.size())len=m_runtimeStrings[si].size();
		else if(si<m_chunk.strings.size())len=m_chunk.strings[si].size();
		setReg(0, len, TAG_INT); break; }
	case S_I2S: { setReg(0, internRuntimeString(std::to_string(m_regs[1])), TAG_STR); break; }
	case S_STRCHAR: {
		uint32_t si=static_cast<uint32_t>(m_regs[1]); std::string s;
		if(si<m_runtimeStrings.size())s=m_runtimeStrings[si];
		else if(si<m_chunk.strings.size())s=m_chunk.strings[si];
		int64_t idx=m_regs[2];
		setReg(0, (idx>=0 && static_cast<size_t>(idx)<s.size()) ? static_cast<unsigned char>(s[static_cast<size_t>(idx)]) : -1, TAG_INT);
		break; }
	case S_STRSLICE: {
		uint32_t si=static_cast<uint32_t>(m_regs[1]); std::string s;
		if(si<m_runtimeStrings.size())s=m_runtimeStrings[si];
		else if(si<m_chunk.strings.size())s=m_chunk.strings[si];
		int64_t start=m_regs[2], count=m_regs[3];
		if(start<0)start=0;
		if(count<0)count=0;
		size_t begin=static_cast<size_t>(start);
		if(begin>s.size())begin=s.size();
		size_t n=static_cast<size_t>(count);
		if(begin+n>s.size())n=s.size()-begin;
		setReg(0, internRuntimeString(s.substr(begin,n)), TAG_STR); break; }
	case S_STRINDEXOF: {
		uint32_t hi=static_cast<uint32_t>(m_regs[1]); std::string h;
		uint32_t ni=static_cast<uint32_t>(m_regs[2]); std::string needle;
		if(hi<m_runtimeStrings.size())h=m_runtimeStrings[hi];
		else if(hi<m_chunk.strings.size())h=m_chunk.strings[hi];
		if(ni<m_runtimeStrings.size())needle=m_runtimeStrings[ni];
		else if(ni<m_chunk.strings.size())needle=m_chunk.strings[ni];
		auto pos=h.find(needle);
		setReg(0, (pos==std::string::npos) ? -1 : static_cast<int64_t>(pos), TAG_INT);
		break; }
	case S_FILEREAD: {
		std::ifstream in(regString(1), std::ios::binary);
		if(!in){setReg(0, internRuntimeString(""), TAG_STR); break;}
		std::ostringstream ss; ss<<in.rdbuf();
		setReg(0, internRuntimeString(ss.str()), TAG_STR); break; }
	case S_FILEWRITE: {
		std::ofstream out(regString(1), std::ios::binary | std::ios::trunc);
		if(!out){setReg(0, 0, TAG_INT); break;}
		out<<regString(2);
		setReg(0, out.good()?1:0, TAG_INT); break; }
	case S_FILEAPPEND: {
		std::ofstream out(regString(1), std::ios::binary | std::ios::app);
		if(!out){setReg(0, 0, TAG_INT); break;}
		out<<regString(2);
		setReg(0, out.good()?1:0, TAG_INT); break; }
	case S_FILEREMOVE: {
		setReg(0, (std::remove(regString(1).c_str())==0)?1:0, TAG_INT); break; }
	case S_ERROR: {
		uint32_t si=static_cast<uint32_t>(m_regs[1]);
		if(si<m_runtimeStrings.size())std::cerr<<m_runtimeStrings[si]<<std::endl;
		else if(si<m_chunk.strings.size())std::cerr<<m_chunk.strings[si]<<std::endl;
		setReg(0, 1, TAG_INT); break; }
	default: setReg(0, -1, TAG_INT); break;
	}
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
				uint8_t srcReg = static_cast<uint8_t>(val.intVal);
				setReg(dstReg, m_regs[srcReg], m_rtag[srcReg]);
			} else if (val.tag == OperandTag::Imm64) {
				setReg(dstReg, val.intVal, TAG_INT);
			} else if (val.tag == OperandTag::StrIdx) {
				setReg(dstReg, val.strIdx, TAG_STR);
			} else if (val.tag == OperandTag::Addr) {
				setReg(dstReg, val.intVal, TAG_INT);
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
			// The data stack is a root set too: the register keeps its own
			// reference, and the saved copy adds a second one.
			if (m_rtag[r] == TAG_HEAP) m_gc->rootAdd(static_cast<uint32_t>(m_regs[r]));
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
				// Install into the register first (acquire), then drop the
				// spill-slot reference (release).  A callee may have clobbered
				// this register, so the spill slot can be the object's only
				// reference -- releasing it first would reclaim it.
				setReg(r, val, tag);
				if (tag == TAG_HEAP) m_gc->rootRemove(static_cast<uint32_t>(val));
			}
			m_jitIp++;
			break;
		}

		case OpCode::Alloc: {
			// operands[0] = type_id (Imm64), [1] = size_reg, [2] = result_reg
			uint32_t typeId = static_cast<uint32_t>(instr.operands[0].intVal);
			uint32_t size = static_cast<uint32_t>(readIntOperand(instr.operands[1]));
			uint8_t resReg = static_cast<uint8_t>(instr.operands[2].intVal);
			uint32_t handle = m_heap.alloc(size, typeId);
			if (handle == Heap::INVALID) runtime_error("heap exhausted (slot space full)");
			m_gc->onAlloc(handle);
			setReg(resReg, static_cast<int64_t>(handle), TAG_HEAP);
			m_jitIp++;
			break;
		}

		case OpCode::HFree: {
			auto &baseOp = instr.operands[0];
			uint8_t baseReg = static_cast<uint8_t>(baseOp.intVal);
			m_gc->onExplicitFree(static_cast<uint32_t>(m_regs[baseReg]));
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
			// The old cell's own tag decides whether an edge existed — the
			// writing register says nothing about what used to be there.
			auto old = m_heap.loadCell(base, off);
			const uint8_t newTag = m_rtag[valReg];
			if (old.second == TAG_HEAP && newTag == TAG_HEAP && old.first == m_regs[valReg]) {
				m_jitIp++;
				break;
			}
			if (old.second == TAG_HEAP) {
				m_gc->edgeRemove(base, static_cast<uint32_t>(old.first));
			}
			m_heap.storeCell(base, off, m_regs[valReg], newTag);
			if (newTag == TAG_HEAP) {
				m_gc->edgeAdd(base, static_cast<uint32_t>(m_regs[valReg]));
			}
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
			auto [val, tag] = m_heap.loadCell(base, off);
			setReg(resultReg, val, tag);
			m_jitIp++;
			break;
		}

		case OpCode::Mov: {
			auto &dst = instr.operands[0];
			auto &val = instr.operands[1];
			uint8_t dstReg = static_cast<uint8_t>(dst.intVal);
			if (val.tag == OperandTag::Imm64) {
				setReg(dstReg, val.intVal, TAG_INT);
			} else if (val.tag == OperandTag::Addr) {
				setReg(dstReg, val.intVal, TAG_INT);
			} else if (val.tag == OperandTag::Reg) {
				uint8_t srcReg = static_cast<uint8_t>(val.intVal);
				setReg(dstReg, m_regs[srcReg], m_rtag[srcReg]);
			}
			m_jitIp++;
			break;
		}

		case OpCode::GLoad: {
			uint32_t slot = static_cast<uint32_t>(readIntOperand(instr.operands[0]));
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[1].intVal);
			if (slot >= 256) runtime_error("global slot out of bounds");
			setReg(dstReg, m_globals[slot], m_globalTags[slot]);
			m_jitIp++;
			break;
		}

		case OpCode::GStore: {
			uint32_t slot = static_cast<uint32_t>(readIntOperand(instr.operands[0]));
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[1].intVal);
			if (slot >= 256) runtime_error("global slot out of bounds");
			setGlobal(static_cast<uint8_t>(slot), m_regs[srcReg], m_rtag[srcReg]);
			m_jitIp++;
			break;
		}
		case OpCode::Add: {
			auto &dst = instr.operands[0];
			auto &src = instr.operands[1];
			auto &val = instr.operands[2];
			uint8_t srcReg = static_cast<uint8_t>(src.intVal);
			uint8_t dstReg = static_cast<uint8_t>(dst.intVal);

			// String concatenation: both operands tagged as strings
			if (m_rtag[srcReg] == 1 && val.tag == OperandTag::Reg) {
				uint8_t valReg = static_cast<uint8_t>(val.intVal);
				if (m_rtag[valReg] == 1) {
					std::string lhs = readStringOperand(src);
					std::string rhs = readStringOperand(val);
					setReg(dstReg, static_cast<int64_t>(internRuntimeString(lhs + rhs)), TAG_STR);
					m_jitIp++;
					break;
				}
			}
			int64_t operand = readIntOperand(val);
			setReg(dstReg, m_regs[srcReg] + operand, TAG_INT);
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
			setReg(dstReg, m_regs[srcReg] - operand, TAG_INT);
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
			setReg(dstReg, m_regs[srcReg] * operand, TAG_INT);
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
			setReg(dstReg, m_regs[srcReg] / operand, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Shl: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			int64_t shift = readIntOperand(instr.operands[1]);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			setReg(dstReg, m_regs[srcReg] << shift, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Shr: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			int64_t shift = readIntOperand(instr.operands[1]);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			setReg(dstReg, m_regs[srcReg] >> shift, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Rem: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			int64_t operand = readIntOperand(instr.operands[1]);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			if (operand == 0) runtime_error("remainder by zero");
			setReg(dstReg, m_regs[srcReg] % operand, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::BitAnd:
		case OpCode::BitOr:
		case OpCode::BitXor: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			int64_t operand = readIntOperand(instr.operands[1]);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			int64_t result;
			if (instr.op == OpCode::BitAnd) result = m_regs[srcReg] & operand;
			else if (instr.op == OpCode::BitOr) result = m_regs[srcReg] | operand;
			else result = m_regs[srcReg] ^ operand;
			setReg(dstReg, result, TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::FAdd: {
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[1].intVal);
			uint8_t valReg = static_cast<uint8_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			setReg(dstReg, std::bit_cast<int64_t>(a + b), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::FSub: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t valReg = static_cast<uint8_t>(instr.operands[1].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			setReg(dstReg, std::bit_cast<int64_t>(a - b), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::FMul: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t valReg = static_cast<uint8_t>(instr.operands[1].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			setReg(dstReg, std::bit_cast<int64_t>(a * b), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::FDiv: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t valReg = static_cast<uint8_t>(instr.operands[1].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[2].intVal);
			double a = std::bit_cast<double>(m_regs[srcReg]);
			double b = std::bit_cast<double>(m_regs[valReg]);
			setReg(dstReg, std::bit_cast<int64_t>(a / b), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::ItoF: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[1].intVal);
			double d = static_cast<double>(m_regs[srcReg]);
			setReg(dstReg, std::bit_cast<int64_t>(d), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::FtoI: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[1].intVal);
			double d = std::bit_cast<double>(m_regs[srcReg]);
			setReg(dstReg, static_cast<int64_t>(d), TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::FNeg: {
			uint8_t srcReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t dstReg = static_cast<uint8_t>(instr.operands[1].intVal);
			double d = std::bit_cast<double>(m_regs[srcReg]);
			setReg(dstReg, std::bit_cast<int64_t>(-d), TAG_FLOAT);
			m_jitIp++;
			break;
		}

		case OpCode::Cmp: {
			auto &r0 = instr.operands[0];
			auto &r1 = instr.operands[1];
			uint8_t reg0 = static_cast<uint8_t>(r0.intVal);
			uint8_t reg1 = static_cast<uint8_t>(r1.intVal);
			m_flags = 0;
			// String comparison
			if (m_rtag[reg0] == 1 && m_rtag[reg1] == 1) {
				std::string s0 = readStringOperand(r0);
				std::string s1 = readStringOperand(r1);
				if (s0 == s1) m_flags |= FLAG_ZERO;
				if (s0 < s1)  m_flags |= FLAG_NEG;
			} else {
				int64_t val0 = m_regs[reg0];
				int64_t val1 = m_regs[reg1];
				if (val0 == val1) m_flags |= FLAG_ZERO;
				if (val0 < val1)  m_flags |= FLAG_NEG;
			}
			m_jitIp++;
			break;
		}

		case OpCode::Jmp: {
			auto &addr = instr.operands[0];
			size_t target;
			if (!resolveJitTarget(static_cast<uint32_t>(addr.intVal), target)) {
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

		case OpCode::NullChk: {
			auto &val = instr.operands[0];
			uint8_t reg = static_cast<uint8_t>(val.intVal);
			bool isNull = false;
			if (val.tag == OperandTag::Reg) {
				// Check rtag for TAG_NULL, or value 0 with tag 0 (int 0 = null ref)
				isNull = (m_rtag[reg] == TAG_NULL) ||
				         (m_rtag[reg] == 0 && m_regs[reg] == 0);
			} else if (val.tag == OperandTag::Imm64) {
				isNull = (val.intVal == 0);
			}
			if (isNull) {
				size_t target;
				if (!resolveJitTarget(static_cast<uint32_t>(instr.operands[1].intVal), target)) {
					halted = true; return;
				}
				m_jitIp = target;
			} else {
				m_jitIp++;
			}
			break;
		}

		case OpCode::CallInd: {
			auto &funcReg = instr.operands[0];
			uint8_t reg = static_cast<uint8_t>(funcReg.intVal);
			uint32_t nargs = static_cast<uint32_t>(instr.operands[1].intVal);
			uint32_t targetOffset = 0;
			if (m_rtag[reg] == TAG_HEAP) {
				uint32_t closureBase = static_cast<uint32_t>(m_regs[reg]);
				auto fnCell = m_heap.loadCell(closureBase, 0);
				targetOffset = static_cast<uint32_t>(fnCell.first);
				for (int i = static_cast<int>(nargs); i >= 1; --i) {
					uint8_t from = static_cast<uint8_t>(i);
					uint8_t to = static_cast<uint8_t>(i + 1);
					setReg(to, m_regs[from], m_rtag[from]);
				}
				setReg(1, m_regs[reg], m_rtag[reg]);
			} else {
				targetOffset = static_cast<uint32_t>(m_regs[reg]);
			}
			size_t target;
			if (!resolveJitTarget(targetOffset, target)) {
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

		case OpCode::Write: {
				auto &val = instr.operands[0];
				if (val.tag == OperandTag::Imm64) {
					std::cout << val.intVal;
				} else if (val.tag == OperandTag::StrIdx) {
					if (val.strIdx < m_chunk.strings.size()) {
						std::cout << m_chunk.strings[val.strIdx];
					}
				} else if (val.tag == OperandTag::Reg) {
					uint8_t reg = static_cast<uint8_t>(val.intVal);
					if (m_rtag[reg] == 0) {
						std::cout << m_regs[reg];
					} else if (m_rtag[reg] == 1) {
						uint32_t strIdx = static_cast<uint32_t>(m_regs[reg]);
						if (strIdx < m_runtimeStrings.size()) {
							std::cout << m_runtimeStrings[strIdx];
						} else if (strIdx < m_chunk.strings.size()) {
							std::cout << m_chunk.strings[strIdx];
						}
					}
				}
				m_jitIp++;
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
					if (strIdx < m_runtimeStrings.size()) {
						std::cout << m_runtimeStrings[strIdx] << "\n";
					} else if (strIdx < m_chunk.strings.size()) {
						std::cout << m_chunk.strings[strIdx] << "\n";
					}
				}
			}
			m_jitIp++;
			break;
		}

		case OpCode::StrLen: {
			auto &src = instr.operands[0];
			auto &dstOp = instr.operands[1];
			uint8_t dstReg = static_cast<uint8_t>(dstOp.intVal);
			int64_t len = 0;
			if (src.tag == OperandTag::StrIdx) {
				if (src.strIdx < m_chunk.strings.size())
					len = static_cast<int64_t>(m_chunk.strings[src.strIdx].size());
			} else if (src.tag == OperandTag::Reg) {
				uint8_t srcReg = static_cast<uint8_t>(src.intVal);
				if (m_rtag[srcReg] == 1) {
					uint32_t strIdx = static_cast<uint32_t>(m_regs[srcReg]);
					if (strIdx < m_runtimeStrings.size())
						len = static_cast<int64_t>(m_runtimeStrings[strIdx].size());
					else if (strIdx < m_chunk.strings.size())
						len = static_cast<int64_t>(m_chunk.strings[strIdx].size());
				}
			}
			setReg(dstReg, len, TAG_INT);
			m_jitIp++;
			break;
		}
		case OpCode::ToStr: {
			auto &src = instr.operands[0];
			auto &dstOp = instr.operands[1];
			uint8_t dstReg = static_cast<uint8_t>(dstOp.intVal);
			std::string str;
			if (src.tag == OperandTag::StrIdx) {
				if (src.strIdx < m_chunk.strings.size())
					str = m_chunk.strings[src.strIdx];
			} else if (src.tag == OperandTag::Reg) {
				uint8_t srcReg = static_cast<uint8_t>(src.intVal);
				if (m_rtag[srcReg] == 0) {
					str = std::to_string(m_regs[srcReg]);
				} else if (m_rtag[srcReg] == 1) {
					uint32_t strIdx = static_cast<uint32_t>(m_regs[srcReg]);
					if (strIdx < m_runtimeStrings.size())
						str = m_runtimeStrings[strIdx];
					else if (strIdx < m_chunk.strings.size())
						str = m_chunk.strings[strIdx];
				}
			} else if (src.tag == OperandTag::Imm64) {
				str = std::to_string(src.intVal);
			}
			setReg(dstReg, static_cast<int64_t>(internRuntimeString(str)), TAG_STR);
			m_jitIp++;
			break;
		}


		case OpCode::VCall: {
			// Virtual call: obj_reg, slot_imm, nargs_imm, return_addr
			uint8_t objReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint32_t slot = static_cast<uint32_t>(instr.operands[1].intVal);
			int64_t heapOffset = m_regs[objReg];
			uint32_t typeId = m_heap.getTypeId(static_cast<uint32_t>(heapOffset));
			size_t target = 0;
			if (typeId < m_chunk.vtables.size() && slot < m_chunk.vtables[typeId].methodOffsets.size()) {
				if (resolveJitTarget(m_chunk.vtables[typeId].methodOffsets[slot], target)) {
					m_callStack.push(m_jitIp + 1);
					m_jitIp = target;
					return;
				}
			}
			halted = true;
			return;
		}

		case OpCode::ICall: {
			// Interface call: obj_reg, iface_typeid_imm, method_idx_imm, nargs_imm, return_addr
			uint8_t objReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint32_t ifaceTypeId = static_cast<uint32_t>(instr.operands[1].intVal);
			uint32_t methodIdx = static_cast<uint32_t>(instr.operands[2].intVal);
			int64_t heapOffset = m_regs[objReg];
			uint32_t objTypeId = m_heap.getTypeId(static_cast<uint32_t>(heapOffset));
			if (objTypeId < m_chunk.vtables.size()) {
				const auto& vte = m_chunk.vtables[objTypeId];
				for (const auto& ite : vte.itable) {
					if (ite.interfaceTypeId == ifaceTypeId && methodIdx < ite.methodOffsets.size()) {
						size_t target;
						if (resolveJitTarget(ite.methodOffsets[methodIdx], target)) {
							m_callStack.push(m_jitIp + 1);
							m_jitIp = target;
							return;
						}
					}
				}
			}
			halted = true;
			return;
		}

		case OpCode::TypeOf: {
			// Load typeId from object header into result register
			uint8_t objReg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t resReg = static_cast<uint8_t>(instr.operands[1].intVal);
			int64_t heapOffset = m_regs[objReg];
			uint32_t typeId = m_heap.getTypeId(static_cast<uint32_t>(heapOffset));
			setReg(resReg, static_cast<int64_t>(typeId), TAG_INT);
			m_jitIp++;
			break;
		}

		case OpCode::Syscall: {
			uint8_t reg = static_cast<uint8_t>(instr.operands[0].intVal);
			uint8_t num = static_cast<uint8_t>(m_regs[reg]);
				invokeSyscall(num);
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
#ifndef NDEBUG
	// Debug-only invariant sweep: every register tagged TAG_HEAP must point at a
	// live slot.  This is what caught the acquire-before-release ordering bug.
	static const bool checkRoots = std::getenv("LOXIS_GC_CHECK") != nullptr;
	int reports = 0;
#endif

	while (m_jitIp < m_jitCode.size() && executeCount < maxExecuteCount && !halted) {
		executeCount++;
		const auto &instr = m_jitCode[m_jitIp];

#ifndef NDEBUG
		if (checkRoots && reports < 8) {
			for (int r = 0; r < 256; r++) {
				if (m_rtag[r] != TAG_HEAP) continue;
				if (m_heap.valid(static_cast<uint32_t>(m_regs[r]))) continue;
				std::cerr << "[gc-check] step " << executeCount << " before "
				          << traceOpcodeName(instr.op) << " @" << instr.byteOffset
				          << ": r" << r << " holds dead handle " << m_regs[r] << "\n";
				reports++;
				break;
			}
		}
#endif

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

	m_executed += static_cast<uint64_t>(executeCount);
	m_truncated = !halted && executeCount >= maxExecuteCount;
	return 0;
}

int VM::run() {
	constexpr int unlimited = 2147483647;
	return run(unlimited);
}
