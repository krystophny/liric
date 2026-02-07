#ifndef LLVM_SUPPORT_ALIGNMENT_H
#define LLVM_SUPPORT_ALIGNMENT_H

#include <cstdint>

namespace llvm {

class Align {
    uint64_t value_;

public:
    Align() : value_(1) {}
    explicit Align(uint64_t v) : value_(v) {}

    uint64_t value() const { return value_; }

    operator uint64_t() const { return value_; }
};

class MaybeAlign {
    uint64_t value_;
    bool has_value_;

public:
    MaybeAlign() : value_(0), has_value_(false) {}
    MaybeAlign(uint64_t v) : value_(v), has_value_(true) {}
    MaybeAlign(Align a) : value_(a.value()), has_value_(true) {}

    bool hasValue() const { return has_value_; }
    explicit operator bool() const { return has_value_; }
    Align getValue() const { return Align(value_); }
    uint64_t valueOrOne() const { return has_value_ ? value_ : 1; }
};

} // namespace llvm

#endif
