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
    uint8_t *record_is_char6;
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


static void bc_record_push(bc_reader_t *r, uint64_t val, bool is_char6) {
    if (r->record_len == r->record_cap) {
        uint32_t new_cap = r->record_cap ? r->record_cap * 2 : 64;
        uint64_t *tmp = NULL;
        uint8_t *tmp_char6 = (uint8_t *)realloc(
            r->record_is_char6, (size_t)new_cap * sizeof(uint8_t));
        if (!tmp_char6) {
            bc_error(r, "out of memory in record metadata buffer");
            return;
        }
        tmp = (uint64_t *)realloc(r->record, (size_t)new_cap * sizeof(uint64_t));
        if (!tmp) {
            r->record_is_char6 = tmp_char6;
            bc_error(r, "out of memory in record buffer");
            return;
        }
        r->record = tmp;
        r->record_is_char6 = tmp_char6;
        r->record_cap = new_cap;
    }
    r->record[r->record_len++] = val;
    r->record_is_char6[r->record_len - 1] = is_char6 ? 1u : 0u;
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
            bc_record_push(r, bc_read_vbr(r, 6), false);
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
                    bc_record_push(r, op->value, false);
            } else if (op->kind == BC_OP_FIXED) {
                uint64_t val = bc_read_fixed(r, (uint32_t)op->value);
                if (i == 0)
                    code = (uint32_t)val;
                else
                    bc_record_push(r, val, false);
            } else if (op->kind == BC_OP_VBR) {
                uint64_t val = bc_read_vbr(r, (uint32_t)op->value);
                if (i == 0)
                    code = (uint32_t)val;
                else
                    bc_record_push(r, val, false);
            } else if (op->kind == BC_OP_CHAR6) {
                uint64_t val = bc_read_fixed(r, 6);
                if (i == 0)
                    code = (uint32_t)val;
                else
                    bc_record_push(r, val, true);
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
                    bc_record_push(r, val, elem_op->kind == BC_OP_CHAR6);
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
    TYPE_CODE_METADATA     = 16,
    TYPE_CODE_OPAQUE_PTR   = 25,
    TYPE_CODE_TARGET_TYPE  = 26
};

enum {
    CONST_CODE_SETTYPE   = 1,
    CONST_CODE_NULL      = 2,
    CONST_CODE_UNDEF     = 3,
    CONST_CODE_INTEGER   = 4,
    CONST_CODE_WIDE_INTEGER = 5,
    CONST_CODE_CE_CAST   = 11,
    CONST_CODE_CE_GEP_OLD = 12,
    CONST_CODE_FLOAT     = 6,
    CONST_CODE_AGGREGATE = 7,
    CONST_CODE_STRING    = 8,
    CONST_CODE_CSTRING   = 9,
    CONST_CODE_CE_INBOUNDS_GEP = 20,
    CONST_CODE_DATA      = 22,
    CONST_CODE_CE_GEP_WITH_INRANGE_INDEX_OLD = 24,
    CONST_CODE_POISON    = 26,
    CONST_CODE_CE_GEP_WITH_INRANGE = 31,
    CONST_CODE_CE_GEP    = 32
};

enum {
    FUNC_CODE_DECLAREBLOCKS = 1,
    FUNC_CODE_INST_BINOP    = 2,
    FUNC_CODE_INST_CAST     = 3,
    FUNC_CODE_INST_SELECT   = 5,
    FUNC_CODE_INST_EXTRACTELT = 6,
    FUNC_CODE_INST_INSERTELT = 7,
    FUNC_CODE_INST_SHUFFLEVEC = 8,
    FUNC_CODE_INST_CMP      = 9,
    FUNC_CODE_INST_RET      = 10,
    FUNC_CODE_INST_BR       = 11,
    FUNC_CODE_INST_SWITCH   = 12,
    FUNC_CODE_INST_UNREACHABLE = 15,
    FUNC_CODE_INST_PHI      = 16,
    FUNC_CODE_INST_ALLOCA   = 19,
    FUNC_CODE_INST_LOAD     = 20,
    FUNC_CODE_INST_VAARG    = 23,
    FUNC_CODE_INST_STORE_OLD = 24,
    FUNC_CODE_INST_EXTRACTVALUE = 26,
    FUNC_CODE_INST_INSERTVALUE = 27,
    FUNC_CODE_INST_CMP2     = 28,
    FUNC_CODE_INST_VSELECT  = 29,
    FUNC_CODE_INST_CALL     = 34,
    FUNC_CODE_INST_GEP      = 43,
    FUNC_CODE_INST_STORE    = 44,
    FUNC_CODE_INST_UNOP     = 56,
    FUNC_CODE_INST_FREEZE   = 58
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
    uint8_t *init_bytes;
    size_t init_size;
    uint32_t *agg_elem_ids;
    uint32_t agg_elem_count;
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
    lr_global_t *global;
    uint32_t init_id;
} bc_global_init_ref_t;

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
    uint32_t cur_func_code;
    const char *cur_func_name;
    bc_global_init_ref_t *global_inits;
    uint32_t global_init_count;
    uint32_t global_init_cap;
    char *err;
    size_t errlen;
} bc_decoder_t;

static void bc_dec_error(bc_decoder_t *d, const char *fmt, ...) {
    va_list ap;
    size_t used;
    if (!d || !d->err || d->errlen == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(d->err, d->errlen, fmt, ap);
    va_end(ap);
    used = strlen(d->err);
    if (d->cur_func_name && d->cur_func_name[0] != '\0') {
        if (used + 64u < d->errlen) {
            snprintf(d->err + used, d->errlen - used, " (func=%s code=%u)",
                     d->cur_func_name, d->cur_func_code);
        }
    } else if (d->cur_func_code != 0u && used + 24u < d->errlen) {
        snprintf(d->err + used, d->errlen - used, " (code=%u)",
                 d->cur_func_code);
    }
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

static void bc_global_init_ref_push(bc_decoder_t *d, lr_global_t *global,
                                    uint32_t init_id) {
    bc_global_init_ref_t *tmp;
    uint32_t new_cap;
    if (!d || !global || init_id == 0u)
        return;
    if (d->global_init_count == d->global_init_cap) {
        new_cap = d->global_init_cap ? d->global_init_cap * 2u : 32u;
        tmp = (bc_global_init_ref_t *)realloc(
            d->global_inits, (size_t)new_cap * sizeof(*tmp));
        if (!tmp)
            return;
        d->global_inits = tmp;
        d->global_init_cap = new_cap;
    }
    d->global_inits[d->global_init_count].global = global;
    d->global_inits[d->global_init_count].init_id = init_id;
    d->global_init_count++;
}

static void bc_try_set_i8_array_initializer(lr_global_t *g, const bc_value_t *cv,
                                            lr_arena_t *arena) {
    uint8_t *buf;
    size_t gsz, copy_sz;
    if (!g || !cv || !arena || !g->type)
        return;
    if (g->type->kind != LR_TYPE_ARRAY || !g->type->array.elem ||
        g->type->array.elem->kind != LR_TYPE_I8)
        return;
    if (!cv->init_bytes || cv->init_size == 0)
        return;
    gsz = lr_type_size(g->type);
    if (gsz == 0)
        gsz = g->type->array.count;
    if (gsz == 0)
        return;
    buf = lr_arena_array(arena, uint8_t, gsz);
    if (!buf)
        return;
    memset(buf, 0, gsz);
    copy_sz = cv->init_size < gsz ? cv->init_size : gsz;
    memcpy(buf, cv->init_bytes, copy_sz);
    g->init_data = buf;
    g->init_size = gsz;
}

static void bc_store_le_bytes(uint8_t *buf, size_t buf_size,
                              size_t off, uint64_t raw, size_t width) {
    size_t nbytes;
    if (!buf || width == 0 || off >= buf_size)
        return;
    nbytes = width;
    if (nbytes > sizeof(raw))
        nbytes = sizeof(raw);
    if (off + nbytes > buf_size)
        nbytes = buf_size - off;
    memcpy(buf + off, &raw, nbytes);
}

static bool bc_try_operand_from_const_bytes(lr_type_t *ty,
                                            const uint8_t *bytes,
                                            size_t nbytes,
                                            lr_operand_t *out) {
    size_t ty_sz;
    uint64_t raw = 0;
    if (!ty || !bytes || !out)
        return false;
    ty_sz = lr_type_size(ty);
    if (ty_sz == 0 || ty_sz > 8 || nbytes < ty_sz)
        return false;
    memcpy(&raw, bytes, ty_sz);
    if (ty->kind == LR_TYPE_FLOAT && ty_sz == 4) {
        uint32_t raw32 = (uint32_t)raw;
        float f32;
        memcpy(&f32, &raw32, sizeof(f32));
        *out = lr_op_imm_f64((double)f32, ty);
        return true;
    }
    if (ty->kind == LR_TYPE_DOUBLE && ty_sz == 8) {
        double f64;
        memcpy(&f64, &raw, sizeof(f64));
        *out = lr_op_imm_f64(f64, ty);
        return true;
    }
    *out = lr_op_imm_i64((int64_t)raw, ty);
    return true;
}

static bool bc_materialize_const_bytes(const bc_value_table_t *vt,
                                       const lr_type_t *dst_ty,
                                       const bc_value_t *cv,
                                       uint8_t *buf,
                                       size_t buf_size,
                                       size_t base_off) {
    size_t dst_sz;
    if (!vt || !dst_ty || !cv || !buf)
        return false;

    if ((dst_ty->kind == LR_TYPE_STRUCT ||
         dst_ty->kind == LR_TYPE_ARRAY ||
         dst_ty->kind == LR_TYPE_VECTOR) &&
        cv->agg_elem_ids && cv->agg_elem_count > 0) {
        if (dst_ty->kind == LR_TYPE_STRUCT) {
            uint32_t n = dst_ty->struc.num_fields;
            if (n > cv->agg_elem_count)
                n = cv->agg_elem_count;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t elem_id = cv->agg_elem_ids[i];
                size_t elem_off = base_off + lr_struct_field_offset(dst_ty, i);
                if (elem_id >= vt->count ||
                    !bc_materialize_const_bytes(vt, dst_ty->struc.fields[i],
                                                &vt->values[elem_id], buf,
                                                buf_size, elem_off))
                    return false;
            }
            return true;
        }
        if (dst_ty->array.elem) {
            uint64_t n64 = dst_ty->array.count;
            size_t elem_sz = lr_type_size(dst_ty->array.elem);
            uint32_t n;
            if (elem_sz == 0)
                elem_sz = 8;
            if (n64 > UINT32_MAX)
                n = UINT32_MAX;
            else
                n = (uint32_t)n64;
            if (n > cv->agg_elem_count)
                n = cv->agg_elem_count;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t elem_id = cv->agg_elem_ids[i];
                size_t elem_off = base_off + (size_t)i * elem_sz;
                if (elem_id >= vt->count ||
                    !bc_materialize_const_bytes(vt, dst_ty->array.elem,
                                                &vt->values[elem_id], buf,
                                                buf_size, elem_off))
                    return false;
            }
            return true;
        }
    }

    dst_sz = lr_type_size(dst_ty);
    if (dst_sz == 0 && dst_ty->kind == LR_TYPE_PTR)
        dst_sz = sizeof(uint64_t);
    if (dst_sz == 0 || base_off >= buf_size)
        return false;

    if (cv->init_bytes && cv->init_size > 0) {
        size_t copy_sz = cv->init_size < dst_sz ? cv->init_size : dst_sz;
        if (base_off + copy_sz > buf_size)
            copy_sz = buf_size - base_off;
        memcpy(buf + base_off, cv->init_bytes, copy_sz);
        return true;
    }

    if (cv->kind != BC_VAL_CONST)
        return false;

    switch (cv->operand.kind) {
    case LR_VAL_NULL:
        bc_store_le_bytes(buf, buf_size, base_off, 0, dst_sz);
        return true;
    case LR_VAL_IMM_I64:
        bc_store_le_bytes(buf, buf_size, base_off,
                          (uint64_t)cv->operand.imm_i64, dst_sz);
        return true;
    case LR_VAL_IMM_F64:
        if (dst_ty->kind == LR_TYPE_FLOAT) {
            float f32 = (float)cv->operand.imm_f64;
            uint32_t raw32 = 0;
            memcpy(&raw32, &f32, sizeof(raw32));
            bc_store_le_bytes(buf, buf_size, base_off, (uint64_t)raw32, 4);
            return true;
        }
        if (dst_ty->kind == LR_TYPE_DOUBLE) {
            uint64_t raw64 = 0;
            memcpy(&raw64, &cv->operand.imm_f64, sizeof(raw64));
            bc_store_le_bytes(buf, buf_size, base_off, raw64, 8);
            return true;
        }
        return false;
    default:
        return false;
    }
}

static void bc_global_add_reloc(bc_decoder_t *d, lr_global_t *g,
                                size_t offset, uint32_t sym_id,
                                int64_t addend) {
    const char *sym_name;
    lr_reloc_t *r;
    if (!d || !g)
        return;
    sym_name = lr_module_symbol_name(d->module, sym_id);
    if (!sym_name || !sym_name[0])
        return;
    r = lr_arena_new(d->arena, lr_reloc_t);
    if (!r)
        return;
    r->offset = offset;
    r->addend = addend;
    r->symbol_name = lr_arena_strdup(d->arena, sym_name, strlen(sym_name));
    r->next = g->relocs;
    g->relocs = r;
}

static void bc_apply_global_init_value(bc_decoder_t *d,
                                       const bc_value_table_t *vt,
                                       lr_global_t *g,
                                       const lr_type_t *dst_ty,
                                       size_t base_off,
                                       uint8_t *buf,
                                       size_t buf_size,
                                       const bc_value_t *cv) {
    size_t dst_sz;
    if (!d || !vt || !g || !dst_ty || !buf || !cv)
        return;

    if ((dst_ty->kind == LR_TYPE_STRUCT ||
         dst_ty->kind == LR_TYPE_ARRAY ||
         dst_ty->kind == LR_TYPE_VECTOR) &&
        cv->agg_elem_ids && cv->agg_elem_count > 0) {
        if (dst_ty->kind == LR_TYPE_STRUCT) {
            uint32_t n = dst_ty->struc.num_fields;
            if (n > cv->agg_elem_count)
                n = cv->agg_elem_count;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t elem_id = cv->agg_elem_ids[i];
                size_t elem_off = base_off + lr_struct_field_offset(dst_ty, i);
                if (elem_id >= vt->count)
                    continue;
                bc_apply_global_init_value(d, vt, g, dst_ty->struc.fields[i],
                                           elem_off, buf, buf_size,
                                           &vt->values[elem_id]);
            }
            return;
        }
        if (dst_ty->array.elem) {
            uint64_t n64 = dst_ty->array.count;
            size_t elem_sz = lr_type_size(dst_ty->array.elem);
            uint32_t n;
            if (elem_sz == 0)
                elem_sz = 8;
            if (n64 > UINT32_MAX)
                n = UINT32_MAX;
            else
                n = (uint32_t)n64;
            if (n > cv->agg_elem_count)
                n = cv->agg_elem_count;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t elem_id = cv->agg_elem_ids[i];
                size_t elem_off = base_off + (size_t)i * elem_sz;
                if (elem_id >= vt->count)
                    continue;
                bc_apply_global_init_value(d, vt, g, dst_ty->array.elem,
                                           elem_off, buf, buf_size,
                                           &vt->values[elem_id]);
            }
            return;
        }
    }

    dst_sz = lr_type_size(dst_ty);
    if (dst_sz == 0 && dst_ty->kind == LR_TYPE_PTR)
        dst_sz = sizeof(uint64_t);

    if (cv->init_bytes && cv->init_size > 0 && dst_sz > 0 && base_off < buf_size) {
        size_t copy_sz = cv->init_size < dst_sz ? cv->init_size : dst_sz;
        if (base_off + copy_sz > buf_size)
            copy_sz = buf_size - base_off;
        memcpy(buf + base_off, cv->init_bytes, copy_sz);
    }

    if (cv->kind == BC_VAL_GLOBAL) {
        bc_global_add_reloc(d, g, base_off, cv->global_sym, 0);
        return;
    }
    if (cv->kind == BC_VAL_FUNC) {
        if (cv->func && cv->func->name) {
            uint32_t sym_id = lr_frontend_intern_symbol(d->module, cv->func->name);
            bc_global_add_reloc(d, g, base_off, sym_id, 0);
        }
        return;
    }
    if (cv->kind != BC_VAL_CONST)
        return;

    switch (cv->operand.kind) {
    case LR_VAL_GLOBAL:
        bc_global_add_reloc(d, g, base_off, cv->operand.global_id,
                            cv->operand.global_offset);
        break;
    case LR_VAL_IMM_I64:
        if (dst_sz > 0)
            bc_store_le_bytes(buf, buf_size, base_off,
                              (uint64_t)cv->operand.imm_i64, dst_sz);
        break;
    case LR_VAL_IMM_F64:
        if (dst_ty->kind == LR_TYPE_FLOAT) {
            float f32 = (float)cv->operand.imm_f64;
            uint32_t raw32 = 0;
            memcpy(&raw32, &f32, sizeof(raw32));
            bc_store_le_bytes(buf, buf_size, base_off, (uint64_t)raw32, 4);
        } else if (dst_ty->kind == LR_TYPE_DOUBLE) {
            uint64_t raw64 = 0;
            memcpy(&raw64, &cv->operand.imm_f64, sizeof(raw64));
            bc_store_le_bytes(buf, buf_size, base_off, raw64, 8);
        }
        break;
    default:
        break;
    }
}

static uint32_t bc_resolve_global_init_value_index(const bc_decoder_t *d,
                                                   uint32_t init_id,
                                                   uint32_t constants_base) {
    uint32_t cands[4];
    uint32_t n = 0;
    uint32_t i;
    const int verbose = (getenv("LIRIC_VERBOSE_BC_GLOBALS") != NULL);
    if (!d || init_id == 0u)
        return UINT32_MAX;
    if (init_id > 0u)
        cands[n++] = init_id - 1u;
    cands[n++] = init_id;
    if (init_id > 0u)
        cands[n++] = constants_base + init_id - 1u;
    cands[n++] = constants_base + init_id;
    if (verbose) {
        fprintf(stderr, "bc global-init-resolve: init_id=%u base=%u cands=", init_id, constants_base);
        for (i = 0; i < n; i++) {
            uint32_t idx = cands[i];
            int kind = (idx < d->global_values.count)
                           ? (int)d->global_values.values[idx].kind
                           : -1;
            fprintf(stderr, "%s%u(kind=%d)", (i == 0) ? "" : ",", idx, kind);
        }
        fprintf(stderr, "\n");
    }
    for (i = 0; i < n; i++) {
        uint32_t idx = cands[i];
        if (idx >= d->global_values.count)
            continue;
        if (d->global_values.values[idx].kind == BC_VAL_CONST ||
            d->global_values.values[idx].kind == BC_VAL_GLOBAL ||
            d->global_values.values[idx].kind == BC_VAL_FUNC)
            return idx;
    }
    return UINT32_MAX;
}

static void bc_apply_global_initializers(bc_decoder_t *d, uint32_t constants_base) {
    uint32_t i;
    const int verbose = (getenv("LIRIC_VERBOSE_BC_GLOBALS") != NULL);
    if (!d)
        return;
    for (i = 0; i < d->global_init_count; i++) {
        bc_global_init_ref_t *ref = &d->global_inits[i];
        uint32_t val_idx;
        if (!ref->global || ref->init_id == 0u)
            continue;
        val_idx = bc_resolve_global_init_value_index(d, ref->init_id, constants_base);
        if (verbose) {
            fprintf(stderr,
                    "bc global-init: name=%s init_id=%u base=%u chosen=%u count=%u\n",
                    ref->global->name ? ref->global->name : "<null>",
                    ref->init_id,
                    constants_base,
                    val_idx,
                    d->global_values.count);
            if (d->global_values.count > 0) {
                uint32_t start = (ref->init_id > 2u) ? ref->init_id - 2u : 0u;
                uint32_t end = ref->init_id + 3u;
                if (end > d->global_values.count)
                    end = d->global_values.count;
                for (uint32_t vi = start; vi < end; vi++) {
                    const bc_value_t *cv = &d->global_values.values[vi];
                    unsigned first = (cv->init_bytes && cv->init_size > 0)
                                         ? (unsigned)cv->init_bytes[0]
                                         : 0u;
                    int tkind = cv->type ? (int)cv->type->kind : -1;
                    unsigned long long tsize =
                        (unsigned long long)(cv->type ? lr_type_size(cv->type) : 0u);
                    unsigned long long tcount =
                        (unsigned long long)((cv->type &&
                                              (cv->type->kind == LR_TYPE_ARRAY ||
                                               cv->type->kind == LR_TYPE_VECTOR))
                                                 ? cv->type->array.count
                                                 : 0u);
                    fprintf(stderr,
                            "bc global-init:   val[%u] kind=%d type=%d size=%llu count=%llu init_size=%zu first=%u\n",
                            vi, (int)cv->kind, tkind, tsize, tcount, cv->init_size, first);
                }
            }
        }
        if (val_idx == UINT32_MAX)
            continue;
        {
            const bc_value_t *cv = &d->global_values.values[val_idx];
            size_t gsz = lr_type_size(ref->global->type);
            uint8_t *buf;
            if (gsz == 0)
                gsz = 8;
            buf = lr_arena_array(d->arena, uint8_t, gsz);
            if (!buf) {
                ref->init_id = 0u;
                continue;
            }
            memset(buf, 0, gsz);
            ref->global->init_data = buf;
            ref->global->init_size = gsz;
            ref->global->relocs = NULL;
            bc_apply_global_init_value(d, &d->global_values, ref->global,
                                       ref->global->type, 0, buf, gsz, cv);
            /* Fast path retained for exact i8 arrays from string/data constants. */
            bc_try_set_i8_array_initializer(ref->global, cv, d->arena);
        }
        if (verbose && ref->global->init_data && ref->global->init_size > 0) {
            fprintf(stderr,
                    "bc global-init: applied name=%s size=%zu first=%u\n",
                    ref->global->name ? ref->global->name : "<null>",
                    ref->global->init_size,
                    (unsigned)ref->global->init_data[0]);
        }
        ref->init_id = 0u;
    }
}

static lr_type_t *bc_get_type(bc_decoder_t *d, uint32_t idx) {
    if (idx >= d->types.count) {
        bc_dec_error(d, "type index %u out of range (have %u, func_code=%u)",
                     idx, d->types.count, d->cur_func_code);
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
    if (val_id > vt->count + 65536u) {
        bc_dec_error(d, "value id too far ahead: id=%u have=%u", val_id, vt->count);
        op.kind = LR_VAL_UNDEF;
        op.type = type_hint ? type_hint : d->module->type_i32;
        return op;
    }
    while (val_id >= vt->count) {
        bc_value_t fwd;
        uint32_t before = vt->count;
        fwd.kind = BC_VAL_VREG;
        fwd.type = type_hint ? type_hint : d->module->type_i32;
        fwd.init_bytes = NULL;
        fwd.init_size = 0;
        fwd.agg_elem_ids = NULL;
        fwd.agg_elem_count = 0;
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
        if (op.kind == LR_VAL_UNDEF && v->init_bytes && v->init_size > 0) {
            lr_operand_t packed;
            if (bc_try_operand_from_const_bytes(v->type, v->init_bytes,
                                                v->init_size, &packed))
                op = packed;
        }
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

static bool bc_const_gep_try_compute_offset(lr_type_t *source_ty,
                                            const lr_operand_t *idx_ops,
                                            uint32_t num_indices,
                                            int64_t *out_offset) {
    const lr_type_t *cur_ty = source_ty;
    int64_t total = 0;
    uint32_t i;

    if (!source_ty || !out_offset)
        return false;
    for (i = 0; i < num_indices; i++) {
        lr_gep_step_t step;
        if (!lr_gep_analyze_step(cur_ty, i == 0u, &idx_ops[i], &step))
            return false;
        if (!step.is_const)
            return false;
        if ((step.const_byte_offset > 0 &&
             total > INT64_MAX - step.const_byte_offset) ||
            (step.const_byte_offset < 0 &&
             total < INT64_MIN - step.const_byte_offset)) {
            return false;
        }
        total += step.const_byte_offset;
        cur_ty = step.next_type ? step.next_type : cur_ty;
    }
    *out_offset = total;
    return true;
}

static int64_t bc_decode_signed_vbr(uint64_t v) {
    if ((v & 1) == 0)
        return (int64_t)(v >> 1);
    if (v != 1)
        return -(int64_t)(v >> 1);
    return INT64_MIN;
}

static uint8_t bc_decode_char6(uint64_t v) {
    if (v < 26u)
        return (uint8_t)('a' + (uint8_t)v);
    if (v < 52u)
        return (uint8_t)('A' + (uint8_t)(v - 26u));
    if (v < 62u)
        return (uint8_t)('0' + (uint8_t)(v - 52u));
    if (v == 62u)
        return (uint8_t)'.';
    return (uint8_t)'_';
}

static bool bc_resolve_rel_value_id(bc_decoder_t *d, uint32_t base_value_id,
                                    uint32_t rel, uint32_t *out_val_id) {
    if (d->bc_version >= 1) {
        *out_val_id = base_value_id - rel;
    } else {
        *out_val_id = rel;
    }
    return true;
}

static bool bc_resolve_phi_value_id(bc_decoder_t *d, uint32_t base_before_def,
                                    int64_t rel_signed, uint32_t *out_val_id) {
    if (rel_signed >= 0) {
        if (d->bc_version >= 1) {
            if ((uint64_t)rel_signed > (uint64_t)base_before_def) {
                bc_dec_error(d, "invalid PHI relative value id: rel=%lld base=%u",
                             (long long)rel_signed, base_before_def);
                return false;
            }
            *out_val_id = base_before_def - (uint32_t)rel_signed;
        } else {
            *out_val_id = (uint32_t)rel_signed;
        }
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

static bool bc_record_get_value(bc_decoder_t *d, const uint64_t *record,
                                uint32_t record_len, uint32_t *slot,
                                uint32_t base_value_id, uint32_t *out_val_id) {
    uint32_t raw;
    if (*slot >= record_len) {
        bc_dec_error(d, "malformed record: missing value operand");
        return false;
    }
    raw = (uint32_t)record[*slot];
    (*slot)++;
    if (d->bc_version >= 1)
        *out_val_id = base_value_id - raw;
    else
        *out_val_id = raw;
    return true;
}

static bool bc_record_get_value_type_pair(bc_decoder_t *d, bc_value_table_t *vt,
                                          const uint64_t *record, uint32_t record_len,
                                          uint32_t *slot, uint32_t base_value_id,
                                          uint32_t *out_val_id, lr_type_t **out_type) {
    uint32_t val_id;
    if (!bc_record_get_value(d, record, record_len, slot, base_value_id, &val_id))
        return false;

    if (val_id < base_value_id) {
        if (val_id >= vt->count) {
            bc_dec_error(d, "invalid backward value id: id=%u base=%u have=%u",
                         val_id, base_value_id, vt->count);
            return false;
        }
        *out_type = vt->values[val_id].type;
    } else {
        uint32_t ty_idx;
        lr_type_t *ty;
        if (*slot >= record_len) {
            bc_dec_error(d, "malformed record: missing forward type id");
            return false;
        }
        ty_idx = (uint32_t)record[*slot];
        (*slot)++;
        ty = bc_get_type(d, ty_idx);
        if (!ty)
            return false;
        *out_type = ty;
    }

    if (!*out_type)
        *out_type = d->module->type_i32;
    *out_val_id = val_id;
    return true;
}

static bool bc_define_vreg_value(bc_decoder_t *d, bc_value_table_t *vt, lr_func_t *func,
                                 uint32_t value_id, lr_type_t *type, uint32_t *out_vreg) {
    while (value_id >= vt->count) {
        bc_value_t fwd;
        uint32_t before = vt->count;
        fwd.kind = BC_VAL_VREG;
        fwd.type = d->module->type_i32;
        fwd.init_bytes = NULL;
        fwd.init_size = 0;
        fwd.agg_elem_ids = NULL;
        fwd.agg_elem_count = 0;
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
        fwd.init_bytes = NULL;
        fwd.init_size = 0;
        fwd.agg_elem_ids = NULL;
        fwd.agg_elem_count = 0;
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
        slot->init_bytes = NULL;
        slot->init_size = 0;
        slot->agg_elem_ids = NULL;
        slot->agg_elem_count = 0;
        slot->operand.kind = LR_VAL_UNDEF;
        slot->operand.type = slot->type;
        slot->operand.global_offset = 0;
    }
    return true;
}

static bool bc_define_alias_value(bc_decoder_t *d, bc_value_table_t *vt,
                                  uint32_t value_id, lr_operand_t op,
                                  lr_type_t *type) {
    while (value_id >= vt->count) {
        bc_value_t fwd;
        uint32_t before = vt->count;
        fwd.kind = BC_VAL_VREG;
        fwd.type = d->module->type_i32;
        fwd.init_bytes = NULL;
        fwd.init_size = 0;
        fwd.agg_elem_ids = NULL;
        fwd.agg_elem_count = 0;
        fwd.vreg = 0;
        bc_value_push(vt, fwd);
        if (vt->count == before) {
            bc_dec_error(d, "out of memory while materializing alias value");
            return false;
        }
    }

    {
        bc_value_t *slot = &vt->values[value_id];
        lr_type_t *alias_type = type ? type : op.type;
        if (!alias_type)
            alias_type = d->module->type_i32;
        slot->kind = BC_VAL_CONST;
        slot->type = alias_type;
        slot->init_bytes = NULL;
        slot->init_size = 0;
        slot->agg_elem_ids = NULL;
        slot->agg_elem_count = 0;
        slot->operand = op;
        slot->operand.type = alias_type;
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

static lr_func_t *bc_find_module_function(const lr_module_t *m,
                                          const char *name) {
    lr_func_t *f;
    if (!m || !name || !name[0])
        return NULL;
    for (f = m->first_func; f; f = f->next) {
        if (f->name && strcmp(f->name, name) == 0)
            return f;
    }
    return NULL;
}

static lr_func_t *bc_resolve_call_callee_func(const bc_decoder_t *d,
                                              const bc_value_table_t *local_vt,
                                              uint32_t callee_vid,
                                              lr_operand_t callee_op) {
    const char *name = NULL;
    if (!d || !d->module || !local_vt)
        return NULL;
    if (callee_vid < local_vt->count) {
        const bc_value_t *cv = &local_vt->values[callee_vid];
        if (cv->kind == BC_VAL_FUNC && cv->func)
            return cv->func;
        if (cv->kind == BC_VAL_GLOBAL)
            name = lr_module_symbol_name(d->module, cv->global_sym);
    }
    if (!name && callee_op.kind == LR_VAL_GLOBAL)
        name = lr_module_symbol_name(d->module, callee_op.global_id);
    return name ? bc_find_module_function(d->module, name) : NULL;
}

static bool bc_append_unique_u32(uint32_t *vals, uint32_t *count,
                                 uint32_t cap, uint32_t v) {
    uint32_t i;
    if (!vals || !count || *count > cap)
        return false;
    for (i = 0; i < *count; i++) {
        if (vals[i] == v)
            return true;
    }
    if (*count >= cap)
        return false;
    vals[*count] = v;
    (*count)++;
    return true;
}

static bool bc_switch_remap_phi_preds(bc_decoder_t *d, lr_block_t *succ,
                                      uint32_t old_pred,
                                      const uint32_t *new_preds,
                                      uint32_t new_pred_count,
                                      bool keep_old_pred) {
    lr_inst_t *inst;
    if (!d || !succ)
        return false;
    for (inst = succ->first; inst; inst = inst->next) {
        uint32_t i;
        uint32_t old_hits = 0;
        uint32_t new_pairs;
        uint32_t out_i = 0;
        lr_operand_t *rewritten;

        if (inst->op != LR_OP_PHI)
            continue;
        if (inst->num_operands < 2 || (inst->num_operands & 1u) != 0u) {
            bc_dec_error(d, "malformed phi node in switch successor");
            return false;
        }

        for (i = 0; i + 1 < inst->num_operands; i += 2) {
            const lr_operand_t *pred = &inst->operands[i + 1];
            if (pred->kind == LR_VAL_BLOCK && pred->block_id == old_pred)
                old_hits++;
        }
        if (old_hits == 0)
            continue;

        if (!keep_old_pred && new_pred_count == 0) {
            bc_dec_error(d, "switch lowering removed predecessor with no replacement");
            return false;
        }

        {
            uint32_t delta_pairs;
            if (keep_old_pred) {
                delta_pairs = old_hits * new_pred_count;
            } else {
                delta_pairs = old_hits * (new_pred_count - 1u);
            }
            new_pairs = (inst->num_operands / 2u) + delta_pairs;
        }

        rewritten = lr_arena_array(d->arena, lr_operand_t, new_pairs * 2u);
        if (!rewritten) {
            bc_dec_error(d, "out of memory remapping switch phi operands");
            return false;
        }

        for (i = 0; i + 1 < inst->num_operands; i += 2) {
            const lr_operand_t val = inst->operands[i];
            const lr_operand_t pred = inst->operands[i + 1];
            if (pred.kind == LR_VAL_BLOCK && pred.block_id == old_pred) {
                uint32_t pi;
                if (keep_old_pred) {
                    rewritten[out_i++] = val;
                    rewritten[out_i++] = pred;
                }
                for (pi = 0; pi < new_pred_count; pi++) {
                    /* Skip duplicate predecessors — a switch may
                       route multiple cases to the same target block,
                       but PHI must have exactly one entry per pred. */
                    uint32_t qi;
                    bool dup = false;
                    for (qi = 0; qi < out_i; qi += 2) {
                        if (rewritten[qi + 1].kind == LR_VAL_BLOCK &&
                            rewritten[qi + 1].block_id == new_preds[pi]) {
                            dup = true;
                            break;
                        }
                    }
                    if (dup)
                        continue;
                    rewritten[out_i++] = val;
                    rewritten[out_i++] = lr_op_block(new_preds[pi]);
                }
            } else {
                rewritten[out_i++] = val;
                rewritten[out_i++] = pred;
            }
        }
        inst->operands = rewritten;
        inst->num_operands = out_i;
    }
    return true;
}

typedef struct bc_switch_phi_fixup {
    uint32_t succ_bb;
    uint32_t old_pred;
    bool keep_old_pred;
    uint32_t *new_preds;
    uint32_t new_pred_count;
} bc_switch_phi_fixup_t;

static bool bc_push_switch_phi_fixup(bc_decoder_t *d,
                                     bc_switch_phi_fixup_t **arr,
                                     uint32_t *count,
                                     uint32_t *cap,
                                     uint32_t succ_bb,
                                     uint32_t old_pred,
                                     bool keep_old_pred,
                                     const uint32_t *new_preds,
                                     uint32_t new_pred_count) {
    bc_switch_phi_fixup_t *grown;
    uint32_t new_cap;
    uint32_t *pred_copy;

    if (!d || !arr || !count || !cap || !new_preds || new_pred_count == 0)
        return false;

    if (*count == *cap) {
        new_cap = (*cap == 0u) ? 8u : (*cap * 2u);
        grown = (bc_switch_phi_fixup_t *)realloc(*arr,
                                                 (size_t)new_cap * sizeof(*grown));
        if (!grown) {
            bc_dec_error(d, "out of memory growing switch phi fixups");
            return false;
        }
        *arr = grown;
        *cap = new_cap;
    }

    pred_copy = (uint32_t *)malloc((size_t)new_pred_count * sizeof(uint32_t));
    if (!pred_copy) {
        bc_dec_error(d, "out of memory storing switch phi fixup preds");
        return false;
    }
    memcpy(pred_copy, new_preds, (size_t)new_pred_count * sizeof(uint32_t));

    (*arr)[*count].succ_bb = succ_bb;
    (*arr)[*count].old_pred = old_pred;
    (*arr)[*count].keep_old_pred = keep_old_pred;
    (*arr)[*count].new_preds = pred_copy;
    (*arr)[*count].new_pred_count = new_pred_count;
    (*count)++;
    return true;
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
            case TYPE_CODE_OPAQUE_PTR:
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
                cv.init_bytes = NULL;
                cv.init_size = 0;
                cv.agg_elem_ids = NULL;
                cv.agg_elem_count = 0;
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
                cv.init_bytes = NULL;
                cv.init_size = 0;
                cv.agg_elem_ids = NULL;
                cv.agg_elem_count = 0;
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
                cv.init_bytes = NULL;
                cv.init_size = 0;
                cv.agg_elem_ids = NULL;
                cv.agg_elem_count = 0;
                cv.operand = lr_op_imm_i64(val, cur_type);
                bc_value_push(vt, cv);
                break;
            }
            case CONST_CODE_WIDE_INTEGER: {
                bc_value_t cv;
                int64_t val = 0;
                if (r->record_len > 0)
                    val = bc_decode_signed_vbr(r->record[0]);
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                cv.init_bytes = NULL;
                cv.init_size = 0;
                cv.agg_elem_ids = NULL;
                cv.agg_elem_count = 0;
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
                cv.init_bytes = NULL;
                cv.init_size = 0;
                cv.agg_elem_ids = NULL;
                cv.agg_elem_count = 0;
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
            case CONST_CODE_STRING:
            case CONST_CODE_CSTRING:
            case CONST_CODE_DATA: {
                bc_value_t cv;
                size_t nbytes = r->record_len;
                uint8_t *bytes = NULL;
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                cv.init_bytes = NULL;
                cv.init_size = 0;
                cv.agg_elem_ids = NULL;
                cv.agg_elem_count = 0;
                cv.operand.kind = LR_VAL_UNDEF;
                cv.operand.type = cur_type;
                cv.operand.global_offset = 0;
                if (code == CONST_CODE_DATA && cur_type &&
                    (cur_type->kind == LR_TYPE_ARRAY ||
                     cur_type->kind == LR_TYPE_VECTOR) &&
                    cur_type->array.elem) {
                    lr_type_t *elem_ty = cur_type->array.elem;
                    size_t elem_sz = lr_type_size(elem_ty);
                    uint64_t elem_count64 = cur_type->array.count;
                    size_t packed_sz = lr_type_size(cur_type);
                    uint32_t elem_count;
                    if (elem_sz == 0 && elem_ty->kind == LR_TYPE_PTR)
                        elem_sz = sizeof(uint64_t);
                    if (elem_sz == 0 || elem_sz > 8)
                        goto const_data_byte_fallback;
                    if (elem_count64 > UINT32_MAX)
                        elem_count = UINT32_MAX;
                    else
                        elem_count = (uint32_t)elem_count64;
                    if (elem_count == 0)
                        elem_count = (uint32_t)r->record_len;
                    if (packed_sz == 0)
                        packed_sz = elem_sz * (size_t)elem_count;
                    if (packed_sz == 0)
                        goto const_data_byte_fallback;
                    bytes = lr_arena_array(d->arena, uint8_t, packed_sz);
                    if (!bytes)
                        goto const_data_byte_fallback;
                    memset(bytes, 0, packed_sz);
                    {
                        size_t n = r->record_len;
                        if (n > elem_count)
                            n = elem_count;
                        for (size_t i = 0; i < n; i++) {
                            size_t off = i * elem_sz;
                            uint64_t raw = r->record[i];
                            if (off >= packed_sz)
                                break;
                            if (elem_ty->kind == LR_TYPE_FLOAT && elem_sz == 4) {
                                bc_store_le_bytes(bytes, packed_sz, off,
                                                  (uint64_t)(uint32_t)raw, 4);
                            } else if (elem_ty->kind == LR_TYPE_DOUBLE && elem_sz == 8) {
                                bc_store_le_bytes(bytes, packed_sz, off, raw, 8);
                            } else {
                                bc_store_le_bytes(bytes, packed_sz, off, raw, elem_sz);
                            }
                        }
                    }
                    cv.init_bytes = bytes;
                    cv.init_size = packed_sz;
                    if (cv.operand.kind == LR_VAL_UNDEF)
                        (void)bc_try_operand_from_const_bytes(
                            cur_type, bytes, packed_sz, &cv.operand);
                    bc_value_push(vt, cv);
                    break;
                }
const_data_byte_fallback:
                if (nbytes > 0) {
                    bytes = lr_arena_array(d->arena, uint8_t, nbytes);
                    if (bytes) {
                        size_t i;
                        bool decode_char6 = false;
                        if (code == CONST_CODE_STRING ||
                            code == CONST_CODE_CSTRING) {
                            decode_char6 = true;
                            for (i = 0; i < nbytes; i++) {
                                if (!r->record_is_char6 ||
                                    !r->record_is_char6[i]) {
                                    decode_char6 = false;
                                    break;
                                }
                            }
                        }
                        for (i = 0; i < nbytes; i++) {
                            if (decode_char6) {
                                bytes[i] = bc_decode_char6(r->record[i]);
                            } else {
                                bytes[i] = (uint8_t)r->record[i];
                            }
                        }
                        cv.init_bytes = bytes;
                        cv.init_size = nbytes;
                    }
                }
                bc_value_push(vt, cv);
                break;
            }
            case CONST_CODE_AGGREGATE: {
                bc_value_t cv;
                size_t nbytes = 0;
                uint8_t *bytes = NULL;
                uint32_t *elem_ids = NULL;
                size_t packed_sz = 0;
                uint8_t *packed_bytes = NULL;
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                cv.init_bytes = NULL;
                cv.init_size = 0;
                cv.agg_elem_ids = NULL;
                cv.agg_elem_count = 0;
                cv.operand.kind = LR_VAL_UNDEF;
                cv.operand.type = cur_type;
                cv.operand.global_offset = 0;
                if (r->record_len > 0 && r->record_len <= UINT32_MAX) {
                    elem_ids = lr_arena_array(d->arena, uint32_t, (uint32_t)r->record_len);
                    if (elem_ids) {
                        for (size_t i = 0; i < r->record_len; i++)
                            elem_ids[i] = (uint32_t)r->record[i];
                        cv.agg_elem_ids = elem_ids;
                        cv.agg_elem_count = (uint32_t)r->record_len;
                    }
                }
                if (cur_type && cur_type->kind == LR_TYPE_ARRAY &&
                    cur_type->array.elem &&
                    cur_type->array.elem->kind == LR_TYPE_I8) {
                    nbytes = (size_t)cur_type->array.count;
                    if (nbytes == 0)
                        nbytes = r->record_len;
                    if (nbytes > 0) {
                        bytes = lr_arena_array(d->arena, uint8_t, nbytes);
                        if (bytes) {
                            size_t i;
                            memset(bytes, 0, nbytes);
                            for (i = 0; i < r->record_len && i < nbytes; i++) {
                                lr_operand_t elem = bc_make_operand_from_value(
                                    d, vt, (uint32_t)r->record[i], NULL,
                                    cur_type->array.elem);
                                if (elem.kind == LR_VAL_IMM_I64)
                                    bytes[i] = (uint8_t)elem.imm_i64;
                            }
                            cv.init_bytes = bytes;
                            cv.init_size = nbytes;
                        }
                    }
                }
                if (cur_type) {
                    packed_sz = lr_type_size(cur_type);
                    if (packed_sz > 0) {
                        packed_bytes = lr_arena_array(d->arena, uint8_t, packed_sz);
                        if (packed_bytes) {
                            memset(packed_bytes, 0, packed_sz);
                            if (bc_materialize_const_bytes(vt, cur_type, &cv,
                                                           packed_bytes, packed_sz, 0)) {
                                cv.init_bytes = packed_bytes;
                                cv.init_size = packed_sz;
                                if (cv.operand.kind == LR_VAL_UNDEF)
                                    (void)bc_try_operand_from_const_bytes(
                                        cur_type, packed_bytes, packed_sz, &cv.operand);
                            }
                        }
                    }
                }
                bc_value_push(vt, cv);
                break;
            }
            case CONST_CODE_CE_CAST: {
                bc_value_t cv;
                uint32_t op_ty_idx;
                uint32_t op_vid;
                lr_type_t *op_ty;
                lr_operand_t op;
                if (r->record_len < 3) {
                    cv.kind = BC_VAL_CONST;
                    cv.type = cur_type;
                    cv.init_bytes = NULL;
                    cv.init_size = 0;
                    cv.agg_elem_ids = NULL;
                    cv.agg_elem_count = 0;
                    cv.operand.kind = LR_VAL_UNDEF;
                    cv.operand.type = cur_type;
                    cv.operand.global_offset = 0;
                    bc_value_push(vt, cv);
                    break;
                }
                op_ty_idx = (uint32_t)r->record[1];
                op_vid = (uint32_t)r->record[2];
                op_ty = bc_get_type(d, op_ty_idx);
                if (!op_ty)
                    op_ty = d->module->type_ptr;
                op = bc_make_operand_from_value(d, vt, op_vid, NULL, op_ty);
                op.type = cur_type;
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                cv.init_bytes = NULL;
                cv.init_size = 0;
                cv.agg_elem_ids = NULL;
                cv.agg_elem_count = 0;
                cv.operand = op;
                bc_value_push(vt, cv);
                break;
            }
            case CONST_CODE_CE_GEP_OLD:
            case CONST_CODE_CE_INBOUNDS_GEP:
            case CONST_CODE_CE_GEP_WITH_INRANGE_INDEX_OLD:
            case CONST_CODE_CE_GEP_WITH_INRANGE:
            case CONST_CODE_CE_GEP: {
                bc_value_t cv;
                uint32_t op_num = 0;
                lr_type_t *pointee_ty = NULL;
                lr_type_t *source_ty = NULL;
                uint32_t base_ty_idx, base_vid;
                lr_operand_t base_op;
                bool folded = false;
                int64_t const_offset = 0;

                memset(&base_op, 0, sizeof(base_op));
                base_op.kind = LR_VAL_UNDEF;
                base_op.type = cur_type;

                if (code == CONST_CODE_CE_GEP_WITH_INRANGE_INDEX_OLD ||
                    code == CONST_CODE_CE_GEP_WITH_INRANGE ||
                    code == CONST_CODE_CE_GEP ||
                    ((r->record_len & 1u) != 0u)) {
                    if (op_num >= r->record_len)
                        goto const_gep_fallback;
                    pointee_ty = bc_get_type(d, (uint32_t)r->record[op_num++]);
                }
                if (code == CONST_CODE_CE_GEP_WITH_INRANGE_INDEX_OLD ||
                    code == CONST_CODE_CE_GEP) {
                    if (op_num >= r->record_len)
                        goto const_gep_fallback;
                    op_num++; /* flags */
                } else if (code == CONST_CODE_CE_GEP_WITH_INRANGE) {
                    /* InRange payload is variable-size; unsupported for now. */
                    goto const_gep_fallback;
                }
                if (op_num + 1 >= r->record_len)
                    goto const_gep_fallback;

                base_ty_idx = (uint32_t)r->record[op_num++];
                source_ty = pointee_ty;
                if (!source_ty)
                    source_ty = bc_get_type(d, base_ty_idx);
                if (r->has_error)
                    return false;
                base_vid = (uint32_t)r->record[op_num++];
                base_op = bc_make_operand_from_value(d, vt, base_vid, NULL, d->module->type_ptr);

                if (base_op.kind == LR_VAL_GLOBAL) {
                    uint32_t idx_words = r->record_len - op_num;
                    uint32_t num_indices = idx_words / 2u;
                    lr_operand_t *idx_ops = NULL;
                    uint32_t i;
                    if ((idx_words & 1u) != 0u) {
                        goto const_gep_fallback;
                    }
                    if (num_indices > 0) {
                        idx_ops = (lr_operand_t *)malloc((size_t)num_indices * sizeof(lr_operand_t));
                        if (!idx_ops)
                            goto const_gep_fallback;
                        for (i = 0; i < num_indices; i++) {
                            uint32_t idx_ty_idx = (uint32_t)r->record[op_num++];
                            uint32_t idx_vid = (uint32_t)r->record[op_num++];
                            lr_type_t *idx_ty = bc_get_type(d, idx_ty_idx);
                            if (r->has_error) {
                                free(idx_ops);
                                return false;
                            }
                            idx_ops[i] = bc_make_operand_from_value(
                                d, vt, idx_vid, NULL, idx_ty ? idx_ty : d->module->type_i64);
                        }
                    }

                    if (op_num == r->record_len && source_ty &&
                        bc_const_gep_try_compute_offset(source_ty, idx_ops,
                                                        num_indices, &const_offset)) {
                        folded = true;
                    }
                    free(idx_ops);
                    if (folded) {
                        cv.kind = BC_VAL_CONST;
                        cv.type = cur_type;
                        cv.init_bytes = NULL;
                        cv.init_size = 0;
                        cv.agg_elem_ids = NULL;
                        cv.agg_elem_count = 0;
                        cv.operand = base_op;
                        cv.operand.type = cur_type;
                        cv.operand.global_offset += const_offset;
                        bc_value_push(vt, cv);
                        break;
                    }
                }

const_gep_fallback:
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                cv.init_bytes = NULL;
                cv.init_size = 0;
                cv.agg_elem_ids = NULL;
                cv.agg_elem_count = 0;
                if (base_op.kind == LR_VAL_GLOBAL) {
                    cv.operand = base_op;
                    cv.operand.type = cur_type;
                } else {
                    cv.operand.kind = LR_VAL_UNDEF;
                    cv.operand.type = cur_type;
                    cv.operand.global_offset = 0;
                }
                bc_value_push(vt, cv);
                break;
            }
            default: {
                bc_value_t cv;
                cv.kind = BC_VAL_CONST;
                cv.type = cur_type;
                cv.init_bytes = NULL;
                cv.init_size = 0;
                cv.agg_elem_ids = NULL;
                cv.agg_elem_count = 0;
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
        case 4: return LR_OP_FDIV;
        case 5: return LR_OP_FREM;
        default: return LR_OP_FADD;
        }
    }
    switch (opc) {
    case 0:  return LR_OP_ADD;
    case 1:  return LR_OP_SUB;
    case 2:  return LR_OP_MUL;
    case 3:  return LR_OP_UDIV;
    case 4:  return LR_OP_SDIV;
    case 5:  return LR_OP_UREM;
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
    if (!t)
        return false;
    if (t->kind == LR_TYPE_FLOAT || t->kind == LR_TYPE_DOUBLE)
        return true;
    if (t->kind == LR_TYPE_VECTOR && t->array.elem)
        return bc_type_is_fp(t->array.elem);
    return false;
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
    bc_switch_phi_fixup_t *switch_fixups = NULL;
    uint32_t switch_fixup_count = 0;
    uint32_t switch_fixup_cap = 0;
    uint32_t i;
    bool dbg_switch = getenv("LIRIC_DBG_BC_SWITCH") != NULL;
    bool ok = true;

    memset(&local_vt, 0, sizeof(local_vt));
    d->cur_func_name = (func && func->name) ? func->name : NULL;

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

            d->cur_func_code = code;
            switch (code) {
            case FUNC_CODE_INST_RET: {
                lr_inst_t *inst;
                if (r->record_len == 0) {
                    inst = lr_inst_create(d->arena, LR_OP_RET_VOID,
                                          d->module->type_void, 0, NULL, 0);
                } else {
                    uint32_t vid = 0;
                    lr_operand_t op;
                    lr_type_t *ret_ty = NULL;
                    uint32_t op_num = 0;
                    if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                       &op_num, next_value_id, &vid, &ret_ty)) {
                        ok = false;
                        break;
                    }
                    op = bc_make_operand_from_value(d, &local_vt, vid, func,
                                                    ret_ty ? ret_ty : func->ret_type);
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
                uint32_t op_num = 0;
                uint32_t lhs_vid = 0;
                uint32_t rhs_vid = 0;
                uint32_t opc;
                lr_operand_t ops[2];
                lr_type_t *res_type;
                uint32_t dest;
                lr_inst_t *inst;
                bool is_fp;
                lr_type_t *lhs_ty = NULL;

                /* LLVM canonical: [lhs pair, rhs, opcode, flags?] */
                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &lhs_vid, &lhs_ty) ||
                    !bc_record_get_value(d, r->record, r->record_len,
                                         &op_num, next_value_id, &rhs_vid) ||
                    op_num >= r->record_len) {
                    bc_dec_error(d, "malformed binop record");
                    ok = false;
                    break;
                }
                opc = (uint32_t)r->record[op_num++];
                ops[0] = bc_make_operand_from_value(d, &local_vt, lhs_vid, func, lhs_ty);
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
                uint32_t op_num = 0;
                uint32_t src_vid = 0;
                uint32_t dest_ty_idx;
                uint32_t cast_opc;
                lr_operand_t op;
                lr_type_t *dest_type;
                lr_type_t *src_type = NULL;
                uint32_t dest;
                lr_inst_t *inst;

                /* LLVM canonical: [op pair, dest_ty, cast_opc, flags?] */
                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &src_vid, &src_type) ||
                    op_num + 1 >= r->record_len) {
                    bc_dec_error(d, "malformed cast record");
                    ok = false;
                    break;
                }
                dest_ty_idx = (uint32_t)r->record[op_num++];
                cast_opc = (uint32_t)r->record[op_num++];
                op = bc_make_operand_from_value(d, &local_vt, src_vid, func, src_type);
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
            case FUNC_CODE_INST_CMP:
            case FUNC_CODE_INST_CMP2: {
                uint32_t lhs_vid = 0;
                uint32_t rhs_vid = 0;
                uint32_t pred = 0;
                lr_operand_t ops[2];
                uint32_t dest;
                lr_inst_t *inst;
                lr_type_t *lhs_ty = NULL;
                uint32_t op_num = 0;

                /* Mirror LLVM BitcodeReader:
                   CMP/CMP2: [opty, opval, opval, pred, ...optional flags] via
                   getValueTypePair + popValue semantics. */
                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &lhs_vid, &lhs_ty) ||
                    !bc_record_get_value(d, r->record, r->record_len,
                                         &op_num, next_value_id, &rhs_vid) ||
                    op_num >= r->record_len) {
                    bc_dec_error(d, "malformed cmp record");
                    ok = false;
                    break;
                }
                pred = (uint32_t)r->record[op_num++];
                ops[0] = bc_make_operand_from_value(d, &local_vt, lhs_vid, func, lhs_ty);
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
                        lr_icmp_pred_t ipred_fallback;
                        if (bc_map_icmp_pred(pred, &ipred_fallback)) {
                            inst = lr_inst_create(d->arena, LR_OP_ICMP,
                                                   d->module->type_i1, dest, ops, 2);
                            if (inst)
                                inst->icmp_pred = ipred_fallback;
                            goto cmp_emit_done;
                        }
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
cmp_emit_done:
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_PHI: {
                uint32_t ty_idx = (uint32_t)r->record[0];
                lr_type_t *phi_type = bc_get_type(d, ty_idx);
                uint32_t payload_len;
                uint32_t npairs;
                lr_operand_t *ops;
                uint32_t dest, j;
                uint32_t phi_base_before_def;
                lr_inst_t *inst;

                if (!phi_type) { ok = false; break; }
                if (r->record_len < 1) {
                    bc_dec_error(d, "malformed phi record");
                    ok = false;
                    break;
                }
                payload_len = r->record_len - 1;
                if ((payload_len & 1u) != 0u) {
                    if (!bc_type_is_fp(phi_type)) {
                        bc_dec_error(d, "invalid phi record");
                        ok = false;
                        break;
                    }
                    payload_len -= 1u; /* trailing FMF */
                }
                npairs = payload_len / 2;
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
                uint32_t inst_ty_idx, op_ty_idx;
                lr_type_t *elem_ty;
                lr_type_t *op_ty;
                lr_operand_t size_op;
                uint32_t dest;
                lr_inst_t *inst;

                if (r->record_len != 4 && r->record_len != 5) {
                    bc_dec_error(d, "malformed alloca record");
                    ok = false;
                    break;
                }

                inst_ty_idx = (uint32_t)r->record[0];
                op_ty_idx = (uint32_t)r->record[1];

                elem_ty = bc_get_type(d, inst_ty_idx);
                if (!elem_ty) { ok = false; break; }
                op_ty = bc_get_type(d, op_ty_idx);
                if (!op_ty)
                    op_ty = d->module->type_i64;

                /* ALLOCA uses absolute value IDs for the size operand. */
                size_op = bc_make_operand_from_value(d, &local_vt, (uint32_t)r->record[2],
                                                     func, op_ty);

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
                uint32_t op_num = 0;
                uint32_t ptr_vid = 0;
                lr_operand_t op;
                lr_type_t *load_ty;
                lr_type_t *ptr_ty = NULL;
                uint32_t dest;
                lr_inst_t *inst;

                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &ptr_vid, &ptr_ty) ||
                    (op_num + 2 != r->record_len && op_num + 3 != r->record_len)) {
                    bc_dec_error(d, "malformed load record");
                    ok = false;
                    break;
                }

                op = bc_make_operand_from_value(d, &local_vt, ptr_vid, func, d->module->type_ptr);
                if (op_num + 3 == r->record_len) {
                    uint32_t ty_idx = (uint32_t)r->record[op_num++];
                    load_ty = bc_get_type(d, ty_idx);
                    if (!load_ty) { ok = false; break; }
                } else {
                    /* Opaque-pointer mode should provide explicit load type. */
                    load_ty = d->module->type_i8;
                }
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
                uint32_t op_num = 0;
                uint32_t ptr_vid = 0;
                uint32_t val_vid = 0;
                lr_type_t *ptr_ty = NULL;
                lr_type_t *val_ty = NULL;
                lr_type_t *implied_val_ty = NULL;

                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &ptr_vid, &ptr_ty)) {
                    ok = false;
                    break;
                }
                (void)ptr_ty;

                if (code == FUNC_CODE_INST_STORE) {
                    if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                       &op_num, next_value_id, &val_vid, &val_ty)) {
                        ok = false;
                        break;
                    }
                } else {
                    if (!implied_val_ty)
                        implied_val_ty = d->module->type_i64;
                    if (!bc_record_get_value(d, r->record, r->record_len,
                                             &op_num, next_value_id, &val_vid)) {
                        ok = false;
                        break;
                    }
                    val_ty = implied_val_ty;
                }
                if (op_num + 2 != r->record_len) {
                    bc_dec_error(d, "malformed store record");
                    ok = false;
                    break;
                }
                ops[0] = bc_make_operand_from_value(d, &local_vt, val_vid, func, val_ty);
                ops[1] = bc_make_operand_from_value(d, &local_vt, ptr_vid,
                                                    func, d->module->type_ptr);
                inst = lr_inst_create(d->arena, LR_OP_STORE,
                                       d->module->type_void, 0, ops, 2);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_VAARG: {
                uint32_t op_num = 0;
                uint32_t valist_ty_idx;
                uint32_t valist_vid = 0;
                uint32_t value_ty_idx;
                lr_type_t *valist_ty;
                lr_type_t *value_ty;
                lr_operand_t valist_addr_op;
                lr_operand_t cursor_op;
                lr_operand_t ptr_i64_op;
                lr_operand_t next_ptr_op;
                uint32_t cursor_vreg;
                uint32_t ptr_i64_vreg;
                uint32_t add_vreg;
                uint32_t next_ptr_vreg;
                uint32_t dest;
                lr_inst_t *inst;
                size_t stride;

                if (r->record_len < 3) {
                    bc_dec_error(d, "malformed va_arg record");
                    ok = false;
                    break;
                }

                valist_ty_idx = (uint32_t)r->record[op_num++];
                valist_ty = bc_get_type(d, valist_ty_idx);
                value_ty_idx = (uint32_t)r->record[op_num + 1];
                value_ty = bc_get_type(d, value_ty_idx);
                if (!value_ty) {
                    ok = false;
                    break;
                }
                if (!bc_record_get_value(d, r->record, r->record_len,
                                         &op_num, next_value_id, &valist_vid)) {
                    ok = false;
                    break;
                }
                op_num++; /* consumed instty */
                valist_addr_op = bc_make_operand_from_value(
                    d, &local_vt, valist_vid, func, valist_ty ? valist_ty : d->module->type_ptr);

                /* Load current vararg cursor pointer from va_list storage. */
                cursor_vreg = lr_vreg_new(func);
                inst = lr_inst_create(d->arena, LR_OP_LOAD, d->module->type_ptr,
                                      cursor_vreg, &valist_addr_op, 1);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                cursor_op = lr_op_vreg(cursor_vreg, d->module->type_ptr);

                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id,
                                          value_ty, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;
                inst = lr_inst_create(d->arena, LR_OP_LOAD, value_ty, dest, &cursor_op, 1);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }

                stride = lr_type_size(value_ty);
                if (stride < 8)
                    stride = 8;
                stride = (stride + 7u) & ~(size_t)7u;

                ptr_i64_vreg = lr_vreg_new(func);
                inst = lr_inst_create(d->arena, LR_OP_PTRTOINT, d->module->type_i64,
                                      ptr_i64_vreg, &cursor_op, 1);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                ptr_i64_op = lr_op_vreg(ptr_i64_vreg, d->module->type_i64);

                {
                    lr_operand_t add_ops[2];
                    add_vreg = lr_vreg_new(func);
                    add_ops[0] = ptr_i64_op;
                    add_ops[1] = lr_op_imm_i64((int64_t)stride, d->module->type_i64);
                    inst = lr_inst_create(d->arena, LR_OP_ADD, d->module->type_i64,
                                          add_vreg, add_ops, 2);
                    if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                        ok = false;
                        break;
                    }
                }
                if (!ok)
                    break;

                ptr_i64_op = lr_op_vreg(add_vreg, d->module->type_i64);
                next_ptr_vreg = lr_vreg_new(func);
                inst = lr_inst_create(d->arena, LR_OP_INTTOPTR, d->module->type_ptr,
                                      next_ptr_vreg, &ptr_i64_op, 1);
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                next_ptr_op = lr_op_vreg(next_ptr_vreg, d->module->type_ptr);

                {
                    lr_operand_t store_ops[2];
                    store_ops[0] = next_ptr_op;
                    store_ops[1] = valist_addr_op;
                    inst = lr_inst_create(d->arena, LR_OP_STORE, d->module->type_void,
                                          0, store_ops, 2);
                    if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                        ok = false;
                        break;
                    }
                }
                break;
            }
            case FUNC_CODE_INST_GEP: {
                uint32_t op_num = 0;
                uint32_t gep_flags;
                uint32_t src_ty_idx = (uint32_t)r->record[1];
                uint32_t base_vid = 0;
                uint32_t nops_total;
                uint32_t j;
                lr_type_t *base_ty;
                lr_operand_t *ops;
                uint32_t dest;
                lr_inst_t *inst;
                lr_type_t *base_ptr_ty = NULL;
                bool parse_ok = true;
                bool use_pair_encoding = true;
                bool retry_value_only = false;

                if (r->record_len < 3) {
                    bc_dec_error(d, "malformed gep record");
                    ok = false;
                    break;
                }
                gep_flags = (uint32_t)r->record[op_num++];
                (void)gep_flags;
                src_ty_idx = (uint32_t)r->record[op_num++];
                base_ty = bc_get_type(d, src_ty_idx);
                if (!base_ty) { ok = false; break; }
                if (!bc_record_get_value_type_pair(d, &local_vt, r->record,
                                                   r->record_len, &op_num,
                                                   next_value_id, &base_vid,
                                                   &base_ptr_ty)) {
                    use_pair_encoding = false;
                    parse_ok = true;
                    if (d->err && d->errlen > 0)
                        d->err[0] = '\0';
                    op_num = 2;
                    if (!bc_record_get_value(d, r->record, r->record_len,
                                             &op_num, next_value_id,
                                             &base_vid)) {
                        ok = false;
                        break;
                    }
                    base_ptr_ty = d->module->type_ptr;
                }

                nops_total = 1u + (r->record_len - op_num);
                ops = (lr_operand_t *)malloc((size_t)nops_total *
                                             sizeof(lr_operand_t));
                if (!ops) {
                    bc_dec_error(d, "out of memory for gep operands");
                    ok = false;
                    break;
                }
                ops[0] = bc_make_operand_from_value(
                    d, &local_vt, base_vid, func,
                    base_ptr_ty ? base_ptr_ty : d->module->type_ptr);

                for (j = 1; j < nops_total; j++) {
                    uint32_t vid = 0;
                    lr_type_t *idx_ty = NULL;
                    if (use_pair_encoding) {
                        if (!bc_record_get_value_type_pair(
                                d, &local_vt, r->record, r->record_len,
                                &op_num, next_value_id, &vid, &idx_ty)) {
                            parse_ok = false;
                            if (d->err &&
                                (strstr(d->err, "missing forward type id") != NULL ||
                                 strstr(d->err, "missing value operand") != NULL)) {
                                retry_value_only = true;
                            }
                            break;
                        }
                    } else {
                        if (!bc_record_get_value(d, r->record, r->record_len,
                                                 &op_num, next_value_id,
                                                 &vid)) {
                            parse_ok = false;
                            break;
                        }
                        if (vid < local_vt.count)
                            idx_ty = local_vt.values[vid].type;
                        if (!idx_ty)
                            idx_ty = d->module->type_i64;
                    }
                    ops[j] = bc_make_operand_from_value(d, &local_vt, vid, func,
                                                        idx_ty);
                    ops[j] = lr_canonicalize_gep_index(d->module,
                                                       blocks[cur_block], func,
                                                       ops[j]);
                }

                if (!parse_ok && retry_value_only) {
                    uint32_t op_num_retry = 2;
                    if (d->err && d->errlen > 0)
                        d->err[0] = '\0';
                    if (!bc_record_get_value(d, r->record, r->record_len,
                                             &op_num_retry, next_value_id,
                                             &base_vid)) {
                        free(ops);
                        ok = false;
                        break;
                    }
                    ops[0] = bc_make_operand_from_value(d, &local_vt, base_vid,
                                                        func,
                                                        d->module->type_ptr);
                    for (j = 1; j < nops_total; j++) {
                        uint32_t vid = 0;
                        lr_type_t *idx_ty = NULL;
                        if (!bc_record_get_value(d, r->record, r->record_len,
                                                 &op_num_retry, next_value_id,
                                                 &vid)) {
                            parse_ok = false;
                            break;
                        }
                        if (vid < local_vt.count)
                            idx_ty = local_vt.values[vid].type;
                        if (!idx_ty)
                            idx_ty = d->module->type_i64;
                        ops[j] = bc_make_operand_from_value(d, &local_vt, vid,
                                                            func, idx_ty);
                        ops[j] = lr_canonicalize_gep_index(d->module,
                                                           blocks[cur_block],
                                                           func, ops[j]);
                    }
                    parse_ok = (j == nops_total);
                }

                if (!parse_ok) {
                    free(ops);
                    ok = false;
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
                uint32_t op_num = 0;
                uint32_t cc_flags;
                uint32_t fn_ty_idx = UINT32_MAX;
                bool explicit_fn_ty = false;
                lr_type_t *fn_type = NULL;
                uint32_t callee_vid = 0;
                lr_type_t *callee_value_ty = NULL;
                lr_type_t *callee_ty = NULL;
                lr_operand_t callee_op;
                uint32_t fixed_args = 0;
                uint32_t nargs = 0;
                uint32_t cap = 0;
                uint32_t i;
                lr_operand_t *ops;
                lr_type_t *ret_type;
                uint32_t dest = 0;
                lr_inst_t *inst;
                lr_func_t *callee_func = NULL;
                bool call_vararg_meta;
                uint32_t call_fixed_args_meta;
                const uint32_t CALL_EXPLICIT_TYPE_BIT = 15u;
                const uint32_t CALL_FMF_BIT = 17u;

                if (r->record_len < 2) {
                    bc_dec_error(d, "malformed call record");
                    ok = false;
                    break;
                }
                op_num++; /* attr */
                cc_flags = (uint32_t)r->record[op_num++];
                if (((cc_flags >> CALL_FMF_BIT) & 1u) != 0u) {
                    if (op_num >= r->record_len) {
                        bc_dec_error(d, "malformed call record");
                        ok = false;
                        break;
                    }
                    op_num++; /* fast-math flags payload */
                }
                explicit_fn_ty = ((cc_flags >> CALL_EXPLICIT_TYPE_BIT) & 1u) != 0u;

                if (explicit_fn_ty) {
                    if (op_num >= r->record_len) {
                        bc_dec_error(d, "malformed call record");
                        ok = false;
                        break;
                    }
                    fn_ty_idx = (uint32_t)r->record[op_num++];
                    fn_type = bc_get_type(d, fn_ty_idx);
                    if (!fn_type || fn_type->kind != LR_TYPE_FUNC) {
                        bc_dec_error(d, "call references non-function type");
                        ok = false;
                        break;
                    }
                }

                if (op_num >= r->record_len) {
                    bc_dec_error(d, "malformed call record");
                    ok = false;
                    break;
                }
                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &callee_vid,
                                                   &callee_ty)) {
                    ok = false;
                    break;
                }
                if (callee_vid < local_vt.count)
                    callee_value_ty = local_vt.values[callee_vid].type;
                callee_op = bc_make_operand_from_value(d, &local_vt, callee_vid,
                                                       func,
                                                       callee_ty ? callee_ty : d->module->type_ptr);

                if (!fn_type) {
                    if (callee_value_ty && callee_value_ty->kind == LR_TYPE_FUNC) {
                        fn_type = callee_value_ty;
                    } else if (callee_vid < local_vt.count &&
                               local_vt.values[callee_vid].kind == BC_VAL_FUNC &&
                               local_vt.values[callee_vid].func &&
                               local_vt.values[callee_vid].func->type &&
                               local_vt.values[callee_vid].func->type->kind == LR_TYPE_FUNC) {
                        fn_type = local_vt.values[callee_vid].func->type;
                    } else {
                        bc_dec_error(d, "call without explicit function type is unsupported");
                        ok = false;
                        break;
                    }
                }
                callee_func = bc_resolve_call_callee_func(d, &local_vt, callee_vid,
                                                          callee_op);
                fixed_args = fn_type->func.num_params;
                if (r->record_len < op_num + fixed_args) {
                    bc_dec_error(d, "insufficient operands in call record");
                    ok = false;
                    break;
                }

                if (bc_call_is_nop_intrinsic(d->module, callee_op))
                    break;

                cap = 1 + fixed_args;
                if (fn_type->func.vararg)
                    cap += (r->record_len - (uint32_t)op_num);
                ops = (lr_operand_t *)malloc((size_t)cap * sizeof(lr_operand_t));
                if (!ops) {
                    bc_dec_error(d, "out of memory for call operands");
                    ok = false;
                    break;
                }
                ops[0] = callee_op;
                for (i = 0; i < fixed_args; i++) {
                    uint32_t arg_vid = 0;
                    lr_type_t *param_ty = fn_type->func.params[i];
                    if (!bc_record_get_value(d, r->record, r->record_len,
                                             &op_num, next_value_id, &arg_vid)) {
                        ok = false;
                        break;
                    }
                    ops[nargs + 1] = bc_make_operand_from_value(d, &local_vt, arg_vid,
                                                                 func, param_ty);
                    nargs++;
                }
                if (!ok) {
                    free(ops);
                    break;
                }

                if (!fn_type->func.vararg && op_num != r->record_len) {
                    free(ops);
                    bc_dec_error(d, "extra operands in non-vararg call");
                    ok = false;
                    break;
                }
                while (fn_type->func.vararg && op_num < r->record_len) {
                    uint32_t arg_vid = 0;
                    lr_type_t *arg_ty = NULL;
                    if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                       &op_num, next_value_id, &arg_vid,
                                                       &arg_ty)) {
                        ok = false;
                        break;
                    }
                    ops[nargs + 1] = bc_make_operand_from_value(d, &local_vt, arg_vid,
                                                                 func, arg_ty);
                    nargs++;
                }
                if (!ok) {
                    free(ops);
                    break;
                }

                ret_type = fn_type->func.ret;
                call_vararg_meta = fn_type->func.vararg;
                call_fixed_args_meta = fixed_args;
                if (callee_func && callee_func->vararg) {
                    call_vararg_meta = true;
                    call_fixed_args_meta = callee_func->num_params;
                }
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
                if (inst) {
                    inst->call_vararg = call_vararg_meta;
                    inst->call_fixed_args = call_fixed_args_meta;
                }
                (void)cc_flags;
                if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                    ok = false;
                    break;
                }
                break;
            }
            case FUNC_CODE_INST_SELECT: {
                uint32_t op_num = 0;
                uint32_t true_vid = 0;
                uint32_t false_vid = 0;
                uint32_t cond_vid = 0;
                lr_operand_t ops[3];
                lr_type_t *res_type;
                uint32_t dest;
                lr_inst_t *inst;
                lr_type_t *true_ty = NULL;

                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &true_vid, &true_ty) ||
                    !bc_record_get_value(d, r->record, r->record_len,
                                         &op_num, next_value_id, &false_vid) ||
                    !bc_record_get_value(d, r->record, r->record_len,
                                         &op_num, next_value_id, &cond_vid) ||
                    op_num != r->record_len) {
                    bc_dec_error(d, "malformed select record");
                    ok = false;
                    break;
                }
                ops[0] = bc_make_operand_from_value(d, &local_vt, cond_vid,
                                                    func, d->module->type_i1);
                ops[1] = bc_make_operand_from_value(d, &local_vt, true_vid, func, true_ty);
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
                uint32_t op_num = 0;
                uint32_t vec_vid = 0;
                uint32_t idx_vid = 0;
                lr_type_t *vec_ty = NULL;
                lr_type_t *idx_ty = NULL;
                lr_type_t *res_ty = d->module->type_i32;
                (void)idx_vid;
                (void)idx_ty;
                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &vec_vid, &vec_ty) ||
                    !bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &idx_vid, &idx_ty)) {
                    bc_dec_error(d, "malformed extractelt record");
                    ok = false;
                    break;
                }
                if (vec_ty && vec_ty->kind == LR_TYPE_VECTOR && vec_ty->array.elem)
                    res_ty = vec_ty->array.elem;
                if (!bc_define_undef_value(d, &local_vt, next_value_id, res_ty)) {
                    ok = false;
                    break;
                }
                next_value_id++;
                break;
            }
            case FUNC_CODE_INST_INSERTELT: {
                uint32_t op_num = 0;
                uint32_t vec_vid = 0, val_vid = 0, idx_vid = 0;
                lr_type_t *vec_ty = NULL;
                lr_type_t *idx_ty = NULL;
                (void)val_vid;
                (void)idx_vid;
                (void)idx_ty;
                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &vec_vid, &vec_ty) ||
                    !bc_record_get_value(d, r->record, r->record_len,
                                         &op_num, next_value_id, &val_vid) ||
                    !bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &idx_vid, &idx_ty)) {
                    bc_dec_error(d, "malformed insertelt record");
                    ok = false;
                    break;
                }
                if (!bc_define_undef_value(d, &local_vt, next_value_id,
                                           vec_ty ? vec_ty : d->module->type_i32)) {
                    ok = false;
                    break;
                }
                next_value_id++;
                break;
            }
            case FUNC_CODE_INST_SHUFFLEVEC: {
                uint32_t op_num = 0;
                uint32_t lhs_vid = 0, rhs_vid = 0, mask_vid = 0;
                lr_type_t *lhs_ty = NULL;
                lr_type_t *mask_ty = NULL;
                (void)rhs_vid;
                (void)mask_vid;
                (void)mask_ty;
                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &lhs_vid, &lhs_ty) ||
                    !bc_record_get_value(d, r->record, r->record_len,
                                         &op_num, next_value_id, &rhs_vid) ||
                    !bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &mask_vid, &mask_ty)) {
                    bc_dec_error(d, "malformed shufflevector record");
                    ok = false;
                    break;
                }
                if (!bc_define_undef_value(d, &local_vt, next_value_id,
                                           lhs_ty ? lhs_ty : d->module->type_i32)) {
                    ok = false;
                    break;
                }
                next_value_id++;
                break;
            }
            case FUNC_CODE_INST_VSELECT: {
                uint32_t op_num = 0;
                uint32_t true_vid = 0;
                uint32_t false_vid = 0;
                uint32_t cond_vid = 0;
                lr_operand_t ops[3];
                lr_type_t *res_type;
                uint32_t dest;
                lr_inst_t *inst;
                lr_type_t *true_ty = NULL;
                lr_type_t *cond_ty = NULL;

                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &true_vid, &true_ty) ||
                    !bc_record_get_value(d, r->record, r->record_len,
                                         &op_num, next_value_id, &false_vid) ||
                    !bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &cond_vid, &cond_ty) ||
                    op_num != r->record_len) {
                    bc_dec_error(d, "malformed vselect record");
                    ok = false;
                    break;
                }
                ops[0] = bc_make_operand_from_value(d, &local_vt, cond_vid, func, cond_ty);
                ops[1] = bc_make_operand_from_value(d, &local_vt, true_vid, func, true_ty);
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
                uint32_t op_num = 0;
                uint32_t agg_vid = 0;
                lr_operand_t op;
                lr_type_t *agg_ty = NULL;
                lr_type_t *res_ty = NULL;
                uint32_t nidx;
                uint32_t *idx_copy = NULL;
                uint32_t dest, j;
                lr_inst_t *inst;

                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &agg_vid, &agg_ty)) {
                    bc_dec_error(d, "malformed extractvalue record");
                    ok = false;
                    break;
                }
                nidx = r->record_len > op_num ? r->record_len - op_num : 0;
                op = bc_make_operand_from_value(d, &local_vt, agg_vid, func, agg_ty);
                res_ty = agg_ty ? agg_ty : d->module->type_i32;
                for (j = 0; j < nidx; j++) {
                    uint32_t idx = (uint32_t)r->record[op_num + j];
                    if (res_ty->kind == LR_TYPE_STRUCT && idx < res_ty->struc.num_fields) {
                        res_ty = res_ty->struc.fields[idx];
                    } else if (res_ty->kind == LR_TYPE_ARRAY) {
                        res_ty = res_ty->array.elem;
                    } else if (res_ty->kind == LR_TYPE_VECTOR) {
                        res_ty = res_ty->array.elem;
                    }
                }
                if (!bc_define_vreg_value(d, &local_vt, func, next_value_id,
                                          res_ty ? res_ty : d->module->type_i32, &dest)) {
                    ok = false;
                    break;
                }
                next_value_id++;

                inst = lr_inst_create(d->arena, LR_OP_EXTRACTVALUE,
                                       res_ty ? res_ty : d->module->type_i32, dest, &op, 1);
                if (inst && nidx > 0) {
                    idx_copy = lr_arena_array(d->arena, uint32_t, nidx);
                    for (j = 0; j < nidx; j++)
                        idx_copy[j] = (uint32_t)r->record[op_num + j];
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
                uint32_t op_num = 0;
                uint32_t agg_vid = 0;
                uint32_t val_vid = 0;
                lr_operand_t ops[2];
                lr_type_t *agg_ty = NULL;
                lr_type_t *val_ty = NULL;
                uint32_t nidx;
                uint32_t *idx_copy = NULL;
                uint32_t dest, j;
                lr_inst_t *inst;

                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &agg_vid, &agg_ty) ||
                    !bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &val_vid, &val_ty)) {
                    bc_dec_error(d, "malformed insertvalue record");
                    ok = false;
                    break;
                }
                nidx = r->record_len > op_num ? r->record_len - op_num : 0;
                ops[0] = bc_make_operand_from_value(d, &local_vt, agg_vid, func, agg_ty);
                ops[1] = bc_make_operand_from_value(d, &local_vt, val_vid,
                                                    func, val_ty);
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
                        idx_copy[j] = (uint32_t)r->record[op_num + j];
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
                uint32_t op_num = 0;
                uint32_t unopc;
                uint32_t src_vid = 0;
                lr_operand_t op;
                lr_type_t *src_ty = NULL;
                uint32_t dest;
                lr_inst_t *inst;

                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &src_vid, &src_ty) ||
                    op_num >= r->record_len) {
                    bc_dec_error(d, "malformed unop record");
                    ok = false;
                    break;
                }
                unopc = (uint32_t)r->record[op_num++];
                op = bc_make_operand_from_value(d, &local_vt, src_vid, func, src_ty);
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
            case FUNC_CODE_INST_FREEZE: {
                uint32_t op_num = 0;
                uint32_t src_vid = 0;
                lr_type_t *freeze_ty = NULL;
                lr_operand_t op;

                if (!bc_record_get_value_type_pair(d, &local_vt, r->record, r->record_len,
                                                   &op_num, next_value_id, &src_vid,
                                                   &freeze_ty) ||
                    op_num != r->record_len) {
                    bc_dec_error(d, "malformed freeze record");
                    ok = false;
                    break;
                }
                op = bc_make_operand_from_value(d, &local_vt, src_vid, func, freeze_ty);
                if (!bc_define_alias_value(d, &local_vt, next_value_id, op, freeze_ty)) {
                    ok = false;
                    break;
                }
                next_value_id++;
                break;
            }
            case FUNC_CODE_INST_SWITCH: {
                uint32_t ty_idx;
                uint32_t cond_rel;
                uint32_t default_bb;
                uint32_t cond_vid = 0;
                uint32_t cases_payload;
                uint32_t num_cases;
                lr_type_t *switch_ty;
                lr_operand_t cond_op;
                lr_block_t *test_block;
                uint32_t ci;
                uint32_t old_switch_pred;
                uint32_t edge_count = 0;
                uint32_t edge_cap = 0;
                uint32_t *edge_targets = NULL;
                uint32_t *edge_preds = NULL;
                uint32_t *target_preds = NULL;

                if (r->record_len < 3) {
                    bc_dec_error(d, "malformed switch record");
                    ok = false;
                    break;
                }
                ty_idx = (uint32_t)r->record[0];
                cond_rel = (uint32_t)r->record[1];
                default_bb = (uint32_t)r->record[2];
                if (default_bb >= num_blocks) {
                    bc_dec_error(d, "switch default block %u out of range (have %u)",
                                 default_bb, num_blocks);
                    ok = false;
                    break;
                }
                switch_ty = bc_get_type(d, ty_idx);
                if (!switch_ty) {
                    ok = false;
                    break;
                }
                if (!bc_resolve_rel_value_id(d, next_value_id, cond_rel, &cond_vid)) {
                    ok = false;
                    break;
                }
                cond_op = bc_make_operand_from_value(d, &local_vt, cond_vid, func, switch_ty);
                cases_payload = r->record_len - 3u;
                if ((cases_payload & 1u) != 0u) {
                    bc_dec_error(d, "malformed switch record payload");
                    ok = false;
                    break;
                }
                num_cases = cases_payload / 2u;
                if (num_cases == 0u) {
                    lr_operand_t op = lr_op_block(default_bb);
                    lr_inst_t *inst = lr_inst_create(d->arena, LR_OP_BR,
                                                     d->module->type_void, 0, &op, 1);
                    if (!bc_emit_inst(d, func, blocks[cur_block], inst)) {
                        ok = false;
                        break;
                    }
                    cur_block++;
                    break;
                }

                old_switch_pred = blocks[cur_block]->id;
                edge_cap = num_cases + 1u; /* each case true-edge + final default edge */
                edge_targets = (uint32_t *)malloc((size_t)edge_cap * sizeof(uint32_t));
                edge_preds = (uint32_t *)malloc((size_t)edge_cap * sizeof(uint32_t));
                target_preds = (uint32_t *)malloc((size_t)edge_cap * sizeof(uint32_t));
                if (!edge_targets || !edge_preds || !target_preds) {
                    free(edge_targets);
                    free(edge_preds);
                    free(target_preds);
                    bc_dec_error(d, "out of memory lowering switch");
                    ok = false;
                    break;
                }

                test_block = blocks[cur_block];
                for (ci = 0; ci < num_cases; ci++) {
                    uint32_t case_val_id = (uint32_t)r->record[3u + (ci * 2u)];
                    uint32_t case_bb = (uint32_t)r->record[4u + (ci * 2u)];
                    uint32_t false_bb = default_bb;
                    uint32_t test_block_id = test_block->id;
                    lr_operand_t case_op;
                    lr_operand_t cmp_ops[2];
                    lr_operand_t br_ops[3];
                    lr_inst_t *icmp;
                    lr_inst_t *brinst;
                    uint32_t cmp_vreg;

                    if (case_bb >= num_blocks) {
                        bc_dec_error(d, "switch case block %u out of range (have %u)",
                                     case_bb, num_blocks);
                        ok = false;
                        break;
                    }
                    if (dbg_switch) {
                        if (case_val_id < local_vt.count &&
                            local_vt.values[case_val_id].kind == BC_VAL_CONST &&
                            local_vt.values[case_val_id].operand.kind == LR_VAL_IMM_I64) {
                            fprintf(stderr,
                                    "bc switch: func=%s base=%u raw_case=%u imm=%lld case_bb=%u default_bb=%u\n",
                                    func && func->name ? func->name : "<anon>",
                                    next_value_id,
                                    (unsigned)case_val_id,
                                    (long long)local_vt.values[case_val_id].operand.imm_i64,
                                    (unsigned)case_bb,
                                    (unsigned)default_bb);
                        } else {
                            fprintf(stderr,
                                    "bc switch: func=%s base=%u raw_case=%u kind=%d case_bb=%u default_bb=%u\n",
                                    func && func->name ? func->name : "<anon>",
                                    next_value_id,
                                    (unsigned)case_val_id,
                                    (case_val_id < local_vt.count)
                                        ? (int)local_vt.values[case_val_id].kind
                                        : -1,
                                    (unsigned)case_bb,
                                    (unsigned)default_bb);
                        }
                    }
                    case_op = bc_make_operand_from_value(d, &local_vt, case_val_id,
                                                         func, switch_ty);

                    if (ci + 1u < num_cases) {
                        char bb_name[32];
                        lr_block_t *next_test;
                        snprintf(bb_name, sizeof(bb_name), "bb.sw.%u", func->num_blocks);
                        next_test = lr_block_create(func, d->arena, bb_name);
                        if (!next_test) {
                            bc_dec_error(d, "out of memory creating switch test block");
                            ok = false;
                            break;
                        }
                        false_bb = next_test->id;
                    }

                    cmp_vreg = lr_vreg_new(func);
                    cmp_ops[0] = cond_op;
                    cmp_ops[1] = case_op;
                    icmp = lr_inst_create(d->arena, LR_OP_ICMP,
                                          d->module->type_i1, cmp_vreg, cmp_ops, 2);
                    if (icmp)
                        icmp->icmp_pred = LR_ICMP_EQ;
                    if (!bc_emit_inst(d, func, test_block, icmp)) {
                        ok = false;
                        break;
                    }

                    br_ops[0] = lr_op_vreg(cmp_vreg, d->module->type_i1);
                    br_ops[1] = lr_op_block(case_bb);
                    br_ops[2] = lr_op_block(false_bb);
                    brinst = lr_inst_create(d->arena, LR_OP_CONDBR,
                                            d->module->type_void, 0, br_ops, 3);
                    if (!bc_emit_inst(d, func, test_block, brinst)) {
                        ok = false;
                        break;
                    }
                    if (edge_count >= edge_cap) {
                        bc_dec_error(d, "internal switch edge accounting overflow");
                        ok = false;
                        break;
                    }
                    edge_targets[edge_count] = case_bb;
                    edge_preds[edge_count] = test_block_id;
                    edge_count++;

                    if (ci + 1u < num_cases)
                        test_block = func->last_block;
                }
                if (!ok) {
                    free(edge_targets);
                    free(edge_preds);
                    free(target_preds);
                    break;
                }

                if (edge_count >= edge_cap) {
                    free(edge_targets);
                    free(edge_preds);
                    free(target_preds);
                    bc_dec_error(d, "internal switch edge accounting overflow");
                    ok = false;
                    break;
                }
                edge_targets[edge_count] = default_bb;
                edge_preds[edge_count] = test_block->id;
                edge_count++;

                for (uint32_t ei = 0; ei < edge_count && ok; ei++) {
                    uint32_t target_bb = edge_targets[ei];
                    uint32_t pred_count = 0;
                    bool seen_target = false;
                    bool has_old_pred = false;

                    for (uint32_t pi = 0; pi < ei; pi++) {
                        if (edge_targets[pi] == target_bb) {
                            seen_target = true;
                            break;
                        }
                    }
                    if (seen_target)
                        continue;

                    for (uint32_t pi = 0; pi < edge_count; pi++) {
                        if (edge_targets[pi] != target_bb)
                            continue;
                        if (!bc_append_unique_u32(target_preds, &pred_count,
                                                  edge_cap, edge_preds[pi])) {
                            bc_dec_error(d, "internal switch predecessor overflow");
                            ok = false;
                            break;
                        }
                        if (edge_preds[pi] == old_switch_pred)
                            has_old_pred = true;
                    }
                    if (!ok)
                        break;

                    if (!has_old_pred) {
                        if (!bc_push_switch_phi_fixup(d, &switch_fixups,
                                                      &switch_fixup_count,
                                                      &switch_fixup_cap,
                                                      target_bb,
                                                      old_switch_pred,
                                                      false,
                                                      target_preds,
                                                      pred_count)) {
                            ok = false;
                            break;
                        }
                    } else if (pred_count > 1u) {
                        uint32_t extra_count = 0;
                        for (uint32_t pi = 0; pi < pred_count; pi++) {
                            if (target_preds[pi] == old_switch_pred)
                                continue;
                            target_preds[extra_count++] = target_preds[pi];
                        }
                        if (extra_count > 0u &&
                            !bc_push_switch_phi_fixup(d, &switch_fixups,
                                                      &switch_fixup_count,
                                                      &switch_fixup_cap,
                                                      target_bb,
                                                      old_switch_pred,
                                                      true,
                                                      target_preds,
                                                      extra_count)) {
                            ok = false;
                            break;
                        }
                    }
                }

                free(edge_targets);
                free(edge_preds);
                free(target_preds);
                if (!ok)
                    break;
                cur_block++;
                break;
            }
            default:
                bc_dec_error(d, "unsupported FUNC record code %u (len=%u)",
                             code, r->record_len);
                ok = false;
                break;
            }

            if (!ok)
                break;
        }
    }

    if (ok && !r->has_error) {
        for (i = 0; i < switch_fixup_count; i++) {
            bc_switch_phi_fixup_t *fx = &switch_fixups[i];
            if (fx->succ_bb >= num_blocks) {
                bc_dec_error(d, "switch phi fixup target block %u out of range",
                             fx->succ_bb);
                ok = false;
                break;
            }
            if (!bc_switch_remap_phi_preds(d, blocks[fx->succ_bb],
                                           fx->old_pred,
                                           fx->new_preds,
                                           fx->new_pred_count,
                                           fx->keep_old_pred)) {
                ok = false;
                break;
            }
        }
    }

    /* Deduplicate PHI predecessor entries.  Switch lowering can
       produce multiple entries for the same predecessor block when
       several case values branch to the same target. */
    if (ok && !r->has_error && num_blocks > 0) {
        for (uint32_t bi = 0; bi < num_blocks; bi++) {
            for (lr_inst_t *inst = blocks[bi]->first; inst;
                 inst = inst->next) {
                if (inst->op != LR_OP_PHI || inst->num_operands < 4)
                    continue;
                uint32_t out = 0;
                for (uint32_t pi = 0; pi + 1 < inst->num_operands;
                     pi += 2) {
                    const lr_operand_t *pred = &inst->operands[pi + 1];
                    bool dup = false;
                    for (uint32_t qi = 0; qi < out; qi += 2) {
                        if (inst->operands[qi + 1].kind == pred->kind &&
                            inst->operands[qi + 1].block_id ==
                                pred->block_id) {
                            dup = true;
                            break;
                        }
                    }
                    if (dup)
                        continue;
                    inst->operands[out] = inst->operands[pi];
                    inst->operands[out + 1] = inst->operands[pi + 1];
                    out += 2;
                }
                inst->num_operands = out;
            }
        }
    }

    /* Fix forward-reference type mismatches.
       When a BINOP/shift references a not-yet-defined vreg, the placeholder
       type from the bitcode record may differ from the final type assigned
       when the vreg is actually defined (e.g., a PHI).  Walk all
       instructions and reconcile operand + instruction types, iterating
       until no more changes (derived types cascade). */
    if (ok && !r->has_error && func && local_vt.count > 0) {
        lr_type_t **vreg_ty = NULL;
        uint32_t nvreg = func->next_vreg;
        if (nvreg > 0)
            vreg_ty = (lr_type_t **)calloc(nvreg, sizeof(lr_type_t *));
        if (vreg_ty) {
            for (uint32_t vi = 0; vi < local_vt.count; vi++) {
                bc_value_t *v = &local_vt.values[vi];
                if (v->kind == BC_VAL_VREG && v->vreg < nvreg && v->type)
                    vreg_ty[v->vreg] = v->type;
            }
            for (uint32_t pass = 0; pass < 4; pass++) {
                bool any_change = false;
                for (uint32_t bi = 0; bi < num_blocks; bi++) {
                    for (lr_inst_t *inst = blocks[bi]->first; inst;
                         inst = inst->next) {
                        for (uint32_t oi = 0; oi < inst->num_operands; oi++) {
                            lr_operand_t *op = &inst->operands[oi];
                            if (op->kind == LR_VAL_VREG && op->vreg < nvreg &&
                                vreg_ty[op->vreg] &&
                                op->type != vreg_ty[op->vreg]) {
                                op->type = vreg_ty[op->vreg];
                                any_change = true;
                            }
                        }
                        /* Propagate corrected operand type to the
                           instruction result for type-inheriting ops
                           (binops/shifts whose result type == operand
                           type). Use a whitelist to avoid corrupting
                           vreg_ty via instructions that lack a dest
                           (store) or whose result type is independent
                           of their first operand (cmp, cast, gep). */
                        lr_opcode_t op = inst->op;
                        bool inherits = (op == LR_OP_ADD ||
                                         op == LR_OP_SUB ||
                                         op == LR_OP_MUL ||
                                         op == LR_OP_SDIV ||
                                         op == LR_OP_UDIV ||
                                         op == LR_OP_SREM ||
                                         op == LR_OP_UREM ||
                                         op == LR_OP_AND ||
                                         op == LR_OP_OR ||
                                         op == LR_OP_XOR ||
                                         op == LR_OP_SHL ||
                                         op == LR_OP_LSHR ||
                                         op == LR_OP_ASHR ||
                                         op == LR_OP_FADD ||
                                         op == LR_OP_FSUB ||
                                         op == LR_OP_FMUL ||
                                         op == LR_OP_FDIV ||
                                         op == LR_OP_FREM ||
                                         op == LR_OP_FNEG);
                        if (inherits && inst->type &&
                            inst->num_operands > 0 &&
                            inst->operands[0].type &&
                            inst->type != inst->operands[0].type) {
                            inst->type = inst->operands[0].type;
                            if (inst->dest < nvreg) {
                                vreg_ty[inst->dest] = inst->type;
                                any_change = true;
                            }
                        }
                    }
                }
                if (!any_change)
                    break;
            }
            free(vreg_ty);
        }
    }

    for (i = 0; i < switch_fixup_count; i++)
        free(switch_fixups[i].new_preds);
    free(switch_fixups);
    free(blocks);
    free(local_vt.values);
    d->cur_func_name = NULL;
    d->cur_func_code = 0;
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
                uint32_t constants_base = d->global_values.count;
                ok = bc_decode_constants_block(d, r, sub_end, &d->global_values);
                if (ok)
                    bc_apply_global_initializers(d, constants_base);
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
                if (!name) {
                    char fallback[32];
                    int n = snprintf(fallback, sizeof(fallback), "unknown.%u",
                                     d->func_list.count);
                    if (n > 0 && (size_t)n < sizeof(fallback))
                        name = lr_arena_strdup(d->arena, fallback, (size_t)n);
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
                /* LLVM bitcode modules use the LLVM-compatible C ABI for all
                   function declarations/definitions. Keep call/param lowering
                   consistent across caller and callee. */
                fn->uses_llvm_abi = true;

                fv.kind = BC_VAL_FUNC;
                fv.type = d->module->type_ptr;
                fv.init_bytes = NULL;
                fv.init_size = 0;
                fv.agg_elem_ids = NULL;
                fv.agg_elem_count = 0;
                fv.func = fn;
                bc_value_push(&d->global_values, fv);
                bc_func_list_push(&d->func_list, fn);
                break;
            }
            case MODULE_CODE_GLOBALVAR: {
                uint32_t strtab_off = 0, strtab_size = 0;
                uint32_t type_idx;
                uint32_t linkage = 0;
                uint32_t init_id = 0;
                lr_type_t *gtype;
                char *gname = NULL;
                lr_global_t *g;
                bc_value_t gv;
                bool is_const, is_external;

                if (d->bc_version >= 2 && r->record_len >= 2) {
                    uint32_t flags = 0;
                    strtab_off = (uint32_t)r->record[0];
                    strtab_size = (uint32_t)r->record[1];
                    type_idx = r->record_len > 2 ? (uint32_t)r->record[2] : 0;
                    flags = r->record_len > 3 ? (uint32_t)r->record[3] : 0;
                    init_id = r->record_len > 4 ? (uint32_t)r->record[4] : 0;
                    linkage = r->record_len > 5 ? (uint32_t)r->record[5] : 0;
                    /* Bit 0 carries "is constant"; higher bits contain
                       extra GLOBALVAR flags (for example explicit type). */
                    is_const = (flags & 1u) != 0u;
                } else {
                    uint32_t isconst_plus1 = 0;
                    type_idx = r->record_len > 0 ? (uint32_t)r->record[0] : 0;
                    isconst_plus1 = r->record_len > 2 ? (uint32_t)r->record[2] : 0;
                    init_id = r->record_len > 3 ? (uint32_t)r->record[3] : 0;
                    linkage = r->record_len > 4 ? (uint32_t)r->record[4] : 0;
                    is_const = (isconst_plus1 > 1);
                }

                gtype = bc_get_type(d, type_idx);
                if (!gtype)
                    gtype = d->module->type_i8;

                if (d->bc_version >= 2 && d->strtab_data && strtab_size > 0 &&
                    (size_t)strtab_off + strtab_size <= d->strtab_len) {
                    gname = lr_arena_strdup(d->arena, (const char *)d->strtab_data + strtab_off,
                                             strtab_size);
                }
                if (!gname) {
                    char fallback[32];
                    int n = snprintf(fallback, sizeof(fallback), "global.%u",
                                     d->global_values.count);
                    if (n > 0 && (size_t)n < sizeof(fallback))
                        gname = lr_arena_strdup(d->arena, fallback, (size_t)n);
                }
                if (!gname)
                    gname = lr_arena_strdup(d->arena, "global", 6);

                is_external = (init_id == 0 && linkage != 8u /* common */);

                g = lr_global_create(d->module, gname, gtype, is_const);
                if (g)
                    g->is_external = is_external;

                lr_frontend_intern_symbol(d->module, gname);

                gv.kind = BC_VAL_GLOBAL;
                gv.type = d->module->type_ptr;
                gv.init_bytes = NULL;
                gv.init_size = 0;
                gv.agg_elem_ids = NULL;
                gv.agg_elem_count = 0;
                gv.global_sym = lr_frontend_intern_symbol(d->module, gname);
                bc_value_push(&d->global_values, gv);
                bc_global_init_ref_push(d, g, init_id);
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
    free(reader.record_is_char6);
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
    free(decoder.global_inits);
    return decoder.module;

cleanup_fail:
    free(reader.record);
    free(reader.record_is_char6);
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
    free(decoder.global_inits);
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
