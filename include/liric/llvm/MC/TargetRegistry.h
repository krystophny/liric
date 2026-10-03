#ifndef LLVM_MC_TARGETREGISTRY_H
#define LLVM_MC_TARGETREGISTRY_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include <memory>
#include <optional>
#include <string>

namespace liric_llvm {

class raw_ostream;
class TargetMachine;
class TargetOptions;

class Target {
public:
    const char *getName() const { return "liric"; }
    const char *getShortDescription() const { return "liric JIT target"; }

    TargetMachine *createTargetMachine(
        const Triple &TT, StringRef CPU, StringRef Features,
        const TargetOptions &Options,
        std::optional<Reloc::Model> RM = std::nullopt,
        std::optional<CodeModel::Model> CM = std::nullopt,
        CodeGenOptLevel OL = CodeGenOptLevel::Default,
        bool JIT = false) const;
    TargetMachine *createTargetMachine(
        StringRef TT, StringRef CPU, StringRef Features,
        const TargetOptions &Options,
        std::optional<Reloc::Model> RM = std::nullopt,
        std::optional<CodeModel::Model> CM = std::nullopt,
        CodeGenOptLevel OL = CodeGenOptLevel::Default,
        bool JIT = false) const;
    MCSubtargetInfo *createMCSubtargetInfo(StringRef TT, StringRef CPU,
                                         StringRef Features) const;

};

struct TargetRegistry {
    static bool isNative(const Triple &triple) {
#if (defined(__linux__) || defined(__APPLE__)) && defined(LLVM_DEFAULT_TARGET_TRIPLE)
        Triple native(LLVM_DEFAULT_TARGET_TRIPLE);
        return triple.hasSupportedComponents()
            && triple.getArch() != Triple::UnknownArch
            && triple.getOS() != Triple::UnknownOS
            && triple.getArch() == native.getArch()
            && triple.getOS() == native.getOS()
            && triple.getEnvironment() == native.getEnvironment()
            && triple.getObjectFormat() == native.getObjectFormat();
#else
        (void)triple;
        return false;
#endif
    }
    static const Target *lookupTarget(const Triple &triple, std::string &error) {
        if (!isNative(triple)) {
            error = "liric: target '" + triple.str() + "' is not supported by this native backend";
            return nullptr;
        }
        error.clear();
        static Target target;
        return &target;
    }
    static const Target *lookupTarget(const std::string &triple, std::string &error) {
        return lookupTarget(Triple(triple), error);
    }
    static const Target *lookupTarget(StringRef arch, Triple &triple,
                                     std::string &error) {
        if (!arch.empty() && arch.str() != triple.getArchName().str()) {
            error = "liric: requested architecture does not match the native target";
            return nullptr;
        }
        return lookupTarget(triple, error);
    }

    static void printRegisteredTargetsForVersion(raw_ostream &OS) {
        OS << "  liric - liric JIT target\n";
    }
};

} // namespace liric_llvm

#include "llvm/Target/TargetMachine.h"

#endif
