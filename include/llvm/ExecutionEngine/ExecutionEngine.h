#ifndef LLVM_EXECUTIONENGINE_EXECUTIONENGINE_H
#define LLVM_EXECUTIONENGINE_EXECUTIONENGINE_H

#include "llvm/Config/llvm-config.h"
#include <string>

extern "C" {
inline const char *LLVMGetDefaultTargetTriple() {
#ifdef LLVM_DEFAULT_TARGET_TRIPLE
    return LLVM_DEFAULT_TARGET_TRIPLE;
#else
    return "";
#endif
}
}

namespace llvm {
namespace sys {

inline std::string getDefaultTargetTriple() {
#ifdef LLVM_DEFAULT_TARGET_TRIPLE
    return LLVM_DEFAULT_TARGET_TRIPLE;
#else
    return "";
#endif
}

inline std::string getHostCPUName() { return "generic"; }

} // namespace sys
} // namespace llvm

#endif
