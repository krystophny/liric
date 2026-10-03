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

namespace liric_llvm {

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
    Triple triple_;
    DataLayout layout_;
public:
    TargetMachine(): TargetMachine(Triple(LLVM_DEFAULT_TARGET_TRIPLE)) {}
    explicit TargetMachine(const Triple &triple): triple_(triple),
        layout_(triple.isAArch64()
            ? "e-p:64:64-i64:64-i128:128-n32:64-S128"
            : "e-p:64:64-i64:64-f80:128-n8:16:32:64-S128") {}
    virtual ~TargetMachine() = default;

    const DataLayout &getDataLayout() const {
        return layout_;
    }

    DataLayout createDataLayout() const { return layout_; }

    const Triple &getTargetTriple() const {
        return triple_;
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

inline TargetMachine *Target::createTargetMachine(
    const Triple &triple, StringRef cpu, StringRef features,
    const TargetOptions &options, std::optional<Reloc::Model> relocation,
    std::optional<CodeModel::Model> code_model, CodeGenOptLevel level, bool jit) const {
    (void)level; (void)jit;
    if (!TargetRegistry::isNative(triple)
            || (!cpu.empty() && cpu.str() != "generic") || !features.empty()
            || (code_model && *code_model != CodeModel::Small)
            || (relocation && *relocation != Reloc::Static && *relocation != Reloc::PIC_))
        return nullptr;
    auto *machine = new TargetMachine(triple);
    machine->Options = options;
    return machine;
}
inline TargetMachine *Target::createTargetMachine(
    StringRef triple, StringRef cpu, StringRef features,
    const TargetOptions &options, std::optional<Reloc::Model> relocation,
    std::optional<CodeModel::Model> code_model, CodeGenOptLevel level, bool jit) const {
    return createTargetMachine(Triple(triple), cpu, features, options,
        relocation, code_model, level, jit);
}
inline MCSubtargetInfo *Target::createMCSubtargetInfo(
    StringRef triple, StringRef cpu, StringRef features) const {
    if (!TargetRegistry::isNative(Triple(triple))
            || (!cpu.empty() && cpu.str() != "generic") || !features.empty()) return nullptr;
    return new MCSubtargetInfo();
}

} // namespace liric_llvm
#endif
