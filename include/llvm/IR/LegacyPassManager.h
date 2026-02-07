#ifndef LLVM_IR_LEGACYPASSMANAGER_H
#define LLVM_IR_LEGACYPASSMANAGER_H

namespace llvm {

class Module;
class Function;

class Pass {
public:
    virtual ~Pass() = default;
};

class FunctionPass : public Pass {};
class ModulePass : public Pass {};
class ImmutablePass : public Pass {};

namespace legacy {

class PassManager {
public:
    void add(Pass *P) { (void)P; }
    bool run(Module &M) { (void)M; return false; }
};

class FunctionPassManager {
public:
    explicit FunctionPassManager(Module *) {}
    void add(Pass *P) { (void)P; }
    bool doInitialization() { return false; }
    bool run(Function &F) { (void)F; return false; }
    bool doFinalization() { return false; }
};

} // namespace legacy
} // namespace llvm

#endif
