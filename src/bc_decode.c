#include "bc_decode.h"
#include "frontend_common.h"
#include <liric/liric_session.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Magic detection --------------------------------------------------- */

bool lr_bc_is_bitcode(const uint8_t *data, size_t len) {
    if (!data || len < 4)
        return false;
    if (data[0] == 0x42 && data[1] == 0x43 && data[2] == 0xC0 && data[3] == 0xDE)
        return true;
    if (data[0] == 0xDE && data[1] == 0xC0 && data[2] == 0x17 && data[3] == 0x0B)
        return true;
    return false;
}

bool lr_bc_parser_available(void) {
    return true;
}

/* ---- Layer 1: Bitstream reader ----------------------------------------- */

enum {
    BC_ABBREV_END_BLOCK   = 0,
    BC_ABBREV_ENTER_BLOCK = 1,
    BC_ABBREV_DEFINE      = 2,
    BC_ABBREV_UNABBREV    = 3,
    BC_FIRST_USER_ABBREV  = 4
};

typedef enum {
    BC_OP_LITERAL = 0,
    BC_OP_FIXED   = 1,
    BC_OP_VBR     = 2,
    BC_OP_ARRAY   = 3,
    BC_OP_CHAR6   = 4,
    BC_OP_BLOB    = 5
} bc_abbrev_op_kind_t;

typedef struct {
    bc_abbrev_op_kind_t kind;
    uint64_t value;
} bc_abbrev_op_t;

typedef struct {
    bc_abbrev_op_t *ops;
    uint32_t num_ops;
} bc_abbrev_t;

typedef struct {
    uint32_t block_id;
    bc_abbrev_t *abbrevs;
    uint32_t num;
    uint32_t cap;
} bc_blockinfo_entry_t;

typedef struct {
    const uint8_t *data;
    size_t len_bits;
    size_t bit_pos;

    bc_abbrev_t *abbrevs;
    uint32_t num_abbrevs;
    uint32_t abbrev_cap;
    uint32_t abbrev_len;

    bc_blockinfo_entry_t *blockinfo;
    uint32_t num_blockinfo;
    uint32_t blockinfo_cap;

    uint64_t *record;
    uint32_t record_len;
    uint32_t record_cap;
    const uint8_t *blob_data;
    size_t blob_len;

    char *err;
    size_t errlen;
    bool has_error;
} bc_reader_t;

static void bc_error(bc_reader_t *r, const char *fmt, ...) {
    va_list ap;
    if (!r || r->has_error)
        return;
    r->has_error = true;
    if (!r->err || r->errlen == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(r->err, r->errlen, fmt, ap);
    va_end(ap);
}

static uint64_t bc_read_fixed(bc_reader_t *r, uint32_t width) {
    uint64_t val = 0;
    uint32_t i;
    if (width == 0)
        return 0;
    if (r->bit_pos + width > r->len_bits) {
        bc_error(r, "bitstream overrun at bit %zu", r->bit_pos);
        return 0;
    }
    for (i = 0; i < width; i++) {
        size_t byte_idx = (r->bit_pos + i) / 8;
        uint32_t bit_idx = (uint32_t)((r->bit_pos + i) % 8);
        if ((r->data[byte_idx] >> bit_idx) & 1)
            val |= (uint64_t)1 << i;
    }
    r->bit_pos += width;
    return val;
}

static uint64_t bc_read_vbr(bc_reader_t *r, uint32_t width) {
    uint64_t val = 0;
    uint32_t shift = 0;
    uint64_t hi_bit = (uint64_t)1 << (width - 1);
    for (;;) {
        uint64_t chunk = bc_read_fixed(r, width);
        if (r->has_error)
            return 0;
        val |= (chunk & (hi_bit - 1)) << shift;
        if (!(chunk & hi_bit))
            break;
        shift += width - 1;
        if (shift > 63) {
            bc_error(r, "VBR overflow");
            return 0;
        }
    }
    return val;
}

static void bc_align32(bc_reader_t *r) {
    uint32_t rem = (uint32_t)(r->bit_pos % 32);
    if (rem != 0)
        r->bit_pos += 32 - rem;
}


static void bc_record_push(bc_reader_t *r, uint64_t val) {
    if (r->record_len == r->record_cap) {
        uint32_t new_cap = r->record_cap ? r->record_cap * 2 : 64;
        uint64_t *tmp = (uint64_t *)realloc(r->record, (size_t)new_cap * sizeof(uint64_t));
        if (!tmp) {
            bc_error(r, "out of memory in record buffer");
            return;
        }
        r->record = tmp;
        r->record_cap = new_cap;
    }
    r->record[r->record_len++] = val;
}

static void bc_abbrev_list_push(bc_abbrev_t **list, uint32_t *count,
                                uint32_t *cap, bc_abbrev_t abbrev) {
    if (*count == *cap) {
        uint32_t new_cap = *cap ? *cap * 2 : 16;
        bc_abbrev_t *tmp = (bc_abbrev_t *)realloc(*list, (size_t)new_cap * sizeof(bc_abbrev_t));
        if (!tmp)
            return;
        *list = tmp;
        *cap = new_cap;
    }
    (*list)[*count] = abbrev;
    (*count)++;
}

static void bc_free_abbrev_list(bc_abbrev_t *list, uint32_t count) {
    uint32_t i;
    if (!list)
        return;
    for (i = 0; i < count; i++)
        free(list[i].ops);
    free(list);
}

static bc_blockinfo_entry_t *bc_find_blockinfo(bc_reader_t *r, uint32_t block_id) {
    uint32_t i;
    for (i = 0; i < r->num_blockinfo; i++) {
        if (r->blockinfo[i].block_id == block_id)
            return &r->blockinfo[i];
    }
    return NULL;
}

static bc_blockinfo_entry_t *bc_get_or_create_blockinfo(bc_reader_t *r, uint32_t block_id) {
    bc_blockinfo_entry_t *entry = bc_find_blockinfo(r, block_id);
    if (entry)
        return entry;
    if (r->num_blockinfo == r->blockinfo_cap) {
        uint32_t new_cap = r->blockinfo_cap ? r->blockinfo_cap * 2 : 8;
        bc_blockinfo_entry_t *tmp = (bc_blockinfo_entry_t *)realloc(
            r->blockinfo, (size_t)new_cap * sizeof(bc_blockinfo_entry_t));
        if (!tmp)
            return NULL;
        r->blockinfo = tmp;
        r->blockinfo_cap = new_cap;
    }
    entry = &r->blockinfo[r->num_blockinfo++];
    memset(entry, 0, sizeof(*entry));
    entry->block_id = block_id;
    return entry;
}

static void bc_read_define_abbrev(bc_reader_t *r) {
    uint32_t numops = (uint32_t)bc_read_vbr(r, 5);
    uint32_t i;
    bc_abbrev_t abbrev;
    bc_abbrev_op_t *ops;
    if (r->has_error)
        return;
    ops = (bc_abbrev_op_t *)malloc((size_t)numops * sizeof(bc_abbrev_op_t));
    if (!ops) {
        bc_error(r, "out of memory in define_abbrev");
        return;
    }
    for (i = 0; i < numops && !r->has_error; i++) {
        uint64_t is_literal = bc_read_fixed(r, 1);
        if (is_literal) {
            ops[i].kind = BC_OP_LITERAL;
            ops[i].value = bc_read_vbr(r, 8);
        } else {
            uint64_t encoding = bc_read_fixed(r, 3);
            ops[i].kind = (bc_abbrev_op_kind_t)encoding;
            ops[i].value = 0;
            if (encoding == BC_OP_FIXED || encoding == BC_OP_VBR)
                ops[i].value = bc_read_vbr(r, 5);
        }
    }
    if (r->has_error) {
        free(ops);
        return;
    }
    abbrev.ops = ops;
    abbrev.num_ops = numops;
    bc_abbrev_list_push(&r->abbrevs, &r->num_abbrevs, &r->abbrev_cap, abbrev);
}

static uint32_t bc_read_record(bc_reader_t *r, uint32_t abbrev_id) {
    uint32_t code;
    r->record_len = 0;
    r->blob_data = NULL;
    r->blob_len = 0;

    if (abbrev_id == BC_ABBREV_UNABBREV) {
        uint32_t numops, i;
        code = (uint32_t)bc_read_vbr(r, 6);
        numops = (uint32_t)bc_read_vbr(r, 6);
        for (i = 0; i < numops && !r->has_error; i++)
            bc_record_push(r, bc_read_vbr(r, 6));
        return code;
    }

    {
        uint32_t idx = abbrev_id - BC_FIRST_USER_ABBREV;
        bc_abbrev_t *abbrev;
        uint32_t i;
        if (idx >= r->num_abbrevs) {
            bc_error(r, "invalid abbreviation id %u (have %u)", abbrev_id, r->num_abbrevs);
            return 0;
        }
        abbrev = &r->abbrevs[idx];
        code = 0;
        for (i = 0; i < abbrev->num_ops && !r->has_error; i++) {
            bc_abbrev_op_t *op = &abbrev->ops[i];
            if (op->kind == BC_OP_LITERAL) {
                if (i == 0)
                    code = (uint32_t)op->value;
                else
                    bc_record_push(r, op->value);
            } else if (op->kind == BC_OP_FIXED) {
                uint64_t val = bc_read_fixed(r, (uint32_t)op->value);
                if (i == 0)
                    code = (uint32_t)val;
                else
                    bc_record_push(r, val);
            } else if (op->kind == BC_OP_VBR) {
                uint64_t val = bc_read_vbr(r, (uint32_t)op->value);
                if (i == 0)
                    code = (uint32_t)val;
                else
                    bc_record_push(r, val);
            } else if (op->kind == BC_OP_CHAR6) {
                uint64_t val = bc_read_fixed(r, 6);
                if (i == 0)
                    code = (uint32_t)val;
                else
                    bc_record_push(r, val);
            } else if (op->kind == BC_OP_ARRAY) {
                uint32_t count = (uint32_t)bc_read_vbr(r, 6);
                uint32_t j;
                bc_abbrev_op_t *elem_op;
                if (i + 1 >= abbrev->num_ops) {
                    bc_error(r, "array abbrev missing element encoding");
                    return 0;
                }
                elem_op = &abbrev->ops[i + 1];
                for (j = 0; j < count && !r->has_error; j++) {
                    uint64_t val = 0;
                    if (elem_op->kind == BC_OP_FIXED)
                        val = bc_read_fixed(r, (uint32_t)elem_op->value);
                    else if (elem_op->kind == BC_OP_VBR)
                        val = bc_read_vbr(r, (uint32_t)elem_op->value);
                    else if (elem_op->kind == BC_OP_CHAR6)
                        val = bc_read_fixed(r, 6);
                    else
                        val = bc_read_vbr(r, 6);
                    bc_record_push(r, val);
                }
                i++;
            } else if (op->kind == BC_OP_BLOB) {
                uint32_t blob_len = (uint32_t)bc_read_vbr(r, 6);
                bc_align32(r);
                if (r->bit_pos + (size_t)blob_len * 8 > r->len_bits) {
                    bc_error(r, "blob overrun");
                    return code;
                }
                r->blob_data = r->data + r->bit_pos / 8;
                r->blob_len = blob_len;
                r->bit_pos += (size_t)blob_len * 8;
                bc_align32(r);
            }
        }
        return code;
    }
}

static void bc_skip_block(bc_reader_t *r) {
    (void)bc_read_vbr(r, 4);
    bc_align32(r);
    {
        uint32_t block_words = (uint32_t)bc_read_fixed(r, 32);
        r->bit_pos += (size_t)block_words * 32;
    }
}

static void bc_process_blockinfo_content(bc_reader_t *r, size_t end_pos) {
    uint32_t current_blockinfo_id = 0;
    bool has_id = false;
    uint32_t saved_abbrev_len = r->abbrev_len;
    bc_abbrev_t *saved_abbrevs = r->abbrevs;
    uint32_t saved_num_abbrevs = r->num_abbrevs;
    uint32_t saved_abbrev_cap = r->abbrev_cap;

    r->abbrevs = NULL;
    r->num_abbrevs = 0;
    r->abbrev_cap = 0;
    r->abbrev_len = 2;

    while (r->bit_pos < end_pos && !r->has_error) {
        uint32_t entry = (uint32_t)bc_read_fixed(r, r->abbrev_len);
        if (entry == BC_ABBREV_END_BLOCK) {
            bc_align32(r);
            break;
        }
        if (entry == BC_ABBREV_DEFINE) {
            bc_abbrev_t abbrev;
            uint32_t numops = (uint32_t)bc_read_vbr(r, 5);
            uint32_t i;
            bc_abbrev_op_t *ops = (bc_abbrev_op_t *)malloc(
                (size_t)numops * sizeof(bc_abbrev_op_t));
            if (!ops) {
                bc_error(r, "out of memory in blockinfo define_abbrev");
                break;
            }
            for (i = 0; i < numops && !r->has_error; i++) {
                uint64_t is_literal = bc_read_fixed(r, 1);
                if (is_literal) {
                    ops[i].kind = BC_OP_LITERAL;
                    ops[i].value = bc_read_vbr(r, 8);
                } else {
                    uint64_t encoding = bc_read_fixed(r, 3);
                    ops[i].kind = (bc_abbrev_op_kind_t)encoding;
                    ops[i].value = 0;
                    if (encoding == BC_OP_FIXED || encoding == BC_OP_VBR)
                        ops[i].value = bc_read_vbr(r, 5);
                }
            }
            if (r->has_error) {
                free(ops);
                break;
            }
            abbrev.ops = ops;
            abbrev.num_ops = numops;
            if (has_id) {
                bc_blockinfo_entry_t *bi = bc_get_or_create_blockinfo(r, current_blockinfo_id);
                if (bi)
                    bc_abbrev_list_push(&bi->abbrevs, &bi->num, &bi->cap, abbrev);
                else
                    free(ops);
            }
            continue;
        }
        if (entry == BC_ABBREV_UNABBREV || entry >= BC_FIRST_USER_ABBREV) {
            uint32_t code = bc_read_record(r, entry);
            if (code == 1 && r->record_len > 0) {
                current_blockinfo_id = (uint32_t)r->record[0];
                has_id = true;
            }
            continue;
        }
        bc_error(r, "unexpected entry in blockinfo: %u", entry);
    }

    free(r->abbrevs);
    r->abbrevs = saved_abbrevs;
    r->num_abbrevs = saved_num_abbrevs;
    r->abbrev_cap = saved_abbrev_cap;
    r->abbrev_len = saved_abbrev_len;
}

/* ---- Layer 2: IR decoder ----------------------------------------------- */

enum {
    BC_MODULE_BLOCK       = 8,
    BC_PARAMATTR_BLOCK    = 9,
    BC_PARAMATTR_GRP_BLOCK = 10,
    BC_CONSTANTS_BLOCK    = 11,
    BC_FUNCTION_BLOCK     = 12,
    BC_IDENTIFICATION_BLOCK = 13,
    BC_VALUE_SYMTAB_BLOCK = 14,
    BC_METADATA_BLOCK     = 15,
    BC_METADATA_ATTACH_BLOCK = 16,
    BC_TYPE_BLOCK         = 17,
    BC_OPERAND_BUNDLE_BLOCK = 21,
    BC_METADATA_KIND_BLOCK = 22,
    BC_STRTAB_BLOCK       = 23,
    BC_SYMTAB_BLOCK       = 25
};

enum {
    MODULE_CODE_VERSION      = 1,
    MODULE_CODE_GLOBALVAR    = 7,
    MODULE_CODE_FUNCTION     = 8,
    MODULE_CODE_SOURCE_FILENAME = 16,
    MODULE_CODE_VSTOFFSET    = 18
};

enum {
    TYPE_CODE_NUMENTRY     = 1,
    TYPE_CODE_VOID         = 2,
    TYPE_CODE_FLOAT        = 3,
    TYPE_CODE_DOUBLE       = 4,
    TYPE_CODE_LABEL        = 5,
    TYPE_CODE_INTEGER      = 7,
    TYPE_CODE_POINTER      = 8,
    TYPE_CODE_HALF         = 10,
    TYPE_CODE_ARRAY        = 11,
    TYPE_CODE_VECTOR       = 12,
    TYPE_CODE_FP128        = 14,
    TYPE_CODE_PPC_FP128    = 15,
    TYPE_CODE_X86_FP80     = 13,
    TYPE_CODE_X86_MMX      = 17,
    TYPE_CODE_STRUCT_ANON  = 18,
    TYPE_CODE_STRUCT_NAME  = 19,
    TYPE_CODE_STRUCT_NAMED = 20,
    TYPE_CODE_FUNCTION     = 21,
    TYPE_CODE_TOKEN        = 22,
    TYPE_CODE_BFLOAT       = 23,
    TYPE_CODE_X86_AMX      = 24,
    TYPE_CODE_METADATA     = 25,
    TYPE_CODE_OPAQUE_PTR   = 25,
    TYPE_CODE_TARGET_TYPE  = 26
};

enum {
    CONST_CODE_SETTYPE   = 1,
    CONST_CODE_NULL      = 2,
    CONST_CODE_UNDEF     = 3,
    CONST_CODE_INTEGER   = 4,
    CONST_CODE_FLOAT     = 6,
    CONST_CODE_AGGREGATE = 7,
    CONST_CODE_POISON    = 26
};

enum {
    FUNC_CODE_DECLAREBLOCKS = 1,
    FUNC_CODE_INST_BINOP    = 2,
    FUNC_CODE_INST_CAST     = 3,
    FUNC_CODE_INST_SELECT   = 5,
    FUNC_CODE_INST_EXTRACTELT = 6,
    FUNC_CODE_INST_INSERTELT = 7,
    FUNC_CODE_INST_SHUFFLEVEC = 8,
    FUNC_CODE_INST_RET      = 10,
    FUNC_CODE_INST_BR       = 11,
    FUNC_CODE_INST_SWITCH   = 12,
    FUNC_CODE_INST_UNREACHABLE = 15,
    FUNC_CODE_INST_PHI      = 16,
    FUNC_CODE_INST_ALLOCA   = 19,
    FUNC_CODE_INST_LOAD     = 20,
    FUNC_CODE_INST_STORE_OLD = 24,
    FUNC_CODE_INST_EXTRACTVALUE = 26,
    FUNC_CODE_INST_INSERTVALUE = 27,
    FUNC_CODE_INST_CMP2     = 28,
    FUNC_CODE_INST_VSELECT  = 29,
    FUNC_CODE_INST_CALL     = 34,
    FUNC_CODE_INST_GEP      = 43,
    FUNC_CODE_INST_STORE    = 44,
    FUNC_CODE_INST_UNOP     = 56
};

enum {
    VST_CODE_ENTRY   = 1,
    VST_CODE_BBENTRY = 2,
    VST_CODE_FNENTRY = 3
};

typedef struct {
    lr_type_t **types;
    uint32_t count;
    uint32_t cap;
} bc_type_table_t;

typedef struct {
    enum { BC_VAL_VREG, BC_VAL_CONST, BC_VAL_GLOBAL, BC_VAL_FUNC } kind;
    lr_type_t *type;
    union {
        uint32_t vreg;
        lr_operand_t operand;
        uint32_t global_sym;
        lr_func_t *func;
    };
} bc_value_t;

typedef struct {
    bc_value_t *values;
    uint32_t count;
    uint32_t cap;
} bc_value_table_t;

typedef struct {
    lr_func_t **funcs;
    uint32_t count;
    uint32_t cap;
} bc_func_list_t;

typedef struct {
    bc_reader_t *reader;
    lr_module_t *module;
    lr_arena_t *arena;
    bc_type_table_t types;
    bc_value_table_t global_values;
    bc_func_list_t func_list;
    const uint8_t *strtab_data;
    size_t strtab_len;
    uint32_t bc_version;
    lr_bc_stream_callback_t on_inst;
    void *on_inst_ctx;
    char *err;
    size_t errlen;
} bc_decoder_t;

static void bc_dec_error(bc_decoder_t *d, const char *fmt, ...) {
    va_list ap;
    if (!d || !d->err || d->errlen == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(d->err, d->errlen, fmt, ap);
    va_end(ap);
}

static void bc_type_table_push(bc_type_table_t *t, lr_type_t *ty) {
    if (t->count == t->cap) {
        uint32_t new_cap = t->cap ? t->cap * 2 : 32;
        lr_type_t **tmp = (lr_type_t **)realloc(t->types, (size_t)new_cap * sizeof(lr_type_t *));
        if (!tmp)
            return;
        t->types = tmp;
        t->cap = new_cap;
    }
    t->types[t->count++] = ty;
}

static void bc_value_push(bc_value_table_t *t, bc_value_t val) {
    if (t->count == t->cap) {
        uint32_t new_cap = t->cap ? t->cap * 2 : 64;
        bc_value_t *tmp = (bc_value_t *)realloc(t->values, (size_t)new_cap * sizeof(bc_value_t));
        if (!tmp)
            return;
        t->values = tmp;
        t->cap = new_cap;
    }
    t->values[t->count++] = val;
}

static void bc_func_list_push(bc_func_list_t *fl, lr_func_t *f) {
    if (fl->count == fl->cap) {
        uint32_t new_cap = fl->cap ? fl->cap * 2 : 16;
        lr_func_t **tmp = (lr_func_t **)realloc(fl->funcs, (size_t)new_cap * sizeof(lr_func_t *));
        if (!tmp)
            return;
        fl->funcs = tmp;
        fl->cap = new_cap;
    }
    fl->funcs[fl->count++] = f;
}

static lr_type_t *bc_get_type(bc_decoder_t *d, uint32_t idx) {
    if (idx >= d->types.count) {
        bc_dec_error(d, "type index %u out of range (have %u)", idx, d->types.count);
        return NULL;
    }
    return d->types.types[idx];
}

static lr_operand_t bc_make_operand_from_value(bc_decoder_t *d, bc_value_table_t *vt,
                                                uint32_t val_id,
                                                lr_func_t *func,
                                                lr_type_t *type_hint) {
    lr_operand_t op;
    bc_value_t *v;
    memset(&op, 0, sizeof(op));
    while (val_id >= vt->count) {
        bc_value_t fwd;
        uint32_t before = vt->count;
        fwd.kind = BC_VAL_VREG;
        fwd.type = type_hint ? type_hint : d->module->type_i32;
        fwd.vreg = func ? lr_vreg_new(func) : 0;
        bc_value_push(vt, fwd);
        if (vt->count == before) {
            bc_dec_error(d, "out of memory while extending value table");
            op.kind = LR_VAL_UNDEF;
            op.type = fwd.type;
            return op;
        }
    }
    v = &vt->values[val_id];
    switch (v->kind) {
    case BC_VAL_VREG:
        op = lr_op_vreg(v->vreg, v->type);
        break;
    case BC_VAL_CONST:
        op = v->operand;
        break;
    case BC_VAL_GLOBAL:
        op = lr_op_global(v->global_sym, d->module->type_ptr);
        break;
    case BC_VAL_FUNC:
        {
            uint32_t sym = lr_frontend_intern_symbol(d->module, v->func->name);
            op = lr_op_global(sym, d->module->type_ptr);
        }
        break;
    }
    return op;
}

static int64_t bc_decode_signed_vbr(uint64_t v) {
    if ((v & 1) == 0)
        return (int64_t)(v >> 1);
    if (v != 1)
        return -(int64_t)(v >> 1);
    return INT64_MIN;
}

static bool bc_resolve_rel_value_id(bc_decoder_t *d, uint32_t base_value_id,
                                    uint32_t rel, uint32_t *out_val_id) {
    if (rel > base_value_id) {
        bc_dec_error(d, "invalid relative value id: rel=%u base=%u", rel, base_value_id);
        return false;
    }
    *out_val_id = base_value_id - rel;
    return true;
}

static bool bc_resolve_phi_value_id(bc_decoder_t *d, uint32_t base_before_def,
                                    int64_t rel_signed, uint32_t *out_val_id) {
    if (rel_signed >= 0) {
        uint32_t rel = (uint32_t)rel_signed;
        if (rel > base_before_def) {
            bc_dec_error(d, "invalid PHI relative value id: rel=%u base=%u",
                         rel, base_before_def);
            return false;
        }
        *out_val_id = base_before_def - rel;
        return true;
    }

    {
        if (rel_signed == INT64_MIN) {
            bc_dec_error(d, "unsupported PHI relative value sentinel");
            return false;
        }
        uint64_t fwd = (uint64_t)(-rel_signed);
        uint64_t id = (uint64_t)base_before_def + fwd;
        if (id > UINT32_MAX) {
            bc_dec_error(d, "PHI forward value id overflow");
            return false;
        }
        *out_val_id = (uint32_t)id;
    }
    return true;
}

static bool bc_define_vreg_value(bc_decoder_t *d, bc_value_table_t *vt, lr_func_t *func,
                                 uint32_t value_id, lr_type_t *type, uint32_t *out_vreg) {
    while (value_id >= vt->count) {
        bc_value_t fwd;
        uint32_t before = vt->count;
        fwd.kind = BC_VAL_VREG;
        fwd.type = d->module->type_i32;
        fwd.vreg = lr_vreg_new(func);
        bc_value_push(vt, fwd);
        if (vt->count == before) {
            bc_dec_error(d, "out of memory while materializing value table");
            return false;
        }
    }

    {
        bc_value_t *slot = &vt->values[value_id];
        if (slot->kind != BC_VAL_VREG) {
            bc_dec_error(d, "value id %u is not vreg-definable", value_id);
            return false;
        }
        slot->type = type;
        *out_vreg = slot->vreg;
    }
    return true;
}

static bool bc_define_undef_value(bc_decoder_t *d, bc_value_table_t *vt,
                                  uint32_t value_id, lr_type_t *type) {
    while (value_id >= vt->count) {
        bc_value_t fwd;
        uint32_t before = vt->count;
        fwd.kind = BC_VAL_VREG;
        fwd.type = d->module->type_i32;
        fwd.vreg = 0;
        bc_value_push(vt, fwd);
        if (vt->count == before) {
            bc_dec_error(d, "out of memory while materializing undef value");
            return false;
        }
    }

    {
        bc_value_t *slot = &vt->values[value_id];
        slot->kind = BC_VAL_CONST;
        slot->type = type ? type : d->module->type_i32;
        slot->operand.kind = LR_VAL_UNDEF;
        slot->operand.type = slot->type;
        slot->operand.global_offset = 0;
    }
    return true;
}

static bool bc_call_is_nop_intrinsic(const lr_module_t *m, lr_operand_t callee_op) {
    const char *name;
    if (!m || callee_op.kind != LR_VAL_GLOBAL)
        return false;
    name = lr_module_symbol_name(m, callee_op.global_id);
    if (!name)
        return false;
    return strncmp(name, "llvm.lifetime.start", 19) == 0 ||
           strncmp(name, "llvm.lifetime.end", 17) == 0;
}

/* ---- Type block decoder ------------------------------------------------ */

static bool bc_decode_type_block(bc_decoder_t *d, bc_reader_t *r, size_t end_pos) {
    char *struct_name = NULL;
    size_t struct_name_len = 0;

    while (r->bit_pos < end_pos && !r->has_error) {
        uint32_t entry = (uint32_t)bc_read_fixed(r, r->abbrev_len);

        if (entry == BC_ABBREV_END_BLOCK) {
            bc_align32(r);
            free(struct_name);
            return true;
        }
        if (entry == BC_ABBREV_ENTER_BLOCK) {
            bc_skip_block(r);
            continue;
        }
        if (entry == BC_ABBREV_DEFINE) {
            bc_read_define_abbrev(r);
            continue;
        }

        {
            uint32_t code = bc_read_record(r, entry);
            if (r->has_error)
                break;

            switch (code) {
            case TYPE_CODE_NUMENTRY:
                break;
            case TYPE_CODE_VOID:
                bc_type_table_push(&d->types, d->module->type_void);
                break;
            case TYPE_CODE_FLOAT:
                bc_type_table_push(&d->types, d->module->type_float);
                break;
            case TYPE_CODE_DOUBLE:
                bc_type_table_push(&d->types, d->module->type_double);
                break;
            case TYPE_CODE_HALF:
            case TYPE_CODE_BFLOAT:
                bc_type_table_push(&d->types, d->module->type_float);
                break;
            case TYPE_CODE_FP128:
            case TYPE_CODE_PPC_FP128:
            case TYPE_CODE_X86_FP80:
                bc_type_table_push(&d->types, d->module->type_double);
                break;
            case TYPE_CODE_LABEL:
                bc_type_table_push(&d->types, d->module->type_void);
                break;
            case TYPE_CODE_INTEGER: {
                uint32_t width = r->record_len > 0 ? (uint32_t)r->record[0] : 0;
                lr_type_t *ty = NULL;
                switch (width) {
                case 1:  ty = d->module->type_i1; break;
                case 8:  ty = d->module->type_i8; break;
                case 16: ty = d->module->type_i16; break;
                case 32: ty = d->module->type_i32; break;
                case 64: ty = d->module->type_i64; break;
                default:
                    bc_dec_error(d, "unsupported integer width: i%u", width);
                    free(struct_name);
                    return false;
                }
                bc_type_table_push(&d->types, ty);
                break;
            }
            case TYPE_CODE_POINTER:
                bc_type_table_push(&d->types, d->module->type_ptr);
                break;
            case TYPE_CODE_ARRAY: {
                uint64_t count = r->record_len > 0 ? r->record[0] : 0;
                uint32_t elem_idx = r->record_len > 1 ? (uint32_t)r->record[1] : 0;
                lr_type_t *elem = bc_get_type(d, elem_idx);
                if (!elem) {
                    free(struct_name);
                    return false;
                }
                bc_type_table_push(&d->types, lr_type_array(d->arena, elem, count));
                break;
            }
            case TYPE_CODE_VECTOR: {
                uint64_t count = r->record_len > 0 ? r->record[0] : 0;
                uint32_t elem_idx = r->record_len > 1 ? (uint32_t)r->record[1] : 0;
                lr_type_t *elem = bc_get_type(d, elem_idx);
                if (!elem) {
                    free(struct_name);
                    return false;
                }
                bc_type_table_push(&d->types, lr_type_vector(d->arena, elem, count));
                break;
            }
            case TYPE_CODE_STRUCT_ANON: {
                uint32_t packed = r->record_len > 0 ? (uint32_t)r->record[0] : 0;
                uint32_t nfields = r->record_len > 1 ? r->record_len - 1 : 0;
                lr_type_t **fields = NULL;
                uint32_t i;
                if (nfields > 0) {
                    fields = lr_arena_array(d->arena, lr_type_t *, nfields);
                    for (i = 0; i < nfields; i++) {
                        fields[i] = bc_get_type(d, (uint32_t)r->record[i + 1]);
                        if (!fields[i]) {
                            free(struct_name);
                            return false;
                        }
                    }
                }
                bc_type_table_push(&d->types,
                    lr_type_struct(d->arena, fields, nfields, packed != 0, NULL));
                break;
            }
            case TYPE_CODE_STRUCT_NAME: {
                uint32_t i;
                free(struct_name);
                struct_name_len = r->record_len;
                struct_name = (char *)malloc(struct_name_len + 1);
                if (struct_name) {
                    for (i = 0; i < r->record_len; i++)
                        struct_name[i] = (char)r->record[i];
                    struct_name[struct_name_len] = '\0';
                }
                break;
            }
            case TYPE_CODE_STRUCT_NAMED: {
                uint32_t packed = r->record_len > 0 ? (uint32_t)r->record[0] : 0;
                uint32_t nfields = r->record_len > 1 ? r->record_len - 1 : 0;
                lr_type_t **fields = NULL;
                char *owned_name = NULL;
                uint32_t i;
                if (nfields > 0) {
                    fields = lr_arena_array(d->arena, lr_type_t *, nfields);
                    for (i = 0; i < nfields; i++) {
                        fields[i] = bc_get_type(d, (uint32_t)r->record[i + 1]);
                        if (!fields[i]) {
                            free(struct_name);
                            return false;
                        }
                    }
                }
                if (struct_name)
                    owned_name = lr_arena_strdup(d->arena, struct_name, struct_name_len);
                bc_type_table_push(&d->types,
                    lr_type_struct(d->arena, fields, nfields, packed != 0, owned_name));
                free(struct_name);
                struct_name = NULL;
                struct_name_len = 0;
                break;
            }
            case TYPE_CODE_FUNCTION: {
                uint32_t vararg = r->record_len > 0 ? (uint32_t)r->record[0] : 0;
                uint32_t ret_idx = r->record_len > 1 ? (uint32_t)r->record[1] : 0;
                uint32_t nparams = r->record_len > 2 ? r->record_len - 2 : 0;
                lr_type_t *ret_ty = bc_get_type(d, ret_idx);
                lr_type_t **params = NULL;
                uint32_t i;
                if (!ret_ty) {
                    free(struct_name);
                    return false;
                }
                if (nparams > 0) {
                    params = lr_arena_array(d->arena, lr_type_t *, nparams);
                    for (i = 0; i < nparams; i++) {
                        params[i] = bc_get_type(d, (uint32_t)r->record[i + 2]);
                        if (!params[i]) {
                            free(struct_name);
                            return false;
                        }
                    }
                }
                bc_type_table_push(&d->types,
                    lr_type_func(d->arena, ret_ty, params, nparams, vararg != 0));
                break;
            }
            case TYPE_CODE_METADATA:
            case TYPE_CODE_X86_MMX:
            case TYPE_CODE_X86_AMX:
            case TYPE_CODE_TOKEN:
            case TYPE_CODE_TARGET_TYPE:
                bc_type_table_push(&d->types, d->module->type_void);
                break;
            default:
                bc_type_table_push(&d->types, d->module->type_void);
                break;
            }
        }
    }
    free(struct_name);
    return !r->has_error;
}

/* ---- Constants block decoder ------------------------------------------- */

static bool bc_decode_constants_block(bc_decoder_t *d, bc_reader_t *r,
                                       size_t end_pos, bc_value_table_t *vt) {
    lr_type_t *cur_type = d->module->type_i32;

    while (r->bit_pos < end_pos && !r->has_error) {
        uint32_t entry = (uint32_t)bc_read_fixed(r, r->abbrev_len);

        if (entry == BC_ABBREV_END_BLOCK) {
            bc_align32(r);
            return true;
        }
        if (entry == BC_ABBREV_DEFINE) {
            bc_read_define_abbrev(r);
            continue;
        }
        if (entry == BC_ABBREV_ENTER_BLOCK) {
            bc_skip_block(r);
            continue;
        }

        {
            uint32_t code = bc_read_record(r, entry);
            if (r->has_error)
                return false;

            switch (code) {
            case CONST_CODE_SETTYPE: {
                uint32_t tidx = r->record_len > 0 ? (uint32_t)r->record[0] : 0;
                lr_type_t *ty = bc_get_type(d, tidx);
                if (!ty)
                    return false;
                cur_type = ty;
                break;
            }
            case CONST_CODE_NULL: {
                bc_value_t cv;
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                if (cur_type->kind == LR_TYPE_PTR)
                    cv.operand = lr_op_null(cur_type);
                else
                    cv.operand = lr_op_imm_i64(0, cur_type);
                bc_value_push(vt, cv);
                break;
            }
            case CONST_CODE_UNDEF:
            case CONST_CODE_POISON: {
                bc_value_t cv;
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                cv.operand.kind = LR_VAL_UNDEF;
                cv.operand.type = cur_type;
                cv.operand.global_offset = 0;
                bc_value_push(vt, cv);
                break;
            }
            case CONST_CODE_INTEGER: {
                bc_value_t cv;
                uint64_t raw = r->record_len > 0 ? r->record[0] : 0;
                int64_t val = bc_decode_signed_vbr(raw);
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                cv.operand = lr_op_imm_i64(val, cur_type);
                bc_value_push(vt, cv);
                break;
            }
            case CONST_CODE_FLOAT: {
                bc_value_t cv;
                uint64_t raw = r->record_len > 0 ? r->record[0] : 0;
                double fval;
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                if (cur_type->kind == LR_TYPE_FLOAT) {
                    uint32_t raw32 = (uint32_t)raw;
                    float f32;
                    memcpy(&f32, &raw32, sizeof(f32));
                    fval = (double)f32;
                } else {
                    memcpy(&fval, &raw, sizeof(fval));
                }
                cv.operand = lr_op_imm_f64(fval, cur_type);
                bc_value_push(vt, cv);
                break;
            }
            case CONST_CODE_AGGREGATE: {
                bc_value_t cv;
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                cv.operand.kind = LR_VAL_UNDEF;
                cv.operand.type = cur_type;
                cv.operand.global_offset = 0;
                bc_value_push(vt, cv);
                break;
            }
            default: {
                bc_value_t cv;
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                cv.operand.kind = LR_VAL_UNDEF;
                cv.operand.type = cur_type;
                cv.operand.global_offset = 0;
                bc_value_push(vt, cv);
                break;
            }
            }
        }
    }
    return !r->has_error;
}

/* ---- Value symtab block decoder ---------------------------------------- */

static bool bc_decode_value_symtab(bc_decoder_t *d, bc_reader_t *r, size_t end_pos,
                                    lr_func_t *func, lr_block_t **blocks, uint32_t nblocks) {
    while (r->bit_pos < end_pos && !r->has_error) {
        uint32_t entry = (uint32_t)bc_read_fixed(r, r->abbrev_len);

        if (entry == BC_ABBREV_END_BLOCK) {
            bc_align32(r);
            return true;
        }
        if (entry == BC_ABBREV_DEFINE) {
            bc_read_define_abbrev(r);
            continue;
        }

        {
            uint32_t code = bc_read_record(r, entry);
            if (r->has_error)
                return false;

            if (code == VST_CODE_BBENTRY && func) {
                uint32_t bb_id = r->record_len > 0 ? (uint32_t)r->record[0] : 0;
                if (bb_id < nblocks && blocks[bb_id]) {
                    uint32_t i;
                    size_t namelen = r->record_len > 1 ? r->record_len - 1 : 0;
                    char *name = lr_arena_strdup(d->arena, "", 0);
                    if (namelen > 0) {
                        char *tmp = (char *)malloc(namelen + 1);
                        if (tmp) {
                            for (i = 0; i < (uint32_t)namelen; i++)
                                tmp[i] = (char)r->record[i + 1];
                            tmp[namelen] = '\0';
                            name = lr_arena_strdup(d->arena, tmp, namelen);
                            free(tmp);
                        }
                    }
                    blocks[bb_id]->name = name;
                }
            }
            if (code == VST_CODE_FNENTRY) {
                /* Module-level FNENTRY: record[0] = valueid, record[1] = strtab_offset */
                /* Name is already set via strtab; skip. */
            }
        }
    }
    return !r->has_error;
}

/* ---- Opcode mapping ---------------------------------------------------- */

static lr_opcode_t bc_map_binop(uint32_t opc, bool is_fp) {
    if (is_fp) {
        switch (opc) {
        case 0: return LR_OP_FADD;
        case 1: return LR_OP_FSUB;
        case 2: return LR_OP_FMUL;
        case 3: return LR_OP_FDIV;
        default: return LR_OP_FADD;
        }
    }
    switch (opc) {
    case 0:  return LR_OP_ADD;
    case 1:  return LR_OP_SUB;
    case 2:  return LR_OP_MUL;
    case 3:  return LR_OP_SDIV;
    case 4:  return LR_OP_SDIV;
    case 5:  return LR_OP_SREM;
    case 6:  return LR_OP_SREM;
    case 7:  return LR_OP_SHL;
    case 8:  return LR_OP_LSHR;
    case 9:  return LR_OP_ASHR;
    case 10: return LR_OP_AND;
    case 11: return LR_OP_OR;
    case 12: return LR_OP_XOR;
    default: return LR_OP_ADD;
    }
}

static lr_opcode_t bc_map_cast(uint32_t opc) {
    switch (opc) {
    case 0:  return LR_OP_TRUNC;
    case 1:  return LR_OP_ZEXT;
    case 2:  return LR_OP_SEXT;
    case 3:  return LR_OP_FPTOUI;
    case 4:  return LR_OP_FPTOSI;
    case 5:  return LR_OP_UITOFP;
    case 6:  return LR_OP_SITOFP;
    case 7:  return LR_OP_FPTRUNC;
    case 8:  return LR_OP_FPEXT;
    case 9:  return LR_OP_PTRTOINT;
    case 10: return LR_OP_INTTOPTR;
    case 11: return LR_OP_BITCAST;
    default: return LR_OP_BITCAST;
    }
}

static bool bc_type_is_fp(lr_type_t *t) {
    return t && (t->kind == LR_TYPE_FLOAT || t->kind == LR_TYPE_DOUBLE);
}

static bool bc_map_icmp_pred(uint32_t pred, lr_icmp_pred_t *out) {
    switch (pred) {
    case 0:  *out = LR_ICMP_EQ; return true;
    case 1:  *out = LR_ICMP_NE; return true;
    case 2:  *out = LR_ICMP_UGT; return true;
    case 3:  *out = LR_ICMP_UGE; return true;
    case 4:  *out = LR_ICMP_ULT; return true;
    case 5:  *out = LR_ICMP_ULE; return true;
    case 6:  *out = LR_ICMP_SGT; return true;
    case 7:  *out = LR_ICMP_SGE; return true;
    case 8:  *out = LR_ICMP_SLT; return true;
    case 9:  *out = LR_ICMP_SLE; return true;
    case 32: *out = LR_ICMP_EQ; return true;
    case 33: *out = LR_ICMP_NE; return true;
    case 34: *out = LR_ICMP_UGT; return true;
    case 35: *out = LR_ICMP_UGE; return true;
    case 36: *out = LR_ICMP_ULT; return true;
    case 37: *out = LR_ICMP_ULE; return true;
    case 38: *out = LR_ICMP_SGT; return true;
    case 39: *out = LR_ICMP_SGE; return true;
    case 40: *out = LR_ICMP_SLT; return true;
    case 41: *out = LR_ICMP_SLE; return true;
    default: return false;
    }
}

static bool bc_map_fcmp_pred(uint32_t pred, lr_fcmp_pred_t *out) {
    switch (pred) {
    case 0:  *out = LR_FCMP_FALSE; return true;
    case 1:  *out = LR_FCMP_OEQ; return true;
    case 2:  *out = LR_FCMP_OGT; return true;
    case 3:  *out = LR_FCMP_OGE; return true;
    case 4:  *out = LR_FCMP_OLT; return true;
    case 5:  *out = LR_FCMP_OLE; return true;
    case 6:  *out = LR_FCMP_ONE; return true;
    case 7:  *out = LR_FCMP_ORD; return true;
    case 8:  *out = LR_FCMP_UNO; return true;
    case 9:  *out = LR_FCMP_UEQ; return true;
    case 10: *out = LR_FCMP_UGT; return true;
    case 11: *out = LR_FCMP_UGE; return true;
    case 12: *out = LR_FCMP_ULT; return true;
    case 13: *out = LR_FCMP_ULE; return true;
    case 14: *out = LR_FCMP_UNE; return true;
    case 15: *out = LR_FCMP_TRUE; return true;
    default: return false;
    }
}

static lr_operand_desc_t bc_operand_to_desc(const lr_operand_t *op) {
    lr_operand_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (!op)
        return desc;

    desc.type = op->type;
    desc.global_offset = op->global_offset;
    switch (op->kind) {
    case LR_VAL_VREG:
        desc.kind = LR_OP_KIND_VREG;
        desc.vreg = op->vreg;
        break;
    case LR_VAL_IMM_I64:
        desc.kind = LR_OP_KIND_IMM_I64;
        desc.imm_i64 = op->imm_i64;
        break;
    case LR_VAL_IMM_F64:
        desc.kind = LR_OP_KIND_IMM_F64;
        desc.imm_f64 = op->imm_f64;
        break;
    case LR_VAL_BLOCK:
        desc.kind = LR_OP_KIND_BLOCK;
        desc.block_id = op->block_id;
        break;
    case LR_VAL_GLOBAL:
        desc.kind = LR_OP_KIND_GLOBAL;
        desc.global_id = op->global_id;
        break;
    case LR_VAL_NULL:
        desc.kind = LR_OP_KIND_NULL;
        break;
    case LR_VAL_UNDEF:
        desc.kind = LR_OP_KIND_UNDEF;
        break;
    }
    return desc;
}

static bool bc_emit_inst(bc_decoder_t *d, lr_func_t *func,
                         lr_block_t *block, lr_inst_t *inst) {
    lr_operand_desc_t *op_descs = NULL;
    lr_bc_inst_desc_t desc;
    if (!d || !func || !block || !inst) {
        if (d)
            bc_dec_error(d, "failed to materialize instruction");
        return false;
    }
    if (d->on_inst) {
        uint32_t i;
        memset(&desc, 0, sizeof(desc));
        if (inst->num_operands > 0) {
            op_descs = (lr_operand_desc_t *)malloc(
                (size_t)inst->num_operands * sizeof(lr_operand_desc_t));
            if (!op_descs) {
                bc_dec_error(d, "bitcode streaming callback allocation failed");
                return false;
            }
            for (i = 0; i < inst->num_operands; i++)
                op_descs[i] = bc_operand_to_desc(&inst->operands[i]);
        }

        desc.op = inst->op;
        desc.type = inst->type;
        desc.dest = inst->dest;
        desc.operands = op_descs;
        desc.num_operands = inst->num_operands;
        desc.indices = inst->indices;
        desc.num_indices = inst->num_indices;
        desc.icmp_pred = 0;
        desc.fcmp_pred = 0;
        if (inst->op == LR_OP_ICMP)
            desc.icmp_pred = (int)inst->icmp_pred;
        if (inst->op == LR_OP_FCMP)
            desc.fcmp_pred = (int)inst->fcmp_pred;
        desc.call_external_abi = inst->call_external_abi;
        desc.call_vararg = inst->call_vararg;
        desc.call_fixed_args = inst->call_fixed_args;

        if (d->on_inst(func, block, &desc, d->on_inst_ctx) != 0) {
            free(op_descs);
            bc_dec_error(d, "bitcode streaming callback failed");
            return false;
        }
    }
    free(op_descs);
    lr_block_append(block, inst);
    return true;
}

/* ---- Function block decoder -------------------------------------------- */

static bool bc_decode_function_block(bc_decoder_t *d, bc_reader_t *r,
                                      size_t end_pos, lr_func_t *func) {
    bc_value_table_t local_vt;
    lr_block_t **blocks = NULL;
    uint32_t num_blocks = 0;
    uint32_t cur_block = 0;
    uint32_t next_value_id = 0;
    uint32_t i;
    bool ok = true;

    memset(&local_vt, 0, sizeof(local_vt));

    /* Pre-populate with global values */
    for (i = 0; i < d->global_values.count; i++)
        bc_value_push(&local_vt, d->global_values.values[i]);

    /* Add function parameters */
    for (i = 0; i < func->num_params; i++) {
        bc_value_t pv;
        pv.kind = BC_VAL_VREG;
        pv.type = func->param_types[i];
        pv.vreg = func->param_vregs[i];
        bc_value_push(&local_vt, pv);
    }
    next_value_id = local_vt.count;

    while (r->bit_pos < end_pos && !r->has_error) {
        uint32_t entry = (uint32_t)bc_read_fixed(r, r->abbrev_len);

        if (entry == BC_ABBREV_END_BLOCK) {
            bc_align32(r);
            break;
        }
        if (entry == BC_ABBREV_ENTER_BLOCK) {
            uint32_t block_id = (uint32_t)bc_read_vbr(r, 8);
            uint32_t new_abbrev_len = (uint32_t)bc_read_vbr(r, 4);
            uint32_t saved_abbrev_len, saved_num_abbrevs, saved_abbrev_cap;
            bc_abbrev_t *saved_abbrevs;
            bc_blockinfo_entry_t *bi;
            size_t sub_end;

            bc_align32(r);
            {
                uint32_t block_words = (uint32_t)bc_read_fixed(r, 32);
                sub_end = r->bit_pos + (size_t)block_words * 32;
            }

            saved_abbrev_len = r->abbrev_len;
            saved_num_abbrevs = r->num_abbrevs;
            saved_abbrev_cap = r->abbrev_cap;
            saved_abbrevs = r->abbrevs;

            r->abbrev_len = new_abbrev_len;
            r->abbrevs = NULL;
            r->num_abbrevs = 0;
            r->abbrev_cap = 0;

            bi = bc_find_blockinfo(r, block_id);
            if (bi) {
                uint32_t j;
                for (j = 0; j < bi->num; j++) {
                    bc_abbrev_t copy;
                    copy.num_ops = bi->abbrevs[j].num_ops;
                    copy.ops = (bc_abbrev_op_t *)malloc(
                        (size_t)copy.num_ops * sizeof(bc_abbrev_op_t));
                    if (copy.ops)
                        memcpy(copy.ops, bi->abbrevs[j].ops,
                               (size_t)copy.num_ops * sizeof(bc_abbrev_op_t));
                    bc_abbrev_list_push(&r->abbrevs, &r->num_abbrevs, &r->abbrev_cap, copy);
                }
            }

            if (block_id == BC_CONSTANTS_BLOCK) {
                ok = bc_decode_constants_block(d, r, sub_end, &local_vt);
                next_value_id = local_vt.count;
            } else if (block_id == BC_VALUE_SYMTAB_BLOCK) {
                ok = bc_decode_value_symtab(d, r, sub_end, func, blocks, num_blocks);
            } else {
                r->bit_pos = sub_end;
            }

            bc_free_abbrev_list(r->abbrevs, r->num_abbrevs);
            r->abbrevs = saved_abbrevs;
            r->num_abbrevs = saved_num_abbrevs;
            r->abbrev_cap = saved_abbrev_cap;
            r->abbrev_len = saved_abbrev_len;

            if (!ok)
                break;
            continue;
        }
        if (entry == BC_ABBREV_DEFINE) {
            bc_read_define_abbrev(r);
            continue;
        }

        {
            uint32_t code = bc_read_record(r, entry);
            if (r->has_error)
                break;

            if (code == FUNC_CODE_DECLAREBLOCKS) {
                num_blocks = r->record_len > 0 ? (uint32_t)r->record[0] : 0;
                blocks = (lr_block_t **)calloc(num_blocks, sizeof(lr_block_t *));
                if (!blocks) {
                    bc_dec_error(d, "out of memory allocating block array");
                    ok = false;
                    break;
                }
                for (i = 0; i < num_blocks; i++) {
                    char name[32];
                    snprintf(name, sizeof(name), "bb%u", i);
                    blocks[i] = lr_block_create(func, d->arena, name);
                }
                cur_block = 0;
                continue;
            }

            if (!blocks || cur_block >= num_blocks) {
                bc_dec_error(d, "instruction before DECLAREBLOCKS or block overflow");
                ok = false;
                break;
            }

            switch (code) {
            case FUNC_CODE_INST_RET: {
                lr_inst_t *inst;
                if (r->record_len == 0) {
                    inst = lr_inst_create(d->arena, LR_OP_RET_VOID,
                                          d->module->type_void, 0, NULL, 0);
                } else {
                    uint32_t rel = (uint32_t)r->record[0];
                    uint32_t vid = 0;
                    lr_operand_t op;
                    if (!bc_resolve_rel_value_id(d, next_value_id, rel, &vid)) {
                        ok = false;
                        break;
                    }
                    op = bc_make_operand_from_value(d, &local_vt, vid, func, func->ret_type);
                    inst = lr_inst_create(d->arena, LR_OP_RET, op.type, 0, &op, 1);
                }
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                cur_block++;
                break;
            }
            case FUNC_CODE_INST_BR: {
                if (r->record_len == 1) {
                    uint32_t dest_bb = (uint32_t)r->record[0];
                    lr_operand_t op = lr_op_block(dest_bb);
                    lr_inst_t *inst = lr_inst_create(d->arena, LR_OP_BR,
                                                     d->module->type_void, 0, &op, 1);
                    if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                        ok = false;
                        break;
                    }
                } else if (r->record_len >= 3) {
                    lr_operand_t ops[3];
                    uint32_t bb_true = (uint32_t)r->record[0];
                    uint32_t bb_false = (uint32_t)r->record[1];
                    uint32_t cond_rel = (uint32_t)r->record[2];
                    uint32_t cond_vid = 0;
                    lr_inst_t *inst;
                    if (!bc_resolve_rel_value_id(d, next_value_id, cond_rel, &cond_vid)) {
                        ok = false;
                        break;
                    }
                    ops[0] = bc_make_operand_from_value(d, &local_vt, cond_vid,
                                                        func, d->module->type_i1);
                    ops[1] = lr_op_block(bb_true);
                    ops[2] = lr_op_block(bb_false);
                    inst = lr_inst_create(d->arena, LR_OP_CONDBR,
                                          d->module->type_void, 0, ops, 3);
                    if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                        ok = false;
                        break;
                    }
                }
                cur_block++;
                break;
            }
            case FUNC_CODE_INST_UNREACHABLE: {
                lr_inst_t *inst = lr_inst_create(d->arena, LR_OP_UNREACHABLE,
                                                  d->module->type_void, 0, NULL, 0);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                cur_block++;
                break;
            }
            case FUNC_CODE_INST_BINOP: {
                uint32_t lhs_rel = (uint32_t)r->record[0];
                uint32_t rhs_rel = (uint32_t)r->record[1];
                uint32_t opc = (uint32_t)r->record[2];
                uint32_t lhs_vid = 0;
                uint32_t rhs_vid = 0;
                lr_operand_t ops[2];
                lr_type_t *res_type;
                uint32_t dest;
                lr_inst_t *inst;
                bool is_fp;

                if (!bc_resolve_rel_value_id(d, next_value_id, lhs_rel, &lhs_vid) ||
                    !bc_resolve_rel_value_id(d, next_value_id, rhs_rel, &rhs_vid)) {
                    ok = false;
                    break;
                }
                ops[0] = bc_make_operand_from_value(d, &local_vt, lhs_vid, func, NULL);
                ops[1] = bc_make_operand_from_value(d, &local_vt, rhs_vid, func, ops[0].type);
                res_type = ops[0].type;
                is_fp = bc_type_is_fp(res_type);
                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id, res_type, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;

                inst = lr_inst_create(d->arena, bc_map_binop(opc, is_fp),
                                       res_type, dest, ops, 2);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_CAST: {
                uint32_t src_rel = (uint32_t)r->record[0];
                uint32_t dest_ty_idx = (uint32_t)r->record[1];
                uint32_t cast_opc = (uint32_t)r->record[2];
                uint32_t src_vid = 0;
                lr_operand_t op;
                lr_type_t *dest_type;
                uint32_t dest;
                lr_inst_t *inst;

                if (!bc_resolve_rel_value_id(d, next_value_id, src_rel, &src_vid)) {
                    ok = false;
                    break;
                }
                op = bc_make_operand_from_value(d, &local_vt, src_vid, func, NULL);
                dest_type = bc_get_type(d, dest_ty_idx);
                if (!dest_type) { ok = false; break; }
                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id, dest_type, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;

                inst = lr_inst_create(d->arena, bc_map_cast(cast_opc),
                                       dest_type, dest, &op, 1);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_CMP2: {
                uint32_t lhs_rel = (uint32_t)r->record[0];
                uint32_t rhs_rel = (uint32_t)r->record[1];
                uint32_t pred = (uint32_t)r->record[2];
                uint32_t lhs_vid = 0;
                uint32_t rhs_vid = 0;
                lr_operand_t ops[2];
                uint32_t dest;
                lr_inst_t *inst;

                if (!bc_resolve_rel_value_id(d, next_value_id, lhs_rel, &lhs_vid) ||
                    !bc_resolve_rel_value_id(d, next_value_id, rhs_rel, &rhs_vid)) {
                    ok = false;
                    break;
                }
                ops[0] = bc_make_operand_from_value(d, &local_vt, lhs_vid, func, NULL);
                ops[1] = bc_make_operand_from_value(d, &local_vt, rhs_vid, func, ops[0].type);
                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id,
                                          d->module->type_i1, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;

                if (bc_type_is_fp(ops[0].type)) {
                    lr_fcmp_pred_t fpred;
                    if (!bc_map_fcmp_pred(pred, &fpred)) {
                        bc_dec_error(d, "unsupported fcmp predicate: %u", pred);
                        ok = false;
                        break;
                    }
                    inst = lr_inst_create(d->arena, LR_OP_FCMP,
                                           d->module->type_i1, dest, ops, 2);
                    if (inst)
                        inst->fcmp_pred = fpred;
                } else {
                    lr_icmp_pred_t ipred;
                    lr_fcmp_pred_t fpred;
                    if (bc_map_icmp_pred(pred, &ipred)) {
                        inst = lr_inst_create(d->arena, LR_OP_ICMP,
                                               d->module->type_i1, dest, ops, 2);
                        if (inst)
                            inst->icmp_pred = ipred;
                    } else if (bc_map_fcmp_pred(pred, &fpred)) {
                        inst = lr_inst_create(d->arena, LR_OP_FCMP,
                                               d->module->type_i1, dest, ops, 2);
                        if (inst)
                            inst->fcmp_pred = fpred;
                    } else {
                        bc_dec_error(d, "unsupported icmp predicate: %u", pred);
                        ok = false;
                        break;
                    }
                }
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_PHI: {
                uint32_t ty_idx = (uint32_t)r->record[0];
                lr_type_t *phi_type = bc_get_type(d, ty_idx);
                uint32_t npairs = (r->record_len - 1) / 2;
                lr_operand_t *ops;
                uint32_t dest, j;
                uint32_t phi_base_before_def;
                lr_inst_t *inst;

                if (!phi_type) { ok = false; break; }
                ops = (lr_operand_t *)malloc((size_t)(npairs * 2) * sizeof(lr_operand_t));
                if (!ops) {
                    bc_dec_error(d, "out of memory for phi operands");
                    ok = false;
                    break;
                }
                phi_base_before_def = next_value_id;
                if (!bc_define_vreg_value(d, &local_vt, func, phi_base_before_def,
                                          phi_type, &dest)) {
                    free(ops);
                    ok = false;
                    break;
                }
                next_value_id++;

                for (j = 0; j < npairs; j++) {
                    int64_t val_signed = bc_decode_signed_vbr(r->record[1 + j * 2]);
                    uint32_t bb_id = (uint32_t)r->record[2 + j * 2];
                    uint32_t val_id;
                    if (!bc_resolve_phi_value_id(d, phi_base_before_def,
                                                 val_signed, &val_id)) {
                        free(ops);
                        ok = false;
                        break;
                    }
                    ops[j * 2] = bc_make_operand_from_value(d, &local_vt, val_id,
                                                            func, phi_type);
                    ops[j * 2 + 1] = lr_op_block(bb_id);
                }
                if (!ok)
                    break;

                inst = lr_inst_create(d->arena, LR_OP_PHI, phi_type, dest, ops, npairs * 2);
                free(ops);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_ALLOCA: {
                uint32_t inst_ty_idx, op_ty_idx, align_raw;
                lr_type_t *elem_ty;
                lr_operand_t size_op;
                uint32_t dest;
                lr_inst_t *inst;

                inst_ty_idx = (uint32_t)r->record[0];
                op_ty_idx = r->record_len > 1 ? (uint32_t)r->record[1] : 0;
                (void)op_ty_idx;
                align_raw = r->record_len > 3 ? (uint32_t)r->record[3] : 0;
                (void)align_raw;

                elem_ty = bc_get_type(d, inst_ty_idx);
                if (!elem_ty) { ok = false; break; }

                if (r->record_len > 2) {
                    uint32_t sz_rel = (uint32_t)r->record[2];
                    uint32_t sz_vid = 0;
                    if (!bc_resolve_rel_value_id(d, next_value_id, sz_rel, &sz_vid)) {
                        ok = false;
                        break;
                    }
                    size_op = bc_make_operand_from_value(d, &local_vt, sz_vid,
                                                         func, d->module->type_i64);
                } else {
                    size_op = lr_op_imm_i64(1, d->module->type_i64);
                }

                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id,
                                          d->module->type_ptr, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;

                inst = lr_inst_create(d->arena, LR_OP_ALLOCA,
                                       d->module->type_ptr, dest, &size_op, 1);
                if (inst)
                    inst->type = elem_ty;
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_LOAD: {
                uint32_t ptr_rel = (uint32_t)r->record[0];
                uint32_t ty_idx = (uint32_t)r->record[1];
                uint32_t ptr_vid = 0;
                lr_operand_t op;
                lr_type_t *load_ty;
                uint32_t dest;
                lr_inst_t *inst;

                if (!bc_resolve_rel_value_id(d, next_value_id, ptr_rel, &ptr_vid)) {
                    ok = false;
                    break;
                }
                op = bc_make_operand_from_value(d, &local_vt, ptr_vid, func, d->module->type_ptr);
                load_ty = bc_get_type(d, ty_idx);
                if (!load_ty) { ok = false; break; }
                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id, load_ty, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;

                inst = lr_inst_create(d->arena, LR_OP_LOAD, load_ty, dest, &op, 1);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_STORE:
            case FUNC_CODE_INST_STORE_OLD: {
                lr_operand_t ops[2];
                lr_inst_t *inst;
                if (code == FUNC_CODE_INST_STORE) {
                    uint32_t ptr_rel = (uint32_t)r->record[0];
                    uint32_t val_rel = (uint32_t)r->record[1];
                    uint32_t ptr_vid = 0, val_vid = 0;
                    if (!bc_resolve_rel_value_id(d, next_value_id, ptr_rel, &ptr_vid) ||
                        !bc_resolve_rel_value_id(d, next_value_id, val_rel, &val_vid)) {
                        ok = false;
                        break;
                    }
                    ops[0] = bc_make_operand_from_value(d, &local_vt, val_vid, func, NULL);
                    ops[1] = bc_make_operand_from_value(d, &local_vt, ptr_vid,
                                                        func, d->module->type_ptr);
                } else {
                    uint32_t ptr_rel = (uint32_t)r->record[0];
                    uint32_t val_rel = (uint32_t)r->record[1];
                    uint32_t ptr_vid = 0, val_vid = 0;
                    if (!bc_resolve_rel_value_id(d, next_value_id, ptr_rel, &ptr_vid) ||
                        !bc_resolve_rel_value_id(d, next_value_id, val_rel, &val_vid)) {
                        ok = false;
                        break;
                    }
                    ops[0] = bc_make_operand_from_value(d, &local_vt, val_vid, func, NULL);
                    ops[1] = bc_make_operand_from_value(d, &local_vt, ptr_vid,
                                                        func, d->module->type_ptr);
                }
                if (!ok)
                    break;
                inst = lr_inst_create(d->arena, LR_OP_STORE,
                                       d->module->type_void, 0, ops, 2);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_GEP: {
                uint32_t inbounds = (uint32_t)r->record[0];
                uint32_t src_ty_idx = (uint32_t)r->record[1];
                uint32_t nops_total = (r->record_len - 2);
                uint32_t j;
                lr_type_t *base_ty;
                lr_operand_t *ops;
                uint32_t dest;
                lr_inst_t *inst;

                (void)inbounds;
                base_ty = bc_get_type(d, src_ty_idx);
                if (!base_ty) { ok = false; break; }

                ops = (lr_operand_t *)malloc((size_t)nops_total * sizeof(lr_operand_t));
                if (!ops) {
                    bc_dec_error(d, "out of memory for gep operands");
                    ok = false;
                    break;
                }
                for (j = 0; j < nops_total; j++) {
                    uint32_t rel = (uint32_t)r->record[2 + j];
                    uint32_t vid = 0;
                    if (!bc_resolve_rel_value_id(d, next_value_id, rel, &vid)) {
                        ok = false;
                        break;
                    }
                    ops[j] = bc_make_operand_from_value(d, &local_vt, vid, func, NULL);
                    if (j > 0)
                        ops[j] = lr_canonicalize_gep_index(d->module, blocks[cur_block],
                                                            func, ops[j]);
                }
                if (!ok) {
                    free(ops);
                    break;
                }
                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id,
                                          d->module->type_ptr, &dest)) {
                    free(ops);
                    ok = false;
                    break;
                }
                next_value_id++;

                inst = lr_inst_create(d->arena, LR_OP_GEP,
                                       d->module->type_ptr, dest, ops, nops_total);
                free(ops);
                if (inst)
                    inst->type = base_ty;
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_CALL: {
                uint32_t cc_flags, fn_ty_idx;
                lr_type_t *fn_type;
                uint32_t callee_rel, callee_vid;
                lr_operand_t callee_op;
                uint32_t nargs, j;
                lr_operand_t *ops;
                lr_type_t *ret_type;
                uint32_t dest = 0;
                lr_inst_t *inst;

                (void)r->record[0]; /* attr */
                cc_flags = (uint32_t)r->record[1];
                fn_ty_idx = (uint32_t)r->record[2];
                fn_type = bc_get_type(d, fn_ty_idx);
                if (!fn_type || fn_type->kind != LR_TYPE_FUNC) {
                    bc_dec_error(d, "call references non-function type");
                    ok = false;
                    break;
                }

                callee_rel = (uint32_t)r->record[3];
                if (!bc_resolve_rel_value_id(d, next_value_id, callee_rel, &callee_vid)) {
                    ok = false;
                    break;
                }
                callee_op = bc_make_operand_from_value(d, &local_vt, callee_vid,
                                                       func, d->module->type_ptr);

                if (bc_call_is_nop_intrinsic(d->module, callee_op))
                    break;

                nargs = 0;
                for (j = 4; j < r->record_len; j++) {
                    uint32_t rel = (uint32_t)r->record[j];
                    if (rel > next_value_id)
                        break;
                    nargs++;
                }
                ops = (lr_operand_t *)malloc((size_t)(nargs + 1) * sizeof(lr_operand_t));
                if (!ops) {
                    bc_dec_error(d, "out of memory for call operands");
                    ok = false;
                    break;
                }
                ops[0] = callee_op;
                for (j = 0; j < nargs; j++) {
                    uint32_t rel = (uint32_t)r->record[4 + j];
                    uint32_t vid = 0;
                    if (!bc_resolve_rel_value_id(d, next_value_id, rel, &vid)) {
                        ok = false;
                        break;
                    }
                    ops[j + 1] = bc_make_operand_from_value(d, &local_vt, vid, func, NULL);
                }
                if (!ok) {
                    free(ops);
                    break;
                }

                ret_type = fn_type->func.ret;
                if (ret_type->kind != LR_TYPE_VOID) {
                    if (!bc_define_vreg_value(d, &local_vt, func, next_value_id,
                                              ret_type, &dest)) {
                        free(ops);
                        ok = false;
                        break;
                    }
                    next_value_id++;
                }

                inst = lr_inst_create(d->arena, LR_OP_CALL,
                                       ret_type, dest, ops, nargs + 1);
                free(ops);
                (void)cc_flags;
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_SELECT: {
                uint32_t base_idx = r->record_len >= 4 ? 1u : 0u;
                uint32_t true_rel;
                uint32_t false_rel;
                uint32_t cond_rel;
                uint32_t true_vid = 0;
                uint32_t false_vid = 0;
                uint32_t cond_vid = 0;
                lr_operand_t ops[3];
                lr_type_t *res_type;
                uint32_t dest;
                lr_inst_t *inst;

                if (r->record_len < base_idx + 3) {
                    bc_dec_error(d, "malformed select record");
                    ok = false;
                    break;
                }
                true_rel = (uint32_t)r->record[base_idx + 0];
                false_rel = (uint32_t)r->record[base_idx + 1];
                cond_rel = (uint32_t)r->record[base_idx + 2];

                if (!bc_resolve_rel_value_id(d, next_value_id, true_rel, &true_vid) ||
                    !bc_resolve_rel_value_id(d, next_value_id, false_rel, &false_vid) ||
                    !bc_resolve_rel_value_id(d, next_value_id, cond_rel, &cond_vid)) {
                    ok = false;
                    break;
                }
                ops[0] = bc_make_operand_from_value(d, &local_vt, cond_vid,
                                                    func, d->module->type_i1);
                ops[1] = bc_make_operand_from_value(d, &local_vt, true_vid, func, NULL);
                ops[2] = bc_make_operand_from_value(d, &local_vt, false_vid,
                                                    func, ops[1].type);
                res_type = ops[1].type;
                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id, res_type, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;

                inst = lr_inst_create(d->arena, LR_OP_SELECT,
                                       res_type, dest, ops, 3);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_EXTRACTELT: {
                if (r->record_len >= 2) {
                    uint32_t vec_rel = (uint32_t)r->record[0];
                    uint32_t idx_rel = (uint32_t)r->record[1];
                    uint32_t vec_vid = 0;
                    uint32_t idx_vid = 0;
                    lr_operand_t vec_op;
                    (void)idx_vid;
                    if (!bc_resolve_rel_value_id(d, next_value_id, vec_rel, &vec_vid) ||
                        !bc_resolve_rel_value_id(d, next_value_id, idx_rel, &idx_vid)) {
                        ok = false;
                        break;
                    }
                    vec_op = bc_make_operand_from_value(d, &local_vt, vec_vid, func, NULL);
                    if (!bc_define_undef_value(d, &local_vt, next_value_id,
                                               vec_op.type ? vec_op.type : d->module->type_i32)) {
                        ok = false;
                        break;
                    }
                } else if (!bc_define_undef_value(d, &local_vt, next_value_id,
                                                   d->module->type_i32)) {
                    ok = false;
                    break;
                }
                next_value_id++;
                break;
            }
            case FUNC_CODE_INST_INSERTELT: {
                if (r->record_len >= 3) {
                    uint32_t vec_rel = (uint32_t)r->record[0];
                    uint32_t val_rel = (uint32_t)r->record[1];
                    uint32_t idx_rel = (uint32_t)r->record[2];
                    uint32_t vec_vid = 0, val_vid = 0, idx_vid = 0;
                    lr_operand_t vec_op;
                    (void)val_vid;
                    (void)idx_vid;
                    if (!bc_resolve_rel_value_id(d, next_value_id, vec_rel, &vec_vid) ||
                        !bc_resolve_rel_value_id(d, next_value_id, val_rel, &val_vid) ||
                        !bc_resolve_rel_value_id(d, next_value_id, idx_rel, &idx_vid)) {
                        ok = false;
                        break;
                    }
                    vec_op = bc_make_operand_from_value(d, &local_vt, vec_vid, func, NULL);
                    if (!bc_define_undef_value(d, &local_vt, next_value_id,
                                               vec_op.type ? vec_op.type : d->module->type_i32)) {
                        ok = false;
                        break;
                    }
                } else if (!bc_define_undef_value(d, &local_vt, next_value_id,
                                                   d->module->type_i32)) {
                    ok = false;
                    break;
                }
                next_value_id++;
                break;
            }
            case FUNC_CODE_INST_SHUFFLEVEC: {
                if (r->record_len >= 3) {
                    uint32_t lhs_rel = (uint32_t)r->record[0];
                    uint32_t rhs_rel = (uint32_t)r->record[1];
                    uint32_t mask_rel = (uint32_t)r->record[2];
                    uint32_t lhs_vid = 0, rhs_vid = 0, mask_vid = 0;
                    lr_operand_t lhs_op;
                    (void)rhs_vid;
                    (void)mask_vid;
                    if (!bc_resolve_rel_value_id(d, next_value_id, lhs_rel, &lhs_vid) ||
                        !bc_resolve_rel_value_id(d, next_value_id, rhs_rel, &rhs_vid) ||
                        !bc_resolve_rel_value_id(d, next_value_id, mask_rel, &mask_vid)) {
                        ok = false;
                        break;
                    }
                    lhs_op = bc_make_operand_from_value(d, &local_vt, lhs_vid, func, NULL);
                    if (!bc_define_undef_value(d, &local_vt, next_value_id,
                                               lhs_op.type ? lhs_op.type : d->module->type_i32)) {
                        ok = false;
                        break;
                    }
                } else if (!bc_define_undef_value(d, &local_vt, next_value_id,
                                                   d->module->type_i32)) {
                    ok = false;
                    break;
                }
                next_value_id++;
                break;
            }
            case FUNC_CODE_INST_VSELECT: {
                uint32_t true_rel;
                uint32_t false_rel;
                uint32_t cond_rel;
                uint32_t true_vid = 0;
                uint32_t false_vid = 0;
                uint32_t cond_vid = 0;
                lr_operand_t ops[3];
                lr_type_t *res_type;
                uint32_t dest;
                lr_inst_t *inst;

                if (r->record_len >= 5) {
                    true_rel = (uint32_t)r->record[1];
                    false_rel = (uint32_t)r->record[2];
                    cond_rel = (uint32_t)r->record[4];
                } else if (r->record_len >= 3) {
                    true_rel = (uint32_t)r->record[0];
                    false_rel = (uint32_t)r->record[1];
                    cond_rel = (uint32_t)r->record[2];
                } else {
                    bc_dec_error(d, "malformed vselect record");
                    ok = false;
                    break;
                }

                if (!bc_resolve_rel_value_id(d, next_value_id, true_rel, &true_vid) ||
                    !bc_resolve_rel_value_id(d, next_value_id, false_rel, &false_vid) ||
                    !bc_resolve_rel_value_id(d, next_value_id, cond_rel, &cond_vid)) {
                    ok = false;
                    break;
                }
                ops[0] = bc_make_operand_from_value(d, &local_vt, cond_vid, func, NULL);
                ops[1] = bc_make_operand_from_value(d, &local_vt, true_vid, func, NULL);
                ops[2] = bc_make_operand_from_value(d, &local_vt, false_vid,
                                                    func, ops[1].type);
                res_type = ops[1].type;
                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id, res_type, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;

                inst = lr_inst_create(d->arena, LR_OP_SELECT,
                                       res_type, dest, ops, 3);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_EXTRACTVALUE: {
                uint32_t agg_rel = (uint32_t)r->record[0];
                uint32_t agg_vid = 0;
                lr_operand_t op;
                uint32_t nidx = r->record_len > 1 ? r->record_len - 1 : 0;
                uint32_t *idx_copy = NULL;
                uint32_t dest, j;
                lr_inst_t *inst;

                if (!bc_resolve_rel_value_id(d, next_value_id, agg_rel, &agg_vid)) {
                    ok = false;
                    break;
                }
                op = bc_make_operand_from_value(d, &local_vt, agg_vid, func, NULL);
                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id,
                                          d->module->type_i32, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;

                inst = lr_inst_create(d->arena, LR_OP_EXTRACTVALUE,
                                       d->module->type_i32, dest, &op, 1);
                if (inst && nidx > 0) {
                    idx_copy = lr_arena_array(d->arena, uint32_t, nidx);
                    for (j = 0; j < nidx; j++)
                        idx_copy[j] = (uint32_t)r->record[1 + j];
                    inst->indices = idx_copy;
                    inst->num_indices = nidx;
                }
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_INSERTVALUE: {
                uint32_t agg_rel = (uint32_t)r->record[0];
                uint32_t val_rel = (uint32_t)r->record[1];
                uint32_t agg_vid = 0;
                uint32_t val_vid = 0;
                lr_operand_t ops[2];
                uint32_t nidx = r->record_len > 2 ? r->record_len - 2 : 0;
                uint32_t *idx_copy = NULL;
                uint32_t dest, j;
                lr_inst_t *inst;

                if (!bc_resolve_rel_value_id(d, next_value_id, agg_rel, &agg_vid) ||
                    !bc_resolve_rel_value_id(d, next_value_id, val_rel, &val_vid)) {
                    ok = false;
                    break;
                }
                ops[0] = bc_make_operand_from_value(d, &local_vt, agg_vid, func, NULL);
                ops[1] = bc_make_operand_from_value(d, &local_vt, val_vid,
                                                    func, ops[0].type);
                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id,
                                          ops[0].type, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;

                inst = lr_inst_create(d->arena, LR_OP_INSERTVALUE,
                                       ops[0].type, dest, ops, 2);
                if (inst && nidx > 0) {
                    idx_copy = lr_arena_array(d->arena, uint32_t, nidx);
                    for (j = 0; j < nidx; j++)
                        idx_copy[j] = (uint32_t)r->record[2 + j];
                    inst->indices = idx_copy;
                    inst->num_indices = nidx;
                }
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_UNOP: {
                uint32_t src_rel = (uint32_t)r->record[0];
                uint32_t unopc = r->record_len > 1 ? (uint32_t)r->record[1] : 0;
                uint32_t src_vid = 0;
                lr_operand_t op;
                uint32_t dest;
                lr_inst_t *inst;

                if (!bc_resolve_rel_value_id(d, next_value_id, src_rel, &src_vid)) {
                    ok = false;
                    break;
                }
                op = bc_make_operand_from_value(d, &local_vt, src_vid, func, NULL);
                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id,
                                          op.type, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;

                (void)unopc;
                inst = lr_inst_create(d->arena, LR_OP_FNEG,
                                       op.type, dest, &op, 1);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_SWITCH: {
                uint32_t ty_idx = (uint32_t)r->record[0];
                uint32_t cond_rel = (uint32_t)r->record[1];
                uint32_t default_bb = (uint32_t)r->record[2];
                uint32_t cond_vid = 0;
                lr_operand_t cond_op;
                (void)ty_idx;
                if (!bc_resolve_rel_value_id(d, next_value_id, cond_rel, &cond_vid)) {
                    ok = false;
                    break;
                }
                cond_op = bc_make_operand_from_value(d, &local_vt, cond_vid, func, NULL);
                (void)cond_op;

                {
                    lr_operand_t op = lr_op_block(default_bb);
                    lr_inst_t *inst = lr_inst_create(d->arena, LR_OP_BR,
                                                     d->module->type_void, 0, &op, 1);
                    if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                        ok = false;
                        break;
                    }
                }
                cur_block++;
                break;
            }
            default:
                break;
            }

            if (!ok)
                break;
        }
    }

    free(blocks);
    free(local_vt.values);
    return ok && !r->has_error;
}

/* ---- Module block decoder ---------------------------------------------- */

static bool bc_decode_module_block(bc_decoder_t *d, bc_reader_t *r, size_t end_pos) {
    uint32_t func_body_idx = 0;
    bool ok = true;

    while (r->bit_pos < end_pos && !r->has_error) {
        uint32_t entry = (uint32_t)bc_read_fixed(r, r->abbrev_len);

        if (entry == BC_ABBREV_END_BLOCK) {
            bc_align32(r);
            return true;
        }
        if (entry == BC_ABBREV_ENTER_BLOCK) {
            uint32_t block_id = (uint32_t)bc_read_vbr(r, 8);
            uint32_t new_abbrev_len = (uint32_t)bc_read_vbr(r, 4);
            uint32_t saved_abbrev_len, saved_num_abbrevs, saved_abbrev_cap;
            bc_abbrev_t *saved_abbrevs;
            bc_blockinfo_entry_t *bi;
            size_t sub_end;

            bc_align32(r);
            {
                uint32_t block_words = (uint32_t)bc_read_fixed(r, 32);
                sub_end = r->bit_pos + (size_t)block_words * 32;
            }

            if (block_id == 0) {
                (void)new_abbrev_len;
                bc_process_blockinfo_content(r, sub_end);
                continue;
            }

            saved_abbrev_len = r->abbrev_len;
            saved_num_abbrevs = r->num_abbrevs;
            saved_abbrev_cap = r->abbrev_cap;
            saved_abbrevs = r->abbrevs;

            r->abbrev_len = new_abbrev_len;
            r->abbrevs = NULL;
            r->num_abbrevs = 0;
            r->abbrev_cap = 0;

            bi = bc_find_blockinfo(r, block_id);
            if (bi) {
                uint32_t j;
                for (j = 0; j < bi->num; j++) {
                    bc_abbrev_t copy;
                    copy.num_ops = bi->abbrevs[j].num_ops;
                    copy.ops = (bc_abbrev_op_t *)malloc(
                        (size_t)copy.num_ops * sizeof(bc_abbrev_op_t));
                    if (copy.ops)
                        memcpy(copy.ops, bi->abbrevs[j].ops,
                               (size_t)copy.num_ops * sizeof(bc_abbrev_op_t));
                    bc_abbrev_list_push(&r->abbrevs, &r->num_abbrevs, &r->abbrev_cap, copy);
                }
            }

            if (block_id == BC_TYPE_BLOCK) {
                ok = bc_decode_type_block(d, r, sub_end);
            } else if (block_id == BC_CONSTANTS_BLOCK) {
                ok = bc_decode_constants_block(d, r, sub_end, &d->global_values);
            } else if (block_id == BC_FUNCTION_BLOCK) {
                lr_func_t *target = NULL;
                while (func_body_idx < d->func_list.count) {
                    if (!d->func_list.funcs[func_body_idx]->is_decl) {
                        target = d->func_list.funcs[func_body_idx];
                        func_body_idx++;
                        break;
                    }
                    func_body_idx++;
                }
                if (target) {
                    ok = bc_decode_function_block(d, r, sub_end, target);
                    if (!ok && d->on_inst == NULL) {
                        /* Keep parsing subsequent functions even if one body
                           uses unsupported records or value encodings. */
                        target->is_decl = true;
                        target->first_block = NULL;
                        target->last_block = NULL;
                        target->num_blocks = 0;
                        target->block_array = NULL;
                        target->linear_inst_array = NULL;
                        target->block_inst_offsets = NULL;
                        target->num_linear_insts = 0;
                        r->bit_pos = sub_end;
                        r->has_error = false;
                        ok = true;
                    }
                } else
                    r->bit_pos = sub_end;
            } else if (block_id == BC_VALUE_SYMTAB_BLOCK) {
                ok = bc_decode_value_symtab(d, r, sub_end, NULL, NULL, 0);
            } else {
                r->bit_pos = sub_end;
            }

            bc_free_abbrev_list(r->abbrevs, r->num_abbrevs);
            r->abbrevs = saved_abbrevs;
            r->num_abbrevs = saved_num_abbrevs;
            r->abbrev_cap = saved_abbrev_cap;
            r->abbrev_len = saved_abbrev_len;

            if (!ok)
                return false;
            continue;
        }
        if (entry == BC_ABBREV_DEFINE) {
            bc_read_define_abbrev(r);
            continue;
        }

        {
            uint32_t code = bc_read_record(r, entry);
            if (r->has_error)
                return false;

            switch (code) {
            case MODULE_CODE_VERSION:
                d->bc_version = r->record_len > 0 ? (uint32_t)r->record[0] : 0;
                break;
            case MODULE_CODE_FUNCTION: {
                uint32_t strtab_off = 0, strtab_size = 0;
                uint32_t type_idx, is_proto;
                lr_type_t *fn_type;
                char *name = NULL;
                lr_type_t *ret_ty;
                lr_type_t **params = NULL;
                uint32_t nparams;
                bool is_decl, vararg;
                lr_func_t *fn;
                bc_value_t fv;

                if (d->bc_version >= 2 && r->record_len >= 2) {
                    strtab_off = (uint32_t)r->record[0];
                    strtab_size = (uint32_t)r->record[1];
                    type_idx = r->record_len > 2 ? (uint32_t)r->record[2] : 0;
                    is_proto = r->record_len > 4 ? (uint32_t)r->record[4] : 0;
                } else {
                    type_idx = r->record_len > 0 ? (uint32_t)r->record[0] : 0;
                    is_proto = r->record_len > 2 ? (uint32_t)r->record[2] : 0;
                }

                fn_type = bc_get_type(d, type_idx);
                if (!fn_type || fn_type->kind != LR_TYPE_FUNC) {
                    bc_dec_error(d, "MODULE_CODE_FUNCTION references non-function type %u", type_idx);
                    return false;
                }

                if (d->bc_version >= 2 && d->strtab_data && strtab_size > 0 &&
                    (size_t)strtab_off + strtab_size <= d->strtab_len) {
                    name = lr_arena_strdup(d->arena, (const char *)d->strtab_data + strtab_off,
                                           strtab_size);
                }
                if (!name)
                    name = lr_arena_strdup(d->arena, "unknown", 7);

                ret_ty = fn_type->func.ret;
                nparams = fn_type->func.num_params;
                vararg = fn_type->func.vararg;
                if (nparams > 0) {
                    params = lr_arena_array(d->arena, lr_type_t *, nparams);
                    memcpy(params, fn_type->func.params, (size_t)nparams * sizeof(lr_type_t *));
                }
                is_decl = (is_proto != 0);

                fn = lr_frontend_create_function(d->module, name, ret_ty, params,
                                                  nparams, vararg, is_decl, NULL);
                if (!fn) {
                    bc_dec_error(d, "failed to create function '%s'", name);
                    return false;
                }

                fv.kind = BC_VAL_FUNC;
                fv.type = d->module->type_ptr;
                fv.func = fn;
                bc_value_push(&d->global_values, fv);
                bc_func_list_push(&d->func_list, fn);
                break;
            }
            case MODULE_CODE_GLOBALVAR: {
                uint32_t strtab_off = 0, strtab_size = 0;
                uint32_t type_idx;
                uint32_t isconst_plus1, linkage;
                lr_type_t *gtype;
                char *gname = NULL;
                lr_global_t *g;
                bc_value_t gv;
                bool is_const, is_external;

                if (d->bc_version >= 2 && r->record_len >= 2) {
                    strtab_off = (uint32_t)r->record[0];
                    strtab_size = (uint32_t)r->record[1];
                    type_idx = r->record_len > 2 ? (uint32_t)r->record[2] : 0;
                    isconst_plus1 = r->record_len > 4 ? (uint32_t)r->record[4] : 0;
                    linkage = r->record_len > 6 ? (uint32_t)r->record[6] : 0;
                } else {
                    type_idx = r->record_len > 0 ? (uint32_t)r->record[0] : 0;
                    isconst_plus1 = r->record_len > 2 ? (uint32_t)r->record[2] : 0;
                    linkage = r->record_len > 4 ? (uint32_t)r->record[4] : 0;
                }

                gtype = bc_get_type(d, type_idx);
                if (!gtype)
                    gtype = d->module->type_i8;

                if (d->bc_version >= 2 && d->strtab_data && strtab_size > 0 &&
                    (size_t)strtab_off + strtab_size <= d->strtab_len) {
                    gname = lr_arena_strdup(d->arena, (const char *)d->strtab_data + strtab_off,
                                             strtab_size);
                }
                if (!gname)
                    gname = lr_arena_strdup(d->arena, "global", 6);

                is_const = (isconst_plus1 > 1);
                is_external = (linkage == 0 && isconst_plus1 == 0);

                g = lr_global_create(d->module, gname, gtype, is_const);
                if (g)
                    g->is_external = is_external;

                lr_frontend_intern_symbol(d->module, gname);

                gv.kind = BC_VAL_GLOBAL;
                gv.type = d->module->type_ptr;
                gv.global_sym = lr_frontend_intern_symbol(d->module, gname);
                bc_value_push(&d->global_values, gv);
                break;
            }
            case MODULE_CODE_SOURCE_FILENAME:
            case MODULE_CODE_VSTOFFSET:
                break;
            default:
                break;
            }
        }
    }
    return ok && !r->has_error;
}

/* ---- Top-level parser -------------------------------------------------- */

static void bc_scan_strtab(bc_decoder_t *d, bc_reader_t *r) {
    size_t saved_pos = r->bit_pos;
    uint32_t saved_abbrev_len = r->abbrev_len;
    bc_abbrev_t *saved_abbrevs = r->abbrevs;
    uint32_t saved_num_abbrevs = r->num_abbrevs;
    uint32_t saved_abbrev_cap = r->abbrev_cap;

    r->bit_pos = 0;

    while (r->bit_pos < r->len_bits && !r->has_error) {
        uint32_t entry = (uint32_t)bc_read_fixed(r, 2);
        if (entry == BC_ABBREV_ENTER_BLOCK) {
            uint32_t block_id = (uint32_t)bc_read_vbr(r, 8);
            uint32_t new_abbrev_len = (uint32_t)bc_read_vbr(r, 4);
            bc_align32(r);
            {
                uint32_t block_words = (uint32_t)bc_read_fixed(r, 32);
                size_t sub_end = r->bit_pos + (size_t)block_words * 32;

                if (block_id == BC_STRTAB_BLOCK) {
                    r->abbrev_len = new_abbrev_len;
                    r->abbrevs = NULL;
                    r->num_abbrevs = 0;
                    r->abbrev_cap = 0;

                    while (r->bit_pos < sub_end && !r->has_error) {
                        uint32_t e = (uint32_t)bc_read_fixed(r, r->abbrev_len);
                        if (e == BC_ABBREV_END_BLOCK) {
                            bc_align32(r);
                            break;
                        }
                        if (e == BC_ABBREV_DEFINE) {
                            bc_read_define_abbrev(r);
                            continue;
                        }
                        (void)bc_read_record(r, e);
                        if (r->blob_data && r->blob_len > 0) {
                            d->strtab_data = r->blob_data;
                            d->strtab_len = r->blob_len;
                        }
                    }

                    bc_free_abbrev_list(r->abbrevs, r->num_abbrevs);
                    break;
                }
                r->bit_pos = sub_end;
            }
        } else {
            break;
        }
    }

    r->bit_pos = saved_pos;
    r->abbrev_len = saved_abbrev_len;
    r->abbrevs = saved_abbrevs;
    r->num_abbrevs = saved_num_abbrevs;
    r->abbrev_cap = saved_abbrev_cap;
    r->has_error = false;
}

lr_module_t *lr_parse_bc_streaming(const uint8_t *data, size_t len,
                                   lr_arena_t *arena,
                                   lr_bc_stream_callback_t on_inst, void *ctx,
                                   char *err, size_t errlen) {
    bc_reader_t reader;
    bc_decoder_t decoder;
    const uint8_t *bc_data = data;
    size_t bc_len = len;

    if (!data && len != 0) {
        lr_frontend_set_error(err, errlen, "invalid bitcode input buffer");
        return NULL;
    }
    if (!arena) {
        lr_frontend_set_error(err, errlen, "arena is required for bitcode parse");
        return NULL;
    }
    if (!lr_bc_is_bitcode(data, len)) {
        lr_frontend_set_error(err, errlen, "input is not LLVM bitcode");
        return NULL;
    }

    /* Handle wrapper format */
    if (len >= 20 && data[0] == 0xDE && data[1] == 0xC0 && data[2] == 0x17 && data[3] == 0x0B) {
        uint32_t bc_offset, bc_size;
        memcpy(&bc_offset, data + 8, 4);
        memcpy(&bc_size, data + 12, 4);
        if ((size_t)bc_offset + bc_size <= len) {
            bc_data = data + bc_offset;
            bc_len = bc_size;
        }
    }

    /* Skip raw bitcode magic "BC\xC0\xDE" */
    if (bc_len >= 4 && bc_data[0] == 0x42 && bc_data[1] == 0x43 &&
        bc_data[2] == 0xC0 && bc_data[3] == 0xDE) {
        bc_data += 4;
        bc_len -= 4;
    }

    memset(&reader, 0, sizeof(reader));
    reader.data = bc_data;
    reader.len_bits = bc_len * 8;
    reader.bit_pos = 0;
    reader.abbrev_len = 2;
    reader.err = err;
    reader.errlen = errlen;
    if (err && errlen > 0)
        err[0] = '\0';

    memset(&decoder, 0, sizeof(decoder));
    decoder.reader = &reader;
    decoder.arena = arena;
    decoder.on_inst = on_inst;
    decoder.on_inst_ctx = ctx;
    decoder.err = err;
    decoder.errlen = errlen;

    decoder.module = lr_module_create(arena);
    if (!decoder.module) {
        lr_frontend_set_error(err, errlen, "failed to allocate liric module");
        return NULL;
    }

    /* First pass: scan for STRTAB block at top level */
    bc_scan_strtab(&decoder, &reader);

    /* Second pass: process all top-level blocks */
    while (reader.bit_pos < reader.len_bits && !reader.has_error) {
        uint32_t entry = (uint32_t)bc_read_fixed(&reader, 2);

        if (entry == BC_ABBREV_ENTER_BLOCK) {
            uint32_t block_id = (uint32_t)bc_read_vbr(&reader, 8);
            uint32_t new_abbrev_len = (uint32_t)bc_read_vbr(&reader, 4);
            uint32_t saved_abbrev_len, saved_num_abbrevs, saved_abbrev_cap;
            bc_abbrev_t *saved_abbrevs;
            bc_blockinfo_entry_t *bi;
            size_t sub_end;

            bc_align32(&reader);
            {
                uint32_t block_words = (uint32_t)bc_read_fixed(&reader, 32);
                sub_end = reader.bit_pos + (size_t)block_words * 32;
            }

            if (block_id == 0) {
                (void)new_abbrev_len;
                bc_process_blockinfo_content(&reader, sub_end);
                continue;
            }

            saved_abbrev_len = reader.abbrev_len;
            saved_num_abbrevs = reader.num_abbrevs;
            saved_abbrev_cap = reader.abbrev_cap;
            saved_abbrevs = reader.abbrevs;

            reader.abbrev_len = new_abbrev_len;
            reader.abbrevs = NULL;
            reader.num_abbrevs = 0;
            reader.abbrev_cap = 0;

            bi = bc_find_blockinfo(&reader, block_id);
            if (bi) {
                uint32_t j;
                for (j = 0; j < bi->num; j++) {
                    bc_abbrev_t copy;
                    copy.num_ops = bi->abbrevs[j].num_ops;
                    copy.ops = (bc_abbrev_op_t *)malloc(
                        (size_t)copy.num_ops * sizeof(bc_abbrev_op_t));
                    if (copy.ops)
                        memcpy(copy.ops, bi->abbrevs[j].ops,
                               (size_t)copy.num_ops * sizeof(bc_abbrev_op_t));
                    bc_abbrev_list_push(&reader.abbrevs, &reader.num_abbrevs,
                                        &reader.abbrev_cap, copy);
                }
            }

            if (block_id == BC_MODULE_BLOCK) {
                if (!bc_decode_module_block(&decoder, &reader, sub_end)) {
                    bc_free_abbrev_list(reader.abbrevs, reader.num_abbrevs);
                    reader.abbrevs = saved_abbrevs;
                    reader.num_abbrevs = saved_num_abbrevs;
                    reader.abbrev_cap = saved_abbrev_cap;
                    reader.abbrev_len = saved_abbrev_len;
                    goto cleanup_fail;
                }
            } else {
                reader.bit_pos = sub_end;
            }

            bc_free_abbrev_list(reader.abbrevs, reader.num_abbrevs);
            reader.abbrevs = saved_abbrevs;
            reader.num_abbrevs = saved_num_abbrevs;
            reader.abbrev_cap = saved_abbrev_cap;
            reader.abbrev_len = saved_abbrev_len;
            continue;
        }

        if (entry == BC_ABBREV_END_BLOCK) {
            bc_align32(&reader);
            continue;
        }

        /* Top-level non-block entry: skip. At top level abbrev_len is 2, so
           only enter_block (1) is expected. End_block (0) or define (2) or
           unabbrev (3) are unusual but handle gracefully. */
        if (entry == BC_ABBREV_DEFINE) {
            bc_read_define_abbrev(&reader);
            continue;
        }
        if (entry == BC_ABBREV_UNABBREV) {
            (void)bc_read_record(&reader, entry);
            continue;
        }
        break;
    }

    if (reader.has_error)
        goto cleanup_fail;

    /* Success */
    free(reader.record);
    bc_free_abbrev_list(reader.abbrevs, reader.num_abbrevs);
    {
        uint32_t i;
        for (i = 0; i < reader.num_blockinfo; i++)
            bc_free_abbrev_list(reader.blockinfo[i].abbrevs, reader.blockinfo[i].num);
    }
    free(reader.blockinfo);
    free(decoder.types.types);
    free(decoder.global_values.values);
    free(decoder.func_list.funcs);
    return decoder.module;

cleanup_fail:
    free(reader.record);
    bc_free_abbrev_list(reader.abbrevs, reader.num_abbrevs);
    {
        uint32_t i;
        for (i = 0; i < reader.num_blockinfo; i++)
            bc_free_abbrev_list(reader.blockinfo[i].abbrevs, reader.blockinfo[i].num);
    }
    free(reader.blockinfo);
    free(decoder.types.types);
    free(decoder.global_values.values);
    free(decoder.func_list.funcs);
    if (err && errlen > 0 && err[0] == '\0')
        lr_frontend_set_error(err, errlen, "failed to parse LLVM bitcode");
    return NULL;
}

lr_module_t *lr_parse_bc_with_arena(const uint8_t *data, size_t len,
                                    lr_arena_t *arena, char *err, size_t errlen) {
    return lr_parse_bc_streaming(data, len, arena, NULL, NULL, err, errlen);
}

/* ---- Session streaming: parse BC then replay through session API ------- */

static lr_type_t *bc_map_type_to_session(lr_session_t *session,
                                          const lr_type_t *src_type) {
    if (!session || !src_type)
        return NULL;
    switch (src_type->kind) {
    case LR_TYPE_VOID:   return lr_type_void_s(session);
    case LR_TYPE_I1:     return lr_type_i1_s(session);
    case LR_TYPE_I8:     return lr_type_i8_s(session);
    case LR_TYPE_I16:    return lr_type_i16_s(session);
    case LR_TYPE_I32:    return lr_type_i32_s(session);
    case LR_TYPE_I64:    return lr_type_i64_s(session);
    case LR_TYPE_FLOAT:  return lr_type_f32_s(session);
    case LR_TYPE_DOUBLE: return lr_type_f64_s(session);
    case LR_TYPE_PTR:    return lr_type_ptr_s(session);
    case LR_TYPE_ARRAY:
        return lr_type_array_s(session,
                               bc_map_type_to_session(session, src_type->array.elem),
                               src_type->array.count);
    case LR_TYPE_VECTOR:
        return lr_type_vector_s(session,
                                bc_map_type_to_session(session, src_type->array.elem),
                                src_type->array.count);
    case LR_TYPE_STRUCT: {
        lr_type_t **fields = NULL;
        uint32_t i;
        if (src_type->struc.num_fields > 0) {
            fields = (lr_type_t **)calloc(src_type->struc.num_fields,
                                           sizeof(*fields));
            if (!fields)
                return NULL;
            for (i = 0; i < src_type->struc.num_fields; i++) {
                fields[i] = bc_map_type_to_session(session,
                                                    src_type->struc.fields[i]);
                if (!fields[i]) {
                    free(fields);
                    return NULL;
                }
            }
        }
        {
            lr_type_t *result = lr_type_struct_s(session, fields,
                                                  src_type->struc.num_fields,
                                                  src_type->struc.packed);
            free(fields);
            return result;
        }
    }
    case LR_TYPE_FUNC: {
        lr_type_t *ret = bc_map_type_to_session(session, src_type->func.ret);
        lr_type_t **params = NULL;
        uint32_t i;
        if (!ret)
            return NULL;
        if (src_type->func.num_params > 0) {
            params = (lr_type_t **)calloc(src_type->func.num_params,
                                           sizeof(*params));
            if (!params)
                return NULL;
            for (i = 0; i < src_type->func.num_params; i++) {
                params[i] = bc_map_type_to_session(session,
                                                    src_type->func.params[i]);
                if (!params[i]) {
                    free(params);
                    return NULL;
                }
            }
        }
        {
            lr_type_t *result = lr_type_function_s(session, ret, params,
                                                    src_type->func.num_params,
                                                    src_type->func.vararg);
            free(params);
            return result;
        }
    }
    default:
        return NULL;
    }
}

static lr_operand_desc_t bc_map_operand_to_session(const lr_operand_t *src_op,
                                                    lr_session_t *session,
                                                    const lr_module_t *src_mod) {
    lr_operand_desc_t out;
    memset(&out, 0, sizeof(out));
    if (!src_op || !session)
        return out;
    out.type = bc_map_type_to_session(session, src_op->type);
    out.global_offset = src_op->global_offset;
    switch (src_op->kind) {
    case LR_VAL_VREG:
        out.kind = LR_OP_KIND_VREG;
        out.vreg = src_op->vreg;
        break;
    case LR_VAL_IMM_I64:
        out.kind = LR_OP_KIND_IMM_I64;
        out.imm_i64 = src_op->imm_i64;
        break;
    case LR_VAL_IMM_F64:
        out.kind = LR_OP_KIND_IMM_F64;
        out.imm_f64 = src_op->imm_f64;
        break;
    case LR_VAL_BLOCK:
        out.kind = LR_OP_KIND_BLOCK;
        out.block_id = src_op->block_id;
        break;
    case LR_VAL_GLOBAL: {
        const char *sym_name = lr_module_symbol_name(src_mod, src_op->global_id);
        out.kind = LR_OP_KIND_GLOBAL;
        if (sym_name) {
            uint32_t sid = lr_session_intern(session, sym_name);
            out.global_id = (sid != UINT32_MAX) ? sid : src_op->global_id;
        } else {
            out.global_id = src_op->global_id;
        }
        break;
    }
    case LR_VAL_NULL:
        out.kind = LR_OP_KIND_NULL;
        break;
    case LR_VAL_UNDEF:
    default:
        out.kind = LR_OP_KIND_UNDEF;
        break;
    }
    return out;
}

static bool bc_opcode_has_dest(lr_opcode_t op, lr_type_t *type) {
    switch (op) {
    case LR_OP_RET:
    case LR_OP_RET_VOID:
    case LR_OP_BR:
    case LR_OP_CONDBR:
    case LR_OP_UNREACHABLE:
    case LR_OP_STORE:
        return false;
    case LR_OP_CALL:
        return type && type->kind != LR_TYPE_VOID;
    default:
        return true;
    }
}

static int bc_replay_func_to_session(const lr_module_t *src_mod,
                                      const lr_func_t *src_func,
                                      lr_session_t *session,
                                      char *err, size_t errlen) {
    lr_type_t **params = NULL;
    lr_type_t *ret_type = NULL;
    uint32_t i;
    const lr_block_t *block;
    lr_error_t serr = {0};
    int rc;

    ret_type = bc_map_type_to_session(session, src_func->ret_type);
    if (!ret_type) {
        lr_frontend_set_error(err, errlen, "unsupported bc return type");
        return -1;
    }
    if (src_func->num_params > 0) {
        params = (lr_type_t **)calloc(src_func->num_params, sizeof(*params));
        if (!params) {
            lr_frontend_set_error(err, errlen, "param allocation failed");
            return -1;
        }
        for (i = 0; i < src_func->num_params; i++) {
            params[i] = bc_map_type_to_session(session, src_func->param_types[i]);
            if (!params[i]) {
                free(params);
                lr_frontend_set_error(err, errlen, "unsupported bc param type");
                return -1;
            }
        }
    }

    rc = lr_session_func_begin(session, src_func->name, ret_type, params,
                               src_func->num_params, src_func->vararg, &serr);
    free(params);
    if (rc != 0) {
        lr_frontend_set_error(err, errlen, "%s", serr.msg);
        return -1;
    }

    for (i = 0; i < src_func->num_blocks; i++) {
        uint32_t block_id = lr_session_block(session);
        if (block_id != i) {
            lr_frontend_set_error(err, errlen,
                                  "session block allocation mismatch");
            return -1;
        }
    }

    for (block = src_func->first_block; block; block = block->next) {
        const lr_inst_t *inst;
        rc = lr_session_set_block(session, block->id, &serr);
        if (rc != 0) {
            lr_frontend_set_error(err, errlen, "%s", serr.msg);
            return -1;
        }

        for (inst = block->first; inst; inst = inst->next) {
            lr_inst_desc_t desc;
            lr_operand_desc_t *ops = NULL;
            lr_error_t emit_err = {0};
            uint32_t emit_dest;

            memset(&desc, 0, sizeof(desc));
            desc.op = inst->op;
            desc.type = bc_map_type_to_session(session, inst->type);
            desc.dest = inst->dest;
            desc.num_operands = inst->num_operands;
            desc.num_indices = inst->num_indices;
            desc.indices = inst->indices;
            desc.icmp_pred = inst->icmp_pred;
            desc.fcmp_pred = inst->fcmp_pred;
            desc.call_external_abi = inst->call_external_abi;
            desc.call_vararg = inst->call_vararg;
            desc.call_fixed_args = inst->call_fixed_args;

            if (desc.num_operands > 0) {
                uint32_t j;
                ops = (lr_operand_desc_t *)calloc(desc.num_operands,
                                                   sizeof(*ops));
                if (!ops) {
                    lr_frontend_set_error(err, errlen,
                                          "operand allocation failed");
                    return -1;
                }
                for (j = 0; j < desc.num_operands; j++) {
                    ops[j] = bc_map_operand_to_session(&inst->operands[j],
                                                        session, src_mod);
                }
                desc.operands = ops;
            }

            emit_dest = lr_session_emit(session, &desc, &emit_err);
            free(ops);
            if (emit_err.code != LR_OK) {
                lr_frontend_set_error(err, errlen, "%s", emit_err.msg);
                return -1;
            }
            if (bc_opcode_has_dest(desc.op, desc.type) &&
                desc.dest != 0 && emit_dest != desc.dest) {
                lr_frontend_set_error(err, errlen, "vreg replay mismatch");
                return -1;
            }
        }
    }

    rc = lr_session_func_end(session, NULL, &serr);
    if (rc != 0) {
        lr_frontend_set_error(err, errlen, "%s", serr.msg);
        return -1;
    }
    return 0;
}

int lr_parse_bc_to_session(const uint8_t *data, size_t len,
                           lr_session_t *session,
                           char *err, size_t errlen) {
    lr_arena_t *tmp_arena = NULL;
    lr_module_t *tmp_mod = NULL;
    lr_func_t *func;
    char parse_err[256] = {0};
    lr_error_t serr = {0};

    if (!data || len == 0 || !session) {
        lr_frontend_set_error(err, errlen,
                              "invalid bc session streaming arguments");
        return -1;
    }
    if (err && errlen > 0)
        err[0] = '\0';

    tmp_arena = lr_arena_create(0);
    if (!tmp_arena) {
        lr_frontend_set_error(err, errlen, "arena allocation failed");
        return -1;
    }

    tmp_mod = lr_parse_bc_streaming(data, len, tmp_arena, NULL, NULL,
                                    parse_err, sizeof(parse_err));
    if (!tmp_mod) {
        lr_arena_destroy(tmp_arena);
        lr_frontend_set_error(err, errlen, "%s",
                              parse_err[0] ? parse_err
                                           : "bc parse failed");
        return -1;
    }

    /* Intern all symbols from the source module into the session. */
    for (func = tmp_mod->first_func; func; func = func->next) {
        if (func->name)
            lr_session_intern(session, func->name);
    }

    /* Replay globals into the session module. */
    {
        lr_module_t *smod = lr_session_module(session);
        lr_global_t *g;
        for (g = tmp_mod->first_global; g; g = g->next) {
            if (g->is_external) {
                lr_session_global_extern(session, g->name,
                                          bc_map_type_to_session(session, g->type));
            } else {
                uint32_t gid = lr_session_global(
                    session, g->name,
                    bc_map_type_to_session(session, g->type),
                    g->is_const, g->init_data, g->init_size);
                if (g->relocs) {
                    lr_reloc_t *r;
                    for (r = g->relocs; r; r = r->next) {
                        lr_session_global_reloc(session, gid, r->offset,
                                                r->symbol_name);
                    }
                }
            }
        }
        (void)smod;
    }

    /* Replay functions: declarations first, then definitions. */
    for (func = tmp_mod->first_func; func; func = func->next) {
        if (!func->is_decl)
            continue;
        {
            lr_type_t **params = NULL;
            lr_type_t *ret_type = bc_map_type_to_session(session,
                                                          func->ret_type);
            uint32_t i;
            int rc;

            if (!ret_type) {
                lr_arena_destroy(tmp_arena);
                lr_frontend_set_error(err, errlen,
                                      "unsupported bc return type in decl");
                return -1;
            }
            if (func->num_params > 0) {
                params = (lr_type_t **)calloc(func->num_params,
                                               sizeof(*params));
                if (!params) {
                    lr_arena_destroy(tmp_arena);
                    lr_frontend_set_error(err, errlen,
                                          "param allocation failed");
                    return -1;
                }
                for (i = 0; i < func->num_params; i++) {
                    params[i] = bc_map_type_to_session(session,
                                                        func->param_types[i]);
                    if (!params[i]) {
                        free(params);
                        lr_arena_destroy(tmp_arena);
                        lr_frontend_set_error(err, errlen,
                                              "unsupported bc param type");
                        return -1;
                    }
                }
            }
            rc = lr_session_declare(session, func->name, ret_type, params,
                                    func->num_params, func->vararg, &serr);
            free(params);
            if (rc != 0) {
                lr_arena_destroy(tmp_arena);
                lr_frontend_set_error(err, errlen, "%s", serr.msg);
                return -1;
            }
        }
    }

    for (func = tmp_mod->first_func; func; func = func->next) {
        if (func->is_decl)
            continue;
        if (lr_func_finalize(func, tmp_arena) != 0) {
            lr_arena_destroy(tmp_arena);
            lr_frontend_set_error(err, errlen,
                                  "bc function finalization failed");
            return -1;
        }
        if (bc_replay_func_to_session(tmp_mod, func, session,
                                       err, errlen) != 0) {
            lr_arena_destroy(tmp_arena);
            return -1;
        }
    }

    lr_arena_destroy(tmp_arena);
    return 0;
}
