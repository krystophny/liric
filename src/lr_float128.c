/*
 * lr_float128.c -- liric's own IEEE 754 binary128 (real(16)) soft-float.
 *
 * Self-contained: pure C99, uint64 limbs, no __int128, no libgcc, no libm.
 * liric's native backends have no 128-bit float hardware, so f128 arithmetic
 * lowers to pointer-based calls into these routines.  Every public entry takes
 * and returns the 16-byte little-endian value through a pointer, so the call
 * ABI is trivial and identical on x86_64, aarch64 and riscv64.
 *
 * The algorithms are the portable binary128 implementation also used by
 * LFortran on platforms without libquadmath; reproduced here so liric carries
 * its own copy and depends on nothing.
 */

#include <stdint.h>
#include <string.h>

/* ---- 128-bit unsigned integer (struct of two u64 limbs) ---------------- */
typedef struct { uint64_t lo; uint64_t hi; } u128;

static inline u128 u128_make(uint64_t hi, uint64_t lo) { u128 r; r.lo = lo; r.hi = hi; return r; }
static inline uint64_t u128_hi(u128 v) { return v.hi; }
static inline uint64_t u128_lo(u128 v) { return v.lo; }
static inline u128 u128_from_u64(uint64_t v) { u128 r; r.lo = v; r.hi = 0; return r; }
static inline u128 u128_zero(void) { u128 r; r.lo = 0; r.hi = 0; return r; }
static inline u128 u128_one (void) { u128 r; r.lo = 1; r.hi = 0; return r; }

static inline u128 u128_add(u128 a, u128 b) {
    u128 r; r.lo = a.lo + b.lo; r.hi = a.hi + b.hi + (r.lo < a.lo ? 1u : 0u); return r;
}
static inline u128 u128_sub(u128 a, u128 b) {
    u128 r; r.lo = a.lo - b.lo; r.hi = a.hi - b.hi - (a.lo < b.lo ? 1u : 0u); return r;
}
static inline u128 u128_inc(u128 a) {
    u128 r; r.lo = a.lo + 1; r.hi = a.hi + (r.lo == 0 ? 1u : 0u); return r;
}
static inline u128 u128_and(u128 a, u128 b) { u128 r; r.lo = a.lo & b.lo; r.hi = a.hi & b.hi; return r; }
static inline u128 u128_or (u128 a, u128 b) { u128 r; r.lo = a.lo | b.lo; r.hi = a.hi | b.hi; return r; }
static inline u128 u128_not(u128 a) { u128 r; r.lo = ~a.lo; r.hi = ~a.hi; return r; }

static inline u128 u128_shl(u128 a, int n) {
    u128 r;
    if (n <= 0)   { return a; }
    if (n >= 128) { r.lo = 0; r.hi = 0; return r; }
    if (n >= 64)  { r.hi = a.lo << (n - 64); r.lo = 0; return r; }
    r.hi = (a.hi << n) | (a.lo >> (64 - n)); r.lo = a.lo << n; return r;
}
static inline u128 u128_shr(u128 a, int n) {
    u128 r;
    if (n <= 0)   { return a; }
    if (n >= 128) { r.lo = 0; r.hi = 0; return r; }
    if (n >= 64)  { r.lo = a.hi >> (n - 64); r.hi = 0; return r; }
    r.lo = (a.lo >> n) | (a.hi << (64 - n)); r.hi = a.hi >> n; return r;
}
static inline int u128_gt (u128 a, u128 b) { return a.hi > b.hi || (a.hi == b.hi && a.lo > b.lo); }
static inline int u128_gte(u128 a, u128 b) { return a.hi > b.hi || (a.hi == b.hi && a.lo >= b.lo); }
static inline int u128_lt (u128 a, u128 b) { return u128_gt(b, a); }
static inline int u128_eq (u128 a, u128 b) { return a.hi == b.hi && a.lo == b.lo; }
static inline int u128_is_zero(u128 a) { return !a.lo && !a.hi; }
static inline int u128_bit(u128 a, int n) {
    if (n >= 64) return (int)((a.hi >> (n - 64)) & 1);
    return (int)((a.lo >> n) & 1);
}
static inline u128 u128_shr1(u128 a) { u128 r; r.lo = (a.lo >> 1) | (a.hi << 63); r.hi = a.hi >> 1; return r; }
static inline u128 u128_shl1(u128 a) { u128 r; r.hi = (a.hi << 1) | (a.lo >> 63); r.lo = a.lo << 1; return r; }

static inline int lf_clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static inline int lf_clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline int u128_clz(u128 v) {
    if (v.hi) return lf_clz64(v.hi);
    if (v.lo) return 64 + lf_clz64(v.lo);
    return 128;
}

static inline u128 mul64x64(uint64_t a, uint64_t b) {
    uint64_t a_lo = a & 0xFFFFFFFFull, a_hi = a >> 32;
    uint64_t b_lo = b & 0xFFFFFFFFull, b_hi = b >> 32;
    uint64_t p0 = a_lo * b_lo, p1 = a_lo * b_hi, p2 = a_hi * b_lo, p3 = a_hi * b_hi;
    uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFull) + (p2 & 0xFFFFFFFFull);
    u128 r;
    r.lo = (mid << 32) | (p0 & 0xFFFFFFFFull);
    r.hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    return r;
}
static void u128_mul128(u128 a, u128 b, u128 *hi_out, u128 *lo_out) {
    u128 c00 = mul64x64(a.lo, b.lo), c01 = mul64x64(a.lo, b.hi);
    u128 c10 = mul64x64(a.hi, b.lo), c11 = mul64x64(a.hi, b.hi);
    uint64_t m0 = c00.hi;
    uint64_t m1 = m0 + c01.lo; int mc1 = (m1 < m0) ? 1 : 0;
    uint64_t m2 = m1 + c10.lo; int mc2 = (m2 < m1) ? 1 : 0;
    lo_out->lo = c00.lo; lo_out->hi = m2;
    uint64_t h0 = c11.lo;
    uint64_t h1 = h0 + c01.hi; int hc1 = (h1 < h0) ? 1 : 0;
    uint64_t h2 = h1 + c10.hi; int hc2 = (h2 < h1) ? 1 : 0;
    uint64_t h3 = h2 + (uint64_t)(mc1 + mc2); int hc3 = (h3 < h2) ? 1 : 0;
    hi_out->lo = h3; hi_out->hi = c11.hi + (uint64_t)(hc1 + hc2 + hc3);
}

/* ---- binary128 value (16 little-endian bytes) -------------------------- */
typedef struct { uint8_t bytes[16]; } lr_f128;

typedef struct {
    int sign; int32_t exp; u128 mant; int is_nan; int is_inf; int is_zero;
} f128_parts;

static f128_parts f128_unpack(lr_f128 v) {
    f128_parts p = {0};
    uint64_t lo, hi;
    memcpy(&lo, v.bytes, 8); memcpy(&hi, v.bytes + 8, 8);
    p.sign = (int)(hi >> 63);
    int32_t biased = (int32_t)((hi >> 48) & 0x7FFFu);
    uint64_t mhi = hi & 0x0000FFFFFFFFFFFFull, mlo = lo;
    if (biased == 0x7FFF) {
        if (mhi || mlo) { p.is_nan = 1; return p; }
        p.is_inf = 1; return p;
    }
    if (!biased && !mhi && !mlo) { p.is_zero = 1; return p; }
    if (biased) { p.mant = u128_make(mhi | (1ull << 48), mlo); p.exp = biased - 16383; }
    else        { p.mant = u128_make(mhi, mlo); p.exp = -16382; }
    return p;
}

static lr_f128 f128_pack_parts(int sign, int32_t exp, u128 mant) {
    lr_f128 r; uint64_t lo, hi;
    if (u128_is_zero(mant)) {
        hi = (uint64_t)sign << 63; lo = 0;
        memcpy(r.bytes, &lo, 8); memcpy(r.bytes + 8, &hi, 8); return r;
    }
    int clz = u128_clz(mant);
    int shift = clz - (127 - 112);
    if (shift > 0) { mant = u128_shl(mant, shift); exp -= shift; }
    else if (shift < 0) {
        int rsh = -shift;
        u128 sticky = u128_and(mant, u128_sub(u128_shl(u128_one(), rsh), u128_one()));
        mant = u128_shr(mant, rsh);
        u128 half = u128_shl(u128_one(), rsh - 1), lsb = u128_one();
        if (u128_gt(sticky, half) || (u128_eq(sticky, half) && !u128_is_zero(u128_and(mant, lsb))))
            mant = u128_inc(mant);
        if (!u128_is_zero(u128_shr(mant, 113))) { mant = u128_shr(mant, 1); exp++; }
        exp += rsh;
    }
    if (exp <= -16383) {
        int subnorm_shift = -16382 - exp + 1;
        if (subnorm_shift >= 113) {
            hi = (uint64_t)sign << 63; lo = 0;
            memcpy(r.bytes, &lo, 8); memcpy(r.bytes + 8, &hi, 8); return r;
        }
        mant = u128_shr(mant, subnorm_shift);
        uint64_t mhi = u128_hi(mant) & 0x0000FFFFFFFFFFFFull, mlo = u128_lo(mant);
        hi = ((uint64_t)sign << 63) | mhi; lo = mlo;
        memcpy(r.bytes, &lo, 8); memcpy(r.bytes + 8, &hi, 8); return r;
    }
    int32_t biased = exp + 16383;
    if (biased >= 0x7FFF) {
        hi = ((uint64_t)sign << 63) | ((uint64_t)0x7FFF << 48); lo = 0;
        memcpy(r.bytes, &lo, 8); memcpy(r.bytes + 8, &hi, 8); return r;
    }
    mant = u128_and(mant, u128_sub(u128_shl(u128_one(), 112), u128_one()));
    uint64_t mhi = u128_hi(mant) & 0x0000FFFFFFFFFFFFull, mlo = u128_lo(mant);
    hi = ((uint64_t)sign << 63) | ((uint64_t)biased << 48) | mhi; lo = mlo;
    memcpy(r.bytes, &lo, 8); memcpy(r.bytes + 8, &hi, 8); return r;
}

static lr_f128 f128_make_nan(void) {
    lr_f128 r; uint64_t hi = 0x7FFF800000000000ull, lo = 0;
    memcpy(r.bytes, &lo, 8); memcpy(r.bytes + 8, &hi, 8); return r;
}
static lr_f128 f128_make_inf(int sign) {
    lr_f128 r; uint64_t hi = ((uint64_t)sign << 63) | ((uint64_t)0x7FFF << 48), lo = 0;
    memcpy(r.bytes, &lo, 8); memcpy(r.bytes + 8, &hi, 8); return r;
}
static lr_f128 f128_make_zero(int sign) {
    lr_f128 r; uint64_t hi = (uint64_t)sign << 63, lo = 0;
    memcpy(r.bytes, &lo, 8); memcpy(r.bytes + 8, &hi, 8); return r;
}
static int f128_is_zero(lr_f128 v) { return f128_unpack(v).is_zero; }

static lr_f128 f128_const(int sign, int32_t biased, uint64_t mhi, uint64_t mlo) {
    lr_f128 r;
    uint64_t hi = ((uint64_t)sign << 63) | ((uint64_t)biased << 48) | (mhi & 0x0000FFFFFFFFFFFFull);
    uint64_t lo = mlo;
    memcpy(r.bytes, &lo, 8); memcpy(r.bytes + 8, &hi, 8); return r;
}
static lr_f128 f128_one(void) { return f128_const(0, 16383, 0, 0); }
static lr_f128 f128_two(void) { return f128_const(0, 16384, 0, 0); }

static int f128_cmp_internal(lr_f128 a, lr_f128 b) {
    f128_parts pa = f128_unpack(a), pb = f128_unpack(b);
    if (pa.is_nan || pb.is_nan) return 2;
    int as = pa.sign, bs = pb.sign;
    if (pa.is_zero && pb.is_zero) return 0;
    if (pa.is_zero) return bs ? 1 : -1;
    if (pb.is_zero) return as ? -1 : 1;
    if (as != bs) return as ? -1 : 1;
    int mag;
    if (pa.is_inf && pb.is_inf) mag = 0;
    else if (pa.is_inf)         mag = 1;
    else if (pb.is_inf)         mag = -1;
    else if (pa.exp != pb.exp)  mag = pa.exp > pb.exp ? 1 : -1;
    else                        mag = u128_gt(pa.mant, pb.mant) ? 1 : u128_lt(pa.mant, pb.mant) ? -1 : 0;
    return as ? -mag : mag;
}

static lr_f128 f128_neg(lr_f128 a) { lr_f128 r = a; r.bytes[15] ^= 0x80; return r; }
static lr_f128 f128_abs(lr_f128 a) { lr_f128 r = a; r.bytes[15] &= 0x7F; return r; }

static lr_f128 f128_add_same_sign(f128_parts pa, f128_parts pb) {
    int shift = pa.exp - pb.exp;
    u128 mb = (shift < 128) ? u128_shr(pb.mant, shift) : u128_zero();
    u128 sum = u128_add(pa.mant, mb);
    int32_t exp = pa.exp;
    if (!u128_is_zero(u128_shr(sum, 113))) { sum = u128_shr(sum, 1); exp++; }
    return f128_pack_parts(pa.sign, exp, sum);
}
static lr_f128 f128_sub_mag(f128_parts pa, f128_parts pb, int result_sign) {
    int shift = pa.exp - pb.exp;
    u128 mb = (shift < 128) ? u128_shr(pb.mant, shift) : u128_zero();
    u128 diff = u128_sub(pa.mant, mb);
    return f128_pack_parts(result_sign, pa.exp, diff);
}
static lr_f128 f128_add(lr_f128 a, lr_f128 b) {
    f128_parts pa = f128_unpack(a), pb = f128_unpack(b);
    if (pa.is_nan) return a;
    if (pb.is_nan) return b;
    if (pa.is_inf && pb.is_inf) { if (pa.sign != pb.sign) return f128_make_nan(); return a; }
    if (pa.is_inf) return a;
    if (pb.is_inf) return b;
    if (pa.is_zero && pb.is_zero) return f128_make_zero(pa.sign & pb.sign);
    if (pa.is_zero) return b;
    if (pb.is_zero) return a;
    int swapped = 0;
    if (pa.exp < pb.exp || (pa.exp == pb.exp && u128_lt(pa.mant, pb.mant))) {
        f128_parts tmp = pa; pa = pb; pb = tmp; swapped = 1;
    }
    if (pa.sign == pb.sign) return f128_add_same_sign(pa, pb);
    if (u128_eq(pa.mant, pb.mant) && pa.exp == pb.exp) return f128_make_zero(0);
    /* After the sort pa holds the larger magnitude, so the difference takes
       pa's sign.  (The original portable code used `swapped ? pb.sign` here,
       which picks the smaller operand's sign when the operands were swapped.) */
    (void)swapped;
    int rsign = pa.sign;
    return f128_sub_mag(pa, pb, rsign);
}
static lr_f128 f128_sub(lr_f128 a, lr_f128 b) { return f128_add(a, f128_neg(b)); }

static lr_f128 f128_mul(lr_f128 a, lr_f128 b) {
    f128_parts pa = f128_unpack(a), pb = f128_unpack(b);
    int rsign = pa.sign ^ pb.sign;
    if (pa.is_nan) return a;
    if (pb.is_nan) return b;
    if (pa.is_inf || pb.is_inf) {
        if (pa.is_zero || pb.is_zero) return f128_make_nan();
        return f128_make_inf(rsign);
    }
    if (pa.is_zero || pb.is_zero) return f128_make_zero(rsign);
    u128 prod_hi, prod_lo;
    u128_mul128(pa.mant, pb.mant, &prod_hi, &prod_lo);
    int32_t exp = pa.exp + pb.exp;
    int lz = u128_clz(prod_hi);
    int rsh = 255 - lz - 112;
    u128 result_mant;
    if (rsh >= 128) {
        int rsh2 = rsh - 128;
        result_mant = u128_shr(prod_hi, rsh2);
        int round_bit = rsh2 > 0 ? u128_bit(prod_hi, rsh2 - 1) : u128_bit(prod_lo, 127);
        u128 sticky = rsh2 > 1
                    ? u128_and(prod_hi, u128_sub(u128_shl(u128_one(), rsh2 - 1), u128_one()))
                    : u128_zero();
        sticky = u128_or(sticky, prod_lo);
        if (round_bit && (!u128_is_zero(sticky) || !u128_is_zero(u128_and(result_mant, u128_one()))))
            result_mant = u128_inc(result_mant);
    } else {
        result_mant = u128_or(u128_shl(prod_hi, 128 - rsh), u128_shr(prod_lo, rsh));
        int round_bit = u128_bit(prod_lo, rsh - 1);
        u128 sticky = u128_and(prod_lo, u128_sub(u128_shl(u128_one(), rsh - 1), u128_one()));
        if (round_bit && (!u128_is_zero(sticky) || !u128_is_zero(u128_and(result_mant, u128_one()))))
            result_mant = u128_inc(result_mant);
    }
    exp += rsh - 112;
    return f128_pack_parts(rsign, exp, result_mant);
}

static lr_f128 f128_div(lr_f128 a, lr_f128 b) {
    f128_parts pa = f128_unpack(a), pb = f128_unpack(b);
    int rsign = pa.sign ^ pb.sign;
    if (pa.is_nan) return a;
    if (pb.is_nan) return b;
    if (pb.is_zero) { if (pa.is_zero) return f128_make_nan(); return f128_make_inf(rsign); }
    if (pa.is_inf) { if (pb.is_inf) return f128_make_nan(); return f128_make_inf(rsign); }
    if (pa.is_zero) return f128_make_zero(rsign);
    if (pb.is_inf)  return f128_make_zero(rsign);
    int32_t exp_a = pa.exp, exp_b = pb.exp;
    u128 mant_a = pa.mant, mant_b = pb.mant;
    u128 remainder = mant_a, quotient = u128_zero();
    for (int i = 0; i < 113; i++) {
        remainder = u128_shl1(remainder);
        quotient  = u128_shl1(quotient);
        if (u128_gte(remainder, mant_b)) {
            remainder = u128_sub(remainder, mant_b);
            quotient  = u128_or(quotient, u128_one());
        }
    }
    if (u128_gte(u128_shl1(remainder), mant_b)) quotient = u128_inc(quotient);
    int32_t rexp = exp_a - exp_b - 1;
    return f128_pack_parts(rsign, rexp, quotient);
}

static lr_f128 f128_from_f64(double d) {
    uint64_t bits; memcpy(&bits, &d, 8);
    int sign = (int)(bits >> 63);
    int32_t dexp = (int32_t)((bits >> 52) & 0x7FF);
    uint64_t dmant = bits & 0x000FFFFFFFFFFFFFull;
    if (dexp == 0x7FF) { if (dmant) return f128_make_nan(); return f128_make_inf(sign); }
    if (!dexp && !dmant) return f128_make_zero(sign);
    u128 mant; int32_t exp;
    if (dexp) { mant = u128_shl(u128_from_u64(dmant | (1ull << 52)), 112 - 52); exp = dexp - 1023; }
    else      { mant = u128_shl(u128_from_u64(dmant), 112 - 52); exp = -1022 - 52; }
    return f128_pack_parts(sign, exp, mant);
}
static lr_f128 f128_from_f32(float f) {
    uint32_t bits; memcpy(&bits, &f, 4);
    int sign = (int)(bits >> 31);
    int32_t fexp = (int32_t)((bits >> 23) & 0xFF);
    uint32_t fmant = bits & 0x007FFFFFu;
    if (fexp == 0xFF) { if (fmant) return f128_make_nan(); return f128_make_inf(sign); }
    if (!fexp && !fmant) return f128_make_zero(sign);
    u128 mant; int32_t exp;
    if (fexp) { mant = u128_shl(u128_from_u64(fmant | (1u << 23)), 112 - 23); exp = fexp - 127; }
    else      { mant = u128_shl(u128_from_u64(fmant), 112 - 23); exp = -126 - 23; }
    return f128_pack_parts(sign, exp, mant);
}
static double f128_to_f64(lr_f128 a) {
    f128_parts p = f128_unpack(a);
    uint64_t bits;
    if (p.is_nan) { bits = 0x7FF8000000000000ull; double r; memcpy(&r, &bits, 8); return r; }
    if (p.is_inf) { bits = ((uint64_t)p.sign << 63) | 0x7FF0000000000000ull; double r; memcpy(&r, &bits, 8); return r; }
    if (p.is_zero) { bits = (uint64_t)p.sign << 63; double r; memcpy(&r, &bits, 8); return r; }
    int32_t dexp = p.exp + 1023;
    if (dexp >= 0x7FF) { bits = ((uint64_t)p.sign << 63) | 0x7FF0000000000000ull; double r; memcpy(&r, &bits, 8); return r; }
    if (dexp <= 0) {
        int extra_shift = 1 - dexp;
        int rsh = (112 - 52) + extra_shift;
        if (rsh >= 128) { bits = (uint64_t)p.sign << 63; double r; memcpy(&r, &bits, 8); return r; }
        uint64_t dmant = u128_lo(u128_shr(p.mant, rsh));
        bits = ((uint64_t)p.sign << 63) | dmant;
        double r; memcpy(&r, &bits, 8); return r;
    }
    int rsh = 112 - 52;
    uint64_t dmant = u128_lo(u128_shr(p.mant, rsh)) & 0x000FFFFFFFFFFFFFull;
    int round_bit = u128_bit(p.mant, rsh - 1);
    u128 sticky = u128_and(p.mant, u128_sub(u128_shl(u128_one(), rsh - 1), u128_one()));
    if (round_bit && (!u128_is_zero(sticky) || (dmant & 1))) dmant++;
    if (dmant >> 52) { dmant >>= 1; dexp++; }
    bits = ((uint64_t)p.sign << 63) | ((uint64_t)dexp << 52) | dmant;
    double r; memcpy(&r, &bits, 8); return r;
}
static lr_f128 f128_from_i32(int32_t x) {
    if (!x) return f128_make_zero(0);
    int sign = x < 0 ? 1 : 0;
    uint64_t abs_x = sign ? (uint64_t)(-(int64_t)x) : (uint64_t)x;
    int lz32 = lf_clz32((uint32_t)abs_x);
    u128 mant = u128_shl(u128_from_u64(abs_x), 112 - (31 - lz32));
    int32_t exp = 31 - lz32;
    return f128_pack_parts(sign, exp, mant);
}
static lr_f128 f128_from_i64(int64_t x) {
    if (!x) return f128_make_zero(0);
    int sign = x < 0 ? 1 : 0;
    uint64_t abs_x = sign ? (uint64_t)(-x) : (uint64_t)x;
    int lz = lf_clz64(abs_x);
    int32_t exp = 63 - lz;
    u128 mant = u128_shl(u128_from_u64(abs_x), 112 - exp);
    return f128_pack_parts(sign, exp, mant);
}
static int32_t f128_to_i32(lr_f128 a) {
    f128_parts p = f128_unpack(a);
    if (p.is_nan || p.is_inf || p.is_zero) return 0;
    if (p.exp < 0) return 0;
    if (p.exp > 30) return p.sign ? (int32_t)0x80000000 : (int32_t)0x7FFFFFFF;
    uint32_t v = (uint32_t)u128_lo(u128_shr(p.mant, 112 - p.exp));
    return p.sign ? -(int32_t)v : (int32_t)v;
}
static int64_t f128_to_i64(lr_f128 a) {
    f128_parts p = f128_unpack(a);
    if (p.is_nan || p.is_inf || p.is_zero) return 0;
    if (p.exp < 0) return 0;
    if (p.exp > 62) return p.sign ? (int64_t)0x8000000000000000LL : (int64_t)0x7FFFFFFFFFFFFFFFLL;
    uint64_t v = u128_lo(u128_shr(p.mant, 112 - p.exp));
    return p.sign ? -(int64_t)v : (int64_t)v;
}
static lr_f128 f128_sqrt(lr_f128 a) {
    f128_parts p = f128_unpack(a);
    if (p.is_nan || p.sign) return f128_make_nan();
    if (p.is_zero) return a;
    if (p.is_inf) return a;
    int32_t half_exp = (p.exp >> 1);
    u128 half_mant = u128_shl(u128_one(), 112);
    lr_f128 x = f128_pack_parts(0, half_exp, half_mant);
    lr_f128 two = f128_two();
    for (int i = 0; i < 7; i++) {
        lr_f128 ax = f128_div(a, x);
        lr_f128 s  = f128_add(x, ax);
        x = f128_div(s, two);
    }
    return x;
}
/* Truncate toward zero (Fortran MOD semantics use this, not floor). */
static lr_f128 f128_trunc(lr_f128 a) {
    f128_parts p = f128_unpack(a);
    if (p.is_nan || p.is_inf || p.is_zero) return a;
    if (p.exp >= 112) return a;
    if (p.exp < 0) return f128_make_zero(p.sign);
    int frac_bits = 112 - p.exp;
    u128 mask = u128_not(u128_sub(u128_shl(u128_one(), frac_bits), u128_one()));
    return f128_pack_parts(p.sign, p.exp, u128_and(p.mant, mask));
}
static lr_f128 f128_mod(lr_f128 a, lr_f128 b) {
    /* Fortran MOD(a,b) = a - INT(a/b)*b, INT truncates toward zero. */
    lr_f128 q = f128_trunc(f128_div(a, b));
    return f128_sub(a, f128_mul(q, b));
}
static lr_f128 f128_pow(lr_f128 base, lr_f128 exp_v) {
    /* Only integer exponents are needed (real(16) ** integer); use repeated
       squaring.  Non-integer exponents fall back to 1.0 (unused by callers). */
    if (f128_is_zero(exp_v)) return f128_one();
    f128_parts ep = f128_unpack(exp_v);
    if (ep.is_nan || ep.is_inf) return f128_make_nan();
    int64_t n = f128_to_i64(exp_v);
    int neg = n < 0; uint64_t m = neg ? (uint64_t)(-n) : (uint64_t)n;
    lr_f128 result = f128_one(), b = base;
    while (m) { if (m & 1) result = f128_mul(result, b); b = f128_mul(b, b); m >>= 1; }
    if (neg) result = f128_div(f128_one(), result);
    return result;
}

/* ---- public pointer-ABI entry points (called by liric's backends) ------
 * Names live in the liric reserved prefix.  Result and operands are 16-byte
 * little-endian binary128 values passed by pointer; comparisons and integer
 * conversions return/accept scalars by value.
 */
#if defined(_WIN32)
#define LR_RT_API __declspec(dllexport)
#elif defined(__GNUC__)
#define LR_RT_API __attribute__((visibility("default")))
#else
#define LR_RT_API
#endif

static inline lr_f128 ld(const uint8_t *p) { lr_f128 v; memcpy(v.bytes, p, 16); return v; }
static inline void    st(uint8_t *p, lr_f128 v) { memcpy(p, v.bytes, 16); }

LR_RT_API void lr_f128_add(uint8_t r[16], const uint8_t a[16], const uint8_t b[16]) { st(r, f128_add(ld(a), ld(b))); }
LR_RT_API void lr_f128_sub(uint8_t r[16], const uint8_t a[16], const uint8_t b[16]) { st(r, f128_sub(ld(a), ld(b))); }
LR_RT_API void lr_f128_mul(uint8_t r[16], const uint8_t a[16], const uint8_t b[16]) { st(r, f128_mul(ld(a), ld(b))); }
LR_RT_API void lr_f128_div(uint8_t r[16], const uint8_t a[16], const uint8_t b[16]) { st(r, f128_div(ld(a), ld(b))); }
LR_RT_API void lr_f128_neg(uint8_t r[16], const uint8_t a[16]) { st(r, f128_neg(ld(a))); }
LR_RT_API void lr_f128_abs(uint8_t r[16], const uint8_t a[16]) { st(r, f128_abs(ld(a))); }
LR_RT_API void lr_f128_sqrt(uint8_t r[16], const uint8_t a[16]) { st(r, f128_sqrt(ld(a))); }
LR_RT_API void lr_f128_mod(uint8_t r[16], const uint8_t a[16], const uint8_t b[16]) { st(r, f128_mod(ld(a), ld(b))); }
LR_RT_API void lr_f128_pow(uint8_t r[16], const uint8_t a[16], const uint8_t b[16]) { st(r, f128_pow(ld(a), ld(b))); }

LR_RT_API void lr_f128_from_i32(uint8_t r[16], int32_t v) { st(r, f128_from_i32(v)); }
LR_RT_API void lr_f128_from_i64(uint8_t r[16], int64_t v) { st(r, f128_from_i64(v)); }
LR_RT_API void lr_f128_from_f32(uint8_t r[16], float  v) { st(r, f128_from_f32(v)); }
LR_RT_API void lr_f128_from_f64(uint8_t r[16], double v) { st(r, f128_from_f64(v)); }
LR_RT_API float   lr_f128_to_f32(const uint8_t a[16]) { return (float)f128_to_f64(ld(a)); }
LR_RT_API double  lr_f128_to_f64(const uint8_t a[16]) { return f128_to_f64(ld(a)); }
LR_RT_API int32_t lr_f128_to_i32(const uint8_t a[16]) { return f128_to_i32(ld(a)); }
LR_RT_API int64_t lr_f128_to_i64(const uint8_t a[16]) { return f128_to_i64(ld(a)); }

/* Comparisons: return 1 (true) / 0 (false).  Unordered (NaN) is false. */
LR_RT_API int32_t lr_f128_eq(const uint8_t a[16], const uint8_t b[16]) { return f128_cmp_internal(ld(a), ld(b)) == 0; }
LR_RT_API int32_t lr_f128_ne(const uint8_t a[16], const uint8_t b[16]) { int c = f128_cmp_internal(ld(a), ld(b)); return c == 2 ? 1 : c != 0; }
LR_RT_API int32_t lr_f128_lt(const uint8_t a[16], const uint8_t b[16]) { int c = f128_cmp_internal(ld(a), ld(b)); return c == 2 ? 0 : c < 0; }
LR_RT_API int32_t lr_f128_le(const uint8_t a[16], const uint8_t b[16]) { int c = f128_cmp_internal(ld(a), ld(b)); return c == 2 ? 0 : c <= 0; }
LR_RT_API int32_t lr_f128_gt(const uint8_t a[16], const uint8_t b[16]) { int c = f128_cmp_internal(ld(a), ld(b)); return c == 2 ? 0 : c > 0; }
LR_RT_API int32_t lr_f128_ge(const uint8_t a[16], const uint8_t b[16]) { int c = f128_cmp_internal(ld(a), ld(b)); return c == 2 ? 0 : c >= 0; }
