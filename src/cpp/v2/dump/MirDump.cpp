#include "Dump.hpp"

#include "../mir/MIR.hpp"
#include "../backend/Lowering.hpp"

#include <sstream>
#include <variant>

namespace loxis::v2::dump {
namespace {

const char* binOpName(MirBinOp op) {
    switch (op) {
        case MirBinOp::Add:    return "Add";
        case MirBinOp::Sub:    return "Sub";
        case MirBinOp::Mul:    return "Mul";
        case MirBinOp::Div:    return "Div";
        case MirBinOp::Rem:    return "Rem";
        case MirBinOp::Shl:    return "Shl";
        case MirBinOp::Shr:    return "Shr";
        case MirBinOp::BitAnd: return "BitAnd";
        case MirBinOp::BitOr:  return "BitOr";
        case MirBinOp::BitXor: return "BitXor";
        case MirBinOp::And:    return "And";
        case MirBinOp::Or:     return "Or";
        case MirBinOp::Eq:     return "Eq";
        case MirBinOp::Ne:     return "Ne";
        case MirBinOp::Lt:     return "Lt";
        case MirBinOp::Le:     return "Le";
        case MirBinOp::Gt:     return "Gt";
        case MirBinOp::Ge:     return "Ge";
    }
    return "?";
}

const char* unaryOpName(MirUnaryOp op) {
    switch (op) {
        case MirUnaryOp::Neg:    return "Neg";
        case MirUnaryOp::Not:    return "Not";
        case MirUnaryOp::Deref:  return "Deref";
        case MirUnaryOp::Ref:    return "Ref";
        case MirUnaryOp::RefMut: return "RefMut";
        case MirUnaryOp::ToStr:  return "ToStr";
    }
    return "?";
}

const char* assignOpName(MirAssignOp op) {
    switch (op) {
        case MirAssignOp::Set:     return "Set";
        case MirAssignOp::AddEq:   return "AddEq";
        case MirAssignOp::SubEq:   return "SubEq";
        case MirAssignOp::MulEq:   return "MulEq";
        case MirAssignOp::DivEq:   return "DivEq";
        case MirAssignOp::RemEq:   return "RemEq";
        case MirAssignOp::AmpEq:   return "AmpEq";
        case MirAssignOp::PipeEq:  return "PipeEq";
        case MirAssignOp::CaretEq: return "CaretEq";
        case MirAssignOp::ShlEq:   return "ShlEq";
        case MirAssignOp::ShrEq:   return "ShrEq";
    }
    return "?";
}

std::string localText(LocalId local) { return "%" + std::to_string(local); }

// Block `n` is always labelled `<fn>_b<n>` -- BlockId is an index into
// MirBody::blocks, so the name is derived rather than stored.
std::string blockText(const std::string& fnPrefix, BlockId block) {
    return fnPrefix + "_b" + std::to_string(block);
}

std::string constText(const OpConst& c) {
    return std::visit([](const auto& value) -> std::string {
        using V = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<V, std::monostate>) return "null";
        else if constexpr (std::is_same_v<V, bool>)  return value ? "true" : "false";
        else if constexpr (std::is_same_v<V, std::string>) return "\"" + value + "\"";
        else if constexpr (std::is_same_v<V, char>)  return "'" + std::string(1, value) + "'";
        else if constexpr (std::is_same_v<V, double>) return std::to_string(value);
        else return std::to_string(value);
    }, c.val);
}

struct OperandPrinter {
    Printer& p;

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, OpCopy>) {
            p.line("copy " + localText(n.local));
        } else if constexpr (std::is_same_v<T, OpMove>) {
            p.line("move " + localText(n.local));
        } else if constexpr (std::is_same_v<T, OpConst>) {
            p.line("const " + constText(n));
        } else if constexpr (std::is_same_v<T, OpLabel>) {
            p.line("label " + n.label);
        }
    }
};

std::string operandText(const Operand& operand) {
    return std::visit([](const auto& n) -> std::string {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, OpCopy>)  return "copy " + localText(n.local);
        else if constexpr (std::is_same_v<T, OpMove>) return "move " + localText(n.local);
        else if constexpr (std::is_same_v<T, OpConst>) return "const " + constText(n);
        else return "label " + n.label;
    }, operand);
}

struct RvaluePrinter {
    Printer& p;
    const std::string& blockPrefix;

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, RvUse>) {
            p.line("use " + operandText(n.op));
        } else if constexpr (std::is_same_v<T, RvRef>) {
            p.line(std::string("ref ") + (n.mut_ ? "mut " : "") + localText(n.local));
        } else if constexpr (std::is_same_v<T, RvPtr>) {
            p.line(std::string("ptr ") + (n.mut_ ? "mut " : "") + localText(n.local));
        } else if constexpr (std::is_same_v<T, RvBinary>) {
            p.line(std::string("binary ") + binOpName(n.op) + " " + operandText(n.l)
                   + ", " + operandText(n.r));
        } else if constexpr (std::is_same_v<T, RvUnary>) {
            p.line(std::string("unary ") + unaryOpName(n.op) + " " + operandText(n.opnd));
        } else if constexpr (std::is_same_v<T, RvCast>) {
            p.line("cast " + operandText(n.op) + " as " + (n.ty ? n.ty->toString() : "?"));
        } else if constexpr (std::is_same_v<T, RvAggregate>) {
            const char* kind = n.kind == RvAggregate::Tuple ? "tuple"
                             : n.kind == RvAggregate::Array ? "array" : "adt";
            p.open(std::string("aggregate ") + kind + " variant=" + std::to_string(n.variant));
            for (const auto& op : n.ops) p.line(operandText(op));
            p.close();
        } else if constexpr (std::is_same_v<T, RvField>) {
            p.line("field " + localText(n.local) + " +" + std::to_string(n.field));
        } else if constexpr (std::is_same_v<T, RvHStore>) {
            p.line("hstore +" + std::to_string(n.offset) + " = " + operandText(n.value));
        } else if constexpr (std::is_same_v<T, RvIndex>) {
            p.line("index " + localText(n.local) + "[" + operandText(n.idx) + "]");
        } else if constexpr (std::is_same_v<T, RvIndexStore>) {
            p.line("indexstore [" + operandText(n.idx) + "] = " + operandText(n.value));
        } else if constexpr (std::is_same_v<T, RvLen>) {
            p.line("len " + localText(n.local));
        } else if constexpr (std::is_same_v<T, RvDiscriminant>) {
            p.line("discriminant " + localText(n.local));
        } else if constexpr (std::is_same_v<T, RvAlloc>) {
            p.line("alloc size=" + std::to_string(n.size) + " typeId=" + std::to_string(n.typeId));
        } else if constexpr (std::is_same_v<T, RvClosure>) {
            p.open("closure " + n.functionName + " envTypeId=" + std::to_string(n.envTypeId));
            for (const auto& capture : n.captures) p.line("capture " + operandText(capture));
            p.close();
        } else if constexpr (std::is_same_v<T, RvClosureGet>) {
            p.line("closure_get " + localText(n.env) + " +" + std::to_string(n.offset));
        } else if constexpr (std::is_same_v<T, RvClosureSet>) {
            p.line("closure_set +" + std::to_string(n.offset) + " = " + operandText(n.value));
        } else if constexpr (std::is_same_v<T, RvVTableSlot>) {
            p.line("vtable_slot " + localText(n.obj) + " [" + std::to_string(n.slot) + "]");
        } else if constexpr (std::is_same_v<T, RvTypeId>) {
            p.line("typeid " + localText(n.obj));
        } else if constexpr (std::is_same_v<T, RvIsCheck>) {
            p.line("is " + localText(n.obj) + " targetTypeId=" + std::to_string(n.targetTypeId));
        } else if constexpr (std::is_same_v<T, RvGlobalLoad>) {
            p.line("global_load g" + std::to_string(n.slot));
        } else if constexpr (std::is_same_v<T, RvGlobalStore>) {
            p.line("global_store g" + std::to_string(n.slot) + " = " + operandText(n.value));
        }
    }
};

struct StatementPrinter {
    Printer& p;
    const std::string& blockPrefix;

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, StAssign>) {
            p.open("assign " + localText(n.place));
            std::visit(RvaluePrinter{p, blockPrefix}, n.rv);
            p.close();
        } else if constexpr (std::is_same_v<T, StStorageLive>) {
            p.line("storage_live " + localText(n.local));
        } else if constexpr (std::is_same_v<T, StStorageDead>) {
            p.line("storage_dead " + localText(n.local));
        } else if constexpr (std::is_same_v<T, StSetDiscriminant>) {
            p.line("set_discriminant " + localText(n.place) + " = " + std::to_string(n.variant));
        } else if constexpr (std::is_same_v<T, StDrop>) {
            p.line("drop " + localText(n.local));
        } else if constexpr (std::is_same_v<T, StPush>) {
            p.line("push " + localText(n.local));
        } else if constexpr (std::is_same_v<T, StPop>) {
            p.line("pop " + localText(n.local));
        } else if constexpr (std::is_same_v<T, StGlobalStore>) {
            p.line("global_store g" + std::to_string(n.slot) + " = " + operandText(n.value));
        } else if constexpr (std::is_same_v<T, StTryPush>) {
            p.line("try_push handler=" + blockText(blockPrefix, n.handler)
                   + " exception=" + localText(n.exceptionLocal));
        } else if constexpr (std::is_same_v<T, StTryPop>) {
            p.line("try_pop");
        } else if constexpr (std::is_same_v<T, StThrow>) {
            p.line("throw " + operandText(n.value));
        }
    }
};

struct TerminatorPrinter {
    Printer& p;
    const std::string& blockPrefix;

    std::string allArgs(const std::vector<Operand>& args) const {
        std::string joined;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) joined += ", ";
            joined += operandText(args[i]);
        }
        return joined;
    }

    template <typename T>
    void operator()(const T& n) const {
        if constexpr (std::is_same_v<T, TmGoto>) {
            p.line("goto " + blockText(blockPrefix, n.target));
        } else if constexpr (std::is_same_v<T, TmSwitchInt>) {
            p.open("switch_int " + operandText(n.discr));
            for (const auto& [value, target] : n.values) {
                p.line("case " + std::to_string(value) + " -> " + blockText(blockPrefix, target));
            }
            p.line("otherwise -> " + blockText(blockPrefix, n.otherwise));
            p.close();
        } else if constexpr (std::is_same_v<T, TmSwitchDiscr>) {
            p.open("switch_discr " + localText(n.local));
            for (size_t i = 0; i < n.targets.size(); ++i) {
                p.line("case " + std::to_string(i) + " -> " + blockText(blockPrefix, n.targets[i]));
            }
            p.line("otherwise -> " + blockText(blockPrefix, n.otherwise));
            p.close();
        } else if constexpr (std::is_same_v<T, TmReturn>) {
            p.line("return");
        } else if constexpr (std::is_same_v<T, TmCall>) {
            p.line("call " + operandText(n.func) + "(" + allArgs(n.args) + ") -> "
                   + localText(n.dest) + " then " + blockText(blockPrefix, n.return_));
        } else if constexpr (std::is_same_v<T, TmCallIndirect>) {
            p.line("call_indirect " + operandText(n.funcPtr) + "(" + allArgs(n.args) + ") -> "
                   + localText(n.dest) + " then " + blockText(blockPrefix, n.return_));
        } else if constexpr (std::is_same_v<T, TmClosureCall>) {
            p.line("closure_call " + operandText(n.closure) + "(" + allArgs(n.args) + ") -> "
                   + localText(n.dest) + " then " + blockText(blockPrefix, n.return_));
        } else if constexpr (std::is_same_v<T, TmCheckNull>) {
            p.line("check_null " + operandText(n.value) + " null->"
                   + blockText(blockPrefix, n.nullTarget) + " ok->"
                   + blockText(blockPrefix, n.okTarget));
        } else if constexpr (std::is_same_v<T, TmUnreachable>) {
            p.line("unreachable");
        } else if constexpr (std::is_same_v<T, TmVirtualCall>) {
            p.line("virtual_call " + localText(n.recv) + " slot=" + std::to_string(n.vtableSlot)
                   + "(" + allArgs(n.args) + ") -> " + localText(n.dest) + " then "
                   + blockText(blockPrefix, n.return_));
        } else if constexpr (std::is_same_v<T, TmInterfaceCall>) {
            p.line("interface_call " + localText(n.recv) + " iface=" + std::to_string(n.ifaceTypeId)
                   + " method=" + std::to_string(n.methodIdx) + "(" + allArgs(n.args) + ") -> "
                   + localText(n.dest) + " then " + blockText(blockPrefix, n.return_));
        }
    }
};

} // namespace

void dumpMir(const LoweringOutput& output, std::ostream& os) {
    Printer p(os);

    os << "; " << output.bodies.size() << " MIR bodies, " << output.classes.size()
       << " classes, " << output.globalInitializers.size() << " global initializers\n";

    for (const auto& global : output.globalInitializers) {
        p.line("global_init g" + std::to_string(global.slot) + " = " + global.initLabel);
    }
    for (const auto& cls : output.classes) {
        p.open("class " + cls.name + " typeId=" + std::to_string(cls.typeId)
               + " parent=" + std::to_string(cls.parentTypeId)
               + " vtable=" + std::to_string(cls.vtableEntryCount));
        for (const auto& name : cls.vtableMethodNames) p.line("vtable " + name);
        p.close();
    }

    for (const auto& body : output.bodies) {
        p.open("fn " + body.name + " -> " + (body.ret_ty ? body.ret_ty->toString() : "?")
               + " params=" + std::to_string(body.paramCount));
        for (size_t i = 0; i < body.locals.size(); ++i) {
            const LocalDecl& local = body.locals[i];
            p.line("local " + localText(static_cast<LocalId>(i))
                   + (local.mut_ ? " mut " : " ") + local.name + " : "
                   + (local.ty ? local.ty->toString() : "?"));
        }
        for (size_t i = 0; i < body.blocks.size(); ++i) {
            const BasicBlock& block = body.blocks[i];
            p.open(blockText(body.name, static_cast<BlockId>(i)) + ":");
            for (const auto& stmt : block.stmts) std::visit(StatementPrinter{p, body.name}, stmt);
            std::visit(TerminatorPrinter{p, body.name}, block.term);
            p.close();
        }
        p.close();
    }
}

} // namespace loxis::v2::dump
