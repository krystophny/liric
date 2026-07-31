/* Public session ABI layout query (issue #527).

   Kept in its own translation unit: src/session.c deliberately declares the
   session entry points with internal types and cannot include the public
   header, but the ABI report must be computed from the public definitions
   that foreign-language bindings actually see. */

#include <liric/liric_session.h>
#include <stddef.h>
#include <string.h>

int lr_session_get_abi_info(lr_session_abi_info_t *out, size_t out_size) {
    lr_session_abi_info_t info;
    if (!out || out_size < sizeof(lr_session_abi_info_t))
        return LR_ERR_ARGUMENT;
    memset(&info, 0, sizeof(info));
    info.abi_version = LIRIC_SESSION_ABI_VERSION;
    info.struct_size = sizeof(lr_session_abi_info_t);
    info.config_size = sizeof(lr_session_config_t);
    info.error_size = sizeof(lr_error_t);
    info.operand_size = sizeof(lr_operand_desc_t);
    info.inst_size = sizeof(lr_inst_desc_t);
    info.config_mode_offset = offsetof(lr_session_config_t, mode);
    info.config_target_offset = offsetof(lr_session_config_t, target);
    info.config_backend_offset = offsetof(lr_session_config_t, backend);
    info.config_opt_level_offset = offsetof(lr_session_config_t, opt_level);
    info.error_code_offset = offsetof(lr_error_t, code);
    info.error_msg_offset = offsetof(lr_error_t, msg);
    info.error_msg_size = sizeof(((lr_error_t *)0)->msg);
    info.operand_kind_offset = offsetof(lr_operand_desc_t, kind);
    info.operand_type_offset = offsetof(lr_operand_desc_t, type);
    info.operand_global_offset_offset =
        offsetof(lr_operand_desc_t, global_offset);
    info.inst_op_offset = offsetof(lr_inst_desc_t, op);
    info.inst_type_offset = offsetof(lr_inst_desc_t, type);
    info.inst_dest_offset = offsetof(lr_inst_desc_t, dest);
    info.inst_operands_offset = offsetof(lr_inst_desc_t, operands);
    info.inst_num_operands_offset = offsetof(lr_inst_desc_t, num_operands);
    info.opcode_count = (uint32_t)LR_OP_COUNT;
    info.operand_kind_count = (uint32_t)LR_OP_KIND_COUNT;
    *out = info;
    return LR_OK;
}
