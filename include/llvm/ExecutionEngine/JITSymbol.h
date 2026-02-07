#ifndef LLVM_EXECUTIONENGINE_JITSYMBOL_H
#define LLVM_EXECUTIONENGINE_JITSYMBOL_H

#include <cstdint>

namespace llvm {

class JITSymbolFlags {
    uint8_t Flags_ = 0;

public:
    JITSymbolFlags() = default;
    explicit JITSymbolFlags(uint8_t F) : Flags_(F) {}

    enum FlagNames : uint8_t {
        None = 0,
        HasError = 1 << 0,
        Weak = 1 << 1,
        Exported = 1 << 4,
    };
};

class JITEvaluatedSymbol {
    uint64_t Address = 0;
    JITSymbolFlags Flags;

public:
    JITEvaluatedSymbol() = default;
    JITEvaluatedSymbol(uint64_t Addr, JITSymbolFlags F)
        : Address(Addr), Flags(F) {}

    uint64_t getAddress() const { return Address; }
    JITSymbolFlags getFlags() const { return Flags; }
    explicit operator bool() const { return Address != 0; }
};

class JITSymbol {
    uint64_t Address = 0;
    JITSymbolFlags Flags;

public:
    JITSymbol() = default;
    JITSymbol(uint64_t Addr, JITSymbolFlags F) : Address(Addr), Flags(F) {}
    JITSymbol(JITEvaluatedSymbol ES)
        : Address(ES.getAddress()), Flags(ES.getFlags()) {}

    uint64_t getAddress() const { return Address; }
    JITSymbolFlags getFlags() const { return Flags; }
    explicit operator bool() const { return Address != 0; }
};

} // namespace llvm

#endif
