#ifndef LLVM_EXECUTIONENGINE_GENERICVALUE_H
#define LLVM_EXECUTIONENGINE_GENERICVALUE_H

#include <cstdint>

namespace llvm {

struct GenericValue {
    union {
        double DoubleVal;
        float FloatVal;
        uint64_t IntVal;
        void *PointerVal;
    };
    GenericValue() : IntVal(0) {}
};

} // namespace llvm

#endif
