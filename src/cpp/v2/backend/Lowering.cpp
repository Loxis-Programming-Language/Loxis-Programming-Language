#include "Lowering.hpp"
#include <cassert>

using namespace loxis::v2;

std::vector<MirBody> Lowering::lowerModule(const Module& mod) {
    std::vector<MirBody> bodies;
    for (const auto& item : mod.items) {
        std::visit([&](const auto& i) {
            using T = std::decay_t<decltype(i)>;
            if constexpr (std::is_same_v<T, ItemFn>) {
                bodies.push_back(lowerFn(i));
            }
            // TODO: const/static global initializers
        }, *item);
    }
    return bodies;
}

MirBody Lowering::lowerFn(const ItemFn& fn) {
    MirBody body;
    body.name = fn.name;
    body.ret_ty = fn.ret ? mkI64() : mkUnit(); // stub type
    body.locals.push_back({body.ret_ty, "_return", false});

    m_scope.clear();
    for (const auto& p : fn.params) {
        CoreTyPtr pty = mkI64(); // stub
        LocalId lid = static_cast<LocalId>(body.locals.size());
        body.locals.push_back({pty, p.first, false});
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

    // Ensure exit block exists and ends with Return
    if (body.blocks.size() <= static_cast<size_t>(m_exitBlock)) {
        body.blocks.resize(m_exitBlock + 1);
    }
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

LocalId Lowering::lowerPlace(const Expr& expr) {
    return std::visit([&](const auto& e) -> LocalId {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, ExprPath>) {
            std::string name = e.path.segs.empty() ? "" : e.path.segs.back();
            auto it = m_scope.find(name);
            if (it != m_scope.end()) return it->second;
            return unitLocal();
        } else if constexpr (std::is_same_v<T, ExprDeref>) {
            return lowerExpr(*e.expr);
        } else if constexpr (std::is_same_v<T, ExprUnary>) {
            if (e.op == ExprUnary::Deref) return lowerExpr(*e.operand);
            return lowerExpr(expr);
        } else {
            return lowerExpr(expr);
        }
    }, expr);
}

LocalId Lowering::lowerExpr(const Expr& expr) {
    if (m_blockTerminated) return unitLocal();

    return std::visit([&](const auto& e) -> LocalId {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, ExprLit>) {
            LocalId tmp = newLocal(mkI64(), "lit");
            emit(StAssign{tmp, RvUse{OpConst{e.val}}});
            return tmp;
        } else if constexpr (std::is_same_v<T, ExprPath>) {
            std::string name = e.path.segs.empty() ? "" : e.path.segs.back();
            auto it = m_scope.find(name);
            if (it != m_scope.end()) return it->second;
            // Unknown path (function or global) - return unit for now
            return unitLocal();
        } else if constexpr (std::is_same_v<T, ExprTuple>) {
            std::vector<Operand> ops;
            for (const auto& elem : e.elems) {
                LocalId l = lowerExpr(*elem);
                ops.push_back(OpCopy{l});
            }
            LocalId tmp = newLocal(mkI64(), "tuple");
            emit(StAssign{tmp, RvAggregate{RvAggregate::Tuple, 0, std::move(ops)}});
            return tmp;
        } else if constexpr (std::is_same_v<T, ExprArray>) {
            std::vector<Operand> ops;
            for (const auto& elem : e.elems) {
                LocalId l = lowerExpr(*elem);
                ops.push_back(OpCopy{l});
            }
            LocalId tmp = newLocal(mkI64(), "array");
            emit(StAssign{tmp, RvAggregate{RvAggregate::Array, 0, std::move(ops)}});
            return tmp;
        } else if constexpr (std::is_same_v<T, ExprStruct>) {
            return unitLocal(); // stub
        } else if constexpr (std::is_same_v<T, ExprCall>) {
            LocalId funcLocal = lowerExpr(*e.func);
            Operand funcOp = OpCopy{funcLocal};
            if (std::holds_alternative<ExprPath>(*e.func)) {
                const auto& path = std::get<ExprPath>(*e.func);
                if (!path.path.segs.empty()) {
                    funcOp = OpConst{path.path.segs.back()};
                }
            }
            std::vector<Operand> args;
            for (const auto& a : e.args) {
                LocalId l = lowerExpr(*a);
                args.push_back(OpCopy{l});
            }
            LocalId dest = newLocal(mkI64(), "call");
            BlockId resume = newBlock();
            term(TmCall{funcOp, std::move(args), dest, resume});
            m_currentBlock = resume;
            m_blockTerminated = false;
            emit(StAssign{dest, RvUse{OpMove{0}}});
            return dest;
        } else if constexpr (std::is_same_v<T, ExprMethodCall>) {
            LocalId recv = lowerExpr(*e.recv);
            std::vector<Operand> args;
            args.push_back(OpCopy{recv});
            for (const auto& a : e.args) {
                LocalId l = lowerExpr(*a);
                args.push_back(OpCopy{l});
            }
            Operand funcOp = OpConst{e.method};
            LocalId dest = newLocal(mkI64(), "call");
            BlockId resume = newBlock();
            term(TmCall{funcOp, std::move(args), dest, resume});
            m_currentBlock = resume;
            m_blockTerminated = false;
            emit(StAssign{dest, RvUse{OpMove{0}}});
            return dest;
        } else if constexpr (std::is_same_v<T, ExprField>) {
            LocalId obj = lowerExpr(*e.obj);
            LocalId tmp = newLocal(mkI64(), "field");
            emit(StAssign{tmp, RvField{obj, 0}}); // TODO field index
            return tmp;
        } else if constexpr (std::is_same_v<T, ExprIndex>) {
            LocalId obj = lowerExpr(*e.obj);
            LocalId idx = lowerExpr(*e.idx);
            LocalId tmp = newLocal(mkI64(), "index");
            emit(StAssign{tmp, RvIndex{obj, OpCopy{idx}}});
            return tmp;
        } else if constexpr (std::is_same_v<T, ExprUnary>) {
            LocalId opnd = lowerExpr(*e.operand);
            LocalId tmp = newLocal(mkI64(), "unary");
            if (e.op == ExprUnary::Neg) {
                emit(StAssign{tmp, RvUnary{ExprUnary::Neg, OpCopy{opnd}}});
            } else if (e.op == ExprUnary::Not) {
                emit(StAssign{tmp, RvUnary{ExprUnary::Not, OpCopy{opnd}}});
            } else if (e.op == ExprUnary::Deref) {
                emit(StAssign{tmp, RvUse{OpCopy{opnd}}});
            } else if (e.op == ExprUnary::Ref || e.op == ExprUnary::RefMut) {
                emit(StAssign{tmp, RvRef{e.op == ExprUnary::RefMut, opnd}});
            } else {
                emit(StAssign{tmp, RvUse{OpCopy{opnd}}});
            }
            return tmp;
        } else if constexpr (std::is_same_v<T, ExprBinary>) {
            LocalId l = lowerExpr(*e.l);
            LocalId r = lowerExpr(*e.r);
            LocalId tmp = newLocal(mkI64(), "bin");
            emit(StAssign{tmp, RvBinary{e.op, OpCopy{l}, OpCopy{r}}});
            return tmp;
        } else if constexpr (std::is_same_v<T, ExprAssign>) {
            LocalId rhs = lowerExpr(*e.rhs);
            LocalId lhs = lowerPlace(*e.lhs);
            if (e.op == ExprAssign::Set) {
                emit(StAssign{lhs, RvUse{OpCopy{rhs}}});
            } else {
                ExprBinary::Op bop;
                switch (e.op) {
                    case ExprAssign::AddEq: bop = ExprBinary::Add; break;
                    case ExprAssign::SubEq: bop = ExprBinary::Sub; break;
                    case ExprAssign::MulEq: bop = ExprBinary::Mul; break;
                    case ExprAssign::DivEq: bop = ExprBinary::Div; break;
                    case ExprAssign::RemEq: bop = ExprBinary::Rem; break;
                    case ExprAssign::AmpEq: bop = ExprBinary::BitAnd; break;
                    case ExprAssign::PipeEq: bop = ExprBinary::BitOr; break;
                    case ExprAssign::CaretEq: bop = ExprBinary::BitXor; break;
                    case ExprAssign::ShlEq: bop = ExprBinary::Shl; break;
                    case ExprAssign::ShrEq: bop = ExprBinary::Shr; break;
                    default: bop = ExprBinary::Add; break;
                }
                emit(StAssign{lhs, RvBinary{bop, OpCopy{lhs}, OpCopy{rhs}}});
            }
            return unitLocal();
        } else if constexpr (std::is_same_v<T, ExprBlock>) {
            BlockId block_bb = newBlock();
            term(TmGoto{block_bb});
            m_currentBlock = block_bb;
            m_blockTerminated = false;
            for (const auto& s : e.stmts) {
                lowerStmt(*s);
            }
            if (e.tail) {
                return lowerExpr(*e.tail);
            } else {
                return unitLocal();
            }
        } else if constexpr (std::is_same_v<T, ExprIf>) {
            LocalId cond = lowerExpr(*e.cond);
            BlockId then_bb = newBlock();
            BlockId else_bb = newBlock();
            BlockId merge_bb = newBlock();
            term(TmSwitchInt{OpCopy{cond}, {{1, then_bb}}, else_bb});

            m_currentBlock = then_bb;
            m_blockTerminated = false;
            LocalId then_val = lowerExpr(*e.then_);
            LocalId result = newLocal(mkI64(), "if_result");
            emit(StAssign{result, RvUse{OpCopy{then_val}}});
            term(TmGoto{merge_bb});

            m_currentBlock = else_bb;
            m_blockTerminated = false;
            if (e.else_) {
                LocalId else_val = lowerExpr(*e.else_);
                emit(StAssign{result, RvUse{OpCopy{else_val}}});
            } else {
                emit(StAssign{result, RvUse{OpConst{0}}});
            }
            term(TmGoto{merge_bb});

            m_currentBlock = merge_bb;
            m_blockTerminated = false;
            return result;
        } else if constexpr (std::is_same_v<T, ExprWhile>) {
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
            if (!m_blockTerminated) {
                term(TmGoto{header});
            }

            m_currentBlock = exit_bb;
            m_blockTerminated = false;
            return unitLocal();
        } else if constexpr (std::is_same_v<T, ExprLoop>) {
            BlockId body_bb = newBlock();
            BlockId exit_bb = newBlock();
            term(TmGoto{body_bb});

            m_currentBlock = body_bb;
            m_blockTerminated = false;
            m_loopStack.push_back({exit_bb, body_bb, e.label});
            lowerExpr(*e.body);
            m_loopStack.pop_back();
            if (!m_blockTerminated) {
                term(TmGoto{body_bb});
            }

            m_currentBlock = exit_bb;
            m_blockTerminated = false;
            return unitLocal();
        } else if constexpr (std::is_same_v<T, ExprFor>) {
            // Stub: execute body once
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
        } else if constexpr (std::is_same_v<T, ExprMatch>) {
            // Stub: lower first arm if present
            if (!e.arms.empty()) {
                LocalId scrut = lowerExpr(*e.scrut);
                (void)scrut;
                return lowerExpr(*e.arms[0].second);
            }
            return unitLocal();
        } else if constexpr (std::is_same_v<T, ExprBreak>) {
            BlockId target = 0;
            if (e.label) {
                for (auto it = m_loopStack.rbegin(); it != m_loopStack.rend(); ++it) {
                    if (it->label == e.label) { target = it->breakTarget; break; }
                }
            } else if (!m_loopStack.empty()) {
                target = m_loopStack.back().breakTarget;
            }
            if (e.expr) {
                LocalId val = lowerExpr(*e.expr);
                (void)val; // TODO loop result
            }
            term(TmGoto{target});
            return unitLocal();
        } else if constexpr (std::is_same_v<T, ExprContinue>) {
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
        } else if constexpr (std::is_same_v<T, ExprReturn>) {
            if (e.expr) {
                LocalId val = lowerExpr(*e.expr);
                emit(StAssign{0, RvUse{OpCopy{val}}});
            } else {
                emit(StAssign{0, RvUse{OpConst{0}}});
            }
            term(TmGoto{m_exitBlock});
            return unitLocal();
        } else if constexpr (std::is_same_v<T, ExprClosure>) {
            return unitLocal(); // stub
        } else if constexpr (std::is_same_v<T, ExprRef>) {
            LocalId val = lowerExpr(*e.expr);
            LocalId tmp = newLocal(mkI64(), "ref");
            emit(StAssign{tmp, RvRef{e.mut, val}});
            return tmp;
        } else if constexpr (std::is_same_v<T, ExprDeref>) {
            LocalId val = lowerExpr(*e.expr);
            LocalId tmp = newLocal(mkI64(), "deref");
            emit(StAssign{tmp, RvUse{OpCopy{val}}});
            return tmp;
        } else if constexpr (std::is_same_v<T, ExprTry>) {
            LocalId val = lowerExpr(*e.expr);
            // TODO: desugar to match Ok/Err
            return val;
        } else if constexpr (std::is_same_v<T, ExprCast>) {
            LocalId val = lowerExpr(*e.expr);
            LocalId tmp = newLocal(mkI64(), "cast");
            emit(StAssign{tmp, RvCast{OpCopy{val}, mkI64()}});
            return tmp;
        } else {
            return unitLocal();
        }
    }, expr);
}

void Lowering::lowerStmt(const Stmt& stmt) {
    if (m_blockTerminated) return;
    std::visit([&](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, StmtLet>) {
            LocalId init = lowerExpr(*s.init);
            std::visit([&](const auto& p) {
                using PT = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<PT, PatBind>) {
                    LocalId lid = newLocal(mkI64(), p.name, p.mut);
                    emit(StAssign{lid, RvUse{OpCopy{init}}});
                    m_scope[p.name] = lid;
                } else if constexpr (std::is_same_v<PT, PatWild>) {
                    // discard
                } else {
                    LocalId lid = newLocal(mkI64(), "pat");
                    emit(StAssign{lid, RvUse{OpCopy{init}}});
                }
            }, *s.pat);
        } else if constexpr (std::is_same_v<T, StmtExpr>) {
            LocalId val = lowerExpr(*s.expr);
            (void)val;
        } else if constexpr (std::is_same_v<T, StmtItem>) {
            // Nested items ignored for now
        }
    }, stmt);
}
