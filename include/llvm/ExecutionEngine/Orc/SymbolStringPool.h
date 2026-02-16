#ifndef LLVM_EXECUTIONENGINE_ORC_SYMBOLSTRINGPOOL_H
#define LLVM_EXECUTIONENGINE_ORC_SYMBOLSTRINGPOOL_H

#include <string>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

namespace llvm {
namespace orc {

class SymbolStringPool {};

class SymbolStringPtr {
    std::string Name;

public:
    SymbolStringPtr() = default;
    SymbolStringPtr(const std::string &N) : Name(N) {}

    const std::string &operator*() const { return Name; }
    bool operator==(const SymbolStringPtr &O) const { return Name == O.Name; }
    bool operator!=(const SymbolStringPtr &O) const { return Name != O.Name; }
    bool operator<(const SymbolStringPtr &O) const { return Name < O.Name; }
};

} // namespace orc
} // namespace llvm

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
