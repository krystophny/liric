#ifndef LLVM_EXECUTIONENGINE_ORC_SELFEXECUTORPROCESSCONTROL_H
#define LLVM_EXECUTIONENGINE_ORC_SELFEXECUTORPROCESSCONTROL_H

#include "llvm/ExecutionEngine/Orc/ExecutorProcessControl.h"
#include "llvm/Support/Error.h"
#include <memory>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

namespace llvm {
namespace orc {

class SelfExecutorProcessControl : public ExecutorProcessControl {
public:
    SelfExecutorProcessControl() {
        SSP = std::make_shared<SymbolStringPool>();
    }

    static Expected<std::unique_ptr<SelfExecutorProcessControl>> Create() {
        auto SEPC = std::make_unique<SelfExecutorProcessControl>();
        return Expected<std::unique_ptr<SelfExecutorProcessControl>>(
            std::move(SEPC));
    }
};

} // namespace orc
} // namespace llvm

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
