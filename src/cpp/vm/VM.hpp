#pragma once

#include <cstdint>
#include <vector>
#include <stack>
#include <unordered_map>
#include "../backend/Chunk.hpp"
#include "Heap.hpp"

class VM {
public:
	explicit VM(Chunk chunk, bool trace = false);

	int run();

	int run(int maxExecuteCount);

private:
	bool m_trace;
	Chunk m_chunk;
	int64_t m_regs[256] = {};
	uint8_t m_rtag[256] = {};   // 0=int, 1=string-index, 2=heap-ref
	uint8_t m_flags = 0;        // FLAG_ZERO=0x01, FLAG_NEG=0x02
	std::stack <size_t> m_callStack;
	std::stack <std::pair<int64_t, uint8_t>> m_dataStack;  // value + rtag
	Heap m_heap;

	static constexpr uint8_t
	FLAG_ZERO = 0x01;
	static constexpr uint8_t
	FLAG_NEG = 0x02;
	static constexpr uint8_t
	TAG_HEAP = 2;  // register holds heap offset
	static constexpr uint8_t
	TAG_NULL = 4;   // register holds null reference
	static constexpr uint8_t
	TAG_FLOAT = 3;  // register holds float64 bits

public:
	struct JitOperand {
		OperandTag tag = OperandTag::Reg;
		int64_t intVal = 0;
		uint32_t strIdx = 0;
	};

	struct JitInstruction {
		OpCode op = OpCode::Nop;
		uint32_t byteOffset = 0;
		uint8_t dsop = 0;
		uint8_t argCount = 0;
		std::vector <JitOperand> operands;
	};

private:

	std::vector <JitInstruction> m_jitCode;
	std::unordered_map <uint32_t, size_t> m_jitIpMap;
	bool m_jitReady = false;
	size_t m_jitIp = 0;

	bool tryBuildJit();

	bool decodeOperandAt(uint32_t &ip, JitOperand &out) const;

	bool resolveJitTarget(uint32_t byteOffset, size_t &target) const;

	int runJit(int maxExecuteCount);

	void execJitInstruction(const JitInstruction &instr, bool &halted);

	int64_t readIntOperand(const JitOperand &operand) const;

	std::string readStringOperand(const JitOperand &operand) const;
};
