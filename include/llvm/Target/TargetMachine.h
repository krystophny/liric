#ifndef LLVM_TARGET_TARGETMACHINE_H
#define LLVM_TARGET_TARGETMACHINE_H

#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/Error.h"
#include "llvm/MC/TargetRegistry.h"
#include <string>

namespace llvm {

class Module;
class raw_ostream;
class raw_pwrite_stream;

class TargetMachine {
public:
    virtual ~TargetMachine() = default;

    const DataLayout &getDataLayout() const {
        static DataLayout dl;
        return dl;
    }

    DataLayout createDataLayout() const { return DataLayout(); }

    const Triple &getTargetTriple() const {
        static Triple t;
        return t;
    }

    void setFastISel(bool) {}

    bool addPassesToEmitFile(void *, raw_pwrite_stream &, raw_pwrite_stream *,
                             CodeGenFileType, bool = true, void * = nullptr) {
        return false;
    }

    const Target &getTarget() const {
        static Target t;
        return t;
    }

    TargetOptions Options;
};

inline std::string sys_getDefaultTargetTriple() { return ""; }
inline std::string sys_getHostCPUName() { return "generic"; }

} // namespace llvm

#endif
