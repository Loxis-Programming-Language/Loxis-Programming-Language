#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include "../ir/OpCode.hpp"
#include "Chunk.hpp"

struct PatchSite {
	size_t offset;      // Bytecode offset where address needs to be patched
	std::string label;    // Label name to resolve
};

class Compiler {
public:
	Chunk compile(const IRProgram &program);

private:
	Chunk m_chunk;
	std::vector <PatchSite> m_patches;
	std::unordered_map <std::string, uint32_t> m_labelOffsets;

	void emitInstruction(const IRInstruction &instr);

	void emitTerminator(const IRInstruction &instr);

	void emitValue(const Value &val);

	uint32_t getLabelOffset(const std::string &name) const;
};
