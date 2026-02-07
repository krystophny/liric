#ifndef LLVM_IR_GLOBALVARIABLE_H
#define LLVM_IR_GLOBALVARIABLE_H

#include "llvm/IR/GlobalValue.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {

class Constant;
class Module;

class GlobalVariable : public GlobalValue {
public:
    enum ThreadLocalMode {
        NotThreadLocal = 0,
        GeneralDynamicTLSModel,
        LocalDynamicTLSModel,
        InitialExecTLSModel,
        LocalExecTLSModel,
    };

    GlobalVariable() = default;

    GlobalVariable(Module &M, Type *Ty, bool isConstant,
                   LinkageTypes Linkage, Constant *Initializer = nullptr,
                   const Twine &Name = "", GlobalVariable *InsertBefore = nullptr,
                   ThreadLocalMode TLMode = NotThreadLocal,
                   unsigned AddressSpace = 0);

    bool isConstant() const { return false; }
    void setConstant(bool v) { (void)v; }

    bool hasInitializer() const { return false; }
    Constant *getInitializer() const { return nullptr; }
    void setInitializer(Constant *InitVal) { (void)InitVal; }

    template <typename AlignTy>
    void setAlignment(AlignTy A) { (void)A; }
};

} // namespace llvm

#endif
