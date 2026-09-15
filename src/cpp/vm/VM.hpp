#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include "../backend/Chunk.hpp"
#include "Heap.hpp"
#include "GC.hpp"

class VM {
public:
	// heapSlots fixes the handle index space (required by the AARC backend,
	// whose collector scratch is sized once); 0 means "pick a default".
	explicit VM(Chunk chunk, bool trace = false, GCMode gcMode = GCMode::NoGC,
	            uint32_t heapSlots = 0);
	~VM();

	void setArguments(std::vector<std::string> args) { m_arguments = std::move(args); }
	int run();

	int run(int maxExecuteCount);

	// Process teardown: drop every root through the normal barriers (what a real
	// exit does), then run a final drain point. Afterwards `live` counts objects
	// unreachable from anywhere — not objects still parked in a register.
	void finishGC();

	// True when the last run hit the instruction cap instead of halting: the
	// benchmark must not report a truncated run as a measurement.
	bool truncated() const { return m_truncated; }
	uint64_t instructionsExecuted() const { return m_executed; }

	// Barrier / heap counters for the benchmark harness.
	const GCMetrics& gcMetrics() const { return m_gc->metrics(); }
	const Heap::Stats& heapStats() const { return m_heap.stats(); }
	const char* gcName() const { return m_gc->name(); }
	void dumpGCStats(std::ostream& os) const { m_gc->dumpMetrics(os); }
	// Collector tuning passthrough; see GCBackend::setCyclePolicy.
	void setCyclePolicy(const std::string& spec) { m_gc->setCyclePolicy(spec); }

private:
	std::vector<std::string> m_arguments;
	std::vector<std::string> m_directoryEntries;
	std::string m_ioError;
	std::string m_netError;
	std::unordered_set<int64_t> m_openSockets;
	bool m_trace;
	Chunk m_chunk;
	int64_t m_regs[4096] = {};
	uint8_t m_rtag[4096] = {};   // TAG_INT / TAG_STR / TAG_HEAP / TAG_FLOAT / TAG_NULL
	int64_t m_globals[256] = {};
	uint8_t m_globalTags[256] = {};
	uint8_t m_flags = 0;        // FLAG_ZERO=0x01, FLAG_NEG=0x02
	struct SavedRegister { uint16_t reg; int64_t value; uint8_t tag; };
	struct CallFrame { size_t returnIp; size_t callIp; std::vector<SavedRegister> locals; };
	std::stack<CallFrame> m_callStack;
	std::stack<SavedRegister> m_dataStack;
	struct ExceptionHandler {
		size_t targetIp = 0;
		uint16_t exceptionReg = 0;
		size_t callDepth = 0;
		size_t dataDepth = 0;
	};
	std::vector<ExceptionHandler> m_exceptionHandlers;
	Heap m_heap;
	std::unique_ptr<GCBackend> m_gcOwned;
	GCBackend* m_gc = nullptr;
	std::vector<std::string> m_runtimeStrings;       // Runtime-interned strings

	static constexpr uint8_t
	FLAG_ZERO = 0x01;
	static constexpr uint8_t
	FLAG_NEG = 0x02;

	// Every register/global overwrite goes through these, so the GC sees the
	// exact point a reference is dropped or taken. Self-assignment is skipped:
	// remove-then-add would momentarily drop the count to zero and reclaim a
	// still-live object.
	void setReg(uint16_t reg, int64_t value, uint8_t tag);
	void setGlobal(uint8_t slot, int64_t value, uint8_t tag);

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
	bool m_truncated = false;
	uint64_t m_executed = 0;

	bool tryBuildJit();

	bool decodeOperandAt(uint32_t &ip, JitOperand &out) const;

	bool resolveJitTarget(uint32_t byteOffset, size_t &target) const;

	int runJit(int maxExecuteCount);

	void execJitInstruction(const JitInstruction &instr, bool &halted);

	int64_t readIntOperand(const JitOperand &operand) const;

	std::string readStringOperand(const JitOperand &operand) const;
	uint32_t internRuntimeString(const std::string& s);
	void throwValue(int64_t value, uint8_t tag);
	void raiseRuntime(const std::string& message);
	void pushCallFrame(size_t returnIp, size_t callIp);
	void restoreCallFrame();
	[[noreturn]] void panic(const std::string& message) const;
	// Unified syscall entry — all native VM functions go through here
	// Args passed in registers: r1..r4, result in r0
	void invokeSyscall(uint8_t op);
};
