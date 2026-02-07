#ifndef LLVM_EXECUTIONENGINE_ORC_SHARED_EXECUTORADDRESS_H
#define LLVM_EXECUTIONENGINE_ORC_SHARED_EXECUTORADDRESS_H

#include <cstdint>

namespace llvm {
namespace orc {

class ExecutorAddr {
    uint64_t Addr = 0;

public:
    ExecutorAddr() = default;
    explicit ExecutorAddr(uint64_t A) : Addr(A) {}

    uint64_t getValue() const { return Addr; }

    template <typename T>
    T toPtr() const {
        return reinterpret_cast<T>(static_cast<uintptr_t>(Addr));
    }

    static ExecutorAddr fromPtr(void *P) {
        return ExecutorAddr(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(P)));
    }

    explicit operator bool() const { return Addr != 0; }
    bool operator==(const ExecutorAddr &O) const { return Addr == O.Addr; }
    bool operator!=(const ExecutorAddr &O) const { return Addr != O.Addr; }
    bool operator<(const ExecutorAddr &O) const { return Addr < O.Addr; }
};

} // namespace orc
} // namespace llvm

#endif
