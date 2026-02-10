#ifndef LLVM_IR_LLVMCONTEXT_H
#define LLVM_IR_LLVMCONTEXT_H

#include <liric/liric_compat.h>
#include <unordered_map>

namespace llvm {

class Function;
class LLVMContext;

namespace detail {
    inline thread_local lc_module_compat_t *fallback_module = nullptr;
    inline thread_local Function *current_function = nullptr;
    inline thread_local std::unordered_map<const void *, lc_value_t *>
        value_wrappers;
    inline thread_local std::unordered_map<const lr_func_t *, Function *>
        function_wrappers;
    inline thread_local std::unordered_map<const lr_block_t *, Function *>
        block_parents;
    inline thread_local std::unordered_map<const lr_type_t *, const LLVMContext *>
        type_contexts;

    inline void register_value_wrapper(const void *obj, lc_value_t *v) {
        if (obj && v) value_wrappers[obj] = v;
    }

    inline lc_value_t *lookup_value_wrapper(const void *obj) {
        auto it = value_wrappers.find(obj);
        return it == value_wrappers.end() ? nullptr : it->second;
    }

    inline void unregister_value_wrapper(const void *obj) {
        if (obj) value_wrappers.erase(obj);
    }

    inline void register_function_wrapper(const lr_func_t *f, Function *fn) {
        if (f && fn) function_wrappers[f] = fn;
    }

    inline Function *lookup_function_wrapper(const lr_func_t *f) {
        auto it = function_wrappers.find(f);
        return it == function_wrappers.end() ? nullptr : it->second;
    }

    inline void unregister_function_wrapper(const lr_func_t *f) {
        if (f) function_wrappers.erase(f);
    }

    inline void register_block_parent(const lr_block_t *b, Function *fn) {
        if (b && fn) block_parents[b] = fn;
    }

    inline Function *lookup_block_parent(const lr_block_t *b) {
        auto it = block_parents.find(b);
        return it == block_parents.end() ? nullptr : it->second;
    }

    inline void unregister_blocks_for_function(Function *fn) {
        for (auto it = block_parents.begin(); it != block_parents.end();) {
            if (it->second == fn) {
                it = block_parents.erase(it);
            } else {
                ++it;
            }
        }
    }

    inline void register_type_context(const lr_type_t *ty,
                                      const LLVMContext *ctx) {
        if (ty && ctx) type_contexts[ty] = ctx;
    }

    inline const LLVMContext *lookup_type_context(const lr_type_t *ty) {
        auto it = type_contexts.find(ty);
        return it == type_contexts.end() ? nullptr : it->second;
    }

    inline void unregister_type_contexts(const LLVMContext *ctx) {
        if (!ctx) return;
        for (auto it = type_contexts.begin(); it != type_contexts.end();) {
            if (it->second == ctx) {
                it = type_contexts.erase(it);
            } else {
                ++it;
            }
        }
    }
}

class LLVMContext {
    lc_context_t *ctx_;
    lc_module_compat_t *default_mod_;

public:
    LLVMContext() : ctx_(lc_context_create()),
                    default_mod_(lc_module_create(ctx_, "__liric_ctx__")) {
        if (!detail::fallback_module)
            detail::fallback_module = default_mod_;
        if (default_mod_) {
            detail::register_type_context(lc_get_void_type(default_mod_), this);
            detail::register_type_context(lc_get_int_type(default_mod_, 1), this);
            detail::register_type_context(lc_get_int_type(default_mod_, 8), this);
            detail::register_type_context(lc_get_int_type(default_mod_, 16), this);
            detail::register_type_context(lc_get_int_type(default_mod_, 32), this);
            detail::register_type_context(lc_get_int_type(default_mod_, 64), this);
            detail::register_type_context(lc_get_float_type(default_mod_), this);
            detail::register_type_context(lc_get_double_type(default_mod_), this);
            detail::register_type_context(lc_get_ptr_type(default_mod_), this);
        }
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
