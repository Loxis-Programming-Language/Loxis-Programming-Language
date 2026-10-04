#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include "../../backend/Chunk.hpp"
#include "../mir/MIR.hpp"
#include "Lowering.hpp"

namespace loxis::v2 {

class MIRCompiler {
public:
    Chunk compile(const LoweringOutput& output);

private:
    Chunk m_chunk;
    std::vector<PatchSite> m_patches;
    std::unordered_map<std::string, uint32_t> m_labelOffsets;
    std::vector<LoweredClass> m_classes;
    const MirBody* m_currentBody = nullptr;

    void compileBody(const MirBody& body);
    void buildVTableMetadata();
    void emitStatement(const Statement& stmt, const std::string& prefix);
    void emitTerminator(const Terminator& term, const std::string& prefix);
    void emitRvalue(LocalId place, const Rvalue& rv);
    void emitOperandToReg(const Operand& op, uint16_t reg);
    void emitArguments(const std::vector<Operand>& args, size_t start = 0);

    void emitReg(uint16_t reg);
    void emitImm(int64_t val);
    void emitAddr(const std::string& label);

    uint16_t localToReg(LocalId lid) const {
        // r0 is the return register, r1..r10 are incoming arguments, and
        // r240..r255 are instruction staging. Function locals live elsewhere.
        if (lid == 0) return 0;
        if (lid >= 4064) throw std::runtime_error("too many VM locals in " + m_currentBody->name);
        uint16_t reg = static_cast<uint16_t>(lid + 15);
        return reg < 240 ? reg : static_cast<uint16_t>(reg + 16);
    }
    uint32_t getLabelOffset(const std::string& name) const;
    void recordSource(const SourceLocation& loc);
    std::string blockLabel(const std::string& prefix, BlockId id) const {
        return prefix + "_b" + std::to_string(id);
    }
};

} // namespace loxis::v2
