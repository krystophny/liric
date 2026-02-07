#ifndef LLVM_TRANSFORMS_IPO_ALWAYSINLINER_H
#define LLVM_TRANSFORMS_IPO_ALWAYSINLINER_H

namespace llvm {

class AlwaysInlinerPass {
public:
    AlwaysInlinerPass() = default;
};

inline AlwaysInlinerPass createAlwaysInlinerPass() {
    return AlwaysInlinerPass();
}

} // namespace llvm

#endif
