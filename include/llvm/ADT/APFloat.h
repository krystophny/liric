#ifndef LLVM_ADT_APFLOAT_H
#define LLVM_ADT_APFLOAT_H

#include "llvm/ADT/APInt.h"
#include <cstdint>
#include <cstring>

namespace llvm {

namespace APFloatBase {
struct Semantics {};
}

class fltSemantics {
public:
    static const fltSemantics &IEEEhalf();
    static const fltSemantics &IEEEsingle();
    static const fltSemantics &IEEEdouble();
};

inline const fltSemantics &fltSemantics::IEEEhalf() {
    static fltSemantics s;
    return s;
}
inline const fltSemantics &fltSemantics::IEEEsingle() {
    static fltSemantics s;
    return s;
}
inline const fltSemantics &fltSemantics::IEEEdouble() {
    static fltSemantics s;
    return s;
}

class APFloat {
    double val_;
    bool is_single_;

public:
    APFloat() : val_(0.0), is_single_(false) {}
    explicit APFloat(double v) : val_(v), is_single_(false) {}
    explicit APFloat(float v) : val_(static_cast<double>(v)), is_single_(true) {}
    APFloat(const fltSemantics &sem, uint64_t bits) : val_(0.0),
        is_single_(&sem == &fltSemantics::IEEEsingle()) {
        (void)bits;
    }
    APFloat(const fltSemantics &sem, const APInt &bits) : val_(0.0),
        is_single_(&sem == &fltSemantics::IEEEsingle()) {
        uint64_t raw = bits.getZExtValue();
        if (bits.getBitWidth() <= 32) {
            float f;
            uint32_t raw32 = static_cast<uint32_t>(raw);
            std::memcpy(&f, &raw32, sizeof(f));
            val_ = f;
        } else {
            std::memcpy(&val_, &raw, sizeof(val_));
        }
    }

    double convertToDouble() const { return val_; }
    float convertToFloat() const { return static_cast<float>(val_); }
    bool isSinglePrecision() const { return is_single_; }

    APInt bitcastToAPInt() const {
        uint64_t bits;
        std::memcpy(&bits, &val_, sizeof(bits));
        return APInt(64, bits);
    }

    bool isNaN() const { return val_ != val_; }
    bool isInfinity() const {
        return val_ == __builtin_inf() || val_ == -__builtin_inf();
    }
    bool isNegative() const { return val_ < 0.0; }

    static APFloat getZero(const fltSemantics &, bool negative = false) {
        return APFloat(negative ? -0.0 : 0.0);
    }
    static APFloat getInf(const fltSemantics &, bool negative = false) {
        return APFloat(negative ? -__builtin_inf() : __builtin_inf());
    }
    static APFloat getNaN(const fltSemantics &, bool negative = false, uint64_t payload = 0) {
        (void)payload;
        double v = __builtin_nan("");
        return APFloat(negative ? -v : v);
    }

    using Semantics = APFloatBase::Semantics;

    enum opStatus {
        opOK = 0,
        opInvalidOp,
        opDivByZero,
        opOverflow,
        opUnderflow,
        opInexact,
    };

    enum roundingMode {
        rmNearestTiesToEven = 0,
        rmTowardPositive,
        rmTowardNegative,
        rmTowardZero,
        rmNearestTiesToAway,
    };
};

} // namespace llvm

#endif
