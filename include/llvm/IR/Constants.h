#ifndef LLVM_IR_CONSTANTS_H
#define LLVM_IR_CONSTANTS_H

#include "llvm/IR/Value.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/ArrayRef.h"
#include <liric/liric_compat.h>

namespace llvm {

class Module;

lc_module_compat_t *liric_get_current_module();

class Constant : public Value {
public:
    static Constant *getNullValue(Type *Ty) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        return static_cast<Constant *>(Value::wrap(
            lc_value_const_null(mod, Ty->impl())));
    }

    static Constant *getAllOnesValue(Type *Ty) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        if (Ty->isIntegerTy()) {
            return static_cast<Constant *>(Value::wrap(
                lc_value_const_int(mod, Ty->impl(), -1,
                                   lc_type_int_width(Ty->impl()))));
        }
        return getNullValue(Ty);
    }

    bool isNullValue() const {
        return impl()->kind == LC_VAL_CONST_NULL;
    }

    bool isZeroValue() const {
        if (impl()->kind == LC_VAL_CONST_NULL) return true;
        if (impl()->kind == LC_VAL_CONST_INT) return impl()->const_int.val == 0;
        if (impl()->kind == LC_VAL_CONST_FP) return impl()->const_fp.val == 0.0;
        return false;
    }
};

class ConstantInt : public Constant {
public:
    static ConstantInt *get(Type *Ty, uint64_t V, bool isSigned = false) {
        (void)isSigned;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        return static_cast<ConstantInt *>(Value::wrap(
            lc_value_const_int(mod, Ty->impl(),
                               static_cast<int64_t>(V),
                               lc_type_int_width(Ty->impl()))));
    }

    static ConstantInt *get(IntegerType *Ty, uint64_t V, bool isSigned = false) {
        return get(static_cast<Type *>(Ty), V, isSigned);
    }

    static ConstantInt *get(LLVMContext &C, const APInt &V) {
        (void)C;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        unsigned width = V.getBitWidth();
        lr_type_t *ty = lc_get_int_type(mod, width);
        return static_cast<ConstantInt *>(Value::wrap(
            lc_value_const_int(mod, ty,
                               static_cast<int64_t>(V.getZExtValue()),
                               width)));
    }

    static ConstantInt *getSigned(Type *Ty, int64_t V) {
        return get(Ty, static_cast<uint64_t>(V), true);
    }

    static ConstantInt *getTrue(LLVMContext &C) {
        (void)C;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        return static_cast<ConstantInt *>(Value::wrap(
            lc_value_const_int(mod, lc_get_int_type(mod, 1), 1, 1)));
    }

    static ConstantInt *getFalse(LLVMContext &C) {
        (void)C;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        return static_cast<ConstantInt *>(Value::wrap(
            lc_value_const_int(mod, lc_get_int_type(mod, 1), 0, 1)));
    }

    int64_t getSExtValue() const { return impl()->const_int.val; }
    uint64_t getZExtValue() const {
        return static_cast<uint64_t>(impl()->const_int.val);
    }

    const APInt &getValue() const {
        static thread_local APInt cached;
        cached = APInt(impl()->const_int.width,
                       static_cast<uint64_t>(impl()->const_int.val));
        return cached;
    }

    unsigned getBitWidth() const { return impl()->const_int.width; }

    bool isZero() const { return impl()->const_int.val == 0; }
    bool isOne() const { return impl()->const_int.val == 1; }
    bool isNegative() const { return impl()->const_int.val < 0; }
};

class ConstantFP : public Constant {
public:
    static ConstantFP *get(Type *Ty, double V) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        bool is_double = Ty->isDoubleTy();
        return static_cast<ConstantFP *>(Value::wrap(
            lc_value_const_fp(mod, Ty->impl(), V, is_double)));
    }

    static ConstantFP *get(Type *Ty, const APFloat &V) {
        return get(Ty, V.convertToDouble());
    }

    static ConstantFP *get(Type *Ty, StringRef Str) {
        double v = 0.0;
        try { v = std::stod(Str.str()); } catch (...) {}
        return get(Ty, v);
    }

    static ConstantFP *get(LLVMContext &C, const APFloat &V) {
        (void)C;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        return static_cast<ConstantFP *>(Value::wrap(
            lc_value_const_fp(mod, lc_get_double_type(mod),
                              V.convertToDouble(), true)));
    }

    double getValueAPF_double() const { return impl()->const_fp.val; }

    const APFloat &getValueAPF() const {
        static thread_local APFloat cached(0.0);
        cached = APFloat(impl()->const_fp.val);
        return cached;
    }

    bool isZero() const { return impl()->const_fp.val == 0.0; }
};

class ConstantPointerNull : public Constant {
public:
    static ConstantPointerNull *get(PointerType *T) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        return static_cast<ConstantPointerNull *>(Value::wrap(
            lc_value_const_null(mod, T->impl())));
    }
};

class UndefValue : public Constant {
public:
    static UndefValue *get(Type *Ty) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        return static_cast<UndefValue *>(Value::wrap(
            lc_value_undef(mod, Ty->impl())));
    }
};

class PoisonValue : public Constant {
public:
    static PoisonValue *get(Type *Ty) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        return static_cast<PoisonValue *>(Value::wrap(
            lc_value_undef(mod, Ty->impl())));
    }
};

class ConstantStruct : public Constant {
public:
    static Constant *get(StructType *T, ArrayRef<Constant *> V) {
        (void)V;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        return static_cast<Constant *>(Value::wrap(
            lc_value_const_aggregate(mod, T->impl(), nullptr, 0)));
    }
};

class ConstantArray : public Constant {
public:
    static Constant *get(ArrayType *T, ArrayRef<Constant *> V) {
        (void)V;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        return static_cast<Constant *>(Value::wrap(
            lc_value_const_aggregate(mod, T->impl(), nullptr, 0)));
    }
};

class ConstantDataArray : public Constant {
public:
    static Constant *getString(LLVMContext &C, StringRef Str,
                               bool AddNull = true) {
        (void)C;
        (void)AddNull;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        size_t len = Str.size() + (AddNull ? 1 : 0);
        lr_type_t *elem = lc_get_int_type(mod, 8);
        lr_type_t *arr = lr_type_array_new(lc_module_get_ir(mod), elem, len);
        return static_cast<Constant *>(Value::wrap(
            lc_value_const_aggregate(mod, arr, Str.data(), Str.size())));
    }

    static Constant *get(LLVMContext &C, ArrayRef<uint8_t> Elts) {
        (void)C;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        lr_type_t *elem = lc_get_int_type(mod, 8);
        lr_type_t *arr = lr_type_array_new(lc_module_get_ir(mod),
                                            elem, Elts.size());
        return static_cast<Constant *>(Value::wrap(
            lc_value_const_aggregate(mod, arr, Elts.data(), Elts.size())));
    }
};

class ConstantExpr : public Constant {
public:
    static Constant *getBitCast(Constant *C, Type *Ty) {
        (void)Ty;
        return C;
    }
    static Constant *getPointerCast(Constant *C, Type *Ty) {
        (void)Ty;
        return C;
    }
    static Constant *getIntToPtr(Constant *C, Type *Ty) {
        (void)Ty;
        return C;
    }
    static Constant *getPtrToInt(Constant *C, Type *Ty) {
        (void)Ty;
        return C;
    }
    static Constant *getGetElementPtr(Type *Ty, Constant *C,
                                      ArrayRef<Constant *> IdxList,
                                      bool InBounds = false) {
        (void)Ty; (void)IdxList; (void)InBounds;
        return C;
    }
    static Constant *getGetElementPtr(Type *Ty, Constant *C,
                                      Constant *Idx,
                                      bool InBounds = false) {
        (void)Ty; (void)Idx; (void)InBounds;
        return C;
    }
};

class BlockAddress : public Constant {
public:
    static BlockAddress *get(Function *F, BasicBlock *BB) {
        (void)F; (void)BB;
        return nullptr;
    }
};

} // namespace llvm

#endif
