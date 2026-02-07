#ifndef LLVM_TRANSFORMS_INSTCOMBINE_INSTCOMBINE_H
#define LLVM_TRANSFORMS_INSTCOMBINE_INSTCOMBINE_H

namespace llvm {

class InstCombinePass {
public:
    template <typename IR, typename AM>
    void run(IR &, AM &) {}
};

} // namespace llvm

#endif
