#include "llvm_backend.h"

#include "liric.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(LIRIC_HAVE_REAL_LLVM_BACKEND) && LIRIC_HAVE_REAL_LLVM_BACKEND

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#define LR_LLVM_BACKEND_CAN_LINK 1
#else
#define LR_LLVM_BACKEND_CAN_LINK 0
#endif

#define LR_LLVM_ERRBUF_DEFAULT 512

static void set_err(char *err, size_t err_cap, const char *fmt, ...) {
    va_list ap;
    if (!err || err_cap == 0)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_cap, fmt, ap);
    va_end(ap);
}

static char *read_file_to_buf(FILE *f, size_t *out_len) {
    long end_pos;
    size_t nread;
    char *buf = NULL;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
        return NULL;
    end_pos = ftell(f);
    if (end_pos < 0)
        return NULL;
    if (fseek(f, 0, SEEK_SET) != 0)
        return NULL;

    buf = (char *)malloc((size_t)end_pos + 1u);
    if (!buf)
        return NULL;
    nread = fread(buf, 1, (size_t)end_pos, f);
    if (nread != (size_t)end_pos) {
        free(buf);
        return NULL;
    }
    buf[nread] = '\0';
    if (out_len)
        *out_len = nread;
    return buf;
}

static char *module_to_ll_text(lr_module_t *m, size_t *out_len) {
    FILE *tmp = NULL;
    char *buf = NULL;
    if (!m)
        return NULL;
    tmp = tmpfile();
    if (!tmp)
        return NULL;
    lr_module_dump(m, tmp);
    fflush(tmp);
    buf = read_file_to_buf(tmp, out_len);
    fclose(tmp);
    return buf;
}

static lr_module_t *clone_module(lr_module_t *m, char *err, size_t err_cap) {
    char parse_err[256] = {0};
    size_t ll_len = 0;
    char *ll = module_to_ll_text(m, &ll_len);
    lr_module_t *out = NULL;
    if (!ll) {
        set_err(err, err_cap, "failed to dump module to text");
        return NULL;
    }
    out = lr_parse_ll(ll, ll_len, parse_err, sizeof(parse_err));
    free(ll);
    if (!out) {
        set_err(err, err_cap, "failed to parse dumped module: %s",
                parse_err[0] ? parse_err : "unknown parse error");
        return NULL;
    }
    return out;
}

static int merge_runtime_into(lr_module_t *work, const char *runtime_ll,
                              size_t runtime_len, char *err, size_t err_cap) {
    char parse_err[256] = {0};
    lr_module_t *rt = NULL;
    if (!runtime_ll || runtime_len == 0)
        return 0;
    rt = lr_parse_ll(runtime_ll, runtime_len, parse_err, sizeof(parse_err));
    if (!rt) {
        set_err(err, err_cap, "failed to parse runtime ll: %s",
                parse_err[0] ? parse_err : "unknown parse error");
        return -1;
    }
    if (lr_module_merge(work, rt) != 0) {
        lr_module_free(rt);
        set_err(err, err_cap, "failed to merge runtime module");
        return -1;
    }
    lr_module_free(rt);
    return 0;
}

/* Emit a simple wrapper main() that calls entry_symbol() with no args. */
static int add_entry_wrapper_if_needed(lr_module_t *work, const char *entry_symbol,
                                       char *err, size_t err_cap) {
    char wrapper[512];
    char parse_err[256] = {0};
    lr_module_t *w = NULL;
    if (!entry_symbol || entry_symbol[0] == '\0' ||
        strcmp(entry_symbol, "main") == 0)
        return 0;

    if (snprintf(wrapper, sizeof(wrapper),
                 "declare i32 @%s()\n"
                 "define i32 @main() {\n"
                 "entry:\n"
                 "  %%ret = call i32 @%s()\n"
                 "  ret i32 %%ret\n"
                 "}\n",
                 entry_symbol, entry_symbol) >= (int)sizeof(wrapper)) {
        set_err(err, err_cap, "entry wrapper generation failed (name too long)");
        return -1;
    }
    w = lr_parse_ll(wrapper, strlen(wrapper), parse_err, sizeof(parse_err));
    if (!w) {
        set_err(err, err_cap, "failed to parse generated main wrapper: %s",
                parse_err[0] ? parse_err : "unknown parse error");
        return -1;
    }
    if (lr_module_merge(work, w) != 0) {
        lr_module_free(w);
        set_err(err, err_cap, "failed to merge main wrapper");
        return -1;
    }
    lr_module_free(w);
    return 0;
}

static int ensure_llvm_target_init(char *err, size_t err_cap) {
    if (LLVMInitializeNativeTarget() != 0) {
        set_err(err, err_cap, "LLVMInitializeNativeTarget failed");
        return -1;
    }
    if (LLVMInitializeNativeAsmPrinter() != 0) {
        set_err(err, err_cap, "LLVMInitializeNativeAsmPrinter failed");
        return -1;
    }
    if (LLVMInitializeNativeAsmParser() != 0) {
        set_err(err, err_cap, "LLVMInitializeNativeAsmParser failed");
        return -1;
    }
    return 0;
}

static int emit_object_from_ll_text(const char *ll, size_t ll_len,
                                    const char *path, char *err,
                                    size_t err_cap) {
    LLVMContextRef ctx = NULL;
    LLVMMemoryBufferRef mem = NULL;
    LLVMModuleRef mod = NULL;
    LLVMTargetRef target = NULL;
    LLVMTargetMachineRef tm = NULL;
    LLVMTargetDataRef td = NULL;
    char *triple = NULL;
    char *msg = NULL;
    char *dl = NULL;
    int rc = -1;

    if (!ll || ll_len == 0 || !path) {
        set_err(err, err_cap, "invalid LLVM object emission arguments");
        return -1;
    }
    if (ensure_llvm_target_init(err, err_cap) != 0)
        return -1;

    triple = LLVMGetDefaultTargetTriple();
    if (!triple || triple[0] == '\0') {
        set_err(err, err_cap, "LLVMGetDefaultTargetTriple failed");
        goto done;
    }
    if (LLVMGetTargetFromTriple(triple, &target, &msg) != 0 || !target) {
        set_err(err, err_cap, "LLVMGetTargetFromTriple failed: %s",
                msg ? msg : "unknown error");
        goto done;
    }

    tm = LLVMCreateTargetMachine(target, triple, "generic", "",
                                 LLVMCodeGenLevelDefault,
                                 LLVMRelocPIC,
                                 LLVMCodeModelDefault);
    if (!tm) {
        set_err(err, err_cap, "LLVMCreateTargetMachine failed");
        goto done;
    }

    ctx = LLVMContextCreate();
    if (!ctx) {
        set_err(err, err_cap, "LLVMContextCreate failed");
        goto done;
    }
    mem = LLVMCreateMemoryBufferWithMemoryRangeCopy(ll, ll_len, "liric_module");
    if (!mem) {
        set_err(err, err_cap, "LLVMCreateMemoryBufferWithMemoryRangeCopy failed");
        goto done;
    }

    if (LLVMParseIRInContext(ctx, mem, &mod, &msg) != 0 || !mod) {
        set_err(err, err_cap, "LLVMParseIRInContext failed: %s",
                msg ? msg : "unknown parse error");
        goto done;
    }
    /* Ownership transfers to the module on parse success. */
    mem = NULL;

    LLVMSetTarget(mod, triple);
    td = LLVMCreateTargetDataLayout(tm);
    if (!td) {
        set_err(err, err_cap, "LLVMCreateTargetDataLayout failed");
        goto done;
    }
    dl = LLVMCopyStringRepOfTargetData(td);
    if (!dl) {
        set_err(err, err_cap, "LLVMCopyStringRepOfTargetData failed");
        goto done;
    }
    LLVMSetDataLayout(mod, dl);

    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &msg) != 0) {
        set_err(err, err_cap, "LLVMVerifyModule failed: %s",
                msg ? msg : "verify failure");
        goto done;
    }

    if (LLVMTargetMachineEmitToFile(tm, mod, (char *)path,
                                    LLVMObjectFile, &msg) != 0) {
        set_err(err, err_cap, "LLVMTargetMachineEmitToFile failed: %s",
                msg ? msg : "emit failure");
        goto done;
    }

    rc = 0;

done:
    if (msg)
        LLVMDisposeMessage(msg);
    if (dl)
        LLVMDisposeMessage(dl);
    if (td)
        LLVMDisposeTargetData(td);
    if (mod)
        LLVMDisposeModule(mod);
    if (mem)
        LLVMDisposeMemoryBuffer(mem);
    if (ctx)
        LLVMContextDispose(ctx);
    if (tm)
        LLVMDisposeTargetMachine(tm);
    if (triple)
        LLVMDisposeMessage(triple);
    return rc;
}

#if LR_LLVM_BACKEND_CAN_LINK
static int link_executable_from_object(const char *obj_path, const char *out_path,
                                       char *err, size_t err_cap) {
    const char *cc_env = getenv("CC");
    const char *cc = (cc_env && cc_env[0]) ? cc_env : "cc";
    pid_t pid;
    int status = 0;
    char *const argv[] = {
        (char *)cc,
        (char *)"-o",
        (char *)out_path,
        (char *)obj_path,
        NULL
    };

    pid = fork();
    if (pid < 0) {
        set_err(err, err_cap, "fork failed while linking executable");
        return -1;
    }
    if (pid == 0) {
        execvp(cc, argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        set_err(err, err_cap, "waitpid failed while linking executable");
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        set_err(err, err_cap, "linker failed with status=%d", status);
        return -1;
    }
    return 0;
}
#endif

int lr_llvm_backend_is_available(void) {
    return 1;
}

int lr_llvm_emit_object_path(lr_module_t *m, const lr_target_t *target,
                             const char *path, char *err, size_t err_cap) {
    const lr_target_t *host = lr_target_host();
    size_t ll_len = 0;
    char *ll = NULL;
    int rc;

    if (!err_cap)
        err_cap = LR_LLVM_ERRBUF_DEFAULT;
    if (err && err_cap)
        err[0] = '\0';

    if (!m || !path || !target) {
        set_err(err, err_cap, "invalid object emission inputs");
        return -1;
    }
    if (!host || strcmp(host->name, target->name) != 0) {
        set_err(err, err_cap, "llvm backend currently supports host target only");
        return -1;
    }

    ll = module_to_ll_text(m, &ll_len);
    if (!ll) {
        set_err(err, err_cap, "failed to materialize module text");
        return -1;
    }
    rc = emit_object_from_ll_text(ll, ll_len, path, err, err_cap);
    free(ll);
    return rc;
}

int lr_llvm_emit_executable_path(lr_module_t *m, const char *runtime_ll,
                                 size_t runtime_len,
                                 const lr_target_t *target,
                                 const char *path,
                                 const char *entry_symbol,
                                 char *err, size_t err_cap) {
#if !LR_LLVM_BACKEND_CAN_LINK
    (void)m;
    (void)runtime_ll;
    (void)runtime_len;
    (void)target;
    (void)path;
    (void)entry_symbol;
    set_err(err, err_cap, "llvm backend executable linking is unsupported on this platform");
    return -1;
#else
    char obj_tpl[] = "/tmp/liric_llvm_obj_XXXXXX";
    int obj_fd = -1;
    lr_module_t *work = NULL;
    int rc = -1;

    if (!err_cap)
        err_cap = LR_LLVM_ERRBUF_DEFAULT;
    if (err && err_cap)
        err[0] = '\0';

    if (!m || !path || !target) {
        set_err(err, err_cap, "invalid executable emission inputs");
        return -1;
    }

    work = clone_module(m, err, err_cap);
    if (!work)
        return -1;
    if (merge_runtime_into(work, runtime_ll, runtime_len, err, err_cap) != 0)
        goto done;
    if (add_entry_wrapper_if_needed(work, entry_symbol, err, err_cap) != 0)
        goto done;

    obj_fd = mkstemp(obj_tpl);
    if (obj_fd < 0) {
        set_err(err, err_cap, "mkstemp failed for temporary object");
        goto done;
    }
    close(obj_fd);
    obj_fd = -1;

    if (lr_llvm_emit_object_path(work, target, obj_tpl, err, err_cap) != 0)
        goto done;
    if (link_executable_from_object(obj_tpl, path, err, err_cap) != 0)
        goto done;

    rc = 0;

done:
    if (obj_fd >= 0)
        close(obj_fd);
    unlink(obj_tpl);
    if (work)
        lr_module_free(work);
    return rc;
#endif
}

#else

int lr_llvm_backend_is_available(void) {
    return 0;
}

int lr_llvm_emit_object_path(lr_module_t *m, const lr_target_t *target,
                             const char *path, char *err, size_t err_cap) {
    (void)m;
    (void)target;
    (void)path;
    if (err && err_cap > 0)
        (void)snprintf(err, err_cap, "real llvm backend is not enabled");
    return -1;
}

int lr_llvm_emit_executable_path(lr_module_t *m, const char *runtime_ll,
                                 size_t runtime_len,
                                 const lr_target_t *target,
                                 const char *path,
                                 const char *entry_symbol,
                                 char *err, size_t err_cap) {
    (void)m;
    (void)runtime_ll;
    (void)runtime_len;
    (void)target;
    (void)path;
    (void)entry_symbol;
    if (err && err_cap > 0)
        (void)snprintf(err, err_cap, "real llvm backend is not enabled");
    return -1;
}

#endif
