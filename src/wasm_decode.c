#include "wasm_decode.h"
#include <stdio.h>
#include <string.h>

#define WASM_MAGIC 0x6d736100  /* \0asm in little-endian */
#define WASM_VERSION 1

/* Section IDs */
#define SEC_TYPE     1
#define SEC_IMPORT   2
#define SEC_FUNCTION 3
#define SEC_MEMORY   5
#define SEC_GLOBAL   6
#define SEC_EXPORT   7
#define SEC_CODE    10
#define SEC_DATA    11

/* ---- LEB128 readers ---- */

size_t lr_wasm_read_leb_u32(const uint8_t *buf, size_t len, uint32_t *out) {
    uint32_t result = 0;
    uint32_t shift = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t b = buf[i++];
        result |= (uint32_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) { *out = result; return i; }
        shift += 7;
        if (shift >= 35) return 0;
    }
    return 0;
}

size_t lr_wasm_read_leb_i32(const uint8_t *buf, size_t len, int32_t *out) {
    int32_t result = 0;
    uint32_t shift = 0;
    size_t i = 0;
    uint8_t b = 0;
    while (i < len) {
        b = buf[i++];
        result |= (int32_t)(b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
        if (shift >= 35) return 0;
    }
    if (i == 0) return 0;
    if (shift < 32 && (b & 0x40)) result |= -(1 << shift);
    *out = result;
    return i;
}

size_t lr_wasm_read_leb_i64(const uint8_t *buf, size_t len, int64_t *out) {
    int64_t result = 0;
    uint32_t shift = 0;
    size_t i = 0;
    uint8_t b = 0;
    while (i < len) {
        b = buf[i++];
        result |= (int64_t)(b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
        if (shift >= 70) return 0;
    }
    if (i == 0) return 0;
    if (shift < 64 && (b & 0x40)) result |= -(((int64_t)1) << shift);
    *out = result;
    return i;
}

/* ---- Cursor-based readers ---- */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    char *err;
    size_t errlen;
    bool failed;
} cursor_t;

static uint8_t cur_u8(cursor_t *c) {
    if (c->pos >= c->len) { c->failed = true; return 0; }
    return c->data[c->pos++];
}

static uint32_t cur_u32(cursor_t *c) {
    uint32_t val;
    size_t n = lr_wasm_read_leb_u32(c->data + c->pos, c->len - c->pos, &val);
    if (n == 0) { c->failed = true; return 0; }
    c->pos += n;
    return val;
}

static int32_t cur_i32(cursor_t *c) {
    int32_t val;
    size_t n = lr_wasm_read_leb_i32(c->data + c->pos, c->len - c->pos, &val);
    if (n == 0) { c->failed = true; return 0; }
    c->pos += n;
    return val;
}

static int64_t cur_i64(cursor_t *c) {
    int64_t val;
    size_t n = lr_wasm_read_leb_i64(c->data + c->pos, c->len - c->pos, &val);
    if (n == 0) { c->failed = true; return 0; }
    c->pos += n;
    return val;
}

static uint32_t cur_u32_le(cursor_t *c) {
    if (c->pos + 4 > c->len) { c->failed = true; return 0; }
    uint32_t val = (uint32_t)c->data[c->pos]
                 | ((uint32_t)c->data[c->pos + 1] << 8)
                 | ((uint32_t)c->data[c->pos + 2] << 16)
                 | ((uint32_t)c->data[c->pos + 3] << 24);
    c->pos += 4;
    return val;
}

static const uint8_t *cur_bytes(cursor_t *c, size_t n) {
    if (c->pos + n > c->len) { c->failed = true; return NULL; }
    const uint8_t *p = c->data + c->pos;
    c->pos += n;
    return p;
}

static char *cur_name(cursor_t *c, lr_arena_t *arena) {
    uint32_t len = cur_u32(c);
    if (c->failed) return NULL;
    const uint8_t *bytes = cur_bytes(c, len);
    if (!bytes) return NULL;
    return lr_arena_strdup(arena, (const char *)bytes, len);
}

static void cur_err(cursor_t *c, const char *msg) {
    if (c->err && c->errlen > 0)
        snprintf(c->err, c->errlen, "%s at offset %zu", msg, c->pos);
    c->failed = true;
}

/* ---- Section decoders ---- */

static void decode_type_section(cursor_t *c, lr_wasm_module_t *m,
                                 lr_arena_t *arena) {
    uint32_t count = cur_u32(c);
    if (c->failed) return;
    m->num_types = count;
    m->types = lr_arena_array(arena, lr_wasm_functype_t, count);
    for (uint32_t i = 0; i < count && !c->failed; i++) {
        uint8_t form = cur_u8(c);
        if (form != 0x60) { cur_err(c, "expected functype 0x60"); return; }
        uint32_t np = cur_u32(c);
        m->types[i].num_params = np;
        m->types[i].params = lr_arena_array(arena, uint8_t, np);
        for (uint32_t j = 0; j < np && !c->failed; j++)
            m->types[i].params[j] = cur_u8(c);
        uint32_t nr = cur_u32(c);
        m->types[i].num_results = nr;
        m->types[i].results = lr_arena_array(arena, uint8_t, nr);
        for (uint32_t j = 0; j < nr && !c->failed; j++)
            m->types[i].results[j] = cur_u8(c);
    }
}

static void decode_import_section(cursor_t *c, lr_wasm_module_t *m,
                                   lr_arena_t *arena) {
    uint32_t count = cur_u32(c);
    if (c->failed) return;
    m->num_imports = count;
    m->imports = lr_arena_array(arena, lr_wasm_import_t, count);
    for (uint32_t i = 0; i < count && !c->failed; i++) {
        m->imports[i].module_name = cur_name(c, arena);
        m->imports[i].name = cur_name(c, arena);
        m->imports[i].kind = cur_u8(c);
        switch (m->imports[i].kind) {
        case 0: /* func */
            m->imports[i].type_idx = cur_u32(c);
            m->num_func_imports++;
            break;
        case 1: /* table */
            cur_u8(c); /* elemtype */
            cur_u32(c); /* limits min */
            /* check for max flag */
            break;
        case 2: /* memory */
            { uint8_t flags = cur_u8(c);
              cur_u32(c); /* min */
              if (flags & 1) cur_u32(c); /* max */
            }
            break;
        case 3: /* global */
            cur_u8(c); /* type */
            cur_u8(c); /* mutability */
            break;
        default:
            cur_err(c, "unknown import kind");
            return;
        }
    }
}

static void decode_function_section(cursor_t *c, lr_wasm_module_t *m,
                                     lr_arena_t *arena) {
    uint32_t count = cur_u32(c);
    if (c->failed) return;
    m->num_funcs = count;
    m->func_type_indices = lr_arena_array(arena, uint32_t, count);
    for (uint32_t i = 0; i < count && !c->failed; i++)
        m->func_type_indices[i] = cur_u32(c);
}

static void decode_memory_section(cursor_t *c, lr_wasm_module_t *m,
                                   lr_arena_t *arena) {
    uint32_t count = cur_u32(c);
    if (c->failed) return;
    m->num_memories = count;
    m->memories = lr_arena_array(arena, lr_wasm_memory_t, count);
    for (uint32_t i = 0; i < count && !c->failed; i++) {
        uint8_t flags = cur_u8(c);
        m->memories[i].min_pages = cur_u32(c);
        if (flags & 1) {
            m->memories[i].has_max = true;
            m->memories[i].max_pages = cur_u32(c);
        }
    }
}

static void decode_global_section(cursor_t *c, lr_wasm_module_t *m,
                                   lr_arena_t *arena) {
    uint32_t count = cur_u32(c);
    if (c->failed) return;
    m->num_globals = count;
    m->globals = lr_arena_array(arena, lr_wasm_global_t, count);
    for (uint32_t i = 0; i < count && !c->failed; i++) {
        m->globals[i].type = cur_u8(c);
        m->globals[i].mutable_ = cur_u8(c) != 0;
        /* Simplified init expression: only i32.const or i64.const + end */
        uint8_t op = cur_u8(c);
        if (op == 0x41) /* i32.const */
            m->globals[i].init_i64 = cur_i32(c);
        else if (op == 0x42) /* i64.const */
            m->globals[i].init_i64 = cur_i64(c);
        else
            m->globals[i].init_i64 = 0;
        uint8_t end = cur_u8(c);
        if (end != 0x0B) { cur_err(c, "expected end in init expr"); return; }
    }
}

static void decode_export_section(cursor_t *c, lr_wasm_module_t *m,
                                   lr_arena_t *arena) {
    uint32_t count = cur_u32(c);
    if (c->failed) return;
    m->num_exports = count;
    m->exports = lr_arena_array(arena, lr_wasm_export_t, count);
    for (uint32_t i = 0; i < count && !c->failed; i++) {
        m->exports[i].name = cur_name(c, arena);
        m->exports[i].kind = cur_u8(c);
        m->exports[i].index = cur_u32(c);
    }
}

static void decode_code_section(cursor_t *c, lr_wasm_module_t *m,
                                 lr_arena_t *arena) {
    uint32_t count = cur_u32(c);
    if (c->failed) return;
    m->num_codes = count;
    m->codes = lr_arena_array(arena, lr_wasm_code_t, count);
    for (uint32_t i = 0; i < count && !c->failed; i++) {
        uint32_t body_size = cur_u32(c);
        size_t body_start = c->pos;
        uint32_t num_local_groups = cur_u32(c);
        m->codes[i].num_local_groups = num_local_groups;
        m->codes[i].local_groups = lr_arena_array(arena, lr_wasm_local_group_t,
                                                    num_local_groups);
        for (uint32_t j = 0; j < num_local_groups && !c->failed; j++) {
            m->codes[i].local_groups[j].count = cur_u32(c);
            m->codes[i].local_groups[j].type = cur_u8(c);
        }
        /* Body is the rest of this code entry (pointer into original data) */
        size_t locals_size = c->pos - body_start;
        m->codes[i].body = c->data + c->pos;
        m->codes[i].body_len = body_size - locals_size;
        c->pos = body_start + body_size;
    }
}

static void decode_data_section(cursor_t *c, lr_wasm_module_t *m,
                                 lr_arena_t *arena) {
    uint32_t count = cur_u32(c);
    if (c->failed) return;
    m->num_data = count;
    m->data = lr_arena_array(arena, lr_wasm_data_t, count);
    for (uint32_t i = 0; i < count && !c->failed; i++) {
        uint32_t seg_flags = cur_u32(c);
        m->data[i].memory_idx = (seg_flags & 2) ? cur_u32(c) : 0;
        if (!(seg_flags & 1)) {
            /* Active segment: offset init expression */
            uint8_t op = cur_u8(c);
            if (op == 0x41) /* i32.const */
                m->data[i].offset = (uint32_t)cur_i32(c);
            else
                m->data[i].offset = 0;
            uint8_t end = cur_u8(c);
            if (end != 0x0B) { cur_err(c, "expected end in data offset"); return; }
        }
        uint32_t size = cur_u32(c);
        m->data[i].size = size;
        m->data[i].bytes = cur_bytes(c, size);
    }
}

/* ---- Main decoder ---- */

lr_wasm_module_t *lr_wasm_decode(const uint8_t *data, size_t len,
                                  lr_arena_t *arena, char *err, size_t errlen) {
    cursor_t c = { .data = data, .len = len, .pos = 0,
                   .err = err, .errlen = errlen, .failed = false };

    uint32_t magic = cur_u32_le(&c);
    if (c.failed || magic != WASM_MAGIC) {
        cur_err(&c, "invalid WASM magic");
        return NULL;
    }
    uint32_t version = cur_u32_le(&c);
    if (c.failed || version != WASM_VERSION) {
        cur_err(&c, "unsupported WASM version");
        return NULL;
    }

    lr_wasm_module_t *m = lr_arena_new(arena, lr_wasm_module_t);
    m->arena = arena;

    while (c.pos < c.len && !c.failed) {
        uint8_t sec_id = cur_u8(&c);
        uint32_t sec_len = cur_u32(&c);
        if (c.failed) break;

        size_t sec_end = c.pos + sec_len;
        cursor_t sc = { .data = c.data, .len = sec_end, .pos = c.pos,
                        .err = err, .errlen = errlen, .failed = false };

        switch (sec_id) {
        case SEC_TYPE:     decode_type_section(&sc, m, arena); break;
        case SEC_IMPORT:   decode_import_section(&sc, m, arena); break;
        case SEC_FUNCTION: decode_function_section(&sc, m, arena); break;
        case SEC_MEMORY:   decode_memory_section(&sc, m, arena); break;
        case SEC_GLOBAL:   decode_global_section(&sc, m, arena); break;
        case SEC_EXPORT:   decode_export_section(&sc, m, arena); break;
        case SEC_CODE:     decode_code_section(&sc, m, arena); break;
        case SEC_DATA:     decode_data_section(&sc, m, arena); break;
        default: break; /* skip unknown sections */
        }

        if (sc.failed) return NULL;
        c.pos = sec_end;
    }

    if (c.failed) return NULL;
    return m;
}
