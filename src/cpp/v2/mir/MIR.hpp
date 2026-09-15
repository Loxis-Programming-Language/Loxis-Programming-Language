#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include "../Token.hpp"
#include "../type/Type.hpp"

namespace loxis::v2 {

using LocalId = uint32_t;
using BlockId = uint32_t;

// ============================================================
// MIR-level operators (decoupled from AST)
// ============================================================
enum class MirBinOp {
    Add, Sub, Mul, Div, Rem, Shl, Shr, BitAnd, BitOr, BitXor,
    And, Or, Eq, Ne, Lt, Le, Gt, Ge
};

enum class MirUnaryOp { Neg, Not, Deref, Ref, RefMut, ToStr };

enum class MirAssignOp {
    Set, AddEq, SubEq, MulEq, DivEq, RemEq, AmpEq, PipeEq, CaretEq, ShlEq, ShrEq
};

// ============================================================
// Operand (copy/move by value)
// ============================================================
struct OpCopy { LocalId local; };
struct OpMove { LocalId local; };
struct OpConst { std::variant<int64_t,uint64_t,double,bool,char,std::string,std::monostate> val; };
struct OpLabel { std::string label; };

struct Operand : std::variant<OpCopy, OpMove, OpConst, OpLabel> {
    using variant::variant;
};

// ============================================================
// Rvalue (right-hand side of assignment)
// ============================================================
struct RvUse { Operand op; };
struct RvRef { bool mut_; LocalId local; };
struct RvPtr { bool mut_; LocalId local; };
struct RvBinary { MirBinOp op; Operand l, r; };
struct RvUnary { MirUnaryOp op; Operand opnd; };
struct RvCast { Operand op; CoreTyPtr ty; };
struct RvAggregate { enum Kind { Tuple, Array, Adt } kind; uint32_t variant = 0; std::vector<Operand> ops; };
struct RvField { LocalId local; uint32_t field; };
struct RvHStore { uint32_t offset; Operand value; };
struct RvIndex { LocalId local; Operand idx; };
struct RvLen { LocalId local; };
struct RvDiscriminant { LocalId local; };
struct RvAlloc { uint32_t size; uint32_t typeId; };
struct RvClosure { std::string functionName; uint32_t envTypeId; std::vector<Operand> captures; };
struct RvClosureGet { LocalId env; uint32_t offset; };
struct RvVTableSlot { LocalId obj; uint32_t slot; };
struct RvTypeId { LocalId obj; };
struct RvIsCheck { LocalId obj; uint32_t targetTypeId; };
struct RvGlobalLoad { uint32_t slot; };

struct Rvalue : std::variant<
    RvUse, RvRef, RvPtr, RvBinary, RvUnary, RvCast,
    RvAggregate, RvField, RvHStore, RvIndex, RvLen, RvDiscriminant,
    RvAlloc, RvClosure, RvClosureGet, RvVTableSlot, RvTypeId, RvIsCheck,
    RvGlobalLoad
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
struct TmClosureCall { Operand closure; std::vector<Operand> args; LocalId dest; BlockId return_; };
struct TmCheckNull { Operand value; BlockId nullTarget; BlockId okTarget; };
struct TmUnreachable {};
struct TmVirtualCall { LocalId recv; uint32_t vtableSlot; std::vector<Operand> args; LocalId dest; BlockId return_; };
struct TmInterfaceCall { LocalId recv; uint32_t ifaceTypeId; uint32_t methodIdx; std::vector<Operand> args; LocalId dest; BlockId return_; };

struct Terminator : std::variant<
    TmGoto, TmSwitchInt, TmSwitchDiscr, TmReturn, TmCall, TmCallIndirect, TmClosureCall, TmCheckNull, TmUnreachable,
    TmVirtualCall, TmInterfaceCall
> {
    using variant::variant;
    SourceLoc loc;
};

// ============================================================
// Statement
// ============================================================
struct StAssign { LocalId place; Rvalue rv; };
struct StStorageLive { LocalId local; };
struct StStorageDead { LocalId local; };
struct StSetDiscriminant { LocalId place; uint32_t variant; };
struct StDrop { LocalId local; };
struct StPush { LocalId local; };
struct StPop  { LocalId local; };
struct StGlobalStore { uint32_t slot; Operand value; };
struct StTryPush { BlockId handler; LocalId exceptionLocal; };
struct StTryPop {};
struct StThrow { Operand value; };

struct Statement : std::variant<
    StAssign, StStorageLive, StStorageDead, StSetDiscriminant, StDrop,
    StPush, StPop, StGlobalStore, StTryPush, StTryPop, StThrow
> {
    using variant::variant;
    SourceLoc loc;
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
    std::string name;
    bool mut_ = false;
};

// ============================================================
// MirBody (one per function)
// ============================================================
struct MirBody {
    std::vector<LocalDecl> locals;
    std::vector<BasicBlock> blocks;
    std::string name;
    CoreTyPtr ret_ty;
    uint32_t paramCount = 0;
};

} // namespace
