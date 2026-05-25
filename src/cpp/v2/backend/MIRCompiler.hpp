#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include "../../backend/Chunk.hpp"
#include "../../backend/Compiler.hpp"
#include "../mir/MIR.hpp"

namespace loxis::v2 {

class MIRCompiler {
public:
    Chunk compile(const std::vector<MirBody>& bodies);

private:
    Chunk m_chunk;
    std::vector<PatchSite> m_patches;
    std::unordered_map<std::string, uint32_t> m_labelOffsets;

    void compileBody(const MirBody& body);
    void emitStatement(const Statement& stmt);
    void emitTerminator(const Terminator& term, const std::string& prefix);
    void emitRvalue(LocalId place, const Rvalue& rv);
    void emitOperandToReg(const Operand& op, uint8_t reg);

    void emitReg(uint8_t reg);
    void emitImm(int64_t val);
    void emitAddr(const std::string& label);

    uint8_t localToReg(LocalId lid) const { return static_cast<uint8_t>(lid); }
    uint32_t getLabelOffset(const std::string& name) const;
    std::string blockLabel(const std::string& prefix, BlockId id) const {
        return prefix + "_b" + std::to_string(id);
    }
};

} // namespace loxis::v2
