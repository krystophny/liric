#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

#include <liric/liric_legacy.h>
#include <liric/liric_compat.h>

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

static inline llvm::LoadInst *compat_CreateLoad(
        llvm::IRBuilder<> &b, llvm::Type *ty, llvm::Value *ptr,
        const char *name = "") {
#if LLVM_VERSION_MAJOR >= 11
    return b.CreateLoad(ty, ptr, name);
#else
    (void)ty;
    return b.CreateLoad(ptr, name);
#endif
}

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dlfcn.h>
#endif

extern "C" int lr_llvm_jit_is_available(void);

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

static int run_exe_capture_stdout(const std::string &path, int expect_rc,
                                  std::string &stdout_out) {
#if defined(__unix__) || defined(__APPLE__)
    char capture_path[] = "/tmp/liric_llvm_stdout_XXXXXX";
    int capture_fd = mkstemp(capture_path);
    int status = 0;
    if (capture_fd < 0) {
        std::fprintf(stderr, "mkstemp failed: %s\n", std::strerror(errno));
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "fork failed: %s\n", std::strerror(errno));
        close(capture_fd);
        unlink(capture_path);
        return 1;
    }
    if (pid == 0) {
        if (dup2(capture_fd, STDOUT_FILENO) < 0)
            _exit(127);
        close(capture_fd);
        execl(path.c_str(), path.c_str(), (char *)NULL);
        _exit(127);
    }

    close(capture_fd);
    if (waitpid(pid, &status, 0) < 0) {
        std::fprintf(stderr, "waitpid failed: %s\n", std::strerror(errno));
        unlink(capture_path);
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expect_rc) {
        unlink(capture_path);
        return 1;
    }

    FILE *fp = std::fopen(capture_path, "rb");
    if (!fp) {
        std::fprintf(stderr, "fopen failed: %s\n", std::strerror(errno));
        unlink(capture_path);
        return 1;
    }
    char buf[256];
    stdout_out.clear();
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), fp);
        if (n > 0)
            stdout_out.append(buf, n);
        if (n < sizeof(buf))
            break;
    }
    std::fclose(fp);
    unlink(capture_path);
    return 0;
#else
    (void)path;
    (void)expect_rc;
    stdout_out.clear();
    return 0;
#endif
}

static void build_main_ret42_module(llvm::Module &mod, llvm::LLVMContext &ctx) {
    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::FunctionType *fty = llvm::FunctionType::get(i32, false);
    llvm::Function *main_fn = llvm::Function::Create(
        fty, llvm::GlobalValue::ExternalLinkage, "main", mod);
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", main_fn);
    llvm::IRBuilder<> b(entry);
    b.CreateRet(llvm::ConstantInt::get(i32, 42));
}

static void build_main_ret_private_global_module(llvm::Module &mod,
                                                 llvm::LLVMContext &ctx) {
    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Constant *init_val = llvm::ConstantInt::get(i32, 42);
    auto *answer = new llvm::GlobalVariable(
        mod, i32, true, llvm::GlobalValue::InternalLinkage,
        init_val, "private_answer");
    (void)answer;

    llvm::FunctionType *fty = llvm::FunctionType::get(i32, false);
    llvm::Function *main_fn = llvm::Function::Create(
        fty, llvm::GlobalValue::ExternalLinkage, "main", mod);
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", main_fn);
    llvm::IRBuilder<> b(entry);
    llvm::Value *loaded = compat_CreateLoad(b, i32, answer, "loaded");
    b.CreateRet(loaded);
}

static void build_main_ret_duplicate_private_global_module(llvm::Module &mod,
                                                           llvm::LLVMContext &ctx) {
    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Constant *init_a = llvm::ConstantInt::get(i32, 7);
    llvm::Constant *init_b = llvm::ConstantInt::get(i32, 42);
    auto *first = new llvm::GlobalVariable(
        mod, i32, true, llvm::GlobalValue::InternalLinkage,
        init_a, "dup_private_answer");
    (void)first;
    auto *second = new llvm::GlobalVariable(
        mod, i32, true, llvm::GlobalValue::InternalLinkage,
        init_b, "dup_private_answer");

    llvm::FunctionType *fty = llvm::FunctionType::get(i32, false);
    llvm::Function *main_fn = llvm::Function::Create(
        fty, llvm::GlobalValue::ExternalLinkage, "main", mod);
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", main_fn);
    llvm::IRBuilder<> b(entry);
    llvm::Value *loaded = compat_CreateLoad(b, i32, second, "loaded");
    b.CreateRet(loaded);
}

static void build_main_puts_module(llvm::Module &mod, llvm::LLVMContext &ctx) {
    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i8 = llvm::Type::getInt8Ty(ctx);
#if LLVM_VERSION_MAJOR >= 21
    llvm::PointerType *i8ptr = llvm::PointerType::getUnqual(ctx);
#else
    llvm::PointerType *i8ptr = llvm::PointerType::getUnqual(i8);
#endif
    (void)i8;

    llvm::FunctionType *puts_ty = llvm::FunctionType::get(i32, {i8ptr}, false);
    llvm::Function *puts_fn = llvm::Function::Create(
        puts_ty, llvm::GlobalValue::ExternalLinkage, "puts", mod);

    llvm::FunctionType *main_ty = llvm::FunctionType::get(i32, false);
    llvm::Function *main_fn = llvm::Function::Create(
        main_ty, llvm::GlobalValue::ExternalLinkage, "main", mod);
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", main_fn);
    llvm::IRBuilder<> b(entry);

#if LLVM_VERSION_MAJOR >= 20
    llvm::Value *msg = b.CreateGlobalString("HHH", "puts_msg");
#else
    llvm::Value *msg = b.CreateGlobalStringPtr("HHH", "puts_msg");
#endif
#if LLVM_VERSION_MAJOR >= 11
    b.CreateCall(puts_ty, puts_fn, {msg});
#else
    b.CreateCall(puts_fn, {msg});
#endif
    b.CreateRet(llvm::ConstantInt::get(i32, 0));
}

static int test_wrapper_object_emit_mode_llvm(void) {
    llvm::LLVMContext ctx;
    lc_context_set_backend(ctx.impl(), LC_BACKEND_LLVM);
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
    try {
        (void)pm.run(mod);
    } catch (...) {
        TEST_ASSERT(false, "pass manager run");
    }
    out.flush();

#if defined(__unix__) || defined(__APPLE__)
    struct stat st;
    std::memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQ(stat(obj_path.c_str(), &st), 0, "object stat");
    TEST_ASSERT(st.st_size > 0, "object non-empty");
#endif

    std::remove(obj_path.c_str());
    return 0;
}

static int test_wrapper_to_api_executable_roundtrip(void) {
    if (!lr_llvm_jit_is_available())
        return 0;

    llvm::LLVMContext ctx;
    lc_context_set_backend(ctx.impl(), LC_BACKEND_LLVM);
    llvm::Module mod("roundtrip_exe", ctx);
    build_main_ret42_module(mod, ctx);
    std::string exe_path = make_temp_path("exe");

    int rc = lc_module_emit_executable(mod.getCompat(), exe_path.c_str());
    TEST_ASSERT_EQ(rc, 0, "compat executable emission");

#if defined(__unix__) || defined(__APPLE__)
    rc = run_exe_expect(exe_path, 42);
    TEST_ASSERT_EQ(rc, 0, "emitted executable exits with 42");
#endif

    std::remove(exe_path.c_str());
    return 0;
}

static int test_wrapper_to_api_executable_roundtrip_with_private_global(void) {
    if (!lr_llvm_jit_is_available())
        return 0;

    llvm::LLVMContext ctx;
    lc_context_set_backend(ctx.impl(), LC_BACKEND_LLVM);
    llvm::Module mod("roundtrip_exe_private_global", ctx);
    build_main_ret_private_global_module(mod, ctx);
    std::string exe_path = make_temp_path("exe_private_global");

    int rc = lc_module_emit_executable(mod.getCompat(), exe_path.c_str());
    TEST_ASSERT_EQ(rc, 0, "compat executable emission with private global");

#if defined(__unix__) || defined(__APPLE__)
    rc = run_exe_expect(exe_path, 42);
    TEST_ASSERT_EQ(rc, 0, "private-global executable exits with 42");
#endif

    std::remove(exe_path.c_str());
    return 0;
}

static int test_wrapper_to_api_executable_roundtrip_with_duplicate_private_globals(void) {
    if (!lr_llvm_jit_is_available())
        return 0;

    llvm::LLVMContext ctx;
    lc_context_set_backend(ctx.impl(), LC_BACKEND_LLVM);
    llvm::Module mod("roundtrip_exe_duplicate_private_globals", ctx);
    build_main_ret_duplicate_private_global_module(mod, ctx);
    std::string exe_path = make_temp_path("exe_duplicate_private_globals");

    int rc = lc_module_emit_executable(mod.getCompat(), exe_path.c_str());
    TEST_ASSERT_EQ(rc, 0, "compat executable emission with duplicate private globals");

#if defined(__unix__) || defined(__APPLE__)
    rc = run_exe_expect(exe_path, 42);
    TEST_ASSERT_EQ(rc, 0, "duplicate-private-global executable exits with 42");
#endif

    std::remove(exe_path.c_str());
    return 0;
}

static int test_wrapper_to_api_executable_roundtrip_flushes_stdout(void) {
    if (!lr_llvm_jit_is_available())
        return 0;

    llvm::LLVMContext ctx;
    lc_context_set_backend(ctx.impl(), LC_BACKEND_LLVM);
    llvm::Module mod("roundtrip_exe_stdout", ctx);
    build_main_puts_module(mod, ctx);
    std::string exe_path = make_temp_path("exe_stdout");

    int rc = lc_module_emit_executable(mod.getCompat(), exe_path.c_str());
    TEST_ASSERT_EQ(rc, 0, "compat executable emission with stdout");

#if defined(__unix__) || defined(__APPLE__)
    std::string stdout_text;
    rc = run_exe_capture_stdout(exe_path, 0, stdout_text);
    TEST_ASSERT_EQ(rc, 0, "stdout executable exits with 0");
    TEST_ASSERT(stdout_text == "HHH\n", "stdout from emitted executable");
#endif

    std::remove(exe_path.c_str());
    return 0;
}

static int test_wrapper_jit_mode_llvm(void) {
    if (!lr_llvm_jit_is_available())
        return 0;

    llvm::LLVMContext ctx;
    lc_context_set_backend(ctx.impl(), LC_BACKEND_LLVM);
    llvm::Module mod("roundtrip_jit", ctx);
    build_main_ret42_module(mod, ctx);

    lr_jit_t *jit = lr_jit_create();
    if (!jit) {
        std::fprintf(stderr, "  FAIL: jit create\n");
        return 1;
    }

    int rc = lc_module_add_to_jit(mod.getCompat(), jit);
    if (rc != 0) {
        std::fprintf(stderr, "  FAIL: add module to jit\n");
        lr_jit_destroy(jit);
        return 1;
    }

    void *main_addr = lr_jit_get_function(jit, "main");
    if (!main_addr) {
        std::fprintf(stderr, "  FAIL: jit lookup main\n");
        lr_jit_destroy(jit);
        return 1;
    }
#if defined(__unix__) || defined(__APPLE__)
    void *host_main = dlsym(nullptr, "main");
    if (host_main && main_addr == host_main) {
        std::fprintf(stderr, "  FAIL: jit lookup resolved host process main\n");
        lr_jit_destroy(jit);
        return 1;
    }
#endif

    int (*fn)(void) = (int (*)(void))main_addr;
    if (fn() != 42) {
        std::fprintf(stderr, "  FAIL: jit main returned non-42\n");
        lr_jit_destroy(jit);
        return 1;
    }

    lr_jit_destroy(jit);
    return 0;
}

int main(void) {
    std::fprintf(stderr, "llvm backend roundtrip tests\n");
    std::fprintf(stderr, "============================\n\n");

    RUN_TEST(test_wrapper_object_emit_mode_llvm);
    RUN_TEST(test_wrapper_to_api_executable_roundtrip);
    RUN_TEST(test_wrapper_to_api_executable_roundtrip_with_private_global);
    RUN_TEST(test_wrapper_to_api_executable_roundtrip_with_duplicate_private_globals);
    RUN_TEST(test_wrapper_to_api_executable_roundtrip_flushes_stdout);
    RUN_TEST(test_wrapper_jit_mode_llvm);

    std::fprintf(stderr, "\nSummary: %d/%d passed, %d failed\n",
                 tests_passed, tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
