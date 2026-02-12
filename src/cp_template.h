#ifndef LIRIC_CP_TEMPLATE_H
#define LIRIC_CP_TEMPLATE_H

#include <stdint.h>
#include <stddef.h>

/*
 * Copy-and-patch template infrastructure.
 *
 * Each template is a snippet of machine code with sentinel values at
 * positions that need patching.  At JIT time we memcpy the template
 * into the code buffer and overwrite the sentinels with actual values
 * (stack offsets, immediates, branch targets, absolute addresses).
 *
 * Sentinel values (little-endian i32 in displacement fields).
 * Must fit in signed 32-bit (max 0x7FFFFFFF) since x86 disp32 is signed.
 * Large positive values chosen so they never collide with real stack
 * offsets (small negative) or typical immediates.
 *
 *   0x11111111 — operand 0 (src0 stack offset, rbp-relative)
 *   0x22222222 — operand 1 (src1 stack offset, rbp-relative)
 *   0x33333333 — destination stack offset, rbp-relative
 *   0x44444444 — i32 immediate / frame size
 */

#define LR_CP_SENTINEL_SRC0  0x11111111u
#define LR_CP_SENTINEL_SRC1  0x22222222u
#define LR_CP_SENTINEL_DEST  0x33333333u
#define LR_CP_SENTINEL_IMM32 0x44444444u

typedef enum lr_cp_patch_kind {
    LR_CP_PATCH_STACK_OFF_I32,  /* 4-byte rbp-relative offset */
    LR_CP_PATCH_IMM_I32,        /* 4-byte immediate */
} lr_cp_patch_kind_t;

typedef struct lr_cp_patch_point {
    uint16_t offset;       /* byte offset within template */
    uint8_t  kind;         /* lr_cp_patch_kind_t */
    uint8_t  operand_idx;  /* 0=src0, 1=src1, 2=dest, 3=imm32 */
} lr_cp_patch_point_t;

#define LR_CP_MAX_PATCHES 4

typedef struct lr_cp_template {
    const uint8_t *code;
    uint16_t code_len;
    uint8_t  num_patches;
    lr_cp_patch_point_t patches[LR_CP_MAX_PATCHES];
} lr_cp_template_t;

/*
 * Scan a template byte range for sentinel values and populate
 * a lr_cp_template_t.  Returns 0 on success, -1 on error
 * (e.g. too many patch points).
 */
static inline int lr_cp_template_init(lr_cp_template_t *t,
                                      const uint8_t *begin,
                                      const uint8_t *end) {
    t->code = begin;
    t->code_len = (uint16_t)(end - begin);
    t->num_patches = 0;

    static const struct {
        uint32_t sentinel;
        uint8_t  kind;
        uint8_t  operand_idx;
    } sentinels[] = {
        { LR_CP_SENTINEL_SRC0,  LR_CP_PATCH_STACK_OFF_I32, 0 },
        { LR_CP_SENTINEL_SRC1,  LR_CP_PATCH_STACK_OFF_I32, 1 },
        { LR_CP_SENTINEL_DEST,  LR_CP_PATCH_STACK_OFF_I32, 2 },
        { LR_CP_SENTINEL_IMM32, LR_CP_PATCH_IMM_I32,       3 },
    };

    for (size_t si = 0; si < sizeof(sentinels) / sizeof(sentinels[0]); si++) {
        uint32_t sv = sentinels[si].sentinel;
        uint8_t sv_bytes[4] = {
            (uint8_t)(sv), (uint8_t)(sv >> 8),
            (uint8_t)(sv >> 16), (uint8_t)(sv >> 24)
        };
        for (uint16_t i = 0; i + 3 < t->code_len; i++) {
            if (begin[i]   == sv_bytes[0] && begin[i+1] == sv_bytes[1] &&
                begin[i+2] == sv_bytes[2] && begin[i+3] == sv_bytes[3]) {
                if (t->num_patches >= LR_CP_MAX_PATCHES)
                    return -1;
                t->patches[t->num_patches].offset = i;
                t->patches[t->num_patches].kind = sentinels[si].kind;
                t->patches[t->num_patches].operand_idx = sentinels[si].operand_idx;
                t->num_patches++;
            }
        }
    }
    return 0;
}

#endif
