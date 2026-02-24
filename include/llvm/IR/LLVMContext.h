#ifndef LLVM_IR_LLVMCONTEXT_H
#define LLVM_IR_LLVMCONTEXT_H

#include <liric/liric_compat.h>
#include <liric/llvm_compat_c.h>
#include <cstdlib>
#include <cstring>
#include <string>

namespace llvm {

class Function;
class Module;
class LLVMContext;

#if defined(__GNUC__) || defined(__clang__)
#define LIRIC_LLVM_COMPAT_HIDDEN __attribute__((visibility("hidden")))
#else
#define LIRIC_LLVM_COMPAT_HIDDEN
#endif

namespace detail {
    struct vector_type_info {
        const lr_type_t *element = nullptr;
        unsigned num_elements = 0;
        bool scalable = false;
    };

    inline lc_module_compat_t *&fallback_module_ref() {
        static thread_local lc_module_compat_t *value = nullptr;
        return value;
    }

    inline Function *&current_function_ref() {
        static thread_local Function *value = nullptr;
        return value;
    }

    inline void register_value_wrapper(const void *obj, lc_value_t *v) {
        lr_llvm_compat_register_value_wrapper(obj, v);
    }

    inline lc_value_t *lookup_value_wrapper(const void *obj) {
        return lr_llvm_compat_lookup_value_wrapper(obj);
    }

    inline void unregister_value_wrapper(const void *obj) {
        lr_llvm_compat_unregister_value_wrapper(obj);
    }

    inline void register_global_alias(const lc_module_compat_t *mod,
                                      const std::string &logical_name,
                                      const std::string &actual_name) {
        lr_llvm_compat_register_global_alias(mod, logical_name.c_str(),
                                             actual_name.c_str());
    }

    inline std::string lookup_global_alias(const lc_module_compat_t *mod,
                                           const std::string &logical_name) {
        char alias[4096];
        if (!lr_llvm_compat_lookup_global_alias(mod, logical_name.c_str(),
                                                alias, sizeof(alias), nullptr))
            return std::string();
        return std::string(alias);
    }

    inline void clear_global_aliases(const lc_module_compat_t *mod) {
        lr_llvm_compat_clear_global_aliases(mod);
    }

    inline void register_function_wrapper(const lr_func_t *f, Function *fn) {
        lr_llvm_compat_register_function_wrapper(f, fn);
    }

    inline Function *lookup_function_wrapper(const lr_func_t *f) {
        return static_cast<Function *>(
            lr_llvm_compat_lookup_function_wrapper(f));
    }

    inline void unregister_function_wrapper(const lr_func_t *f) {
        lr_llvm_compat_unregister_function_wrapper(f);
    }

    inline void register_block_parent(const lr_block_t *b, Function *fn) {
        lr_llvm_compat_register_block_parent(b, fn);
    }

    inline Function *lookup_block_parent(const lr_block_t *b) {
        return static_cast<Function *>(lr_llvm_compat_lookup_block_parent(b));
    }

    inline void unregister_block_parent(const lr_block_t *b) {
        lr_llvm_compat_unregister_block_parent(b);
    }

    inline void unregister_blocks_for_function(Function *fn) {
        lr_llvm_compat_unregister_blocks_for_function(fn);
    }

    inline void register_type_context(const lr_type_t *ty,
                                      const LLVMContext *ctx) {
        lr_llvm_compat_register_type_context(ty, ctx);
    }

    inline const LLVMContext *lookup_type_context(const lr_type_t *ty) {
        return static_cast<const LLVMContext *>(
            lr_llvm_compat_lookup_type_context(ty));
    }

    inline void register_vector_type(const lr_type_t *ty,
                                     const lr_type_t *element,
                                     unsigned num_elements,
                                     bool scalable) {
        lr_llvm_compat_register_vector_type(ty, element, num_elements,
                                            scalable ? 1 : 0);
    }

    inline const vector_type_info *lookup_vector_type(const lr_type_t *ty) {
        static thread_local vector_type_info info;
        lr_llvm_compat_vector_type_info_t cinfo;
        if (!lr_llvm_compat_lookup_vector_type(ty, &cinfo))
            return nullptr;
        info.element = cinfo.element;
        info.num_elements = cinfo.num_elements;
        info.scalable = cinfo.scalable != 0;
        return &info;
    }

    inline void unregister_type_contexts(const LLVMContext *ctx) {
        lr_llvm_compat_unregister_type_contexts(ctx);
    }
}

class LIRIC_LLVM_COMPAT_HIDDEN LLVMContext {
    lc_context_t *ctx_;
    lc_module_compat_t *default_mod_;

public:
    LLVMContext() : ctx_(lc_context_create()),
                    default_mod_(nullptr) {
        if (ctx_) {
            const char *mode = std::getenv("LIRIC_COMPILE_MODE");
            if (mode && std::strcmp(mode, "copy_patch") == 0) {
                lc_context_set_backend(ctx_, LC_BACKEND_COPY_PATCH);
            } else if (mode && std::strcmp(mode, "llvm") == 0) {
                lc_context_set_backend(ctx_, LC_BACKEND_LLVM);
            } else {
                lc_context_set_backend(ctx_, LC_BACKEND_ISEL);
            }
        }
        default_mod_ = lc_module_create(ctx_, "__liric_ctx__");
        if (!detail::fallback_module_ref())
            detail::fallback_module_ref() = default_mod_;
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
        if (detail::fallback_module_ref() == default_mod_)
            detail::fallback_module_ref() = nullptr;
        detail::unregister_type_contexts(this);
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

#undef LIRIC_LLVM_COMPAT_HIDDEN

#endif
