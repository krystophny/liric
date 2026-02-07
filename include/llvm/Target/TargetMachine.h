#ifndef LLVM_TARGET_TARGETMACHINE_H
#define LLVM_TARGET_TARGETMACHINE_H

#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace llvm {

class Module;
class raw_ostream;
class raw_pwrite_stream;

class TargetOptions {
public:
    bool UnsafeFPMath = false;
    bool NoInfsFPMath = false;
    bool NoNaNsFPMath = false;
    bool HonorSignDependentRoundingFPMathOption = false;
    bool NoZerosInBSS = false;
    bool GuaranteedTailCallOpt = false;
    bool StackAlignmentOverride = false;
    bool EnableFastISel = false;
};

class Target {
public:
    const char *getName() const { return "liric"; }
    const char *getShortDescription() const { return "liric JIT target"; }

    TargetMachine *createTargetMachine(StringRef, StringRef, StringRef,
                                       const TargetOptions &, int,
                                       int = 0, int = 0) const {
        return nullptr;
    }
};

class TargetMachine {
public:
    virtual ~TargetMachine() = default;

    const DataLayout &getDataLayout() const {
        static DataLayout dl;
        return dl;
    }

    StringRef getTargetTriple() const { return ""; }

    void setFastISel(bool) {}

    bool addPassesToEmitFile(void *, raw_pwrite_stream &, void *, int,
                             bool = true, void * = nullptr) {
        return false;
    }

    const Target &getTarget() const {
        static Target t;
        return t;
    }
};

namespace TargetRegistry {
    inline const Target *lookupTarget(StringRef, std::string &) {
        return nullptr;
    }
}

inline std::string sys_getDefaultTargetTriple() { return ""; }
inline std::string sys_getHostCPUName() { return "generic"; }

} // namespace llvm

#endif
