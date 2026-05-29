#include "MIRCompiler.hpp"
#include <stdexcept>
#include <cstring>

using namespace loxis::v2;

Chunk MIRCompiler::compile(const std::vector<MirBody>& bodies) {
    m_chunk = Chunk();
    m_patches.clear();
    m_labelOffsets.clear();

    // Synthetic entry point: call main, then halt
    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Call));
    m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Addr));
    size_t entryPatch = m_chunk.offset();
    m_chunk.emitU32(0);
    m_patches.push_back({entryPatch, "main"});
    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Halt));

    for (const auto& body : bodies) {
        compileBody(body);
    }

    for (const auto& patch : m_patches) {
        m_chunk.patchU32(patch.offset, getLabelOffset(patch.label));
    }

    return m_chunk;
}

void MIRCompiler::compileBody(const MirBody& body) {
    std::string prefix = body.name;
    uint32_t startOffset = static_cast<uint32_t>(m_chunk.offset());
    m_labelOffsets[body.name] = startOffset;
    for (size_t i = 0; i < body.blocks.size(); ++i) {
        std::string label = blockLabel(prefix, static_cast<BlockId>(i));
        uint32_t offset = static_cast<uint32_t>(m_chunk.offset());
        m_labelOffsets[label] = offset;

        const auto& bb = body.blocks[i];
        for (const auto& stmt : bb.stmts) {
            emitStatement(stmt);
        }
        emitTerminator(bb.term, prefix);
    }
}

void MIRCompiler::emitStatement(const Statement& stmt) {
    std::visit([&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, StAssign>) {
            emitRvalue(s.place, s.rv);
        } else if constexpr (std::is_same_v<T, StStorageLive>) {
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Nop));
        } else if constexpr (std::is_same_v<T, StStorageDead>) {
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Nop));
        } else if constexpr (std::is_same_v<T, StSetDiscriminant>) {
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Nop));
        } else if constexpr (std::is_same_v<T, StPush>) {
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::PushR));
            emitReg(localToReg(s.local));
        } else if constexpr (std::is_same_v<T, StPop>) {
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::PopR));
            emitReg(localToReg(s.local));
        } else if constexpr (std::is_same_v<T, StDrop>) {
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Nop));
        }
    }, stmt);
}

void MIRCompiler::emitTerminator(const Terminator& term, const std::string& prefix) {
    std::visit([&](const auto& t) {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, TmGoto>) {
            std::string label = blockLabel(prefix, t.target);
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jmp));
            emitAddr(label);
            emitAddr(label);
        } else if constexpr (std::is_same_v<T, TmSwitchInt>) {
            uint8_t discrReg = 252;
            emitOperandToReg(t.discr, discrReg);
            for (const auto& p : t.values) {
                int64_t val = p.first;
                std::string targetLabel = blockLabel(prefix, p.second);
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                emitReg(251);
                emitImm(val);
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Cmp));
                emitReg(discrReg);
                emitReg(251);
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jz));
                emitAddr(targetLabel);
            }
            std::string otherwiseLabel = blockLabel(prefix, t.otherwise);
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jmp));
            emitAddr(otherwiseLabel);
            emitAddr(otherwiseLabel);
        } else if constexpr (std::is_same_v<T, TmSwitchDiscr>) {
            std::string otherwiseLabel = blockLabel(prefix, t.otherwise);
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jmp));
            emitAddr(otherwiseLabel);
            emitAddr(otherwiseLabel);
        } else if constexpr (std::is_same_v<T, TmReturn>) {
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Ret));
        } else if constexpr (std::is_same_v<T, TmCall>) {
            std::string funcLabel;
            std::visit([&](const auto& f) {
                using FT = std::decay_t<decltype(f)>;
                if constexpr (std::is_same_v<FT, OpConst>) {
                    std::visit([&](const auto& c) {
                        using CT = std::decay_t<decltype(c)>;
                        if constexpr (std::is_same_v<CT, std::string>) {
                            funcLabel = c;
                        }
                    }, f.val);
                }
            }, t.func);
            if (funcLabel.empty()) funcLabel = "unknown";
            if (funcLabel == "print" || funcLabel == "println") {
                // Built-in print: use temp reg, then explicitly jump to resume
                if (!t.args.empty()) emitOperandToReg(t.args[0], 254);
                if (!t.args.empty()) emitOperandToReg(t.args[0], 254);
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Print));
                emitReg(254);
                // Explicitly jump to resume block (Jmp needs 2 Addr operands like TmGoto)
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jmp));
                emitAddr(blockLabel(prefix, t.return_));
                emitAddr(blockLabel(prefix, t.return_));
            } else {
                // Regular call: move args to param regs
                for (size_t i = 0; i < t.args.size(); ++i) {
                    uint8_t paramReg = static_cast<uint8_t>(i + 1);
                    emitOperandToReg(t.args[i], paramReg);
                }
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Call));
                emitAddr(funcLabel);
                // Explicitly jump to resume block (fixes block ordering)
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jmp));
                emitAddr(blockLabel(prefix, t.return_));
                emitAddr(blockLabel(prefix, t.return_));
            }
        } else if constexpr (std::is_same_v<T, TmCallIndirect>) {
            // Move arguments into param regs, then call via function pointer
            for (size_t i = 0; i < t.args.size(); ++i) {
                uint8_t paramReg = static_cast<uint8_t>(i + 1);
                emitOperandToReg(t.args[i], paramReg);
            }
            // Load function pointer into temp reg, then call
            emitOperandToReg(t.funcPtr, 250);
            // Emit as regular Call with label "indirect" for now
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Call));
            m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Addr));
            size_t patchOff = m_chunk.offset();
            m_chunk.emitU32(0);
            m_patches.push_back({patchOff, "_indirect_stub"});
        } else if constexpr (std::is_same_v<T, TmCheckNull>) {
            // Cmp value, 0; Jz nullTarget; fallthrough = okTarget
            uint8_t valReg = 253;
            emitOperandToReg(t.value, valReg);
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
            uint8_t zr = 251;
            emitReg(zr);
            emitImm(0);
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Cmp));
            emitReg(valReg);
            emitReg(zr);
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jz));
            emitAddr(blockLabel(prefix, t.nullTarget));
            // okTarget: fallthrough — set label here so subsequent code lands here
            m_labelOffsets[blockLabel(prefix, t.okTarget)] = static_cast<uint32_t>(m_chunk.offset());
        } else if constexpr (std::is_same_v<T, TmUnreachable>) {
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Halt));
        }
    }, term);
}

void MIRCompiler::emitRvalue(LocalId place, const Rvalue& rv) {
    uint8_t placeReg = localToReg(place);
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, RvUse>) {
            emitOperandToReg(v.op, placeReg);
        } else if constexpr (std::is_same_v<T, RvBinary>) {
            uint8_t lReg = 254;
            uint8_t rReg = 253;
            emitOperandToReg(v.l, lReg);
            emitOperandToReg(v.r, rReg);
            OpCode opc = OpCode::Nop;
            bool addLike = false;
            switch (v.op) {
                case ExprBinary::Add: opc = OpCode::Add; addLike = true; break;
                case ExprBinary::Sub: opc = OpCode::Sub; break;
                case ExprBinary::Mul: opc = OpCode::Mul; break;
                case ExprBinary::Div: opc = OpCode::Div; break;
                case ExprBinary::Rem: opc = OpCode::Rem; break;
                case ExprBinary::Shl: opc = OpCode::Shl; break;
                case ExprBinary::Shr: opc = OpCode::Shr; break;
                case ExprBinary::BitAnd: opc = OpCode::BitAnd; break;
                case ExprBinary::BitOr: opc = OpCode::BitOr; break;
                case ExprBinary::BitXor: opc = OpCode::BitXor; break;
                case ExprBinary::And:
                case ExprBinary::Or:
                    opc = OpCode::Nop; break;
                case ExprBinary::Eq:
                case ExprBinary::Ne:
                case ExprBinary::Lt:
                case ExprBinary::Le:
                case ExprBinary::Gt:
                case ExprBinary::Ge:
                    opc = OpCode::Cmp; break;
                default: opc = OpCode::Nop; break;
            }
            if (opc == OpCode::Nop) {
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Nop));
                return;
            }
            if (opc == OpCode::Cmp) {
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Cmp));
                emitReg(lReg);
                emitReg(rReg);

                static int cmpLabelId = 0;
                int id = cmpLabelId++;

                // Emit: Push dest, 1; if (true) skip; Push dest, 0; end:
                // After Cmp l,r: Z=1 if l==r, N=1 if l<r (signed)

                auto emitSetTrue  = [&]() {
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                    emitReg(placeReg);
                    emitImm(1);
                };
                auto emitSetFalse = [&]() {
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                    emitReg(placeReg);
                    emitImm(0);
                };
                auto emitJmpEnd = [&](int n) {
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jmp));
                    emitAddr("_ce" + std::to_string(n));
                    emitAddr("_ce" + std::to_string(n));
                };
                auto emitEndLabel = [&](int n) {
                    m_labelOffsets["_ce" + std::to_string(n)] =
                        static_cast<uint32_t>(m_chunk.offset());
                };

                switch (v.op) {
                case ExprBinary::Eq:
                    // True if Z=1: set 1, Jz skip, set 0, end
                    emitSetTrue();
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jz));
                    emitAddr("_ce" + std::to_string(id));
                    emitSetFalse();
                    emitEndLabel(id);
                    break;
                case ExprBinary::Ne:
                    // True if Z=0
                    emitSetTrue();
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jnz));
                    emitAddr("_ce" + std::to_string(id));
                    emitSetFalse();
                    emitEndLabel(id);
                    break;
                case ExprBinary::Lt:
                    // True if N=1
                    emitSetTrue();
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jn));
                    emitAddr("_ce" + std::to_string(id));
                    emitSetFalse();
                    emitEndLabel(id);
                    break;
                case ExprBinary::Le:
                    // True if Z=1 or N=1
                    emitSetTrue();
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jz));
                    emitAddr("_ce" + std::to_string(id));
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jn));
                    emitAddr("_ce" + std::to_string(id));
                    emitSetFalse();
                    emitEndLabel(id);
                    break;
                case ExprBinary::Gt:
                    // True if Z=0 and N=0. False if Z=1 or N=1.
                    // Set 1; if false (Z|N) goto false; Jmp end; false: set 0; end:
                    emitSetTrue();
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jz));
                    emitAddr("_cf" + std::to_string(id));
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jn));
                    emitAddr("_cf" + std::to_string(id));
                    emitJmpEnd(id);
                    m_labelOffsets["_cf" + std::to_string(id)] =
                        static_cast<uint32_t>(m_chunk.offset());
                    emitSetFalse();
                    emitEndLabel(id);
                    break;
                case ExprBinary::Ge:
                    // True if N=0. False if N=1.
                    emitSetTrue();
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jn));
                    emitAddr("_cf" + std::to_string(id));
                    emitJmpEnd(id);
                    m_labelOffsets["_cf" + std::to_string(id)] =
                        static_cast<uint32_t>(m_chunk.offset());
                    emitSetFalse();
                    emitEndLabel(id);
                    break;
                default:
                    emitSetFalse();
                    break;
                }
                return;
            }
            m_chunk.emitByte(static_cast<uint8_t>(opc));
            if (addLike) {
                emitReg(placeReg);
                emitReg(lReg);
                emitReg(rReg);
            } else {
                emitReg(lReg);
                emitReg(rReg);
                emitReg(placeReg);
            }
        } else if constexpr (std::is_same_v<T, RvUnary>) {
            if (v.op == ExprUnary::Neg) {
                uint8_t opndReg = 254;
                emitOperandToReg(v.opnd, opndReg);
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                emitReg(placeReg);
                emitImm(0);
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Sub));
                emitReg(placeReg);
                emitReg(opndReg);
                emitReg(placeReg);
            } else {
                if (v.op == ExprUnary::Not) {
                    uint8_t opndReg = 254;
                    uint8_t zeroReg = 253;
                    static int notLabelId = 0;
                    int id = notLabelId++;
                    std::string trueLabel = "_not_true" + std::to_string(id);
                    std::string endLabel = "_not_end" + std::to_string(id);

                    emitOperandToReg(v.opnd, opndReg);
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                    emitReg(zeroReg);
                    emitImm(0);
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Cmp));
                    emitReg(opndReg);
                    emitReg(zeroReg);
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jz));
                    emitAddr(trueLabel);
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                    emitReg(placeReg);
                    emitImm(0);
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Jmp));
                    emitAddr(endLabel);
                    emitAddr(endLabel);
                    m_labelOffsets[trueLabel] = static_cast<uint32_t>(m_chunk.offset());
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                    emitReg(placeReg);
                    emitImm(1);
                    m_labelOffsets[endLabel] = static_cast<uint32_t>(m_chunk.offset());
                } else {
                    emitOperandToReg(v.opnd, placeReg);
                }
            }
        } else if constexpr (std::is_same_v<T, RvCast>) {
            uint8_t srcReg = 254;
            emitOperandToReg(v.op, srcReg);
            if (v.ty && (v.ty->kind() == TyKind::Float || v.ty->kind() == TyKind::Double)) {
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::ItoF));
                emitReg(srcReg);
                emitReg(placeReg);
            } else if (v.ty && (v.ty->kind() == TyKind::Int || v.ty->kind() == TyKind::Long)) {
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::FtoI));
                emitReg(srcReg);
                emitReg(placeReg);
            } else {
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Mov));
                emitReg(placeReg);
                emitReg(srcReg);
            }
        } else if constexpr (std::is_same_v<T, RvRef> || std::is_same_v<T, RvPtr>) {
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Mov));
            emitReg(placeReg);
            emitReg(localToReg(v.local));
        } else {
            // Aggregate, Field, Index, Len, Discriminant
            m_chunk.emitByte(static_cast<uint8_t>(OpCode::Nop));
        }
    }, rv);
}

void MIRCompiler::emitOperandToReg(const Operand& op, uint8_t reg) {
    std::visit([&](const auto& o) {
        using T = std::decay_t<decltype(o)>;
        if constexpr (std::is_same_v<T, OpConst>) {
            std::visit([&](const auto& c) {
                using CT = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<CT, int64_t>) {
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                    emitReg(reg);
                    emitImm(c);
                } else if constexpr (std::is_same_v<CT, uint64_t>) {
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                    emitReg(reg);
                    emitImm(static_cast<int64_t>(c));
                } else if constexpr (std::is_same_v<CT, double>) {
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                    emitReg(reg);
                    int64_t bits = 0;
                    static_assert(sizeof(bits) == sizeof(c));
                    std::memcpy(&bits, &c, sizeof(bits));
                    emitImm(bits);
                } else if constexpr (std::is_same_v<CT, bool>) {
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                    emitReg(reg);
                    emitImm(c ? 1 : 0);
                } else if constexpr (std::is_same_v<CT, char>) {
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                    emitReg(reg);
                    emitImm(static_cast<int64_t>(c));
                } else if constexpr (std::is_same_v<CT, std::string>) {
                    uint32_t strIdx = m_chunk.internString(c);
                    m_chunk.emitByte(static_cast<uint8_t>(OpCode::Push));
                    emitReg(reg);
                    m_chunk.emitByte(static_cast<uint8_t>(OperandTag::StrIdx));
                    m_chunk.emitU32(strIdx);
                }
            }, o.val);
        } else if constexpr (std::is_same_v<T, OpCopy> || std::is_same_v<T, OpMove>) {
            uint8_t src = localToReg(o.local);
            if (src != reg) {
                m_chunk.emitByte(static_cast<uint8_t>(OpCode::Mov));
                emitReg(reg);
                emitReg(src);
            }
        }
    }, op);
}

void MIRCompiler::emitReg(uint8_t reg) {
    m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Reg));
    m_chunk.emitByte(reg);
}

void MIRCompiler::emitImm(int64_t val) {
    m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Imm64));
    m_chunk.emitI64(val);
}

void MIRCompiler::emitAddr(const std::string& label) {
    m_chunk.emitByte(static_cast<uint8_t>(OperandTag::Addr));
    size_t patchOffset = m_chunk.offset();
    m_chunk.emitU32(0);
    m_patches.push_back({patchOffset, label});
}

uint32_t MIRCompiler::getLabelOffset(const std::string& name) const {
    if (name == "print") {
        return 0xFFFFFFFF;
    }
    auto it = m_labelOffsets.find(name);
    if (it == m_labelOffsets.end()) {
        throw std::runtime_error("undefined label: " + name);
    }
    return it->second;
}
