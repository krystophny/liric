#include <stdint.h>

extern char __hole_src0_off;
extern char __hole_src1_off;
extern char __hole_dst_off;

__attribute__((used))
void stencil_fadd_f64(uint8_t *stack_base) {
    intptr_t src0_off = (intptr_t)&__hole_src0_off;
    intptr_t src1_off = (intptr_t)&__hole_src1_off;
    intptr_t dst_off = (intptr_t)&__hole_dst_off;
    double a = *(double *)(stack_base + src0_off);
    double b = *(double *)(stack_base + src1_off);
    *(double *)(stack_base + dst_off) = a + b;
}
