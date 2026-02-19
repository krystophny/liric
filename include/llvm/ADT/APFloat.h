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

    static bool isSingleSemantics(const fltSemantics &sem) {
        return &sem == &fltSemantics::IEEEsingle();
    }

    static bool isDoubleSemantics(const fltSemantics &sem) {
        return &sem == &fltSemantics::IEEEdouble();
    }

    static double decodeBits(uint64_t raw, unsigned bit_width) {
        if (bit_width <= 32) {
            float f;
            uint32_t raw32 = static_cast<uint32_t>(raw);
            std::memcpy(&f, &raw32, sizeof(f));
            return static_cast<double>(f);
        }
        double d;
        std::memcpy(&d, &raw, sizeof(d));
        return d;
    }

    static double decodeBits(const fltSemantics &sem, uint64_t raw,
                             unsigned fallback_bit_width) {
        if (isSingleSemantics(sem)) {
            return decodeBits(raw, 32);
        }
        if (isDoubleSemantics(sem)) {
            return decodeBits(raw, 64);
        }
        return decodeBits(raw, fallback_bit_width);
    }

public:
    APFloat() : val_(0.0), is_single_(false) {}
    explicit APFloat(double v) : val_(v), is_single_(false) {}
    explicit APFloat(float v) : val_(static_cast<double>(v)), is_single_(true) {}
    APFloat(const fltSemantics &sem, uint64_t bits)
        : val_(decodeBits(sem, bits, 64)),
          is_single_(isSingleSemantics(sem)) {}
    APFloat(const fltSemantics &sem, const APInt &bits)
        : val_(decodeBits(sem, bits.getZExtValue(), bits.getBitWidth())),
          is_single_(isSingleSemantics(sem)) {}

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

    static APFloat getZero(const fltSemantics &sem, bool negative = false) {
        APFloat r(negative ? -0.0 : 0.0);
        r.is_single_ = (&sem == &fltSemantics::IEEEsingle());
        return r;
    }
    static APFloat getInf(const fltSemantics &sem, bool negative = false) {
        APFloat r(negative ? -__builtin_inf() : __builtin_inf());
        r.is_single_ = (&sem == &fltSemantics::IEEEsingle());
        return r;
    }
    static APFloat getNaN(const fltSemantics &sem, bool negative = false, uint64_t payload = 0) {
        (void)payload;
        double v = __builtin_nan("");
        APFloat r(negative ? -v : v);
        r.is_single_ = (&sem == &fltSemantics::IEEEsingle());
        return r;
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
