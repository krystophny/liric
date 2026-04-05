#ifndef LLVM_ANALYSIS_TARGETLIBRARYINFO_H
#define LLVM_ANALYSIS_TARGETLIBRARYINFO_H

namespace liric_llvm {

class TargetLibraryInfoImpl {};

class TargetLibraryInfoWrapperPass {
public:
    TargetLibraryInfoWrapperPass() = default;
    explicit TargetLibraryInfoWrapperPass(const TargetLibraryInfoImpl &) {}
};

inline TargetLibraryInfoWrapperPass *createTargetLibraryInfoWrapperPass(
    const TargetLibraryInfoImpl & = TargetLibraryInfoImpl()) {
    return new TargetLibraryInfoWrapperPass();
}

} // namespace liric_llvm

#endif
