#ifndef LLVM_ADT_APINT_H
#define LLVM_ADT_APINT_H
#include "llvm/ADT/ArrayRef.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace liric_llvm {
class APInt {
    std::vector<uint64_t> words_;
    unsigned width_;
    void mask() {
        if (width_ % 64) words_.back() &= (uint64_t(1) << (width_ % 64)) - 1;
    }
public:
    APInt(): APInt(64, 0) {}
    APInt(unsigned width, uint64_t value, bool is_signed = false):
        words_((width + 63) / 64, is_signed && int64_t(value) < 0 ? ~uint64_t(0) : 0),
        width_(width) {
        if (!width) throw std::invalid_argument("liric: integer width must be positive");
        words_[0] = value;
        mask();
    }
    APInt(unsigned width, ArrayRef<uint64_t> words):
        words_((width + 63) / 64, 0), width_(width) {
        if (!width)
            throw std::invalid_argument("liric: integer width must be positive");
        std::copy_n(words.data(), std::min(words.size(), words_.size()), words_.data());
        mask();
    }
    unsigned getBitWidth() const { return width_; }
    uint64_t getZExtValue() const {
        for (size_t i = 1; i < words_.size(); i++) {
            if (words_[i]) throw std::runtime_error("liric: integer value does not fit 64 bits");
        }
        return words_[0];
    }
    int64_t getSExtValue() const {
        if (width_ <= 64) {
            uint64_t value = words_[0];
            if (width_ < 64 && isNegative()) value |= ~uint64_t(0) << width_;
            return int64_t(value);
        }
        APInt extended = APInt(64, words_[0]).sext(width_);
        if (*this != extended) throw std::runtime_error("liric: signed integer value does not fit 64 bits");
        return int64_t(words_[0]);
    }
    bool isNegative() const { return (words_.back() >> ((width_ - 1) % 64)) & 1; }
    bool operator==(const APInt &other) const { return width_ == other.width_ && words_ == other.words_; }
    bool operator!=(const APInt &other) const { return !(*this == other); }
    bool operator==(uint64_t value) const {
        if (words_[0] != value) return false;
        return std::all_of(words_.begin() + 1, words_.end(), [](uint64_t word) { return word == 0; });
    }
    bool needsCleanup() const { return width_ > 64; }
    const uint64_t *getRawData() const { return words_.data(); }
    APInt zext(unsigned width) const {
        if (width < width_) throw std::invalid_argument("liric: extension cannot reduce integer width");
        APInt result(width, 0);
        std::copy(words_.begin(), words_.end(), result.words_.begin());
        return result;
    }
    APInt sext(unsigned width) const {
        APInt result = zext(width);
        if (isNegative() && width > width_) {
            for (size_t i = words_.size(); i < result.words_.size(); i++) result.words_[i] = ~uint64_t(0);
            if (width_ % 64) result.words_[words_.size() - 1] |= ~uint64_t(0) << (width_ % 64);
            result.mask();
        }
        return result;
    }
    APInt trunc(unsigned width) const {
        if (width > width_) throw std::invalid_argument("liric: truncation cannot increase integer width");
        return APInt(width, ArrayRef<uint64_t>(words_.data(), (width + 63) / 64));
    }
};
}
#endif
