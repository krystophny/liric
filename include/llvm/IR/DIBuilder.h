#ifndef LLVM_IR_DIBUILDER_H
#define LLVM_IR_DIBUILDER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/LLVMContext.h"
#include <cstdint>

namespace llvm {

class Module;
class Function;
class BasicBlock;
class Instruction;
class Type;
class LLVMContext;

class Metadata {};

class DINode {
public:
    enum DIFlags {
        FlagZero = 0,
        FlagPrototyped = (1 << 8),
    };
};

class DIScope {
public:
    LLVMContext &getContext() {
        static LLVMContext dummy;
        return dummy;
    }
};

class DIType : public DIScope, public Metadata {};
class DIBasicType : public DIType {};
class DIDerivedType : public DIType {};
class DICompositeType : public DIType {};
class DISubroutineType : public DIType {};
class DIFile : public DIScope {};

class DICompileUnit : public DIScope {
public:
    StringRef getFilename() const { return ""; }
    StringRef getDirectory() const { return ""; }
};

class DISubprogram : public DIScope {
public:
    enum DISPFlags {
        SPFlagZero = 0,
        SPFlagDefinition = (1 << 1),
    };
};

class DILocalVariable {};
class DIGlobalVariableExpression {};
class DIExpression {};

class DILocation {
public:
    static DILocation *get(LLVMContext &Ctx, unsigned Line, unsigned Col,
                           DIScope *Scope, DILocation *InlinedAt = nullptr) {
        (void)Ctx; (void)Line; (void)Col; (void)Scope; (void)InlinedAt;
        return nullptr;
    }
};

class DILexicalBlock : public DIScope {};

class DITypeRefArray {
public:
    DITypeRefArray() = default;
};

class DebugLoc {
public:
    DebugLoc() = default;
    DebugLoc(const DILocation *) {}
    static DebugLoc get(unsigned, unsigned, const DIScope *,
                        const DILocation * = nullptr) {
        return DebugLoc();
    }
    operator bool() const { return false; }
};

class DIBuilder {
public:
    explicit DIBuilder(Module &) {}

    void finalize() {}

    DICompileUnit *createCompileUnit(unsigned, DIFile *, StringRef, bool,
                                     StringRef, unsigned,
                                     StringRef = "",
                                     unsigned = 0,
                                     bool = false) {
        return nullptr;
    }

    DIFile *createFile(StringRef, StringRef) { return nullptr; }

    DIBasicType *createBasicType(StringRef, uint64_t, unsigned) {
        return nullptr;
    }

    DIDerivedType *createPointerType(DIType *, uint64_t,
                                     unsigned = 0, unsigned = 0,
                                     StringRef = "") {
        return nullptr;
    }

    DIDerivedType *createMemberType(DIScope *, StringRef, DIFile *,
                                    unsigned, uint64_t, unsigned,
                                    uint64_t, unsigned, DIType *) {
        return nullptr;
    }

    DICompositeType *createStructType(DIScope *, StringRef, DIFile *,
                                      unsigned, uint64_t, unsigned,
                                      unsigned, DIType *, void *) {
        return nullptr;
    }

    DICompositeType *createArrayType(uint64_t, unsigned, DIType *, void *) {
        return nullptr;
    }

    DISubroutineType *createSubroutineType(void *) { return nullptr; }

    DISubroutineType *createSubroutineType(DITypeRefArray) {
        return nullptr;
    }

    DISubprogram *createFunction(DIScope *, StringRef, StringRef,
                                 DIFile *, unsigned, DISubroutineType *,
                                 unsigned, unsigned, unsigned) {
        return nullptr;
    }

    DILocalVariable *createAutoVariable(DIScope *, StringRef, DIFile *,
                                        unsigned, DIType *, bool = false,
                                        unsigned = 0, unsigned = 0) {
        return nullptr;
    }

    DILocalVariable *createParameterVariable(DIScope *, StringRef, unsigned,
                                             DIFile *, unsigned, DIType *,
                                             bool = false, unsigned = 0) {
        return nullptr;
    }

    DIGlobalVariableExpression *createGlobalVariableExpression(
        DIScope *, StringRef, StringRef, DIFile *, unsigned, DIType *,
        bool, bool = false, DIExpression * = nullptr, void * = nullptr,
        unsigned = 0) {
        return nullptr;
    }

    DILexicalBlock *createLexicalBlock(DIScope *, DIFile *, unsigned,
                                       unsigned) {
        return nullptr;
    }

    DIExpression *createExpression() { return nullptr; }

    DITypeRefArray getOrCreateTypeArray(ArrayRef<Metadata *> Elements) {
        (void)Elements;
        return DITypeRefArray();
    }

    Instruction *insertDeclare(Value *, DILocalVariable *, DIExpression *,
                               const DebugLoc &, BasicBlock *) {
        return nullptr;
    }

    Instruction *insertDbgValueIntrinsic(Value *, DILocalVariable *,
                                         DIExpression *, const DebugLoc &,
                                         BasicBlock *) {
        return nullptr;
    }
};

namespace dwarf {
enum {
    DW_ATE_address = 1,
    DW_ATE_boolean = 2,
    DW_ATE_complex_float = 3,
    DW_ATE_float = 4,
    DW_ATE_signed = 5,
    DW_ATE_signed_char = 6,
    DW_ATE_unsigned = 7,
    DW_ATE_unsigned_char = 8,
    DW_LANG_C = 0x0001,
    DW_LANG_Fortran95 = 0x0018,
    DW_LANG_Fortran03 = 0x0022,
    DW_LANG_Fortran08 = 0x0023,
};
} // namespace dwarf

} // namespace llvm

#endif
