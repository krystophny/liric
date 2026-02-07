#ifndef LIRIC_TARGET_X86_64_H
#define LIRIC_TARGET_X86_64_H

#include "target.h"

/* x86_64 register numbering (matching ModRM encoding) */
enum {
    X86_RAX = 0, X86_RCX = 1, X86_RDX = 2, X86_RBX = 3,
    X86_RSP = 4, X86_RBP = 5, X86_RSI = 6, X86_RDI = 7,
    X86_R8  = 8, X86_R9  = 9, X86_R10 = 10, X86_R11 = 11,
    X86_R12 = 12, X86_R13 = 13, X86_R14 = 14, X86_R15 = 15,
};

/* x86_64 XMM register numbers (separate namespace from GPRs) */
enum {
    X86_XMM0 = 0, X86_XMM1 = 1, X86_XMM2 = 2, X86_XMM3 = 3,
    X86_XMM4 = 4, X86_XMM5 = 5, X86_XMM6 = 6, X86_XMM7 = 7,
};

/* x86_64 condition codes */
enum {
    X86_CC_O = 0, X86_CC_NO = 1,
    X86_CC_B = 2, X86_CC_AE = 3,
    X86_CC_E = 4, X86_CC_NE = 5,
    X86_CC_BE = 6, X86_CC_A = 7,
    X86_CC_S = 8, X86_CC_NS = 9,
    X86_CC_P = 10, X86_CC_NP = 11,
    X86_CC_L = 12, X86_CC_GE = 13,
    X86_CC_LE = 14, X86_CC_G = 15,
};

#endif
