#ifndef LLVM_TRANSFORMS_IPO_H
#define LLVM_TRANSFORMS_IPO_H

namespace llvm {

class Pass;

inline Pass *createFunctionInliningPass() { return nullptr; }

inline Pass *createFunctionInliningPass(unsigned OptLevel) {
    (void)OptLevel;
    return nullptr;
}

} // namespace llvm

#endif
