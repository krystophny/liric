#ifndef LLVM_IR_BASICBLOCK_H
#define LLVM_IR_BASICBLOCK_H

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Value.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include <liric/llvm_compat_c.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

namespace llvm {

class Function;
class Instruction;
class LLVMContext;
class Module;

namespace detail {

} // namespace detail

class BasicBlock : public Value {
public:
    using iterator = Instruction *;

    lr_block_t *impl_block() const {
        return lc_value_get_block(impl());
    }

    static BasicBlock *wrap(lc_value_t *v) {
        if (!v) {
            static lr_type_t poison_ty = { LR_TYPE_PTR, {} };
            static lc_value_t poison_val = { LC_VAL_CONST_UNDEF, &poison_ty, {} };
            return reinterpret_cast<BasicBlock *>(&poison_val);
        }
        return reinterpret_cast<BasicBlock *>(v);
    }

    static BasicBlock *Create(LLVMContext &Context, const Twine &Name = "",
                              Function *Parent = nullptr,
                              BasicBlock *InsertBefore = nullptr);

    Function *getParent() const {
        lr_block_t *block = impl_block();
        if (!block) return nullptr;

        Function *parent = detail::lookup_block_parent(block);
        if (parent) return parent;

        /* Recover from missing cache entries using the IR-owned block link. */
        if (block->func) {
            parent = detail::lookup_function_wrapper(block->func);
            if (parent) {
                detail::register_block_parent(block, parent);
                return parent;
            }
        }
        return nullptr;
    }
    Module *getModule() const { return nullptr; }

    bool empty() const {
        lr_block_t *b = impl_block();
        return !b || !b->first;
    }

    iterator end() { return nullptr; }
    iterator getFirstInsertionPt() { return nullptr; }

    Instruction *getTerminator() const {
        lr_block_t *b = impl_block();
        if (!b || !lc_block_has_terminator(b)) return nullptr;
        return reinterpret_cast<Instruction *>(b);
    }

    BasicBlock *getSinglePredecessor() const { return nullptr; }
    BasicBlock *getUniquePredecessor() const { return nullptr; }

    void eraseFromParent() {
        lr_block_t *block = impl_block();
        uint32_t removed_id = 0;
        if (!block) return;
        if (!lr_llvm_compat_block_erase(block, &removed_id))
            return;
        detail::unregister_block_parent(block);
        (void)removed_id;
    }

    void moveAfter(BasicBlock *other) {
        lr_block_t *block = impl_block();
        lr_block_t *anchor = other ? other->impl_block() : nullptr;
        (void)lr_llvm_compat_block_move_after(block, anchor);
    }

    void moveBefore(BasicBlock *other) {
        lr_block_t *block = impl_block();
        lr_block_t *anchor = other ? other->impl_block() : nullptr;
        (void)lr_llvm_compat_block_move_before(block, anchor);
    }
};

} // namespace llvm

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
