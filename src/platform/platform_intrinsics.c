#include "platform.h"
#include "platform_os.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Pre-assembled riscv64 blobs as C byte arrays -- always available on every
   host so that cross-compilation (x86_64 host -> riscv64 target) works. */
#include "platform_intrinsic_blobs_riscv64.h"

typedef struct {
    const char *name;
    const uint8_t *blob_begin;
    const uint8_t *blob_end;
} lr_platform_intrinsic_desc_t;

#if (defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__) || \
     (defined(__riscv) && __riscv_xlen == 64))) || \
    (defined(__APPLE__) && defined(__aarch64__))
#define LR_PLATFORM_HAS_INTRINSIC_BLOBS 1
extern const uint8_t lr_stub_llvm_fabs_f32_begin[];
extern const uint8_t lr_stub_llvm_fabs_f32_end[];
extern const uint8_t lr_stub_llvm_fabs_f64_begin[];
extern const uint8_t lr_stub_llvm_fabs_f64_end[];
extern const uint8_t lr_stub_llvm_sqrt_f32_begin[];
extern const uint8_t lr_stub_llvm_sqrt_f32_end[];
extern const uint8_t lr_stub_llvm_sqrt_f64_begin[];
extern const uint8_t lr_stub_llvm_sqrt_f64_end[];
extern const uint8_t lr_stub_llvm_exp_f32_begin[];
extern const uint8_t lr_stub_llvm_exp_f32_end[];
extern const uint8_t lr_stub_llvm_exp_f64_begin[];
extern const uint8_t lr_stub_llvm_exp_f64_end[];
extern const uint8_t lr_stub_llvm_pow_f32_begin[];
extern const uint8_t lr_stub_llvm_pow_f32_end[];
extern const uint8_t lr_stub_llvm_pow_f64_begin[];
extern const uint8_t lr_stub_llvm_pow_f64_end[];
extern const uint8_t lr_stub_llvm_copysign_f32_begin[];
extern const uint8_t lr_stub_llvm_copysign_f32_end[];
extern const uint8_t lr_stub_llvm_copysign_f64_begin[];
extern const uint8_t lr_stub_llvm_copysign_f64_end[];
extern const uint8_t lr_stub_llvm_powi_f32_i32_begin[];
extern const uint8_t lr_stub_llvm_powi_f32_i32_end[];
extern const uint8_t lr_stub_llvm_powi_f64_i32_begin[];
extern const uint8_t lr_stub_llvm_powi_f64_i32_end[];
extern const uint8_t lr_stub_llvm_powi_f32_i64_begin[];
extern const uint8_t lr_stub_llvm_powi_f32_i64_end[];
extern const uint8_t lr_stub_llvm_powi_f64_i64_begin[];
extern const uint8_t lr_stub_llvm_powi_f64_i64_end[];
extern const uint8_t lr_stub_llvm_memset_i32_begin[];
extern const uint8_t lr_stub_llvm_memset_i32_end[];
extern const uint8_t lr_stub_llvm_memset_i64_begin[];
extern const uint8_t lr_stub_llvm_memset_i64_end[];
extern const uint8_t lr_stub_llvm_memcpy_i32_begin[];
extern const uint8_t lr_stub_llvm_memcpy_i32_end[];
extern const uint8_t lr_stub_llvm_memcpy_i64_begin[];
extern const uint8_t lr_stub_llvm_memcpy_i64_end[];
extern const uint8_t lr_stub_llvm_memmove_i32_begin[];
extern const uint8_t lr_stub_llvm_memmove_i32_end[];
extern const uint8_t lr_stub_llvm_memmove_i64_begin[];
extern const uint8_t lr_stub_llvm_memmove_i64_end[];
extern const uint8_t lr_stub_llvm_sin_f32_begin[];
extern const uint8_t lr_stub_llvm_sin_f32_end[];
extern const uint8_t lr_stub_llvm_sin_f64_begin[];
extern const uint8_t lr_stub_llvm_sin_f64_end[];
extern const uint8_t lr_stub_llvm_cos_f32_begin[];
extern const uint8_t lr_stub_llvm_cos_f32_end[];
extern const uint8_t lr_stub_llvm_cos_f64_begin[];
extern const uint8_t lr_stub_llvm_cos_f64_end[];
extern const uint8_t lr_stub_llvm_log_f32_begin[];
extern const uint8_t lr_stub_llvm_log_f32_end[];
extern const uint8_t lr_stub_llvm_log_f64_begin[];
extern const uint8_t lr_stub_llvm_log_f64_end[];
extern const uint8_t lr_stub_llvm_log2_f32_begin[];
extern const uint8_t lr_stub_llvm_log2_f32_end[];
extern const uint8_t lr_stub_llvm_log2_f64_begin[];
extern const uint8_t lr_stub_llvm_log2_f64_end[];
extern const uint8_t lr_stub_llvm_log10_f32_begin[];
extern const uint8_t lr_stub_llvm_log10_f32_end[];
extern const uint8_t lr_stub_llvm_log10_f64_begin[];
extern const uint8_t lr_stub_llvm_log10_f64_end[];
extern const uint8_t lr_stub_llvm_exp2_f32_begin[];
extern const uint8_t lr_stub_llvm_exp2_f32_end[];
extern const uint8_t lr_stub_llvm_exp2_f64_begin[];
extern const uint8_t lr_stub_llvm_exp2_f64_end[];
extern const uint8_t lr_stub_llvm_exp10_f32_begin[];
extern const uint8_t lr_stub_llvm_exp10_f32_end[];
extern const uint8_t lr_stub_llvm_exp10_f64_begin[];
extern const uint8_t lr_stub_llvm_exp10_f64_end[];
extern const uint8_t lr_stub_llvm_floor_f32_begin[];
extern const uint8_t lr_stub_llvm_floor_f32_end[];
extern const uint8_t lr_stub_llvm_floor_f64_begin[];
extern const uint8_t lr_stub_llvm_floor_f64_end[];
extern const uint8_t lr_stub_llvm_ceil_f32_begin[];
extern const uint8_t lr_stub_llvm_ceil_f32_end[];
extern const uint8_t lr_stub_llvm_ceil_f64_begin[];
extern const uint8_t lr_stub_llvm_ceil_f64_end[];
extern const uint8_t lr_stub_llvm_trunc_f32_begin[];
extern const uint8_t lr_stub_llvm_trunc_f32_end[];
extern const uint8_t lr_stub_llvm_trunc_f64_begin[];
extern const uint8_t lr_stub_llvm_trunc_f64_end[];
extern const uint8_t lr_stub_llvm_round_f32_begin[];
extern const uint8_t lr_stub_llvm_round_f32_end[];
extern const uint8_t lr_stub_llvm_round_f64_begin[];
extern const uint8_t lr_stub_llvm_round_f64_end[];
extern const uint8_t lr_stub_llvm_rint_f32_begin[];
extern const uint8_t lr_stub_llvm_rint_f32_end[];
extern const uint8_t lr_stub_llvm_rint_f64_begin[];
extern const uint8_t lr_stub_llvm_rint_f64_end[];
extern const uint8_t lr_stub_llvm_nearbyint_f32_begin[];
extern const uint8_t lr_stub_llvm_nearbyint_f32_end[];
extern const uint8_t lr_stub_llvm_nearbyint_f64_begin[];
extern const uint8_t lr_stub_llvm_nearbyint_f64_end[];
extern const uint8_t lr_stub_llvm_fma_f32_begin[];
extern const uint8_t lr_stub_llvm_fma_f32_end[];
extern const uint8_t lr_stub_llvm_fma_f64_begin[];
extern const uint8_t lr_stub_llvm_fma_f64_end[];
extern const uint8_t lr_stub_llvm_fmuladd_v2f32_begin[];
extern const uint8_t lr_stub_llvm_fmuladd_v2f32_end[];
extern const uint8_t lr_stub_llvm_fmuladd_v4f32_begin[];
extern const uint8_t lr_stub_llvm_fmuladd_v4f32_end[];
extern const uint8_t lr_stub_llvm_fmuladd_v2f64_begin[];
extern const uint8_t lr_stub_llvm_fmuladd_v2f64_end[];
extern const uint8_t lr_stub_llvm_minnum_f32_begin[];
extern const uint8_t lr_stub_llvm_minnum_f32_end[];
extern const uint8_t lr_stub_llvm_minnum_f64_begin[];
extern const uint8_t lr_stub_llvm_minnum_f64_end[];
extern const uint8_t lr_stub_llvm_maxnum_f32_begin[];
extern const uint8_t lr_stub_llvm_maxnum_f32_end[];
extern const uint8_t lr_stub_llvm_maxnum_f64_begin[];
extern const uint8_t lr_stub_llvm_maxnum_f64_end[];
extern const uint8_t lr_stub_llvm_abs_i32_begin[];
extern const uint8_t lr_stub_llvm_abs_i32_end[];
extern const uint8_t lr_stub_llvm_abs_i8_begin[];
extern const uint8_t lr_stub_llvm_abs_i8_end[];
extern const uint8_t lr_stub_llvm_abs_i16_begin[];
extern const uint8_t lr_stub_llvm_abs_i16_end[];
extern const uint8_t lr_stub_llvm_abs_i64_begin[];
extern const uint8_t lr_stub_llvm_abs_i64_end[];
extern const uint8_t lr_stub_llvm_assume_begin[];
extern const uint8_t lr_stub_llvm_assume_end[];
extern const uint8_t lr_stub_llvm_trap_begin[];
extern const uint8_t lr_stub_llvm_trap_end[];
extern const uint8_t lr_stub_llvm_is_fpclass_f32_begin[];
extern const uint8_t lr_stub_llvm_is_fpclass_f32_end[];
extern const uint8_t lr_stub_llvm_is_fpclass_f64_begin[];
extern const uint8_t lr_stub_llvm_is_fpclass_f64_end[];
#else
#define LR_PLATFORM_HAS_INTRINSIC_BLOBS 0
#if !defined(LR_PLATFORM_SKIP_HOST_BLOB_CHECK)
#error "unsupported host platform for intrinsic blobs"
#endif
#endif

#if LR_PLATFORM_HAS_INTRINSIC_BLOBS
#define LR_STUB_BLOB(begin, end) begin, end
#else
#define LR_STUB_BLOB(begin, end) NULL, NULL
#endif

static const lr_platform_intrinsic_desc_t g_intrinsics[] = {
    { "llvm.fabs.f32", LR_STUB_BLOB(lr_stub_llvm_fabs_f32_begin, lr_stub_llvm_fabs_f32_end) },
    { "llvm.fabs.f64", LR_STUB_BLOB(lr_stub_llvm_fabs_f64_begin, lr_stub_llvm_fabs_f64_end) },
    { "llvm.sqrt.f32", LR_STUB_BLOB(lr_stub_llvm_sqrt_f32_begin, lr_stub_llvm_sqrt_f32_end) },
    { "llvm.sqrt.f64", LR_STUB_BLOB(lr_stub_llvm_sqrt_f64_begin, lr_stub_llvm_sqrt_f64_end) },
    { "llvm.exp.f32", LR_STUB_BLOB(lr_stub_llvm_exp_f32_begin, lr_stub_llvm_exp_f32_end) },
    { "llvm.exp.f64", LR_STUB_BLOB(lr_stub_llvm_exp_f64_begin, lr_stub_llvm_exp_f64_end) },
    { "llvm.pow.f32", LR_STUB_BLOB(lr_stub_llvm_pow_f32_begin, lr_stub_llvm_pow_f32_end) },
    { "llvm.pow.f64", LR_STUB_BLOB(lr_stub_llvm_pow_f64_begin, lr_stub_llvm_pow_f64_end) },
    { "llvm.copysign.f32", LR_STUB_BLOB(lr_stub_llvm_copysign_f32_begin, lr_stub_llvm_copysign_f32_end) },
    { "llvm.copysign.f64", LR_STUB_BLOB(lr_stub_llvm_copysign_f64_begin, lr_stub_llvm_copysign_f64_end) },
    { "llvm.powi.f32", LR_STUB_BLOB(lr_stub_llvm_powi_f32_i32_begin, lr_stub_llvm_powi_f32_i32_end) },
    { "llvm.powi.f64", LR_STUB_BLOB(lr_stub_llvm_powi_f64_i32_begin, lr_stub_llvm_powi_f64_i32_end) },
    { "llvm.powi.f32.i32", LR_STUB_BLOB(lr_stub_llvm_powi_f32_i32_begin, lr_stub_llvm_powi_f32_i32_end) },
    { "llvm.powi.f64.i32", LR_STUB_BLOB(lr_stub_llvm_powi_f64_i32_begin, lr_stub_llvm_powi_f64_i32_end) },
    { "llvm.powi.f32.i64", LR_STUB_BLOB(lr_stub_llvm_powi_f32_i64_begin, lr_stub_llvm_powi_f32_i64_end) },
    { "llvm.powi.f64.i64", LR_STUB_BLOB(lr_stub_llvm_powi_f64_i64_begin, lr_stub_llvm_powi_f64_i64_end) },
    { "llvm.memset.p0i8.i32", LR_STUB_BLOB(lr_stub_llvm_memset_i32_begin, lr_stub_llvm_memset_i32_end) },
    { "llvm.memset.p0i8.i64", LR_STUB_BLOB(lr_stub_llvm_memset_i64_begin, lr_stub_llvm_memset_i64_end) },
    { "llvm.memset.p0.i32", LR_STUB_BLOB(lr_stub_llvm_memset_i32_begin, lr_stub_llvm_memset_i32_end) },
    { "llvm.memset.p0.i64", LR_STUB_BLOB(lr_stub_llvm_memset_i64_begin, lr_stub_llvm_memset_i64_end) },
    { "llvm.memcpy.p0i8.p0i8.i32", LR_STUB_BLOB(lr_stub_llvm_memcpy_i32_begin, lr_stub_llvm_memcpy_i32_end) },
    { "llvm.memcpy.p0i8.p0i8.i64", LR_STUB_BLOB(lr_stub_llvm_memcpy_i64_begin, lr_stub_llvm_memcpy_i64_end) },
    { "llvm.memcpy.p0.p0.i32", LR_STUB_BLOB(lr_stub_llvm_memcpy_i32_begin, lr_stub_llvm_memcpy_i32_end) },
    { "llvm.memcpy.p0.p0.i64", LR_STUB_BLOB(lr_stub_llvm_memcpy_i64_begin, lr_stub_llvm_memcpy_i64_end) },
    { "llvm.memmove.p0i8.p0i8.i32", LR_STUB_BLOB(lr_stub_llvm_memmove_i32_begin, lr_stub_llvm_memmove_i32_end) },
    { "llvm.memmove.p0i8.p0i8.i64", LR_STUB_BLOB(lr_stub_llvm_memmove_i64_begin, lr_stub_llvm_memmove_i64_end) },
    { "llvm.memmove.p0.p0.i32", LR_STUB_BLOB(lr_stub_llvm_memmove_i32_begin, lr_stub_llvm_memmove_i32_end) },
    { "llvm.memmove.p0.p0.i64", LR_STUB_BLOB(lr_stub_llvm_memmove_i64_begin, lr_stub_llvm_memmove_i64_end) },
    { "llvm.sin.f32", LR_STUB_BLOB(lr_stub_llvm_sin_f32_begin, lr_stub_llvm_sin_f32_end) },
    { "llvm.sin.f64", LR_STUB_BLOB(lr_stub_llvm_sin_f64_begin, lr_stub_llvm_sin_f64_end) },
    { "llvm.cos.f32", LR_STUB_BLOB(lr_stub_llvm_cos_f32_begin, lr_stub_llvm_cos_f32_end) },
    { "llvm.cos.f64", LR_STUB_BLOB(lr_stub_llvm_cos_f64_begin, lr_stub_llvm_cos_f64_end) },
    { "llvm.log.f32", LR_STUB_BLOB(lr_stub_llvm_log_f32_begin, lr_stub_llvm_log_f32_end) },
    { "llvm.log.f64", LR_STUB_BLOB(lr_stub_llvm_log_f64_begin, lr_stub_llvm_log_f64_end) },
    { "llvm.log2.f32", LR_STUB_BLOB(lr_stub_llvm_log2_f32_begin, lr_stub_llvm_log2_f32_end) },
    { "llvm.log2.f64", LR_STUB_BLOB(lr_stub_llvm_log2_f64_begin, lr_stub_llvm_log2_f64_end) },
    { "llvm.log10.f32", LR_STUB_BLOB(lr_stub_llvm_log10_f32_begin, lr_stub_llvm_log10_f32_end) },
    { "llvm.log10.f64", LR_STUB_BLOB(lr_stub_llvm_log10_f64_begin, lr_stub_llvm_log10_f64_end) },
    { "llvm.exp2.f32", LR_STUB_BLOB(lr_stub_llvm_exp2_f32_begin, lr_stub_llvm_exp2_f32_end) },
    { "llvm.exp2.f64", LR_STUB_BLOB(lr_stub_llvm_exp2_f64_begin, lr_stub_llvm_exp2_f64_end) },
    { "llvm.floor.f32", LR_STUB_BLOB(lr_stub_llvm_floor_f32_begin, lr_stub_llvm_floor_f32_end) },
    { "llvm.floor.f64", LR_STUB_BLOB(lr_stub_llvm_floor_f64_begin, lr_stub_llvm_floor_f64_end) },
    { "llvm.ceil.f32", LR_STUB_BLOB(lr_stub_llvm_ceil_f32_begin, lr_stub_llvm_ceil_f32_end) },
    { "llvm.ceil.f64", LR_STUB_BLOB(lr_stub_llvm_ceil_f64_begin, lr_stub_llvm_ceil_f64_end) },
    { "llvm.trunc.f32", LR_STUB_BLOB(lr_stub_llvm_trunc_f32_begin, lr_stub_llvm_trunc_f32_end) },
    { "llvm.trunc.f64", LR_STUB_BLOB(lr_stub_llvm_trunc_f64_begin, lr_stub_llvm_trunc_f64_end) },
    { "llvm.round.f32", LR_STUB_BLOB(lr_stub_llvm_round_f32_begin, lr_stub_llvm_round_f32_end) },
    { "llvm.round.f64", LR_STUB_BLOB(lr_stub_llvm_round_f64_begin, lr_stub_llvm_round_f64_end) },
    { "llvm.rint.f32", LR_STUB_BLOB(lr_stub_llvm_rint_f32_begin, lr_stub_llvm_rint_f32_end) },
    { "llvm.rint.f64", LR_STUB_BLOB(lr_stub_llvm_rint_f64_begin, lr_stub_llvm_rint_f64_end) },
    { "llvm.nearbyint.f32", LR_STUB_BLOB(lr_stub_llvm_nearbyint_f32_begin, lr_stub_llvm_nearbyint_f32_end) },
    { "llvm.nearbyint.f64", LR_STUB_BLOB(lr_stub_llvm_nearbyint_f64_begin, lr_stub_llvm_nearbyint_f64_end) },
    { "llvm.fma.f32", LR_STUB_BLOB(lr_stub_llvm_fma_f32_begin, lr_stub_llvm_fma_f32_end) },
    { "llvm.fma.f64", LR_STUB_BLOB(lr_stub_llvm_fma_f64_begin, lr_stub_llvm_fma_f64_end) },
    { "llvm.fmuladd.f32", LR_STUB_BLOB(lr_stub_llvm_fma_f32_begin, lr_stub_llvm_fma_f32_end) },
    { "llvm.fmuladd.f64", LR_STUB_BLOB(lr_stub_llvm_fma_f64_begin, lr_stub_llvm_fma_f64_end) },
    { "llvm.fmuladd.v2f32", LR_STUB_BLOB(lr_stub_llvm_fmuladd_v2f32_begin, lr_stub_llvm_fmuladd_v2f32_end) },
    { "llvm.fmuladd.v4f32", LR_STUB_BLOB(lr_stub_llvm_fmuladd_v4f32_begin, lr_stub_llvm_fmuladd_v4f32_end) },
    { "llvm.fmuladd.v2f64", LR_STUB_BLOB(lr_stub_llvm_fmuladd_v2f64_begin, lr_stub_llvm_fmuladd_v2f64_end) },
    { "llvm.minnum.f32", LR_STUB_BLOB(lr_stub_llvm_minnum_f32_begin, lr_stub_llvm_minnum_f32_end) },
    { "llvm.minnum.f64", LR_STUB_BLOB(lr_stub_llvm_minnum_f64_begin, lr_stub_llvm_minnum_f64_end) },
    { "llvm.maxnum.f32", LR_STUB_BLOB(lr_stub_llvm_maxnum_f32_begin, lr_stub_llvm_maxnum_f32_end) },
    { "llvm.maxnum.f64", LR_STUB_BLOB(lr_stub_llvm_maxnum_f64_begin, lr_stub_llvm_maxnum_f64_end) },
    { "llvm.abs.i8", LR_STUB_BLOB(lr_stub_llvm_abs_i8_begin, lr_stub_llvm_abs_i8_end) },
    { "llvm.abs.i16", LR_STUB_BLOB(lr_stub_llvm_abs_i16_begin, lr_stub_llvm_abs_i16_end) },
    { "llvm.abs.i32", LR_STUB_BLOB(lr_stub_llvm_abs_i32_begin, lr_stub_llvm_abs_i32_end) },
    { "llvm.abs.i64", LR_STUB_BLOB(lr_stub_llvm_abs_i64_begin, lr_stub_llvm_abs_i64_end) },
    { "llvm.assume", LR_STUB_BLOB(lr_stub_llvm_assume_begin, lr_stub_llvm_assume_end) },
    { "llvm.trap", LR_STUB_BLOB(lr_stub_llvm_trap_begin, lr_stub_llvm_trap_end) },
    { "llvm.is.fpclass.f32", LR_STUB_BLOB(lr_stub_llvm_is_fpclass_f32_begin, lr_stub_llvm_is_fpclass_f32_end) },
    { "llvm.is.fpclass.f64", LR_STUB_BLOB(lr_stub_llvm_is_fpclass_f64_begin, lr_stub_llvm_is_fpclass_f64_end) },
};

static const char *normalize_intrinsic_name(const char *name) {
    if (!name)
        return NULL;
    while (*name == '\1' || *name == '_')
        name++;
    return name;
}

static const lr_platform_intrinsic_desc_t *lookup_intrinsic(const char *name) {
    size_t i;
    if (!name || !name[0])
        return NULL;
    for (i = 0; i < sizeof(g_intrinsics) / sizeof(g_intrinsics[0]); i++) {
        if (strcmp(g_intrinsics[i].name, name) == 0)
            return &g_intrinsics[i];
    }
    return NULL;
}

static bool starts_with(const char *s, const char *prefix) {
    if (!s || !prefix)
        return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool parse_int_suffix_bits(const char *name,
                                  const char *prefix,
                                  unsigned *out_bits) {
    const char *p;
    unsigned bits = 0;
    if (!name || !prefix || !out_bits)
        return false;
    if (!starts_with(name, prefix))
        return false;
    p = name + strlen(prefix);
    if (*p == '\0')
        return false;
    while (*p >= '0' && *p <= '9') {
        bits = bits * 10u + (unsigned)(*p - '0');
        p++;
    }
    if (*p != '\0')
        return false;
    *out_bits = bits;
    return true;
}

static uint64_t lr_intrin_umax_i64(uint64_t a, uint64_t b) { return (a > b) ? a : b; }
static uint64_t lr_intrin_umin_i64(uint64_t a, uint64_t b) { return (a < b) ? a : b; }
static int64_t lr_intrin_smax_i64(int64_t a, int64_t b) { return (a > b) ? a : b; }
static int64_t lr_intrin_smin_i64(int64_t a, int64_t b) { return (a < b) ? a : b; }
static uint32_t lr_intrin_umax_i32(uint32_t a, uint32_t b) { return (a > b) ? a : b; }
static uint32_t lr_intrin_umin_i32(uint32_t a, uint32_t b) { return (a < b) ? a : b; }
static int32_t lr_intrin_smax_i32(int32_t a, int32_t b) { return (a > b) ? a : b; }
static int32_t lr_intrin_smin_i32(int32_t a, int32_t b) { return (a < b) ? a : b; }

static int8_t lr_intrin_abs_i8(int8_t x, uint8_t poison) { (void)poison; return x < 0 ? (int8_t)-x : x; }
static int16_t lr_intrin_abs_i16(int16_t x, uint8_t poison) { (void)poison; return x < 0 ? (int16_t)-x : x; }
static int32_t lr_intrin_abs_i32_fallback(int32_t x, uint8_t poison) { (void)poison; return x < 0 ? -x : x; }
static int64_t lr_intrin_abs_i64_fallback(int64_t x, uint8_t poison) { (void)poison; return x < 0 ? -x : x; }

static void lr_intrin_noop(void) { }
static void lr_intrin_assume_i1(uint8_t cond) { (void)cond; }
static void lr_intrin_trap(void) { abort(); }

/* Unknown object size sentinel for llvm.objectsize.* intrinsics.
   Returning all-ones matches "unknown size" semantics used by fortified libc
   lowering paths and avoids rejecting host-clang generated IR in no-link mode. */
static uint64_t lr_intrin_objectsize_i64_unknown(const void *ptr, ...) {
    (void)ptr;
    return UINT64_MAX;
}

static uint32_t lr_intrin_objectsize_i32_unknown(const void *ptr, ...) {
    (void)ptr;
    return UINT32_MAX;
}

static double lr_intrin_exp10_f64(double x) { return pow(10.0, x); }
static float lr_intrin_exp10_f32(float x) { return powf(10.0f, x); }

static uint8_t lr_intrin_ctpop_i8(uint8_t x) {
    uint8_t c = 0;
    while (x) {
        x &= (uint8_t)(x - 1u);
        c++;
    }
    return c;
}

static uint16_t lr_intrin_ctpop_i16(uint16_t x) {
    uint16_t c = 0;
    while (x) {
        x &= (uint16_t)(x - 1u);
        c++;
    }
    return c;
}

static uint32_t lr_intrin_ctpop_i32(uint32_t x) {
    uint32_t c = 0;
    while (x) {
        x &= (x - 1u);
        c++;
    }
    return c;
}

static uint64_t lr_intrin_ctpop_i64(uint64_t x) {
    uint64_t c = 0;
    while (x) {
        x &= (x - 1u);
        c++;
    }
    return c;
}

static uint8_t lr_intrin_ctlz_i8(uint8_t x, uint8_t is_zero_undef) {
    uint8_t n = 0;
    if (x == 0)
        return is_zero_undef ? 0 : 8;
    while ((x & 0x80u) == 0u) {
        n++;
        x <<= 1;
    }
    return n;
}

static uint16_t lr_intrin_ctlz_i16(uint16_t x, uint8_t is_zero_undef) {
    uint16_t n = 0;
    if (x == 0)
        return is_zero_undef ? 0 : 16;
    while ((x & 0x8000u) == 0u) {
        n++;
        x <<= 1;
    }
    return n;
}

static uint32_t lr_intrin_ctlz_i32(uint32_t x, uint8_t is_zero_undef) {
    uint32_t n = 0;
    if (x == 0)
        return is_zero_undef ? 0 : 32;
    while ((x & 0x80000000u) == 0u) {
        n++;
        x <<= 1;
    }
    return n;
}

static uint64_t lr_intrin_ctlz_i64(uint64_t x, uint8_t is_zero_undef) {
    uint64_t n = 0;
    if (x == 0)
        return is_zero_undef ? 0 : 64;
    while ((x & 0x8000000000000000ULL) == 0ULL) {
        n++;
        x <<= 1;
    }
    return n;
}

static uint8_t lr_intrin_cttz_i8(uint8_t x, uint8_t is_zero_undef) {
    uint8_t n = 0;
    if (x == 0)
        return is_zero_undef ? 0 : 8;
    while ((x & 1u) == 0u) {
        n++;
        x >>= 1;
    }
    return n;
}

static uint16_t lr_intrin_cttz_i16(uint16_t x, uint8_t is_zero_undef) {
    uint16_t n = 0;
    if (x == 0)
        return is_zero_undef ? 0 : 16;
    while ((x & 1u) == 0u) {
        n++;
        x >>= 1;
    }
    return n;
}

static uint32_t lr_intrin_cttz_i32(uint32_t x, uint8_t is_zero_undef) {
    uint32_t n = 0;
    if (x == 0)
        return is_zero_undef ? 0 : 32;
    while ((x & 1u) == 0u) {
        n++;
        x >>= 1;
    }
    return n;
}

static uint64_t lr_intrin_cttz_i64(uint64_t x, uint8_t is_zero_undef) {
    uint64_t n = 0;
    if (x == 0)
        return is_zero_undef ? 0 : 64;
    while ((x & 1u) == 0u) {
        n++;
        x >>= 1;
    }
    return n;
}

static const char *intrinsic_libc_name_impl(const char *name) {
    if (!name || strncmp(name, "llvm.", 5) != 0)
        return NULL;

    if (strcmp(name, "llvm.fabs.f32") == 0) return "fabsf";
    if (strcmp(name, "llvm.fabs.f64") == 0) return "fabs";
    if (strcmp(name, "llvm.sqrt.f32") == 0) return "sqrtf";
    if (strcmp(name, "llvm.sqrt.f64") == 0) return "sqrt";
    if (strcmp(name, "llvm.pow.f32") == 0) return "powf";
    if (strcmp(name, "llvm.pow.f64") == 0) return "pow";
    if (strcmp(name, "llvm.copysign.f32") == 0) return "copysignf";
    if (strcmp(name, "llvm.copysign.f64") == 0) return "copysign";
    if (strcmp(name, "llvm.sin.f32") == 0) return "sinf";
    if (strcmp(name, "llvm.sin.f64") == 0) return "sin";
    if (strcmp(name, "llvm.asin.f32") == 0) return "asinf";
    if (strcmp(name, "llvm.asin.f64") == 0) return "asin";
    if (strcmp(name, "llvm.acos.f32") == 0) return "acosf";
    if (strcmp(name, "llvm.acos.f64") == 0) return "acos";
    if (strcmp(name, "llvm.atan.f32") == 0) return "atanf";
    if (strcmp(name, "llvm.atan.f64") == 0) return "atan";
    if (strcmp(name, "llvm.atan2.f32") == 0) return "atan2f";
    if (strcmp(name, "llvm.atan2.f64") == 0) return "atan2";
    if (strcmp(name, "llvm.cos.f32") == 0) return "cosf";
    if (strcmp(name, "llvm.cos.f64") == 0) return "cos";
    if (strcmp(name, "llvm.tan.f32") == 0) return "tanf";
    if (strcmp(name, "llvm.tan.f64") == 0) return "tan";
    if (strcmp(name, "llvm.exp.f32") == 0) return "expf";
    if (strcmp(name, "llvm.exp.f64") == 0) return "exp";
    if (strcmp(name, "llvm.exp10.f32") == 0) {
#if defined(__APPLE__)
        return "__exp10f";
#else
        return "exp10f";
#endif
    }
    if (strcmp(name, "llvm.exp10.f64") == 0) {
#if defined(__APPLE__)
        return "__exp10";
#else
        return "exp10";
#endif
    }
    if (strcmp(name, "llvm.exp2.f32") == 0) return "exp2f";
    if (strcmp(name, "llvm.exp2.f64") == 0) return "exp2";
    if (strcmp(name, "llvm.log.f32") == 0) return "logf";
    if (strcmp(name, "llvm.log.f64") == 0) return "log";
    if (strcmp(name, "llvm.log2.f32") == 0) return "log2f";
    if (strcmp(name, "llvm.log2.f64") == 0) return "log2";
    if (strcmp(name, "llvm.log10.f32") == 0) return "log10f";
    if (strcmp(name, "llvm.log10.f64") == 0) return "log10";
    if (strcmp(name, "llvm.sinh.f32") == 0) return "sinhf";
    if (strcmp(name, "llvm.sinh.f64") == 0) return "sinh";
    if (strcmp(name, "llvm.cosh.f32") == 0) return "coshf";
    if (strcmp(name, "llvm.cosh.f64") == 0) return "cosh";
    if (strcmp(name, "llvm.tanh.f32") == 0) return "tanhf";
    if (strcmp(name, "llvm.tanh.f64") == 0) return "tanh";
    if (strcmp(name, "llvm.floor.f32") == 0) return "floorf";
    if (strcmp(name, "llvm.floor.f64") == 0) return "floor";
    if (strcmp(name, "llvm.ceil.f32") == 0) return "ceilf";
    if (strcmp(name, "llvm.ceil.f64") == 0) return "ceil";
    if (strcmp(name, "llvm.trunc.f32") == 0) return "truncf";
    if (strcmp(name, "llvm.trunc.f64") == 0) return "trunc";
    if (strcmp(name, "llvm.round.f32") == 0) return "roundf";
    if (strcmp(name, "llvm.round.f64") == 0) return "round";
    if (strcmp(name, "llvm.rint.f32") == 0) return "rintf";
    if (strcmp(name, "llvm.rint.f64") == 0) return "rint";
    if (strcmp(name, "llvm.nearbyint.f32") == 0) return "nearbyintf";
    if (strcmp(name, "llvm.nearbyint.f64") == 0) return "nearbyint";
    if (strcmp(name, "llvm.fma.f32") == 0) return "fmaf";
    if (strcmp(name, "llvm.fma.f64") == 0) return "fma";
    if (strcmp(name, "llvm.fmuladd.f32") == 0) return "fmaf";
    if (strcmp(name, "llvm.fmuladd.f64") == 0) return "fma";
    if (strcmp(name, "llvm.maximum.f32") == 0) return "fmaxf";
    if (strcmp(name, "llvm.maximum.f64") == 0) return "fmax";
    if (strcmp(name, "llvm.maxnum.f32") == 0) return "fmaxf";
    if (strcmp(name, "llvm.maxnum.f64") == 0) return "fmax";
    if (strcmp(name, "llvm.minimum.f32") == 0) return "fminf";
    if (strcmp(name, "llvm.minimum.f64") == 0) return "fmin";
    if (strcmp(name, "llvm.minnum.f32") == 0) return "fminf";
    if (strcmp(name, "llvm.minnum.f64") == 0) return "fmin";
    if (strcmp(name, "llvm.abs.i32") == 0) return "abs";
    if (strcmp(name, "llvm.abs.i64") == 0) return "llabs";

    if (starts_with(name, "llvm.memcpy.")) return "memcpy";
    if (starts_with(name, "llvm.memmove.")) return "memmove";
    if (starts_with(name, "llvm.memset.")) return "memset";

    return NULL;
}

static void *resolve_builtin_intrinsic_addr(const char *name) {
    unsigned bits = 0;
    if (!name || !name[0])
        return NULL;

    if (strcmp(name, "llvm.umax.i64") == 0) return (void *)(uintptr_t)lr_intrin_umax_i64;
    if (strcmp(name, "llvm.umin.i64") == 0) return (void *)(uintptr_t)lr_intrin_umin_i64;
    if (strcmp(name, "llvm.smax.i64") == 0) return (void *)(uintptr_t)lr_intrin_smax_i64;
    if (strcmp(name, "llvm.smin.i64") == 0) return (void *)(uintptr_t)lr_intrin_smin_i64;
    if (strcmp(name, "llvm.umax.i32") == 0) return (void *)(uintptr_t)lr_intrin_umax_i32;
    if (strcmp(name, "llvm.umin.i32") == 0) return (void *)(uintptr_t)lr_intrin_umin_i32;
    if (strcmp(name, "llvm.smax.i32") == 0) return (void *)(uintptr_t)lr_intrin_smax_i32;
    if (strcmp(name, "llvm.smin.i32") == 0) return (void *)(uintptr_t)lr_intrin_smin_i32;

    if (strcmp(name, "llvm.abs.i8") == 0) return (void *)(uintptr_t)lr_intrin_abs_i8;
    if (strcmp(name, "llvm.abs.i16") == 0) return (void *)(uintptr_t)lr_intrin_abs_i16;
    if (strcmp(name, "llvm.abs.i32") == 0) return (void *)(uintptr_t)lr_intrin_abs_i32_fallback;
    if (strcmp(name, "llvm.abs.i64") == 0) return (void *)(uintptr_t)lr_intrin_abs_i64_fallback;

    if (starts_with(name, "llvm.assume")) return (void *)(uintptr_t)lr_intrin_assume_i1;
    if (starts_with(name, "llvm.lifetime.start")) return (void *)(uintptr_t)lr_intrin_noop;
    if (starts_with(name, "llvm.lifetime.end")) return (void *)(uintptr_t)lr_intrin_noop;
    if (starts_with(name, "llvm.dbg.declare")) return (void *)(uintptr_t)lr_intrin_noop;
    if (starts_with(name, "llvm.dbg.value")) return (void *)(uintptr_t)lr_intrin_noop;
    if (strcmp(name, "llvm.trap") == 0) return (void *)(uintptr_t)lr_intrin_trap;

    if (strcmp(name, "llvm.exp10.f64") == 0 || strcmp(name, "exp10") == 0)
        return (void *)(uintptr_t)lr_intrin_exp10_f64;
    if (strcmp(name, "llvm.exp10.f32") == 0 || strcmp(name, "exp10f") == 0)
        return (void *)(uintptr_t)lr_intrin_exp10_f32;

    if (strcmp(name, "llvm.objectsize.i64") == 0 ||
        starts_with(name, "llvm.objectsize.i64.")) {
        return (void *)(uintptr_t)lr_intrin_objectsize_i64_unknown;
    }
    if (strcmp(name, "llvm.objectsize.i32") == 0 ||
        starts_with(name, "llvm.objectsize.i32.")) {
        return (void *)(uintptr_t)lr_intrin_objectsize_i32_unknown;
    }

    if (parse_int_suffix_bits(name, "llvm.ctpop.i", &bits)) {
        if (bits == 8) return (void *)(uintptr_t)lr_intrin_ctpop_i8;
        if (bits == 16) return (void *)(uintptr_t)lr_intrin_ctpop_i16;
        if (bits == 32) return (void *)(uintptr_t)lr_intrin_ctpop_i32;
        if (bits == 64) return (void *)(uintptr_t)lr_intrin_ctpop_i64;
        return NULL;
    }
    if (parse_int_suffix_bits(name, "llvm.ctlz.i", &bits)) {
        if (bits == 8) return (void *)(uintptr_t)lr_intrin_ctlz_i8;
        if (bits == 16) return (void *)(uintptr_t)lr_intrin_ctlz_i16;
        if (bits == 32) return (void *)(uintptr_t)lr_intrin_ctlz_i32;
        if (bits == 64) return (void *)(uintptr_t)lr_intrin_ctlz_i64;
        return NULL;
    }
    if (parse_int_suffix_bits(name, "llvm.cttz.i", &bits)) {
        if (bits == 8) return (void *)(uintptr_t)lr_intrin_cttz_i8;
        if (bits == 16) return (void *)(uintptr_t)lr_intrin_cttz_i16;
        if (bits == 32) return (void *)(uintptr_t)lr_intrin_cttz_i32;
        if (bits == 64) return (void *)(uintptr_t)lr_intrin_cttz_i64;
        return NULL;
    }

    return NULL;
}

static bool is_target_lowered_intrinsic(const char *name) {
    return starts_with(name, "llvm.va_start") ||
           starts_with(name, "llvm.va_end") ||
           starts_with(name, "llvm.va_copy");
}

const char *lr_platform_intrinsic_canonical_name(const char *name) {
    return normalize_intrinsic_name(name);
}

int lr_platform_intrinsic_lookup(const char *name,
                                 lr_platform_intrinsic_info_t *out_info) {
    const char *canonical = normalize_intrinsic_name(name);
    const lr_platform_intrinsic_desc_t *d = NULL;
    const char *libc_name = NULL;
    void *builtin_addr = NULL;
    bool known = false;
    lr_platform_intrinsic_strategy_t preferred = LR_PLATFORM_INTRINSIC_UNSUPPORTED;

    if (!canonical || !canonical[0]) {
        if (out_info)
            memset(out_info, 0, sizeof(*out_info));
        return 0;
    }

    d = lookup_intrinsic(canonical);
    libc_name = intrinsic_libc_name_impl(canonical);
    builtin_addr = resolve_builtin_intrinsic_addr(canonical);

    if (d && d->blob_begin && d->blob_end && d->blob_end > d->blob_begin) {
        known = true;
        preferred = LR_PLATFORM_INTRINSIC_BLOB;
    } else if (libc_name) {
        known = true;
        preferred = LR_PLATFORM_INTRINSIC_LIBC;
    } else if (builtin_addr) {
        known = true;
        preferred = LR_PLATFORM_INTRINSIC_BUILTIN;
    } else if (is_target_lowered_intrinsic(canonical)) {
        known = true;
        preferred = LR_PLATFORM_INTRINSIC_TARGET_LOWER;
    }

    if (out_info) {
        memset(out_info, 0, sizeof(*out_info));
        out_info->canonical_name = canonical;
        out_info->libc_name = libc_name;
        out_info->blob_begin = d ? d->blob_begin : NULL;
        out_info->blob_end = d ? d->blob_end : NULL;
        out_info->preferred_strategy = preferred;
        out_info->known = known;
        out_info->has_blob = d && d->blob_begin && d->blob_end && d->blob_end > d->blob_begin;
        out_info->has_builtin = builtin_addr != NULL;
    }

    return known ? 1 : 0;
}

static void *resolve_symbol_handle(void *handle, const char *name) {
    void *addr;
    if (!name || !name[0])
        return NULL;

    if (handle) {
        addr = lr_platform_dlsym(handle, name);
        if (addr)
            return addr;
        if (name[0] == '_')
            return lr_platform_dlsym(handle, name + 1);
        return NULL;
    }

    addr = lr_platform_dlsym_default(name);
    if (addr)
        return addr;
    if (name[0] == '_')
        return lr_platform_dlsym_default(name + 1);
    return NULL;
}

void *lr_platform_intrinsic_resolve_addr(const char *name, void *runtime_handle) {
    const char *canonical = normalize_intrinsic_name(name);
    const char *libc_name;
    void *addr;

    if (!canonical || !canonical[0])
        return NULL;

    libc_name = intrinsic_libc_name_impl(canonical);
    if (libc_name) {
        addr = resolve_symbol_handle(NULL, libc_name);
        if (!addr && runtime_handle)
            addr = resolve_symbol_handle(runtime_handle, libc_name);
        if (addr)
            return addr;
    }

    addr = resolve_builtin_intrinsic_addr(canonical);
    if (addr)
        return addr;

    return NULL;
}

bool lr_platform_intrinsic_is_supported(const char *name) {
    lr_platform_intrinsic_info_t info;
    if (lr_platform_intrinsic_lookup(name, &info) == 0)
        return false;
    return info.known;
}

size_t lr_platform_intrinsic_registry_count(void) {
    return sizeof(g_intrinsics) / sizeof(g_intrinsics[0]);
}

const char *lr_platform_intrinsic_registry_name(size_t idx) {
    size_t n = sizeof(g_intrinsics) / sizeof(g_intrinsics[0]);
    if (idx >= n)
        return NULL;
    return g_intrinsics[idx].name;
}

bool lr_platform_intrinsic_supported(const char *name) {
    lr_platform_intrinsic_info_t info;
    if (lr_platform_intrinsic_lookup(name, &info) == 0)
        return false;
    return info.has_blob;
}

bool lr_platform_intrinsic_blob_lookup(const char *name,
                                       const uint8_t **begin,
                                       const uint8_t **end) {
    lr_platform_intrinsic_info_t info;
    if (!begin || !end)
        return false;
    if (lr_platform_intrinsic_lookup(name, &info) == 0)
        return false;
    if (!info.has_blob)
        return false;
    *begin = info.blob_begin;
    *end = info.blob_end;
    return true;
}

size_t lr_platform_intrinsic_count(void) {
    return lr_platform_intrinsic_registry_count();
}

const char *lr_platform_intrinsic_name(size_t idx) {
    return lr_platform_intrinsic_registry_name(idx);
}

const char *lr_platform_intrinsic_libc_name(const char *name) {
    const char *canonical = normalize_intrinsic_name(name);
    const char *mapped;
    if (!canonical || !canonical[0])
        return name;
    mapped = intrinsic_libc_name_impl(canonical);
    if (mapped)
        return mapped;
    return name;
}

/* --- Target-specific riscv64 blob table (from pre-assembled byte arrays) --- */

#define LR_RV_BLOB(name) \
    lr_rvblob_##name, lr_rvblob_##name + sizeof(lr_rvblob_##name)

static const lr_platform_intrinsic_desc_t g_riscv64_intrinsics[] = {
    { "llvm.fabs.f32", LR_RV_BLOB(llvm_fabs_f32) },
    { "llvm.fabs.f64", LR_RV_BLOB(llvm_fabs_f64) },
    { "llvm.sqrt.f32", LR_RV_BLOB(llvm_sqrt_f32) },
    { "llvm.sqrt.f64", LR_RV_BLOB(llvm_sqrt_f64) },
    { "llvm.exp.f32", LR_RV_BLOB(llvm_exp_f32) },
    { "llvm.exp.f64", LR_RV_BLOB(llvm_exp_f64) },
    { "llvm.pow.f32", LR_RV_BLOB(llvm_pow_f32) },
    { "llvm.pow.f64", LR_RV_BLOB(llvm_pow_f64) },
    { "llvm.copysign.f32", LR_RV_BLOB(llvm_copysign_f32) },
    { "llvm.copysign.f64", LR_RV_BLOB(llvm_copysign_f64) },
    { "llvm.powi.f32", LR_RV_BLOB(llvm_powi_f32_i32) },
    { "llvm.powi.f64", LR_RV_BLOB(llvm_powi_f64_i32) },
    { "llvm.powi.f32.i32", LR_RV_BLOB(llvm_powi_f32_i32) },
    { "llvm.powi.f64.i32", LR_RV_BLOB(llvm_powi_f64_i32) },
    { "llvm.powi.f32.i64", LR_RV_BLOB(llvm_powi_f32_i64) },
    { "llvm.powi.f64.i64", LR_RV_BLOB(llvm_powi_f64_i64) },
    { "llvm.memset.p0i8.i32", LR_RV_BLOB(llvm_memset_i32) },
    { "llvm.memset.p0i8.i64", LR_RV_BLOB(llvm_memset_i64) },
    { "llvm.memset.p0.i32", LR_RV_BLOB(llvm_memset_i32) },
    { "llvm.memset.p0.i64", LR_RV_BLOB(llvm_memset_i64) },
    { "llvm.memcpy.p0i8.p0i8.i32", LR_RV_BLOB(llvm_memcpy_i32) },
    { "llvm.memcpy.p0i8.p0i8.i64", LR_RV_BLOB(llvm_memcpy_i64) },
    { "llvm.memcpy.p0.p0.i32", LR_RV_BLOB(llvm_memcpy_i32) },
    { "llvm.memcpy.p0.p0.i64", LR_RV_BLOB(llvm_memcpy_i64) },
    { "llvm.memmove.p0i8.p0i8.i32", LR_RV_BLOB(llvm_memmove_i32) },
    { "llvm.memmove.p0i8.p0i8.i64", LR_RV_BLOB(llvm_memmove_i64) },
    { "llvm.memmove.p0.p0.i32", LR_RV_BLOB(llvm_memmove_i32) },
    { "llvm.memmove.p0.p0.i64", LR_RV_BLOB(llvm_memmove_i64) },
    { "llvm.sin.f32", LR_RV_BLOB(llvm_sin_f32) },
    { "llvm.sin.f64", LR_RV_BLOB(llvm_sin_f64) },
    { "llvm.cos.f32", LR_RV_BLOB(llvm_cos_f32) },
    { "llvm.cos.f64", LR_RV_BLOB(llvm_cos_f64) },
    { "llvm.log.f32", LR_RV_BLOB(llvm_log_f32) },
    { "llvm.log.f64", LR_RV_BLOB(llvm_log_f64) },
    { "llvm.log2.f32", LR_RV_BLOB(llvm_log2_f32) },
    { "llvm.log2.f64", LR_RV_BLOB(llvm_log2_f64) },
    { "llvm.log10.f32", LR_RV_BLOB(llvm_log10_f32) },
    { "llvm.log10.f64", LR_RV_BLOB(llvm_log10_f64) },
    { "llvm.exp2.f32", LR_RV_BLOB(llvm_exp2_f32) },
    { "llvm.exp2.f64", LR_RV_BLOB(llvm_exp2_f64) },
    { "llvm.floor.f32", LR_RV_BLOB(llvm_floor_f32) },
    { "llvm.floor.f64", LR_RV_BLOB(llvm_floor_f64) },
    { "llvm.ceil.f32", LR_RV_BLOB(llvm_ceil_f32) },
    { "llvm.ceil.f64", LR_RV_BLOB(llvm_ceil_f64) },
    { "llvm.trunc.f32", LR_RV_BLOB(llvm_trunc_f32) },
    { "llvm.trunc.f64", LR_RV_BLOB(llvm_trunc_f64) },
    { "llvm.round.f32", LR_RV_BLOB(llvm_round_f32) },
    { "llvm.round.f64", LR_RV_BLOB(llvm_round_f64) },
    { "llvm.rint.f32", LR_RV_BLOB(llvm_rint_f32) },
    { "llvm.rint.f64", LR_RV_BLOB(llvm_rint_f64) },
    { "llvm.nearbyint.f32", LR_RV_BLOB(llvm_nearbyint_f32) },
    { "llvm.nearbyint.f64", LR_RV_BLOB(llvm_nearbyint_f64) },
    { "llvm.fma.f32", LR_RV_BLOB(llvm_fma_f32) },
    { "llvm.fma.f64", LR_RV_BLOB(llvm_fma_f64) },
    { "llvm.fmuladd.f32", LR_RV_BLOB(llvm_fma_f32) },
    { "llvm.fmuladd.f64", LR_RV_BLOB(llvm_fma_f64) },
    { "llvm.fmuladd.v2f32", LR_RV_BLOB(llvm_fmuladd_v2f32) },
    { "llvm.fmuladd.v4f32", LR_RV_BLOB(llvm_fmuladd_v4f32) },
    { "llvm.fmuladd.v2f64", LR_RV_BLOB(llvm_fmuladd_v2f64) },
    { "llvm.minnum.f32", LR_RV_BLOB(llvm_minnum_f32) },
    { "llvm.minnum.f64", LR_RV_BLOB(llvm_minnum_f64) },
    { "llvm.maxnum.f32", LR_RV_BLOB(llvm_maxnum_f32) },
    { "llvm.maxnum.f64", LR_RV_BLOB(llvm_maxnum_f64) },
    { "llvm.abs.i8", LR_RV_BLOB(llvm_abs_i8) },
    { "llvm.abs.i16", LR_RV_BLOB(llvm_abs_i16) },
    { "llvm.abs.i32", LR_RV_BLOB(llvm_abs_i32) },
    { "llvm.abs.i64", LR_RV_BLOB(llvm_abs_i64) },
    { "llvm.assume", LR_RV_BLOB(llvm_assume) },
    { "llvm.trap", LR_RV_BLOB(llvm_trap) },
    { "llvm.is.fpclass.f32", LR_RV_BLOB(llvm_is_fpclass_f32) },
    { "llvm.is.fpclass.f64", LR_RV_BLOB(llvm_is_fpclass_f64) },
    { "llvm.exp10.f32", LR_RV_BLOB(llvm_exp10_f32) },
    { "llvm.exp10.f64", LR_RV_BLOB(llvm_exp10_f64) },
};

static const lr_platform_intrinsic_desc_t *
lookup_intrinsic_in_table(const char *name,
                          const lr_platform_intrinsic_desc_t *table,
                          size_t count) {
    size_t i;
    if (!name || !name[0])
        return NULL;
    for (i = 0; i < count; i++) {
        if (strcmp(table[i].name, name) == 0)
            return &table[i];
    }
    return NULL;
}

static bool is_riscv64_target(const char *target_name) {
    return target_name &&
           strncmp(target_name, "riscv64", 7) == 0;
}

bool lr_platform_intrinsic_supported_for_target(const char *name,
                                                 const char *target_name) {
    const char *canonical = normalize_intrinsic_name(name);
    const lr_platform_intrinsic_desc_t *d;
    if (!canonical || !canonical[0])
        return false;

    if (is_riscv64_target(target_name)) {
        d = lookup_intrinsic_in_table(
            canonical, g_riscv64_intrinsics,
            sizeof(g_riscv64_intrinsics) / sizeof(g_riscv64_intrinsics[0]));
        return d && d->blob_begin && d->blob_end && d->blob_end > d->blob_begin;
    }

    /* Non-riscv64 targets: use host blob table */
    return lr_platform_intrinsic_supported(canonical);
}

bool lr_platform_intrinsic_blob_lookup_for_target(const char *name,
                                                   const char *target_name,
                                                   const uint8_t **begin,
                                                   const uint8_t **end) {
    const char *canonical = normalize_intrinsic_name(name);
    const lr_platform_intrinsic_desc_t *d;
    if (!begin || !end)
        return false;
    if (!canonical || !canonical[0])
        return false;

    if (is_riscv64_target(target_name)) {
        d = lookup_intrinsic_in_table(
            canonical, g_riscv64_intrinsics,
            sizeof(g_riscv64_intrinsics) / sizeof(g_riscv64_intrinsics[0]));
        if (!d || !d->blob_begin || !d->blob_end || d->blob_end <= d->blob_begin)
            return false;
        *begin = d->blob_begin;
        *end = d->blob_end;
        return true;
    }

    return lr_platform_intrinsic_blob_lookup(canonical, begin, end);
}
