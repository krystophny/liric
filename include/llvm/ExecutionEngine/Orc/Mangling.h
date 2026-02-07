#ifndef LLVM_EXECUTIONENGINE_ORC_MANGLING_H
#define LLVM_EXECUTIONENGINE_ORC_MANGLING_H

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {

class DataLayout;

namespace orc {

class MangleAndInterner {
    ExecutionSession &ES_;

public:
    MangleAndInterner(ExecutionSession &ES, const DataLayout &DL)
        : ES_(ES) { (void)DL; }

    SymbolStringPtr operator()(StringRef Name) {
        return ES_.intern(Name);
    }
};

} // namespace orc
} // namespace llvm

#endif
