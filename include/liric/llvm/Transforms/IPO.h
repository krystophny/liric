#ifndef LLVM_TRANSFORMS_IPO_H
#define LLVM_TRANSFORMS_IPO_H

namespace liric_llvm {

class Pass;

inline Pass *createFunctionInliningPass() { return nullptr; }

inline Pass *createFunctionInliningPass(unsigned OptLevel) {
    (void)OptLevel;
    return nullptr;
}

} // namespace liric_llvm

#endif
