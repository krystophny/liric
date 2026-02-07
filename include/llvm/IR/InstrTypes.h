#ifndef LLVM_IR_INSTRTYPES_H
#define LLVM_IR_INSTRTYPES_H

#include "llvm/IR/Value.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/CallingConv.h"

namespace llvm {

class Instruction : public Value {
public:
    void eraseFromParent() {}
    BasicBlock *getParent() const { return nullptr; }
};

class CmpInst : public Instruction {
public:
    enum Predicate {
        FCMP_FALSE = 0,
        FCMP_OEQ = 1,
        FCMP_OGT = 2,
        FCMP_OGE = 3,
        FCMP_OLT = 4,
        FCMP_OLE = 5,
        FCMP_ONE = 6,
        FCMP_ORD = 7,
        FCMP_UNO = 8,
        FCMP_UEQ = 9,
        FCMP_UGT = 10,
        FCMP_UGE = 11,
        FCMP_ULT = 12,
        FCMP_ULE = 13,
        FCMP_UNE = 14,
        FCMP_TRUE = 15,
        FIRST_FCMP_PREDICATE = FCMP_FALSE,
        LAST_FCMP_PREDICATE = FCMP_TRUE,

        ICMP_EQ = 32,
        ICMP_NE = 33,
        ICMP_UGT = 34,
        ICMP_UGE = 35,
        ICMP_ULT = 36,
        ICMP_ULE = 37,
        ICMP_SGT = 38,
        ICMP_SGE = 39,
        ICMP_SLT = 40,
        ICMP_SLE = 41,
        FIRST_ICMP_PREDICATE = ICMP_EQ,
        LAST_ICMP_PREDICATE = ICMP_SLE,
    };

    Predicate getPredicate() const { return ICMP_EQ; }
};

class ICmpInst : public CmpInst {};
class FCmpInst : public CmpInst {};

class CallInst : public Instruction {
public:
    void setCallingConv(CallingConv::ID CC) { (void)CC; }
    CallingConv::ID getCallingConv() const { return CallingConv::C; }

    void addAttribute(unsigned, unsigned) {}
    void addParamAttr(unsigned, unsigned) {}

    unsigned arg_size() const { return 0; }
    Value *getArgOperand(unsigned i) const { (void)i; return nullptr; }
    FunctionType *getFunctionType() const { return nullptr; }
};

class BranchInst : public Instruction {
public:
    bool isConditional() const { return false; }
    bool isUnconditional() const { return true; }
};

class ReturnInst : public Instruction {
public:
    Value *getReturnValue() const { return nullptr; }
};

class UnaryInstruction : public Instruction {};

class CastInst : public UnaryInstruction {
public:
    static bool castIsValid(unsigned, Value *, Type *) { return true; }
};

} // namespace llvm

#endif
