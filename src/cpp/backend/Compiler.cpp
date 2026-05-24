#include "Compiler.hpp"
#include <stdexcept>

Chunk Compiler::compile(const IRProgram &program) {
	m_chunk = Chunk();
	m_patches.clear();
	m_labelOffsets.clear();

	// Transfer type pool (generic types, no erasure)
	for (size_t i = 0; i < program.typePool.size(); i++) {
		m_chunk.typePool.push_back(program.typePool[i]);
		std::string key(reinterpret_cast<const char *>(program.typePool[i].data()),
		                program.typePool[i].size());
		m_chunk.typeIndices[key] = static_cast<uint32_t>(i);
	}

	// Pass 1: record block byte offsets, emit body + terminator
	for (const auto &block: program.blocks) {
		m_labelOffsets[block.label] = static_cast<uint32_t>(m_chunk.offset());
		for (const auto &instr: block.instructions)
			emitInstruction(instr);
		if (block.terminator)
			emitTerminator(*block.terminator);
	}

	// Pass 2: back-patch label references
	for (const auto &patch: m_patches) {
		m_chunk.patchU32(patch.offset, getLabelOffset(patch.label));
	}

	return m_chunk;
}

void Compiler::emitInstruction(const IRInstruction &instr) {
	uint8_t opcode = static_cast<uint8_t>(instr.op);
	m_chunk.emitByte(opcode);

	for (const auto &val: instr.operands) {
		emitValue(val);
	}
}

void Compiler::emitTerminator(const IRInstruction &instr) {
	// Handle Br (branch) opcodes specially, converting to Jmp for bytecode
	if (instr.op == OpCode::Br) {
		// Br always has CondRef as first operand
		const auto &condRef = std::get<CondRef>(instr.operands[0]);

		if (condRef.cond == BranchCond::Always) {
			// Br Always, t → Jmp opcode + emitLabelRef(t) + emitLabelRef(t)
			const auto &targetLabel = std::get<LabelRef>(instr.operands[1]).name;
			m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jmp));
			// Emit label reference for both branches (same target for unconditional)
			m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Addr));
			size_t patchOffset = m_chunk.offset();
			m_chunk.emitU32(0);
			m_patches.push_back({patchOffset, targetLabel});
			m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Addr));
			patchOffset = m_chunk.offset();
			m_chunk.emitU32(0);
			m_patches.push_back({patchOffset, targetLabel});
		} else if (condRef.cond == BranchCond::Zero) {
			// Br Zero, t, f → Jmp opcode + emitLabelRef(t) + emitLabelRef(f)
			const auto &trueLabel = std::get<LabelRef>(instr.operands[1]).name;
			const auto &falseLabel = std::get<LabelRef>(instr.operands[2]).name;
			m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jmp));
			m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Addr));
			size_t patchOffset = m_chunk.offset();
			m_chunk.emitU32(0);
			m_patches.push_back({patchOffset, trueLabel});
			m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Addr));
			patchOffset = m_chunk.offset();
			m_chunk.emitU32(0);
			m_patches.push_back({patchOffset, falseLabel});
		} else if (condRef.cond == BranchCond::NonZero) {
			// Br NonZero, t, f → Jmp opcode + emitLabelRef(f) + emitLabelRef(t) [swapped!]
			const auto &trueLabel = std::get<LabelRef>(instr.operands[1]).name;
			const auto &falseLabel = std::get<LabelRef>(instr.operands[2]).name;
			m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jmp));
			// Emit swapped: false branch first, true branch second
			m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Addr));
			size_t patchOffset = m_chunk.offset();
			m_chunk.emitU32(0);
			m_patches.push_back({patchOffset, falseLabel});
			m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Addr));
			patchOffset = m_chunk.offset();
			m_chunk.emitU32(0);
			m_patches.push_back({patchOffset, trueLabel});
		}
		return;
	}

	// For non-Br terminators (Call, Ret, Halt), emit normally
	emitInstruction(instr);
}

void Compiler::emitValue(const Value &val) {
	if (std::holds_alternative<CondRef>(val)) {
		throw std::logic_error("CondRef must not be emitted directly");
	}

	if (std::holds_alternative<int64_t>(val)) {
		int64_t imm = std::get<int64_t>(val);
		m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Imm64));
		m_chunk.emitI64(imm);
		return;
	}

	if (std::holds_alternative<RegId>(val)) {
		uint8_t regId = std::get<RegId>(val).id;
		m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Reg));
		m_chunk.emitByte(regId);
		return;
	}

	if (std::holds_alternative<std::string>(val)) {
		uint32_t strIdx = m_chunk.internString(std::get<std::string>(val));
		m_chunk.emitByte(static_cast<uint8_t>(OperandTag::StrIdx));
		m_chunk.emitU32(strIdx);
		return;
	}

	if (std::holds_alternative<LabelRef>(val)) {
		const auto &labelRef = std::get<LabelRef>(val);
		m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Addr));
		size_t patchOffset = m_chunk.offset();
		m_chunk.emitU32(0);  // Placeholder; will be patched later
		m_patches.push_back({patchOffset, labelRef.name});
		return;
	}
}

uint32_t Compiler::getLabelOffset(const std::string &name) const {
	// Check if it's a builtin
	if (name == "print") {
		return 0xFFFFFFFF;  // Special sentinel for print builtin
	}

	auto it = m_labelOffsets.find(name);
	if (it == m_labelOffsets.end()) {
		throw std::runtime_error("undefined label: " + name);
	}
	return it->second;
}
