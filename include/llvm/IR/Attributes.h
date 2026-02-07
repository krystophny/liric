#ifndef LLVM_IR_ATTRIBUTES_H
#define LLVM_IR_ATTRIBUTES_H

#include "llvm/ADT/StringRef.h"
#include <cstdint>

namespace llvm {

class LLVMContext;

class Attribute {
public:
    enum AttrKind {
        None = 0,
        Alignment,
        AllocAlign,
        AllocatedPointer,
        AlwaysInline,
        Builtin,
        ByRef,
        ByVal,
        Cold,
        Convergent,
        Dereferenceable,
        DereferenceableOrNull,
        Hot,
        ImmArg,
        InAlloca,
        InReg,
        MinSize,
        MustProgress,
        Naked,
        Nest,
        NoAlias,
        NoBuiltin,
        NoCapture,
        NoCfCheck,
        NoDuplicate,
        NoFree,
        NoImplicitFloat,
        NoInline,
        NoMerge,
        NoRecurse,
        NoRedZone,
        NoReturn,
        NoSanitizeBounds,
        NoSanitizeCoverage,
        NoSync,
        NoUndef,
        NoUnwind,
        NonNull,
        OptForFuzzing,
        OptimizeForSize,
        OptimizeNone,
        ReadNone,
        ReadOnly,
        Returned,
        SExt,
        SafeStack,
        SanitizeAddress,
        SanitizeMemory,
        SanitizeThread,
        ShadowCallStack,
        Speculatable,
        StackAlignment,
        StackProtect,
        StackProtectReq,
        StackProtectStrong,
        StrictFP,
        StructRet,
        SwiftAsync,
        SwiftError,
        SwiftSelf,
        UWTable,
        WillReturn,
        WriteOnly,
        ZExt,
        EndAttrKinds,
    };

    static Attribute get(LLVMContext &, AttrKind) { return Attribute(); }
    static Attribute get(LLVMContext &, StringRef, StringRef = "") {
        return Attribute();
    }
};

class AttributeList {
public:
    AttributeList() = default;
    static AttributeList get(LLVMContext &, unsigned, Attribute::AttrKind) {
        return AttributeList();
    }
    AttributeList addAttribute(LLVMContext &, unsigned, Attribute::AttrKind) const {
        return *this;
    }
    AttributeList addAttribute(LLVMContext &, unsigned, StringRef, StringRef = "") const {
        return *this;
    }
    AttributeList addParamAttribute(LLVMContext &, unsigned, Attribute::AttrKind) const {
        return *this;
    }
};

class AttrBuilder {
public:
    AttrBuilder(LLVMContext &) {}
    AttrBuilder &addAttribute(Attribute::AttrKind) { return *this; }
    AttrBuilder &addAttribute(StringRef, StringRef = "") { return *this; }
    AttrBuilder &addAlignmentAttr(uint64_t) { return *this; }
    AttrBuilder &addByValAttr(Type *) { return *this; }
    AttrBuilder &addStructRetAttr(Type *) { return *this; }
    AttributeList getAttributes() const { return AttributeList(); }
};

} // namespace llvm

#endif
