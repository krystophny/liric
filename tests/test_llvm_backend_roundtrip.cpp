#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

#include <liric/liric_legacy.h>
#include <llvm-c/LiricCompat.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        std::fprintf(stderr, "  FAIL: %s: got %lld, expected %lld (line %d)\n", \
                     msg, _a, _b, __LINE__); \
        return 1; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    tests_run++; \
    std::fprintf(stderr, "  %s...", #fn); \
    if (fn() == 0) { \
        tests_passed++; \
        std::fprintf(stderr, " ok\n"); \
    } else { \
        tests_failed++; \
        std::fprintf(stderr, "\n"); \
    } \
} while (0)

static std::string make_temp_path(const char *prefix) {
#if defined(__unix__) || defined(__APPLE__)
    char path[] = "/tmp/liric_llvm_roundtrip_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) {
        close(fd);
        unlink(path);
        std::string out = std::string(path) + "_" + prefix;
        return out;
    }
#endif
    return std::string("./") + prefix;
}

static void restore_mode_env(const char *prev) {
#if defined(_WIN32)
    if (prev && prev[0]) {
        (void)_putenv_s("LIRIC_COMPILE_MODE", prev);
    } else {
        (void)_putenv_s("LIRIC_COMPILE_MODE", "");
    }
#else
    if (prev && prev[0]) {
        (void)setenv("LIRIC_COMPILE_MODE", prev, 1);
    } else {
        (void)unsetenv("LIRIC_COMPILE_MODE");
    }
#endif
}

static void set_mode_env(const char *value) {
#if defined(_WIN32)
    (void)_putenv_s("LIRIC_COMPILE_MODE", value ? value : "");
#else
    if (value)
        (void)setenv("LIRIC_COMPILE_MODE", value, 1);
    else
        (void)unsetenv("LIRIC_COMPILE_MODE");
#endif
}

static int run_exe_expect(const std::string &path, int expect_rc) {
#if defined(__unix__) || defined(__APPLE__)
    pid_t pid = fork();
    int status = 0;
    if (pid < 0) {
        std::fprintf(stderr, "fork failed: %s\n", std::strerror(errno));
        return 1;
    }
    if (pid == 0) {
        execl(path.c_str(), path.c_str(), (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        std::fprintf(stderr, "waitpid failed: %s\n", std::strerror(errno));
        return 1;
    }
    if (!WIFEXITED(status))
        return 1;
    return (WEXITSTATUS(status) == expect_rc) ? 0 : 1;
#else
    (void)path;
    (void)expect_rc;
    return 0;
#endif
}

static void build_main_ret42_module(llvm::Module &mod, llvm::LLVMContext &ctx) {
    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::FunctionType *fty = llvm::FunctionType::get(i32, false);
    llvm::Function *main_fn = llvm::Function::Create(
        fty, llvm::GlobalValue::ExternalLinkage, "main", mod);
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", main_fn);
    llvm::IRBuilder<> b(entry, ctx);
    b.CreateRet(llvm::ConstantInt::get(i32, 42));
}

static int test_wrapper_object_emit_mode_llvm(void) {
    const char *old_mode = std::getenv("LIRIC_COMPILE_MODE");
    char old_copy[64] = {0};
    if (old_mode)
        std::snprintf(old_copy, sizeof(old_copy), "%s", old_mode);
    set_mode_env("llvm");

    llvm::LLVMContext ctx;
    llvm::Module mod("roundtrip_obj", ctx);
    build_main_ret42_module(mod, ctx);
    std::string obj_path = make_temp_path("obj.o");
    std::error_code ec;
    llvm::raw_fd_ostream out(obj_path, ec);
    TEST_ASSERT(!ec, "open object output");

    llvm::legacy::PassManager pm;
    llvm::TargetMachine tm;
    bool cannot_emit = tm.addPassesToEmitFile(pm, out, nullptr,
                                              llvm::CodeGenFileType::ObjectFile);
    TEST_ASSERT(!cannot_emit, "target machine accepts object emission");
    bool ok = pm.run(mod);
    TEST_ASSERT(ok, "pass manager run");
    out.flush();

#if defined(__unix__) || defined(__APPLE__)
    struct stat st;
    std::memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQ(stat(obj_path.c_str(), &st), 0, "object stat");
    TEST_ASSERT(st.st_size > 0, "object non-empty");
#endif

    std::remove(obj_path.c_str());
    restore_mode_env(old_copy);
    return 0;
}

static int test_wrapper_to_api_executable_roundtrip(void) {
    const char *old_mode = std::getenv("LIRIC_COMPILE_MODE");
    char old_copy[64] = {0};
    if (old_mode)
        std::snprintf(old_copy, sizeof(old_copy), "%s", old_mode);
    set_mode_env("llvm");

    llvm::LLVMContext ctx;
    llvm::Module mod("roundtrip_exe", ctx);
    build_main_ret42_module(mod, ctx);
    std::string exe_path = make_temp_path("exe");

    static const char *runtime_ll =
        "define i32 @__lfortran_rt_dummy() {\n"
        "entry:\n"
        "  ret i32 0\n"
        "}\n";
    int rc = lc_module_emit_executable(mod.getCompat(), exe_path.c_str(),
                                       runtime_ll, std::strlen(runtime_ll));
    TEST_ASSERT_EQ(rc, 0, "compat executable emission");

#if defined(__unix__) || defined(__APPLE__)
    rc = run_exe_expect(exe_path, 42);
    TEST_ASSERT_EQ(rc, 0, "emitted executable exits with 42");
#endif

    std::remove(exe_path.c_str());
    restore_mode_env(old_copy);
    return 0;
}

static int test_wrapper_jit_mode_llvm(void) {
    const char *old_mode = std::getenv("LIRIC_COMPILE_MODE");
    char old_copy[64] = {0};
    if (old_mode)
        std::snprintf(old_copy, sizeof(old_copy), "%s", old_mode);
    set_mode_env("llvm");

    llvm::LLVMContext ctx;
    llvm::Module mod("roundtrip_jit", ctx);
    build_main_ret42_module(mod, ctx);

    lr_jit_t *jit = lr_jit_create();
    if (!jit) {
        std::fprintf(stderr, "  FAIL: jit create\n");
        restore_mode_env(old_copy);
        return 1;
    }

    int rc = lc_module_add_to_jit(mod.getCompat(), jit);
    if (rc != 0) {
        std::fprintf(stderr, "  FAIL: add module to jit\n");
        lr_jit_destroy(jit);
        restore_mode_env(old_copy);
        return 1;
    }

    void *main_addr = lr_jit_get_function(jit, "main");
    if (!main_addr) {
        std::fprintf(stderr, "  FAIL: jit lookup main\n");
        lr_jit_destroy(jit);
        restore_mode_env(old_copy);
        return 1;
    }

    int (*fn)(void) = (int (*)(void))main_addr;
    if (fn() != 42) {
        std::fprintf(stderr, "  FAIL: jit main returned non-42\n");
        lr_jit_destroy(jit);
        restore_mode_env(old_copy);
        return 1;
    }

    lr_jit_destroy(jit);
    restore_mode_env(old_copy);
    return 0;
}

int main(void) {
    std::fprintf(stderr, "llvm backend roundtrip tests\n");
    std::fprintf(stderr, "============================\n\n");

    RUN_TEST(test_wrapper_object_emit_mode_llvm);
    RUN_TEST(test_wrapper_to_api_executable_roundtrip);
    RUN_TEST(test_wrapper_jit_mode_llvm);

    std::fprintf(stderr, "\nSummary: %d/%d passed, %d failed\n",
                 tests_passed, tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
