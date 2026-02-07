#ifndef LLVM_IR_LLVMCONTEXT_H
#define LLVM_IR_LLVMCONTEXT_H

#include <liric/liric_compat.h>

namespace llvm {

class Function;

namespace detail {
    inline thread_local lc_module_compat_t *fallback_module = nullptr;
    inline thread_local Function *current_function = nullptr;
}

class LLVMContext {
    lc_context_t *ctx_;
    lc_module_compat_t *default_mod_;

public:
    LLVMContext() : ctx_(lc_context_create()),
                    default_mod_(lc_module_create(ctx_, "__liric_ctx__")) {
        if (!detail::fallback_module)
            detail::fallback_module = default_mod_;
    }
    ~LLVMContext() {
        if (detail::fallback_module == default_mod_)
            detail::fallback_module = nullptr;
        lc_module_destroy(default_mod_);
        lc_context_destroy(ctx_);
    }

    LLVMContext(const LLVMContext &) = delete;
    LLVMContext &operator=(const LLVMContext &) = delete;

    lc_context_t *impl() const { return ctx_; }
    lc_module_compat_t *getDefaultModule() const { return default_mod_; }

    static LLVMContext &getGlobal() {
        static LLVMContext global;
        return global;
    }
};

} // namespace llvm

#endif
