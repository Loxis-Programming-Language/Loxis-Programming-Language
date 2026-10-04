#include "Disasm.hpp"

#include <cstring>
#include <iomanip>
#include <ostream>
#include <sstream>

const char* opcodeName(OpCode op) {
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
        case OpCode::TryPush: return "trypush";
        case OpCode::TryPop: return "trypop";
        case OpCode::Throw: return "throw";
        case OpCode::IsA: return "isa";
        case OpCode::Syscall: return "syscall";
        case OpCode::Write:  return "write";
        case OpCode::Print:  return "print";
        case OpCode::StrLen: return "strlen";
        case OpCode::ToStr:  return "tostr";
        case OpCode::Halt:   return "halt";
    }
    return "?";
}

namespace {

// Operand count for `op`, with `ip` pointing just past the opcode byte so the
// zero-or-one-operand `skip` form can be decided by peeking. Returns false for
// an opcode this build does not know.
bool operandCountFor(OpCode op, const std::vector<uint8_t>& code, uint32_t ip, int& out) {
    switch (op) {
        case OpCode::Nop:
        case OpCode::Ret:
        case OpCode::TryPop:
        case OpCode::Halt:
            out = 0;
            return true;
        case OpCode::Jnz:
        case OpCode::Jz:
        case OpCode::Jn:
        case OpCode::Call:
        case OpCode::Write:
        case OpCode::Print:
        case OpCode::PushR:
        case OpCode::PopR:
        case OpCode::Throw:
        case OpCode::HFree:
        case OpCode::Syscall:   // syscall number in reg
            out = 1;
            return true;
        case OpCode::Push:
        case OpCode::Mov:
        case OpCode::Cmp:
        case OpCode::Jmp:
        case OpCode::GLoad:
        case OpCode::GStore:
        case OpCode::ItoF:
        case OpCode::FtoI:
        case OpCode::FNeg:
        case OpCode::NullChk:
        case OpCode::TypeOf:
        case OpCode::StrLen:
        case OpCode::ToStr:
        case OpCode::TryPush:
        case OpCode::CallInd:
            out = 2;
            return true;
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
        case OpCode::IsA:
            out = 3;
            return true;
        case OpCode::VCall:
            out = 4;   // obj_reg, slot_imm, nargs_imm, return_addr
            return true;
        case OpCode::ICall:
            out = 5;   // obj_reg, iface_typeid_imm, method_idx_imm, nargs_imm, return_addr
            return true;
        case OpCode::Skip:
            // 0 or 1 operands: an operand is present only when the next byte is
            // an Addr tag.
            out = (ip < code.size() && code[ip] == static_cast<uint8_t>(OperandTag::Addr)) ? 1 : 0;
            return true;
        default:
            return false;
    }
}

// Decodes one tag-prefixed operand at `ip`, advancing it. Mirrors the encoding
// written by Chunk's emit helpers and the compiler's emitReg/emitImm/emitAddr.
bool decodeOperandAt(const std::vector<uint8_t>& code, uint32_t& ip, DecodedOperand& out) {
    if (ip >= code.size()) return false;

    const uint8_t tag = code[ip++];
    out.tag = static_cast<OperandTag>(tag);

    switch (out.tag) {
        case OperandTag::WideReg:
            if (ip + 1 >= code.size()) return false;
            out.intVal = code[ip] | (static_cast<uint16_t>(code[ip + 1]) << 8);
            ip += 2;
            if (out.intVal >= 4096) return false;
            out.tag = OperandTag::Reg;   // fold away the widening
            return true;
        case OperandTag::Reg:
            if (ip >= code.size()) return false;
            out.intVal = code[ip++];
            return true;
        case OperandTag::Null:
            out.intVal = 0;
            return true;
        case OperandTag::Float64:
        case OperandTag::Imm64: {
            if (ip + 7 >= code.size()) return false;
            uint64_t value = 0;
            for (int byte = 0; byte < 8; ++byte)
                value |= static_cast<uint64_t>(code[ip++]) << (8 * byte);
            out.intVal = static_cast<int64_t>(value);
            return true;
        }
        case OperandTag::StrIdx:
        case OperandTag::Addr: {
            if (ip + 3 >= code.size()) return false;
            uint32_t value = 0;
            for (int byte = 0; byte < 4; ++byte)
                value |= static_cast<uint32_t>(code[ip++]) << (8 * byte);
            if (out.tag == OperandTag::StrIdx) out.strIdx = value;
            else out.intVal = value;   // Addr keeps its target byte offset in intVal
            return true;
        }
        default:
            return false;
    }
}

} // namespace

std::string formatOperand(const DecodedOperand& operand, const Chunk& chunk) {
    std::ostringstream oss;
    switch (operand.tag) {
        case OperandTag::Reg:
            oss << "r" << static_cast<int>(operand.intVal);
            break;
        case OperandTag::Imm64:
            oss << "#" << operand.intVal;
            break;
        case OperandTag::Float64: {
            double value = 0.0;
            const uint64_t bits = static_cast<uint64_t>(operand.intVal);
            std::memcpy(&value, &bits, sizeof(value));
            oss << "%" << value;
            break;
        }
        case OperandTag::StrIdx:
            oss << "str[" << operand.strIdx << "]";
            if (operand.strIdx < chunk.strings.size())
                oss << "(\"" << chunk.strings[operand.strIdx] << "\")";
            break;
        case OperandTag::Addr:
            oss << "0x" << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<uint32_t>(operand.intVal) << std::dec;
            break;
        case OperandTag::Null:
            oss << "null";
            break;
        case OperandTag::WideReg:
            oss << "r" << static_cast<int>(operand.intVal) << ":wide";
            break;
    }
    return oss.str();
}

bool decodeBytecode(const Chunk& chunk, std::vector<DecodedInstr>& out, std::string* error) {
    out.clear();

    const std::vector<uint8_t>& code = chunk.code;
    uint32_t ip = 0;
    while (ip < code.size()) {
        DecodedInstr instr;
        instr.byteOffset = ip;
        instr.op = static_cast<OpCode>(code[ip++]);

        int operandCount = 0;
        if (!operandCountFor(instr.op, code, ip, operandCount)) {
            if (error)
                *error = "unknown opcode 0x" + std::to_string(static_cast<int>(code[instr.byteOffset]))
                       + " at byte offset " + std::to_string(instr.byteOffset);
            return false;
        }

        for (int i = 0; i < operandCount; ++i) {
            DecodedOperand operand;
            if (!decodeOperandAt(code, ip, operand)) {
                if (error)
                    *error = "malformed operand " + std::to_string(i) + " for "
                           + opcodeName(instr.op) + " at byte offset "
                           + std::to_string(instr.byteOffset);
                return false;
            }
            instr.operands.push_back(operand);
        }

        out.push_back(std::move(instr));
    }
    return true;
}

void dumpBytecode(const Chunk& chunk, std::ostream& os) {
    std::vector<DecodedInstr> instrs;
    std::string error;
    if (!decodeBytecode(chunk, instrs, &error)) {
        os << "; <decode failed: " << error << ">\n";
        return;
    }

    // Byte offset -> enclosing function, so each block can be titled.
    std::vector<const FunctionDebugInfo*> starts;
    for (const auto& fn : chunk.functions) starts.push_back(&fn);

    os << "; " << instrs.size() << " instructions, " << chunk.code.size() << " bytes\n";

    size_t nextFn = 0;
    for (const auto& instr : instrs) {
        while (nextFn < starts.size() && starts[nextFn]->start < instr.byteOffset) ++nextFn;
        if (nextFn < starts.size() && starts[nextFn]->start == instr.byteOffset) {
            const auto& fn = *starts[nextFn];
            os << fn.name << ":\n";   // function entry
            ++nextFn;
        }

        std::ostringstream line;
        line << "  " << std::hex << std::setw(5) << std::setfill('0') << instr.byteOffset
             << std::dec << "  " << std::setfill(' ')   // '0' must not leak into the mnemonic
             << std::setw(9) << std::left << opcodeName(instr.op);
        for (size_t i = 0; i < instr.operands.size(); ++i) {
            line << (i == 0 ? " " : ", ") << formatOperand(instr.operands[i], chunk);
        }
        os << line.str() << "\n";
    }
}
