#ifndef LLVM_EXECUTIONENGINE_EXECUTIONENGINE_H
#define LLVM_EXECUTIONENGINE_EXECUTIONENGINE_H

#include "llvm/Config/llvm-config.h"
#include "llvm/ADT/StringMap.h"
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

namespace liric_llvm {
namespace sys {

inline std::string getDefaultTargetTriple() {
#ifdef LLVM_DEFAULT_TARGET_TRIPLE
    return LLVM_DEFAULT_TARGET_TRIPLE;
#else
    return "";
#endif
}

inline StringRef getHostCPUName() { return "generic"; }

// LLVM permits an empty map when host feature detection is unavailable.
inline const StringMap<bool, MallocAllocator> getHostCPUFeatures() { return {}; }
inline bool getHostCPUFeatures(StringMap<bool, MallocAllocator> &features) {
    features = {};
    return false;
}

} // namespace sys
} // namespace liric_llvm

#endif
