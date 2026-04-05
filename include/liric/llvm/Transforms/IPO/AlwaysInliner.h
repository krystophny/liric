#ifndef LLVM_TRANSFORMS_IPO_ALWAYSINLINER_H
#define LLVM_TRANSFORMS_IPO_ALWAYSINLINER_H

namespace liric_llvm {

class AlwaysInlinerPass {
public:
    AlwaysInlinerPass() = default;
};

inline AlwaysInlinerPass createAlwaysInlinerPass() {
    return AlwaysInlinerPass();
}

} // namespace liric_llvm

#endif
