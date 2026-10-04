#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "Chunk.hpp"
#include "../ir/OpCode.hpp"

// Bytecode decoding shared by the VM's JIT build and `--dump-bytecode`.
//
// Before this existed the opcode-name table and the operand-count switch lived
// in VM.cpp as file-local statics, so a disassembler would have had to carry a
// second copy that could silently drift from the one the VM actually uses.
// Everything now decodes through here.

// One decoded operand. Mirrors the VM's JitOperand exactly: WideReg has already
// been folded into Reg, so a consumer never has to know the 2-byte widening
// exists. Null keeps its own tag but carries intVal == 0.
struct DecodedOperand {
    OperandTag tag = OperandTag::Reg;
    int64_t intVal = 0;
    uint32_t strIdx = 0;
};

struct DecodedInstr {
    OpCode op = OpCode::Nop;
    uint32_t byteOffset = 0;
    std::vector<DecodedOperand> operands;
};

// Human-readable mnemonic. Single source of truth for the VM's trace output,
// its panic reports, and --dump-bytecode.
const char* opcodeName(OpCode op);

// Renders one operand the way the VM's trace does: r0, #42, str[3]("hi"),
// 0x0000001a.
std::string formatOperand(const DecodedOperand& operand, const Chunk& chunk);

// Linear decode of `chunk.code`. Returns false on an unknown opcode or a
// truncated operand; when `error` is non-null it receives a human-readable
// reason. `out` is cleared first and left partially filled on failure.
bool decodeBytecode(const Chunk& chunk, std::vector<DecodedInstr>& out,
                    std::string* error = nullptr);

// --dump-bytecode: one instruction per line, prefixed with its byte offset.
void dumpBytecode(const Chunk& chunk, std::ostream& os);
