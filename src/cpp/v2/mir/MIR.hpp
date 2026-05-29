#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include "../AST.hpp"
#include "../type/Type.hpp"

namespace loxis::v2 {

using LocalId = uint32_t;
using BlockId = uint32_t;

// ============================================================
// Operand (copy/move by value)
// ============================================================
struct OpCopy { LocalId local; };
struct OpMove { LocalId local; };
struct OpConst { std::variant<int64_t,uint64_t,double,bool,char,std::string> val; };

struct Operand : std::variant<OpCopy, OpMove, OpConst> {
    using variant::variant;
};

// ============================================================
// Rvalue (right-hand side of assignment)
// ============================================================
struct RvUse { Operand op; };
struct RvRef { bool mut_; LocalId local; };
struct RvPtr { bool mut_; LocalId local; };
struct RvBinary { ExprBinary::Op op; Operand l, r; };
struct RvUnary { ExprUnary::Op op; Operand opnd; };
struct RvCast { Operand op; CoreTyPtr ty; };
struct RvAggregate { enum Kind { Tuple, Array, Adt } kind; uint32_t variant = 0; std::vector<Operand> ops; };
struct RvField { LocalId local; uint32_t field; };
struct RvIndex { LocalId local; Operand idx; };
struct RvLen { LocalId local; };
struct RvDiscriminant { LocalId local; };

struct Rvalue : std::variant<
    RvUse, RvRef, RvPtr, RvBinary, RvUnary, RvCast,
    RvAggregate, RvField, RvIndex, RvLen, RvDiscriminant
> {
    using variant::variant;
};

// ============================================================
// Terminator
// ============================================================
struct TmGoto { BlockId target; };
struct TmSwitchInt { Operand discr; std::vector<std::pair<int64_t,BlockId>> values; BlockId otherwise; };
struct TmSwitchDiscr { LocalId local; std::vector<BlockId> targets; BlockId otherwise; };
struct TmReturn {};
struct TmCall { Operand func; std::vector<Operand> args; LocalId dest; BlockId return_; };
struct TmCallIndirect { Operand funcPtr; std::vector<Operand> args; LocalId dest; BlockId return_; };
struct TmCheckNull { Operand value; BlockId nullTarget; BlockId okTarget; };
struct TmUnreachable {};

struct Terminator : std::variant<
    TmGoto, TmSwitchInt, TmSwitchDiscr, TmReturn, TmCall, TmCallIndirect, TmCheckNull, TmUnreachable
> {
    using variant::variant;
};

// ============================================================
// Statement
// ============================================================
struct StAssign { LocalId place; Rvalue rv; };
struct StStorageLive { LocalId local; };
struct StStorageDead { LocalId local; };
struct StSetDiscriminant { LocalId place; uint32_t variant; };
struct StDrop { LocalId local; };
struct StPush { LocalId local; }; // push register to data stack
struct StPop  { LocalId local; }; // pop data stack to register

struct Statement : std::variant<
    StAssign, StStorageLive, StStorageDead, StSetDiscriminant, StDrop,
    StPush, StPop
> {
    using variant::variant;
};

// ============================================================
// Basic Block
// ============================================================
struct BasicBlock {
    std::vector<Statement> stmts;
    Terminator term;
};

// ============================================================
// Local decl
// ============================================================
struct LocalDecl {
    CoreTyPtr ty;
    std::string name; // for debug
    bool mut_ = false;
};

// ============================================================
// MirBody (one per function)
// ============================================================
struct MirBody {
    std::vector<LocalDecl> locals; // local 0 = return place
    std::vector<BasicBlock> blocks;
    std::string name;
    CoreTyPtr ret_ty;
    // locals[1..=n] = arguments
};

} // namespace
