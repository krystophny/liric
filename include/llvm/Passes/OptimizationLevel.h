#ifndef LLVM_PASSES_OPTIMIZATIONLEVEL_H
#define LLVM_PASSES_OPTIMIZATIONLEVEL_H

namespace llvm {

class OptimizationLevel {
    unsigned SpeedLevel;
    unsigned SizeLevel;

    OptimizationLevel(unsigned Speed, unsigned Size)
        : SpeedLevel(Speed), SizeLevel(Size) {}

public:
    static const OptimizationLevel O0;
    static const OptimizationLevel O1;
    static const OptimizationLevel O2;
    static const OptimizationLevel O3;
    static const OptimizationLevel Os;
    static const OptimizationLevel Oz;

    unsigned getSpeedupLevel() const { return SpeedLevel; }
    unsigned getSizeLevel() const { return SizeLevel; }

    bool isOptimizingForSpeed() const { return SpeedLevel > 0; }
    bool isOptimizingForSize() const { return SizeLevel > 0; }

    bool operator==(const OptimizationLevel &Other) const {
        return SpeedLevel == Other.SpeedLevel && SizeLevel == Other.SizeLevel;
    }
    bool operator!=(const OptimizationLevel &Other) const {
        return !(*this == Other);
    }
};

inline const OptimizationLevel OptimizationLevel::O0 = {0, 0};
inline const OptimizationLevel OptimizationLevel::O1 = {1, 0};
inline const OptimizationLevel OptimizationLevel::O2 = {2, 0};
inline const OptimizationLevel OptimizationLevel::O3 = {3, 0};
inline const OptimizationLevel OptimizationLevel::Os = {2, 1};
inline const OptimizationLevel OptimizationLevel::Oz = {2, 2};

} // namespace llvm

#endif
