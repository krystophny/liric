#ifndef LLVM_TRANSFORMS_UTILS_MEM2REG_H
#define LLVM_TRANSFORMS_UTILS_MEM2REG_H

namespace llvm {

class PromotePass {
public:
    template <typename IR, typename AM>
    void run(IR &, AM &) {}
};

} // namespace llvm

#endif
