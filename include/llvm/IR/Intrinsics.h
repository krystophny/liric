#ifndef LLVM_IR_INTRINSICS_H
#define LLVM_IR_INTRINSICS_H

namespace llvm {
namespace Intrinsic {

enum ID {
    not_intrinsic = 0,
    abs,
    copysign,
    cos,
    ctlz,
    ctpop,
    cttz,
    exp,
    exp2,
    fabs,
    floor,
    ceil,
    round,
    trunc,
    fma,
    fmuladd,
    log,
    log2,
    log10,
    maximum,
    maxnum,
    minimum,
    minnum,
    memcpy,
    memmove,
    memset,
    pow,
    powi,
    sin,
    sqrt,
    lifetime_start,
    lifetime_end,
    assume,
    dbg_declare,
    dbg_value,
    trap,
    num_intrinsics,
};

} // namespace Intrinsic
} // namespace llvm

#endif
