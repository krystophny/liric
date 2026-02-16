#ifndef LLVM_EXECUTIONENGINE_ORC_THREADSAFEMODULE_H
#define LLVM_EXECUTIONENGINE_ORC_THREADSAFEMODULE_H

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <memory>
#include <mutex>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

namespace llvm {

namespace orc {

class ThreadSafeContext {
    std::shared_ptr<LLVMContext> Ctx;
    std::shared_ptr<std::mutex> Lock;

public:
    ThreadSafeContext()
        : Ctx(std::make_shared<LLVMContext>()),
          Lock(std::make_shared<std::mutex>()) {}

    ThreadSafeContext(std::unique_ptr<LLVMContext> C)
        : Ctx(std::move(C)),
          Lock(std::make_shared<std::mutex>()) {}

    LLVMContext *getContext() { return Ctx.get(); }
    std::mutex &getLock() { return *Lock; }
};

class ThreadSafeModule {
    std::unique_ptr<Module> M;
    ThreadSafeContext TSCtx;

public:
    ThreadSafeModule() = default;
    ThreadSafeModule(std::unique_ptr<Module> M_, ThreadSafeContext TSC)
        : M(std::move(M_)), TSCtx(std::move(TSC)) {}

    Module *getModuleUnlocked() { return M.get(); }
    const Module *getModuleUnlocked() const { return M.get(); }

    explicit operator bool() const { return M != nullptr; }

    template <typename Func>
    auto withModuleDo(Func &&F) -> decltype(F(*M)) {
        return F(*M);
    }

    template <typename Func>
    auto withModuleDo(Func &&F) const -> decltype(F(std::declval<const Module &>())) {
        return F(*M);
    }
};

} // namespace orc
} // namespace llvm

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
