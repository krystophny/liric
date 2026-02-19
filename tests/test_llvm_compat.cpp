#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

#include <liric/liric_compat.h>

#include <llvm/Config/llvm-config.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/Twine.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s: got %lld, expected %lld (line %d)\n", \
                msg, _a, _b, __LINE__); \
        return 1; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    tests_run++; \
    fprintf(stderr, "  %s...", #fn); \
    if (fn() == 0) { \
        tests_passed++; \
        fprintf(stderr, " ok\n"); \
    } else { \
        tests_failed++; \
        fprintf(stderr, "\n"); \
    } \
} while (0)

class ScopedEnvVar {
    const char *name_;
    char *old_value_;

public:
    ScopedEnvVar(const char *name, const char *value)
        : name_(name), old_value_(nullptr) {
        const char *current = std::getenv(name_);
        if (current) old_value_ = ::strdup(current);
        if (value) {
            setenv(name_, value, 1);
        } else {
            unsetenv(name_);
        }
    }

    ~ScopedEnvVar() {
        if (old_value_) {
            setenv(name_, old_value_, 1);
            std::free(old_value_);
            old_value_ = nullptr;
        } else {
            unsetenv(name_);
        }
    }
};

static int ret42_symbol_for_stringref_lookup(void) {
    return 42;
}

static lr_global_t *find_module_global_by_name(lr_module_t *ir,
                                               const char *name) {
    if (!ir || !name)
        return nullptr;
    for (lr_global_t *g = ir->first_global; g; g = g->next) {
        if (g->name && std::strcmp(g->name, name) == 0)
            return g;
    }
    return nullptr;
}

static int test_llvm_version() {
    TEST_ASSERT_EQ(LLVM_VERSION_MAJOR, 21, "version major");
    return 0;
}

static int test_context_and_module() {
    llvm::LLVMContext ctx;
    llvm::Module mod("test", ctx);

    TEST_ASSERT(mod.getCompat() != nullptr, "module created");
    TEST_ASSERT(mod.getIR() != nullptr, "ir module present");
    TEST_ASSERT(llvm::Module::getCurrentModule() == mod.getCompat(),
                "current module set");
    return 0;
}

static int test_basic_types() {
    llvm::LLVMContext ctx;
    llvm::Module mod("types", ctx);

    llvm::Type *voidTy = llvm::Type::getVoidTy(ctx);
    llvm::Type *floatTy = llvm::Type::getFloatTy(ctx);
    llvm::Type *doubleTy = llvm::Type::getDoubleTy(ctx);
    llvm::IntegerType *i1 = llvm::Type::getInt1Ty(ctx);
    llvm::IntegerType *i8 = llvm::Type::getInt8Ty(ctx);
    llvm::IntegerType *i16 = llvm::Type::getInt16Ty(ctx);
    llvm::IntegerType *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::IntegerType *i64 = llvm::Type::getInt64Ty(ctx);

    TEST_ASSERT(voidTy != nullptr, "void type");
    TEST_ASSERT(voidTy->isVoidTy(), "is void");
    TEST_ASSERT(floatTy->isFloatTy(), "is float");
    TEST_ASSERT(doubleTy->isDoubleTy(), "is double");
    TEST_ASSERT(i1->isIntegerTy(), "i1 is integer");
    TEST_ASSERT_EQ(i1->getBitWidth(), 1, "i1 width");
    TEST_ASSERT_EQ(i8->getBitWidth(), 8, "i8 width");
    TEST_ASSERT_EQ(i16->getBitWidth(), 16, "i16 width");
    TEST_ASSERT_EQ(i32->getBitWidth(), 32, "i32 width");
    TEST_ASSERT_EQ(i64->getBitWidth(), 64, "i64 width");

    llvm::PointerType *ptrTy = llvm::PointerType::getUnqual(ctx);
    TEST_ASSERT(ptrTy != nullptr, "ptr type");
    TEST_ASSERT(ptrTy->isPointerTy(), "is pointer");

    return 0;
}

static int test_type_context_stability_across_nested_modules() {
    llvm::LLVMContext ctx_a;
    llvm::Module mod_a("ctx_a", ctx_a);

    llvm::Type *a_i8 = llvm::Type::getInt8Ty(ctx_a);
    llvm::Type *a_i8_ptr = a_i8 ? a_i8->getPointerTo() : nullptr;
    TEST_ASSERT(a_i8 != nullptr, "ctx_a i8");
    TEST_ASSERT(a_i8_ptr != nullptr, "ctx_a i8*");

    {
        llvm::LLVMContext ctx_b;
        llvm::Module mod_b("ctx_b", ctx_b);
        llvm::Type *b_i8 = llvm::Type::getInt8Ty(ctx_b);
        llvm::Type *b_i8_ptr = b_i8 ? b_i8->getPointerTo() : nullptr;
        TEST_ASSERT(b_i8 != nullptr, "ctx_b i8");
        TEST_ASSERT(b_i8_ptr != nullptr, "ctx_b i8*");
        TEST_ASSERT(a_i8 != b_i8, "contexts must keep distinct i8 identities");
        TEST_ASSERT(a_i8_ptr != b_i8_ptr, "contexts must keep distinct i8* identities");
    }

    llvm::Type *a_i8_after = llvm::Type::getInt8Ty(ctx_a);
    llvm::Type *a_i8_ptr_after = a_i8_after ? a_i8_after->getPointerTo() : nullptr;
    TEST_ASSERT(a_i8_after == a_i8, "ctx_a i8 identity stable after nested module");
    TEST_ASSERT(a_i8_ptr_after == a_i8_ptr, "ctx_a i8* identity stable after nested module");
    return 0;
}

static int test_function_type() {
    llvm::LLVMContext ctx;
    llvm::Module mod("ftypes", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *params[] = {i32, i32};
    llvm::FunctionType *ft = llvm::FunctionType::get(i32, params, false);

    TEST_ASSERT(ft != nullptr, "func type created");
    TEST_ASSERT_EQ(ft->getNumParams(), 2, "param count");
    TEST_ASSERT(!ft->isVarArg(), "not vararg");

    llvm::FunctionType *fv = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), false);
    TEST_ASSERT(fv != nullptr, "void func type");
    TEST_ASSERT(fv->getReturnType()->isVoidTy(), "returns void");

    llvm::Type *f32 = llvm::Type::getFloatTy(ctx);
    llvm::FixedVectorType *vec2 = llvm::FixedVectorType::get(f32, 2);
    TEST_ASSERT(vec2 != nullptr, "fixed vector type created");
    TEST_ASSERT(vec2->isVectorTy(), "fixed vector reports vector type");

    llvm::FunctionType *fv_ret = llvm::FunctionType::get(vec2, false);
    TEST_ASSERT(fv_ret != nullptr, "vector return func type");
    TEST_ASSERT(!fv_ret->getReturnType()->isVoidTy(), "vector return preserved");
    TEST_ASSERT(fv_ret->getReturnType()->isVectorTy(), "return type remains vector");

    return 0;
}

static int test_struct_type() {
    llvm::LLVMContext ctx;
    llvm::Module mod("structs", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);

    llvm::StructType *st = llvm::StructType::create(ctx, "my_struct");
    TEST_ASSERT(st != nullptr, "named struct created");
    TEST_ASSERT(st->isOpaque(), "initially opaque");

    llvm::Type *fields[] = {i32, i64};
    st->setBody(fields);
    TEST_ASSERT_EQ(st->getNumElements(), 2, "2 fields after setBody");
    TEST_ASSERT(st->getElementType(0)->isIntegerTy(32), "field 0 is i32");
    TEST_ASSERT(st->getElementType(1)->isIntegerTy(64), "field 1 is i64");

    llvm::StructType *lit = llvm::StructType::get(ctx, fields);
    TEST_ASSERT(lit != nullptr, "literal struct");
    TEST_ASSERT_EQ(lit->getNumElements(), 2, "literal 2 fields");

    return 0;
}

static int test_array_type() {
    llvm::LLVMContext ctx;
    llvm::Module mod("arrays", ctx);

    llvm::Type *i8 = llvm::Type::getInt8Ty(ctx);
    llvm::ArrayType *at = llvm::ArrayType::get(i8, 16);

    TEST_ASSERT(at != nullptr, "array type created");
    TEST_ASSERT(at->isArrayTy(), "is array");
    TEST_ASSERT_EQ(at->getNumElements(), 16, "16 elements");
    TEST_ASSERT(at->getElementType()->isIntegerTy(8), "element is i8");

    return 0;
}

static int test_constants() {
    llvm::LLVMContext ctx;
    llvm::Module mod("consts", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::ConstantInt *c42 = llvm::ConstantInt::get(i32, 42);

    TEST_ASSERT(c42 != nullptr, "const int created");
    TEST_ASSERT_EQ(c42->getSExtValue(), 42, "value is 42");
    TEST_ASSERT_EQ(c42->getZExtValue(), 42, "zext value is 42");

    llvm::ConstantInt *cn = llvm::ConstantInt::get(i32, -1, true);
    TEST_ASSERT_EQ(cn->getSExtValue(), -1, "signed -1");

    llvm::Type *dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::ConstantFP *cfp = llvm::ConstantFP::get(dblTy, 3.14);
    TEST_ASSERT(cfp != nullptr, "const fp created");

    llvm::PointerType *ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::ConstantPointerNull *cpn = llvm::ConstantPointerNull::get(ptrTy);
    TEST_ASSERT(cpn != nullptr, "null pointer");

    llvm::UndefValue *uv = llvm::UndefValue::get(i32);
    TEST_ASSERT(uv != nullptr, "undef value");

    llvm::Constant *nv = llvm::Constant::getNullValue(i32);
    TEST_ASSERT(nv != nullptr, "null value");

    return 0;
}

static int test_constant_data_array_addnull() {
    llvm::LLVMContext ctx;
    llvm::Module mod("const_data_array", ctx);

    llvm::Constant *c = llvm::ConstantDataArray::getString(ctx, "AB", true);
    TEST_ASSERT(c != nullptr, "ConstantDataArray::getString");
    TEST_ASSERT(c->impl()->kind == LC_VAL_CONST_AGGREGATE, "aggregate kind");
    TEST_ASSERT_EQ(c->impl()->aggregate.size, 3, "includes null terminator");
    const uint8_t *p = static_cast<const uint8_t *>(c->impl()->aggregate.data);
    TEST_ASSERT(p != nullptr, "aggregate data");
    TEST_ASSERT_EQ(p[0], 'A', "byte 0");
    TEST_ASSERT_EQ(p[1], 'B', "byte 1");
    TEST_ASSERT_EQ(p[2], 0, "byte 2 null");
    return 0;
}

static int test_constant_struct_and_array_bytes() {
    llvm::LLVMContext ctx;
    llvm::Module mod("const_aggregate_bytes", ctx);

    llvm::Type *i16 = llvm::Type::getInt16Ty(ctx);
    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *fields[] = {i32, i32};
    llvm::StructType *sty = llvm::StructType::get(ctx, fields);
    llvm::Constant *sv[] = {
        llvm::ConstantInt::get(i32, 1),
        llvm::ConstantInt::get(i32, 2),
    };
    llvm::Constant *sc = llvm::ConstantStruct::get(sty, sv);
    TEST_ASSERT(sc != nullptr, "constant struct created");
    TEST_ASSERT(sc->impl()->kind == LC_VAL_CONST_AGGREGATE, "struct aggregate kind");
    TEST_ASSERT_EQ(sc->impl()->aggregate.size, 8, "struct aggregate size");
    const uint8_t *sb = static_cast<const uint8_t *>(sc->impl()->aggregate.data);
    TEST_ASSERT(sb != nullptr, "struct aggregate data");
    TEST_ASSERT_EQ(sb[0], 1, "struct field0 byte0");
    TEST_ASSERT_EQ(sb[4], 2, "struct field1 byte0");

    llvm::ArrayType *aty = llvm::ArrayType::get(i16, 3);
    llvm::Constant *av[] = {
        llvm::ConstantInt::get(i16, 7),
        llvm::ConstantInt::get(i16, 8),
        llvm::ConstantInt::get(i16, 9),
    };
    llvm::Constant *ac = llvm::ConstantArray::get(aty, av);
    TEST_ASSERT(ac != nullptr, "constant array created");
    TEST_ASSERT(ac->impl()->kind == LC_VAL_CONST_AGGREGATE, "array aggregate kind");
    TEST_ASSERT_EQ(ac->impl()->aggregate.size, 6, "array aggregate size");
    const uint8_t *ab = static_cast<const uint8_t *>(ac->impl()->aggregate.data);
    TEST_ASSERT(ab != nullptr, "array aggregate data");
    TEST_ASSERT_EQ(ab[0], 7, "array element0 byte0");
    TEST_ASSERT_EQ(ab[2], 8, "array element1 byte0");
    TEST_ASSERT_EQ(ab[4], 9, "array element2 byte0");
    return 0;
}

static int test_constant_array_single_aggregate_payload_preserved() {
    llvm::LLVMContext ctx;
    llvm::Module mod("const_array_single_aggregate_payload", ctx);

    llvm::Type *i16 = llvm::Type::getInt16Ty(ctx);
    llvm::ArrayType *aty = llvm::ArrayType::get(i16, 3);
    llvm::Constant *elems[] = {
        llvm::ConstantInt::get(i16, 7),
        llvm::ConstantInt::get(i16, 8),
        llvm::ConstantInt::get(i16, 9),
    };
    llvm::Constant *src = llvm::ConstantArray::get(aty, elems);
    TEST_ASSERT(src != nullptr, "source constant array created");
    TEST_ASSERT(src->impl()->kind == LC_VAL_CONST_AGGREGATE, "source aggregate kind");
    TEST_ASSERT_EQ(src->impl()->aggregate.size, 6, "source aggregate size");

    llvm::Constant *single_value[] = {src};
    llvm::Constant *wrapped = llvm::ConstantArray::get(aty, single_value);
    TEST_ASSERT(wrapped != nullptr, "wrapped constant array created");
    TEST_ASSERT(wrapped->impl()->kind == LC_VAL_CONST_AGGREGATE, "wrapped aggregate kind");
    TEST_ASSERT_EQ(wrapped->impl()->aggregate.size, 6, "wrapped aggregate size");

    const uint8_t *src_bytes = static_cast<const uint8_t *>(src->impl()->aggregate.data);
    const uint8_t *wrapped_bytes = static_cast<const uint8_t *>(wrapped->impl()->aggregate.data);
    TEST_ASSERT(src_bytes != nullptr, "source aggregate bytes");
    TEST_ASSERT(wrapped_bytes != nullptr, "wrapped aggregate bytes");
    TEST_ASSERT(std::memcmp(src_bytes, wrapped_bytes, 6) == 0,
                "single aggregate array payload must be preserved");
    TEST_ASSERT_EQ(wrapped_bytes[0], 7, "wrapped element0 byte0");
    TEST_ASSERT_EQ(wrapped_bytes[2], 8, "wrapped element1 byte0");
    TEST_ASSERT_EQ(wrapped_bytes[4], 9, "wrapped element2 byte0");
    return 0;
}

static int test_parse_assembly_wrapper_fast_path() {
    llvm::LLVMContext ctx;
    llvm::SMDiagnostic err;
    const char *wrapper_ir =
        "declare i32 @main(i32, i8**)\n"
        "define i32 @__lfortran_jit_entry(i32 %argc, i8** %argv) {\n"
        "entry:\n"
        "  %ret = call i32 @main(i32 %argc, i8** %argv)\n"
        "  ret i32 %ret\n"
        "}\n";
    std::unique_ptr<llvm::Module> mod = llvm::parseAssemblyString(
        llvm::StringRef(wrapper_ir), err, ctx);
    TEST_ASSERT(mod != nullptr, "parseAssemblyString wrapper path");

    lr_module_t *ir = lc_module_get_ir(mod->getCompat());
    TEST_ASSERT(ir != nullptr, "parsed module available");
    bool found_main_decl = false;
    bool found_entry = false;
    for (lr_func_t *f = ir->first_func; f; f = f->next) {
        if (f->name && std::strcmp(f->name, "main") == 0)
            found_main_decl = f->is_decl;
        if (f->name && std::strcmp(f->name, "__lfortran_jit_entry") == 0)
            found_entry = !f->is_decl && f->first_block != nullptr;
    }
    TEST_ASSERT(found_main_decl, "wrapper main declaration");
    TEST_ASSERT(found_entry, "wrapper entry definition");
    return 0;
}

static int test_global_lookup_set_initializer_and_jit() {
    llvm::LLVMContext ctx;
    llvm::Module mod("global_init", ctx);
    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    lr_module_t *ir = lc_module_get_ir(mod.getCompat());
    TEST_ASSERT(ir != nullptr, "module ir");

    llvm::Constant *inserted = mod.getOrInsertGlobal("g", i32);
    TEST_ASSERT(inserted != nullptr, "getOrInsertGlobal");

    llvm::GlobalVariable *g = mod.getNamedGlobal("g");
    TEST_ASSERT(g != nullptr, "getNamedGlobal");
    TEST_ASSERT(!g->hasInitializer(), "no initializer initially");
    lr_global_t *ir_g = find_module_global_by_name(ir, "g");
    TEST_ASSERT(ir_g != nullptr, "global present in IR");
    TEST_ASSERT(ir_g->is_external, "global starts as declaration");

    g->setInitializer(llvm::ConstantInt::get(i32, 123));
    TEST_ASSERT(g->hasInitializer(), "initializer applied");
    TEST_ASSERT(!ir_g->is_external, "initializer materializes definition");
    TEST_ASSERT(ir_g->init_data != nullptr, "initializer bytes present");

    llvm::FunctionType *ft = llvm::FunctionType::get(i32, false);
    llvm::Function *fn = llvm::Function::Create(
        ft, llvm::GlobalValue::ExternalLinkage, "read_g", mod);
    TEST_ASSERT(fn != nullptr, "function created");
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    TEST_ASSERT(entry != nullptr, "entry block");

    llvm::IRBuilder<> builder(entry);
    llvm::Value *v = builder.CreateLoad(i32, g, "v");
    TEST_ASSERT(v != nullptr, "load global");
    builder.CreateRet(v);

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef int (*fn_t)(void);
    fn_t fp = (fn_t)jit.lookup("read_g");
    TEST_ASSERT(fp != nullptr, "lookup read_g");
    TEST_ASSERT_EQ(fp(), 123, "global initializer value");
    return 0;
}

static int test_create_global_without_initializer_is_declaration() {
    llvm::LLVMContext ctx;
    llvm::Module mod("global_decl", ctx);
    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    lr_module_t *ir = lc_module_get_ir(mod.getCompat());
    TEST_ASSERT(ir != nullptr, "module ir");

    llvm::GlobalVariable *g = mod.createGlobalVariable(
        "extern_only", i32, false, llvm::GlobalValue::ExternalLinkage);
    TEST_ASSERT(g != nullptr, "global declaration created");

    lr_global_t *ir_g = find_module_global_by_name(ir, "extern_only");
    TEST_ASSERT(ir_g != nullptr, "decl global in IR");
    TEST_ASSERT(ir_g->is_external, "decl global remains external");
    TEST_ASSERT(ir_g->init_data == nullptr, "decl global has no initializer");
    TEST_ASSERT_EQ(ir_g->init_size, 0u, "decl global has no init bytes");
    return 0;
}

static int test_duplicate_global_names_are_uniquified() {
    llvm::LLVMContext ctx;
    llvm::Module mod("global_unique_names", ctx);
    llvm::Type *i8 = llvm::Type::getInt8Ty(ctx);
    llvm::ArrayType *arr1 = llvm::ArrayType::get(i8, 1);
    const uint8_t a_init[1] = {'A'};
    const uint8_t b_init[1] = {'B'};

    llvm::GlobalVariable *ga = mod.createGlobalVariable(
        "dup_global", arr1, true, llvm::GlobalValue::ExternalLinkage,
        a_init, sizeof(a_init));
    llvm::GlobalVariable *gb = mod.createGlobalVariable(
        "dup_global", arr1, true, llvm::GlobalValue::ExternalLinkage,
        b_init, sizeof(b_init));
    TEST_ASSERT(ga != nullptr, "first duplicate global created");
    TEST_ASSERT(gb != nullptr, "second duplicate global created");

    lc_value_t *vga = llvm::detail::lookup_value_wrapper(ga);
    lc_value_t *vgb = llvm::detail::lookup_value_wrapper(gb);
    TEST_ASSERT(vga != nullptr, "first global wrapper value");
    TEST_ASSERT(vgb != nullptr, "second global wrapper value");
    TEST_ASSERT(vga->global.name != nullptr, "first global has name");
    TEST_ASSERT(vgb->global.name != nullptr, "second global has name");
    TEST_ASSERT(std::strcmp(vga->global.name, vgb->global.name) != 0,
                "duplicate global names are uniquified");

    lr_module_t *ir = lc_module_get_ir(mod.getCompat());
    TEST_ASSERT(ir != nullptr, "module ir");
    lr_global_t *ga_ir = nullptr;
    lr_global_t *gb_ir = nullptr;
    for (lr_global_t *g = ir->first_global; g; g = g->next) {
        if (!ga_ir && g->name && std::strcmp(g->name, vga->global.name) == 0)
            ga_ir = g;
        if (!gb_ir && g->name && std::strcmp(g->name, vgb->global.name) == 0)
            gb_ir = g;
    }
    TEST_ASSERT(ga_ir != nullptr, "first unique global present in IR");
    TEST_ASSERT(gb_ir != nullptr, "second unique global present in IR");
    TEST_ASSERT(ga_ir->init_data != nullptr, "first unique global initializer");
    TEST_ASSERT(gb_ir->init_data != nullptr, "second unique global initializer");
    TEST_ASSERT_EQ(((const uint8_t *)ga_ir->init_data)[0], (uint8_t)'A',
                   "first unique initializer byte");
    TEST_ASSERT_EQ(((const uint8_t *)gb_ir->init_data)[0], (uint8_t)'B',
                   "second unique initializer byte");
    return 0;
}

static int test_function_creation() {
    llvm::LLVMContext ctx;
    llvm::Module mod("funcs", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64, i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("add", ft, false);
    TEST_ASSERT(fn != nullptr, "function created");
    TEST_ASSERT_EQ(fn->arg_size(), 2, "2 args");

    llvm::Function *decl = mod.createFunction("ext_func", ft, true);
    TEST_ASSERT(decl != nullptr, "declaration created");

    return 0;
}

static int test_stringref_slice_module_symbol_lookup() {
    llvm::LLVMContext ctx;
    llvm::Module mod("slice_lookup", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::FunctionType *ft = llvm::FunctionType::get(i32, false);
    llvm::Function *fn = mod.createFunction("sum", ft, true);
    TEST_ASSERT(fn != nullptr, "function declaration created");

    const char fn_storage[] = {'s', 'u', 'm', 'X', '\0'};
    llvm::StringRef fn_name(fn_storage, 3);
    llvm::Function *resolved = mod.getFunction(fn_name);
    TEST_ASSERT(resolved == fn, "StringRef slice resolves function symbol exactly");

    const char global_storage[] = {'g', 'v', 'Z', '\0'};
    llvm::StringRef global_name(global_storage, 2);
    llvm::Constant *g1 = mod.getOrInsertGlobal(global_name, i32);
    llvm::Constant *g2 = mod.getOrInsertGlobal("gv", i32);
    TEST_ASSERT(g1 != nullptr, "global from StringRef slice");
    TEST_ASSERT(g2 != nullptr, "global from c-string");
    TEST_ASSERT_EQ(g1->impl()->global.id, g2->impl()->global.id,
                   "StringRef slice and c-string resolve same global");

    return 0;
}

static int test_block_parent_tracking_across_decls() {
    llvm::LLVMContext ctx;
    llvm::Module mod("parent_tracking", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *params[] = {i32};
    llvm::FunctionType *ft = llvm::FunctionType::get(i32, params, false);

    llvm::Function *main_fn = mod.createFunction("main_fn", ft, false);
    llvm::Function *ext_decl = mod.createFunction("ext_decl", ft, true);
    TEST_ASSERT(ext_decl != nullptr, "decl created");

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", nullptr);
    TEST_ASSERT(entry->impl_block() != nullptr, "implicit parent block created");
    TEST_ASSERT(entry->getParent() == main_fn, "decl must not clobber current function");

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(main_fn);
    builder.SetInsertPoint(entry);

    llvm::BasicBlock *insert_block = builder.GetInsertBlock();
    TEST_ASSERT(insert_block != nullptr, "insert block");
    TEST_ASSERT(insert_block->getParent() == main_fn, "insert block parent");

    llvm::Function *other_fn = mod.createFunction("other_fn", ft, false);
    TEST_ASSERT(other_fn != nullptr, "other function");
    builder.SetInsertPointForFunction(other_fn);

    llvm::Function *late_decl = mod.createFunction("late_decl", ft, true);
    TEST_ASSERT(late_decl != nullptr, "late decl");

    builder.SetInsertPoint(entry);
    llvm::BasicBlock *insert_before = llvm::BasicBlock::Create(ctx, "before", nullptr, entry);
    TEST_ASSERT(insert_before->impl_block() != nullptr, "insert-before block created");
    TEST_ASSERT(insert_before->getParent() == main_fn, "insert-before parent");

    return 0;
}

static int test_block_parent_recovery_from_ir_func_link() {
    llvm::LLVMContext ctx;
    llvm::Module mod("parent_recovery", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::FunctionType *ft = llvm::FunctionType::get(i32, false);
    llvm::Function *fn = mod.createFunction("parent_fn", ft, false);
    TEST_ASSERT(fn != nullptr, "function created");

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    TEST_ASSERT(entry->impl_block() != nullptr, "entry block created");

    llvm::detail::unregister_blocks_for_function(fn);
    TEST_ASSERT(entry->getParent() == fn, "parent recovered via block->func");

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPoint(entry);

    llvm::BasicBlock *insert_block = builder.GetInsertBlock();
    TEST_ASSERT(insert_block != nullptr, "insert block present");
    TEST_ASSERT(insert_block->getParent() == fn, "insert block parent recovered");

    builder.CreateRet(llvm::ConstantInt::get(i32, 0));
    return 0;
}

static int test_builder_syncs_module_from_insert_block() {
    llvm::LLVMContext ctx;
    llvm::IRBuilder<> builder(ctx);
    llvm::Module mod("builder_sync", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i8p = llvm::Type::getInt8PtrTy(ctx);

    llvm::Type *foo_params[] = {i8p};
    llvm::FunctionType *foo_ty = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), foo_params, false);
    llvm::Function *foo = llvm::Function::Create(
        foo_ty, llvm::GlobalValue::ExternalLinkage, "foo", mod);
    TEST_ASSERT(foo != nullptr, "foo decl");

    llvm::FunctionType *main_ty = llvm::FunctionType::get(i32, false);
    llvm::Function *main_fn = llvm::Function::Create(
        main_ty, llvm::GlobalValue::ExternalLinkage, "main", mod);
    TEST_ASSERT(main_fn != nullptr, "main function");
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", main_fn);
    TEST_ASSERT(entry->impl_block() != nullptr, "entry block");

    builder.SetInsertPoint(entry);
    llvm::Constant *str = builder.CreateGlobalStringPtr("I4", "serialization_info");
    TEST_ASSERT(str != nullptr, "global string");
    llvm::Value *foo_args[] = {str};
    llvm::Value *call_ret = builder.CreateCall(foo, llvm::ArrayRef<llvm::Value *>(foo_args, 1));
    (void)call_ret;
    builder.CreateRet(llvm::ConstantInt::get(i32, 0));

    /* Verify the builder synced to the module: the global exists and the
       symbol was interned.  In DIRECT mode instructions go to the backend
       (not IR), so we verify the module-level artefacts instead of
       dumping the function body. */
    lr_module_t *m = lc_module_get_ir(mod.getCompat());
    TEST_ASSERT(m != nullptr, "module ir handle");
    TEST_ASSERT(m->first_global != nullptr, "global was created");
    TEST_ASSERT(std::strcmp(m->first_global->name, "serialization_info") == 0,
                "global has correct name");
    TEST_ASSERT(main_fn->getIRFunc() != nullptr, "main IR func exists");
    return 0;
}

static int test_basicblock_mutation_ops() {
    llvm::LLVMContext ctx;
    llvm::Module mod("bb_mutation", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::FunctionType *ft = llvm::FunctionType::get(i32, false);
    llvm::Function *fn = mod.createFunction("bb_mutation_fn", ft, false);
    TEST_ASSERT(fn != nullptr, "function created");

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::BasicBlock *dead = llvm::BasicBlock::Create(ctx, "dead", fn);
    llvm::BasicBlock *mid = llvm::BasicBlock::Create(ctx, "mid", fn);
    llvm::BasicBlock *tail = llvm::BasicBlock::Create(ctx, "tail", fn);
    TEST_ASSERT(entry != nullptr && dead != nullptr
                && mid != nullptr && tail != nullptr, "blocks created");

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);

    builder.SetInsertPoint(entry);
    builder.CreateBr(mid);
    builder.SetInsertPoint(dead);
    builder.CreateBr(tail);
    builder.SetInsertPoint(mid);
    builder.CreateBr(tail);
    builder.SetInsertPoint(tail);
    builder.CreateRet(llvm::ConstantInt::get(i32, 0));

    lr_func_t *irf = fn->getIRFunc();
    TEST_ASSERT(irf != nullptr, "function has IR backing");
    TEST_ASSERT_EQ(irf->num_blocks, 4, "initial number of blocks");

    dead->moveAfter(tail);
    TEST_ASSERT(std::strcmp(irf->first_block->name, "entry") == 0, "entry stays first");
    TEST_ASSERT(std::strcmp(irf->first_block->next->name, "mid") == 0,
                "mid follows entry after moveAfter");
    TEST_ASSERT(std::strcmp(irf->last_block->name, "dead") == 0,
                "dead moved to the end");

    dead->moveBefore(mid);
    TEST_ASSERT(std::strcmp(irf->first_block->next->name, "dead") == 0,
                "dead moved before mid");
    TEST_ASSERT(std::strcmp(irf->first_block->next->next->name, "mid") == 0,
                "mid follows dead after moveBefore");

    dead->eraseFromParent();
    TEST_ASSERT(dead->getParent() == nullptr, "erased block parent cleared");
    TEST_ASSERT_EQ(irf->num_blocks, 3, "block count decremented after erase");
    TEST_ASSERT(std::strcmp(irf->first_block->name, "entry") == 0, "entry still first");
    TEST_ASSERT(std::strcmp(irf->first_block->next->name, "mid") == 0,
                "mid now second");
    TEST_ASSERT(std::strcmp(irf->last_block->name, "tail") == 0, "tail remains last");
    TEST_ASSERT_EQ(irf->first_block->id, 0, "entry id unchanged");
    TEST_ASSERT_EQ(irf->first_block->next->id, 1, "mid id compacted");
    TEST_ASSERT_EQ(irf->last_block->id, 2, "tail id compacted");

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule succeeds after block erase");
    typedef int (*fn_t)(void);
    fn_t fp = (fn_t)jit.lookup("bb_mutation_fn");
    TEST_ASSERT(fp != nullptr, "lookup mutated function");
    TEST_ASSERT_EQ(fp(), 0, "mutated CFG still executes correctly");

    return 0;
}

static int test_function_block_list_insert_and_iteration() {
    llvm::LLVMContext ctx;
    llvm::Module mod("fn_block_list", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::FunctionType *ft = llvm::FunctionType::get(i32, false);
    llvm::Function *fn = mod.createFunction("order_fn", ft, false);
    TEST_ASSERT(fn != nullptr, "function created");

    llvm::BasicBlock *detached = llvm::BasicBlock::Create(ctx, "detached", nullptr);
    TEST_ASSERT(detached != nullptr && detached->impl_block() != nullptr,
                "detached block created");
    lr_func_t *irf = fn->getIRFunc();
    TEST_ASSERT(irf != nullptr, "function has IR backing");
    TEST_ASSERT(irf->first_block == nullptr, "detached block is not auto-attached");

    fn->insert(fn->end(), detached);
    TEST_ASSERT(irf->first_block == detached->impl_block(), "insert(end, bb) attaches block");
    TEST_ASSERT(irf->last_block == detached->impl_block(), "single attached block is tail");
    TEST_ASSERT_EQ(fn->getBasicBlockList().size(), 1, "list size after first insert");

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::BasicBlock *tail = llvm::BasicBlock::Create(ctx, "tail", fn);
    TEST_ASSERT(entry != nullptr && tail != nullptr, "extra blocks created");
    TEST_ASSERT_EQ(fn->getBasicBlockList().size(), 3, "list size after auto-attach");

    fn->insert(entry, tail);
    const char *expected_order[] = {"detached", "tail", "entry"};

    int iter_count = 0;
    for (auto it = fn->begin(); it != fn->end(); ++it) {
        llvm::BasicBlock *bb = *it;
        TEST_ASSERT(bb != nullptr, "function iterator yields block");
        TEST_ASSERT(std::strcmp(bb->impl_block()->name, expected_order[iter_count]) == 0,
                    "function iterator order is preserved");
        ++iter_count;
    }
    TEST_ASSERT_EQ(iter_count, 3, "function iterator traverses all blocks");

    iter_count = 0;
    auto &bbl = fn->getBasicBlockList();
    for (auto it = bbl.begin(); it != bbl.end(); ++it) {
        llvm::BasicBlock *bb = *it;
        TEST_ASSERT(bb != nullptr, "block list iterator yields block");
        TEST_ASSERT(std::strcmp(bb->impl_block()->name, expected_order[iter_count]) == 0,
                    "block list iterator order is preserved");
        ++iter_count;
    }
    TEST_ASSERT_EQ(iter_count, 3, "block list iterator traverses all blocks");

    llvm::Function *fn2 = mod.createFunction("push_back_fn", ft, false);
    TEST_ASSERT(fn2 != nullptr, "second function created");
    llvm::BasicBlock *only = llvm::BasicBlock::Create(ctx, "only", nullptr);
    TEST_ASSERT(only != nullptr && only->impl_block() != nullptr,
                "push_back test block created");
    lr_func_t *irf2 = fn2->getIRFunc();
    TEST_ASSERT(irf2 != nullptr, "second function has IR backing");
    TEST_ASSERT(irf2->first_block == nullptr, "second function starts detached");

    auto &bbl2 = fn2->getBasicBlockList();
    TEST_ASSERT_EQ(bbl2.size(), 0, "second function list starts empty");
    bbl2.push_back(only);
    TEST_ASSERT(irf2->first_block == only->impl_block(), "push_back attaches detached block");
    TEST_ASSERT(irf2->last_block == only->impl_block(), "push_back updates tail");
    TEST_ASSERT_EQ(bbl2.size(), 1, "second function list size after push_back");

    return 0;
}

static int test_irbuilder_arithmetic() {
    llvm::LLVMContext ctx;
    llvm::Module mod("arith", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64, i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_add", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *a = fn->getArg(0);
    llvm::Value *b = fn->getArg(1);
    TEST_ASSERT(a != nullptr, "arg 0");
    TEST_ASSERT(b != nullptr, "arg 1");

    llvm::Value *sum = builder.CreateAdd(a, b, "sum");
    TEST_ASSERT(sum != nullptr, "add created");

    llvm::Value *diff = builder.CreateSub(sum, b, "diff");
    TEST_ASSERT(diff != nullptr, "sub created");

    llvm::Value *prod = builder.CreateMul(a, b, "prod");
    TEST_ASSERT(prod != nullptr, "mul created");

    builder.CreateRet(sum);

    return 0;
}

static int test_irbuilder_control_flow() {
    llvm::LLVMContext ctx;
    llvm::Module mod("cf", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_branch", ft, false);

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::BasicBlock *then_bb = llvm::BasicBlock::Create(ctx, "then", fn);
    llvm::BasicBlock *else_bb = llvm::BasicBlock::Create(ctx, "else", fn);
    llvm::BasicBlock *merge = llvm::BasicBlock::Create(ctx, "merge", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);

    builder.SetInsertPoint(entry);
    llvm::Value *arg = fn->getArg(0);
    llvm::Value *zero = llvm::ConstantInt::get(i64, 0);
    llvm::Value *cmp = builder.CreateICmpEQ(arg, zero, "cmp");
    builder.CreateCondBr(cmp, then_bb, else_bb);

    builder.SetInsertPoint(then_bb);
    llvm::Value *v1 = llvm::ConstantInt::get(i64, 1);
    builder.CreateBr(merge);

    builder.SetInsertPoint(else_bb);
    llvm::Value *v2 = llvm::ConstantInt::get(i64, 2);
    builder.CreateBr(merge);

    builder.SetInsertPoint(merge);
    llvm::PHINode *phi = builder.CreatePHI(i64, 2, "result");
    TEST_ASSERT(phi != nullptr, "phi created");
    phi->addIncoming(v1, then_bb);
    phi->addIncoming(v2, else_bb);
    phi->finalize();

    builder.CreateRet(phi);

    return 0;
}

static int test_irbuilder_memory() {
    llvm::LLVMContext ctx;
    llvm::Module mod("mem", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, false);

    llvm::Function *fn = mod.createFunction("test_mem", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::AllocaInst *alloca = builder.CreateAlloca(i64, nullptr, "x");
    TEST_ASSERT(alloca != nullptr, "alloca created");
    TEST_ASSERT(alloca->getAllocatedType() != nullptr, "alloca type set");

    llvm::Value *val = llvm::ConstantInt::get(i64, 42);
    builder.CreateStore(val, alloca);

    llvm::Value *loaded = builder.CreateLoad(i64, alloca, "loaded");
    TEST_ASSERT(loaded != nullptr, "load created");

    builder.CreateRet(loaded);
    return 0;
}

static int test_alloca_casting_precision() {
    llvm::LLVMContext ctx;
    llvm::Module mod("alloca_cast", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, false);
    llvm::Function *fn = mod.createFunction("test_alloca_cast", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *c1 = llvm::ConstantInt::get(i64, 1);
    llvm::Value *c2 = llvm::ConstantInt::get(i64, 2);
    llvm::Value *sum = builder.CreateAdd(c1, c2, "sum");
    TEST_ASSERT(sum != nullptr, "sum created");
    TEST_ASSERT(!llvm::isa<llvm::AllocaInst>(sum),
                "non-alloca instruction must not be recognized as AllocaInst");
    TEST_ASSERT(llvm::dyn_cast<llvm::AllocaInst>(sum) == nullptr,
                "dyn_cast<AllocaInst> must reject non-alloca values");

    llvm::AllocaInst *slot = builder.CreateAlloca(i64, nullptr, "slot");
    TEST_ASSERT(slot != nullptr, "alloca created");
    llvm::Value *as_value = slot;
    TEST_ASSERT(llvm::isa<llvm::AllocaInst>(as_value),
                "alloca value recognized as AllocaInst");
    TEST_ASSERT(llvm::dyn_cast<llvm::AllocaInst>(as_value) == slot,
                "dyn_cast<AllocaInst> returns original alloca");
    TEST_ASSERT(slot->getAllocatedType() == i64, "alloca type preserved");

    builder.CreateRet(sum);
    return 0;
}

static int test_irbuilder_casts() {
    llvm::LLVMContext ctx;
    llvm::Module mod("casts", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type *ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::Type *params[] = {i32};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_casts", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *arg = fn->getArg(0);
    llvm::Value *ext = builder.CreateSExt(arg, i64, "sext");
    TEST_ASSERT(ext != nullptr, "sext");

    llvm::Value *zext = builder.CreateZExt(arg, i64, "zext");
    TEST_ASSERT(zext != nullptr, "zext");

    llvm::Value *trunc = builder.CreateTrunc(ext, i32, "trunc");
    TEST_ASSERT(trunc != nullptr, "trunc");

    llvm::Value *fp = builder.CreateSIToFP(arg, dblTy, "sitofp");
    TEST_ASSERT(fp != nullptr, "sitofp");

    llvm::Value *intval = builder.CreateFPToSI(fp, i64, "fptosi");
    TEST_ASSERT(intval != nullptr, "fptosi");

    llvm::Value *ptr = builder.CreateIntToPtr(ext, ptrTy, "inttoptr");
    TEST_ASSERT(ptr != nullptr, "inttoptr");

    llvm::Value *back = builder.CreatePtrToInt(ptr, i64, "ptrtoint");
    TEST_ASSERT(back != nullptr, "ptrtoint");

    llvm::Value *sor = builder.CreateSExtOrTrunc(arg, i64, "sortrunc");
    TEST_ASSERT(sor != nullptr, "sextortrunc");

    builder.CreateRet(ext);
    return 0;
}

static int test_irbuilder_fp_ops() {
    llvm::LLVMContext ctx;
    llvm::Module mod("fpops", ctx);

    llvm::Type *dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type *params[] = {dblTy, dblTy};
    llvm::FunctionType *ft = llvm::FunctionType::get(dblTy, params, false);

    llvm::Function *fn = mod.createFunction("test_fp", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *a = fn->getArg(0);
    llvm::Value *b = fn->getArg(1);

    llvm::Value *fadd = builder.CreateFAdd(a, b, "fadd");
    TEST_ASSERT(fadd != nullptr, "fadd");

    llvm::Value *fsub = builder.CreateFSub(a, b, "fsub");
    TEST_ASSERT(fsub != nullptr, "fsub");

    llvm::Value *fmul = builder.CreateFMul(a, b, "fmul");
    TEST_ASSERT(fmul != nullptr, "fmul");

    llvm::Value *fdiv = builder.CreateFDiv(a, b, "fdiv");
    TEST_ASSERT(fdiv != nullptr, "fdiv");

    llvm::Value *fneg = builder.CreateFNeg(a, "fneg");
    TEST_ASSERT(fneg != nullptr, "fneg");

    llvm::Value *cmp = builder.CreateFCmpOLT(a, b, "fcmp");
    TEST_ASSERT(cmp != nullptr, "fcmp_olt");

    builder.CreateRet(fadd);
    return 0;
}

static int test_casting_helpers() {
    llvm::LLVMContext ctx;
    llvm::Module mod("casting", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::ConstantInt *ci = llvm::ConstantInt::get(i32, 7);
    llvm::Value *v = ci;

    llvm::ConstantInt *back = llvm::dyn_cast<llvm::ConstantInt>(v);
    TEST_ASSERT(back != nullptr, "dyn_cast non-null");

    llvm::ConstantInt *casted = llvm::cast<llvm::ConstantInt>(v);
    TEST_ASSERT(casted != nullptr, "cast non-null");

    TEST_ASSERT(llvm::isa<llvm::Value>(v), "isa Value");
    TEST_ASSERT(llvm::isa<llvm::ConstantInt>(v), "isa ConstantInt");

    llvm::Value *null_val = nullptr;
    llvm::ConstantInt *null_cast = llvm::dyn_cast_or_null<llvm::ConstantInt>(null_val);
    TEST_ASSERT(null_cast == nullptr, "dyn_cast_or_null nullptr");

    return 0;
}

static int test_apint_apfloat() {
    llvm::APInt a(32, 42);
    TEST_ASSERT_EQ(a.getBitWidth(), 32, "apint width");
    TEST_ASSERT_EQ(a.getZExtValue(), 42, "apint value");

    llvm::APInt neg(32, static_cast<uint64_t>(-1), true);
    TEST_ASSERT_EQ(neg.getSExtValue(), -1, "apint signed");

    llvm::APFloat f(3.14);
    double d = f.convertToDouble();
    TEST_ASSERT(d > 3.13 && d < 3.15, "apfloat value");

    return 0;
}

static int test_data_layout() {
    llvm::LLVMContext ctx;
    llvm::Module mod("dl", ctx);

    const llvm::DataLayout &dl = mod.getDataLayout();
    TEST_ASSERT_EQ(dl.getPointerSize(), 8, "ptr size");
    TEST_ASSERT_EQ(dl.getPointerSizeInBits(), 64, "ptr bits");

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    TEST_ASSERT(dl.getTypeAllocSize(i32) > 0, "i32 alloc size > 0");

    return 0;
}

static int test_noop_passes() {
    llvm::LLVMContext ctx;
    llvm::Module mod("passes", ctx);

    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    return 0;
}

static int test_noop_verifier() {
    llvm::LLVMContext ctx;
    llvm::Module mod("verify", ctx);

    bool broken = llvm::verifyModule(mod);
    TEST_ASSERT(!broken, "module verifies");

    return 0;
}

static int test_noop_di_builder() {
    llvm::LLVMContext ctx;
    llvm::Module mod("debug", ctx);

    llvm::DIBuilder dib(mod);
    dib.finalize();

    return 0;
}

static int test_stringref_twine() {
    llvm::StringRef sr("hello");
    TEST_ASSERT_EQ(sr.size(), 5, "stringref size");
    TEST_ASSERT(sr == "hello", "stringref eq");
    TEST_ASSERT(sr != "world", "stringref ne");

    llvm::Twine tw("hello");
    std::string s = tw.str();
    TEST_ASSERT(s == "hello", "twine str");

    return 0;
}

static int test_intrinsic_name_lookup_helper() {
    const char *sin_name = lc_intrinsic_name((unsigned)llvm::Intrinsic::sin);
    const char *trap_name = lc_intrinsic_name((unsigned)llvm::Intrinsic::trap);
    const char *dbg_name = lc_intrinsic_name((unsigned)llvm::Intrinsic::dbg_value);

    TEST_ASSERT(sin_name != nullptr && std::strcmp(sin_name, "sin") == 0,
                "intrinsic sin mapping");
    TEST_ASSERT(trap_name != nullptr && std::strcmp(trap_name, "abort") == 0,
                "intrinsic trap mapping");
    TEST_ASSERT(dbg_name == nullptr, "unsupported intrinsic returns null");
    return 0;
}

static int test_stringref_nullptr_zero_len_safety() {
    llvm::StringRef empty_from_null(nullptr, 0);
    TEST_ASSERT(empty_from_null.empty(), "nullptr+0 StringRef is empty");

    std::string rendered = empty_from_null.str();
    TEST_ASSERT(rendered.empty(), "nullptr+0 StringRef renders empty");

    llvm::Twine tw(empty_from_null);
    TEST_ASSERT(tw.str().empty(), "twine from nullptr+0 StringRef renders empty");

    std::string out;
    llvm::raw_string_ostream os(out);
    os << empty_from_null;
    TEST_ASSERT(out.empty(), "raw_string_ostream ignores nullptr+0 StringRef");
    return 0;
}

static int test_raw_ostream_null_cstr_safety() {
    std::string out;
    llvm::raw_string_ostream os(out);

    const char *null_cstr = nullptr;
    os << null_cstr;
    TEST_ASSERT(out.empty(), "raw_string_ostream ignores null c-string");

    os.write(nullptr, 4);
    TEST_ASSERT(out.empty(), "raw_string_ostream ignores null write pointer");
    return 0;
}

static int test_raw_ostream_numeric_formatting() {
    std::string out;
    llvm::raw_string_ostream os(out);
    os << -12 << " " << 34u << " " << 5.5;
    os << " " << static_cast<const void *>(&os);
    TEST_ASSERT(out.find("-12") != std::string::npos, "signed integer formatting");
    TEST_ASSERT(out.find("34") != std::string::npos, "unsigned integer formatting");
    TEST_ASSERT(out.find("5.500000") != std::string::npos, "double formatting");
    TEST_ASSERT(out.find("0x") != std::string::npos, "pointer formatting");
    return 0;
}

static int test_twine_abi_layout_and_concat() {
    size_t expected_size = sizeof(void *) == 8 ? 40u : 20u;
    TEST_ASSERT_EQ(sizeof(llvm::Twine), expected_size, "twine ABI size");
    TEST_ASSERT_EQ(alignof(llvm::Twine), alignof(void *), "twine ABI alignment");

    llvm::Twine lhs("ab");
    llvm::Twine rhs("cd");
    std::string joined = (lhs + rhs).str();
    TEST_ASSERT(joined == "abcd", "twine concat");

    llvm::Twine null_twine = llvm::Twine::createNull();
    TEST_ASSERT(null_twine.str().empty(), "null twine renders empty");

    return 0;
}

static int test_arrayref() {
    int arr[] = {1, 2, 3};
    llvm::ArrayRef<int> ref(arr, 3);
    TEST_ASSERT_EQ(ref.size(), 3, "arrayref size");
    TEST_ASSERT_EQ(ref[0], 1, "arrayref idx 0");
    TEST_ASSERT_EQ(ref[2], 3, "arrayref idx 2");
    return 0;
}

static int test_irbuilder_gep_and_struct_gep() {
    llvm::LLVMContext ctx;
    llvm::Module mod("gep", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::Type *params[] = {ptrTy};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_gep", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *ptr = fn->getArg(0);

    llvm::Type *fields[] = {i32, i64};
    llvm::StructType *st = llvm::StructType::get(ctx, fields);

    llvm::Value *sgep = builder.CreateStructGEP(st, ptr, 1, "sgep");
    TEST_ASSERT(sgep != nullptr, "struct gep");

    llvm::Value *idx = llvm::ConstantInt::get(i64, 0);
    llvm::Value *gep = builder.CreateGEP(i64, ptr, idx, "gep");
    TEST_ASSERT(gep != nullptr, "gep");

    llvm::Value *igep = builder.CreateInBoundsGEP(i64, ptr, idx, "igep");
    TEST_ASSERT(igep != nullptr, "inbounds gep");

    llvm::Value *loaded = builder.CreateLoad(i64, sgep, "val");
    builder.CreateRet(loaded);

    return 0;
}

static int test_irbuilder_select() {
    llvm::LLVMContext ctx;
    llvm::Module mod("select", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64, i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_sel", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *a = fn->getArg(0);
    llvm::Value *b = fn->getArg(1);
    llvm::Value *zero = llvm::ConstantInt::get(i64, 0);
    llvm::Value *cond = builder.CreateICmpSGT(a, zero, "cond");
    llvm::Value *sel = builder.CreateSelect(cond, a, b, "sel");
    TEST_ASSERT(sel != nullptr, "select created");

    builder.CreateRet(sel);
    return 0;
}

static int test_irbuilder_call() {
    llvm::LLVMContext ctx;
    llvm::Module mod("call", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64, i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *add_fn = mod.createFunction("add", ft, false);
    {
        llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", add_fn);
        llvm::IRBuilder<> b(ctx);
        b.SetModule(mod.getCompat());
        b.SetInsertPointForFunction(add_fn);
        b.SetInsertPoint(bb);
        llvm::Value *sum = b.CreateAdd(add_fn->getArg(0),
                                       add_fn->getArg(1));
        b.CreateRet(sum);
    }

    llvm::Function *caller = mod.createFunction("caller", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", caller);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(caller);
    builder.SetInsertPoint(bb);

    llvm::Value *a = caller->getArg(0);
    llvm::Value *b_arg = caller->getArg(1);
    llvm::Value *args[] = {a, b_arg};
    llvm::Value *result = builder.CreateCall(ft,
        llvm::Value::wrap(add_fn->getFuncVal()), args, "result");
    TEST_ASSERT(result != nullptr, "call result");
    builder.CreateRet(result);

    return 0;
}

static int test_irbuilder_extractvalue_insertvalue() {
    llvm::LLVMContext ctx;
    llvm::Module mod("aggr", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *fields[] = {i32, i64};
    llvm::StructType *st = llvm::StructType::get(ctx, fields);

    llvm::Type *params[] = {i32};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_ev", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *undef = llvm::UndefValue::get(st);
    llvm::Value *arg = fn->getArg(0);
    unsigned idx0[] = {0};
    llvm::Value *inserted = builder.CreateInsertValue(undef, arg, idx0, "ins");
    TEST_ASSERT(inserted != nullptr, "insertvalue");

    unsigned idx1[] = {1};
    llvm::Value *extracted = builder.CreateExtractValue(inserted, idx1, "ext");
    TEST_ASSERT(extracted != nullptr, "extractvalue");

    llvm::Value *ret = builder.CreateSExt(arg, i64);
    builder.CreateRet(ret);

    return 0;
}

static int test_irbuilder_bitwise() {
    llvm::LLVMContext ctx;
    llvm::Module mod("bits", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64, i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_bits", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *a = fn->getArg(0);
    llvm::Value *b = fn->getArg(1);

    llvm::Value *v_and = builder.CreateAnd(a, b, "and");
    TEST_ASSERT(v_and != nullptr, "and");

    llvm::Value *v_or = builder.CreateOr(a, b, "or");
    TEST_ASSERT(v_or != nullptr, "or");

    llvm::Value *v_xor = builder.CreateXor(a, b, "xor");
    TEST_ASSERT(v_xor != nullptr, "xor");

    llvm::Value *v_shl = builder.CreateShl(a, b, "shl");
    TEST_ASSERT(v_shl != nullptr, "shl");

    llvm::Value *v_lshr = builder.CreateLShr(a, b, "lshr");
    TEST_ASSERT(v_lshr != nullptr, "lshr");

    llvm::Value *v_ashr = builder.CreateAShr(a, b, "ashr");
    TEST_ASSERT(v_ashr != nullptr, "ashr");

    llvm::Value *v_not = builder.CreateNot(a, "not");
    TEST_ASSERT(v_not != nullptr, "not");

    builder.CreateRet(v_and);
    return 0;
}

static int test_target_select_noop() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    return 0;
}

static int test_replace_all_uses_with_rewrites_existing_operands() {
    ScopedEnvVar policy("LIRIC_POLICY", "ir");
    llvm::LLVMContext ctx;
    llvm::Module mod("rauw_operands", ctx);
    auto *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *params[] = {i32};
    auto *fty = llvm::FunctionType::get(
        i32, llvm::ArrayRef<llvm::Type *>(params, 1), false);
    auto *fn = llvm::Function::Create(
        fty, llvm::GlobalValue::ExternalLinkage, "rauw_operand_fn", mod);
    auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::IRBuilder<> builder(entry);

    llvm::Value *x = fn->getArg(0);
    llvm::Value *one = llvm::ConstantInt::get(i32, 1);
    llvm::Value *two = llvm::ConstantInt::get(i32, 2);
    llvm::Value *tmp = builder.CreateAdd(x, one, "tmp");
    llvm::Value *mul = builder.CreateMul(tmp, two, "mul");
    tmp->replaceAllUsesWith(x);
    builder.CreateRet(mul);

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef int (*fn_t)(int);
    fn_t fp = (fn_t)jit.lookup("rauw_operand_fn");
    TEST_ASSERT(fp != nullptr, "lookup rauw_operand_fn");
    TEST_ASSERT_EQ(fp(7), 14, "replaceAllUsesWith rewrites existing IR uses");
    return 0;
}

static int test_switch_add_case_builds_dispatch_chain() {
    ScopedEnvVar policy("LIRIC_POLICY", "ir");
    llvm::LLVMContext ctx;
    llvm::Module mod("switch_add_case", ctx);
    auto *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *params[] = {i32};
    auto *fty = llvm::FunctionType::get(
        i32, llvm::ArrayRef<llvm::Type *>(params, 1), false);
    auto *fn = llvm::Function::Create(
        fty, llvm::GlobalValue::ExternalLinkage, "switch_case_fn", mod);

    auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    auto *case1 = llvm::BasicBlock::Create(ctx, "case1", fn);
    auto *case2 = llvm::BasicBlock::Create(ctx, "case2", fn);
    auto *def = llvm::BasicBlock::Create(ctx, "default", fn);

    llvm::IRBuilder<> builder(entry);
    llvm::Value *x = fn->getArg(0);
    auto *sw = builder.CreateSwitch(x, def, 2);
    TEST_ASSERT(sw != nullptr, "CreateSwitch");
    sw->addCase(llvm::ConstantInt::get(i32, 1), case1);
    sw->addCase(llvm::ConstantInt::get(i32, 2), case2);

    builder.SetInsertPoint(case1);
    builder.CreateRet(llvm::ConstantInt::get(i32, 11));
    builder.SetInsertPoint(case2);
    builder.CreateRet(llvm::ConstantInt::get(i32, 22));
    builder.SetInsertPoint(def);
    builder.CreateRet(llvm::ConstantInt::get(i32, 33));

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef int (*fn_t)(int);
    fn_t fp = (fn_t)jit.lookup("switch_case_fn");
    TEST_ASSERT(fp != nullptr, "lookup switch_case_fn");
    TEST_ASSERT_EQ(fp(1), 11, "switch case 1");
    TEST_ASSERT_EQ(fp(2), 22, "switch case 2");
    TEST_ASSERT_EQ(fp(9), 33, "switch default");
    return 0;
}

static int test_jit_smoke_ret_42() {
    llvm::LLVMContext ctx;
    llvm::Module mod("jit_smoke", ctx);
    auto *i32 = llvm::Type::getInt32Ty(ctx);
    auto *fty = llvm::FunctionType::get(i32, false);
    auto *fn = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage,
                                       "ret42", mod);
    auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::IRBuilder<> builder(entry);
    builder.CreateRet(llvm::ConstantInt::get(i32, 42));

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef int (*fn_t)(void);
    fn_t fp = (fn_t)jit.lookup("ret42");
    TEST_ASSERT(fp != nullptr, "lookup ret42");
    int result = fp();
    TEST_ASSERT_EQ(result, 42, "ret42() == 42");
    return 0;
}

static int test_jit_smoke_add_args() {
    llvm::LLVMContext ctx;
    llvm::Module mod("jit_add", ctx);
    auto *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *params[] = {i32, i32};
    auto *fty = llvm::FunctionType::get(i32, llvm::ArrayRef<llvm::Type *>(params, 2), false);
    auto *fn = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage,
                                       "add_args", mod);
    auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::IRBuilder<> builder(entry);
    llvm::Value *a = fn->getArg(0);
    llvm::Value *b = fn->getArg(1);
    llvm::Value *sum = builder.CreateAdd(a, b, "sum");
    builder.CreateRet(sum);

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef int (*fn_t)(int, int);
    fn_t fp = (fn_t)jit.lookup("add_args");
    TEST_ASSERT(fp != nullptr, "lookup add_args");
    int result = fp(17, 25);
    TEST_ASSERT_EQ(result, 42, "add_args(17,25) == 42");
    return 0;
}

static int test_jit_smoke_vector_return_call() {
    llvm::LLVMContext ctx;
    llvm::Module mod("jit_vec_ret", ctx);

    auto *i32 = llvm::Type::getInt32Ty(ctx);
    auto *vec2 = llvm::FixedVectorType::get(i32, 2);
    TEST_ASSERT(vec2 != nullptr, "fixed vector type");

    auto *pair_ty = llvm::FunctionType::get(vec2, false);
    auto *pair_fn = llvm::Function::Create(
        pair_ty, llvm::GlobalValue::ExternalLinkage, "make_pair_v", mod);
    auto *pair_entry = llvm::BasicBlock::Create(ctx, "entry", pair_fn);
    llvm::IRBuilder<> pair_builder(pair_entry);
    llvm::Value *pair_undef = llvm::UndefValue::get(vec2);
    unsigned idx0_data[] = {0};
    unsigned idx1_data[] = {1};
    llvm::ArrayRef<unsigned> idx0(idx0_data, 1);
    llvm::ArrayRef<unsigned> idx1(idx1_data, 1);
    llvm::Value *pair_v0 = pair_builder.CreateInsertValue(
        pair_undef, llvm::ConstantInt::get(i32, 19), idx0, "v0");
    llvm::Value *pair_v1 = pair_builder.CreateInsertValue(
        pair_v0, llvm::ConstantInt::get(i32, 23), idx1, "v1");
    pair_builder.CreateRet(pair_v1);

    auto *sum_ty = llvm::FunctionType::get(i32, false);
    auto *sum_fn = llvm::Function::Create(
        sum_ty, llvm::GlobalValue::ExternalLinkage, "sum_pair_v", mod);
    auto *sum_entry = llvm::BasicBlock::Create(ctx, "entry", sum_fn);
    llvm::IRBuilder<> sum_builder(sum_entry);
    llvm::Value *pair = sum_builder.CreateCall(pair_fn, {}, "pair");
    llvm::Value *e0 = sum_builder.CreateExtractValue(pair, idx0, "e0");
    llvm::Value *e1 = sum_builder.CreateExtractValue(pair, idx1, "e1");
    llvm::Value *sum = sum_builder.CreateAdd(e0, e1, "sum");
    sum_builder.CreateRet(sum);

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");

    typedef int (*fn_t)(void);
    fn_t fp = (fn_t)jit.lookup("sum_pair_v");
    TEST_ASSERT(fp != nullptr, "lookup sum_pair_v");
    int result = fp();
    TEST_ASSERT_EQ(result, 42, "sum_pair_v() == 42");
    return 0;
}

static int test_jit_smoke_branch() {
    llvm::LLVMContext ctx;
    llvm::Module mod("jit_branch", ctx);
    auto *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *params[] = {i32};
    auto *fty = llvm::FunctionType::get(i32, llvm::ArrayRef<llvm::Type *>(params, 1), false);
    auto *fn = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage,
                                       "abs_val", mod);
    auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    auto *then_bb = llvm::BasicBlock::Create(ctx, "then", fn);
    auto *else_bb = llvm::BasicBlock::Create(ctx, "else", fn);
    auto *merge_bb = llvm::BasicBlock::Create(ctx, "merge", fn);

    llvm::IRBuilder<> builder(entry);
    llvm::Value *x = fn->getArg(0);
    llvm::Value *zero = llvm::ConstantInt::get(i32, 0);
    llvm::Value *cmp = builder.CreateICmpSLT(x, zero, "neg");
    builder.CreateCondBr(cmp, then_bb, else_bb);

    builder.SetInsertPoint(then_bb);
    llvm::Value *negx = builder.CreateSub(zero, x, "negx");
    builder.CreateBr(merge_bb);

    builder.SetInsertPoint(else_bb);
    builder.CreateBr(merge_bb);

    builder.SetInsertPoint(merge_bb);
    auto *phi = builder.CreatePHI(i32, 2, "result");
    phi->addIncoming(negx, then_bb);
    phi->addIncoming(x, else_bb);
    builder.CreateRet(phi);

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef int (*fn_t)(int);
    fn_t fp = (fn_t)jit.lookup("abs_val");
    TEST_ASSERT(fp != nullptr, "lookup abs_val");
    TEST_ASSERT_EQ(fp(5), 5, "abs_val(5) == 5");
    TEST_ASSERT_EQ(fp(-7), 7, "abs_val(-7) == 7");
    TEST_ASSERT_EQ(fp(0), 0, "abs_val(0) == 0");
    return 0;
}

static int test_jit_smoke_branch_manual_phi_finalize() {
    llvm::LLVMContext ctx;
    llvm::Module mod("jit_branch_manual_phi_finalize", ctx);
    auto *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *params[] = {i32};
    auto *fty = llvm::FunctionType::get(
        i32, llvm::ArrayRef<llvm::Type *>(params, 1), false);
    auto *fn = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage,
                                      "abs_val_manual_phi_finalize", mod);
    auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    auto *then_bb = llvm::BasicBlock::Create(ctx, "then", fn);
    auto *else_bb = llvm::BasicBlock::Create(ctx, "else", fn);
    auto *merge_bb = llvm::BasicBlock::Create(ctx, "merge", fn);

    llvm::IRBuilder<> builder(entry);
    llvm::Value *x = fn->getArg(0);
    llvm::Value *zero = llvm::ConstantInt::get(i32, 0);
    llvm::Value *cmp = builder.CreateICmpSLT(x, zero, "neg");
    builder.CreateCondBr(cmp, then_bb, else_bb);

    builder.SetInsertPoint(then_bb);
    llvm::Value *negx = builder.CreateSub(zero, x, "negx");
    builder.CreateBr(merge_bb);

    builder.SetInsertPoint(else_bb);
    builder.CreateBr(merge_bb);

    builder.SetInsertPoint(merge_bb);
    auto *phi = builder.CreatePHI(i32, 2, "result");
    phi->addIncoming(negx, then_bb);
    phi->addIncoming(x, else_bb);
    phi->finalize();
    builder.CreateRet(phi);

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef int (*fn_t)(int);
    fn_t fp = (fn_t)jit.lookup("abs_val_manual_phi_finalize");
    TEST_ASSERT(fp != nullptr, "lookup abs_val_manual_phi_finalize");
    TEST_ASSERT_EQ(fp(5), 5, "abs_val_manual_phi_finalize(5) == 5");
    TEST_ASSERT_EQ(fp(-7), 7, "abs_val_manual_phi_finalize(-7) == 7");
    TEST_ASSERT_EQ(fp(0), 0, "abs_val_manual_phi_finalize(0) == 0");
    return 0;
}

static int test_jit_smoke_indirect_bitcast_external_fp_call() {
    llvm::LLVMContext ctx;
    llvm::Module mod("jit_indirect_ext_fp", ctx);
    auto *dbl = llvm::Type::getDoubleTy(ctx);
    auto *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *sin_params[] = {dbl};
    auto *sin_ty = llvm::FunctionType::get(
        dbl, llvm::ArrayRef<llvm::Type *>(sin_params, 1), false);
    auto *sin_decl = llvm::Function::Create(
        sin_ty, llvm::GlobalValue::ExternalLinkage, "sin", mod);
    TEST_ASSERT(sin_decl != nullptr, "sin declaration");

    auto *caller_ty = llvm::FunctionType::get(i64, false);
    auto *caller = llvm::Function::Create(
        caller_ty, llvm::GlobalValue::ExternalLinkage, "call_sin_bitcast", mod);
    auto *entry = llvm::BasicBlock::Create(ctx, "entry", caller);
    llvm::IRBuilder<> builder(entry);

    llvm::Value *callee = builder.CreateBitCast(
        sin_decl, llvm::Type::getInt8PtrTy(ctx), "sin_ptr");
    TEST_ASSERT(callee != nullptr, "bitcasted callee");
    llvm::Value *arg = llvm::ConstantFP::get(dbl, 1.5707963267948966);
    llvm::Value *call_args[] = {arg};
    llvm::Value *s = builder.CreateCall(
        sin_ty, callee, llvm::ArrayRef<llvm::Value *>(call_args, 1), "s");
    TEST_ASSERT(s != nullptr, "sin call");
    llvm::Value *scaled = builder.CreateFMul(
        s, llvm::ConstantFP::get(dbl, 1000.0), "scaled");
    llvm::Value *as_i64 = builder.CreateFPToSI(scaled, i64, "as_i64");
    builder.CreateRet(as_i64);

    llvm::orc::LLJIT jit;
    double (*sin_fn)(double) = std::sin;
    void *sin_addr = nullptr;
    memcpy(&sin_addr, &sin_fn, sizeof(sin_addr));
    jit.addSymbol("sin", sin_addr);
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef long long (*fn_t)(void);
    fn_t fp = (fn_t)jit.lookup("call_sin_bitcast");
    TEST_ASSERT(fp != nullptr, "lookup call_sin_bitcast");
    long long result = fp();
    TEST_ASSERT(result >= 999 && result <= 1001,
                "bitcasted external FP call uses correct ABI");
    return 0;
}

static int test_jit_stringref_slice_symbol_lookup() {
    llvm::orc::LLJIT jit;

    typedef int (*fn_t)(void);
    fn_t native = ret42_symbol_for_stringref_lookup;
    void *native_addr = nullptr;
    memcpy(&native_addr, &native, sizeof(native_addr));

    const char add_storage[] = {'r', 'e', 't', '4', '2', 'X', '\0'};
    jit.addSymbol(llvm::StringRef(add_storage, 5), native_addr);

    const char lookup_storage[] = {'r', 'e', 't', '4', '2', 'Y', '\0'};
    void *resolved = jit.lookup(llvm::StringRef(lookup_storage, 5));
    TEST_ASSERT(resolved != nullptr, "LLJIT lookup with StringRef slice");

    fn_t fp = (fn_t)resolved;
    TEST_ASSERT_EQ(fp(), 42, "resolved StringRef slice symbol executes");
    return 0;
}

int main() {
    fprintf(stderr, "LLVM C++ compat test suite\n");
    fprintf(stderr, "==========================\n\n");

    fprintf(stderr, "Infrastructure tests:\n");
    RUN_TEST(test_llvm_version);
    RUN_TEST(test_stringref_twine);
    RUN_TEST(test_intrinsic_name_lookup_helper);
    RUN_TEST(test_stringref_nullptr_zero_len_safety);
    RUN_TEST(test_raw_ostream_null_cstr_safety);
    RUN_TEST(test_raw_ostream_numeric_formatting);
    RUN_TEST(test_twine_abi_layout_and_concat);
    RUN_TEST(test_arrayref);
    RUN_TEST(test_apint_apfloat);
    RUN_TEST(test_casting_helpers);
    RUN_TEST(test_target_select_noop);

    fprintf(stderr, "\nModule and Type tests:\n");
    RUN_TEST(test_context_and_module);
    RUN_TEST(test_basic_types);
    RUN_TEST(test_type_context_stability_across_nested_modules);
    RUN_TEST(test_function_type);
    RUN_TEST(test_struct_type);
    RUN_TEST(test_array_type);
    RUN_TEST(test_data_layout);

    fprintf(stderr, "\nConstant tests:\n");
    RUN_TEST(test_constants);
    RUN_TEST(test_constant_data_array_addnull);
    RUN_TEST(test_constant_struct_and_array_bytes);
    RUN_TEST(test_constant_array_single_aggregate_payload_preserved);
    RUN_TEST(test_global_lookup_set_initializer_and_jit);
    RUN_TEST(test_create_global_without_initializer_is_declaration);
    RUN_TEST(test_duplicate_global_names_are_uniquified);
    RUN_TEST(test_parse_assembly_wrapper_fast_path);

    fprintf(stderr, "\nFunction tests:\n");
    RUN_TEST(test_function_creation);
    RUN_TEST(test_stringref_slice_module_symbol_lookup);
    RUN_TEST(test_block_parent_tracking_across_decls);
    RUN_TEST(test_block_parent_recovery_from_ir_func_link);
    RUN_TEST(test_builder_syncs_module_from_insert_block);
    RUN_TEST(test_basicblock_mutation_ops);
    RUN_TEST(test_function_block_list_insert_and_iteration);

    fprintf(stderr, "\nIRBuilder tests:\n");
    RUN_TEST(test_irbuilder_arithmetic);
    RUN_TEST(test_irbuilder_control_flow);
    RUN_TEST(test_irbuilder_memory);
    RUN_TEST(test_alloca_casting_precision);
    RUN_TEST(test_irbuilder_casts);
    RUN_TEST(test_irbuilder_fp_ops);
    RUN_TEST(test_irbuilder_gep_and_struct_gep);
    RUN_TEST(test_irbuilder_select);
    RUN_TEST(test_irbuilder_call);
    RUN_TEST(test_irbuilder_extractvalue_insertvalue);
    RUN_TEST(test_irbuilder_bitwise);

    fprintf(stderr, "\nNo-op verification tests:\n");
    RUN_TEST(test_noop_passes);
    RUN_TEST(test_noop_verifier);
    RUN_TEST(test_noop_di_builder);

    fprintf(stderr, "\nJIT smoke tests:\n");
    RUN_TEST(test_replace_all_uses_with_rewrites_existing_operands);
    RUN_TEST(test_switch_add_case_builds_dispatch_chain);
    RUN_TEST(test_jit_smoke_ret_42);
    RUN_TEST(test_jit_smoke_add_args);
    RUN_TEST(test_jit_smoke_vector_return_call);
    RUN_TEST(test_jit_smoke_branch);
    RUN_TEST(test_jit_smoke_branch_manual_phi_finalize);
    RUN_TEST(test_jit_smoke_indirect_bitcast_external_fp_call);
    RUN_TEST(test_jit_stringref_slice_symbol_lookup);

    fprintf(stderr, "\n==========================\n");
    fprintf(stderr, "%d tests: %d passed, %d failed\n",
            tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
