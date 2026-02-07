#ifndef LLVM_IR_MODULE_H
#define LLVM_IR_MODULE_H

#include <liric/liric_compat.h>
#include <liric/liric.h>
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
#include <vector>
#include <memory>

namespace llvm {

class Module {
    lc_module_compat_t *compat_;
    LLVMContext &ctx_;
    std::string name_;
    std::vector<std::unique_ptr<Function>> owned_functions_;
    std::vector<std::unique_ptr<GlobalVariable>> owned_globals_;

    static inline thread_local lc_module_compat_t *current_ = nullptr;

public:
    Module(StringRef name, LLVMContext &ctx)
        : ctx_(ctx), name_(name.str()) {
        compat_ = lc_module_create(ctx.impl(), name.data());
        current_ = compat_;
    }

    ~Module() {
        if (current_ == compat_) current_ = nullptr;
        lc_module_destroy(compat_);
    }

    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;

    static lc_module_compat_t *getCurrentModule() { return current_; }
    static void setCurrentModule(lc_module_compat_t *m) { current_ = m; }

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
            if (std::strcmp(f->name, Name.data()) == 0) {
                for (auto &of : owned_functions_) {
                    lr_func_t *irf = of->getIRFunc();
                    if (irf == f) return of.get();
                }
                return nullptr;
            }
        }
        return nullptr;
    }

    GlobalVariable *getGlobalVariable(StringRef Name, bool AllowInternal = false) const {
        (void)Name; (void)AllowInternal;
        return nullptr;
    }

    GlobalVariable *getNamedGlobal(StringRef Name) const {
        return getGlobalVariable(Name);
    }

    void print(raw_ostream &OS, void * = nullptr) const {
        (void)OS;
        lc_module_dump(compat_);
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
        owned_functions_.push_back(std::move(fn));
        return ptr;
    }

    GlobalVariable *createGlobalVariable(
        const char *name, Type *ty, bool is_const,
        GlobalValue::LinkageTypes linkage,
        const void *init_data = nullptr, size_t init_size = 0) {
        (void)linkage;
        lc_value_t *gv;
        if (init_data && init_size > 0) {
            gv = lc_global_create(compat_, name, ty->impl(), is_const,
                                  init_data, init_size);
        } else {
            gv = lc_global_create(compat_, name, ty->impl(), is_const,
                                  nullptr, 0);
        }
        auto g = std::make_unique<GlobalVariable>();
        (void)gv;
        GlobalVariable *ptr = g.get();
        owned_globals_.push_back(std::move(g));
        return ptr;
    }
};

inline lc_module_compat_t *liric_get_current_module() {
    return Module::getCurrentModule();
}

inline Type *Type::getVoidTy(LLVMContext &C) {
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? Type::wrap(lc_get_void_type(mod)) : nullptr;
}
inline Type *Type::getFloatTy(LLVMContext &C) {
    (void)C;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? Type::wrap(lc_get_float_type(mod)) : nullptr;
}
inline Type *Type::getDoubleTy(LLVMContext &C) {
    (void)C;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? Type::wrap(lc_get_double_type(mod)) : nullptr;
}
inline IntegerType *Type::getInt1Ty(LLVMContext &C) {
    (void)C;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? IntegerType::wrap(lc_get_int_type(mod, 1)) : nullptr;
}
inline IntegerType *Type::getInt8Ty(LLVMContext &C) {
    (void)C;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? IntegerType::wrap(lc_get_int_type(mod, 8)) : nullptr;
}
inline IntegerType *Type::getInt16Ty(LLVMContext &C) {
    (void)C;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? IntegerType::wrap(lc_get_int_type(mod, 16)) : nullptr;
}
inline IntegerType *Type::getInt32Ty(LLVMContext &C) {
    (void)C;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? IntegerType::wrap(lc_get_int_type(mod, 32)) : nullptr;
}
inline IntegerType *Type::getInt64Ty(LLVMContext &C) {
    (void)C;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? IntegerType::wrap(lc_get_int_type(mod, 64)) : nullptr;
}

inline PointerType *Type::getPointerTo(unsigned AddrSpace) const {
    (void)AddrSpace;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? PointerType::wrap(lc_get_ptr_type(mod)) : nullptr;
}

inline LLVMContext &Type::getContext() const {
    static LLVMContext dummy;
    return dummy;
}

inline IntegerType *IntegerType::get(LLVMContext &C, unsigned NumBits) {
    (void)C;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? IntegerType::wrap(lc_get_int_type(mod, NumBits)) : nullptr;
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
    (void)C;
    lc_module_compat_t *mod = Module::getCurrentModule();
    if (!mod) return nullptr;
    lr_module_t *m = lc_module_get_ir(mod);
    char *name_dup = lr_arena_strdup(m->arena, Name.data(), Name.size());
    lr_type_t *st = lr_type_struct_new(m, nullptr, 0, false);
    st->struc.name = name_dup;
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
    (void)C;
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
    return StructType::wrap(st);
}

inline ArrayType *ArrayType::get(Type *ElementType, uint64_t NumElements) {
    lc_module_compat_t *mod = Module::getCurrentModule();
    if (!mod) return nullptr;
    lr_type_t *at = lr_type_array_new(lc_module_get_ir(mod),
                                       ElementType->impl(), NumElements);
    return ArrayType::wrap(at);
}

inline PointerType *PointerType::get(LLVMContext &C, unsigned AddressSpace) {
    (void)C; (void)AddressSpace;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? PointerType::wrap(lc_get_ptr_type(mod)) : nullptr;
}

inline PointerType *PointerType::getUnqual(Type *ElementType) {
    (void)ElementType;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? PointerType::wrap(lc_get_ptr_type(mod)) : nullptr;
}

inline PointerType *PointerType::getUnqual(LLVMContext &C) {
    (void)C;
    lc_module_compat_t *mod = Module::getCurrentModule();
    return mod ? PointerType::wrap(lc_get_ptr_type(mod)) : nullptr;
}

inline GlobalVariable::GlobalVariable(Module &M, Type *Ty, bool isConstant,
                                       LinkageTypes Linkage,
                                       Constant *Initializer,
                                       const Twine &Name,
                                       GlobalVariable *InsertBefore,
                                       bool ThreadLocal,
                                       unsigned AddressSpace) {
    (void)InsertBefore; (void)ThreadLocal; (void)AddressSpace;
    (void)Initializer;
    M.createGlobalVariable(Name.c_str(), Ty, isConstant, Linkage);
}

inline Function *Function::Create(FunctionType *Ty, GlobalValue::LinkageTypes Linkage,
                                    const Twine &Name, Module &M) {
    bool is_decl = (Linkage == GlobalValue::ExternalLinkage &&
                    Ty->getReturnType() == nullptr);
    return M.createFunction(Name.c_str(), Ty, is_decl);
}

inline Function *Function::Create(FunctionType *Ty, GlobalValue::LinkageTypes Linkage,
                                    const Twine &Name, Module *M) {
    if (!M) return nullptr;
    return Function::Create(Ty, Linkage, Name, *M);
}

inline BasicBlock *BasicBlock::Create(LLVMContext &Context, const Twine &Name,
                                       Function *Parent,
                                       BasicBlock *InsertBefore) {
    (void)Context; (void)InsertBefore;
    if (!Parent) return nullptr;
    lc_module_compat_t *mod = Parent->getCompatMod();
    lr_func_t *f = Parent->getIRFunc();
    if (!mod || !f) return nullptr;
    lc_value_t *bv = lc_block_create(mod, f, Name.c_str());
    return BasicBlock::wrap(bv);
}

} // namespace llvm

#endif
