#ifndef LLVM_IR_BASICBLOCK_H
#define LLVM_IR_BASICBLOCK_H

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Value.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

namespace llvm {

class Function;
class Instruction;
class LLVMContext;
class Module;

namespace detail {

inline lr_block_t *find_prev_block(lr_func_t *func, lr_block_t *target) {
    if (!func || !target || func->first_block == target) return nullptr;
    for (lr_block_t *b = func->first_block; b && b->next; b = b->next) {
        if (b->next == target) return b;
    }
    return nullptr;
}

inline void invalidate_function_block_caches(lr_func_t *func) {
    if (!func) return;
    func->block_array = nullptr;
    func->linear_inst_array = nullptr;
    func->block_inst_offsets = nullptr;
    func->num_linear_insts = 0;
}

inline bool function_uses_block_id(const lr_func_t *func,
                                   const lr_block_t *skip,
                                   uint32_t block_id) {
    return lc_func_uses_block_id(const_cast<lr_func_t *>(func),
                                 const_cast<lr_block_t *>(skip),
                                 block_id);
}

inline void remap_block_operands_after_erase(lr_func_t *func,
                                             uint32_t removed_id) {
    lc_func_remap_block_operands_after_erase(func, removed_id);
}

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
        if (!block || !block->func) return;

        lr_func_t *func = block->func;
        if (detail::function_uses_block_id(func, block, block->id)) {
            return;
        }

        lr_block_t *prev = detail::find_prev_block(func, block);
        if (!prev && func->first_block != block) return;

        if (prev) {
            prev->next = block->next;
        } else {
            func->first_block = block->next;
        }
        if (func->last_block == block) {
            func->last_block = prev;
        }

        const uint32_t removed_id = block->id;
        block->func = nullptr;
        block->next = nullptr;
        detail::unregister_block_parent(block);

        if (func->num_blocks > 0u) {
            func->num_blocks--;
        }
        for (lr_block_t *it = func->first_block; it; it = it->next) {
            if (it->id > removed_id) it->id--;
        }
        detail::remap_block_operands_after_erase(func, removed_id);
        if (!func->first_block) {
            func->is_decl = true;
        } else {
            func->is_decl = false;
        }
        detail::invalidate_function_block_caches(func);
    }

    void moveAfter(BasicBlock *other) {
        lr_block_t *block = impl_block();
        lr_block_t *anchor = other ? other->impl_block() : nullptr;
        if (!block || !anchor || block == anchor) return;
        if (!block->func || block->func != anchor->func) return;

        lr_func_t *func = block->func;
        if (anchor->next == block) return;

        lr_block_t *prev = detail::find_prev_block(func, block);
        if (!prev && func->first_block != block) return;

        if (prev) {
            prev->next = block->next;
        } else {
            func->first_block = block->next;
        }
        if (func->last_block == block) {
            func->last_block = prev;
        }

        block->next = anchor->next;
        anchor->next = block;
        if (func->last_block == anchor) {
            func->last_block = block;
        }
        detail::invalidate_function_block_caches(func);
    }

    void moveBefore(BasicBlock *other) {
        lr_block_t *block = impl_block();
        lr_block_t *anchor = other ? other->impl_block() : nullptr;
        if (!block || !anchor || block == anchor) return;
        if (!block->func || block->func != anchor->func) return;
        if (block->next == anchor) return;

        lr_func_t *func = block->func;
        lr_block_t *prev = detail::find_prev_block(func, block);
        if (!prev && func->first_block != block) return;

        if (prev) {
            prev->next = block->next;
        } else {
            func->first_block = block->next;
        }
        if (func->last_block == block) {
            func->last_block = prev;
        }

        lr_block_t *anchor_prev = detail::find_prev_block(func, anchor);
        if (anchor_prev) {
            anchor_prev->next = block;
        } else {
            func->first_block = block;
        }
        block->next = anchor;
        detail::invalidate_function_block_caches(func);
    }
};

} // namespace llvm

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
