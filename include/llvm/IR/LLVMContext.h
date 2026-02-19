#ifndef LLVM_IR_LLVMCONTEXT_H
#define LLVM_IR_LLVMCONTEXT_H

#include <liric/liric_compat.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

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

    inline std::unordered_map<const void *, lc_value_t *> &value_wrappers_ref() {
        static thread_local std::unordered_map<const void *, lc_value_t *> value;
        return value;
    }

    inline std::unordered_map<const lr_func_t *, Function *> &function_wrappers_ref() {
        static thread_local std::unordered_map<const lr_func_t *, Function *> value;
        return value;
    }

    inline std::unordered_map<const lr_block_t *, Function *> &block_parents_ref() {
        static thread_local std::unordered_map<const lr_block_t *, Function *> value;
        return value;
    }

    inline std::unordered_map<const lr_type_t *, const LLVMContext *> &type_contexts_ref() {
        static thread_local std::unordered_map<const lr_type_t *, const LLVMContext *> value;
        return value;
    }

    inline std::unordered_map<const lr_type_t *, vector_type_info> &vector_types_ref() {
        static thread_local std::unordered_map<const lr_type_t *, vector_type_info> value;
        return value;
    }

    inline std::unordered_map<const lc_module_compat_t *,
        std::unordered_map<std::string, std::string>> &global_aliases_ref() {
        static thread_local std::unordered_map<const lc_module_compat_t *,
            std::unordered_map<std::string, std::string>> value;
        return value;
    }

    inline void register_value_wrapper(const void *obj, lc_value_t *v) {
        if (obj && v) value_wrappers_ref()[obj] = v;
    }

    inline lc_value_t *lookup_value_wrapper(const void *obj) {
        auto &value_wrappers = value_wrappers_ref();
        auto it = value_wrappers.find(obj);
        return it == value_wrappers.end() ? nullptr : it->second;
    }

    inline void unregister_value_wrapper(const void *obj) {
        if (obj) value_wrappers_ref().erase(obj);
    }

    inline void register_global_alias(const lc_module_compat_t *mod,
                                      const std::string &logical_name,
                                      const std::string &actual_name) {
        if (!mod || logical_name.empty() || actual_name.empty())
            return;
        global_aliases_ref()[mod][logical_name] = actual_name;
    }

    inline std::string lookup_global_alias(const lc_module_compat_t *mod,
                                           const std::string &logical_name) {
        if (!mod || logical_name.empty())
            return std::string();
        auto &global_aliases = global_aliases_ref();
        auto mit = global_aliases.find(mod);
        if (mit == global_aliases.end())
            return std::string();
        auto ait = mit->second.find(logical_name);
        if (ait == mit->second.end())
            return std::string();
        return ait->second;
    }

    inline void clear_global_aliases(const lc_module_compat_t *mod) {
        if (mod)
            global_aliases_ref().erase(mod);
    }

    inline void register_function_wrapper(const lr_func_t *f, Function *fn) {
        if (f && fn) function_wrappers_ref()[f] = fn;
    }

    inline Function *lookup_function_wrapper(const lr_func_t *f) {
        auto &function_wrappers = function_wrappers_ref();
        auto it = function_wrappers.find(f);
        return it == function_wrappers.end() ? nullptr : it->second;
    }

    inline void unregister_function_wrapper(const lr_func_t *f) {
        if (f) function_wrappers_ref().erase(f);
    }

    inline void register_block_parent(const lr_block_t *b, Function *fn) {
        if (b && fn) block_parents_ref()[b] = fn;
    }

    inline Function *lookup_block_parent(const lr_block_t *b) {
        auto &block_parents = block_parents_ref();
        auto it = block_parents.find(b);
        return it == block_parents.end() ? nullptr : it->second;
    }

    inline void unregister_block_parent(const lr_block_t *b) {
        if (b) block_parents_ref().erase(b);
    }

    inline void unregister_blocks_for_function(Function *fn) {
        auto &block_parents = block_parents_ref();
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
        if (ty && ctx) type_contexts_ref()[ty] = ctx;
    }

    inline const LLVMContext *lookup_type_context(const lr_type_t *ty) {
        auto &type_contexts = type_contexts_ref();
        auto it = type_contexts.find(ty);
        return it == type_contexts.end() ? nullptr : it->second;
    }

    inline void register_vector_type(const lr_type_t *ty,
                                     const lr_type_t *element,
                                     unsigned num_elements,
                                     bool scalable) {
        if (!ty || !element || num_elements == 0) return;
        vector_types_ref()[ty] = vector_type_info{element, num_elements, scalable};
    }

    inline const vector_type_info *lookup_vector_type(const lr_type_t *ty) {
        auto &vector_types = vector_types_ref();
        auto it = vector_types.find(ty);
        return it == vector_types.end() ? nullptr : &it->second;
    }

    inline void unregister_type_contexts(const LLVMContext *ctx) {
        if (!ctx) return;
        auto &type_contexts = type_contexts_ref();
        auto &vector_types = vector_types_ref();
        for (auto it = type_contexts.begin(); it != type_contexts.end();) {
            if (it->second == ctx) {
                vector_types.erase(it->first);
                it = type_contexts.erase(it);
            } else {
                ++it;
            }
        }
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
