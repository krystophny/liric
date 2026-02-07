#ifndef LLVM_IR_INSTRUCTIONS_H
#define LLVM_IR_INSTRUCTIONS_H

#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"

namespace llvm {

class AllocaInst : public UnaryInstruction {
    lc_alloca_inst_t *alloca_impl_;
    Type *alloc_type_;

public:
    AllocaInst() : alloca_impl_(nullptr), alloc_type_(nullptr) {}

    void setAllocaImpl(lc_alloca_inst_t *ai) {
        alloca_impl_ = ai;
        if (ai) alloc_type_ = Type::wrap(ai->alloc_type);
    }

    lc_alloca_inst_t *getAllocaImpl() const { return alloca_impl_; }
    Type *getAllocatedType() const { return alloc_type_; }

    void setAlignment(unsigned A) { (void)A; }
    void setAlignment(uint64_t A) { (void)A; }
};

class PHINode : public Instruction {
public:
    static PHINode *wrap(lc_value_t *v) {
        return reinterpret_cast<PHINode *>(v);
    }

    void addIncoming(Value *V, BasicBlock *BB) {
        lc_phi_node_t *phi = lc_value_get_phi_node(impl());
        if (phi) {
            lc_phi_add_incoming(phi, V->impl(), BB->impl_block());
        }
    }

    void finalize() {
        lc_phi_node_t *phi = lc_value_get_phi_node(impl());
        if (phi) lc_phi_finalize(phi);
    }

    unsigned getNumIncomingValues() const {
        lc_phi_node_t *phi = lc_value_get_phi_node(
            const_cast<lc_value_t *>(
                reinterpret_cast<const lc_value_t *>(this)));
        return phi ? phi->num_incoming : 0;
    }
};

class LoadInst : public UnaryInstruction {
public:
    void setAlignment(unsigned A) { (void)A; }
    void setAlignment(uint64_t A) { (void)A; }
    void setVolatile(bool V) { (void)V; }
};

class StoreInst : public Instruction {
public:
    void setAlignment(unsigned A) { (void)A; }
    void setAlignment(uint64_t A) { (void)A; }
    void setVolatile(bool V) { (void)V; }
};

class GetElementPtrInst : public Instruction {
public:
    static GetElementPtrInst *Create(Type *, Value *, ArrayRef<Value *>,
                                     const Twine & = "") {
        return nullptr;
    }
};

class SwitchInst : public Instruction {
public:
    void addCase(Value *, BasicBlock *) {}
};

class SelectInst : public Instruction {};

} // namespace llvm

#endif
