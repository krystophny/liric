#ifndef LLVM_IR_VERIFIER_H
#define LLVM_IR_VERIFIER_H

namespace llvm {

class Module;
class Function;
class raw_ostream;

inline bool verifyModule(const Module &M, raw_ostream *OS = nullptr) {
    (void)M; (void)OS;
    return false;
}

inline bool verifyFunction(const Function &F, raw_ostream *OS = nullptr) {
    (void)F; (void)OS;
    return false;
}

class VerifierPass {
public:
    bool run(Module &, void *) { return false; }
    bool run(Function &, void *) { return false; }
};

} // namespace llvm

#endif
