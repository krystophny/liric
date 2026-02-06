#ifndef LIRIC_TARGET_AARCH64_H
#define LIRIC_TARGET_AARCH64_H

#include "target.h"

/* AArch64 general-purpose register numbers */
enum {
    A64_X0 = 0, A64_X1 = 1, A64_X2 = 2, A64_X3 = 3,
    A64_X4 = 4, A64_X5 = 5, A64_X6 = 6, A64_X7 = 7,
    A64_X8 = 8, A64_X9 = 9, A64_X10 = 10, A64_X11 = 11,
    A64_X12 = 12, A64_X13 = 13, A64_X14 = 14, A64_X15 = 15,
    A64_X16 = 16, A64_X17 = 17, A64_X18 = 18, A64_X19 = 19,
    A64_X20 = 20, A64_X21 = 21, A64_X22 = 22, A64_X23 = 23,
    A64_X24 = 24, A64_X25 = 25, A64_X26 = 26, A64_X27 = 27,
    A64_X28 = 28, A64_FP = 29, A64_LR = 30, A64_SP = 31,
};

#endif
