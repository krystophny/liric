#include "ll_parser.h"
#include "frontend_common.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define VREG_MAP_INIT_CAP 4096u
#define BLOCK_MAP_INIT_CAP 1024u
#define GLOBAL_MAP_INIT_CAP 1024u
#define FUNC_MAP_INIT_CAP 256u
#define TYPE_MAP_INIT_CAP 256u

#define VREG_INDEX_INIT_CAP 8192u
#define BLOCK_INDEX_INIT_CAP 2048u
#define GLOBAL_INDEX_INIT_CAP 2048u
#define VREG_NUMERIC_INIT_CAP 4096u

#define INDEX_MAX_LOAD_NUM 7u
#define INDEX_MAX_LOAD_DEN 10u

typedef struct {
    char *name;
    size_t name_len;
    uint32_t id;
    uint32_t hash;
} vreg_map_entry_t;

typedef struct {
    char *name;
    size_t name_len;
    uint32_t id;
    lr_block_t *block;
    uint32_t hash;
} block_map_entry_t;

typedef struct {
    char *name;
    size_t name_len;
    uint32_t id;
    uint32_t hash;
} global_map_entry_t;

typedef struct {
    char *name;
    lr_func_t *func;
} func_map_entry_t;

typedef struct {
    char *name;
    lr_type_t *type;
    bool placeholder;
} type_map_entry_t;

typedef struct lr_parser {
    lr_lexer_t lex;
    lr_token_t cur;
    lr_token_t prev;
    lr_arena_t *arena;
    lr_module_t *module;
    char *err;
    size_t errlen;
    bool had_error;

    /* vreg name -> id mapping for current function */
    vreg_map_entry_t *vreg_map;
    uint32_t vreg_map_count;
    uint32_t vreg_map_cap;
    int32_t *vreg_index;
    uint32_t vreg_index_cap;
    uint32_t *vreg_numeric;
    uint32_t vreg_numeric_cap;

    /* block name -> id mapping for current function */
    block_map_entry_t *block_map;
    uint32_t block_map_count;
    uint32_t block_map_cap;
    int32_t *block_index;
    uint32_t block_index_cap;

    /* global/function symbol name -> id mapping */
    global_map_entry_t *global_map;
    uint32_t global_map_count;
    uint32_t global_map_cap;
    int32_t *global_index;
    uint32_t global_index_cap;

    /* function name -> func mapping */
    func_map_entry_t *func_map;
    uint32_t func_map_count;
    uint32_t func_map_cap;

    /* named type alias mapping (e.g. %string_descriptor -> struct type) */
    type_map_entry_t *type_map;
    uint32_t type_map_count;
    uint32_t type_map_cap;

    lr_func_t *cur_func;
} lr_parser_t;

static void error(lr_parser_t *p, const char *fmt, ...) {
    if (p->had_error) return;
    p->had_error = true;
    if (p->err && p->errlen > 0) {
        uint32_t line = 0, col = 0;
        va_list ap;
        va_start(ap, fmt);
        int n = 0;
        if (p->cur.start) {
            lr_lexer_compute_loc(&p->lex, p->cur.start, &line, &col);
            n = snprintf(p->err, p->errlen, "line %u col %u: ", line, col);
        }
        if (n > 0 && (size_t)n < p->errlen)
            vsnprintf(p->err + n, p->errlen - n, fmt, ap);
        else if (n == 0)
            vsnprintf(p->err, p->errlen, fmt, ap);
        va_end(ap);
    }
}

static void next(lr_parser_t *p) {
    p->prev = p->cur;
    p->cur = lr_lexer_next(&p->lex);
}

static bool check(lr_parser_t *p, lr_tok_t kind) {
    return p->cur.kind == kind;
}

static bool match(lr_parser_t *p, lr_tok_t kind) {
    if (p->cur.kind == kind) { next(p); return true; }
    return false;
}

static void expect(lr_parser_t *p, lr_tok_t kind) {
    if (!match(p, kind))
        error(p, "expected '%s', got '%s'", lr_tok_name(kind), lr_tok_name(p->cur.kind));
}

/* Extract name from %name or @name token (skip the prefix character) */
typedef struct {
    const char *s;
    size_t len;
} name_view_t;

static name_view_t tok_name_view(const lr_token_t *t) {
    const char *s = t->start;
    size_t len = t->len;
    /* skip % or @ prefix */
    if (len > 0 && (s[0] == '%' || s[0] == '@')) {
        s++;
        len--;
    }
    /* skip quotes if present */
    if (len >= 2 && s[0] == '"' && s[len-1] == '"') {
        s++;
        len -= 2;
    }
    return (name_view_t){ s, len };
}

/* Extract name from %name or @name token (skip the prefix character) */
static char *tok_name(lr_parser_t *p, const lr_token_t *t) {
    name_view_t nv = tok_name_view(t);
    return lr_arena_strdup(p->arena, nv.s, nv.len);
}

static uint32_t hash_name_n(const char *name, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t hash_name(const char *name) {
    return hash_name_n(name, strlen(name));
}

static bool parse_u32_decimal_n(const char *s, size_t len, uint32_t *out) {
    uint32_t v = 0;

    if (len == 0)
        return false;

    for (size_t i = 0; i < len; i++) {
        unsigned d = (unsigned)((unsigned char)s[i] - (unsigned char)'0');
        if (d > 9u)
            return false;
        if (v > (UINT32_MAX - d) / 10u)
            return false;
        v = v * 10u + d;
    }

    *out = v;
    return true;
}

static void clear_index(int32_t *index, size_t n) {
    for (size_t i = 0; i < n; i++)
        index[i] = -1;
}

static void clear_u32_map(uint32_t *map, size_t n) {
    for (size_t i = 0; i < n; i++)
        map[i] = UINT32_MAX;
}

static uint32_t next_pow2_u32(uint32_t v) {
    uint32_t p2 = 1u;
    if (v <= 1u)
        return 1u;
    while (p2 < v && p2 <= (UINT32_MAX / 2u))
        p2 <<= 1u;
    return p2 >= v ? p2 : UINT32_MAX;
}

static bool ensure_array_capacity(lr_parser_t *p, void **arr, uint32_t *cap,
                                  uint32_t min_cap, uint32_t init_cap,
                                  size_t elem_size, const char *what) {
    uint32_t new_cap;
    void *new_arr;
    if (*cap >= min_cap)
        return true;
    new_cap = (*cap == 0) ? init_cap : *cap;
    while (new_cap < min_cap) {
        if (new_cap > (UINT32_MAX / 2u)) {
            error(p, "%s capacity overflow", what);
            return false;
        }
        new_cap <<= 1u;
    }
    new_arr = realloc(*arr, (size_t)new_cap * elem_size);
    if (!new_arr) {
        error(p, "out of memory growing %s to %u entries", what, new_cap);
        return false;
    }
    *arr = new_arr;
    *cap = new_cap;
    return true;
}

static bool ensure_vreg_map_capacity(lr_parser_t *p, uint32_t min_cap) {
    return ensure_array_capacity(p, (void **)&p->vreg_map, &p->vreg_map_cap, min_cap,
                                 VREG_MAP_INIT_CAP, sizeof(*p->vreg_map), "vreg map");
}

static bool ensure_block_map_capacity(lr_parser_t *p, uint32_t min_cap) {
    return ensure_array_capacity(p, (void **)&p->block_map, &p->block_map_cap, min_cap,
                                 BLOCK_MAP_INIT_CAP, sizeof(*p->block_map), "block map");
}

static bool ensure_global_map_capacity(lr_parser_t *p, uint32_t min_cap) {
    return ensure_array_capacity(p, (void **)&p->global_map, &p->global_map_cap, min_cap,
                                 GLOBAL_MAP_INIT_CAP, sizeof(*p->global_map), "global map");
}

static bool ensure_func_map_capacity(lr_parser_t *p, uint32_t min_cap) {
    return ensure_array_capacity(p, (void **)&p->func_map, &p->func_map_cap, min_cap,
                                 FUNC_MAP_INIT_CAP, sizeof(*p->func_map), "function map");
}

static bool ensure_type_map_capacity(lr_parser_t *p, uint32_t min_cap) {
    return ensure_array_capacity(p, (void **)&p->type_map, &p->type_map_cap, min_cap,
                                 TYPE_MAP_INIT_CAP, sizeof(*p->type_map), "type map");
}

static bool ensure_vreg_numeric_capacity(lr_parser_t *p, uint32_t min_cap) {
    uint32_t old_cap = p->vreg_numeric_cap;
    if (!ensure_array_capacity(p, (void **)&p->vreg_numeric, &p->vreg_numeric_cap, min_cap,
                               VREG_NUMERIC_INIT_CAP, sizeof(*p->vreg_numeric),
                               "vreg numeric map")) {
        return false;
    }
    for (uint32_t i = old_cap; i < p->vreg_numeric_cap; i++)
        p->vreg_numeric[i] = UINT32_MAX;
    return true;
}

static bool rehash_vreg_index(lr_parser_t *p, uint32_t min_cap) {
    uint32_t cap = next_pow2_u32(min_cap < 16u ? 16u : min_cap);
    int32_t *idx;
    uint32_t mask;
    if (cap == UINT32_MAX) {
        error(p, "vreg index capacity overflow");
        return false;
    }
    idx = malloc((size_t)cap * sizeof(*idx));
    if (!idx) {
        error(p, "out of memory growing vreg index to %u entries", cap);
        return false;
    }
    clear_index(idx, cap);
    mask = cap - 1u;
    for (uint32_t i = 0; i < p->vreg_map_count; i++) {
        uint32_t slot = p->vreg_map[i].hash & mask;
        while (idx[slot] >= 0)
            slot = (slot + 1u) & mask;
        idx[slot] = (int32_t)i;
    }
    free(p->vreg_index);
    p->vreg_index = idx;
    p->vreg_index_cap = cap;
    return true;
}

static bool rehash_block_index(lr_parser_t *p, uint32_t min_cap) {
    uint32_t cap = next_pow2_u32(min_cap < 16u ? 16u : min_cap);
    int32_t *idx;
    uint32_t mask;
    if (cap == UINT32_MAX) {
        error(p, "block index capacity overflow");
        return false;
    }
    idx = malloc((size_t)cap * sizeof(*idx));
    if (!idx) {
        error(p, "out of memory growing block index to %u entries", cap);
        return false;
    }
    clear_index(idx, cap);
    mask = cap - 1u;
    for (uint32_t i = 0; i < p->block_map_count; i++) {
        uint32_t slot = p->block_map[i].hash & mask;
        while (idx[slot] >= 0)
            slot = (slot + 1u) & mask;
        idx[slot] = (int32_t)i;
    }
    free(p->block_index);
    p->block_index = idx;
    p->block_index_cap = cap;
    return true;
}

static bool rehash_global_index(lr_parser_t *p, uint32_t min_cap) {
    uint32_t cap = next_pow2_u32(min_cap < 16u ? 16u : min_cap);
    int32_t *idx;
    uint32_t mask;
    if (cap == UINT32_MAX) {
        error(p, "global index capacity overflow");
        return false;
    }
    idx = malloc((size_t)cap * sizeof(*idx));
    if (!idx) {
        error(p, "out of memory growing global index to %u entries", cap);
        return false;
    }
    clear_index(idx, cap);
    mask = cap - 1u;
    for (uint32_t i = 0; i < p->global_map_count; i++) {
        uint32_t slot = p->global_map[i].hash & mask;
        while (idx[slot] >= 0)
            slot = (slot + 1u) & mask;
        idx[slot] = (int32_t)i;
    }
    free(p->global_index);
    p->global_index = idx;
    p->global_index_cap = cap;
    return true;
}

static bool ensure_vreg_index_room(lr_parser_t *p, uint32_t next_count) {
    uint32_t need = p->vreg_index_cap ? p->vreg_index_cap : VREG_INDEX_INIT_CAP;
    while ((uint64_t)next_count * INDEX_MAX_LOAD_DEN >=
           (uint64_t)need * INDEX_MAX_LOAD_NUM) {
        if (need > (UINT32_MAX / 2u)) {
            error(p, "vreg index capacity overflow");
            return false;
        }
        need <<= 1u;
    }
    if (p->vreg_index_cap >= need)
        return true;
    return rehash_vreg_index(p, need);
}

static bool ensure_block_index_room(lr_parser_t *p, uint32_t next_count) {
    uint32_t need = p->block_index_cap ? p->block_index_cap : BLOCK_INDEX_INIT_CAP;
    while ((uint64_t)next_count * INDEX_MAX_LOAD_DEN >=
           (uint64_t)need * INDEX_MAX_LOAD_NUM) {
        if (need > (UINT32_MAX / 2u)) {
            error(p, "block index capacity overflow");
            return false;
        }
        need <<= 1u;
    }
    if (p->block_index_cap >= need)
        return true;
    return rehash_block_index(p, need);
}

static bool ensure_global_index_room(lr_parser_t *p, uint32_t next_count) {
    uint32_t need = p->global_index_cap ? p->global_index_cap : GLOBAL_INDEX_INIT_CAP;
    while ((uint64_t)next_count * INDEX_MAX_LOAD_DEN >=
           (uint64_t)need * INDEX_MAX_LOAD_NUM) {
        if (need > (UINT32_MAX / 2u)) {
            error(p, "global index capacity overflow");
            return false;
        }
        need <<= 1u;
    }
    if (p->global_index_cap >= need)
        return true;
    return rehash_global_index(p, need);
}

static bool register_vreg_numeric_slot(lr_parser_t *p, uint32_t numeric, uint32_t id) {
    if (!ensure_vreg_numeric_capacity(p, numeric + 1u))
        return false;
    p->vreg_numeric[numeric] = id;
    return true;
}

static uint32_t index_find_vreg_n(const lr_parser_t *p, const char *name, size_t name_len, uint32_t hash) {
    uint32_t slot = hash & (p->vreg_index_cap - 1u);
    for (;;) {
        int32_t idx = p->vreg_index[slot];
        if (idx < 0)
            return UINT32_MAX;
        if (p->vreg_map[idx].hash == hash &&
                p->vreg_map[idx].name_len == name_len &&
                memcmp(p->vreg_map[idx].name, name, name_len) == 0)
            return (uint32_t)idx;
        slot = (slot + 1u) & (p->vreg_index_cap - 1u);
    }
}

static void index_insert_vreg(lr_parser_t *p, uint32_t idx) {
    uint32_t slot = p->vreg_map[idx].hash & (p->vreg_index_cap - 1u);
    while (p->vreg_index[slot] >= 0)
        slot = (slot + 1u) & (p->vreg_index_cap - 1u);
    p->vreg_index[slot] = (int32_t)idx;
}

static uint32_t index_find_block_n(const lr_parser_t *p, const char *name, size_t name_len, uint32_t hash) {
    uint32_t slot = hash & (p->block_index_cap - 1u);
    for (;;) {
        int32_t idx = p->block_index[slot];
        if (idx < 0)
            return UINT32_MAX;
        if (p->block_map[idx].hash == hash &&
                p->block_map[idx].name_len == name_len &&
                memcmp(p->block_map[idx].name, name, name_len) == 0)
            return (uint32_t)idx;
        slot = (slot + 1u) & (p->block_index_cap - 1u);
    }
}

static void index_insert_block(lr_parser_t *p, uint32_t idx) {
    uint32_t slot = p->block_map[idx].hash & (p->block_index_cap - 1u);
    while (p->block_index[slot] >= 0)
        slot = (slot + 1u) & (p->block_index_cap - 1u);
    p->block_index[slot] = (int32_t)idx;
}

static uint32_t index_find_global_n(const lr_parser_t *p, const char *name, size_t name_len, uint32_t hash) {
    uint32_t slot = hash & (p->global_index_cap - 1u);
    for (;;) {
        int32_t idx = p->global_index[slot];
        if (idx < 0)
            return UINT32_MAX;
        if (p->global_map[idx].hash == hash &&
                p->global_map[idx].name_len == name_len &&
                memcmp(p->global_map[idx].name, name, name_len) == 0)
            return (uint32_t)idx;
        slot = (slot + 1u) & (p->global_index_cap - 1u);
    }
}

static void index_insert_global(lr_parser_t *p, uint32_t idx) {
    uint32_t slot = p->global_map[idx].hash & (p->global_index_cap - 1u);
    while (p->global_index[slot] >= 0)
        slot = (slot + 1u) & (p->global_index_cap - 1u);
    p->global_index[slot] = (int32_t)idx;
}

static bool parser_init_work_buffers(lr_parser_t *p) {
    if (!ensure_vreg_map_capacity(p, VREG_MAP_INIT_CAP))
        return false;
    if (!ensure_block_map_capacity(p, BLOCK_MAP_INIT_CAP))
        return false;
    if (!ensure_global_map_capacity(p, GLOBAL_MAP_INIT_CAP))
        return false;
    if (!ensure_func_map_capacity(p, FUNC_MAP_INIT_CAP))
        return false;
    if (!ensure_type_map_capacity(p, TYPE_MAP_INIT_CAP))
        return false;
    if (!ensure_vreg_numeric_capacity(p, VREG_NUMERIC_INIT_CAP))
        return false;
    if (!rehash_vreg_index(p, VREG_INDEX_INIT_CAP))
        return false;
    if (!rehash_block_index(p, BLOCK_INDEX_INIT_CAP))
        return false;
    if (!rehash_global_index(p, GLOBAL_INDEX_INIT_CAP))
        return false;

    clear_index(p->vreg_index, p->vreg_index_cap);
    clear_index(p->block_index, p->block_index_cap);
    clear_index(p->global_index, p->global_index_cap);
    clear_u32_map(p->vreg_numeric, p->vreg_numeric_cap);
    return true;
}

static void parser_free_work_buffers(lr_parser_t *p) {
    free(p->vreg_map);
    free(p->vreg_index);
    free(p->vreg_numeric);
    free(p->block_map);
    free(p->block_index);
    free(p->global_map);
    free(p->global_index);
    free(p->func_map);
    free(p->type_map);
}

static void register_vreg_name(lr_parser_t *p, char *name, uint32_t id) {
    uint32_t hash;
    uint32_t idx;
    size_t name_len;
    uint32_t numeric_name;

    if (!ensure_vreg_map_capacity(p, p->vreg_map_count + 1u))
        return;
    if (!ensure_vreg_index_room(p, p->vreg_map_count + 1u))
        return;

    name_len = strlen(name);
    hash = hash_name(name);
    idx = p->vreg_map_count++;
    p->vreg_map[idx].name = name;
    p->vreg_map[idx].name_len = name_len;
    p->vreg_map[idx].id = id;
    p->vreg_map[idx].hash = hash;
    index_insert_vreg(p, idx);

    if (parse_u32_decimal_n(name, name_len, &numeric_name))
        (void)register_vreg_numeric_slot(p, numeric_name, id);
}

static void register_vreg_number(lr_parser_t *p, uint32_t number, uint32_t id) {
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%u", number);
    if (len <= 0 || (size_t)len >= sizeof(buf)) {
        error(p, "vreg number formatting failed");
        return;
    }
    register_vreg_name(p, lr_arena_strdup(p->arena, buf, (size_t)len), id);
}

static uint32_t resolve_vreg_n(lr_parser_t *p, const char *name, size_t name_len) {
    uint32_t numeric_name;
    if (parse_u32_decimal_n(name, name_len, &numeric_name)) {
        if (!ensure_vreg_numeric_capacity(p, numeric_name + 1u))
            return 0;
        if (p->vreg_numeric[numeric_name] != UINT32_MAX)
            return p->vreg_numeric[numeric_name];
        /* auto-create common numeric vreg names without hash probe */
        uint32_t id = lr_vreg_new(p->cur_func);
        register_vreg_number(p, numeric_name, id);
        return id;
    }

    uint32_t hash = hash_name_n(name, name_len);
    uint32_t idx = index_find_vreg_n(p, name, name_len, hash);
    if (idx != UINT32_MAX)
        return p->vreg_map[idx].id;

    /* auto-create */
    uint32_t id = lr_vreg_new(p->cur_func);
    register_vreg_name(p, lr_arena_strdup(p->arena, name, name_len), id);
    return id;
}

static uint32_t resolve_block_n(lr_parser_t *p, const char *name, size_t name_len) {
    uint32_t hash = hash_name_n(name, name_len);
    uint32_t idx = index_find_block_n(p, name, name_len, hash);
    if (idx != UINT32_MAX)
        return p->block_map[idx].id;

    /* forward reference: create block */
    char *owned_name = lr_arena_strdup(p->arena, name, name_len);
    lr_block_t *b = lr_block_create(p->cur_func, p->arena, owned_name);
    if (!ensure_block_map_capacity(p, p->block_map_count + 1u))
        return b->id;
    if (!ensure_block_index_room(p, p->block_map_count + 1u))
        return b->id;
    uint32_t ins = p->block_map_count++;
    p->block_map[ins].name = owned_name;
    p->block_map[ins].name_len = name_len;
    p->block_map[ins].id = b->id;
    p->block_map[ins].block = b;
    p->block_map[ins].hash = hash;
    index_insert_block(p, ins);
    return b->id;
}

static lr_block_t *resolve_block_ptr_n(lr_parser_t *p, const char *name, size_t name_len) {
    uint32_t hash = hash_name_n(name, name_len);
    uint32_t idx = index_find_block_n(p, name, name_len, hash);
    if (idx != UINT32_MAX)
        return p->block_map[idx].block;

    char *owned_name = lr_arena_strdup(p->arena, name, name_len);
    lr_block_t *b = lr_block_create(p->cur_func, p->arena, owned_name);
    if (!ensure_block_map_capacity(p, p->block_map_count + 1u))
        return b;
    if (!ensure_block_index_room(p, p->block_map_count + 1u))
        return b;
    uint32_t ins = p->block_map_count++;
    p->block_map[ins].name = owned_name;
    p->block_map[ins].name_len = name_len;
    p->block_map[ins].id = b->id;
    p->block_map[ins].block = b;
    p->block_map[ins].hash = hash;
    index_insert_block(p, ins);
    return b;
}

static lr_block_t *resolve_block_ptr(lr_parser_t *p, const char *name) {
    return resolve_block_ptr_n(p, name, strlen(name));
}

static uint32_t resolve_global_n(lr_parser_t *p, const char *name, size_t name_len) {
    uint32_t hash = hash_name_n(name, name_len);
    uint32_t idx = index_find_global_n(p, name, name_len, hash);
    return idx == UINT32_MAX ? UINT32_MAX : p->global_map[idx].id;
}

static uint32_t resolve_global(lr_parser_t *p, const char *name) {
    return resolve_global_n(p, name, strlen(name));
}

static void register_global_n(lr_parser_t *p, const char *name, size_t name_len, uint32_t id) {
    if (!ensure_global_map_capacity(p, p->global_map_count + 1u))
        return;
    if (!ensure_global_index_room(p, p->global_map_count + 1u))
        return;
    uint32_t idx = p->global_map_count++;
    p->global_map[idx].name = lr_arena_strdup(p->arena, name, name_len);
    p->global_map[idx].name_len = name_len;
    p->global_map[idx].id = id;
    p->global_map[idx].hash = hash_name_n(name, name_len);
    index_insert_global(p, idx);
}

static void register_global(lr_parser_t *p, const char *name, uint32_t id) {
    register_global_n(p, name, strlen(name), id);
}

static void register_func(lr_parser_t *p, const char *name, lr_func_t *f) {
    if (!ensure_func_map_capacity(p, p->func_map_count + 1u))
        return;
    p->func_map[p->func_map_count].name = lr_arena_strdup(p->arena, name, strlen(name));
    p->func_map[p->func_map_count].func = f;
    p->func_map_count++;
}

static int32_t find_type_index_n(lr_parser_t *p, const char *name, size_t name_len) {
    for (uint32_t i = 0; i < p->type_map_count; i++) {
        if (strlen(p->type_map[i].name) == name_len &&
                memcmp(p->type_map[i].name, name, name_len) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void register_type(lr_parser_t *p, const char *name, lr_type_t *ty) {
    int32_t idx = find_type_index_n(p, name, strlen(name));
    if (idx >= 0) {
        if (p->type_map[idx].placeholder) {
            lr_type_t *placeholder = p->type_map[idx].type;
            *placeholder = *ty;
            if (placeholder->kind == LR_TYPE_STRUCT && !placeholder->struc.name)
                placeholder->struc.name = p->type_map[idx].name;
            p->type_map[idx].type = placeholder;
            p->type_map[idx].placeholder = false;
        } else {
            p->type_map[idx].type = ty;
        }
        return;
    }

    if (!ensure_type_map_capacity(p, p->type_map_count + 1u))
        return;
    p->type_map[p->type_map_count].name = lr_arena_strdup(p->arena, name, strlen(name));
    p->type_map[p->type_map_count].type = ty;
    p->type_map[p->type_map_count].placeholder = false;
    p->type_map_count++;
}

static lr_type_t *resolve_type(lr_parser_t *p, const char *name) {
    int32_t idx = find_type_index_n(p, name, strlen(name));
    return idx >= 0 ? p->type_map[idx].type : NULL;
}

static lr_type_t *resolve_or_create_forward_type(lr_parser_t *p, const char *name) {
    int32_t idx = find_type_index_n(p, name, strlen(name));
    if (idx >= 0)
        return p->type_map[idx].type;

    lr_type_t *placeholder = lr_arena_new(p->arena, lr_type_t);
    placeholder->kind = LR_TYPE_STRUCT;
    placeholder->struc.fields = NULL;
    placeholder->struc.num_fields = 0;
    placeholder->struc.packed = false;
    placeholder->struc.name = lr_arena_strdup(p->arena, name, strlen(name));

    if (!ensure_type_map_capacity(p, p->type_map_count + 1u))
        return placeholder;
    p->type_map[p->type_map_count].name =
        lr_arena_strdup(p->arena, name, strlen(name));
    p->type_map[p->type_map_count].type = placeholder;
    p->type_map[p->type_map_count].placeholder = true;
    p->type_map_count++;

    return placeholder;
}

static lr_type_t *parse_type(lr_parser_t *p);
static lr_operand_t parse_typed_operand(lr_parser_t *p);

static lr_type_t *call_result_type(lr_type_t *ty, bool *is_vararg_out,
                                   uint32_t *fixed_args_out) {
    if (is_vararg_out)
        *is_vararg_out = false;
    if (fixed_args_out)
        *fixed_args_out = 0;
    if (ty && ty->kind == LR_TYPE_FUNC) {
        if (is_vararg_out)
            *is_vararg_out = ty->func.vararg;
        if (fixed_args_out)
            *fixed_args_out = ty->func.num_params;
        if (ty->func.ret)
            return ty->func.ret;
    }
    return ty;
}
static void skip_balanced_parens(lr_parser_t *p);
static void skip_balanced_braces(lr_parser_t *p);
static void skip_balanced_brackets(lr_parser_t *p);

static lr_type_t *parse_type(lr_parser_t *p) {
    lr_token_t t = p->cur;
    lr_type_t *ty = NULL;
    switch (t.kind) {
    case LR_TOK_VOID:   next(p); ty = p->module->type_void; break;
    case LR_TOK_I1:     next(p); ty = p->module->type_i1; break;
    case LR_TOK_I8:     next(p); ty = p->module->type_i8; break;
    case LR_TOK_I16:    next(p); ty = p->module->type_i16; break;
    case LR_TOK_I32:    next(p); ty = p->module->type_i32; break;
    case LR_TOK_I64:    next(p); ty = p->module->type_i64; break;
    case LR_TOK_FLOAT:  next(p); ty = p->module->type_float; break;
    case LR_TOK_DOUBLE: next(p); ty = p->module->type_double; break;
    case LR_TOK_PTR:    next(p); ty = p->module->type_ptr; break;
    case LR_TOK_LOCAL_ID: {
        char *tname = tok_name(p, &p->cur);
        next(p);
        lr_type_t *resolved = resolve_type(p, tname);
        ty = resolved ? resolved : resolve_or_create_forward_type(p, tname);
        break;
    }
    case LR_TOK_LBRACKET: {
        next(p);
        int64_t count = p->cur.int_val;
        expect(p, LR_TOK_INT_LIT);
        expect(p, LR_TOK_X);
        lr_type_t *elem = parse_type(p);
        expect(p, LR_TOK_RBRACKET);
        ty = lr_type_array(p->arena, elem, count);
        break;
    }
    case LR_TOK_LBRACE: {
        next(p);
        lr_type_t *fields[256];
        uint32_t nf = 0;
        if (!check(p, LR_TOK_RBRACE)) {
            fields[nf++] = parse_type(p);
            while (match(p, LR_TOK_COMMA))
                fields[nf++] = parse_type(p);
        }
        expect(p, LR_TOK_RBRACE);
        ty = lr_type_struct(p->arena, fields, nf, false, NULL);
        break;
    }
    case LR_TOK_LANGLE: {
        next(p);
        if (check(p, LR_TOK_INT_LIT)) {
            /* Vector type: <N x T> */
            int64_t count = p->cur.int_val;
            expect(p, LR_TOK_INT_LIT);
            expect(p, LR_TOK_X);
            lr_type_t *elem = parse_type(p);
            expect(p, LR_TOK_RANGLE);
            ty = lr_type_array(p->arena, elem, count);
        } else {
            /* Packed struct: <{ ... }> */
            expect(p, LR_TOK_LBRACE);
            lr_type_t *fields[256];
            uint32_t nf = 0;
            if (!check(p, LR_TOK_RBRACE)) {
                fields[nf++] = parse_type(p);
                while (match(p, LR_TOK_COMMA))
                    fields[nf++] = parse_type(p);
            }
            expect(p, LR_TOK_RBRACE);
            expect(p, LR_TOK_RANGLE);
            ty = lr_type_struct(p->arena, fields, nf, true, NULL);
        }
        break;
    }
    default:
        error(p, "expected type, got '%s'", lr_tok_name(t.kind));
        ty = p->module->type_void;
        break;
    }

    /* Handle type suffixes: pointers and function types.
     * Examples: i8*, i8**, i32 (i64)*, i8* (i32)* */
    while (true) {
        if (match(p, LR_TOK_STAR)) {
            /* Typed pointer suffix or pointer to function */
            ty = p->module->type_ptr;
        } else if (check(p, LR_TOK_LPAREN)) {
            /* Function type: RetType (ParamTypes...)
             * Note: ty is the return type at this point */
            next(p);
            lr_type_t *ret = ty;
            lr_type_t *params[256];
            uint32_t nparams = 0;
            bool vararg = false;

            if (!check(p, LR_TOK_RPAREN)) {
                if (check(p, LR_TOK_DOTDOTDOT)) {
                    vararg = true;
                    next(p);
                } else {
                    params[nparams++] = parse_type(p);
                    while (match(p, LR_TOK_COMMA)) {
                        if (check(p, LR_TOK_DOTDOTDOT)) {
                            vararg = true;
                            next(p);
                            break;
                        }
                        params[nparams++] = parse_type(p);
                    }
                }
            }
            expect(p, LR_TOK_RPAREN);
            ty = lr_type_func(p->arena, ret, params, nparams, vararg);
        } else {
            break;
        }
    }

    return ty;
}

static bool is_bare_identifier(const lr_token_t *tok) {
    if (tok->kind != LR_TOK_LOCAL_ID || tok->len == 0)
        return false;
    return tok->start[0] != '%' && tok->start[0] != '@';
}

static void skip_attr_payload(lr_parser_t *p) {
    if (!check(p, LR_TOK_LPAREN))
        return;
    skip_balanced_parens(p);
}

/* Skip attribute annotations we don't care about */
static void skip_attrs(lr_parser_t *p) {
    while (true) {
        if (p->cur.kind == LR_TOK_NSW || p->cur.kind == LR_TOK_NUW ||
            p->cur.kind == LR_TOK_INBOUNDS || p->cur.kind == LR_TOK_NONNULL ||
            p->cur.kind == LR_TOK_NOUNDEF || p->cur.kind == LR_TOK_SIGNEXT ||
            p->cur.kind == LR_TOK_ZEROEXT || p->cur.kind == LR_TOK_NOCAPTURE ||
            p->cur.kind == LR_TOK_READONLY || p->cur.kind == LR_TOK_WRITEONLY ||
            p->cur.kind == LR_TOK_NNAN || p->cur.kind == LR_TOK_NINF ||
            p->cur.kind == LR_TOK_NSZ || p->cur.kind == LR_TOK_DSOLOCAL ||
            p->cur.kind == LR_TOK_LINKONCE_ODR ||
            p->cur.kind == LR_TOK_EXTERNAL || p->cur.kind == LR_TOK_INTERNAL ||
            p->cur.kind == LR_TOK_PRIVATE || p->cur.kind == LR_TOK_COMMON ||
            p->cur.kind == LR_TOK_UNNAMED_ADDR ||
            p->cur.kind == LR_TOK_LOCAL_UNNAMED_ADDR ||
            p->cur.kind == LR_TOK_ATTR_GROUP || p->cur.kind == LR_TOK_METADATA_ID) {
            next(p);
            continue;
        }
        if (p->cur.kind == LR_TOK_ALIGN) {
            next(p);
            if (check(p, LR_TOK_INT_LIT))
                next(p);
            continue;
        }
        if (is_bare_identifier(&p->cur)) {
            next(p);
            skip_attr_payload(p);
            continue;
        }
        break;
    }
}

static bool token_equals(const lr_token_t *tok, const char *s) {
    size_t n = strlen(s);
    if (!tok || tok->len != n)
        return false;
    return memcmp(tok->start, s, n) == 0;
}

static void skip_memory_qualifiers(lr_parser_t *p) {
    while (check(p, LR_TOK_LOCAL_ID)) {
        if (token_equals(&p->cur, "volatile") ||
            token_equals(&p->cur, "atomic")) {
            next(p);
            continue;
        }
        break;
    }
}

static lr_operand_t parse_const_gep_operand(lr_parser_t *p, lr_type_t *result_ty) {
    bool wrapped = false;
    bool offset_ok = true;
    int64_t byte_offset = 0;
    expect(p, LR_TOK_GETELEMENTPTR);
    skip_attrs(p);
    if (match(p, LR_TOK_LPAREN))
        wrapped = true;
    lr_type_t *source_ty = parse_type(p);
    lr_type_t *cur_ty = source_ty;
    uint32_t idx_pos = 0;
    expect(p, LR_TOK_COMMA);

    lr_operand_t base = parse_typed_operand(p);
    while (match(p, LR_TOK_COMMA)) {
        lr_operand_t idx_op = parse_typed_operand(p);
        int64_t idx = 0;
        if (offset_ok && idx_op.kind == LR_VAL_IMM_I64) {
            idx = idx_op.imm_i64;
        } else {
            offset_ok = false;
        }

        if (!offset_ok)
            continue;

        if (idx_pos == 0) {
            byte_offset += idx * (int64_t)lr_type_size(cur_ty);
        } else if (cur_ty->kind == LR_TYPE_ARRAY) {
            byte_offset += idx * (int64_t)lr_type_size(cur_ty->array.elem);
            cur_ty = cur_ty->array.elem;
        } else if (cur_ty->kind == LR_TYPE_STRUCT) {
            if (idx < 0 || (uint64_t)idx >= cur_ty->struc.num_fields) {
                offset_ok = false;
            } else {
                byte_offset += (int64_t)lr_struct_field_offset(cur_ty, (uint32_t)idx);
                cur_ty = cur_ty->struc.fields[(uint32_t)idx];
            }
        } else {
            byte_offset += idx * (int64_t)lr_type_size(cur_ty);
        }

        idx_pos++;
    }
    if (wrapped)
        expect(p, LR_TOK_RPAREN);

    if (base.kind == LR_VAL_GLOBAL) {
        lr_operand_t out = lr_op_global(base.global_id, result_ty);
        if (offset_ok)
            out.global_offset = base.global_offset + byte_offset;
        else
            out.global_offset = base.global_offset;
        return out;
    }
    if (base.kind == LR_VAL_VREG)
        return lr_op_vreg(base.vreg, result_ty);
    if (base.kind == LR_VAL_NULL)
        return lr_op_null(result_ty);
    return lr_op_null(result_ty);
}

static lr_operand_t parse_operand(lr_parser_t *p, lr_type_t *type);

static void pack_scalar_bits(uint8_t *buf, size_t offset, lr_type_t *ft,
                             lr_operand_t *op) {
    size_t fsz = lr_type_size(ft);
    if (op->kind == LR_VAL_IMM_I64) {
        int64_t v = op->imm_i64;
        memcpy(buf + offset, &v, fsz < 8 ? fsz : 8);
    } else if (op->kind == LR_VAL_IMM_F64) {
        if (ft->kind == LR_TYPE_FLOAT) {
            float f = (float)op->imm_f64;
            memcpy(buf + offset, &f, 4);
        } else {
            double d = op->imm_f64;
            memcpy(buf + offset, &d, 8);
        }
    } else {
        memset(buf + offset, 0, fsz);
    }
}

static lr_operand_t parse_struct_constant_fields(lr_parser_t *p,
                                                 lr_type_t *type,
                                                 lr_operand_t *field_ops,
                                                 uint32_t *nfields_out) {
    bool packed = check(p, LR_TOK_LANGLE);
    if (packed)
        next(p);
    expect(p, LR_TOK_LBRACE);

    uint32_t nf = 0;
    bool have_type = type && type->kind == LR_TYPE_STRUCT;
    uint32_t max_fields = have_type ? type->struc.num_fields : 0;

    while (!check(p, LR_TOK_RBRACE) && !check(p, LR_TOK_EOF)) {
        lr_operand_t fop = parse_typed_operand(p);
        if (field_ops && nf < max_fields)
            field_ops[nf] = fop;
        nf++;
        if (!match(p, LR_TOK_COMMA))
            break;
    }
    expect(p, LR_TOK_RBRACE);
    if (packed)
        expect(p, LR_TOK_RANGLE);

    if (nfields_out)
        *nfields_out = nf;

    if (!have_type || nf != max_fields) {
        return (lr_operand_t){ .kind = LR_VAL_UNDEF, .type = type };
    }

    size_t total = lr_type_size(type);
    if (total <= 8) {
        uint8_t buf[8] = {0};
        for (uint32_t i = 0; i < nf; i++) {
            size_t off = lr_struct_field_offset(type, i);
            pack_scalar_bits(buf, off, type->struc.fields[i], &field_ops[i]);
        }
        int64_t packed_val = 0;
        memcpy(&packed_val, buf, total);
        return lr_op_imm_i64(packed_val, type);
    }

    return (lr_operand_t){ .kind = LR_VAL_UNDEF, .type = type };
}

#define AGG_FIELDS_MAX 16u

static lr_operand_t parse_aggregate_constant_operand(lr_parser_t *p, lr_type_t *type) {
    if (check(p, LR_TOK_LBRACE) && type && type->kind == LR_TYPE_STRUCT) {
        lr_operand_t fields[AGG_FIELDS_MAX];
        uint32_t nf = 0;
        return parse_struct_constant_fields(p, type, fields, &nf);
    }
    if (check(p, LR_TOK_LANGLE)) {
        next(p);
        skip_balanced_braces(p);
        if (check(p, LR_TOK_RANGLE))
            next(p);
    } else if (check(p, LR_TOK_LBRACE)) {
        skip_balanced_braces(p);
    } else {
        skip_balanced_brackets(p);
    }
    return (lr_operand_t){ .kind = LR_VAL_UNDEF, .type = type };
}

static lr_operand_t parse_operand(lr_parser_t *p, lr_type_t *type) {
    if (check(p, LR_TOK_INT_LIT)) {
        int64_t val = p->cur.int_val;
        next(p);
        return lr_op_imm_i64(val, type);
    }
    if (check(p, LR_TOK_FLOAT_LIT)) {
        double val = p->cur.float_val;
        next(p);
        return lr_op_imm_f64(val, type);
    }
    if (check(p, LR_TOK_TRUE)) {
        next(p);
        return lr_op_imm_i64(1, type);
    }
    if (check(p, LR_TOK_FALSE)) {
        next(p);
        return lr_op_imm_i64(0, type);
    }
    if (check(p, LR_TOK_NULL)) {
        next(p);
        return lr_op_null(type);
    }
    if (check(p, LR_TOK_UNDEF)) {
        next(p);
        return (lr_operand_t){ .kind = LR_VAL_UNDEF, .type = type };
    }
    if (check(p, LR_TOK_ZEROINITIALIZER)) {
        next(p);
        return lr_op_imm_i64(0, type);
    }
    if (check(p, LR_TOK_STRING_LIT)) {
        next(p);
        return lr_op_null(type);
    }
    if (check(p, LR_TOK_LOCAL_ID)) {
        name_view_t name = tok_name_view(&p->cur);
        next(p);
        uint32_t vreg = resolve_vreg_n(p, name.s, name.len);
        return lr_op_vreg(vreg, type);
    }
    if (check(p, LR_TOK_GLOBAL_ID)) {
        name_view_t name = tok_name_view(&p->cur);
        next(p);
        uint32_t gid = resolve_global_n(p, name.s, name.len);
        if (gid == UINT32_MAX) {
            char *owned = lr_arena_strdup(p->arena, name.s, name.len);
            gid = lr_frontend_intern_symbol(p->module, owned);
            register_global_n(p, owned, name.len, gid);
        }
        return lr_op_global(gid, type);
    }
    if (check(p, LR_TOK_GETELEMENTPTR))
        return parse_const_gep_operand(p, type);
    if (check(p, LR_TOK_BITCAST) || check(p, LR_TOK_INTTOPTR) ||
        check(p, LR_TOK_PTRTOINT) || check(p, LR_TOK_SEXT) ||
        check(p, LR_TOK_ZEXT) || check(p, LR_TOK_TRUNC) ||
        check(p, LR_TOK_SITOFP) || check(p, LR_TOK_UITOFP) ||
        check(p, LR_TOK_FPTOSI) || check(p, LR_TOK_FPTOUI) ||
        check(p, LR_TOK_FPEXT) || check(p, LR_TOK_FPTRUNC)) {
        next(p);
        expect(p, LR_TOK_LPAREN);
        lr_operand_t src = parse_typed_operand(p);
        expect(p, LR_TOK_TO);
        (void)parse_type(p);
        expect(p, LR_TOK_RPAREN);
        src.type = type;
        return src;
    }
    if (check(p, LR_TOK_LBRACE) || check(p, LR_TOK_LBRACKET))
        return parse_aggregate_constant_operand(p, type);
    if (check(p, LR_TOK_LANGLE)) {
        if (type && type->kind == LR_TYPE_STRUCT) {
            lr_operand_t fields[AGG_FIELDS_MAX];
            uint32_t nf = 0;
            return parse_struct_constant_fields(p, type, fields, &nf);
        }
        next(p);
        if (!check(p, LR_TOK_LBRACE)) {
            error(p, "expected '{' after '<' in packed struct literal");
            return lr_op_imm_i64(0, type);
        }
        skip_balanced_braces(p);
        expect(p, LR_TOK_RANGLE);
        return (lr_operand_t){ .kind = LR_VAL_UNDEF, .type = type };
    }
    error(p, "expected operand, got '%s'", lr_tok_name(p->cur.kind));
    return lr_op_imm_i64(0, type);
}

/* Parse a typed operand: type value */
static lr_operand_t parse_typed_operand(lr_parser_t *p) {
    lr_type_t *t = parse_type(p);
    skip_attrs(p);
    return parse_operand(p, t);
}

static void skip_balanced_parens(lr_parser_t *p) {
    uint32_t depth = 0;
    expect(p, LR_TOK_LPAREN);
    depth = 1;
    while (depth > 0 && !check(p, LR_TOK_EOF)) {
        if (match(p, LR_TOK_LPAREN)) {
            depth++;
            continue;
        }
        if (match(p, LR_TOK_RPAREN)) {
            depth--;
            continue;
        }
        next(p);
    }
    if (depth != 0)
        error(p, "unterminated parenthesized type in call");
}

static void skip_balanced_braces(lr_parser_t *p) {
    uint32_t depth = 0;
    expect(p, LR_TOK_LBRACE);
    depth = 1;
    while (depth > 0 && !check(p, LR_TOK_EOF)) {
        if (match(p, LR_TOK_LBRACE)) {
            depth++;
            continue;
        }
        if (match(p, LR_TOK_RBRACE)) {
            depth--;
            continue;
        }
        next(p);
    }
    if (depth != 0)
        error(p, "unterminated aggregate constant");
}

static void skip_balanced_brackets(lr_parser_t *p) {
    uint32_t depth = 0;
    expect(p, LR_TOK_LBRACKET);
    depth = 1;
    while (depth > 0 && !check(p, LR_TOK_EOF)) {
        if (match(p, LR_TOK_LBRACKET)) {
            depth++;
            continue;
        }
        if (match(p, LR_TOK_RBRACKET)) {
            depth--;
            continue;
        }
        next(p);
    }
    if (depth != 0)
        error(p, "unterminated array constant");
}

static bool skip_optional_callee_signature(lr_parser_t *p,
                                           bool *has_vararg_out,
                                           uint32_t *num_params_out) {
    bool has_vararg = false;
    uint32_t num_params = 0;
    bool in_param = false;
    /*
     * Accept typed callee signatures like:
     *   call ptr (ptr, i64, ...) @foo(...)
     *   call i32 (i32)* @fn(i32 1)
     */
    if (has_vararg_out)
        *has_vararg_out = false;
    if (num_params_out)
        *num_params_out = 0;
    if (check(p, LR_TOK_LPAREN)) {
        uint32_t depth = 0;
        expect(p, LR_TOK_LPAREN);
        depth = 1;
        while (depth > 0 && !check(p, LR_TOK_EOF)) {
            if (match(p, LR_TOK_LPAREN)) {
                depth++;
                continue;
            }
            if (match(p, LR_TOK_RPAREN)) {
                if (depth == 1 && in_param) {
                    num_params++;
                    in_param = false;
                }
                depth--;
                continue;
            }
            if (depth == 1 && check(p, LR_TOK_DOTDOTDOT)) {
                has_vararg = true;
                in_param = false;
                next(p);
                continue;
            }
            if (depth == 1 && check(p, LR_TOK_COMMA)) {
                if (in_param)
                    num_params++;
                in_param = false;
                next(p);
                continue;
            }
            if (depth == 1)
                in_param = true;
            next(p);
        }
        if (depth != 0)
            error(p, "unterminated parenthesized type in call");
        while (match(p, LR_TOK_STAR)) {}
        skip_attrs(p);
        if (has_vararg_out)
            *has_vararg_out = has_vararg;
        if (num_params_out)
            *num_params_out = num_params;
        return true;
    }
    return false;
}

static bool is_integer_type(const lr_type_t *ty) {
    if (!ty) return false;
    return ty->kind == LR_TYPE_I1 || ty->kind == LR_TYPE_I8 ||
           ty->kind == LR_TYPE_I16 || ty->kind == LR_TYPE_I32 ||
           ty->kind == LR_TYPE_I64;
}

static lr_operand_t canonicalize_gep_index_operand(lr_parser_t *p,
                                                    lr_func_t *func,
                                                    lr_block_t *block,
                                                    const lr_operand_t *op) {
    lr_operand_t out = *op;
    if (!is_integer_type(op->type))
        return out;

    if (op->kind == LR_VAL_IMM_I64) {
        out.type = p->module->type_i64;
        return out;
    }

    if (op->kind == LR_VAL_VREG && op->type->kind != LR_TYPE_I64) {
        uint32_t tmp_vreg = lr_vreg_new(func);
        lr_operand_t cast_ops[1] = {*op};
        lr_inst_t *cast = lr_inst_create(p->arena, LR_OP_SEXT,
                                         p->module->type_i64,
                                         tmp_vreg, cast_ops, 1);
        lr_block_append(block, cast);
        return lr_op_vreg(tmp_vreg, p->module->type_i64);
    }

    out.type = p->module->type_i64;
    return out;
}

static void parse_instruction(lr_parser_t *p, lr_func_t *func, lr_block_t *block) {
    /* Check for label: */
    if (check(p, LR_TOK_LOCAL_ID)) {
        /* Could be: %x = ... or a label. Peek ahead for = */
        lr_token_t saved = p->cur;
        next(p);
        if (check(p, LR_TOK_EQUALS)) {
            /* %x = instruction */
            next(p);
            name_view_t dest_name = tok_name_view(&saved);
            uint32_t dest = resolve_vreg_n(p, dest_name.s, dest_name.len);

            lr_tok_t op_tok = p->cur.kind;
            next(p);
            skip_attrs(p);

            switch (op_tok) {
            case LR_TOK_ADD: case LR_TOK_SUB: case LR_TOK_MUL:
            case LR_TOK_SDIV: case LR_TOK_SREM: case LR_TOK_UREM:
            case LR_TOK_AND: case LR_TOK_OR: case LR_TOK_XOR:
            case LR_TOK_SHL: case LR_TOK_LSHR: case LR_TOK_ASHR:
            case LR_TOK_FADD: case LR_TOK_FSUB:
            case LR_TOK_FMUL: case LR_TOK_FDIV: {
                skip_attrs(p);
                lr_type_t *ty = parse_type(p);
                skip_attrs(p);
                lr_operand_t lhs = parse_operand(p, ty);
                expect(p, LR_TOK_COMMA);
                lr_operand_t rhs = parse_operand(p, ty);

                lr_opcode_t irop;
                switch (op_tok) {
                case LR_TOK_ADD:  irop = LR_OP_ADD; break;
                case LR_TOK_SUB:  irop = LR_OP_SUB; break;
                case LR_TOK_MUL:  irop = LR_OP_MUL; break;
                case LR_TOK_SDIV: irop = LR_OP_SDIV; break;
                case LR_TOK_SREM: irop = LR_OP_SREM; break;
                case LR_TOK_UREM: irop = LR_OP_SREM; break;
                case LR_TOK_AND:  irop = LR_OP_AND; break;
                case LR_TOK_OR:   irop = LR_OP_OR; break;
                case LR_TOK_XOR:  irop = LR_OP_XOR; break;
                case LR_TOK_SHL:  irop = LR_OP_SHL; break;
                case LR_TOK_LSHR: irop = LR_OP_LSHR; break;
                case LR_TOK_ASHR: irop = LR_OP_ASHR; break;
                case LR_TOK_FADD: irop = LR_OP_FADD; break;
                case LR_TOK_FSUB: irop = LR_OP_FSUB; break;
                case LR_TOK_FMUL: irop = LR_OP_FMUL; break;
                case LR_TOK_FDIV: irop = LR_OP_FDIV; break;
                default: irop = LR_OP_ADD; break;
                }

                lr_operand_t ops[2] = {lhs, rhs};
                lr_inst_t *inst = lr_inst_create(p->arena, irop, ty, dest, ops, 2);
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_ICMP: {
                lr_icmp_pred_t pred;
                switch (p->cur.kind) {
                case LR_TOK_EQ:  pred = LR_ICMP_EQ; break;
                case LR_TOK_NE:  pred = LR_ICMP_NE; break;
                case LR_TOK_SGT: pred = LR_ICMP_SGT; break;
                case LR_TOK_SGE: pred = LR_ICMP_SGE; break;
                case LR_TOK_SLT: pred = LR_ICMP_SLT; break;
                case LR_TOK_SLE: pred = LR_ICMP_SLE; break;
                case LR_TOK_UGT: pred = LR_ICMP_UGT; break;
                case LR_TOK_UGE: pred = LR_ICMP_UGE; break;
                case LR_TOK_ULT: pred = LR_ICMP_ULT; break;
                case LR_TOK_ULE: pred = LR_ICMP_ULE; break;
                default:
                    error(p, "expected icmp predicate");
                    pred = LR_ICMP_EQ;
                }
                next(p);
                lr_type_t *ty = parse_type(p);
                lr_operand_t lhs = parse_operand(p, ty);
                expect(p, LR_TOK_COMMA);
                lr_operand_t rhs = parse_operand(p, ty);
                lr_operand_t ops[2] = {lhs, rhs};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_ICMP,
                    p->module->type_i1, dest, ops, 2);
                inst->icmp_pred = pred;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_ALLOCA: {
                lr_type_t *ty = parse_type(p);
                lr_operand_t count_op = {0};
                bool has_count = false;
                /* check for optional count: ", <inttype> <operand>" */
                if (match(p, LR_TOK_COMMA)) {
                    if (check(p, LR_TOK_ALIGN)) {
                        /* just align, no count */
                        next(p); next(p);
                    } else {
                        /* parse count operand */
                        lr_type_t *count_ty = parse_type(p);
                        count_op = parse_operand(p, count_ty);
                        has_count = true;
                        /* check for optional ", align N" after count */
                        if (match(p, LR_TOK_COMMA)) {
                            if (check(p, LR_TOK_ALIGN)) { next(p); next(p); }
                        }
                    }
                }
                lr_inst_t *inst;
                if (has_count) {
                    lr_operand_t ops[1] = {count_op};
                    inst = lr_inst_create(p->arena, LR_OP_ALLOCA,
                        p->module->type_ptr, dest, ops, 1);
                } else {
                    inst = lr_inst_create(p->arena, LR_OP_ALLOCA,
                        p->module->type_ptr, dest, NULL, 0);
                }
                inst->type = ty;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_LOAD: {
                skip_memory_qualifiers(p);
                lr_type_t *ty = parse_type(p);
                expect(p, LR_TOK_COMMA);
                lr_operand_t src = parse_typed_operand(p);
                /* skip optional ", align N" */
                if (match(p, LR_TOK_COMMA)) {
                    if (check(p, LR_TOK_ALIGN)) { next(p); next(p); }
                }
                lr_operand_t ops[1] = {src};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_LOAD,
                    ty, dest, ops, 1);
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_CALL: {
                bool call_sig_vararg = false;
                uint32_t call_sig_fixed = 0;
                bool sig_vararg = false;
                uint32_t sig_fixed = 0;
                lr_type_t *ret_ty = call_result_type(parse_type(p),
                                                     &call_sig_vararg,
                                                     &call_sig_fixed);
                skip_attrs(p);
                if (skip_optional_callee_signature(p, &sig_vararg, &sig_fixed)) {
                    call_sig_vararg = call_sig_vararg || sig_vararg;
                    call_sig_fixed = sig_fixed;
                }
                lr_operand_t callee = parse_operand(p, p->module->type_ptr);
                expect(p, LR_TOK_LPAREN);
                lr_operand_t args[64];
                uint32_t nargs = 0;
                if (!check(p, LR_TOK_RPAREN)) {
                    args[nargs++] = parse_typed_operand(p);
                    while (match(p, LR_TOK_COMMA)) {
                        skip_attrs(p);
                        args[nargs++] = parse_typed_operand(p);
                    }
                }
                expect(p, LR_TOK_RPAREN);
                /* args[0..nargs-1] are the actual args, callee is separate */
                lr_operand_t all_ops[65];
                all_ops[0] = callee;
                for (uint32_t i = 0; i < nargs; i++)
                    all_ops[i + 1] = args[i];
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_CALL,
                    ret_ty, dest, all_ops, nargs + 1);
                inst->call_vararg = call_sig_vararg;
                inst->call_fixed_args = call_sig_fixed;
                if (callee.kind != LR_VAL_GLOBAL)
                    inst->call_external_abi = true;
                lr_block_append(block, inst);
                /* skip trailing attribute groups */
                skip_attrs(p);
                break;
            }

            case LR_TOK_SEXT: case LR_TOK_ZEXT: case LR_TOK_TRUNC:
            case LR_TOK_BITCAST: case LR_TOK_PTRTOINT: case LR_TOK_INTTOPTR:
            case LR_TOK_SITOFP: case LR_TOK_UITOFP:
            case LR_TOK_FPTOSI: case LR_TOK_FPTOUI:
            case LR_TOK_FPEXT: case LR_TOK_FPTRUNC: {
                lr_operand_t src = parse_typed_operand(p);
                expect(p, LR_TOK_TO);
                lr_type_t *dst_ty = parse_type(p);
                lr_opcode_t irop;
                switch (op_tok) {
                case LR_TOK_SEXT:     irop = LR_OP_SEXT; break;
                case LR_TOK_ZEXT:     irop = LR_OP_ZEXT; break;
                case LR_TOK_TRUNC:    irop = LR_OP_TRUNC; break;
                case LR_TOK_BITCAST:  irop = LR_OP_BITCAST; break;
                case LR_TOK_PTRTOINT: irop = LR_OP_PTRTOINT; break;
                case LR_TOK_INTTOPTR: irop = LR_OP_INTTOPTR; break;
                case LR_TOK_SITOFP:   irop = LR_OP_SITOFP; break;
                case LR_TOK_UITOFP:   irop = LR_OP_UITOFP; break;
                case LR_TOK_FPTOSI:   irop = LR_OP_FPTOSI; break;
                case LR_TOK_FPTOUI:   irop = LR_OP_FPTOUI; break;
                case LR_TOK_FPEXT:    irop = LR_OP_FPEXT; break;
                case LR_TOK_FPTRUNC:  irop = LR_OP_FPTRUNC; break;
                default: irop = LR_OP_BITCAST; break;
                }
                lr_operand_t ops[1] = {src};
                lr_inst_t *inst = lr_inst_create(p->arena, irop,
                    dst_ty, dest, ops, 1);
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_FNEG: {
                lr_operand_t src = parse_typed_operand(p);
                lr_operand_t ops[1] = {src};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_FNEG,
                    src.type, dest, ops, 1);
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_SELECT: {
                lr_operand_t cond = parse_typed_operand(p);
                expect(p, LR_TOK_COMMA);
                lr_operand_t tv = parse_typed_operand(p);
                expect(p, LR_TOK_COMMA);
                lr_operand_t fv = parse_typed_operand(p);
                lr_operand_t ops[3] = {cond, tv, fv};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_SELECT,
                    tv.type, dest, ops, 3);
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_GETELEMENTPTR: {
                skip_attrs(p);
                lr_type_t *base_ty = parse_type(p);
                expect(p, LR_TOK_COMMA);
                lr_operand_t ops[16];
                uint32_t nops = 0;
                ops[nops++] = parse_typed_operand(p);
                while (match(p, LR_TOK_COMMA)) {
                    lr_operand_t idx = parse_typed_operand(p);
                    idx = canonicalize_gep_index_operand(p, func, block, &idx);
                    ops[nops++] = idx;
                }
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_GEP,
                    p->module->type_ptr, dest, ops, nops);
                /* store base type in inst->type for GEP offset computation */
                inst->type = base_ty;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_PHI: {
                lr_type_t *ty = parse_type(p);
                lr_operand_t ops[64];
                uint32_t nops = 0;
                /* [ val, %label ] pairs */
                do {
                    expect(p, LR_TOK_LBRACKET);
                    ops[nops++] = parse_operand(p, ty);
                    expect(p, LR_TOK_COMMA);
                    /* block label */
                    if (check(p, LR_TOK_LOCAL_ID)) {
                        name_view_t bname = tok_name_view(&p->cur);
                        next(p);
                        uint32_t bid = resolve_block_n(p, bname.s, bname.len);
                        ops[nops++] = lr_op_block(bid);
                    }
                    expect(p, LR_TOK_RBRACKET);
                } while (match(p, LR_TOK_COMMA));
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_PHI,
                    ty, dest, ops, nops);
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_EXTRACTVALUE: {
                lr_operand_t src = parse_typed_operand(p);
                uint32_t indices[16];
                uint32_t nidx = 0;
                lr_type_t *result_ty = p->module->type_i64;
                while (match(p, LR_TOK_COMMA)) {
                    indices[nidx++] = (uint32_t)p->cur.int_val;
                    expect(p, LR_TOK_INT_LIT);
                }
                if (src.type) {
                    const lr_type_t *leaf_ty = NULL;
                    if (lr_aggregate_index_path(src.type, indices, nidx, NULL, &leaf_ty) &&
                        leaf_ty) {
                        result_ty = (lr_type_t *)leaf_ty;
                    }
                }
                lr_operand_t ops[1] = {src};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_EXTRACTVALUE,
                    result_ty, dest, ops, 1);
                inst->indices = lr_arena_array(p->arena, uint32_t, nidx);
                memcpy(inst->indices, indices, sizeof(uint32_t) * nidx);
                inst->num_indices = nidx;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_LOCAL_ID: {
                if (!token_equals(&p->prev, "extractelement")) {
                    error(p, "unknown instruction '%.*s'", (int)p->prev.len, p->prev.start);
                    break;
                }
                lr_operand_t src = parse_typed_operand(p);
                uint32_t idx = 0;
                lr_type_t *result_ty = p->module->type_i64;
                expect(p, LR_TOK_COMMA);
                {
                    lr_operand_t idx_op = parse_typed_operand(p);
                    if (idx_op.kind == LR_VAL_IMM_I64) {
                        idx = (uint32_t)idx_op.imm_i64;
                    } else {
                        error(p, "extractelement currently requires constant index");
                    }
                }
                if (src.type && src.type->kind == LR_TYPE_ARRAY)
                    result_ty = src.type->array.elem;
                lr_operand_t ops[1] = {src};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_EXTRACTVALUE,
                    result_ty, dest, ops, 1);
                inst->indices = lr_arena_array(p->arena, uint32_t, 1);
                inst->indices[0] = idx;
                inst->num_indices = 1;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_INSERTVALUE: {
                lr_operand_t agg = parse_typed_operand(p);
                expect(p, LR_TOK_COMMA);
                lr_operand_t val = parse_typed_operand(p);
                uint32_t indices[16];
                uint32_t nidx = 0;
                while (match(p, LR_TOK_COMMA)) {
                    indices[nidx++] = (uint32_t)p->cur.int_val;
                    expect(p, LR_TOK_INT_LIT);
                }
                lr_operand_t ops[2] = {agg, val};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_INSERTVALUE,
                    agg.type, dest, ops, 2);
                inst->indices = lr_arena_array(p->arena, uint32_t, nidx);
                memcpy(inst->indices, indices, sizeof(uint32_t) * nidx);
                inst->num_indices = nidx;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_FCMP: {
                lr_fcmp_pred_t pred;
                switch (p->cur.kind) {
                case LR_TOK_FALSE: pred = LR_FCMP_FALSE; break;
                case LR_TOK_OEQ: pred = LR_FCMP_OEQ; break;
                case LR_TOK_OGT: pred = LR_FCMP_OGT; break;
                case LR_TOK_OGE: pred = LR_FCMP_OGE; break;
                case LR_TOK_OLT: pred = LR_FCMP_OLT; break;
                case LR_TOK_OLE: pred = LR_FCMP_OLE; break;
                case LR_TOK_ONE: pred = LR_FCMP_ONE; break;
                case LR_TOK_ORD: pred = LR_FCMP_ORD; break;
                case LR_TOK_UEQ: pred = LR_FCMP_UEQ; break;
                case LR_TOK_UGT: pred = LR_FCMP_UGT; break;
                case LR_TOK_UGE: pred = LR_FCMP_UGE; break;
                case LR_TOK_ULT: pred = LR_FCMP_ULT; break;
                case LR_TOK_ULE: pred = LR_FCMP_ULE; break;
                case LR_TOK_UNE: pred = LR_FCMP_UNE; break;
                case LR_TOK_UNO: pred = LR_FCMP_UNO; break;
                case LR_TOK_TRUE: pred = LR_FCMP_TRUE; break;
                default:
                    error(p, "expected fcmp predicate");
                    pred = LR_FCMP_OEQ;
                }
                next(p);
                lr_type_t *ty = parse_type(p);
                lr_operand_t lhs = parse_operand(p, ty);
                expect(p, LR_TOK_COMMA);
                lr_operand_t rhs = parse_operand(p, ty);
                lr_operand_t ops[2] = {lhs, rhs};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_FCMP,
                    p->module->type_i1, dest, ops, 2);
                inst->fcmp_pred = pred;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_INVOKE: {
                bool call_sig_vararg = false;
                uint32_t call_sig_fixed = 0;
                bool sig_vararg = false;
                uint32_t sig_fixed = 0;
                lr_type_t *ret_ty = call_result_type(parse_type(p),
                                                     &call_sig_vararg,
                                                     &call_sig_fixed);
                skip_attrs(p);
                if (skip_optional_callee_signature(p, &sig_vararg, &sig_fixed)) {
                    call_sig_vararg = call_sig_vararg || sig_vararg;
                    call_sig_fixed = sig_fixed;
                }
                lr_operand_t callee = parse_operand(p, p->module->type_ptr);
                expect(p, LR_TOK_LPAREN);
                lr_operand_t args[64];
                uint32_t nargs = 0;
                if (!check(p, LR_TOK_RPAREN)) {
                    args[nargs++] = parse_typed_operand(p);
                    while (match(p, LR_TOK_COMMA)) {
                        skip_attrs(p);
                        args[nargs++] = parse_typed_operand(p);
                    }
                }
                expect(p, LR_TOK_RPAREN);
                lr_operand_t all_ops[65];
                all_ops[0] = callee;
                for (uint32_t i = 0; i < nargs; i++)
                    all_ops[i + 1] = args[i];
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_CALL,
                    ret_ty, dest, all_ops, nargs + 1);
                inst->call_vararg = call_sig_vararg;
                inst->call_fixed_args = call_sig_fixed;
                if (callee.kind != LR_VAL_GLOBAL)
                    inst->call_external_abi = true;
                lr_block_append(block, inst);
                skip_attrs(p);
                /* to label %normal unwind label %except */
                expect(p, LR_TOK_TO);
                expect(p, LR_TOK_LABEL);
                name_view_t nname = tok_name_view(&p->cur);
                next(p);
                uint32_t normal_id = resolve_block_n(p, nname.s, nname.len);
                /* skip "unwind label %except" */
                while (!check(p, LR_TOK_NEWLINE) && !check(p, LR_TOK_EOF) &&
                       !p->had_error)
                    next(p);
                lr_operand_t br_ops[1] = {lr_op_block(normal_id)};
                lr_inst_t *br = lr_inst_create(p->arena, LR_OP_BR,
                    p->module->type_void, 0, br_ops, 1);
                lr_block_append(block, br);
                break;
            }

            case LR_TOK_LANDINGPAD: {
                /* skip to end of line — dead code from invoke lowering */
                while (!check(p, LR_TOK_NEWLINE) && !check(p, LR_TOK_EOF) &&
                       !p->had_error)
                    next(p);
                break;
            }

            default:
                error(p, "unknown instruction '%.*s'", (int)p->prev.len, p->prev.start);
                break;
            }
            return;
        }
        /* Not an assignment, rewind. This is a label reference being used
           as a bare identifier - shouldn't happen at instruction level.
           Let's re-process as a label. */
        p->cur = saved;
        /* fall through to label check below */
    }

    /* terminators and void instructions */
    lr_tok_t op_tok = p->cur.kind;

    if (op_tok == LR_TOK_RET) {
        next(p);
        if (check(p, LR_TOK_VOID)) {
            next(p);
            lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_RET_VOID,
                p->module->type_void, 0, NULL, 0);
            lr_block_append(block, inst);
        } else {
            lr_operand_t val = parse_typed_operand(p);
            lr_operand_t ops[1] = {val};
            lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_RET,
                val.type, 0, ops, 1);
            lr_block_append(block, inst);
        }
        return;
    }

    if (op_tok == LR_TOK_BR) {
        next(p);
        if (check(p, LR_TOK_I1)) {
            /* conditional: br i1 %cond, label %t, label %f */
            next(p);
            lr_operand_t cond = parse_operand(p, p->module->type_i1);
            expect(p, LR_TOK_COMMA);
            expect(p, LR_TOK_LABEL);
            if (check(p, LR_TOK_LOCAL_ID)) {
                name_view_t tname = tok_name_view(&p->cur);
                next(p);
                uint32_t tid = resolve_block_n(p, tname.s, tname.len);
                expect(p, LR_TOK_COMMA);
                expect(p, LR_TOK_LABEL);
                name_view_t fname = tok_name_view(&p->cur);
                next(p);
                uint32_t fid = resolve_block_n(p, fname.s, fname.len);
                lr_operand_t ops[3] = {cond, lr_op_block(tid), lr_op_block(fid)};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_CONDBR,
                    p->module->type_void, 0, ops, 3);
                lr_block_append(block, inst);
            }
        } else {
            /* unconditional: br label %dest */
            expect(p, LR_TOK_LABEL);
            if (check(p, LR_TOK_LOCAL_ID)) {
                name_view_t dname = tok_name_view(&p->cur);
                next(p);
                uint32_t did = resolve_block_n(p, dname.s, dname.len);
                lr_operand_t ops[1] = {lr_op_block(did)};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_BR,
                    p->module->type_void, 0, ops, 1);
                lr_block_append(block, inst);
            }
        }
        return;
    }

    if (op_tok == LR_TOK_STORE) {
        next(p);
        skip_memory_qualifiers(p);
        lr_type_t *val_ty = parse_type(p);
        skip_attrs(p);

        bool is_agg = val_ty && val_ty->kind == LR_TYPE_STRUCT &&
                      lr_type_size(val_ty) > 8 &&
                      (check(p, LR_TOK_LANGLE) || check(p, LR_TOK_LBRACE));
        if (is_agg) {
            lr_operand_t fields[AGG_FIELDS_MAX];
            uint32_t nf = 0;
            parse_struct_constant_fields(p, val_ty, fields, &nf);
            expect(p, LR_TOK_COMMA);
            lr_operand_t dst = parse_typed_operand(p);
            if (match(p, LR_TOK_COMMA)) {
                if (check(p, LR_TOK_ALIGN)) { next(p); next(p); }
            }
            uint32_t max_f = val_ty->struc.num_fields;
            if (nf > max_f) nf = max_f;
            for (uint32_t i = 0; i < nf; i++) {
                uint32_t gep_dest = lr_vreg_new(func);
                lr_operand_t gep_ops[3] = {
                    dst,
                    lr_op_imm_i64(0, p->module->type_i32),
                    lr_op_imm_i64((int64_t)i, p->module->type_i32)
                };
                lr_inst_t *gep = lr_inst_create(p->arena, LR_OP_GEP,
                    val_ty, gep_dest, gep_ops, 3);
                lr_block_append(block, gep);
                lr_operand_t st_ops[2] = {
                    fields[i],
                    lr_op_vreg(gep_dest, p->module->type_ptr)
                };
                lr_inst_t *st = lr_inst_create(p->arena, LR_OP_STORE,
                    p->module->type_void, 0, st_ops, 2);
                lr_block_append(block, st);
            }
            return;
        }

        lr_operand_t val = parse_operand(p, val_ty);
        expect(p, LR_TOK_COMMA);
        lr_operand_t dst = parse_typed_operand(p);
        if (match(p, LR_TOK_COMMA)) {
            if (check(p, LR_TOK_ALIGN)) { next(p); next(p); }
        }
        lr_operand_t ops[2] = {val, dst};
        lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_STORE,
            p->module->type_void, 0, ops, 2);
        lr_block_append(block, inst);
        return;
    }

    if (op_tok == LR_TOK_UNREACHABLE) {
        next(p);
        lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_UNREACHABLE,
            p->module->type_void, 0, NULL, 0);
        lr_block_append(block, inst);
        return;
    }

    if (op_tok == LR_TOK_SWITCH) {
        next(p);
        lr_type_t *val_ty = parse_type(p);
        lr_operand_t val = parse_operand(p, val_ty);
        expect(p, LR_TOK_COMMA);
        expect(p, LR_TOK_LABEL);
        name_view_t dname = tok_name_view(&p->cur);
        next(p);
        uint32_t default_id = resolve_block_n(p, dname.s, dname.len);

        struct { int64_t case_val; uint32_t block_id; } cases[256];
        uint32_t ncases = 0;
        expect(p, LR_TOK_LBRACKET);
        while (!check(p, LR_TOK_RBRACKET) && !check(p, LR_TOK_EOF) &&
               !p->had_error) {
            parse_type(p);
            int64_t cv = p->cur.int_val;
            expect(p, LR_TOK_INT_LIT);
            expect(p, LR_TOK_COMMA);
            expect(p, LR_TOK_LABEL);
            name_view_t cname = tok_name_view(&p->cur);
            next(p);
            uint32_t cid = resolve_block_n(p, cname.s, cname.len);
            if (ncases < 256) {
                cases[ncases].case_val = cv;
                cases[ncases].block_id = cid;
                ncases++;
            }
        }
        expect(p, LR_TOK_RBRACKET);

        if (ncases == 0) {
            lr_operand_t ops[1] = {lr_op_block(default_id)};
            lr_inst_t *br = lr_inst_create(p->arena, LR_OP_BR,
                p->module->type_void, 0, ops, 1);
            lr_block_append(block, br);
        } else {
            for (uint32_t ci = 0; ci < ncases; ci++) {
                uint32_t cmp_dest = lr_vreg_new(func);
                lr_operand_t cmp_ops[2] = {
                    val, lr_op_imm_i64(cases[ci].case_val, val_ty)
                };
                lr_inst_t *cmp = lr_inst_create(p->arena, LR_OP_ICMP,
                    p->module->type_i1, cmp_dest, cmp_ops, 2);
                cmp->icmp_pred = LR_ICMP_EQ;
                lr_block_append(block, cmp);

                uint32_t next_id;
                if (ci + 1 < ncases) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "switch.%u.%u", block->id, ci);
                    lr_block_t *nb = lr_block_create(func, p->arena, buf);
                    next_id = nb->id;
                } else {
                    next_id = default_id;
                }

                lr_operand_t br_ops[3] = {
                    lr_op_vreg(cmp_dest, p->module->type_i1),
                    lr_op_block(cases[ci].block_id),
                    lr_op_block(next_id)
                };
                lr_inst_t *br = lr_inst_create(p->arena, LR_OP_CONDBR,
                    p->module->type_void, 0, br_ops, 3);
                lr_block_append(block, br);

                if (ci + 1 < ncases) {
                    block = func->block_array ? NULL :
                        func->last_block;
                    if (!block) {
                        error(p, "switch lowering lost block");
                        return;
                    }
                }
            }
        }
        return;
    }

    /* void call */
    if (op_tok == LR_TOK_CALL) {
        next(p);
        bool call_sig_vararg = false;
        uint32_t call_sig_fixed = 0;
        bool sig_vararg = false;
        uint32_t sig_fixed = 0;
        lr_type_t *ret_ty = call_result_type(parse_type(p),
                                             &call_sig_vararg,
                                             &call_sig_fixed);
        skip_attrs(p);
        if (skip_optional_callee_signature(p, &sig_vararg, &sig_fixed)) {
            call_sig_vararg = call_sig_vararg || sig_vararg;
            call_sig_fixed = sig_fixed;
        }
        lr_operand_t callee = parse_operand(p, p->module->type_ptr);
        expect(p, LR_TOK_LPAREN);
        lr_operand_t args[64];
        uint32_t nargs = 0;
        if (!check(p, LR_TOK_RPAREN)) {
            args[nargs++] = parse_typed_operand(p);
            while (match(p, LR_TOK_COMMA)) {
                skip_attrs(p);
                args[nargs++] = parse_typed_operand(p);
            }
        }
        expect(p, LR_TOK_RPAREN);
        lr_operand_t all_ops[65];
        all_ops[0] = callee;
        for (uint32_t i = 0; i < nargs; i++)
            all_ops[i + 1] = args[i];
        lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_CALL,
            ret_ty, 0, all_ops, nargs + 1);
        inst->call_vararg = call_sig_vararg;
        inst->call_fixed_args = call_sig_fixed;
        if (callee.kind != LR_VAL_GLOBAL)
            inst->call_external_abi = true;
        lr_block_append(block, inst);
        skip_attrs(p);
        return;
    }

    /* void invoke → lower to call + br to normal label */
    if (op_tok == LR_TOK_INVOKE) {
        next(p);
        bool call_sig_vararg = false;
        uint32_t call_sig_fixed = 0;
        bool sig_vararg = false;
        uint32_t sig_fixed = 0;
        lr_type_t *ret_ty = call_result_type(parse_type(p),
                                             &call_sig_vararg,
                                             &call_sig_fixed);
        skip_attrs(p);
        if (skip_optional_callee_signature(p, &sig_vararg, &sig_fixed)) {
            call_sig_vararg = call_sig_vararg || sig_vararg;
            call_sig_fixed = sig_fixed;
        }
        lr_operand_t callee = parse_operand(p, p->module->type_ptr);
        expect(p, LR_TOK_LPAREN);
        lr_operand_t args[64];
        uint32_t nargs = 0;
        if (!check(p, LR_TOK_RPAREN)) {
            args[nargs++] = parse_typed_operand(p);
            while (match(p, LR_TOK_COMMA)) {
                skip_attrs(p);
                args[nargs++] = parse_typed_operand(p);
            }
        }
        expect(p, LR_TOK_RPAREN);
        lr_operand_t all_ops[65];
        all_ops[0] = callee;
        for (uint32_t i = 0; i < nargs; i++)
            all_ops[i + 1] = args[i];
        lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_CALL,
            ret_ty, 0, all_ops, nargs + 1);
        inst->call_vararg = call_sig_vararg;
        inst->call_fixed_args = call_sig_fixed;
        if (callee.kind != LR_VAL_GLOBAL)
            inst->call_external_abi = true;
        lr_block_append(block, inst);
        skip_attrs(p);
        expect(p, LR_TOK_TO);
        expect(p, LR_TOK_LABEL);
        name_view_t nname = tok_name_view(&p->cur);
        next(p);
        uint32_t normal_id = resolve_block_n(p, nname.s, nname.len);
        /* skip "unwind label %except" */
        while (!check(p, LR_TOK_NEWLINE) && !check(p, LR_TOK_EOF) &&
               !p->had_error)
            next(p);
        lr_operand_t br_ops[1] = {lr_op_block(normal_id)};
        lr_inst_t *br = lr_inst_create(p->arena, LR_OP_BR,
            p->module->type_void, 0, br_ops, 1);
        lr_block_append(block, br);
        return;
    }

    /* landingpad/resume — skip (dead code from invoke lowering) */
    if (op_tok == LR_TOK_LANDINGPAD || op_tok == LR_TOK_RESUME) {
        while (!check(p, LR_TOK_NEWLINE) && !check(p, LR_TOK_EOF) &&
               !p->had_error)
            next(p);
        return;
    }

    error(p, "unexpected token '%s' in basic block", lr_tok_name(op_tok));
}

static void parse_function_body(lr_parser_t *p, lr_func_t *func, char **param_names) {
    p->cur_func = func;
    p->vreg_map_count = 0;
    p->block_map_count = 0;
    clear_index(p->vreg_index, p->vreg_index_cap);
    clear_index(p->block_index, p->block_index_cap);
    clear_u32_map(p->vreg_numeric, p->vreg_numeric_cap);

    /* register parameter vregs: named params get only name, unnamed get numeric alias */
    for (uint32_t i = 0; i < func->num_params; i++) {
        if (param_names && param_names[i]) {
            /* named parameter: register only the name, not numeric alias */
            register_vreg_name(p, param_names[i], func->param_vregs[i]);
        } else {
            /* unnamed parameter: register numeric alias */
            register_vreg_number(p, i, func->param_vregs[i]);
        }
    }

    expect(p, LR_TOK_LBRACE);

    /* first block - may be unlabeled */
    lr_block_t *cur_block = NULL;

    while (!check(p, LR_TOK_RBRACE) && !check(p, LR_TOK_EOF) && !p->had_error) {
        /* Check for label: name followed by colon */
        if (check(p, LR_TOK_LOCAL_ID) || check(p, LR_TOK_STRING_LIT)
            || check(p, LR_TOK_INT_LIT)) {
            /* peek: is next token a colon? */
            lr_token_t saved_tok = p->cur;
            size_t saved_pos = p->lex.pos;

            next(p);
            if (check(p, LR_TOK_COLON)) {
                /* it is a label */
                next(p);
                name_view_t bname = tok_name_view(&saved_tok);
                cur_block = resolve_block_ptr_n(p, bname.s, bname.len);
                continue;
            }
            /* not a label, restore and parse as instruction */
            p->cur = saved_tok;
            p->lex.pos = saved_pos;
        }

        /* Check for bare keyword label (e.g. "entry:" without %) */
        /* In LLVM IR, block labels can be bare identifiers */
        /* We handle this by checking known patterns */

        if (!cur_block) {
            cur_block = resolve_block_ptr(p, "entry");
        }

        parse_instruction(p, func, cur_block);

        /* skip trailing metadata attachments: , !name !num */
        while (check(p, LR_TOK_COMMA)) {
            lr_token_t saved = p->cur;
            size_t saved_pos = p->lex.pos;
            next(p);
            if (check(p, LR_TOK_METADATA_ID) || check(p, LR_TOK_EXCLAIM)) {
                while (check(p, LR_TOK_METADATA_ID) || check(p, LR_TOK_EXCLAIM))
                    next(p);
                continue;
            }
            /* not metadata, restore comma and stop */
            p->cur = saved;
            p->lex.pos = saved_pos;
            break;
        }
    }

    expect(p, LR_TOK_RBRACE);
    p->cur_func = NULL;
}

static lr_type_t *parse_param_type(lr_parser_t *p) {
    lr_type_t *ty = parse_type(p);
    skip_attrs(p);
    return ty;
}

static void parse_function_def(lr_parser_t *p, bool is_decl) {
    skip_attrs(p);
    lr_type_t *ret_type = parse_type(p);

    if (!check(p, LR_TOK_GLOBAL_ID)) {
        error(p, "expected function name");
        return;
    }
    char *name = tok_name(p, &p->cur);
    next(p);

    expect(p, LR_TOK_LPAREN);
    lr_type_t *params[256];
    char *param_names[256];
    uint32_t nparams = 0;
    bool vararg = false;
    memset(param_names, 0, sizeof(param_names));
    if (!check(p, LR_TOK_RPAREN)) {
        if (check(p, LR_TOK_DOTDOTDOT)) {
            vararg = true;
            next(p);
        } else {
            params[nparams] = parse_param_type(p);
            if (check(p, LR_TOK_LOCAL_ID)) {
                param_names[nparams] = tok_name(p, &p->cur);
                next(p);
            }
            nparams++;
            while (match(p, LR_TOK_COMMA)) {
                if (check(p, LR_TOK_DOTDOTDOT)) {
                    vararg = true;
                    next(p);
                    break;
                }
                skip_attrs(p);
                params[nparams] = parse_param_type(p);
                if (check(p, LR_TOK_LOCAL_ID)) {
                    param_names[nparams] = tok_name(p, &p->cur);
                    next(p);
                }
                nparams++;
            }
        }
    }
    expect(p, LR_TOK_RPAREN);

    /* skip trailing attrs like unnamed_addr #0 */
    skip_attrs(p);
    while (check(p, LR_TOK_UNNAMED_ADDR) || check(p, LR_TOK_LOCAL_UNNAMED_ADDR)) next(p);
    skip_attrs(p);
    /* skip personality clause: personality ptr @__gxx_personality_v0 */
    if (check(p, LR_TOK_PERSONALITY)) {
        while (!check(p, LR_TOK_LBRACE) && !check(p, LR_TOK_NEWLINE) &&
               !check(p, LR_TOK_EOF) && !p->had_error)
            next(p);
    }

    uint32_t sym_id = UINT32_MAX;
    lr_func_t *func = lr_frontend_create_function(p->module, name, ret_type,
                                                  params, nparams, vararg,
                                                  is_decl, &sym_id);
    if (resolve_global(p, name) == UINT32_MAX)
        register_global(p, name, sym_id);
    register_func(p, name, func);

    if (!is_decl)
        parse_function_body(p, func, param_names);
}

/* Skip lines we don't understand (metadata, target triple, attributes, etc.) */
static void skip_line(lr_parser_t *p) {
    /* Skip tokens until we hit something that looks like a new top-level construct */
    while (!check(p, LR_TOK_EOF)) {
        bool at_toplevel_col = (p->cur.start == p->lex.src ||
                                p->cur.start[-1] == '\n');
        if (at_toplevel_col && (check(p, LR_TOK_DEFINE) || check(p, LR_TOK_DECLARE)))
            return;
        if (at_toplevel_col && (check(p, LR_TOK_GLOBAL_ID) || check(p, LR_TOK_LOCAL_ID)))
            return;
        next(p);
    }
}

static void parse_aggregate_initializer(lr_parser_t *p, lr_global_t *g,
                                         uint8_t *buf, size_t buf_size,
                                         const lr_type_t *ty, size_t base_offset);

/*
 * Parse a single scalar initializer value at field_off within buf.
 * Handles: integers, floats, GEP, bare global refs, null, undef, nested aggregates.
 * Records relocations for pointer-to-global values.
 */
static void parse_init_field_value(lr_parser_t *p, lr_global_t *g,
                                    uint8_t *buf, size_t buf_size,
                                    const lr_type_t *field_type, size_t field_off) {
    size_t field_sz = lr_type_size(field_type);

    if (check(p, LR_TOK_LANGLE) || check(p, LR_TOK_LBRACE) ||
        check(p, LR_TOK_LBRACKET)) {
        parse_aggregate_initializer(p, g, buf, buf_size, field_type, field_off);
    } else if (check(p, LR_TOK_GETELEMENTPTR)) {
        lr_operand_t gep = parse_const_gep_operand(p, p->module->type_ptr);
        if (gep.kind == LR_VAL_GLOBAL) {
            const char *ref = lr_module_symbol_name(p->module, gep.global_id);
            if (ref) {
                lr_reloc_t *r = lr_arena_new(p->arena, lr_reloc_t);
                r->offset = field_off;
                r->addend = gep.global_offset;
                r->symbol_name = lr_arena_strdup(p->arena, ref, strlen(ref));
                r->next = g->relocs;
                g->relocs = r;
            }
        }
    } else if (check(p, LR_TOK_BITCAST) || check(p, LR_TOK_INTTOPTR) ||
               check(p, LR_TOK_PTRTOINT) || check(p, LR_TOK_SEXT) ||
               check(p, LR_TOK_ZEXT) || check(p, LR_TOK_TRUNC) ||
               check(p, LR_TOK_SITOFP) || check(p, LR_TOK_UITOFP) ||
               check(p, LR_TOK_FPTOSI) || check(p, LR_TOK_FPTOUI) ||
               check(p, LR_TOK_FPEXT) || check(p, LR_TOK_FPTRUNC)) {
        lr_tok_t cast_tok = p->cur.kind;
        next(p);
        expect(p, LR_TOK_LPAREN);
        lr_operand_t src = parse_typed_operand(p);
        expect(p, LR_TOK_TO);
        (void)parse_type(p);
        expect(p, LR_TOK_RPAREN);
        if (src.kind == LR_VAL_GLOBAL) {
            const char *ref = lr_module_symbol_name(p->module, src.global_id);
            if (ref) {
                lr_reloc_t *r = lr_arena_new(p->arena, lr_reloc_t);
                r->offset = field_off;
                r->addend = src.global_offset;
                r->symbol_name = lr_arena_strdup(p->arena, ref, strlen(ref));
                r->next = g->relocs;
                g->relocs = r;
            }
        } else if (cast_tok == LR_TOK_INTTOPTR &&
                   src.kind == LR_VAL_IMM_I64 &&
                   field_off + field_sz <= buf_size) {
            uint64_t raw = (uint64_t)src.imm_i64;
            size_t copy_sz = field_sz < sizeof(raw) ? field_sz : sizeof(raw);
            memcpy(buf + field_off, &raw, copy_sz);
        }
    } else if (check(p, LR_TOK_INT_LIT)) {
        int64_t val = p->cur.int_val;
        next(p);
        if (field_off + field_sz <= buf_size)
            memcpy(buf + field_off, &val, field_sz < 8 ? field_sz : 8);
    } else if (check(p, LR_TOK_TRUE)) {
        uint8_t v = 1u;
        next(p);
        if (field_off + field_sz <= buf_size && field_sz > 0)
            memcpy(buf + field_off, &v, field_sz < 1 ? field_sz : 1);
    } else if (check(p, LR_TOK_FALSE)) {
        uint8_t v = 0u;
        next(p);
        if (field_off + field_sz <= buf_size && field_sz > 0)
            memcpy(buf + field_off, &v, field_sz < 1 ? field_sz : 1);
    } else if (check(p, LR_TOK_FLOAT_LIT)) {
        double val = p->cur.float_val;
        next(p);
        if (field_type->kind == LR_TYPE_FLOAT) {
            float fv = (float)val;
            if (field_off + 4 <= buf_size)
                memcpy(buf + field_off, &fv, 4);
        } else {
            if (field_off + 8 <= buf_size)
                memcpy(buf + field_off, &val, 8);
        }
    } else if (check(p, LR_TOK_NULL)) {
        next(p);
    } else if (check(p, LR_TOK_ZEROINITIALIZER)) {
        next(p);
    } else if (check(p, LR_TOK_GLOBAL_ID)) {
        char *ref_name = tok_name(p, &p->cur);
        next(p);
        uint32_t gid = resolve_global(p, ref_name);
        if (gid == UINT32_MAX) {
            gid = lr_frontend_intern_symbol(p->module, ref_name);
            register_global(p, ref_name, gid);
        }
        lr_reloc_t *r = lr_arena_new(p->arena, lr_reloc_t);
        r->offset = field_off;
        r->addend = 0;
        r->symbol_name = lr_arena_strdup(p->arena, ref_name, strlen(ref_name));
        r->next = g->relocs;
        g->relocs = r;
    } else if (check(p, LR_TOK_UNDEF) || check(p, LR_TOK_STRING_LIT)) {
        next(p);
    } else {
        next(p);
    }
}

/*
 * Parse an aggregate constant initializer and write field values into buf.
 * Record relocations for pointer-to-global fields on the global g.
 * base_offset is the byte offset of this aggregate within the top-level global.
 */
static void parse_aggregate_initializer(lr_parser_t *p, lr_global_t *g,
                                         uint8_t *buf, size_t buf_size,
                                         const lr_type_t *ty, size_t base_offset) {
    bool packed_struct = false;

    if (check(p, LR_TOK_LANGLE)) {
        next(p);
        expect(p, LR_TOK_LBRACE);
        packed_struct = true;
    } else if (check(p, LR_TOK_LBRACE)) {
        next(p);
    } else if (check(p, LR_TOK_LBRACKET)) {
        next(p);
        if (ty->kind == LR_TYPE_ARRAY) {
            size_t elem_sz = lr_type_size(ty->array.elem);
            for (uint64_t i = 0; i < ty->array.count; i++) {
                if (check(p, LR_TOK_RBRACKET))
                    break;
                (void)parse_type(p);
                skip_attrs(p);
                size_t elem_off = base_offset + i * elem_sz;
                parse_init_field_value(p, g, buf, buf_size, ty->array.elem, elem_off);
                if (!match(p, LR_TOK_COMMA))
                    break;
            }
        }
        expect(p, LR_TOK_RBRACKET);
        return;
    } else {
        return;
    }

    if (ty->kind != LR_TYPE_STRUCT) {
        uint32_t depth = 1;
        while (depth > 0 && !check(p, LR_TOK_EOF)) {
            if (match(p, LR_TOK_LBRACE)) { depth++; continue; }
            if (match(p, LR_TOK_RBRACE)) { depth--; continue; }
            next(p);
        }
        if (packed_struct)
            match(p, LR_TOK_RANGLE);
        return;
    }

    for (uint32_t fi = 0; fi < ty->struc.num_fields; fi++) {
        if (check(p, LR_TOK_RBRACE))
            break;
        (void)parse_type(p);
        skip_attrs(p);
        size_t field_off = base_offset + lr_struct_field_offset(ty, fi);
        parse_init_field_value(p, g, buf, buf_size, ty->struc.fields[fi], field_off);
        if (!match(p, LR_TOK_COMMA))
            break;
    }

    expect(p, LR_TOK_RBRACE);
    if (packed_struct)
        expect(p, LR_TOK_RANGLE);
}

static void parse_global(lr_parser_t *p) {
    char *name = tok_name(p, &p->cur);
    next(p);
    expect(p, LR_TOK_EQUALS);

    /* skip linkage */
    while (check(p, LR_TOK_EXTERNAL) || check(p, LR_TOK_INTERNAL) ||
           check(p, LR_TOK_PRIVATE) || check(p, LR_TOK_COMMON) ||
           check(p, LR_TOK_LINKONCE_ODR) || check(p, LR_TOK_DSOLOCAL) ||
           check(p, LR_TOK_UNNAMED_ADDR) || check(p, LR_TOK_LOCAL_UNNAMED_ADDR))
        next(p);

    bool is_const = false;
    if (check(p, LR_TOK_GLOBAL)) {
        next(p);
    } else if (check(p, LR_TOK_CONSTANT)) {
        next(p);
        is_const = true;
    } else if (check(p, LR_TOK_TYPE)) {
        next(p);
        if (check(p, LR_TOK_OPAQUE)) {
            next(p);
        } else {
            lr_type_t *alias = parse_type(p);
            register_type(p, name, alias);
        }
        skip_line(p);
        return;
    } else {
        skip_line(p);
        return;
    }

    lr_type_t *ty = parse_type(p);
    lr_global_t *g = lr_global_create(p->module, name, ty, is_const);
    uint32_t sym_id = lr_frontend_intern_symbol(p->module, g->name);
    if (resolve_global(p, g->name) == UINT32_MAX)
        register_global(p, g->name, sym_id);

    if (check(p, LR_TOK_STRING_LIT)) {
        const char *s = p->cur.start;
        size_t slen = p->cur.len;
        if (slen >= 3 && s[0] == 'c' && s[1] == '"') {
            s += 2; slen -= 3;
            uint8_t *buf = lr_arena_array(p->arena, uint8_t, slen + 1);
            size_t out = 0;
            for (size_t i = 0; i < slen; i++) {
                if (s[i] == '\\' && i + 1 < slen) {
                    if (i + 2 < slen) {
                        int hi = 0, lo = 0;
                        char c1 = s[i + 1], c2 = s[i + 2];
                        hi = (c1 >= '0' && c1 <= '9') ? c1 - '0' :
                             (c1 >= 'a' && c1 <= 'f') ? c1 - 'a' + 10 :
                             (c1 >= 'A' && c1 <= 'F') ? c1 - 'A' + 10 : -1;
                        lo = (c2 >= '0' && c2 <= '9') ? c2 - '0' :
                             (c2 >= 'a' && c2 <= 'f') ? c2 - 'a' + 10 :
                             (c2 >= 'A' && c2 <= 'F') ? c2 - 'A' + 10 : -1;
                        if (hi >= 0 && lo >= 0) {
                            buf[out++] = (uint8_t)(hi * 16 + lo);
                            i += 2;
                            continue;
                        }
                    }
                    if (s[i + 1] == '\\') {
                        buf[out++] = '\\';
                        i++;
                        continue;
                    }
                }
                buf[out++] = (uint8_t)s[i];
            }
            g->init_data = buf;
            g->init_size = out;
        }
        next(p);
    } else if (check(p, LR_TOK_ZEROINITIALIZER)) {
        next(p);
    } else if (check(p, LR_TOK_INT_LIT)) {
        int64_t val = p->cur.int_val;
        size_t sz = lr_type_size(ty);
        if (sz > 0 && sz <= 8) {
            uint8_t *buf = lr_arena_array(p->arena, uint8_t, sz);
            memcpy(buf, &val, sz);
            g->init_data = buf;
            g->init_size = sz;
        }
        next(p);
    } else if (check(p, LR_TOK_FLOAT_LIT)) {
        double val = p->cur.float_val;
        size_t sz = lr_type_size(ty);
        if (sz > 0) {
            uint8_t *buf = lr_arena_array(p->arena, uint8_t, sz);
            memset(buf, 0, sz);
            if (ty->kind == LR_TYPE_FLOAT) {
                float fv = (float)val;
                memcpy(buf, &fv, 4);
            } else {
                memcpy(buf, &val, sz < 8 ? sz : 8);
            }
            g->init_data = buf;
            g->init_size = sz;
        }
        next(p);
    } else if (check(p, LR_TOK_LANGLE) || check(p, LR_TOK_LBRACE) ||
               check(p, LR_TOK_LBRACKET)) {
        size_t sz = lr_type_size(ty);
        if (sz > 0) {
            uint8_t *buf = lr_arena_array(p->arena, uint8_t, sz);
            memset(buf, 0, sz);
            g->init_data = buf;
            g->init_size = sz;
            parse_aggregate_initializer(p, g, buf, sz, ty, 0);
        } else {
            if (check(p, LR_TOK_LBRACE))
                skip_balanced_braces(p);
            else if (check(p, LR_TOK_LBRACKET))
                skip_balanced_brackets(p);
            else {
                next(p);
                skip_balanced_braces(p);
                match(p, LR_TOK_RANGLE);
            }
        }
    } else if (check(p, LR_TOK_NULL)) {
        next(p);
    } else if (check(p, LR_TOK_TRUE)) {
        uint8_t *buf = lr_arena_array(p->arena, uint8_t, 1);
        buf[0] = 1;
        g->init_data = buf;
        g->init_size = 1;
        next(p);
    } else if (check(p, LR_TOK_FALSE)) {
        next(p);
    } else if (check(p, LR_TOK_GETELEMENTPTR)) {
        lr_operand_t gep = parse_const_gep_operand(p, p->module->type_ptr);
        size_t sz = lr_type_size(ty);
        if (sz == 0)
            sz = 8;
        uint8_t *buf = lr_arena_array(p->arena, uint8_t, sz);
        memset(buf, 0, sz);
        g->init_data = buf;
        g->init_size = sz;
        if (gep.kind == LR_VAL_GLOBAL) {
            const char *ref = lr_module_symbol_name(p->module, gep.global_id);
            if (ref) {
                lr_reloc_t *r = lr_arena_new(p->arena, lr_reloc_t);
                r->offset = 0;
                r->addend = gep.global_offset;
                r->symbol_name = lr_arena_strdup(p->arena, ref, strlen(ref));
                r->next = g->relocs;
                g->relocs = r;
            }
        }
    } else if (check(p, LR_TOK_GLOBAL_ID)) {
        char *ref_name = tok_name(p, &p->cur);
        next(p);
        uint32_t gid = resolve_global(p, ref_name);
        if (gid == UINT32_MAX) {
            gid = lr_frontend_intern_symbol(p->module, ref_name);
            register_global(p, ref_name, gid);
        }
        size_t sz = lr_type_size(ty);
        if (sz == 0)
            sz = 8;
        uint8_t *buf = lr_arena_array(p->arena, uint8_t, sz);
        memset(buf, 0, sz);
        g->init_data = buf;
        g->init_size = sz;
        lr_reloc_t *r = lr_arena_new(p->arena, lr_reloc_t);
        r->offset = 0;
        r->addend = 0;
        r->symbol_name = lr_arena_strdup(p->arena, ref_name, strlen(ref_name));
        r->next = g->relocs;
        g->relocs = r;
    }

    skip_line(p);
}

lr_module_t *lr_parse_ll_text(const char *src, size_t len,
                               lr_arena_t *arena, char *err, size_t errlen) {
    lr_parser_t p = {0};
    lr_module_t *out = NULL;
    lr_lexer_init(&p.lex, src, len);
    p.arena = arena;
    p.err = err;
    p.errlen = errlen;
    if (err && errlen > 0) err[0] = '\0';

    if (!parser_init_work_buffers(&p)) {
        parser_free_work_buffers(&p);
        return NULL;
    }

    p.module = lr_module_create(arena);
    next(&p);

    while (!check(&p, LR_TOK_EOF) && !p.had_error) {
        if (check(&p, LR_TOK_DEFINE)) {
            next(&p);
            parse_function_def(&p, false);
        } else if (check(&p, LR_TOK_DECLARE)) {
            next(&p);
            parse_function_def(&p, true);
        } else if (check(&p, LR_TOK_GLOBAL_ID)) {
            parse_global(&p);
        } else if (check(&p, LR_TOK_LOCAL_ID)) {
            /* type alias: %name = type ... */
            char *tname = tok_name(&p, &p.cur);
            next(&p);
            if (match(&p, LR_TOK_EQUALS) && match(&p, LR_TOK_TYPE)) {
                if (check(&p, LR_TOK_OPAQUE)) {
                    next(&p);
                } else {
                    lr_type_t *alias = parse_type(&p);
                    register_type(&p, tname, alias);
                }
            }
            skip_line(&p);
        } else {
            /* skip unknown top-level directives (source_filename, target, attributes, metadata) */
            skip_line(&p);
        }
    }

    if (!p.had_error)
        out = p.module;
    parser_free_work_buffers(&p);
    return out;
}
