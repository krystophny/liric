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
        return reinterpret_cast<Type *>(t);
    }

    TypeID getTypeID() const {
        switch (impl()->kind) {
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

    bool isVoidTy() const { return impl()->kind == LR_TYPE_VOID; }
    bool isIntegerTy() const { return lc_type_is_integer(impl()); }
    bool isIntegerTy(unsigned w) const {
        return isIntegerTy() && lc_type_int_width(impl()) == w;
    }
    bool isFloatingPointTy() const { return lc_type_is_floating(impl()); }
    bool isFloatTy() const { return impl()->kind == LR_TYPE_FLOAT; }
    bool isDoubleTy() const { return impl()->kind == LR_TYPE_DOUBLE; }
    bool isPointerTy() const { return lc_type_is_pointer(impl()); }
    bool isStructTy() const { return impl()->kind == LR_TYPE_STRUCT; }
    bool isArrayTy() const { return impl()->kind == LR_TYPE_ARRAY; }
    bool isFunctionTy() const { return impl()->kind == LR_TYPE_FUNC; }
    bool isVectorTy() const { return false; }

    unsigned getIntegerBitWidth() const { return lc_type_int_width(impl()); }

    unsigned getScalarSizeInBits() const {
        if (isIntegerTy()) return lc_type_int_width(impl());
        if (isFloatTy()) return 32;
        if (isDoubleTy()) return 64;
        if (isPointerTy()) return 64;
        return 0;
    }

    unsigned getPrimitiveSizeInBits() const {
        return lc_type_primitive_size_bits(impl());
    }

    Type *getScalarType() { return this; }

    Type *getPointerElementType() const;

    Type *getContainedType(unsigned i) const {
        return Type::wrap(lc_type_contained(impl(), i));
    }

    Type *getStructElementType(unsigned i) const {
        return Type::wrap(lc_type_struct_field(impl(), i));
    }

    unsigned getStructNumElements() const {
        return lc_type_struct_num_fields(impl());
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
