#include "objfile.h"
#include "objfile_macho.h"
#include "objfile_elf.h"
#include "arena.h"
#ifdef __APPLE__
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>

#define OBJ_CODE_BUF_SIZE (4 * 1024 * 1024)
#define OBJ_DATA_BUF_SIZE (1 * 1024 * 1024)
#define OBJ_INITIAL_RELOC_CAP 256
#define OBJ_INITIAL_SYMBOL_CAP 128

static uint32_t obj_symbol_hash(const char *name) {
    uint32_t h = 2166136261u;
    while (*name) {
        h ^= (uint8_t)*name++;
        h *= 16777619u;
    }
    return h;
}

static int obj_symbol_index_rebuild(lr_objfile_ctx_t *oc, uint32_t min_symbols) {
    uint32_t cap = 1;
    while (cap < (min_symbols << 1))
        cap <<= 1;

    uint32_t *new_index = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!new_index)
        return -1;

    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        uint32_t slot = oc->symbols[i].hash & (cap - 1u);
        while (new_index[slot] != 0)
            slot = (slot + 1u) & (cap - 1u);
        new_index[slot] = i + 1u;
    }

    free(oc->symbol_index);
    oc->symbol_index = new_index;
    oc->symbol_index_cap = cap;
    return 0;
}

static int obj_build_module_symbol_cache(lr_objfile_ctx_t *oc, lr_module_t *m) {
    if (!oc || !m)
        return -1;

    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->name && f->name[0])
            lr_module_intern_symbol(m, f->name);
    }
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (g->name && g->name[0])
            lr_module_intern_symbol(m, g->name);
    }

    oc->module_sym_count = m->num_symbols;
    if (oc->module_sym_count == 0)
        return 0;

    oc->module_sym_defined = (uint8_t *)calloc(oc->module_sym_count, sizeof(uint8_t));
    oc->module_sym_funcs = (lr_func_t **)calloc(oc->module_sym_count,
                                                sizeof(lr_func_t *));
    if (!oc->module_sym_defined || !oc->module_sym_funcs) {
        free(oc->module_sym_defined);
        free(oc->module_sym_funcs);
        oc->module_sym_defined = NULL;
        oc->module_sym_funcs = NULL;
        oc->module_sym_count = 0;
        return -1;
    }

    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->name || !f->name[0])
            continue;
        uint32_t sym_id = lr_module_intern_symbol(m, f->name);
        if (sym_id >= oc->module_sym_count)
            continue;
        oc->module_sym_funcs[sym_id] = f;
        if (f->first_block)
            oc->module_sym_defined[sym_id] = 1;
    }
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (!g->name || !g->name[0] || g->is_external)
            continue;
        uint32_t sym_id = lr_module_intern_symbol(m, g->name);
        if (sym_id < oc->module_sym_count)
            oc->module_sym_defined[sym_id] = 1;
    }
    return 0;
}

static const char *remap_intrinsic(const char *name) {
    if (!name || strncmp(name, "llvm.", 5) != 0) return name;
    if (strcmp(name, "llvm.pow.f32") == 0) return "powf";
    if (strcmp(name, "llvm.pow.f64") == 0) return "pow";
    if (strcmp(name, "llvm.sqrt.f32") == 0) return "sqrtf";
    if (strcmp(name, "llvm.sqrt.f64") == 0) return "sqrt";
    if (strcmp(name, "llvm.copysign.f32") == 0) return "copysignf";
    if (strcmp(name, "llvm.copysign.f64") == 0) return "copysign";
    if (strcmp(name, "llvm.powi.f32.i32") == 0) return "powf";
    if (strcmp(name, "llvm.powi.f64.i32") == 0) return "pow";
    if (strcmp(name, "llvm.powi.f32.i64") == 0) return "powf";
    if (strcmp(name, "llvm.powi.f64.i64") == 0) return "pow";
    if (strcmp(name, "llvm.fabs.f32") == 0) return "fabsf";
    if (strcmp(name, "llvm.fabs.f64") == 0) return "fabs";
    if (strcmp(name, "llvm.sin.f32") == 0) return "sinf";
    if (strcmp(name, "llvm.sin.f64") == 0) return "sin";
    if (strcmp(name, "llvm.cos.f32") == 0) return "cosf";
    if (strcmp(name, "llvm.cos.f64") == 0) return "cos";
    if (strcmp(name, "llvm.exp.f32") == 0) return "expf";
    if (strcmp(name, "llvm.exp.f64") == 0) return "exp";
    if (strcmp(name, "llvm.exp2.f32") == 0) return "exp2f";
    if (strcmp(name, "llvm.exp2.f64") == 0) return "exp2";
    if (strcmp(name, "llvm.log.f32") == 0) return "logf";
    if (strcmp(name, "llvm.log.f64") == 0) return "log";
    if (strcmp(name, "llvm.log2.f32") == 0) return "log2f";
    if (strcmp(name, "llvm.log2.f64") == 0) return "log2";
    if (strcmp(name, "llvm.log10.f32") == 0) return "log10f";
    if (strcmp(name, "llvm.log10.f64") == 0) return "log10";
    if (strcmp(name, "llvm.floor.f32") == 0) return "floorf";
    if (strcmp(name, "llvm.floor.f64") == 0) return "floor";
    if (strcmp(name, "llvm.ceil.f32") == 0) return "ceilf";
    if (strcmp(name, "llvm.ceil.f64") == 0) return "ceil";
    if (strcmp(name, "llvm.trunc.f32") == 0) return "truncf";
    if (strcmp(name, "llvm.trunc.f64") == 0) return "trunc";
    if (strcmp(name, "llvm.round.f32") == 0) return "roundf";
    if (strcmp(name, "llvm.round.f64") == 0) return "round";
    if (strcmp(name, "llvm.fma.f32") == 0) return "fmaf";
    if (strcmp(name, "llvm.fma.f64") == 0) return "fma";
    if (strcmp(name, "llvm.minnum.f32") == 0) return "fminf";
    if (strcmp(name, "llvm.minnum.f64") == 0) return "fmin";
    if (strcmp(name, "llvm.maxnum.f32") == 0) return "fmaxf";
    if (strcmp(name, "llvm.maxnum.f64") == 0) return "fmax";
    if (strcmp(name, "llvm.rint.f32") == 0) return "rintf";
    if (strcmp(name, "llvm.rint.f64") == 0) return "rint";
    if (strcmp(name, "llvm.nearbyint.f32") == 0) return "nearbyintf";
    if (strcmp(name, "llvm.nearbyint.f64") == 0) return "nearbyint";
    return name;
}

uint32_t lr_obj_ensure_symbol(lr_objfile_ctx_t *oc, const char *name,
                               bool is_defined, uint8_t section,
                               uint32_t offset) {
    if (!name) return UINT32_MAX;
    if (!oc->preserve_symbol_names)
        name = remap_intrinsic(name);
    uint32_t hash = obj_symbol_hash(name);

    if (oc->symbol_index_cap == 0) {
        if (obj_symbol_index_rebuild(oc, 1) != 0)
            return UINT32_MAX;
    }

    uint32_t slot = hash & (oc->symbol_index_cap - 1u);
    while (1) {
        uint32_t stored = oc->symbol_index[slot];
        if (stored == 0)
            break;
        uint32_t i = stored - 1u;
        if (oc->symbols[i].hash == hash &&
            strcmp(oc->symbols[i].name, name) == 0) {
            if (is_defined && !oc->symbols[i].is_defined) {
                oc->symbols[i].is_defined = true;
                oc->symbols[i].section = section;
                oc->symbols[i].offset = offset;
            }
            return i;
        }
        slot = (slot + 1u) & (oc->symbol_index_cap - 1u);
    }

    if (oc->num_symbols == oc->symbol_cap) {
        uint32_t new_cap = oc->symbol_cap == 0
            ? OBJ_INITIAL_SYMBOL_CAP
            : oc->symbol_cap * 2;
        lr_obj_symbol_t *ns = realloc(oc->symbols,
                                       new_cap * sizeof(lr_obj_symbol_t));
        if (!ns) return UINT32_MAX;
        oc->symbols = ns;
        oc->symbol_cap = new_cap;
    }

    if ((oc->num_symbols + 1u) * 2u > oc->symbol_index_cap) {
        if (obj_symbol_index_rebuild(oc, oc->num_symbols + 1u) != 0)
            return UINT32_MAX;
    }

    uint32_t idx = oc->num_symbols++;
    oc->symbols[idx].name = name;
    oc->symbols[idx].hash = hash;
    oc->symbols[idx].offset = offset;
    oc->symbols[idx].section = section;
    oc->symbols[idx].is_defined = is_defined;

    slot = hash & (oc->symbol_index_cap - 1u);
    while (oc->symbol_index[slot] != 0)
        slot = (slot + 1u) & (oc->symbol_index_cap - 1u);
    oc->symbol_index[slot] = idx + 1u;
    return idx;
}

void lr_obj_add_reloc(lr_objfile_ctx_t *oc, uint32_t offset,
                       uint32_t symbol_idx, uint8_t type) {
    if (oc->num_relocs == oc->reloc_cap) {
        uint32_t new_cap = oc->reloc_cap == 0
            ? OBJ_INITIAL_RELOC_CAP
            : oc->reloc_cap * 2;
        lr_obj_reloc_t *nr = realloc(oc->relocs,
                                      new_cap * sizeof(lr_obj_reloc_t));
        if (!nr) return;
        oc->relocs = nr;
        oc->reloc_cap = new_cap;
    }

    uint32_t i = oc->num_relocs++;
    oc->relocs[i].offset = offset;
    oc->relocs[i].symbol_idx = symbol_idx;
    oc->relocs[i].type = type;
}

typedef struct {
    uint8_t *code_buf;
    uint8_t *data_buf;
    size_t code_pos;
    size_t data_pos;
    bool has_data;
    lr_objfile_ctx_t ctx;
} lr_obj_build_result_t;

static void obj_ctx_destroy(lr_objfile_ctx_t *ctx) {
    if (!ctx)
        return;
    free(ctx->relocs);
    free(ctx->symbols);
    free(ctx->symbol_index);
    free(ctx->module_sym_defined);
    free(ctx->module_sym_funcs);
    memset(ctx, 0, sizeof(*ctx));
}

static void obj_build_result_destroy(lr_obj_build_result_t *build) {
    if (!build)
        return;
    obj_ctx_destroy(&build->ctx);
    free(build->code_buf);
    free(build->data_buf);
    memset(build, 0, sizeof(*build));
}

static int write_object_payload(FILE *out, const lr_target_t *target,
                                const lr_obj_build_result_t *build) {
    if (!out || !target || !build)
        return -1;
#ifdef __APPLE__
    if (strcmp(target->name, "aarch64") == 0) {
        return write_macho(out, build->code_buf, build->code_pos,
                           build->has_data ? build->data_buf : NULL,
                           build->has_data ? build->data_pos : 0,
                           (lr_objfile_ctx_t *)&build->ctx, 0x0100000Cu,
                           macho_reloc_arm64);
    }
    return -1;
#else
    if (strcmp(target->name, "x86_64") == 0) {
        return write_elf(out, build->code_buf, build->code_pos,
                         build->has_data ? build->data_buf : NULL,
                         build->has_data ? build->data_pos : 0,
                         (lr_objfile_ctx_t *)&build->ctx, 62,
                         elf_reloc_x86_64);
    }
    if (strcmp(target->name, "aarch64") == 0) {
        return write_elf(out, build->code_buf, build->code_pos,
                         build->has_data ? build->data_buf : NULL,
                         build->has_data ? build->data_pos : 0,
                         (lr_objfile_ctx_t *)&build->ctx, 183,
                         elf_reloc_x86_64);
    }
    return -1;
#endif
}

#ifdef __APPLE__
static int run_process_quiet(char *const argv[]) {
    pid_t pid;
    int status = 0;
    if (!argv || !argv[0])
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

static int run_codesign_adhoc(const char *path) {
    char *const argv[] = {
        "/usr/bin/codesign", "--force", "--sign", "-", (char *)path, NULL
    };
    if (!path || !path[0])
        return -1;
    return run_process_quiet(argv);
}

static int copy_file_to_stream(const char *path, FILE *out) {
    FILE *in = NULL;
    uint8_t buf[4096];
    size_t nread;
    if (!path || !out)
        return -1;
    in = fopen(path, "rb");
    if (!in)
        return -1;
    while ((nread = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, nread, out) != nread) {
            fclose(in);
            return -1;
        }
    }
    if (ferror(in)) {
        fclose(in);
        return -1;
    }
    fclose(in);
    return fflush(out) == 0 ? 0 : -1;
}
#endif

static int obj_build_module(lr_module_t *m, const lr_target_t *target,
                            bool preserve_symbol_names,
                            lr_obj_build_result_t *out) {
    if (!m || !target || !out)
        return -1;

    memset(out, 0, sizeof(*out));
    out->code_buf = (uint8_t *)calloc(1, OBJ_CODE_BUF_SIZE);
    out->data_buf = (uint8_t *)calloc(1, OBJ_DATA_BUF_SIZE);
    if (!out->code_buf || !out->data_buf) {
        obj_build_result_destroy(out);
        return -1;
    }

    lr_arena_t *arena = lr_arena_create(0);
    if (!arena) {
        obj_build_result_destroy(out);
        return -1;
    }

    out->ctx.preserve_symbol_names = preserve_symbol_names;
    m->obj_ctx = &out->ctx;
    if (obj_build_module_symbol_cache(&out->ctx, m) != 0) {
        m->obj_ctx = NULL;
        lr_arena_destroy(arena);
        obj_build_result_destroy(out);
        return -1;
    }

    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->is_decl || !f->first_block)
            continue;

        uint32_t sym_idx = lr_obj_ensure_symbol(&out->ctx, f->name, true, 1,
                                                (uint32_t)out->code_pos);
        if (sym_idx == UINT32_MAX) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            obj_build_result_destroy(out);
            return -1;
        }

        uint32_t reloc_base = out->ctx.num_relocs;

        size_t func_len = 0;
        int rc = target->compile_func(f, m,
                                      out->code_buf + out->code_pos,
                                      OBJ_CODE_BUF_SIZE - out->code_pos,
                                      &func_len, arena);
        if (rc != 0) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            obj_build_result_destroy(out);
            return -1;
        }

        for (uint32_t ri = reloc_base; ri < out->ctx.num_relocs; ri++)
            out->ctx.relocs[ri].offset += (uint32_t)out->code_pos;

        out->code_pos += func_len;
    }

    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->is_decl && f->first_block)
            continue;
        if (lr_obj_ensure_symbol(&out->ctx, f->name, false, 0, 0) == UINT32_MAX) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            obj_build_result_destroy(out);
            return -1;
        }
    }

    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (g->is_external) {
            if (lr_obj_ensure_symbol(&out->ctx, g->name, false, 0, 0) == UINT32_MAX) {
                m->obj_ctx = NULL;
                lr_arena_destroy(arena);
                obj_build_result_destroy(out);
                return -1;
            }
            continue;
        }

        size_t gsize = lr_type_size(g->type);
        if (gsize == 0)
            gsize = 8;

        size_t galign = lr_type_align(g->type);
        if (galign == 0)
            galign = 8;

        out->data_pos = obj_align_up(out->data_pos, galign);

        if (out->data_pos + gsize > OBJ_DATA_BUF_SIZE) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            obj_build_result_destroy(out);
            return -1;
        }

        if (g->init_data && g->init_size > 0) {
            size_t copy_n = g->init_size < gsize ? g->init_size : gsize;
            memcpy(out->data_buf + out->data_pos, g->init_data, copy_n);
        }

        if (lr_obj_ensure_symbol(&out->ctx, g->name, true, 2,
                                 (uint32_t)out->data_pos) == UINT32_MAX) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            obj_build_result_destroy(out);
            return -1;
        }

        out->data_pos += gsize;
        out->has_data = true;
    }

    m->obj_ctx = NULL;
    lr_arena_destroy(arena);
    return 0;
}

int lr_emit_object(lr_module_t *m, const lr_target_t *target, FILE *out) {
    if (!m || !target || !out)
        return -1;

    lr_obj_build_result_t build;
    if (obj_build_module(m, target, false, &build) != 0)
        return -1;

    int result = write_object_payload(out, target, &build);

    obj_build_result_destroy(&build);
    return result;
}

int lr_emit_executable(lr_module_t *m, const lr_target_t *target, FILE *out,
                       const char *entry_symbol) {
    if (!m || !target || !out)
        return -1;
    if (!entry_symbol || !entry_symbol[0])
        entry_symbol = "main";

    lr_obj_build_result_t build;
    if (obj_build_module(m, target, true, &build) != 0)
        return -1;

    int result = -1;
#if defined(__linux__)
    if (strcmp(target->name, "x86_64") == 0) {
        result = write_elf_executable_x86_64(
            out, build.code_buf, build.code_pos,
            build.has_data ? build.data_buf : NULL,
            build.has_data ? build.data_pos : 0,
            &build.ctx, entry_symbol);
    }
#else
    if (strcmp(target->name, "aarch64") == 0) {
        char exe_tpl[] = "/tmp/liric_exe_XXXXXX";
        int exe_fd = -1;
        FILE *exe_out = NULL;
        int sign_rc;
        int copy_rc;

        exe_fd = mkstemp(exe_tpl);
        if (exe_fd < 0)
            goto done;
        exe_out = fdopen(exe_fd, "wb");
        if (!exe_out) {
            close(exe_fd);
            exe_fd = -1;
            goto done;
        }
        exe_fd = -1;

        result = write_macho_executable_arm64(
            exe_out, build.code_buf, build.code_pos,
            build.has_data ? build.data_buf : NULL,
            build.has_data ? build.data_pos : 0,
            &build.ctx, entry_symbol
        );
        if (fclose(exe_out) != 0)
            result = -1;
        exe_out = NULL;
        if (result != 0)
            goto done;

        sign_rc = run_codesign_adhoc(exe_tpl);
        if (sign_rc != 0) {
            result = -1;
            goto done;
        }

        copy_rc = copy_file_to_stream(exe_tpl, out);
        if (copy_rc != 0) {
            result = -1;
            goto done;
        }
        result = 0;

done:
        if (exe_out)
            fclose(exe_out);
        if (exe_fd >= 0)
            close(exe_fd);
        unlink(exe_tpl);
    }
#endif

    obj_build_result_destroy(&build);
    return result;
}
