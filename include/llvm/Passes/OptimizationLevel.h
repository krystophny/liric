#ifndef LLVM_PASSES_OPTIMIZATIONLEVEL_H
#define LLVM_PASSES_OPTIMIZATIONLEVEL_H

namespace llvm {

class OptimizationLevel {
    unsigned SpeedLevel;
    unsigned SizeLevel;

    constexpr OptimizationLevel(unsigned Speed, unsigned Size)
        : SpeedLevel(Speed), SizeLevel(Size) {}

public:
    static const OptimizationLevel O0;
    static const OptimizationLevel O1;
    static const OptimizationLevel O2;
    static const OptimizationLevel O3;
    static const OptimizationLevel Os;
    static const OptimizationLevel Oz;

    constexpr unsigned getSpeedupLevel() const { return SpeedLevel; }
    constexpr unsigned getSizeLevel() const { return SizeLevel; }

    constexpr bool isOptimizingForSpeed() const { return SpeedLevel > 0; }
    constexpr bool isOptimizingForSize() const { return SizeLevel > 0; }

    constexpr bool operator==(const OptimizationLevel &Other) const {
        return SpeedLevel == Other.SpeedLevel && SizeLevel == Other.SizeLevel;
    }
    constexpr bool operator!=(const OptimizationLevel &Other) const {
        return !(*this == Other);
    }
};

inline constexpr OptimizationLevel OptimizationLevel::O0 = {0, 0};
inline constexpr OptimizationLevel OptimizationLevel::O1 = {1, 0};
inline constexpr OptimizationLevel OptimizationLevel::O2 = {2, 0};
inline constexpr OptimizationLevel OptimizationLevel::O3 = {3, 0};
inline constexpr OptimizationLevel OptimizationLevel::Os = {2, 1};
inline constexpr OptimizationLevel OptimizationLevel::Oz = {2, 2};

} // namespace llvm

#endif
