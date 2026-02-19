#ifndef LLVM_IR_GLOBALVARIABLE_H
#define LLVM_IR_GLOBALVARIABLE_H

#include "llvm/IR/GlobalValue.h"
#include "llvm/ADT/StringRef.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

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
    ~GlobalVariable() {
        detail::unregister_value_wrapper(this);
        detail::unregister_global_value_state(this);
    }

    GlobalVariable(Module &M, Type *Ty, bool isConstant,
                   LinkageTypes Linkage, Constant *Initializer = nullptr,
                   const Twine &Name = "", GlobalVariable *InsertBefore = nullptr,
                   ThreadLocalMode TLMode = NotThreadLocal,
                   unsigned AddressSpace = 0);

    bool isConstant() const;
    void setConstant(bool v);

    bool hasInitializer() const;
    Constant *getInitializer() const;
    void setInitializer(Constant *InitVal);

    template <typename AlignTy>
    void setAlignment(AlignTy A) { (void)A; }
};

} // namespace llvm

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
