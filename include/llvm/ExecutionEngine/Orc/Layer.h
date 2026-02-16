#ifndef LLVM_EXECUTIONENGINE_ORC_LAYER_H
#define LLVM_EXECUTIONENGINE_ORC_LAYER_H

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/Mangling.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/Error.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

namespace llvm {
namespace orc {

class IRLayer {
public:
    virtual ~IRLayer() = default;

    virtual Error add(JITDylib &JD, ThreadSafeModule TSM) {
        (void)JD;
        (void)TSM;
        return Error::success();
    }
};

class ObjectLayer {
public:
    virtual ~ObjectLayer() = default;
};

} // namespace orc
} // namespace llvm

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
