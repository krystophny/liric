#ifndef LLVM_EXECUTIONENGINE_GENERICVALUE_H
#define LLVM_EXECUTIONENGINE_GENERICVALUE_H

#include <cstdint>

namespace liric_llvm {

struct GenericValue {
    union {
        double DoubleVal;
        float FloatVal;
        uint64_t IntVal;
        void *PointerVal;
    };
    GenericValue() : IntVal(0) {}
};

} // namespace liric_llvm

#endif
