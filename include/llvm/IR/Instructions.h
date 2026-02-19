#ifndef LLVM_IR_INSTRUCTIONS_H
#define LLVM_IR_INSTRUCTIONS_H

#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

namespace llvm {

class AllocaInst : public UnaryInstruction {
public:
    static AllocaInst *wrap(lc_value_t *v) {
        return static_cast<AllocaInst *>(Value::wrap(v));
    }

    static bool classof(const Value *V) {
        if (!V) return false;
        lc_value_t *impl = V->impl();
        if (!impl || impl->kind != LC_VAL_VREG) return false;
        return lc_value_get_alloca_type(impl) != nullptr;
    }

    Type *getAllocatedType() const {
        lr_type_t *at = lc_value_get_alloca_type(impl());
        return at ? Type::wrap(at) : nullptr;
    }

    void setAlignment(unsigned A) { (void)A; }
    void setAlignment(uint64_t A) { (void)A; }
};

class PHINode : public Instruction {
public:
    static PHINode *wrap(lc_value_t *v) {
        return static_cast<PHINode *>(Value::wrap(v));
    }

    void addIncoming(Value *V, BasicBlock *BB) {
        lc_phi_node_t *phi = lc_value_get_phi_node(impl());
        if (phi && BB && BB->impl_block()) {
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
    lc_switch_builder_t *builder_;

public:
    explicit SwitchInst(lc_switch_builder_t *builder)
        : builder_(builder) {}

    ~SwitchInst() {
        if (builder_) {
            lc_switch_builder_destroy(builder_);
            builder_ = nullptr;
        }
    }

    void addCase(Value *on_value, BasicBlock *dest) {
        if (!builder_ || !on_value || !dest) return;
        lr_block_t *dest_block = dest->impl_block();
        if (!dest_block) return;
        (void)lc_switch_builder_add_case(builder_,
                                         on_value->impl(),
                                         dest_block);
    }
};

class SelectInst : public Instruction {};

} // namespace llvm

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
