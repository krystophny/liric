#ifndef LLVM_EXECUTIONENGINE_ORC_EXECUTORPROCESSCONTROL_H
#define LLVM_EXECUTIONENGINE_ORC_EXECUTORPROCESSCONTROL_H

#include "llvm/TargetParser/Triple.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include <memory>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

namespace llvm {
namespace orc {

class ExecutorProcessControl {
protected:
    Triple TargetTriple;
    std::shared_ptr<SymbolStringPool> SSP;

public:
    virtual ~ExecutorProcessControl() = default;

    const Triple &getTargetTriple() const { return TargetTriple; }

    std::shared_ptr<SymbolStringPool> getSymbolStringPool() const {
        return SSP;
    }
};

} // namespace orc
} // namespace llvm

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
