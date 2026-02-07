#ifndef LLVM_ANALYSIS_TARGETLIBRARYINFO_H
#define LLVM_ANALYSIS_TARGETLIBRARYINFO_H

namespace llvm {

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

} // namespace llvm

#endif
