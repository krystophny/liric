#ifndef LLVM_IR_DERIVEDTYPES_H
#define LLVM_IR_DERIVEDTYPES_H

#include "llvm/IR/Type.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <vector>
#include <cstring>

namespace llvm {

class Module;

class IntegerType : public Type {
public:
    unsigned getBitWidth() const { return lc_type_int_width(impl()); }

    static IntegerType *get(LLVMContext &C, unsigned NumBits);

    static IntegerType *wrap(lr_type_t *t) {
        return static_cast<IntegerType *>(Type::wrap(t));
    }
};

class FunctionType : public Type {
public:
    Type *getReturnType() const {
        return Type::wrap(impl()->func.ret);
    }

    unsigned getNumParams() const { return impl()->func.num_params; }

    Type *getParamType(unsigned i) const {
        return Type::wrap(impl()->func.params[i]);
    }

    bool isVarArg() const { return impl()->func.vararg; }

    using param_iterator = Type **;

    static FunctionType *get(Type *Result, ArrayRef<Type *> Params,
                             bool isVarArg);

    static FunctionType *get(Type *Result, bool isVarArg);

    static FunctionType *wrap(lr_type_t *t) {
        return static_cast<FunctionType *>(Type::wrap(t));
    }
};

class StructType : public Type {
public:
    unsigned getNumElements() const {
        return impl()->struc.num_fields;
    }

    Type *getElementType(unsigned idx) const {
        return Type::wrap(impl()->struc.fields[idx]);
    }

    bool isPacked() const { return impl()->struc.packed; }

    bool isOpaque() const { return impl()->struc.num_fields == 0; }
    bool isLiteral() const { return impl()->struc.name == nullptr; }

    StringRef getName() const {
        if (impl()->struc.name) return impl()->struc.name;
        return "";
    }

    void setBody(ArrayRef<Type *> Elements, bool isPacked = false);

    static StructType *create(LLVMContext &C, StringRef Name);
    static StructType *create(LLVMContext &C, ArrayRef<Type *> Elements,
                              StringRef Name, bool isPacked = false);

    static StructType *get(LLVMContext &C, ArrayRef<Type *> Elements,
                           bool isPacked = false);

    static StructType *wrap(lr_type_t *t) {
        return static_cast<StructType *>(Type::wrap(t));
    }
};

class ArrayType : public Type {
public:
    Type *getElementType() const {
        return Type::wrap(impl()->array.elem);
    }

    uint64_t getNumElements() const { return impl()->array.count; }

    static ArrayType *get(Type *ElementType, uint64_t NumElements);

    static ArrayType *wrap(lr_type_t *t) {
        return static_cast<ArrayType *>(Type::wrap(t));
    }
};

class PointerType : public Type {
public:
    static PointerType *get(LLVMContext &C, unsigned AddressSpace = 0);
    static PointerType *getUnqual(Type *ElementType);
    static PointerType *getUnqual(LLVMContext &C);

    unsigned getAddressSpace() const { return 0; }

    static PointerType *wrap(lr_type_t *t) {
        return static_cast<PointerType *>(Type::wrap(t));
    }
};

} // namespace llvm

#endif
