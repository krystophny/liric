#ifndef LLVM_TARGET_TARGETOPTIONS_H
#define LLVM_TARGET_TARGETOPTIONS_H

namespace llvm {

class TargetOptions {
public:
    bool UnsafeFPMath = false;
    bool NoInfsFPMath = false;
    bool NoNaNsFPMath = false;
    bool HonorSignDependentRoundingFPMathOption = false;
    bool NoZerosInBSS = false;
    bool GuaranteedTailCallOpt = false;
    bool StackAlignmentOverride = false;
    bool EnableFastISel = false;
    bool EnableGlobalISel = false;
    bool NoTrapAfterNoreturn = false;
};

} // namespace llvm

#endif
