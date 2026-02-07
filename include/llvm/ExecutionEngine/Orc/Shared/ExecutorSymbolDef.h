#ifndef LLVM_EXECUTIONENGINE_ORC_SHARED_EXECUTORSYMBOLDEF_H
#define LLVM_EXECUTIONENGINE_ORC_SHARED_EXECUTORSYMBOLDEF_H

#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"

namespace llvm {
namespace orc {

class JITSymbolFlags {
    uint8_t Flags = 0;

public:
    JITSymbolFlags() = default;
    explicit JITSymbolFlags(uint8_t F) : Flags(F) {}

    enum FlagNames : uint8_t {
        None = 0,
        HasError = 1 << 0,
        Weak = 1 << 1,
        Common = 1 << 2,
        Absolute = 1 << 3,
        Exported = 1 << 4,
        Callable = 1 << 5,
    };

    bool operator==(const JITSymbolFlags &O) const { return Flags == O.Flags; }
};

class ExecutorSymbolDef {
    ExecutorAddr Addr;
    JITSymbolFlags Flags;

public:
    ExecutorSymbolDef() = default;
    ExecutorSymbolDef(ExecutorAddr A, JITSymbolFlags F) : Addr(A), Flags(F) {}

    const ExecutorAddr &getAddress() const { return Addr; }
    const JITSymbolFlags &getFlags() const { return Flags; }
};

} // namespace orc
} // namespace llvm

#endif
