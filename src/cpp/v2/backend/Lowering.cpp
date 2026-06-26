#include "Lowering.hpp"
#include <cassert>

namespace loxis::v2 {

LoweringOutput Lowering::lower(const HirModule& hir) {
    LoweringOutput output;
    for (const auto& item : hir.items) {
        std::visit([&](const auto& i) {
            using T = std::decay_t<decltype(i)>;
            if constexpr (std::is_same_v<T, HirFn>) {
                output.bodies.push_back(lowerFn(i));
            } else if constexpr (std::is_same_v<T, HirClass>) {
                output.bodies.push_back(lowerClassCtor(i));
                for (const auto& m : i.methods) {
                    output.bodies.push_back(lowerMethod(i, m));
                }
                LoweredClass lc;
                lc.name = i.name;
                lc.typeId = i.typeId;
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
    body.locals.push_back({body.ret_ty, "_return", false});

    m_scope.clear();
    for (const auto& pd : cls.ctorParams) {
        LocalId lid = static_cast<LocalId>(body.locals.size());
        body.locals.push_back({pd.ty, pd.name, false});
        m_scope[pd.name] = lid;
    }

    m_body = &body;
    m_loopStack.clear();
    body.blocks.clear();

    BlockId entry = newBlock();
    m_currentBlock = entry;
    m_blockTerminated = false;
    m_exitBlock = newBlock();

    uint32_t allocSize = cls.totalFieldSize > 0 ? cls.totalFieldSize : 9;
    LocalId obj = newLocal(mkInt(), "self");
    emit(StAssign{obj, RvAlloc{allocSize, cls.typeId}});

    uint32_t fieldIdx = 0;
    for (const auto& pd : cls.ctorParams) {
        if (pd.isVal || pd.isVar) {
            auto it = m_scope.find(pd.name);
            if (it != m_scope.end()) {
                emit(StAssign{obj, RvHStore{fieldIdx * 9, OpCopy{it->second}}});
            }
            fieldIdx++;
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
    body.locals.push_back({body.ret_ty, "_return", false});

    m_scope.clear();
    LocalId selfLid = static_cast<LocalId>(body.locals.size());
    body.locals.push_back({mkInt(), "self", false});
    m_scope["self"] = selfLid;

    for (const auto& p : method.params) {
        LocalId lid = static_cast<LocalId>(body.locals.size());
        body.locals.push_back({p.second, p.first, false});
        m_scope[p.first] = lid;
    }

    m_body = &body;
    m_loopStack.clear();
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
    body.locals.push_back({body.ret_ty, "_return", false});

    m_scope.clear();
    for (const auto& p : fn.params) {
        LocalId lid = static_cast<LocalId>(body.locals.size());
        body.locals.push_back({p.second, p.first, false});
        m_scope[p.first] = lid;
    }

    m_body = &body;
    m_loopStack.clear();
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
    m_body->blocks[m_currentBlock].stmts.push_back(std::move(stmt));
}

void Lowering::term(Terminator t) {
    if (m_blockTerminated) return;
    m_body->blocks[m_currentBlock].term = std::move(t);
    m_blockTerminated = true;
}

LocalId Lowering::unitLocal() {
    LocalId tmp = newLocal(mkUnit(), "unit");
    emit(StAssign{tmp, RvUse{OpConst{0}}});
    return tmp;
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
            return lowerExpr(expr);
        }
    }, expr);
}

LocalId Lowering::lowerExpr(const HirExpr& expr) {
    if (m_blockTerminated) return unitLocal();

    return std::visit([&](const auto& e) -> LocalId {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, HirLit>) {
            LocalId tmp = newLocal(e.ty, "lit");
            emit(StAssign{tmp, RvUse{OpConst{e.val}}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirPath>) {
            auto it = m_scope.find(e.name);
            if (it != m_scope.end()) return it->second;
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
        } else if constexpr (std::is_same_v<T, HirCall>) {
            Operand funcOp = OpCopy{lowerExpr(*e.func)};
            if (!e.ctorLabel.empty()) funcOp = OpConst{e.ctorLabel};
            else if (e.staticName) funcOp = OpConst{*e.staticName};

            std::vector<Operand> args;
            for (const auto& a : e.args) {
                LocalId l = lowerExpr(*a);
                args.push_back(OpCopy{l});
            }
            LocalId dest = newLocal(e.ty, "call");

            std::vector<LocalId> saved;
            uint32_t n = static_cast<uint32_t>(m_body->locals.size());
            for (uint32_t lid = 1; lid < n; ++lid) {
                emit(StPush{static_cast<LocalId>(lid)});
                saved.push_back(static_cast<LocalId>(lid));
            }

            BlockId resume = newBlock();
            term(TmCall{funcOp, std::move(args), dest, resume});
            m_currentBlock = resume;
            m_blockTerminated = false;

            for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
                emit(StPop{*it});
            }

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

            std::vector<LocalId> saved;
            uint32_t n = static_cast<uint32_t>(m_body->locals.size());
            for (uint32_t lid = 1; lid < n; ++lid) {
                emit(StPush{static_cast<LocalId>(lid)});
                saved.push_back(static_cast<LocalId>(lid));
            }

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

            for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
                emit(StPop{*it});
            }

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
            LocalId l = lowerExpr(*e.l);
            LocalId r = lowerExpr(*e.r);
            LocalId tmp = newLocal(e.ty, "bin");
            emit(StAssign{tmp, RvBinary{e.op, OpCopy{l}, OpCopy{r}}});
            return tmp;
        } else if constexpr (std::is_same_v<T, HirAssign>) {
            // Check if LHS is a field access (needs HStore instead of StAssign)
            bool isField = (e.lhs && std::holds_alternative<HirField>(*e.lhs));
            if (isField) {
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
            m_loopStack.push_back({exit_bb, header, e.label});
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
            m_loopStack.push_back({exit_bb, body_bb, e.label});
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
            m_loopStack.push_back({exit_bb, body_bb, e.label});
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
                        // Compare scrut == cv
                        LocalId eqTmp = newLocal(e.ty);
                        emit(StAssign{eqTmp, RvBinary{MirBinOp::Eq, OpCopy{scrut}, OpCopy{cv}}});
                        term(TmSwitchInt{OpCopy{eqTmp}, {{1, bodyBBs[ci]}}, nextBB});
                    } else {
                        // Pattern or wildcard — always match for now
                        (void)scrut;
                        term(TmGoto{bodyBBs[ci]});
                    }

                    // Body block
                    m_currentBlock = bodyBBs[ci];
                    m_blockTerminated = false;
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
            if (e.label) {
                for (auto it = m_loopStack.rbegin(); it != m_loopStack.rend(); ++it) {
                    if (it->label == e.label) { target = it->breakTarget; break; }
                }
            } else if (!m_loopStack.empty()) {
                target = m_loopStack.back().breakTarget;
            }
            if (e.expr) { LocalId val = lowerExpr(*e.expr); (void)val; }
            term(TmGoto{target});
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirContinue>) {
            BlockId target = 0;
            if (e.label) {
                for (auto it = m_loopStack.rbegin(); it != m_loopStack.rend(); ++it) {
                    if (it->label == e.label) { target = it->continueTarget; break; }
                }
            } else if (!m_loopStack.empty()) {
                target = m_loopStack.back().continueTarget;
            }
            term(TmGoto{target});
            return unitLocal();
        } else if constexpr (std::is_same_v<T, HirReturn>) {
            if (e.expr) {
                LocalId val = lowerExpr(*e.expr);
                emit(StAssign{0, RvUse{OpCopy{val}}});
            } else {
                emit(StAssign{0, RvUse{OpConst{0}}});
            }
            term(TmGoto{m_exitBlock});
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
            emit(StAssign{tmp, RvUse{OpConst{0}}});
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
            return unitLocal();
        }
    }, expr);
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