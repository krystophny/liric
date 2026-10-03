#ifndef LLVM_TRANSFORMS_UTILS_MODULEUTILS_H
#define LLVM_TRANSFORMS_UTILS_MODULEUTILS_H

#include "llvm/IR/Module.h"
#include <stdexcept>

namespace liric_llvm {
inline void appendToGlobalCtors(Module &, Function *, int, Constant * = nullptr) {
    throw std::runtime_error("liric: global constructors are not supported");
}
}
#endif
