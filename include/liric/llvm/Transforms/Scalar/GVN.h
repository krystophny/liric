#ifndef LLVM_TRANSFORMS_SCALAR_GVN_H
#define LLVM_TRANSFORMS_SCALAR_GVN_H

namespace liric_llvm {

class GVNPass {
public:
    template <typename IR, typename AM>
    void run(IR &, AM &) {}
};

} // namespace liric_llvm

#endif
