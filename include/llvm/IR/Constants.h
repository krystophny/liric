#ifndef LLVM_IR_CONSTANTS_H
#define LLVM_IR_CONSTANTS_H

#include "llvm/IR/Value.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/ArrayRef.h"
#include <liric/liric_compat.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace llvm {

class Module;
class Function;
class BasicBlock;
class Constant;

lc_module_compat_t *liric_get_current_module();

namespace detail {

inline size_t liric_type_align(const lr_type_t *t) {
    if (!t) return 1;
    switch (t->kind) {
    case LR_TYPE_VOID:   return 1;
    case LR_TYPE_I1:     return 1;
    case LR_TYPE_I8:     return 1;
    case LR_TYPE_I16:    return 2;
    case LR_TYPE_I32:    return 4;
    case LR_TYPE_I64:    return 8;
    case LR_TYPE_FLOAT:  return 4;
    case LR_TYPE_DOUBLE: return 8;
    case LR_TYPE_PTR:    return 8;
    case LR_TYPE_ARRAY:  return liric_type_align(t->array.elem);
    case LR_TYPE_STRUCT: {
        if (t->struc.packed) return 1;
        size_t max_align = 1;
        for (uint32_t i = 0; i < t->struc.num_fields; i++) {
            size_t a = liric_type_align(t->struc.fields[i]);
            if (a > max_align) max_align = a;
        }
        return max_align;
    }
    case LR_TYPE_FUNC:   return 1;
    }
    return 1;
}

inline size_t liric_type_size(const lr_type_t *t) {
    if (!t) return 0;
    switch (t->kind) {
    case LR_TYPE_VOID:   return 0;
    case LR_TYPE_I1:     return 1;
    case LR_TYPE_I8:     return 1;
    case LR_TYPE_I16:    return 2;
    case LR_TYPE_I32:    return 4;
    case LR_TYPE_I64:    return 8;
    case LR_TYPE_FLOAT:  return 4;
    case LR_TYPE_DOUBLE: return 8;
    case LR_TYPE_PTR:    return 8;
    case LR_TYPE_ARRAY:
        return liric_type_size(t->array.elem) * t->array.count;
    case LR_TYPE_STRUCT: {
        size_t sz = 0;
        for (uint32_t i = 0; i < t->struc.num_fields; i++) {
            size_t fsz = liric_type_size(t->struc.fields[i]);
            if (!t->struc.packed) {
                size_t fa = liric_type_align(t->struc.fields[i]);
                sz = (sz + fa - 1) & ~(fa - 1);
            }
            sz += fsz;
        }
        if (!t->struc.packed && t->struc.num_fields > 0) {
            size_t sa = liric_type_align(t);
            sz = (sz + sa - 1) & ~(sa - 1);
        }
        return sz;
    }
    case LR_TYPE_FUNC:
        return 0;
    }
    return 0;
}

inline size_t liric_struct_field_offset(const lr_type_t *st, uint32_t field_idx) {
    size_t off = 0;
    if (!st || st->kind != LR_TYPE_STRUCT)
        return 0;
    for (uint32_t i = 0; i < st->struc.num_fields && i < field_idx; i++) {
        if (!st->struc.packed) {
            size_t fa = liric_type_align(st->struc.fields[i]);
            off = (off + fa - 1) & ~(fa - 1);
        }
        off += liric_type_size(st->struc.fields[i]);
    }
    if (field_idx < st->struc.num_fields && !st->struc.packed) {
        size_t fa = liric_type_align(st->struc.fields[field_idx]);
        off = (off + fa - 1) & ~(fa - 1);
    }
    return off;
}

inline bool liric_pack_constant_bytes(Value *C, const lr_type_t *ty,
                                      std::vector<uint8_t> &out) {
    size_t sz = liric_type_size(ty);
    out.assign(sz, 0);
    if (!C || !C->impl() || !ty)
        return false;

    lc_value_t *v = C->impl();
    switch (v->kind) {
    case LC_VAL_CONST_NULL:
    case LC_VAL_CONST_UNDEF:
        return true;
    case LC_VAL_CONST_INT: {
        if (sz == 0)
            return true;
        if (ty->kind == LR_TYPE_I1) {
            out[0] = v->const_int.val ? 1u : 0u;
            return true;
        }
        uint64_t raw = static_cast<uint64_t>(v->const_int.val);
        size_t n = std::min(sz, sizeof(raw));
        for (size_t i = 0; i < n; i++)
            out[i] = static_cast<uint8_t>((raw >> (8u * i)) & 0xffu);
        return true;
    }
    case LC_VAL_CONST_FP:
        if (ty->kind == LR_TYPE_FLOAT) {
            float f = static_cast<float>(v->const_fp.val);
            size_t n = std::min(sz, sizeof(f));
            std::memcpy(out.data(), &f, n);
        } else {
            double d = v->const_fp.val;
            size_t n = std::min(sz, sizeof(d));
            std::memcpy(out.data(), &d, n);
        }
        return true;
    case LC_VAL_CONST_AGGREGATE:
        if (v->aggregate.data && v->aggregate.size > 0) {
            size_t n = std::min(sz, v->aggregate.size);
            std::memcpy(out.data(), v->aggregate.data, n);
        }
        return true;
    case LC_VAL_GLOBAL:
        /* Keep zero bytes. Pointer relocations are handled when the global
           itself is used directly as an initializer. */
        return true;
    default:
        return false;
    }
}

} // namespace detail

class Constant : public Value {
public:
    static Constant *getNullValue(Type *Ty) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod || !Ty || !Ty->impl()) return nullptr;
        return static_cast<Constant *>(Value::wrap(
            lc_value_const_null(mod, Ty->impl())));
    }

    static Constant *getAllOnesValue(Type *Ty) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod || !Ty || !Ty->impl()) return nullptr;
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

    Constant *getAggregateElement(unsigned Elt) const {
        (void)Elt;
        return nullptr;
    }

    static bool classof(const Value *V) {
        if (!V) return false;
        lc_value_kind_t k = V->impl()->kind;
        return k == LC_VAL_CONST_INT || k == LC_VAL_CONST_FP ||
               k == LC_VAL_CONST_NULL || k == LC_VAL_CONST_UNDEF ||
               k == LC_VAL_CONST_AGGREGATE || k == LC_VAL_GLOBAL;
    }
};

class ConstantInt : public Constant {
public:
    static ConstantInt *get(Type *Ty, uint64_t V, bool isSigned = false) {
        (void)isSigned;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod || !Ty || !Ty->impl()) return nullptr;
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

    static ConstantInt *get(Type *Ty, const APInt &V) {
        return get(Ty, V.getZExtValue(), false);
    }

    static ConstantInt *getSigned(Type *Ty, int64_t V) {
        if (!Ty || !Ty->impl()) return nullptr;
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

    static bool classof(const Value *V) {
        return V && V->impl()->kind == LC_VAL_CONST_INT;
    }
};

class ConstantFP : public Constant {
public:
    static ConstantFP *get(Type *Ty, double V) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod || !Ty || !Ty->impl()) return nullptr;
        bool is_double = Ty->isDoubleTy();
        return static_cast<ConstantFP *>(Value::wrap(
            lc_value_const_fp(mod, Ty->impl(), V, is_double)));
    }

    static ConstantFP *get(Type *Ty, const APFloat &V) {
        if (!Ty || !Ty->impl()) return nullptr;
        return get(Ty, V.convertToDouble());
    }

    static ConstantFP *get(Type *Ty, StringRef Str) {
        if (!Ty || !Ty->impl()) return nullptr;
        double v = 0.0;
        try { v = std::stod(Str.str()); } catch (...) {}
        return get(Ty, v);
    }

    static ConstantFP *get(LLVMContext &C, const APFloat &V) {
        (void)C;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        bool is_double = !V.isSinglePrecision();
        lr_type_t *ty = is_double ? lc_get_double_type(mod)
                                  : lc_get_float_type(mod);
        return static_cast<ConstantFP *>(Value::wrap(
            lc_value_const_fp(mod, ty, V.convertToDouble(), is_double)));
    }

    double getValueAPF_double() const { return impl()->const_fp.val; }

    const APFloat &getValueAPF() const {
        static thread_local APFloat cached(0.0);
        cached = APFloat(impl()->const_fp.val);
        return cached;
    }

    bool isZero() const { return impl()->const_fp.val == 0.0; }

    static bool classof(const Value *V) {
        return V && V->impl()->kind == LC_VAL_CONST_FP;
    }
};

class ConstantPointerNull : public Constant {
public:
    static ConstantPointerNull *get(PointerType *T) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod || !T || !T->impl()) return nullptr;
        return static_cast<ConstantPointerNull *>(Value::wrap(
            lc_value_const_null(mod, T->impl())));
    }
};

class UndefValue : public Constant {
public:
    static UndefValue *get(Type *Ty) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod || !Ty || !Ty->impl()) return nullptr;
        return static_cast<UndefValue *>(Value::wrap(
            lc_value_undef(mod, Ty->impl())));
    }
};

class PoisonValue : public Constant {
public:
    static PoisonValue *get(Type *Ty) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod || !Ty || !Ty->impl()) return nullptr;
        return static_cast<PoisonValue *>(Value::wrap(
            lc_value_undef(mod, Ty->impl())));
    }
};

class ConstantStruct : public Constant {
public:
    static Constant *get(StructType *T, ArrayRef<Constant *> V) {
        struct RelocRef {
            size_t offset;
            const char *symbol;
        };
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod || !T || !T->impl()) return nullptr;
        const lr_type_t *sty = T->impl();
        std::vector<uint8_t> bytes(detail::liric_type_size(sty), 0);
        std::vector<RelocRef> relocs;
        if (sty->kind == LR_TYPE_STRUCT) {
            uint32_t nfields = sty->struc.num_fields;
            uint32_t nvals = static_cast<uint32_t>(V.size());
            for (uint32_t i = 0; i < nfields && i < nvals; i++) {
                std::vector<uint8_t> field;
                detail::liric_pack_constant_bytes(V[i], sty->struc.fields[i], field);
                size_t off = detail::liric_struct_field_offset(sty, i);
                lc_value_t *elem = V[i] ? V[i]->impl() : nullptr;
                if (elem && elem->kind == LC_VAL_GLOBAL &&
                        sty->struc.fields[i] &&
                        sty->struc.fields[i]->kind == LR_TYPE_PTR &&
                        elem->global.name) {
                    relocs.push_back({off, elem->global.name});
                }
                if (off >= bytes.size())
                    continue;
                size_t ncopy = std::min(field.size(), bytes.size() - off);
                if (ncopy > 0)
                    std::memcpy(bytes.data() + off, field.data(), ncopy);
            }
        }
        lc_value_t *agg = lc_value_const_aggregate(mod, T->impl(),
                                                   bytes.empty() ? nullptr : bytes.data(),
                                                   bytes.size());
        if (agg) {
            for (const RelocRef &r : relocs) {
                (void)lc_value_const_aggregate_add_reloc(mod, agg, r.offset,
                                                         r.symbol, 0);
            }
        }
        return static_cast<Constant *>(Value::wrap(agg));
    }
};

class ConstantArray : public Constant {
public:
    static Constant *get(ArrayType *T, ArrayRef<Constant *> V) {
        struct RelocRef {
            size_t offset;
            const char *symbol;
        };
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod || !T || !T->impl()) return nullptr;
        const lr_type_t *aty = T->impl();
        std::vector<uint8_t> bytes(detail::liric_type_size(aty), 0);
        std::vector<RelocRef> relocs;
        if (aty->kind == LR_TYPE_ARRAY) {
            const lr_type_t *elem_ty = aty->array.elem;
            size_t elem_sz = detail::liric_type_size(elem_ty);
            uint64_t nvals = std::min<uint64_t>(aty->array.count, V.size());
            for (uint64_t i = 0; i < nvals; i++) {
                std::vector<uint8_t> elem;
                detail::liric_pack_constant_bytes(V[static_cast<size_t>(i)],
                                                  elem_ty, elem);
                size_t off = static_cast<size_t>(i) * elem_sz;
                lc_value_t *elem_val = V[static_cast<size_t>(i)]
                    ? V[static_cast<size_t>(i)]->impl() : nullptr;
                if (elem_val && elem_val->kind == LC_VAL_GLOBAL &&
                        elem_ty && elem_ty->kind == LR_TYPE_PTR &&
                        elem_val->global.name) {
                    relocs.push_back({off, elem_val->global.name});
                }
                if (off >= bytes.size())
                    continue;
                size_t ncopy = std::min(elem.size(), bytes.size() - off);
                if (ncopy > 0)
                    std::memcpy(bytes.data() + off, elem.data(), ncopy);
            }
        }
        lc_value_t *agg = lc_value_const_aggregate(mod, T->impl(),
                                                   bytes.empty() ? nullptr : bytes.data(),
                                                   bytes.size());
        if (agg) {
            for (const RelocRef &r : relocs) {
                (void)lc_value_const_aggregate_add_reloc(mod, agg, r.offset,
                                                         r.symbol, 0);
            }
        }
        return static_cast<Constant *>(Value::wrap(agg));
    }
};

class ConstantDataArray : public Constant {
public:
    static Constant *getString(LLVMContext &C, StringRef Str,
                               bool AddNull = true) {
        (void)C;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        std::string bytes(Str.data(), Str.size());
        if (AddNull)
            bytes.push_back('\0');
        lr_type_t *elem = lc_get_int_type(mod, 8);
        lr_type_t *arr = lr_type_array_new(lc_module_get_ir(mod),
                                           elem, bytes.size());
        return static_cast<Constant *>(Value::wrap(
            lc_value_const_aggregate(mod, arr,
                                     bytes.empty() ? nullptr : bytes.data(),
                                     bytes.size())));
    }

    static Constant *get(LLVMContext &C, ArrayRef<uint8_t> Elts) {
        (void)C;
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        lr_type_t *elem = lc_get_int_type(mod, 8);
        lr_type_t *arr = lr_type_array_new(lc_module_get_ir(mod),
                                            elem, Elts.size());
        return static_cast<Constant *>(Value::wrap(
            lc_value_const_aggregate(mod, arr,
                                     Elts.empty() ? nullptr : Elts.data(),
                                     Elts.size())));
    }
};

class ConstantAggregateZero : public Constant {
public:
    static ConstantAggregateZero *get(Type *Ty) {
        lc_module_compat_t *mod = liric_get_current_module();
        if (!mod) return nullptr;
        return static_cast<ConstantAggregateZero *>(Value::wrap(
            lc_value_const_null(mod, Ty->impl())));
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

    static Constant *getCast(unsigned Op, Constant *C, Type *Ty) {
        (void)Op; (void)Ty;
        return C;
    }

    static Constant *getInBoundsGetElementPtr(Type *Ty, Constant *C,
                                              ArrayRef<Constant *> IdxList) {
        (void)Ty; (void)IdxList;
        return C;
    }

    static Constant *getInBoundsGetElementPtr(Type *Ty, Constant *C,
                                              ArrayRef<Value *> IdxList) {
        (void)Ty; (void)IdxList;
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
