#include <stdint.h>

extern char __hole_src0_off;
extern char __hole_src1_off;
extern char __hole_dst_off;

__attribute__((used))
void stencil_sub_i64(uint8_t *stack_base) {
    intptr_t src0_off = (intptr_t)&__hole_src0_off;
    intptr_t src1_off = (intptr_t)&__hole_src1_off;
    intptr_t dst_off = (intptr_t)&__hole_dst_off;
    int64_t a = *(int64_t *)(stack_base + src0_off);
    int64_t b = *(int64_t *)(stack_base + src1_off);
    *(int64_t *)(stack_base + dst_off) = a - b;
}
