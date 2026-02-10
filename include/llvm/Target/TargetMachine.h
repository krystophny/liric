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
namespace legacy { class PassManager; }

namespace detail {
struct __attribute__((visibility("hidden"))) ObjEmitState {
    raw_pwrite_stream *out = nullptr;
    CodeGenFileType file_type{};
};
inline thread_local ObjEmitState obj_emit_state;
} // namespace detail

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

    bool addPassesToEmitFile(legacy::PassManager &, raw_pwrite_stream &Out,
                             raw_pwrite_stream *,
                             CodeGenFileType FT, bool = true) {
        detail::obj_emit_state.out = &Out;
        detail::obj_emit_state.file_type = FT;
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
