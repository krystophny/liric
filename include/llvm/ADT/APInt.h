#ifndef LLVM_ADT_APINT_H
#define LLVM_ADT_APINT_H

#include <cstdint>

namespace llvm {

class APInt {
    uint64_t val_;
    unsigned width_;

public:
    APInt() : val_(0), width_(64) {}
    APInt(unsigned numBits, uint64_t val, bool isSigned = false)
        : val_(val), width_(numBits) {
        (void)isSigned;
    }

    unsigned getBitWidth() const { return width_; }
    uint64_t getZExtValue() const { return val_; }
    int64_t getSExtValue() const { return static_cast<int64_t>(val_); }
    bool isNegative() const { return static_cast<int64_t>(val_) < 0; }

    bool operator==(const APInt &other) const { return val_ == other.val_; }
    bool operator!=(const APInt &other) const { return val_ != other.val_; }
    bool operator==(uint64_t v) const { return val_ == v; }

    APInt zext(unsigned width) const { return APInt(width, val_); }
    APInt sext(unsigned width) const { return APInt(width, val_, true); }
    APInt trunc(unsigned width) const {
        uint64_t mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
        return APInt(width, val_ & mask);
    }
};

} // namespace llvm

#endif
