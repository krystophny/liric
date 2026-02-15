#ifndef LLVM_EXECUTIONENGINE_ORC_IRCOMPILELAYER_H
#define LLVM_EXECUTIONENGINE_ORC_IRCOMPILELAYER_H

#include <liric/liric_compat.h>
#include "llvm/ExecutionEngine/Orc/Layer.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include <memory>

namespace llvm {

class Module;

namespace orc {

class IRCompileLayer : public IRLayer {
public:
    class IRCompiler {
    public:
        virtual ~IRCompiler() = default;
        virtual Expected<std::unique_ptr<MemoryBuffer>>
        operator()(Module &M) = 0;
    };

private:
    ExecutionSession &ES;
    ObjectLayer &BaseLayer;
    std::unique_ptr<IRCompiler> Compile;

public:
    IRCompileLayer(ExecutionSession &ES_, ObjectLayer &BaseLayer_,
                   std::unique_ptr<IRCompiler> Compile_)
        : ES(ES_), BaseLayer(BaseLayer_), Compile(std::move(Compile_)) {
        (void)BaseLayer;
    }

    inline Error add(JITDylib &JD, ThreadSafeModule TSM) override;
};

} // namespace orc
} // namespace llvm

#include "llvm/IR/Module.h"

inline llvm::Error llvm::orc::IRCompileLayer::add(
    llvm::orc::JITDylib &JD, llvm::orc::ThreadSafeModule TSM) {
    (void)JD;
    Module *M = TSM.getModuleUnlocked();
    if (!M) return make_error("Null module");
    int rc = ES.addCompatModule(M->getCompat());
    if (rc != 0) return make_error("LLVMLiricSessionAddCompatModule failed");
    return Error::success();
}

#endif
