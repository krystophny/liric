#ifndef LLVM_EXECUTIONENGINE_ORC_COMPILEUTILS_H
#define LLVM_EXECUTIONENGINE_ORC_COMPILEUTILS_H

#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include <memory>

namespace llvm {

class Module;
class TargetMachine;

namespace orc {

class SimpleCompiler : public IRCompileLayer::IRCompiler {
public:
    SimpleCompiler(TargetMachine &TM) { (void)TM; }

    Expected<std::unique_ptr<MemoryBuffer>> operator()(Module &M) override {
        (void)M;
        return std::make_unique<MemoryBuffer>("", "");
    }
};

class ConcurrentIRCompiler : public IRCompileLayer::IRCompiler {
public:
    ConcurrentIRCompiler(JITTargetMachineBuilder) {}

    Expected<std::unique_ptr<MemoryBuffer>> operator()(Module &M) override {
        (void)M;
        return std::make_unique<MemoryBuffer>("", "");
    }
};

} // namespace orc
} // namespace llvm

#endif
