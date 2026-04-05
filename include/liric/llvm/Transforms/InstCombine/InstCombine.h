#ifndef LLVM_TRANSFORMS_INSTCOMBINE_INSTCOMBINE_H
#define LLVM_TRANSFORMS_INSTCOMBINE_INSTCOMBINE_H

namespace liric_llvm {

class InstCombinePass {
public:
    template <typename IR, typename AM>
    void run(IR &, AM &) {}
};

} // namespace liric_llvm

#endif
