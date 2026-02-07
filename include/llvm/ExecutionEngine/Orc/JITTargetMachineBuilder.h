#ifndef LLVM_EXECUTIONENGINE_ORC_JITTARGETMACHINEBUILDER_H
#define LLVM_EXECUTIONENGINE_ORC_JITTARGETMACHINEBUILDER_H

#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Target/TargetMachine.h"
#include <memory>

namespace llvm {
namespace orc {

class JITTargetMachineBuilder {
    Triple TT;

public:
    JITTargetMachineBuilder(Triple T) : TT(std::move(T)) {}

    static Expected<JITTargetMachineBuilder> detectHost() {
        return JITTargetMachineBuilder(Triple());
    }

    JITTargetMachineBuilder &setRelocationModel(Reloc::Model RM) {
        (void)RM;
        return *this;
    }

    JITTargetMachineBuilder &setCodeModel(CodeModel CM) {
        (void)CM;
        return *this;
    }

    const Triple &getTargetTriple() const { return TT; }

    Expected<std::unique_ptr<TargetMachine>> createTargetMachine() const {
        return std::make_unique<TargetMachine>();
    }
};

} // namespace orc
} // namespace llvm

#endif
