#ifndef LLVM_EXECUTIONENGINE_ORC_JITTARGETMACHINEBUILDER_H
#define LLVM_EXECUTIONENGINE_ORC_JITTARGETMACHINEBUILDER_H

#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Error.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Target/TargetMachine.h"
#include <memory>

namespace liric_llvm {
namespace orc {

class JITTargetMachineBuilder {
    Triple TT;
    std::optional<Reloc::Model> relocation_;
    std::optional<CodeModel::Model> code_model_;

public:
    JITTargetMachineBuilder(Triple T) : TT(std::move(T)) {}

    static Expected<JITTargetMachineBuilder> detectHost() {
        return JITTargetMachineBuilder(Triple(LLVM_DEFAULT_TARGET_TRIPLE));
    }

    JITTargetMachineBuilder &setRelocationModel(Reloc::Model RM) {
        relocation_ = RM;
        return *this;
    }

    JITTargetMachineBuilder &setCodeModel(CodeModel::Model CM) {
        code_model_ = CM;
        return *this;
    }

    const Triple &getTargetTriple() const { return TT; }

    Expected<DataLayout> getDefaultDataLayoutForTarget() const {
        auto machine = createTargetMachine();
        if (!machine) return machine.takeError();
        return (*machine)->createDataLayout();
    }

    Expected<std::unique_ptr<TargetMachine>> createTargetMachine() const {
        std::string error;
        const Target *target = TargetRegistry::lookupTarget(TT, error);
        if (!target) return make_error(error);
        std::unique_ptr<TargetMachine> machine(target->createTargetMachine(
            TT, "generic", "", TargetOptions(), relocation_, code_model_));
        if (!machine) return make_error("liric: requested target options are not supported");
        return machine;
    }
};

} // namespace orc
} // namespace liric_llvm

#endif
