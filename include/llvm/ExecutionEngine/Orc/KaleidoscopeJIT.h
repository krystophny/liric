#ifndef LLVM_EXECUTIONENGINE_ORC_KALEIDOSCOPEJIT_H
#define LLVM_EXECUTIONENGINE_ORC_KALEIDOSCOPEJIT_H

#include "llvm/Config/llvm-config.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/SelfExecutorProcessControl.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <memory>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

namespace llvm {
namespace orc {

class KaleidoscopeJIT {
    std::unique_ptr<LLJIT> lljit_;
    DataLayout dl_;
    std::vector<std::unique_ptr<Module>> owned_modules_;

public:
    KaleidoscopeJIT(std::unique_ptr<LLJIT> lljit, DataLayout dl)
        : lljit_(std::move(lljit)), dl_(std::move(dl)) {}

    static Expected<std::unique_ptr<KaleidoscopeJIT>> Create() {
        auto epc = SelfExecutorProcessControl::Create();
        if (!epc) {
            return epc.takeError();
        }
        JITTargetMachineBuilder jtmb((*epc)->getTargetTriple());
        auto dl = jtmb.getDefaultDataLayoutForTarget();
        if (!dl) {
            return dl.takeError();
        }
        auto lljit = std::make_unique<LLJIT>();
        return std::make_unique<KaleidoscopeJIT>(std::move(lljit), std::move(*dl));
    }

    const DataLayout &getDataLayout() const { return dl_; }

    Error addModule(std::unique_ptr<Module> M, std::unique_ptr<LLVMContext> &Ctx) {
        if (!M) {
            return make_error("KaleidoscopeJIT::addModule(): null module");
        }
        if (lljit_->addModule(*M) != 0) {
            return make_error("KaleidoscopeJIT::addModule(): LLJIT addModule failed");
        }
        owned_modules_.push_back(std::move(M));
        Ctx = std::make_unique<LLVMContext>();
        return Error::success();
    }

#if LLVM_VERSION_MAJOR < 17
    Expected<JITEvaluatedSymbol> lookup(StringRef Name) {
        void *addr = lljit_->lookup(Name);
        if (!addr) {
            return make_error("KaleidoscopeJIT::lookup(): symbol not found");
        }
        return JITEvaluatedSymbol((uint64_t)(uintptr_t)addr, llvm::JITSymbolFlags());
    }
#else
    Expected<ExecutorSymbolDef> lookup(StringRef Name) {
        void *addr = lljit_->lookup(Name);
        if (!addr) {
            return make_error("KaleidoscopeJIT::lookup(): symbol not found");
        }
        return ExecutorSymbolDef(ExecutorAddr::fromPtr(addr), llvm::orc::JITSymbolFlags());
    }
#endif
};

} // namespace orc
} // namespace llvm

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
