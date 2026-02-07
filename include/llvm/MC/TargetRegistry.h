#ifndef LLVM_MC_TARGETREGISTRY_H
#define LLVM_MC_TARGETREGISTRY_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/TargetParser/Triple.h"
#include <memory>
#include <optional>
#include <string>

namespace llvm {

class raw_ostream;
class TargetMachine;
class TargetOptions;

class Target {
public:
    const char *getName() const { return "liric"; }
    const char *getShortDescription() const { return "liric JIT target"; }

    TargetMachine *createTargetMachine(
        StringRef TT, StringRef CPU, StringRef Features,
        const TargetOptions &Options,
        std::optional<Reloc::Model> RM = std::nullopt,
        std::optional<CodeModel> CM = std::nullopt,
        CodeGenOptLevel OL = CodeGenOptLevel::Default,
        bool JIT = false) const {
        (void)TT; (void)CPU; (void)Features; (void)Options;
        (void)RM; (void)CM; (void)OL; (void)JIT;
        return nullptr;
    }
};

struct TargetRegistry {
    static const Target *lookupTarget(const std::string &Triple,
                                       std::string &Error) {
        (void)Triple;
        Error.clear();
        static Target t;
        return &t;
    }

    static const Target *lookupTarget(StringRef ArchName,
                                       Triple &TheTriple,
                                       std::string &Error) {
        (void)ArchName; (void)TheTriple;
        Error.clear();
        static Target t;
        return &t;
    }

    static void printRegisteredTargetsForVersion(raw_ostream &OS) {
        OS << "  liric - liric JIT target\n";
    }
};

} // namespace llvm

#endif
