#include "Lowering.hpp"
#include "../common.hpp"
#include <cassert>
#include <stdexcept>

namespace loxis::v2 {

namespace {
// Maps a compound assignment operator onto the binary operator it applies.
// MirAssignOp::Set has no binary counterpart and falls back to Add.
MirBinOp binOpForAssign(MirAssignOp op) {
    switch (op) {
        case MirAssignOp::AddEq:  return MirBinOp::Add;
        case MirAssignOp::SubEq:  return MirBinOp::Sub;
        case MirAssignOp::MulEq:  return MirBinOp::Mul;
        case MirAssignOp::DivEq:  return MirBinOp::Div;
        case MirAssignOp::RemEq:  return MirBinOp::Rem;
        case MirAssignOp::AmpEq:  return MirBinOp::BitAnd;
        case MirAssignOp::PipeEq: return MirBinOp::BitOr;
        case MirAssignOp::CaretEq:return MirBinOp::BitXor;
        case MirAssignOp::ShlEq:  return MirBinOp::Shl;
        case MirAssignOp::ShrEq:  return MirBinOp::Shr;
        default:                  return MirBinOp::Add;
    }
}
} // namespace

bool Lowering::lowerFinalizersTo(size_t depth) {
    if (depth > m_finallyStack.size()) depth = m_finallyStack.size();
    auto saved = m_finallyStack;
    auto savedHandlers = m_finallyHandlerActive;
    m_finallyStack.resize(depth);
    m_finallyHandlerActive.resize(depth);
    for (size_t i = saved.size(); i > depth; --i) {
        if (i - 1 < savedHandlers.size() && savedHandlers[i - 1]) emit(StTryPop{});
        if (saved[i - 1]) lowerExpr(*saved[i - 1]);
        if (m_blockTerminated) {
            m_finallyStack = std::move(saved);
            m_finallyHandlerActive = std::move(savedHandlers);
            return false;
        }
    }
    m_finallyStack = std::move(saved);
    m_finallyHandlerActive = std::move(savedHandlers);
    return true;
}

LoweringOutput Lowering::lower(const HirModule& hir) {
    LoweringOutput output;
    for (const auto& item : hir.items) {
        std::visit([&](const auto& i) {
            using T = std::decay_t<decltype(i)>;
            if constexpr (std::is_same_v<T, HirFn>) {
                output.bodies.push_back(lowerFn(i));
                // A top-level `var` initializer also fills its global slot. The
                // runtime calls the body and stores its result, the same way it
                // seeds singleton objects.
                if (i.globalSlot != ~0u) {
                    output.globalInitializers.push_back({i.globalSlot, i.name});
                }
            } else if constexpr (std::is_same_v<T, HirClass>) {
                output.bodies.push_back(lowerClassCtor(i));
                if (i.isSingleton && i.globalSlot != ~0u) {
                    output.globalInitializers.push_back({i.globalSlot, i.name + ".init"});
                }
                for (const auto& m : i.methods) {
                    output.bodies.push_back(lowerMethod(i, m));
                }
                LoweredClass lc;
                lc.name = i.name;
                lc.typeId = i.typeId;
                lc.parentTypeId = i.parentTypeId;
                lc.vtableEntryCount = i.vtableEntryCount;
                lc.vtableMethodNames = i.vtableMethodNames;
                lc.itables = i.itables;
                output.classes.push_back(std::move(lc));
            }
        }, item);
    }
    return output;
}

MirBody Lowering::lowerClassCtor(const HirClass& cls) {
    MirBody body;
    m_currentClassName = cls.name;
    body.name = cls.name + ".init";
    body.ret_ty = mkInt();
    body.paramCount = static_cast<uint32_t>(cls.ctorParams.size());
    body.locals.push_back({body.ret_ty, "_return", false});

    m_scope.clear();
    m_scratchLocal.reset();
    m_closureEnvLocal.reset();
    for (const auto& pd : cls.ctorParams) {
        LocalId lid = static_cast<LocalId>(body.locals.size());
        body.locals.push_back({pd.ty, pd.name, false});
        m_scope[pd.name] = lid;
    }

    m_body = &body;
    m_loopStack.clear();
    m_finallyStack.clear();
    m_finallyHandlerActive.clear();
    body.blocks.clear();

    BlockId entry = newBlock();
    m_currentBlock = entry;
    m_blockTerminated = false;
    m_exitBlock = newBlock();

    uint32_t allocSize = cls.totalFieldSize > 0 ? cls.totalFieldSize : 9;
    LocalId obj = newLocal(mkInt(), "self");
    m_scope["self"] = obj;
    m_scope["this"] = obj;
    emit(StAssign{obj, RvAlloc{allocSize, cls.typeId}});

    for (size_t i = 0; i < cls.superClassArgs.size(); ++i) {
        LocalId value = lowerExpr(*cls.superClassArgs[i]);
        uint32_t offset = i < cls.superFieldOffsets.size()
            ? cls.superFieldOffsets[i]
            : static_cast<uint32_t>(i * 9);
        emit(StAssign{obj, RvHStore{offset, OpCopy{value}}});
    }

    for (const auto& pd : cls.ctorParams) {
        if (pd.isVal || pd.isVar) {
            auto it = m_scope.find(pd.name);
            if (it != m_scope.end()) {
                emit(StAssign{obj, RvHStore{pd.fieldOffset, OpCopy{it->second}}});
            }
        }
    }

    for (const auto& field : cls.fields) {
        if (!field.initializer) continue;
        LocalId initVal = lowerExpr(*field.initializer);
        emit(StAssign{obj, RvHStore{field.offset, OpCopy{initVal}}});
    }

    for (const auto& initBlock : cls.initBlocks) {
        if (initBlock) {
            lowerExpr(*initBlock);
        }
    }

    if (!m_blockTerminated) {
        emit(StAssign{0, RvUse{OpCopy{obj}}});
        term(TmGoto{m_exitBlock});
    }

    if (body.blocks.size() <= static_cast<size_t>(m_exitBlock))
        body.blocks.resize(m_exitBlock + 1);
    body.blocks[m_exitBlock].term = TmReturn{};

    m_body = nullptr;
    m_currentClassName.clear();
    return body;
}

MirBody Lowering::lowerMethod(const HirClass& cls, const HirFn& method) {
    std::string qualifiedName = cls.name + "." + method.name;
    m_currentClassName = cls.name;
    MirBody body;
    body.name = qualifiedName;
    body.ret_ty = method.ret;
    body.paramCount = static_cast<uint32_t>(method.params.size() + 1);
    body.locals.push_back({body.ret_ty, "_return", false});

    m_scope.clear();
    m_scratchLocal.reset();
    m_closureEnvLocal.reset();
    LocalId selfLid = static_cast<LocalId>(body.locals.size());
    body.locals.push_back({mkInt(), "self", false});
    m_scope["self"] = selfLid;
    m_scope["this"] = selfLid;

    for (const auto& p : method.params) {
        LocalId lid = static_cast<LocalId>(body.locals.size());
        body.locals.push_back({p.second, p.first, false});
        m_scope[p.first] = lid;
    }

    m_body = &body;
    m_loopStack.clear();
    m_finallyStack.clear();
    m_finallyHandlerActive.clear();
    body.blocks.clear();

    BlockId entry = newBlock();
    m_currentBlock = entry;
    m_blockTerminated = false;
    m_exitBlock = newBlock();

    LocalId result = lowerExpr(*method.body);
    if (!m_blockTerminated) {
        emit(StAssign{0, RvUse{OpCopy{result}}});
        term(TmGoto{m_exitBlock});
    }

    if (body.blocks.size() <= static_cast<size_t>(m_exitBlock))
        body.blocks.resize(m_exitBlock + 1);
    body.blocks[m_exitBlock].term = TmReturn{};

    m_body = nullptr;
    m_currentClassName.clear();
    return body;
}

MirBody Lowering::lowerFn(const HirFn& fn) {
    MirBody body;
    body.name = fn.name;
    body.ret_ty = fn.ret;
    body.paramCount = static_cast<uint32_t>(fn.params.size() + (fn.closureEnvTypeId != ~0u ? 1 : 0));
    body.locals.push_back({body.ret_ty, "_return", false});

    m_scope.clear();
    m_scratchLocal.reset();
    m_closureEnvLocal.reset();
    if (fn.closureEnvTypeId != ~0u) {
        LocalId envLid = static_cast<LocalId>(body.locals.size());
        body.locals.push_back({mkInt(), "__closure_env", false});
        m_scope["__closure_env"] = envLid;
        m_closureEnvLocal = envLid;
    }
    for (const auto& p : fn.params) {
        LocalId lid = static_cast<LocalId>(body.locals.size());
        body.locals.push_back({p.second, p.first, false});
        m_scope[p.first] = lid;
    }

    m_body = &body;
    m_loopStack.clear();
    m_finallyStack.clear();
    m_finallyHandlerActive.clear();
    body.blocks.clear();

    BlockId entry = newBlock();
    m_currentBlock = entry;
    m_blockTerminated = false;
    m_exitBlock = newBlock();

    LocalId result = lowerExpr(*fn.body);
    if (!m_blockTerminated) {
        emit(StAssign{0, RvUse{OpCopy{result}}});
        term(TmGoto{m_exitBlock});
    }

    if (body.blocks.size() <= static_cast<size_t>(m_exitBlock))
        body.blocks.resize(m_exitBlock + 1);
    body.blocks[m_exitBlock].term = TmReturn{};

    m_body = nullptr;
    return body;
}

LocalId Lowering::newLocal(CoreTyPtr ty, const std::string& name, bool mut) {
    LocalId id = static_cast<LocalId>(m_body->locals.size());
    m_body->locals.push_back({ty, name, mut});
    return id;
}

BlockId Lowering::newBlock() {
    BlockId id = static_cast<BlockId>(m_body->blocks.size());
    m_body->blocks.push_back(BasicBlock{});
    return id;
}

void Lowering::emit(Statement stmt) {
    if (m_blockTerminated) return;
    stmt.loc = m_currentLoc;
    m_body->blocks[m_currentBlock].stmts.push_back(std::move(stmt));
}

void Lowering::term(Terminator t) {
    if (m_blockTerminated) return;
    t.loc = m_currentLoc;
    m_body->blocks[m_currentBlock].term = std::move(t);
    m_blockTerminated = true;
}

LocalId Lowering::unitLocal() {
    LocalId tmp = newLocal(mkUnit(), "unit");
    emit(StAssign{tmp, RvUse{OpConst{0}}});
    return tmp;
}

LocalId Lowering::scratchLocal() {
    if (!m_scratchLocal) m_scratchLocal = newLocal(mkUnit(), "_scratch");
    return *m_scratchLocal;
}

LocalId Lowering::lowerPlace(const HirExpr& expr) {
    return std::visit([&](const auto& e) -> LocalId {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, HirPath>) {
            auto it = m_scope.find(e.name);
            if (it != m_scope.end()) return it->second;
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirDeref>) {
            return lowerExpr(*e.expr);
        } else if constexpr (std::is_same_v<T, HirUnary>) {
            if (e.op == MirUnaryOp::Deref) return lowerExpr(*e.operand);
            return lowerExpr(expr);
        } else {
            // There is no place here, only a value. Falling back to lowerExpr would
            // hand back a discarded temporary and turn every assignment into a silent
            // no-op -- that is how `a[i] = x` went unnoticed. The type checker's
            // isMutablePlace admits only Path/Deref/Field/Index as assignment targets,
            // so anything else arriving here is a compiler bug, not user input.
            panic("lowering was asked for the place of a non-place expression");
        }
    }, expr);
}

LocalId Lowering::lowerExpr(const HirExpr& expr) {
    if (m_blockTerminated) return unitLocal();

    SourceLocation previousLoc = m_currentLoc;
    m_currentLoc = expr.loc;
    LocalId lowered = std::visit([&](const auto& e) -> LocalId {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, HirLit>) {
            LocalId tmp = newLocal(e.ty, "lit");
            std::visit([&](const auto& value) { emit(StAssign{tmp, RvUse{OpConst{value}}}); }, e.val);
            return tmp;
        } else if constexpr (std::is_same_v<T, HirPath>) {
            if (e.globalSlot != ~0u) {
                LocalId tmp = newLocal(e.ty, "global");
                emit(StAssign{tmp, RvGlobalLoad{e.globalSlot}});
                return tmp;
            }
            auto it = m_scope.find(e.name);
            if (it != m_scope.end()) return it->second;
            if (e.captureOffset != ~0u && m_closureEnvLocal.has_value()) {
                LocalId tmp = newLocal(e.ty, "capture");
                emit(StAssign{tmp, RvClosureGet{*m_closureEnvLocal, e.captureOffset}});
                return tmp;
            }
            if (e.isStaticFunction) {
                LocalId tmp = newLocal(e.ty, "fnptr");
                emit(StAssign{tmp, RvUse{OpLabel{e.name}}});
                return tmp;
            }
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirTuple>) {
            std::vector<Operand> ops;
            for (const auto& elem : e.elems) {
                LocalId l = lowerExpr(*elem);
                ops.push_back(OpCopy{l});
            }
            LocalId tmp = newLocal(e.ty, "tuple");
            emit(StAssign{tmp, RvAggregate{RvAggregate::Tuple, 0, std::move(ops)}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirArray>) {
            std::vector<Operand> ops;
            for (const auto& elem : e.elems) {
                LocalId l = lowerExpr(*elem);
                ops.push_back(OpCopy{l});
            }
            LocalId tmp = newLocal(e.ty, "array");
            emit(StAssign{tmp, RvAggregate{RvAggregate::Array, 0, std::move(ops)}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirStruct>) {
            uint32_t sz = e.totalFieldSize > 0 ? e.totalFieldSize : 9;
            LocalId obj = newLocal(e.ty, "obj");
            emit(StAssign{obj, RvAlloc{sz, e.typeId}});
            uint32_t fieldIdx = 0;
            for (const auto& f : e.fields) {
                LocalId fval = lowerExpr(*f.expr);
                emit(StAssign{obj, RvHStore{fieldIdx * 9, OpCopy{fval}}});
                fieldIdx++;
            }
            return obj;
        } else if constexpr (std::is_same_v<T, HirClosure>) {
            std::vector<Operand> captures;
            for (const auto& capture : e.captures) {
                auto it = m_scope.find(capture.name);
                if (it != m_scope.end()) {
                    captures.push_back(OpCopy{it->second});
                }
            }
            LocalId tmp = newLocal(e.ty, "closure");
            emit(StAssign{tmp, RvClosure{e.functionName, e.envTypeId, std::move(captures)}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirEnumCtor>) {
            uint32_t sz = e.totalFieldSize > 0 ? e.totalFieldSize : 9;
            LocalId obj = newLocal(e.ty, "enum");
            emit(StAssign{obj, RvAlloc{sz, e.typeId}});
            emit(StAssign{obj, RvHStore{0, OpConst{static_cast<int64_t>(e.variantIndex)}}});
            for (size_t i = 0; i < e.args.size() && i < e.fieldOffsets.size(); ++i) {
                LocalId value = lowerExpr(*e.args[i]);
                emit(StAssign{obj, RvHStore{e.fieldOffsets[i], OpCopy{value}}});
            }
            return obj;
        } else if constexpr (std::is_same_v<T, HirCall>) {
            Operand funcOp = OpConst{std::string{}};
            bool directCall = !e.ctorLabel.empty() || e.staticName.has_value();
            bool closureCall = false;
            if (!e.ctorLabel.empty()) funcOp = OpConst{e.ctorLabel};
            else if (e.staticName) funcOp = OpConst{*e.staticName};
            else {
                LocalId funcLocal = lowerExpr(*e.func);
                funcOp = OpCopy{funcLocal};
                if (e.func && std::holds_alternative<HirClosure>(*e.func)) {
                    closureCall = true;
                } else if (e.func && std::holds_alternative<HirPath>(*e.func)) {
                    const auto& path = std::get<HirPath>(*e.func);
                    closureCall = path.captureOffset != ~0u;
                }
            }

            std::vector<Operand> args;
            for (const auto& a : e.args) {
                LocalId l = lowerExpr(*a);
                args.push_back(OpCopy{l});
            }
            LocalId dest = newLocal(e.ty, "call");

            BlockId resume = newBlock();
            if (directCall) term(TmCall{funcOp, std::move(args), dest, resume});
            else if (closureCall) term(TmClosureCall{funcOp, std::move(args), dest, resume});
            else term(TmCallIndirect{funcOp, std::move(args), dest, resume});
            m_currentBlock = resume;
            m_blockTerminated = false;

            emit(StAssign{dest, RvUse{OpMove{0}}});
            return dest;
        } else if constexpr (std::is_same_v<T, HirMethodCall>) {
            LocalId recv = lowerExpr(*e.recv);
            std::vector<Operand> args;
            args.push_back(OpCopy{recv});
            for (const auto& a : e.args) {
                LocalId l = lowerExpr(*a);
                args.push_back(OpCopy{l});
            }
            LocalId dest = newLocal(e.ty, "call");

            BlockId resume = newBlock();
            if (e.isInterfaceDispatch) {
                term(TmInterfaceCall{recv, e.ifaceTypeId, e.ifaceMethodIndex, std::move(args), dest, resume});
            } else if (e.vtableIndex != ~0u) {
                term(TmVirtualCall{recv, e.vtableIndex, std::move(args), dest, resume});
            } else {
                std::string target = e.qualifiedName.empty() ? e.method : e.qualifiedName;
                term(TmCall{OpConst{target}, std::move(args), dest, resume});
            }

            m_currentBlock = resume;
            m_blockTerminated = false;

            emit(StAssign{dest, RvUse{OpMove{0}}});
            return dest;
        } else if constexpr (std::is_same_v<T, HirField>) {
            LocalId obj = lowerExpr(*e.obj);
            LocalId tmp = newLocal(e.ty, "field");
            emit(StAssign{tmp, RvField{obj, e.fieldOffset}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirIndex>) {
            LocalId obj = lowerExpr(*e.obj);
            LocalId idx = lowerExpr(*e.idx);
            LocalId tmp = newLocal(e.ty, "index");
            emit(StAssign{tmp, RvIndex{obj, OpCopy{idx}}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirUnary>) {
            LocalId opnd = lowerExpr(*e.operand);
            LocalId tmp = newLocal(e.ty, "unary");
            if (e.op == MirUnaryOp::ToStr) {
                emit(StAssign{tmp, RvUnary{MirUnaryOp::ToStr, OpCopy{opnd}}});
            } else if (e.op == MirUnaryOp::Neg || e.op == MirUnaryOp::Not) {
                emit(StAssign{tmp, RvUnary{e.op, OpCopy{opnd}}});
            } else if (e.op == MirUnaryOp::Deref) {
                emit(StAssign{tmp, RvUse{OpCopy{opnd}}});
            } else if (e.op == MirUnaryOp::Ref || e.op == MirUnaryOp::RefMut) {
                emit(StAssign{tmp, RvRef{e.op == MirUnaryOp::RefMut, opnd}});
            } else {
                emit(StAssign{tmp, RvUse{OpCopy{opnd}}});
            }
            return tmp;
        } else if constexpr (std::is_same_v<T, HirBinary>) {
            if (e.op == MirBinOp::And || e.op == MirBinOp::Or) {
                auto constant = std::make_shared<HirExpr>(HirLit{mkBool(), e.op == MirBinOp::Or});
                HirIf branch{e.l,
                    e.op == MirBinOp::And ? e.r : constant,
                    e.op == MirBinOp::And ? constant : e.r, mkBool()};
                return lowerExpr(HirExpr{branch});
            }
            LocalId l = lowerExpr(*e.l);
            LocalId r = lowerExpr(*e.r);
            LocalId tmp = newLocal(e.ty, "bin");
            emit(StAssign{tmp, RvBinary{e.op, OpCopy{l}, OpCopy{r}}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirAssign>) {
            // Check if LHS is a field access (needs HStore instead of StAssign)
            bool isField = (e.lhs && std::holds_alternative<HirField>(*e.lhs));
            bool isIndex = (e.lhs && std::holds_alternative<HirIndex>(*e.lhs));
            uint32_t globalSlot = ~0u;
            if (e.lhs) {
                if (auto* path = std::get_if<HirPath>(e.lhs.get())) globalSlot = path->globalSlot;
            }
            if (globalSlot != ~0u) {
                // Top-level `var`: the binding lives in a VM global, so this is a
                // GStore rather than an assignment to a register.
                LocalId rhs = lowerExpr(*e.rhs);
                if (e.op != MirAssignOp::Set) {
                    LocalId oldVal = newLocal(e.ty, "glob_rd");
                    emit(StAssign{oldVal, RvGlobalLoad{globalSlot}});
                    LocalId newVal = newLocal(e.ty, "glob_wr");
                    emit(StAssign{newVal, RvBinary{binOpForAssign(e.op), OpCopy{oldVal}, OpCopy{rhs}}});
                    rhs = newVal;
                }
                emit(StAssign{scratchLocal(), RvGlobalStore{globalSlot, OpCopy{rhs}}});
            } else if (isField) {
                auto& f = std::get<HirField>(*e.lhs);
                LocalId obj = lowerExpr(*f.obj);
                LocalId rhs = lowerExpr(*e.rhs);
                if (e.op == MirAssignOp::Set) {
                    emit(StAssign{obj, RvHStore{f.fieldOffset, OpCopy{rhs}}});
                } else {
                    // Compound assignment: read field, compute, write back
                    LocalId oldVal = newLocal(f.ty, "fld_rd");
                    emit(StAssign{oldVal, RvField{obj, f.fieldOffset}});
                    MirBinOp bop;
                    switch (e.op) {
                        case MirAssignOp::AddEq: bop = MirBinOp::Add; break;
                        case MirAssignOp::SubEq: bop = MirBinOp::Sub; break;
                        case MirAssignOp::MulEq: bop = MirBinOp::Mul; break;
                        case MirAssignOp::DivEq: bop = MirBinOp::Div; break;
                        case MirAssignOp::RemEq: bop = MirBinOp::Rem; break;
                        case MirAssignOp::AmpEq: bop = MirBinOp::BitAnd; break;
                        case MirAssignOp::PipeEq: bop = MirBinOp::BitOr; break;
                        case MirAssignOp::CaretEq: bop = MirBinOp::BitXor; break;
                        case MirAssignOp::ShlEq: bop = MirBinOp::Shl; break;
                        case MirAssignOp::ShrEq: bop = MirBinOp::Shr; break;
                        default: bop = MirBinOp::Add; break;
                    }
                    LocalId newVal = newLocal(f.ty, "fld_wr");
                    emit(StAssign{newVal, RvBinary{bop, OpCopy{oldVal}, OpCopy{rhs}}});
                    emit(StAssign{obj, RvHStore{f.fieldOffset, OpCopy{newVal}}});
                }
            } else if (isIndex) {
                auto& ix = std::get<HirIndex>(*e.lhs);
                LocalId obj = lowerExpr(*ix.obj);
                LocalId idx = lowerExpr(*ix.idx);
                LocalId rhs = lowerExpr(*e.rhs);
                if (e.op == MirAssignOp::Set) {
                    emit(StAssign{obj, RvIndexStore{OpCopy{idx}, OpCopy{rhs}}});
                } else {
                    // Compound assignment: read the element, compute, write back
                    LocalId oldVal = newLocal(ix.ty, "elt_rd");
                    emit(StAssign{oldVal, RvIndex{obj, OpCopy{idx}}});
                    LocalId newVal = newLocal(ix.ty, "elt_wr");
                    emit(StAssign{newVal, RvBinary{binOpForAssign(e.op), OpCopy{oldVal}, OpCopy{rhs}}});
                    emit(StAssign{obj, RvIndexStore{OpCopy{idx}, OpCopy{newVal}}});
                }
            } else {
                LocalId rhs = lowerExpr(*e.rhs);
                LocalId lhs = lowerPlace(*e.lhs);
                if (e.op == MirAssignOp::Set) {
                    emit(StAssign{lhs, RvUse{OpCopy{rhs}}});
                } else {
                    MirBinOp bop;
                    switch (e.op) {
                        case MirAssignOp::AddEq: bop = MirBinOp::Add; break;
                        case MirAssignOp::SubEq: bop = MirBinOp::Sub; break;
                        case MirAssignOp::MulEq: bop = MirBinOp::Mul; break;
                        case MirAssignOp::DivEq: bop = MirBinOp::Div; break;
                        case MirAssignOp::RemEq: bop = MirBinOp::Rem; break;
                        case MirAssignOp::AmpEq: bop = MirBinOp::BitAnd; break;
                        case MirAssignOp::PipeEq: bop = MirBinOp::BitOr; break;
                        case MirAssignOp::CaretEq: bop = MirBinOp::BitXor; break;
                        case MirAssignOp::ShlEq: bop = MirBinOp::Shl; break;
                        case MirAssignOp::ShrEq: bop = MirBinOp::Shr; break;
                        default: bop = MirBinOp::Add; break;
                    }
                    emit(StAssign{lhs, RvBinary{bop, OpCopy{lhs}, OpCopy{rhs}}});
                }
            }
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirBlock>) {
            BlockId block_bb = newBlock();
            term(TmGoto{block_bb});
            m_currentBlock = block_bb;
            m_blockTerminated = false;
            for (const auto& s : e.stmts) lowerStmt(*s);
            if (e.tail) return lowerExpr(*e.tail);
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirIf>) {
            LocalId cond = lowerExpr(*e.cond);
            BlockId then_bb = newBlock();
            BlockId else_bb = newBlock();
            BlockId merge_bb = newBlock();
            term(TmSwitchInt{OpCopy{cond}, {{1, then_bb}}, else_bb});

            m_currentBlock = then_bb;
            m_blockTerminated = false;
            LocalId then_val = lowerExpr(*e.then_);
            bool thenTerminated = m_blockTerminated;
            LocalId result = newLocal(e.ty, "if_result");
            if (!thenTerminated) {
                emit(StAssign{result, RvUse{OpCopy{then_val}}});
                term(TmGoto{merge_bb});
            }

            m_currentBlock = else_bb;
            m_blockTerminated = false;
            if (e.else_) {
                LocalId else_val = lowerExpr(*e.else_);
                if (!m_blockTerminated) {
                    emit(StAssign{result, RvUse{OpCopy{else_val}}});
                    term(TmGoto{merge_bb});
                }
            } else {
                emit(StAssign{result, RvUse{OpConst{0}}});
                term(TmGoto{merge_bb});
            }

            m_currentBlock = merge_bb;
            m_blockTerminated = false;
            return result;
        } else if constexpr (std::is_same_v<T, HirWhile>) {
            BlockId header = newBlock();
            BlockId body_bb = newBlock();
            BlockId exit_bb = newBlock();
            term(TmGoto{header});

            m_currentBlock = header;
            m_blockTerminated = false;
            LocalId cond = lowerExpr(*e.cond);
            term(TmSwitchInt{OpCopy{cond}, {{1, body_bb}}, exit_bb});

            m_currentBlock = body_bb;
            m_blockTerminated = false;
            m_loopStack.push_back({exit_bb, header, e.label, m_finallyStack.size()});
            lowerExpr(*e.body);
            m_loopStack.pop_back();
            if (!m_blockTerminated) term(TmGoto{header});

            m_currentBlock = exit_bb;
            m_blockTerminated = false;
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirLoop>) {
            BlockId body_bb = newBlock();
            BlockId exit_bb = newBlock();
            term(TmGoto{body_bb});

            m_currentBlock = body_bb;
            m_blockTerminated = false;
            m_loopStack.push_back({exit_bb, body_bb, e.label, m_finallyStack.size()});
            lowerExpr(*e.body);
            m_loopStack.pop_back();
            if (!m_blockTerminated) term(TmGoto{body_bb});

            m_currentBlock = exit_bb;
            m_blockTerminated = false;
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirFor>) {
            BlockId body_bb = newBlock();
            BlockId exit_bb = newBlock();
            term(TmGoto{body_bb});

            m_currentBlock = body_bb;
            m_blockTerminated = false;
            m_loopStack.push_back({exit_bb, body_bb, e.label, m_finallyStack.size()});
            lowerExpr(*e.body);
            m_loopStack.pop_back();
            term(TmGoto{exit_bb});

            m_currentBlock = exit_bb;
            m_blockTerminated = false;
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirWhen>) {
            if (e.arms.empty()) return unitLocal();

            // Evaluate scrutinee once
            LocalId scrut = 0;
            if (e.scrut) scrut = lowerExpr(*e.scrut);

            BlockId exit_bb = newBlock();
            LocalId result = newLocal(e.ty, "when");

            // Separate else arm
            int elseIdx = -1;
            for (int i = 0; i < static_cast<int>(e.arms.size()); ++i) {
                if (e.arms[i].isElse) { elseIdx = i; break; }
            }

            // Build check/body blocks for non-else arms
            std::vector<int> checkArmIndices;
            std::vector<BlockId> checkBBs, bodyBBs;
            for (int i = 0; i < static_cast<int>(e.arms.size()); ++i) {
                if (i == elseIdx) continue;
                checkArmIndices.push_back(i);
                checkBBs.push_back(newBlock());
                bodyBBs.push_back(newBlock());
            }
            BlockId elseBB = elseIdx >= 0 ? newBlock() : exit_bb;

            // Jump from current block to first check
            if (!checkBBs.empty()) {
                if (!m_blockTerminated) term(TmGoto{checkBBs[0]});

                for (size_t ci = 0; ci < checkArmIndices.size(); ++ci) {
                    int ai = checkArmIndices[ci];
                    const auto& arm = e.arms[ai];
                    BlockId nextBB = (ci + 1 < checkBBs.size()) ? checkBBs[ci + 1] : elseBB;

                    // Check block
                    m_currentBlock = checkBBs[ci];
                    m_blockTerminated = false;

                    if (std::holds_alternative<HirExprPtr>(arm.condition)) {
                        auto condExpr = std::get<HirExprPtr>(arm.condition);
                        LocalId cv = lowerExpr(*condExpr);
                        LocalId eqTmp = newLocal(e.ty);
                        emit(StAssign{eqTmp, RvBinary{MirBinOp::Eq, OpCopy{scrut}, OpCopy{cv}}});
                        term(TmSwitchInt{OpCopy{eqTmp}, {{1, bodyBBs[ci]}}, nextBB});
                    } else if (std::holds_alternative<HirPatPtr>(arm.condition)) {
                        auto pat = std::get<HirPatPtr>(arm.condition);
                        if (pat && std::holds_alternative<HirPatEnum>(*pat)) {
                            const auto& enumPat = std::get<HirPatEnum>(*pat);
                            LocalId discr = newLocal(mkInt(), "when_discr");
                            emit(StAssign{discr, RvField{scrut, 0}});
                            LocalId eqTmp = newLocal(mkBool(), "when_match");
                            emit(StAssign{eqTmp, RvBinary{MirBinOp::Eq, OpCopy{discr}, OpConst{static_cast<int64_t>(enumPat.variantIndex)}}});
                            term(TmSwitchInt{OpCopy{eqTmp}, {{1, bodyBBs[ci]}}, nextBB});
                        } else {
                            term(TmGoto{bodyBBs[ci]});
                        }
                    } else {
                        term(TmGoto{bodyBBs[ci]});
                    }

                    // Body block
                    m_currentBlock = bodyBBs[ci];
                    m_blockTerminated = false;
                    if (std::holds_alternative<HirPatPtr>(arm.condition)) {
                        auto pat = std::get<HirPatPtr>(arm.condition);
                        if (pat && std::holds_alternative<HirPatEnum>(*pat)) {
                            const auto& enumPat = std::get<HirPatEnum>(*pat);
                            for (size_t pi = 0; pi < enumPat.elems.size(); ++pi) {
                                if (!enumPat.elems[pi]) continue;
                                if (std::holds_alternative<HirPatBind>(*enumPat.elems[pi])) {
                                    const auto& bind = std::get<HirPatBind>(*enumPat.elems[pi]);
                                    LocalId payload = newLocal(mkInt(), bind.name, bind.mut);
                                    emit(StAssign{payload, RvField{scrut, static_cast<uint32_t>((pi + 1) * 9)}});
                                    m_scope[bind.name] = payload;
                                }
                            }
                        }
                    }
                    LocalId armVal = lowerExpr(*arm.body);
                    if (!m_blockTerminated) {
                        emit(StAssign{result, RvUse{OpCopy{armVal}}});
                        term(TmGoto{exit_bb});
                    }
                }
            }

            // Else/default block
            m_currentBlock = elseBB;
            m_blockTerminated = false;
            if (elseIdx >= 0) {
                LocalId elseVal = lowerExpr(*e.arms[elseIdx].body);
                if (!m_blockTerminated) {
                    emit(StAssign{result, RvUse{OpCopy{elseVal}}});
                    term(TmGoto{exit_bb});
                }
            } else if (elseBB != exit_bb) {
                // No else arm, and elseBB was created → fall through to exit
                emit(StAssign{result, RvUse{OpConst{0}}});
                term(TmGoto{exit_bb});
            }

            m_currentBlock = exit_bb;
            m_blockTerminated = false;
            return result;
        } else if constexpr (std::is_same_v<T, HirBreak>) {
            BlockId target = 0;
            size_t finallyDepth = m_finallyStack.size();
            if (e.label) {
                for (auto it = m_loopStack.rbegin(); it != m_loopStack.rend(); ++it) {
                    if (it->label == e.label) { target = it->breakTarget; finallyDepth = it->finallyDepth; break; }
                }
            } else if (!m_loopStack.empty()) {
                target = m_loopStack.back().breakTarget;
                finallyDepth = m_loopStack.back().finallyDepth;
            }
            if (e.expr) { LocalId val = lowerExpr(*e.expr); (void)val; }
            if (lowerFinalizersTo(finallyDepth)) term(TmGoto{target});
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirContinue>) {
            BlockId target = 0;
            size_t finallyDepth = m_finallyStack.size();
            if (e.label) {
                for (auto it = m_loopStack.rbegin(); it != m_loopStack.rend(); ++it) {
                    if (it->label == e.label) { target = it->continueTarget; finallyDepth = it->finallyDepth; break; }
                }
            } else if (!m_loopStack.empty()) {
                target = m_loopStack.back().continueTarget;
                finallyDepth = m_loopStack.back().finallyDepth;
            }
            if (lowerFinalizersTo(finallyDepth)) term(TmGoto{target});
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirReturn>) {
            LocalId val = 0;
            if (e.expr) {
                val = lowerExpr(*e.expr);
            }
            if (lowerFinalizersTo(0)) {
                if (e.expr) emit(StAssign{0, RvUse{OpCopy{val}}});
                else emit(StAssign{0, RvUse{OpConst{0}}});
                term(TmGoto{m_exitBlock});
            }
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirRef>) {
            LocalId val = lowerExpr(*e.expr);
            LocalId tmp = newLocal(e.ty, "ref");
            emit(StAssign{tmp, RvRef{e.mut, val}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirDeref>) {
            LocalId val = lowerExpr(*e.expr);
            LocalId tmp = newLocal(e.ty, "deref");
            emit(StAssign{tmp, RvUse{OpCopy{val}}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirTry>) {
            return lowerExpr(*e.expr);
        } else if constexpr (std::is_same_v<T, HirThrow>) {
            LocalId value = lowerExpr(*e.expr);
            size_t runtimeHandledDepth = 0;
            for (size_t i = m_finallyHandlerActive.size(); i > 0; --i) {
                if (m_finallyHandlerActive[i - 1]) { runtimeHandledDepth = i; break; }
            }
            if (lowerFinalizersTo(runtimeHandledDepth)) {
                emit(StThrow{OpCopy{value}});
                m_blockTerminated = true;
            }
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirTryCatch>) {
            LocalId result = newLocal(e.ty, "try_result");
            LocalId exception = newLocal(mkInt(), "exception");
            BlockId handlerB = newBlock();
            BlockId normalFinallyB = newBlock();
            BlockId exceptionalFinallyB = newBlock();
            BlockId mergeB = newBlock();

            emit(StTryPush{handlerB, exception});
            if (e.finallyBody) { m_finallyStack.push_back(e.finallyBody); m_finallyHandlerActive.push_back(true); }
            LocalId bodyValue = lowerExpr(*e.body);
            if (e.finallyBody) { m_finallyStack.pop_back(); m_finallyHandlerActive.pop_back(); }
            if (!m_blockTerminated) {
                emit(StAssign{result, RvUse{OpCopy{bodyValue}}});
                emit(StTryPop{});
                term(TmGoto{normalFinallyB});
            }

            m_currentBlock = handlerB;
            m_blockTerminated = false;
            BlockId nextTest = handlerB;
            for (const auto& clause : e.catches) {
                m_currentBlock = nextTest;
                m_blockTerminated = false;
                LocalId matches = newLocal(mkBool(), "catch_match");
                emit(StAssign{matches, RvIsCheck{exception, clause.typeId}});
                BlockId catchB = newBlock();
                nextTest = newBlock();
                term(TmSwitchInt{OpCopy{matches}, {{1, catchB}}, nextTest});

                m_currentBlock = catchB;
                m_blockTerminated = false;
                auto old = m_scope.find(clause.name);
                std::optional<LocalId> oldLocal;
                if (old != m_scope.end()) oldLocal = old->second;
                m_scope[clause.name] = exception;
                if (e.finallyBody) { m_finallyStack.push_back(e.finallyBody); m_finallyHandlerActive.push_back(false); }
                LocalId catchValue = lowerExpr(*clause.body);
                if (e.finallyBody) { m_finallyStack.pop_back(); m_finallyHandlerActive.pop_back(); }
                if (oldLocal) m_scope[clause.name] = *oldLocal;
                else m_scope.erase(clause.name);
                if (!m_blockTerminated) {
                    emit(StAssign{result, RvUse{OpCopy{catchValue}}});
                    term(TmGoto{normalFinallyB});
                }
            }
            m_currentBlock = nextTest;
            m_blockTerminated = false;
            term(TmGoto{exceptionalFinallyB});

            m_currentBlock = normalFinallyB;
            m_blockTerminated = false;
            if (e.finallyBody) lowerExpr(*e.finallyBody);
            if (!m_blockTerminated) term(TmGoto{mergeB});

            m_currentBlock = exceptionalFinallyB;
            m_blockTerminated = false;
            if (e.finallyBody) lowerExpr(*e.finallyBody);
            if (!m_blockTerminated) {
                emit(StThrow{OpCopy{exception}});
                m_blockTerminated = true;
            }

            m_currentBlock = mergeB;
            m_blockTerminated = false;
            return result;
        } else if constexpr (std::is_same_v<T, HirCast>) {
            LocalId val = lowerExpr(*e.expr);
            LocalId tmp = newLocal(e.targetTy, "cast");
            emit(StAssign{tmp, RvCast{OpCopy{val}, e.targetTy}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirForceUnwrap>) {
            LocalId val = lowerExpr(*e.expr);
            BlockId okB = newBlock();
            BlockId panicB = newBlock();
            term(TmCheckNull{OpCopy{val}, panicB, okB});
            m_currentBlock = panicB; m_blockTerminated = false;
            term(TmUnreachable{});
            m_currentBlock = okB; m_blockTerminated = false;
            return val;
        } else if constexpr (std::is_same_v<T, HirElvis>) {
            LocalId lhs = lowerExpr(*e.lhs);
            LocalId result = newLocal(e.ty, "elvis");
            BlockId okB = newBlock();
            BlockId fbB = newBlock();
            BlockId mergeB = newBlock();
            term(TmCheckNull{OpCopy{lhs}, fbB, okB});
            m_currentBlock = okB; m_blockTerminated = false;
            emit(StAssign{result, RvUse{OpCopy{lhs}}});
            term(TmGoto{mergeB});
            m_currentBlock = fbB; m_blockTerminated = false;
            LocalId rhs = lowerExpr(*e.rhs);
            emit(StAssign{result, RvUse{OpCopy{rhs}}});
            term(TmGoto{mergeB});
            m_currentBlock = mergeB; m_blockTerminated = false;
            return result;
        } else if constexpr (std::is_same_v<T, HirSafeCall>) {
            LocalId recv = lowerExpr(*e.recv);
            LocalId result = newLocal(e.ty, "sc");
            BlockId callB = newBlock();
            BlockId nullB = newBlock();
            BlockId mergeB = newBlock();
            term(TmCheckNull{OpCopy{recv}, nullB, callB});
            m_currentBlock = nullB; m_blockTerminated = false;
            emit(StAssign{result, RvUse{OpConst{0}}});
            term(TmGoto{mergeB});
            m_currentBlock = callB; m_blockTerminated = false;
            std::vector<Operand> args; args.push_back(OpCopy{recv});
            for (const auto& a : e.args) { LocalId l = lowerExpr(*a); args.push_back(OpCopy{l}); }
            LocalId cd = newLocal(mkInt(), "sc_d");
            BlockId cr = newBlock();
            std::string scTarget = e.method;
            term(TmCall{OpConst{scTarget}, args, cd, cr});
            m_currentBlock = cr; m_blockTerminated = false;
            emit(StAssign{result, RvUse{OpCopy{cd}}});
            term(TmGoto{mergeB});
            m_currentBlock = mergeB; m_blockTerminated = false;
            return result;
        } else if constexpr (std::is_same_v<T, HirSafeField>) {
            LocalId recv = lowerExpr(*e.recv);
            LocalId result = newLocal(e.ty, "sf");
            BlockId okB = newBlock();
            BlockId nullB = newBlock();
            BlockId mergeB = newBlock();
            term(TmCheckNull{OpCopy{recv}, nullB, okB});
            m_currentBlock = nullB; m_blockTerminated = false;
            emit(StAssign{result, RvUse{OpConst{0}}});
            term(TmGoto{mergeB});
            m_currentBlock = okB; m_blockTerminated = false;
            emit(StAssign{result, RvField{recv, e.fieldOffset}});
            term(TmGoto{mergeB});
            m_currentBlock = mergeB; m_blockTerminated = false;
            return result;
        } else if constexpr (std::is_same_v<T, HirNull>) {
            LocalId tmp = newLocal(e.ty, "null");
            if (e.ty && e.ty->kind() == TyKind::Null)
                emit(StAssign{tmp, RvUse{OpConst{std::monostate{}}}});
            else emit(StAssign{tmp, RvUse{OpConst{0}}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirIs>) {
            LocalId val = lowerExpr(*e.expr);
            LocalId tmp = newLocal(mkBool(), "is");
            emit(StAssign{tmp, RvIsCheck{val, e.targetTypeId}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirIsNot>) {
            LocalId val = lowerExpr(*e.expr);
            LocalId checkTmp = newLocal(mkBool(), "isnot_chk");
            emit(StAssign{checkTmp, RvIsCheck{val, e.targetTypeId}});
            LocalId tmp = newLocal(mkBool(), "isnot");
            emit(StAssign{tmp, RvUnary{MirUnaryOp::Not, OpCopy{checkTmp}}});
            return tmp;
        } else {
            throw std::runtime_error("lowering received an unhandled HIR expression");
        }
    }, expr);
    m_currentLoc = std::move(previousLoc);
    return lowered;
}

void Lowering::lowerStmt(const HirStmt& stmt) {
    if (m_blockTerminated) return;
    std::visit([&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, HirStmtLet>) {
            LocalId init = lowerExpr(*s.init);
            std::visit([&](const auto& p) {
                using PT = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<PT, HirPatBind>) {
                    CoreTyPtr localTy = s.ty ? s.ty : m_body->locals[init].ty;
                    LocalId lid = newLocal(localTy, p.name, p.mut);
                    emit(StAssign{lid, RvUse{OpCopy{init}}});
                    m_scope[p.name] = lid;
                } else if constexpr (std::is_same_v<PT, HirPatWild>) {
                    // discard
                }
            }, *s.pat);
        } else if constexpr (std::is_same_v<T, HirStmtExpr>) {
            if (s.expr) { LocalId val = lowerExpr(*s.expr); (void)val; }
        }
    }, stmt);
}

} // namespace loxis::v2
