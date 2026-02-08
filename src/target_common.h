#ifndef LIRIC_TARGET_COMMON_H
#define LIRIC_TARGET_COMMON_H

#include "target.h"

bool lr_target_alloca_uses_static_storage(const lr_inst_t *inst);
size_t lr_target_alloca_elem_size(const lr_inst_t *inst, size_t min_size);

bool lr_target_inst_has_result_slot(const lr_inst_t *inst);
size_t lr_target_inst_result_slot_size(const lr_inst_t *inst, size_t min_size);

uint8_t lr_target_cc_from_icmp(lr_icmp_pred_t pred);
uint8_t lr_target_cc_from_fcmp(lr_fcmp_pred_t pred);

#endif
