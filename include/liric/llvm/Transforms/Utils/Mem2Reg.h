#ifndef LLVM_TRANSFORMS_UTILS_MEM2REG_H
#define LLVM_TRANSFORMS_UTILS_MEM2REG_H

namespace liric_llvm {

class PromotePass {
public:
    template <typename IR, typename AM>
    void run(IR &, AM &) {}
};

} // namespace liric_llvm

#endif
