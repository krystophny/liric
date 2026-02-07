#ifndef LLVM_IR_LLVMCONTEXT_H
#define LLVM_IR_LLVMCONTEXT_H

#include <liric/liric_compat.h>

namespace llvm {

class LLVMContext {
    lc_context_t *ctx_;

public:
    LLVMContext() : ctx_(lc_context_create()) {}
    ~LLVMContext() { lc_context_destroy(ctx_); }

    LLVMContext(const LLVMContext &) = delete;
    LLVMContext &operator=(const LLVMContext &) = delete;

    lc_context_t *impl() const { return ctx_; }

    static LLVMContext &getGlobal() {
        static LLVMContext global;
        return global;
    }
};

} // namespace llvm

#endif
