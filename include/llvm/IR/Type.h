#ifndef LLVM_IR_TYPE_H
#define LLVM_IR_TYPE_H

#include <liric/liric_compat.h>
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {

class raw_ostream;

class PointerType;
class IntegerType;
class FunctionType;
class StructType;
class ArrayType;

class Type {
public:
    enum TypeID {
        VoidTyID = 0,
        HalfTyID,
        BFloatTyID,
        FloatTyID,
        DoubleTyID,
        X86_FP80TyID,
        FP128TyID,
        PPC_FP128TyID,
        LabelTyID,
        MetadataTyID,
        X86_MMXTyID,
        X86_AMXTyID,
        TokenTyID,
        IntegerTyID,
        FunctionTyID,
        StructTyID,
        ArrayTyID,
        FixedVectorTyID,
        ScalableVectorTyID,
        TypedPointerTyID,
        PointerTyID,
    };

    lr_type_t *impl() const {
        return const_cast<lr_type_t *>(
            reinterpret_cast<const lr_type_t *>(this));
    }

    static Type *wrap(lr_type_t *t) {
        if (!t) {
            static lr_type_t poison_ty = { LR_TYPE_PTR, {} };
            return reinterpret_cast<Type *>(&poison_ty);
        }
        return reinterpret_cast<Type *>(t);
    }

    TypeID getTypeID() const {
        lr_type_t *t = impl();
        if (!t) return VoidTyID;
        switch (t->kind) {
        case LR_TYPE_VOID:   return VoidTyID;
        case LR_TYPE_I1:
        case LR_TYPE_I8:
        case LR_TYPE_I16:
        case LR_TYPE_I32:
        case LR_TYPE_I64:    return IntegerTyID;
        case LR_TYPE_FLOAT:  return FloatTyID;
        case LR_TYPE_DOUBLE: return DoubleTyID;
        case LR_TYPE_PTR:    return PointerTyID;
        case LR_TYPE_ARRAY:  return ArrayTyID;
        case LR_TYPE_STRUCT: return StructTyID;
        case LR_TYPE_FUNC:   return FunctionTyID;
        }
        return VoidTyID;
    }

    bool isVoidTy() const {
        lr_type_t *t = impl();
        return t && t->kind == LR_TYPE_VOID;
    }
    bool isIntegerTy() const {
        lr_type_t *t = impl();
        return t && lc_type_is_integer(t);
    }
    bool isIntegerTy(unsigned w) const {
        lr_type_t *t = impl();
        return t && lc_type_is_integer(t) && lc_type_int_width(t) == w;
    }
    bool isFloatingPointTy() const {
        lr_type_t *t = impl();
        return t && lc_type_is_floating(t);
    }
    bool isFloatTy() const {
        lr_type_t *t = impl();
        return t && t->kind == LR_TYPE_FLOAT;
    }
    bool isDoubleTy() const {
        lr_type_t *t = impl();
        return t && t->kind == LR_TYPE_DOUBLE;
    }
    bool isPointerTy() const {
        lr_type_t *t = impl();
        return t && lc_type_is_pointer(t);
    }
    bool isStructTy() const {
        lr_type_t *t = impl();
        return t && t->kind == LR_TYPE_STRUCT;
    }
    bool isArrayTy() const {
        lr_type_t *t = impl();
        return t && t->kind == LR_TYPE_ARRAY;
    }
    bool isFunctionTy() const {
        lr_type_t *t = impl();
        return t && t->kind == LR_TYPE_FUNC;
    }
    bool isVectorTy() const { return false; }

    unsigned getIntegerBitWidth() const {
        lr_type_t *t = impl();
        return t ? lc_type_int_width(t) : 0;
    }

    unsigned getScalarSizeInBits() const {
        lr_type_t *t = impl();
        if (!t) return 0;
        if (lc_type_is_integer(t)) return lc_type_int_width(t);
        if (isFloatTy()) return 32;
        if (isDoubleTy()) return 64;
        if (isPointerTy()) return 64;
        return 0;
    }

    unsigned getPrimitiveSizeInBits() const {
        lr_type_t *t = impl();
        return t ? lc_type_primitive_size_bits(t) : 0;
    }

    Type *getScalarType() { return this; }

    Type *getPointerElementType() const;

    Type *getContainedType(unsigned i) const {
        lr_type_t *t = impl();
        if (!t) return nullptr;
        return Type::wrap(lc_type_contained(t, i));
    }

    Type *getStructElementType(unsigned i) const {
        lr_type_t *t = impl();
        if (!t) return nullptr;
        return Type::wrap(lc_type_struct_field(t, i));
    }

    unsigned getStructNumElements() const {
        lr_type_t *t = impl();
        return t ? lc_type_struct_num_fields(t) : 0;
    }

    void print(raw_ostream &OS, bool IsForDebug = false) const;

    PointerType *getPointerTo(unsigned AddrSpace = 0) const;

    LLVMContext &getContext() const;

    static Type *getVoidTy(LLVMContext &C);
    static Type *getFloatTy(LLVMContext &C);
    static Type *getDoubleTy(LLVMContext &C);
    static IntegerType *getInt1Ty(LLVMContext &C);
    static IntegerType *getInt8Ty(LLVMContext &C);
    static IntegerType *getInt16Ty(LLVMContext &C);
    static IntegerType *getInt32Ty(LLVMContext &C);
    static IntegerType *getInt64Ty(LLVMContext &C);
    static PointerType *getInt8PtrTy(LLVMContext &C, unsigned AS = 0);
    static IntegerType *getIntNTy(LLVMContext &C, unsigned N);
};

} // namespace llvm

#endif
