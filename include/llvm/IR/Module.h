#ifndef LLVM_IR_MODULE_H
#define LLVM_IR_MODULE_H

#include <liric/liric_compat.h>
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Target/TargetMachine.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <cstdio>

namespace llvm {

#if defined(__GNUC__) || defined(__clang__)
#define LIRIC_LLVM_COMPAT_HIDDEN __attribute__((visibility("hidden")))
#else
#define LIRIC_LLVM_COMPAT_HIDDEN
#endif

class LIRIC_LLVM_COMPAT_HIDDEN Module {
    lc_module_compat_t *compat_;
    LLVMContext &ctx_;
    std::string name_;
    std::vector<std::unique_ptr<Function>> owned_functions_;
    std::vector<std::unique_ptr<GlobalVariable>> owned_globals_;

    static inline thread_local lc_module_compat_t *current_ = nullptr;

public:
    Module(StringRef name, LLVMContext &ctx)
        : ctx_(ctx), name_(name.str()) {
        compat_ = lc_module_create(ctx.impl(), name_.c_str());
        current_ = compat_;
        detail::fallback_module = compat_;
    }

    ~Module() {
        for (auto &og : owned_globals_) {
            detail::unregister_value_wrapper(og.get());
        }
        for (auto &of : owned_functions_) {
            Function *fn = of.get();
            if (detail::current_function == fn) detail::current_function = nullptr;
            detail::unregister_blocks_for_function(fn);
            detail::unregister_value_wrapper(fn);
            detail::unregister_function_wrapper(fn->getIRFunc());
        }
        if (current_ == compat_) current_ = nullptr;
        if (detail::fallback_module == compat_)
            detail::fallback_module = ctx_.getDefaultModule();
        detail::clear_global_aliases(compat_);
        lc_module_destroy(compat_);
    }

    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;

    static lc_module_compat_t *getCurrentModule() {
        if (current_) return current_;
        if (detail::fallback_module) return detail::fallback_module;
        return LLVMContext::getGlobal().getDefaultModule();
    }
    static void setCurrentModule(lc_module_compat_t *m) { current_ = m; }

    static bool isLocalGlobalLinkage(GlobalValue::LinkageTypes linkage) {
        return linkage == GlobalValue::InternalLinkage ||
               linkage == GlobalValue::PrivateLinkage;
    }

    static std::string linkageScopedGlobalName(
        lc_module_compat_t *compat, StringRef name,
        GlobalValue::LinkageTypes linkage) {
        std::string base = name.str();
        if (!compat || base.empty() || !isLocalGlobalLinkage(linkage))
            return base;
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "%llx",
                      (unsigned long long)(uintptr_t)compat);
        base += ".__liric_local.";
        base += suffix;
        return base;
    }

    lc_module_compat_t *getCompat() const { return compat_; }
    lr_module_t *getIR() const { return lc_module_get_ir(compat_); }
    LLVMContext &getContext() { return ctx_; }
    const LLVMContext &getContext() const { return ctx_; }
    StringRef getName() const { return name_; }

    void setDataLayout(StringRef DL) { (void)DL; }
    void setDataLayout(const class DataLayout &DL) { (void)DL; }
    const class DataLayout &getDataLayout() const {
        static DataLayout dl;
        return dl;
    }

    void setTargetTriple(StringRef Triple) { (void)Triple; }
    StringRef getTargetTriple() const { return ""; }

    Function *getFunction(StringRef Name) const {
        lr_module_t *m = lc_module_get_ir(compat_);
        for (lr_func_t *f = m->first_func; f; f = f->next) {
            if (Name.equals(f->name)) {
                for (auto &of : owned_functions_) {
                    lr_func_t *irf = of->getIRFunc();
                    if (irf == f) return of.get();
                }
                lc_value_t *fv = lc_func_declare(compat_, f->name, f->type);
                if (!fv)
                    return nullptr;
                auto fn = std::make_unique<Function>();
                fn->setFuncVal(fv);
                fn->setCompatMod(compat_);
                Function *ptr = fn.get();
                detail::register_value_wrapper(ptr, fv);
                const_cast<Module *>(this)->owned_functions_.push_back(std::move(fn));
                detail::register_function_wrapper(ptr->getIRFunc(), ptr);
                return ptr;
            }
        }
        return nullptr;
    }

    GlobalVariable *getGlobalVariable(StringRef Name, bool AllowInternal = false) const {
        (void)AllowInternal;
        Module::setCurrentModule(compat_);
        std::string global_name = Name.str();
        std::string alias_name = detail::lookup_global_alias(compat_, global_name);
        if (!alias_name.empty())
            global_name = alias_name;

        for (const auto &og : owned_globals_) {
            lc_value_t *gv = detail::lookup_value_wrapper(og.get());
            if (!gv || gv->kind != LC_VAL_GLOBAL || !gv->global.name)
                continue;
            if (global_name == gv->global.name)
                return og.get();
        }

        lc_value_t *gv = lc_global_lookup(compat_, global_name.c_str());
        if (!gv)
            return nullptr;

        auto g = std::make_unique<GlobalVariable>();
        GlobalVariable *ptr = g.get();
        detail::register_value_wrapper(ptr, gv);
        const_cast<Module *>(this)->owned_globals_.push_back(std::move(g));
        return ptr;
    }

    GlobalVariable *getNamedGlobal(StringRef Name) const {
        return getGlobalVariable(Name);
    }

    Constant *getOrInsertGlobal(StringRef Name, Type *Ty) {
        std::string global_name = Name.str();
        lc_value_t *gv = lc_global_lookup_or_create(compat_, global_name.c_str(),
                                                    Ty->impl());
        return static_cast<Constant *>(Value::wrap(gv));
    }

    void print(raw_ostream &OS, void * = nullptr) const {
        FILE *f = OS.getFileOrNull();
        if (f) {
            lc_module_print(compat_, f);
        } else {
            size_t len = 0;
            char *buf = lc_module_sprint(compat_, &len);
            if (buf) {
                OS.write(buf, len);
                free(buf);
            }
        }
    }

    void dump() const { lc_module_dump(compat_); }

    using iterator = Function **;

    void addModuleFlag(unsigned, StringRef, unsigned) {}
    void addModuleFlag(unsigned, StringRef, Value *) {}

    class FunctionListType {
    public:
        unsigned size() const { return 0; }
    };
    FunctionListType &getFunctionList() {
        static FunctionListType fl;
        return fl;
    }

    Function *createFunction(const char *name, FunctionType *fty,
                             bool is_decl) {
        lc_value_t *fv;
        if (is_decl) {
            fv = lc_func_declare(compat_, name, fty->impl());
        } else {
            fv = lc_func_create(compat_, name, fty->impl());
        }
        auto fn = std::make_unique<Function>();
        fn->setFuncVal(fv);
        fn->setCompatMod(compat_);
        Function *ptr = fn.get();
        detail::register_value_wrapper(ptr, fv);
        owned_functions_.push_back(std::move(fn));
        detail::register_function_wrapper(ptr->getIRFunc(), ptr);
        if (!is_decl) detail::current_function = ptr;
        return ptr;
    }

    GlobalVariable *createGlobalVariable(
        const char *name, Type *ty, bool is_const,
        GlobalValue::LinkageTypes linkage,
        const void *init_data = nullptr, size_t init_size = 0) {
        Module::setCurrentModule(compat_);
        std::string requested_name = name ? name : "";
        std::string actual_name =
            linkageScopedGlobalName(compat_, requested_name, linkage);
        const char *symbol_name = actual_name.empty() ? name : actual_name.c_str();
        if (!symbol_name)
            symbol_name = "";
        lc_value_t *gv;
        bool must_define = (init_data && init_size > 0) ||
                           isLocalGlobalLinkage(linkage);
        if (must_define) {
            gv = lc_global_create(compat_, symbol_name, ty->impl(), is_const,
                                  init_data, init_size);
        } else {
            gv = lc_global_declare(compat_, symbol_name, ty->impl());
        }
        auto g = std::make_unique<GlobalVariable>();
        (void)gv;
        GlobalVariable *ptr = g.get();
        detail::register_value_wrapper(ptr, gv);
        ptr->setLinkage(linkage);
        std::string final_name = actual_name;
        if (gv && gv->kind == LC_VAL_GLOBAL && gv->global.name)
            final_name = gv->global.name;
        if (!requested_name.empty() && requested_name != final_name) {
            detail::register_global_alias(compat_, requested_name, final_name);
        }
        owned_globals_.push_back(std::move(g));
        return ptr;
    }
};

#undef LIRIC_LLVM_COMPAT_HIDDEN

inline lc_module_compat_t *liric_get_current_module() {
    return Module::getCurrentModule();
}

inline Type *Type::getVoidTy(LLVMContext &C) {
    lc_module_compat_t *mod = C.getDefaultModule();
    Type *ty = mod ? Type::wrap(lc_get_void_type(mod)) : nullptr;
    if (ty) detail::register_type_context(ty->impl(), &C);
    return ty;
}
inline Type *Type::getFloatTy(LLVMContext &C) {
    lc_module_compat_t *mod = C.getDefaultModule();
    Type *ty = mod ? Type::wrap(lc_get_float_type(mod)) : nullptr;
    if (ty) detail::register_type_context(ty->impl(), &C);
    return ty;
}
inline Type *Type::getDoubleTy(LLVMContext &C) {
    lc_module_compat_t *mod = C.getDefaultModule();
    Type *ty = mod ? Type::wrap(lc_get_double_type(mod)) : nullptr;
    if (ty) detail::register_type_context(ty->impl(), &C);
    return ty;
}
inline IntegerType *Type::getInt1Ty(LLVMContext &C) {
    lc_module_compat_t *mod = C.getDefaultModule();
    IntegerType *ty = mod ? IntegerType::wrap(lc_get_int_type(mod, 1)) : nullptr;
    if (ty) detail::register_type_context(ty->impl(), &C);
    return ty;
}
inline IntegerType *Type::getInt8Ty(LLVMContext &C) {
    lc_module_compat_t *mod = C.getDefaultModule();
    IntegerType *ty = mod ? IntegerType::wrap(lc_get_int_type(mod, 8)) : nullptr;
    if (ty) detail::register_type_context(ty->impl(), &C);
    return ty;
}
inline IntegerType *Type::getInt16Ty(LLVMContext &C) {
    lc_module_compat_t *mod = C.getDefaultModule();
    IntegerType *ty = mod ? IntegerType::wrap(lc_get_int_type(mod, 16)) : nullptr;
    if (ty) detail::register_type_context(ty->impl(), &C);
    return ty;
}
inline IntegerType *Type::getInt32Ty(LLVMContext &C) {
    lc_module_compat_t *mod = C.getDefaultModule();
    IntegerType *ty = mod ? IntegerType::wrap(lc_get_int_type(mod, 32)) : nullptr;
    if (ty) detail::register_type_context(ty->impl(), &C);
    return ty;
}
inline IntegerType *Type::getInt64Ty(LLVMContext &C) {
    lc_module_compat_t *mod = C.getDefaultModule();
    IntegerType *ty = mod ? IntegerType::wrap(lc_get_int_type(mod, 64)) : nullptr;
    if (ty) detail::register_type_context(ty->impl(), &C);
    return ty;
}

inline void Type::print(raw_ostream &OS, bool IsForDebug) const {
    (void)IsForDebug;
    lr_type_t *t = impl();
    switch (t->kind) {
    case LR_TYPE_VOID:   OS << "void"; break;
    case LR_TYPE_I1:     OS << "i1"; break;
    case LR_TYPE_I8:     OS << "i8"; break;
    case LR_TYPE_I16:    OS << "i16"; break;
    case LR_TYPE_I32:    OS << "i32"; break;
    case LR_TYPE_I64:    OS << "i64"; break;
    case LR_TYPE_FLOAT:  OS << "float"; break;
    case LR_TYPE_DOUBLE: OS << "double"; break;
    case LR_TYPE_PTR:    OS << "ptr"; break;
    default:             OS << "type"; break;
    }
}

inline Type *Type::getPointerElementType() const {
    LLVMContext &ctx = getContext();
    lc_module_compat_t *mod = ctx.getDefaultModule();
    Type *ty = mod ? Type::wrap(lc_get_ptr_type(mod)) : nullptr;
    if (ty) detail::register_type_context(ty->impl(), &ctx);
    return ty;
}

inline PointerType *Type::getInt8PtrTy(LLVMContext &C, unsigned AS) {
    (void)AS;
    return PointerType::get(C, 0);
}

inline IntegerType *Type::getIntNTy(LLVMContext &C, unsigned N) {
    return IntegerType::get(C, N);
}

inline PointerType *Type::getPointerTo(unsigned AddrSpace) const {
    (void)AddrSpace;
    return PointerType::get(getContext(), 0);
}

inline LLVMContext &Type::getContext() const {
    if (const LLVMContext *ctx = detail::lookup_type_context(impl()))
        return *const_cast<LLVMContext *>(ctx);
    return LLVMContext::getGlobal();
}

inline IntegerType *IntegerType::get(LLVMContext &C, unsigned NumBits) {
    lc_module_compat_t *mod = C.getDefaultModule();
    IntegerType *ty = mod ? IntegerType::wrap(lc_get_int_type(mod, NumBits)) : nullptr;
    if (ty) detail::register_type_context(ty->impl(), &C);
    return ty;
}

inline FunctionType *FunctionType::get(Type *Result, ArrayRef<Type *> Params,
                                       bool isVarArg) {
    lc_module_compat_t *mod = Module::getCurrentModule();
    if (!mod) return nullptr;
    std::vector<lr_type_t *> param_types(Params.size());
    for (size_t i = 0; i < Params.size(); i++) {
        param_types[i] = Params[i]->impl();
    }
    lr_type_t *ft = lr_type_func_new(
        lc_module_get_ir(mod), Result->impl(),
        Params.empty() ? nullptr : param_types.data(),
        static_cast<uint32_t>(Params.size()), isVarArg);
    detail::register_type_context(ft, &Result->getContext());
    return FunctionType::wrap(ft);
}

inline FunctionType *FunctionType::get(Type *Result, bool isVarArg) {
    return FunctionType::get(Result, ArrayRef<Type *>(), isVarArg);
}

inline void StructType::setBody(ArrayRef<Type *> Elements, bool isPkd) {
    lr_type_t *self = impl();
    lc_module_compat_t *mod = Module::getCurrentModule();
    if (!mod) return;
    lr_module_t *m = lc_module_get_ir(mod);
    uint32_t n = static_cast<uint32_t>(Elements.size());
    lr_type_t **fields = static_cast<lr_type_t **>(
        lr_arena_alloc(m->arena, sizeof(lr_type_t *) * n, alignof(lr_type_t *)));
    for (uint32_t i = 0; i < n; i++) {
        fields[i] = Elements[i]->impl();
    }
    self->struc.fields = fields;
    self->struc.num_fields = n;
    self->struc.packed = isPkd;
}

inline StructType *StructType::create(LLVMContext &C, StringRef Name) {
    lc_module_compat_t *mod = Module::getCurrentModule();
    if (!mod) return nullptr;
    lr_module_t *m = lc_module_get_ir(mod);
    char *name_dup = lr_arena_strdup(m->arena, Name.data(), Name.size());
    lr_type_t *st = lr_type_struct_new(m, nullptr, 0, false);
    st->struc.name = name_dup;
    detail::register_type_context(st, &C);
    return StructType::wrap(st);
}

inline StructType *StructType::create(LLVMContext &C, ArrayRef<Type *> Elements,
                                       StringRef Name, bool isPkd) {
    StructType *st = create(C, Name);
    if (st && !Elements.empty()) {
        st->setBody(Elements, isPkd);
    }
    return st;
}

inline StructType *StructType::get(LLVMContext &C, ArrayRef<Type *> Elements,
                                    bool isPkd) {
    lc_module_compat_t *mod = Module::getCurrentModule();
    if (!mod) return nullptr;
    lr_module_t *m = lc_module_get_ir(mod);
    uint32_t n = static_cast<uint32_t>(Elements.size());
    std::vector<lr_type_t *> fields(n);
    for (uint32_t i = 0; i < n; i++) {
        fields[i] = Elements[i]->impl();
    }
    lr_type_t *st = lr_type_struct_new(m,
                                        fields.empty() ? nullptr : fields.data(),
                                        n, isPkd);
    detail::register_type_context(st, &C);
    return StructType::wrap(st);
}

inline ArrayType *ArrayType::get(Type *ElementType, uint64_t NumElements) {
    lc_module_compat_t *mod = Module::getCurrentModule();
    if (!mod) return nullptr;
    lr_type_t *at = lr_type_array_new(lc_module_get_ir(mod),
                                       ElementType->impl(), NumElements);
    detail::register_type_context(at, &ElementType->getContext());
    return ArrayType::wrap(at);
}

inline PointerType *PointerType::get(LLVMContext &C, unsigned AddressSpace) {
    (void)AddressSpace;
    lc_module_compat_t *mod = C.getDefaultModule();
    PointerType *ty = mod ? PointerType::wrap(lc_get_ptr_type(mod)) : nullptr;
    if (ty) detail::register_type_context(ty->impl(), &C);
    return ty;
}

inline PointerType *PointerType::getUnqual(Type *ElementType) {
    if (ElementType) return PointerType::get(ElementType->getContext(), 0);
    return PointerType::get(LLVMContext::getGlobal(), 0);
}

inline PointerType *PointerType::getUnqual(LLVMContext &C) {
    return PointerType::get(C, 0);
}

inline GlobalVariable::GlobalVariable(Module &M, Type *Ty, bool isConstant,
                                       LinkageTypes Linkage,
                                       Constant *Initializer,
                                       const Twine &Name,
                                       GlobalVariable *InsertBefore,
                                       ThreadLocalMode TLMode,
                                       unsigned AddressSpace) {
    (void)InsertBefore; (void)TLMode; (void)AddressSpace;
    GlobalVariable *created =
        M.createGlobalVariable(Name.c_str(), Ty, isConstant, Linkage);
    if (created) {
        detail::register_value_wrapper(this,
            detail::lookup_value_wrapper(created));
        setLinkage(Linkage);
        if (Initializer) {
            Module::setCurrentModule(M.getCompat());
            setInitializer(Initializer);
        }
    }
}

inline bool GlobalVariable::isConstant() const {
    return false;
}

inline void GlobalVariable::setConstant(bool v) {
    (void)v;
}

inline bool GlobalVariable::hasInitializer() const {
    lc_module_compat_t *mod = Module::getCurrentModule();
    if (!mod)
        return false;
    return lc_global_has_initializer(mod, impl());
}

inline Constant *GlobalVariable::getInitializer() const {
    return nullptr;
}

inline void GlobalVariable::setInitializer(Constant *InitVal) {
    lc_module_compat_t *mod = Module::getCurrentModule();
    if (!mod || !InitVal)
        return;
    (void)lc_global_set_initializer(mod, impl(), InitVal->impl());
}

inline Function *Function::Create(FunctionType *Ty, GlobalValue::LinkageTypes Linkage,
                                    const Twine &Name, Module &M) {
    /* In real LLVM, a function starts as a declaration and becomes a
       definition when basic blocks are added.  We mirror that: always
       create as a declaration here.  createFunction(is_decl=true) calls
       lc_func_declare which sets is_decl=true.  When the caller later
       adds a BasicBlock the block list becomes non-empty, and
       lr_emit_object uses (!f->is_decl && f->first_block) to decide
       what to compile vs. leave as an undefined symbol. */
    (void)Linkage;
    return M.createFunction(Name.c_str(), Ty, /*is_decl=*/true);
}

inline Function *Function::Create(FunctionType *Ty, GlobalValue::LinkageTypes Linkage,
                                    const Twine &Name, Module *M) {
    if (!M) return nullptr;
    return Function::Create(Ty, Linkage, Name, *M);
}

inline BasicBlock *BasicBlock::Create(LLVMContext &Context, const Twine &Name,
                                       Function *Parent,
                                       BasicBlock *InsertBefore) {
    (void)Context;
    Function *fn = Parent;
    if (!fn && InsertBefore) {
        fn = InsertBefore->getParent();
    }
    if (!fn) {
        fn = detail::current_function;
    }
    lc_module_compat_t *mod = nullptr;
    lr_func_t *f = nullptr;
    if (fn) {
        detail::current_function = fn;
        mod = fn->getCompatMod();
        f = fn->getIRFunc();
    }
    if ((!mod || !f) && InsertBefore) {
        f = lc_value_get_block_func(InsertBefore->impl());
        if (!fn && f) {
            fn = detail::lookup_function_wrapper(f);
            if (fn) detail::current_function = fn;
        }
        if (!mod) {
            mod = Module::getCurrentModule();
        }
    }
    if (!mod || !f) return BasicBlock::wrap(nullptr);
    lc_value_t *bv = nullptr;
    if (!Parent && !InsertBefore && !f->first_block) {
        bv = lc_block_create_detached(mod, f, Name.c_str());
    } else {
        bv = lc_block_create(mod, f, Name.c_str());
    }
    if (fn) {
        detail::register_block_parent(lc_value_get_block(bv), fn);
    }
    return BasicBlock::wrap(bv);
}

inline bool legacy::PassManager::run(Module &M) {
    if (!detail::obj_emit_state.out)
        return false;
    if (detail::obj_emit_state.file_type != CodeGenFileType::ObjectFile)
        return false;

    lc_module_compat_t *compat = M.getCompat();
    if (!compat)
        return false;

    FILE *f = detail::obj_emit_state.out->getFileOrNull();
    if (!f)
        return false;

    int rc = lc_module_emit_object_to_file(compat, f);
    detail::obj_emit_state.out = nullptr;
    return rc == 0;
}

} // namespace llvm

#endif
