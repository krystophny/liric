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

    static bool classof(const Type *T) {
        return T && T->getTypeID() == IntegerTyID;
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

    static bool classof(const Type *T) {
        return T && T->getTypeID() == FunctionTyID;
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
    bool hasName() const { return lc_type_struct_has_name(impl()); }

    Type *getStructElementType(unsigned i) const {
        return Type::wrap(lc_type_struct_field(impl(), i));
    }

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

    template <typename... Types>
    static StructType *get(Type *First, Types *... Rest) {
        Type *elems[] = {First, static_cast<Type *>(Rest)...};
        LLVMContext &C = First->getContext();
        return get(C, ArrayRef<Type *>(elems, 1 + sizeof...(Rest)));
    }

    static StructType *create(ArrayRef<Type *> Elements, StringRef Name = "",
                              bool isPacked = false) {
        if (Elements.empty()) return nullptr;
        LLVMContext &C = Elements[0]->getContext();
        return create(C, Elements, Name, isPacked);
    }

    static StructType *wrap(lr_type_t *t) {
        return static_cast<StructType *>(Type::wrap(t));
    }

    static bool classof(const Type *T) {
        return T && T->getTypeID() == StructTyID;
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

    static bool classof(const Type *T) {
        return T && T->getTypeID() == ArrayTyID;
    }
};

class PointerType : public Type {
public:
    static PointerType *get(LLVMContext &C, unsigned AddressSpace = 0);
    static PointerType *get(Type *ElementType, unsigned AddressSpace) {
        (void)ElementType; (void)AddressSpace;
        return getUnqual(ElementType);
    }
    static PointerType *getUnqual(Type *ElementType);
    static PointerType *getUnqual(LLVMContext &C);

    unsigned getAddressSpace() const { return 0; }

    static PointerType *wrap(lr_type_t *t) {
        return static_cast<PointerType *>(Type::wrap(t));
    }

    static bool classof(const Type *T) {
        return T && T->getTypeID() == PointerTyID;
    }
};

class VectorType : public Type {
public:
    static VectorType *get(Type *ElementTy, unsigned NumElts,
                           bool Scalable = false) {
        if (!ElementTy || NumElts == 0 || Scalable)
            return nullptr;

        LLVMContext &C = ElementTy->getContext();
        lc_module_compat_t *mod = C.getDefaultModule();
        if (!mod) return nullptr;

        lr_module_t *m = lc_module_get_ir(mod);
        std::vector<lr_type_t *> fields(NumElts, ElementTy->impl());
        lr_type_t *vec = lr_type_struct_new(
            m, fields.data(), static_cast<uint32_t>(NumElts), true);
        detail::register_type_context(vec, &C);
        detail::register_vector_type(vec, ElementTy->impl(), NumElts, false);
        return VectorType::wrap(vec);
    }

    static VectorType *wrap(lr_type_t *t) {
        return static_cast<VectorType *>(Type::wrap(t));
    }

    static bool classof(const Type *T) {
        if (!T) return false;
        auto id = T->getTypeID();
        return id == FixedVectorTyID || id == ScalableVectorTyID;
    }
};

class FixedVectorType : public VectorType {
public:
    static FixedVectorType *get(Type *ElementTy, unsigned NumElts) {
        return static_cast<FixedVectorType *>(
            VectorType::get(ElementTy, NumElts, false));
    }

    static FixedVectorType *wrap(lr_type_t *t) {
        return static_cast<FixedVectorType *>(Type::wrap(t));
    }

    static bool classof(const Type *T) {
        return T && T->getTypeID() == FixedVectorTyID;
    }
};

} // namespace llvm

#endif
