#ifndef LLVM_PASSES_PASSBUILDER_H
#define LLVM_PASSES_PASSBUILDER_H

namespace llvm {

class Module;
class Function;
class TargetMachine;

class FunctionAnalysisManager {
public:
    template <typename T> void registerPass(T &&) {}
    bool empty() const { return true; }
    void clear() {}
};

class ModuleAnalysisManager {
public:
    template <typename T> void registerPass(T &&) {}
    bool empty() const { return true; }
    void clear() {}
};

class CGSCCAnalysisManager {
public:
    template <typename T> void registerPass(T &&) {}
    bool empty() const { return true; }
    void clear() {}
};

class LoopAnalysisManager {
public:
    template <typename T> void registerPass(T &&) {}
    bool empty() const { return true; }
    void clear() {}
};

class FunctionPassManager {
public:
    template <typename T> void addPass(T &&) {}
    void run(Function &, FunctionAnalysisManager &) {}
};

class ModulePassManager {
public:
    template <typename T> void addPass(T &&) {}
    void run(Module &, ModuleAnalysisManager &) {}
};

class PassBuilder {
public:
    PassBuilder() = default;
    PassBuilder(TargetMachine *) {}

    void registerModuleAnalyses(ModuleAnalysisManager &) {}
    void registerCGSCCAnalyses(CGSCCAnalysisManager &) {}
    void registerFunctionAnalyses(FunctionAnalysisManager &) {}
    void registerLoopAnalyses(LoopAnalysisManager &) {}
    void crossRegisterProxies(LoopAnalysisManager &,
                              FunctionAnalysisManager &,
                              CGSCCAnalysisManager &,
                              ModuleAnalysisManager &) {}

    ModulePassManager buildPerModuleDefaultPipeline(int) {
        return ModulePassManager();
    }
    ModulePassManager buildO0DefaultPipeline(int = 0, bool = false) {
        return ModulePassManager();
    }
    FunctionPassManager buildFunctionSimplificationPipeline(int, int) {
        return FunctionPassManager();
    }
};

} // namespace llvm

#endif
