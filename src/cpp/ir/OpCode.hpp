#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>
#include <optional>
#include "../Error.hpp"

enum class OpCode : uint8_t {
	// Data movement
	Nop = 0x00,
	Push = 0x01,
	Pop = 0x02,
	Mov = 0x03,
	PushR = 0x04,  // push register to data stack
	PopR = 0x05,   // pop data stack to register
	Alloc = 0x06,  // heap allocate: size_reg, result_reg
	HFree = 0x07,  // heap free: base_reg
	HStore = 0x08, // heap store: base_reg, offset_reg, value_reg
	HLoad = 0x09,  // heap load: base_reg, offset_reg, result_reg
	Shl = 0x0A,    // shift left: src, shift, dst
	Shr = 0x0B,    // shift right (arithmetic): src, shift, dst
	Rem = 0x0C,    // remainder: src, val, dst
	BitAnd = 0x0D, // bitwise and: src, val, dst
	BitOr = 0x0E,  // bitwise or: src, val, dst
	BitXor = 0x0F, // bitwise xor: src, val, dst

	// Arithmetic
	Add = 0x10,
	Sub = 0x11,
	Mul = 0x12,
	Div = 0x13,

	// Floating-point
	FAdd = 0x14,  // f64 add: dst, src, val
	FSub = 0x15,  // f64 sub: src, val, dst
	FMul = 0x16,  // f64 mul: src, val, dst
	FDiv = 0x17,  // f64 div: src, val, dst
	ItoF = 0x18,  // int→f64: src_reg, dst_reg
	FtoI = 0x19,  // f64→int: src_reg, dst_reg
	FNeg = 0x1A,  // f64 neg: src, dst

	// Comparison
	Cmp = 0x20,

	// Control flow
	Jmp = 0x30,
	Jnz = 0x31,
	Jz = 0x32,
	Skip = 0x33,
	Call = 0x34,
	Ret = 0x35,
	Br = 0x36,   // IR-only explicit branch (not emitted to bytecode)
	Jn = 0x37,   // jump if NEG flag set
	NullChk = 0x38, // null check: val_reg, null_target_addr
	CallInd = 0x39, // indirect call: func_ptr_reg

	// I/O
	Print = 0x40,

	// Termination
	Halt = 0xFF
};

// Branch condition for IR
enum class BranchCond : uint8_t {
	Always = 0,
	Zero = 1,
	NonZero = 2,
};

struct CondRef {
	BranchCond cond;
};

// IR value types
struct RegId {
	uint8_t id;
};

struct LabelRef {
	std::string name;
};

using Value = std::variant<int64_t, RegId, LabelRef, std::string, CondRef>;

struct IRInstruction {
	OpCode op;
	std::vector <Value> operands;
	SourceLocation loc;
};

struct BasicBlock {
	std::string label;
	std::vector <IRInstruction> instructions;
	std::optional <IRInstruction> terminator;
	std::vector <std::string> successors;
};

struct IRProgram {
	std::vector <BasicBlock> blocks;
	std::vector <std::vector<uint8_t>> typePool;
};
